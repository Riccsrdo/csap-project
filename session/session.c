/* session.c

Responsible for:
- creating users;
- login;
- session table in shm;
- privileges (with seteuid).
*/
#include"session.h"
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

// unprivileged user and group IDs in Debian/Ubuntu, these values are not certain
uid_t unpriv_uid = 65534; // 'nobody'
gid_t unpriv_gid = 65534; // 'nogroup'

// resolve values
void setup_unprivileged_user(){
    struct passwd *pwd = getpwnam("nobody");
    struct group *grp = getgrnam("nogroup");
    if(pwd){
        unpriv_uid = pwd->pw_uid;
    }
    if(grp){
        unpriv_gid = grp->gr_gid;
    }
}

/*
Function used to drop privileges of the process to the unprivileged user and group.
*/
int drop_privileges(){
    if(geteuid() !=0) return 0;

    if(setegid(unpriv_gid) < 0){
        return -errno;
    }

    if(seteuid(unpriv_uid) < 0){
        int saved= errno;
        setegid(0); // try to restore group ID to root
        return -saved;
    }

    return 0;
}

/*
Function used to run an executable file with arguments, blocking SIGCHLD during the execution.
*/
static int run_exec(const char *file, char *const argv[]) {
    // block standard server handler from running on SIGCHLD
    sigset_t child_mask, old_mask;
    sigemptyset(&child_mask);
    sigaddset(&child_mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &child_mask, &old_mask); 

    fflush(NULL);

    pid_t pid = fork();
    if(pid < 0) {
        sigprocmask(SIG_SETMASK, &old_mask, NULL); // restore old mask
        return -1;
    }
    if(pid == 0) { // child process
        execvp(file, argv);
        perror("execvp failed");
        _exit(EXIT_FAILURE); // if execvp fails
    }

    int status = -1;
    pid_t w = waitpid(pid, &status, 0);
    sigprocmask(SIG_SETMASK, &old_mask, NULL); // restore

    if(w!= pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return -1; // waitpid failed
    }

    return 0;

}


gid_t server_gid = 0;

/*
Helper function used to validate validity of provided username, especially for the creation of users.
*/
static int validate_username(const char *username, char *err_msg, uint32_t err_size) {
    size_t len = strlen(username);
    if(len < 3 || len > 32) { // check len bounded between 3 and 32 characters
        snprintf(err_msg, err_size, "Username must be between 3 and 32 characters");
        return -1;
    }
    if(!isalpha((unsigned char)username[0])) { // check first character is a letter, number is forbidden
        snprintf(err_msg, err_size, "Username must start with a letter");
        return -1;
    }
    for(size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)username[i]; // cast to unsigned char to avoid issues with negative values
        if(!isalnum(ch) && ch != '_' && ch != '-') { // check if character is not in [a-zA-Z0-9_-]
            snprintf(err_msg, err_size,
                "Username can only contain letters, numbers, underscores and hyphens");
            return -1;
        }
    }
    return 0;
}

/*
Helper function used to check if a user is a valid CSAP user.
*/
int is_csap_user(const struct passwd *pwd) {
    if(pwd == NULL) return 0;
    if(pwd->pw_uid < 1000) return 0; // 1000 is the typical minimum UID for regular users on Linux
    if(server_gid != 0 && pwd->pw_gid != server_gid) return 0;
    return 1;
}

