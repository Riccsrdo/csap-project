/* session.c

Responsible for:
- creating users;
- login;
- session table in shm;
- privileges (with seteuid).
*/
#include"session.h"

gid_t server_gid = 0;

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

    // create the group, using fork() and execlp to call groupadd
    fflush(NULL); // flush all stdio buffers before forking
    pid_t pid = fork();
    if(pid < 0) {
        snprintf(error_msg, err_size - 1, "Failed to fork for group creation: %s", strerror(errno));
        return -1;
    } else if(pid == 0) { // child process
        execlp("groupadd", "groupadd", "csap_group", NULL);
        // if execlp returns, it means it failed
        perror("execlp groupadd");
        _exit(EXIT_FAILURE); // use _exit to avoid flushing stdio buffers again
    } else { // parent process
        int status;
        waitpid(pid, &status, 0);
        if(WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            // group created successfully, get the gid
            grp = getgrnam("csap_group");
            if(grp) {
                server_gid = grp->gr_gid;
                // set effective UID back to original user
                seteuid(getuid());
                return 0;
            } else {
                snprintf(error_msg, err_size - 1, "Failed to get group info after creation: %s", strerror(errno));
                seteuid(getuid());
                return -1;
            }
        }
        else {
            snprintf(error_msg, err_size - 1, "Failed to create group: %s", strerror(errno));
            error_msg[err_size - 1] = '\0';
            seteuid(getuid());
            return -1;
        }
    }

}

int handle_create_user(const char* username, mode_t perms, const char *root, char *err_msg, uint32_t err_size) {
    // first check if username already exists in the system
    struct passwd *pwd = getpwnam(username);
    if(pwd == NULL) {
        // create user
        // if the user does not exist, validate first the username, checking if it's in[a-z0-9_-],
        // first character not a number, and length between 3 and 32 characters
        size_t len = strlen(username);
        if(len < 3 || len > 32) {
            strncpy(err_msg, "Username must be between 3 and 32 characters", err_size - 1);
            err_msg[err_size - 1] = '\0';
            return -1;
        }
        if(!isalpha(username[0])) {
            strncpy(err_msg, "Username must start with a letter", err_size - 1);
            err_msg[err_size - 1] = '\0';
            return -1;
        }
        for(size_t i = 0; i < len; i++) {
            // if character i is not in [a-z0-9_-], return error
            if(!isalnum(username[i]) && username[i] != '_' && username[i] != '-') {
                strncpy(err_msg, "Username can only contain letters, numbers, underscores and hyphens", err_size - 1);
                err_msg[err_size - 1] = '\0';
                return -1;
            }
        }

        // parsing of permissions, validate perms is between 0000 and 0777
        if(perms > 0777) {
            strncpy(err_msg, "Permissions must be between 0000 and 0777", err_size - 1);
            err_msg[err_size - 1] = '\0';
            return -1;
        }

        // create the user, using fork() and execlp to call useradd

        // check if the server is running as root, if not return error
        if(geteuid() != 0) {
            strncpy(err_msg, "Server must be run as root to create users", err_size - 1);
            errno = EPERM;
            err_msg[err_size - 1] = '\0';
            return -1;
        }

        fflush(NULL); // flush all stdio buffers before forking
        pid_t pid = fork();
        if(pid < 0) {
            strncpy(err_msg, "Failed to fork for user creation", err_size - 1);
            err_msg[err_size - 1] = '\0';
            return -1;
        } else if(pid == 0) { // child process
            // set the group for the new user to the server's group
            execlp("adduser", "adduser", "--disabled-password", "--gecos", "", "--ingroup", "csap_group",
            "--no-create-home", username, NULL);
            // if execlp returns, it means it failed
            perror("execlp adduser");
            _exit(EXIT_FAILURE); // use _exit to avoid flushing stdio buffers again
        } else { // parent process
            int status;
            waitpid(pid, &status, 0);
            if(WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                // user created successfully
                // obtain uid
                struct passwd *new_pwd = getpwnam(username);
                if(!new_pwd) {
                    strncpy(err_msg, "Failed to get new user info", err_size - 1);
                    err_msg[err_size - 1] = '\0';
                    return -1;
                }

                int result = create_home_directory(root, username, err_msg, err_size, perms, new_pwd);
                if(result < 0) {
                    return -1;
                }

                return 0;
            }
            else {
                strncpy(err_msg, "Failed to create user", err_size - 1);
                err_msg[err_size - 1] = '\0';
                return -1;
            }
        }
    }

    // create folder for the user in the root directory, with the specified permissions

    int result = create_home_directory(root, username, err_msg, err_size, perms, pwd);
    if(result < 0) {
        return -1;
    }
    return 0;

}

int handle_login(session_t *session, char *username, char *err_msg, uint32_t err_size) {
    // TODO: Authentication mechanism here

    // check if the user exists in the system
    struct passwd *pwd = getpwnam(username);
    if(pwd == NULL) {
        strncpy(err_msg, "User does not exist", err_size - 1);
        err_msg[err_size - 1] = '\0';
        return -1;
    }

    // verify if the user's home directory exists in the root directory
    char home_dir[PATH_MAX];
    snprintf(home_dir, sizeof(home_dir), "%s/%s", session->root_path, username);

    // perform canonicalization of the home_dir path to ensure it is within the root directory
    char resolved_home[PATH_MAX];
    if(realpath(home_dir, resolved_home) == NULL) {
        strncpy(err_msg, "Failed to resolve home directory path", err_size - 1);
        err_msg[err_size - 1] = '\0';
        return -1;
    }

    struct stat st;
    if(stat(resolved_home, &st) < 0 || !S_ISDIR(st.st_mode)) {
        strncpy(err_msg, "User's home directory does not exist", err_size - 1);
        err_msg[err_size - 1] = '\0';
        return -1;
    }

    // populate the session struct with the user's information
    session->uid = pwd->pw_uid;
    strncpy(session->user, username, sizeof(session->user) - 1);
    session->user[sizeof(session->user) - 1] = '\0';
    snprintf(session->home_path, sizeof(session->home_path), "%s", resolved_home);
    session->home_path[sizeof(session->home_path) - 1] = '\0';


    char fifo_name[PATH_MAX + 64];
    snprintf(fifo_name, sizeof(fifo_name), "%s/.sessions/fifo_%d", session->root_path, getpid());

    // Unlink the fifo if it already exists
    unlink(fifo_name);

    if(mkfifo(fifo_name, 0600) <0){
        strncpy(err_msg, "Failed to create FIFO for notifications", err_size - 1);
        err_msg[err_size - 1] = '\0';
        perror("mkfifo");
        return -1;
    }

    // open fifo for reading and writing, I avoid in this way blocking behaviour and EOF
    if((session->notify_fd = open(fifo_name, O_RDWR)) < 0){
        perror("open fifo");
        strncpy(err_msg, "Failed to open FIFO for notifications", err_size - 1);
        err_msg[err_size - 1] = '\0';
        unlink(fifo_name);
        return -1;
    }

    // set cwd of the user to their home directory
    if(chdir(home_dir) < 0) {
        snprintf(err_msg, err_size - 1, "Failed to change directory to user's home: %s", strerror(errno));
        return -1;
    }

    if(setegid(server_gid) < 0) {
        snprintf(err_msg, err_size - 1, "Failed to set effective GID: %s", strerror(errno));
        return -1;
    }
    if(seteuid(session->uid) < 0) {
        snprintf(err_msg, err_size - 1, "Failed to set effective UID: %s", strerror(errno));
        return -1;
    }

    session->logged_in = 1;


    return 0;
}