int create_home_directory(const char *root, const char *username, char *err_msg, 
    uint32_t err_size, mode_t perms, struct passwd *new_pwd) {
    char home_dir[PATH_MAX +64];
    snprintf(home_dir, sizeof(home_dir), "%s/%s", root, username);

    if(mkdir(home_dir, perms) < 0) {
        if(errno == EEXIST) {
            snprintf(err_msg, err_size , "Home directory already exists");
            err_msg[err_size - 1] = '\0';
            return -1;
        }
        snprintf(err_msg, err_size , "Failed to create home directory: %s", strerror(errno));
        err_msg[err_size - 1] = '\0';
        return -1;
    }




    // assign ownership of the home directory to the new user
    if(chown(home_dir, new_pwd->pw_uid, server_gid) < 0) {
        snprintf(err_msg, err_size , "Failed to set ownership of home directory: %s", strerror(errno));
        err_msg[err_size - 1] = '\0';
        return -1;
    }

    // explicitly set the permissions of the home directory
    if(chmod(home_dir, perms) < 0) {
        snprintf(err_msg, err_size , "Failed to set permissions of home directory: %s", strerror(errno));
        err_msg[err_size - 1] = '\0';
        return -1;
    }


    return 0;
}

// function used to create the server's group, and set the global variable server_gid
int setup_server_gid(char *error_msg, uint32_t err_size) {
    // check if the group already exists
    struct group *grp = getgrnam("csap_group");
    if(grp) {
        server_gid = grp->gr_gid;
        return 0; // group already exists
    }

    // if the group does not exist, create it
    // this requires root privileges, so the server must be run with sudo
    if(seteuid(0) < 0) {
        snprintf(error_msg, err_size - 1, "Failed to set effective UID to root: %s", strerror(errno));
        return -1;
    }

    // create the group, using fork() and execvp to call groupadd
    char *argv[] = { "groupadd", "csap_group", NULL};
    if(run_exec("groupadd", argv) < 0){
        snprintf(error_msg, err_size - 1, "Failed to create group: %s", strerror(errno));
        seteuid(getuid());
        return -1;
    }

    // validate correct creation
    grp = getgrnam("csap_group");
    if(!grp){
        snprintf(error_msg, err_size - 1, "Failed to validate group creation");
        seteuid(getuid());
        return -1;
    }
    
    // set the global variable server_gid
    server_gid = grp->gr_gid;
    seteuid(getuid());

    return 0;

}

int handle_create_user(const char* username, mode_t perms, const char* root, char *err_msg, uint32_t err_size){
    // first obtain the current effective UID to restore it later
    uid_t original_euid = geteuid();

    int rc = -1;

    // check if username is valid according to unix username rules
    if(validate_username(username, err_msg, err_size) < 0) {
        err_msg[err_size - 1] = '\0';
        return -1;
    }

    // check the validity of provided permissions
    if(perms > 0777){
        snprintf(err_msg, err_size - 1, "Invalid permissions: must be between 0000 and 0777");
        err_msg[err_size - 1] = '\0';
        return -1;
    }

    // set effective UID to root to create the user
    if(seteuid(0) < 0) {
        snprintf(err_msg, err_size - 1, "Failed to set effective UID to root: %s", strerror(errno));
        err_msg[err_size - 1] = '\0';
        return -1;
    }

    // obtain a pwd struct for the new user
    struct passwd *pwd = getpwnam(username);

    if(pwd == NULL){ // new user
        if(geteuid() !=0) {
            strncpy(err_msg, "Insufficient privileges to create user", err_size - 1);
            err_msg[err_size - 1] = '\0';
            errno = EPERM;

            if(seteuid(original_euid) < 0) {
                snprintf(err_msg, err_size - 1, "Failed to restore effective UID: %s", strerror(errno));
                err_msg[err_size - 1] = '\0';
                return -1;
            }

            return rc;
        }

        // handle user creation using fork() and execvp to call useradd
        char *argv[] = { "adduser", "--disabled-password", "--gecos", "", "--ingroup", "csap_group", "--no-create-home", (char *)username, NULL};
        if(run_exec("adduser", argv) < 0){
            strncpy(err_msg, "Failed to create user", err_size - 1);
            err_msg[err_size - 1] = '\0';

            if(seteuid(original_euid) < 0) {
                snprintf(err_msg, err_size - 1, "Failed to restore effective UID: %s", strerror(errno));
                err_msg[err_size - 1] = '\0';
                return -1;
            }

            return rc;
        }

        pwd = getpwnam(username); // verify that the user was created successfully
        if(pwd == NULL) {
            snprintf(err_msg, err_size - 1, "Failed to retrieve user info after creation");
            err_msg[err_size - 1] = '\0';

            if(seteuid(original_euid) < 0) {
                snprintf(err_msg, err_size - 1, "Failed to restore effective UID: %s", strerror(errno));
                err_msg[err_size - 1] = '\0';
                return -1;
            }

            return rc;

        }
    } else if(!is_csap_user(pwd)) { // if the user already exists but is not a valid CSAP user, return an error
        snprintf(err_msg, err_size - 1, "User exists but is not a valid CSAP user");
        err_msg[err_size - 1] = '\0';

        if(seteuid(original_euid) < 0) {
            snprintf(err_msg, err_size - 1, "Failed to restore effective UID: %s", strerror(errno));
            err_msg[err_size - 1] = '\0';
            return -1;
        }

        return rc;
    }

    // create the home directory for the new user
    if(create_home_directory(root, username, err_msg, err_size, perms, pwd) < 0) {
        if(seteuid(original_euid) < 0) {
            snprintf(err_msg, err_size - 1, "Failed to restore effective UID: %s", strerror(errno));
            err_msg[err_size - 1] = '\0';
            return -1;
        }
        return rc;
    }

    rc = 0; // user created successfully

    // restore the original effective UID
    if(seteuid(original_euid) < 0) {
        snprintf(err_msg, err_size - 1, "Failed to restore effective UID: %s", strerror(errno));
        err_msg[err_size - 1] = '\0';
        return -1;

    }

    return rc;

}

/*
Function used to build the fifo path for a specific user, given their PID and the root directory.
*/
void session_fifo_path(char *out_path, size_t out_size, const char *root, pid_t pid) {
    snprintf(out_path, out_size, "%s/.sessions/fifo_%d", root, pid);
}

/*
Writes a formatted message in the fifo, using 
the provided format and variable arguments.
*/
int notify_pid(const char *root, pid_t pid, const char *format, ...) {
    char fifo_path[PATH_MAX + 64];
    session_fifo_path(fifo_path, sizeof(fifo_path), root, pid); // construct fifo path

    // open fifo in O_NONBLOCK mode to avoid blocking if the reader is not ready
    int fd = open(fifo_path, O_WRONLY | O_NONBLOCK);
    if(fd < 0) {
        return -errno;
    }

    char msg[PATH_MAX + 128];
    va_list args; // initialize variable argument list
    va_start(args, format);
    int n = vsnprintf(msg, sizeof(msg), format, args); // format the message
    va_end(args);

    if(n<0){
        close(fd);
        return -EIO;
    }

    int r = write_all(fd, msg, n); // write the message to the fifo
    close(fd);

    return r;
}

int handle_login(session_t *session, char *username, char *err_msg, uint32_t err_size) {

    int n_fd = -1;
    int rc = -1;
    char fifo_name[PATH_MAX + 64];
    fifo_name[0] = '\0';

    // first check if the user already exists in the system
    struct passwd *pwd = getpwnam(username);
    if(pwd == NULL){
        snprintf(err_msg, err_size - 1, "User does not exist");
        return -1;
    }

    // if it exists, check its a valid csap_user
    if(!is_csap_user(pwd)) {
        snprintf(err_msg, err_size - 1, "User exists but is not a valid CSAP user");
        return -1;
    }

    // check if the user's home directory exists in the root directory
    char home_dir[PATH_MAX + 64];
    snprintf(home_dir, sizeof(home_dir), "%s/%.32s", session->root_path, username);

    // canonicalize the home_dir path to avoid issues with symlinks or relative paths
    char resolved_home[PATH_MAX];
    if(validate_path(home_dir, session->root_path, resolved_home) < 0) {
        snprintf(err_msg, err_size - 1, "User's home directory is invalid or does not exist");
        return -1;
    }

    struct stat st;
    if(stat(resolved_home, &st) < 0 || !S_ISDIR(st.st_mode)) {
        snprintf(err_msg, err_size - 1, "User's home directory does not exist");
        return -1;
    }

    // elevate privileges to create fifo in the .sessions directory
    if(seteuid(0) < 0) {
        snprintf(err_msg, err_size - 1, "Failed to set effective UID to root: %s", strerror(errno));
        return -1;
    }

    session_fifo_path(fifo_name, sizeof(fifo_name), session->root_path, getpid());

    // unlink fifo if it already exists
    unlink(fifo_name);


    // create the fifo for notifications
    if(mkfifo(fifo_name, 0660) < 0) {
        snprintf(err_msg, err_size - 1, "Failed to create fifo: %s", strerror(errno));
        
        if(n_fd >= 0) close(n_fd);
        if(fifo_name[0]) unlink(fifo_name);
        session->notify_fd = -1;
        session->logged_in = 0;
        if(geteuid() == 0) drop_privileges();
        return rc;
    }

    // open the fifo for reading and writing to avoid blocks and EOF
    if((n_fd = open(fifo_name, O_RDWR | O_NONBLOCK)) < 0) {
        snprintf(err_msg, err_size - 1, "Failed to open fifo: %s", strerror(errno));
        unlink(fifo_name);
        session->notify_fd = -1;
        session->logged_in = 0;
        if(geteuid() == 0) drop_privileges();
        return rc;
    }

    // set ownership of .sessions dir to root:csap_group
    char session_dir[PATH_MAX + 64];
    snprintf(session_dir, sizeof(session_dir), "%s/.sessions", session->root_path);
    if(chown(session_dir, 0, server_gid) < 0) {
        snprintf(err_msg, err_size - 1, "Failed to set ownership of .sessions directory: %s", strerror(errno));
        close(n_fd);
        unlink(fifo_name);
        session->notify_fd = -1;
        session->logged_in = 0;
        if(geteuid() == 0) drop_privileges();
        return rc;
    }

    // set ownership of the named fifo to user
    if(chown(fifo_name, pwd->pw_uid, server_gid) < 0) {
        snprintf(err_msg, err_size - 1, "Failed to set ownership of fifo: %s", strerror(errno));
        close(n_fd);
        unlink(fifo_name);
        session->notify_fd = -1;
        session->logged_in = 0;
        if(geteuid() == 0) drop_privileges();
        return rc;
    }

    // set CWD to the user's home directory
    if(chdir(resolved_home) < 0) {
        snprintf(err_msg, err_size - 1, "Failed to change directory to user's home: %s", strerror(errno));
        close(n_fd);
        unlink(fifo_name);
        session->notify_fd = -1;
        session->logged_in = 0;
        if(geteuid() == 0) drop_privileges();
        return rc;
    }

    // set effective UID to the user's UID and group ID to the server's group ID
    if(setegid(server_gid) <0){
        snprintf(err_msg, err_size - 1, "Failed to set effective GID: %s", strerror(errno));
        close(n_fd);
        unlink(fifo_name);
        session->notify_fd = -1;
        session->logged_in = 0;
        if(geteuid() == 0) drop_privileges();
        return rc;
    }

    if(seteuid(pwd->pw_uid) < 0) {
        snprintf(err_msg, err_size - 1, "Failed to set effective UID: %s", strerror(errno));
        close(n_fd);
        unlink(fifo_name);
        session->notify_fd = -1;
        session->logged_in = 0;
        if(geteuid() == 0) drop_privileges();
        return rc;
    }

    // build session struct
    session->notify_fd = n_fd;
    session->logged_in = 1;
    session->uid = pwd->pw_uid;
    snprintf(session->user, sizeof(session->user), "%s", username);
    snprintf(session->home_path, sizeof(session->home_path), "%s", resolved_home);

    return 0;

}