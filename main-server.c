/* main-server.c

Responsible for:
- Starting;
- Parsing of argv:
- Setup of shm and sem;
- loop accept and fork;
- overall cleanup of resources.
*/

#include<netinet/in.h> // Address information
#include<stdio.h>
#include<stdlib.h>
#include<sys/socket.h> // Socket APIs
#include<arpa/inet.h> // inet_pton
#include<string.h>
#include<unistd.h>
#include<sys/select.h> // select
#include<signal.h>
#include<fcntl.h> // fcntl
#include<sys/wait.h> // waitpid
#include<errno.h>
#include"utils/utils.h"
//#include"network.h"
#include"network/network.h" // fix with -I
#include"paths/paths.h"
#include"protocol/protocol.h"
#include"fsops/fsops.h"
#include<sys/types.h>
#include<sys/stat.h>
#include<limits.h>
#include"session/session.h"
#include"transfer/transfer.h"
#include"locks/locks.h"
#include"sh_mem/sh_mem.h"

#define MAX_CLIENT_BUFFER 1024

char root_directory[PATH_MAX]; // global variable to hold the root directory path

// setup shared memory segment
int shm_id = -1, sem_id = -1;
shared_memory_t *shared_memory = NULL;

volatile sig_atomic_t sessions_cleanup = 0; // flag to indicate that we need to clean up sessions

static void cleanup_ipc(void){
    cleanup_shmem(shm_id, sem_id, shared_memory);
    shm_id = -1;
    shared_memory = NULL;
}

void handle_termination(int sig){
    (void)sig; // suppress unused parameter warning
    cleanup_ipc();
    signal(SIGTERM, SIG_IGN); // ignore further SIGTERM signals
    kill(0, SIGTERM); // send SIGTERM to all processes in the same process group
    _exit(EXIT_SUCCESS); // exit the process
}

void handle_signals(int sig){
    (void)sig;

    int saved_errno = errno; // save errno to restore it later
    while(waitpid(-1, NULL, WNOHANG) > 0) {
        sessions_cleanup = 1; // set the flag to indicate that we need to clean up sessions
    }
    errno = saved_errno; // restore errno
}

static void collect_stale_sessions(){
    sessions_cleanup = 0; // reset the flag

    if(shared_memory == NULL || sem_id < 0) {
        return;
    }

    int r = shm_sessions_collect_stale(shared_memory, sem_id);
    if(r < 0) {
        fprintf(stderr, "Error collecting stale sessions: %s\n", strerror(-r));
        sessions_cleanup = 1; // set the flag to indicate that we need to clean up sessions again
    }
}

int setup_signal_handler() {
    struct sigaction sa;
    sa.sa_handler = handle_signals;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; // restart interrupted system calls
    if(sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
    return 0;
}


/*
Start server, listen on localhost:8080, return the socket descriptor.
*/

int start_server(char *ip_address, char *port_number){
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if(sockfd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    printf("[SETUP]: Socket created successfully.\n");

    int opt1  = 1;
    // Set SO_REUSEADDR to allow the socket to be bound to an address that is already in use.
    if(setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt1, sizeof(opt1)) < 0) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_address;
    // construct socket endpoint
    if(make_endpoint(ip_address, port_number, &server_address) < 0){
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // Bind the socket to the address
    if(bind(sockfd, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) {
        perror("bind");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    
    printf("[SETUP]: Socket bound to %s:%s\n", ip_address, port_number);

    // listen for incoming connections
    if(listen(sockfd, 10) < 0) {
        perror("listen");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    printf("[SETUP]: Listening for incoming connections...\n");

    return sockfd;

}

void configure_read_set(fd_set *readfds, int socket_fd) {
    FD_ZERO(readfds);
    FD_SET(STDIN_FILENO, readfds); // add stdin to the set
    FD_SET(socket_fd, readfds); // add the socket to the set
}

/*
Function used to return correct error message related to errno outputted by validate_path function.
*/
static const char *path_error_reason(int err) {
    switch(err) {
        case EPERM:        
            return "Path outside of allowed scope";
        case ENOENT:       
            return "No such file or directory (check the parent directory)";
        case ENAMETOOLONG: 
            return "Path or file name too long";
        case EACCES:       
            return "Permission denied on a path component";
        case ELOOP:        
            return "Too many symbolic links in path";
        case ENOTDIR:      
            return "A component of the path is not a directory";
        case ENOMEM:       
            return "Out of memory while resolving the path";
        default:           
            return "Invalid path";
    }
}


/*
Parse cmd from payload into cmd_t structure.
*/
static int cmd_parse(int fd, const void *payload, cmd_t *out){
    char error_msg[256];
    error_msg[0] = '\0'; 

    if(parse_command(payload, out, error_msg, sizeof(error_msg)) < 0) {
        send_err(fd, EINVAL, error_msg);
        return -1;
    }

    return 0;
}

/*
Validate path with provided root directory, and return the resolved path in out.
*/
static int cmd_resolve(int fd, const char* path, const char *root, char *out){
    int r = validate_path(path, root, out);
    if(r < 0) {
        send_err(fd, -r, path_error_reason(-r));
        return -1;
    }
    return 0;
}

/*
Validate permissions string and convert it to mode_t.
*/
static int cmd_perms(int fd, const char *arg, mode_t *out){
    long perms = 0;
    int r = validate_permissions(&perms, (char*) arg);
    if(r < 0) {
        send_err(fd, EINVAL, "Invalid permissions format");
        return -1;
    }
    *out = (mode_t)perms;
    return 0;
}


void dispatch(session_t *session, int clientSocket, uint8_t command, void *payload, uint32_t payload_len) {
    (void)payload_len; // suppress warning
    
    char error_msg[256];
    error_msg[0] = '\0'; // initialize error message to empty string
    uint32_t err_size = sizeof(error_msg);

    // Commands without login
    if(command == CMD_LOGIN) {
        
        // parse payload to extract username
        cmd_t login_command;
        if(cmd_parse(clientSocket, payload, &login_command) < 0) {
            return; // error message already sent to client
        }

        // if user has already logged in, send error response to client
        if(session->logged_in) {
            char reason[256];  
            snprintf(reason, sizeof(reason), "User %s is already logged in", session->user);
            send_err(clientSocket, EACCES, reason);
            return;
        }

        char *username = login_command.buf;
        if(strlen(username) > sizeof(session->user) - 1) {
            const char *reason = "Username too long";
            send_err(clientSocket, EINVAL, reason);
            return;
        }

        uint32_t err_size = sizeof(error_msg);
        int result = handle_login(session, username, error_msg, err_size);
        if(result < 0) {
            // send error response to client indicating that login failed
            send_err(clientSocket, EACCES, error_msg);
        } else {
            // send success response to client
            char msg[128];
            int n = snprintf(msg, sizeof(msg), "Logged in as %s", session->user);
            send_ok(clientSocket, msg, n);

            // add the session to the shared memory
            int r = shm_session_add(shared_memory, sem_id, session->user, getpid());
            if(r < 0) {
                fprintf(stderr, "Failed to add session to shared memory: %s\n", strerror(-r));
            }

            // check requests arrived while user was offline, and notify the user if any
            transfer_request_entry_t requests[MAX_ENTRIES];
            int num_requests = shm_requests_for_dest(shared_memory, sem_id, session->user, requests, MAX_ENTRIES);
            
            for(int i = 0; i < num_requests; i++) {
                char base_name[NAME_MAX + 1];
                path_basename(base_name, sizeof(base_name), requests[i].file_path_absolute);

                int r = notify_pid(session->root_path, getpid(), "REQ|%d|%s\n", requests[i].id, base_name);

                if(r < 0) {
                    fprintf(stderr, "Failed to notify user %s: %s\n", session->user, strerror(-r));
                }
            }
        }

        #if DEBUG
        printf("[DEBUG]: User %s logged in with UID %d\n", session->user, session->uid);
        #endif

        return;
    }
    if(command == CMD_CREATE_USER) {
    
        cmd_t cu_command;
        if(cmd_parse(clientSocket, payload, &cu_command) < 0) {
            return; 
        }

        if(cu_command.argc < 2) {
            const char *reason = "Insufficient arguments for create_user";
            send_err(clientSocket, EINVAL, reason);
            return;
        }

        mode_t perms;
        if(cmd_perms(clientSocket, cu_command.buf2, &perms) < 0) {
            return; 
        }

        uint32_t err_size = sizeof(error_msg);
        int result = handle_create_user(cu_command.buf, perms, session->root_path, error_msg, err_size);
        // check if err_message is not empty, if so send it to the client
        if(result < 0) {
            send_err(clientSocket, EACCES, error_msg[0] ? error_msg : "Failed to create user");
        } else {
            // send success response to client
            send_ok(clientSocket, NULL, 0);
        }

        #if DEBUG
        printf("[DEBUG]: Create user command executed for user %s with permissions %o\n", cu_command.buf, (unsigned int)perms);
        #endif
        
        return;
    }

    // Commands with login

    // first check if user is logged in, if not send an error response to the client
    if(!session->logged_in) {
        // send error response to client indicating that login is required
        const char *reason = "Login required, please execute first 'login <username>' command";
        send_err(clientSocket, EACCES, reason);
        return;
    }

    // terminate with send_ok or send_err, depending on the result of the command execution
    switch(command){
        case(CMD_ACCEPT):{

            cmd_t accept_command;
            if(cmd_parse(clientSocket, payload, &accept_command) < 0) {
                return; 
            }

            // accept <directory> <request_id>
            int request_id = atoi(accept_command.buf2);
            if(request_id <= 0) {
                const char *reason = "Invalid request ID";
                send_err(clientSocket, EINVAL, reason);
                return;
            }

            // validate dest path against home_path, if not valid return error
            char full_dest_path[PATH_MAX];
            if(cmd_resolve(clientSocket, accept_command.buf, session->home_path, full_dest_path) < 0) {
                return;
            }

            transfer_request_entry_t request_copy;
            // accept the request, changing its status to ACCEPTED and retrieving the request information
            int r = shm_request_take(shared_memory, sem_id, request_id, session->user, ACCEPTED, &request_copy);
            if(r < 0) {
                send_err(clientSocket, -r, "Failed to accept request");
                return;
            }

            // obtain source user PID and online status to notify them
            pid_t source_user_pid = -1;
            int source_user_online = 0;
            int found = shm_session_find(shared_memory, sem_id, request_copy.source_username, &source_user_pid, &source_user_online);

            if(found < 0) {
                send_err(clientSocket, -found, "Failed to look up source user");
                return;
            }

            if(found == 0){
                send_err(clientSocket, ENOENT, "Source user not found");
                return;
            }

            char base_name[NAME_MAX + 1];
            path_basename(base_name, sizeof(base_name), request_copy.file_path_absolute);

            // final path of copying given by <dir> plus file name
            char full_dest_file_path[PATH_MAX];
            snprintf(full_dest_file_path, sizeof(full_dest_file_path), "%s/%s", full_dest_path, base_name);

            int copy_result = execute_transfer_copy(request_copy.file_path_absolute, full_dest_file_path);
            if(copy_result < 0) {
                shm_request_set(shared_memory, sem_id, request_id, FAILED); // set the request status to FAILED
            }

            // unlock sender
            if(source_user_online){
                int r;
                if(copy_result < 0) {
                    r = notify_pid(session->root_path, source_user_pid, "FAIL|%d|%d\n",
                                    request_copy.id, -copy_result);
                } else {
                    r = notify_pid(session->root_path, source_user_pid, "ACCEPT|%d|%s\n",
                                    request_copy.id, full_dest_file_path);
                }

                if(r < 0){
                    send_err(clientSocket, -r, "Failed to notify source user about acceptance");
                    return;
                }
            }

            if(copy_result < 0) {
                send_err(clientSocket, -copy_result, "Failed to copy file");
                return;
            }

            char msg[128 + NAME_MAX];
            snprintf(msg, sizeof(msg), "Transfer request accepted successfully, file copied to %s", full_dest_file_path);
            send_ok_str(clientSocket, msg);

            break;
        }
        case(CMD_REJECT):{
            cmd_t reject_command;
            if(cmd_parse(clientSocket, payload, &reject_command) < 0) {
                return; 
            }

            // reject <request_id>

            int request_id = atoi(reject_command.buf);
            if(request_id <= 0) {
                const char *reason = "Invalid request ID";
                send_err(clientSocket, EINVAL, reason);
                return;
            }

            transfer_request_entry_t request_copy;
            int r= shm_request_take(shared_memory, sem_id, request_id, session->user, REJECTED, &request_copy);
            if(r < 0) {
                send_err(clientSocket, -r, "Failed to reject request");
                return;
            }

            // look up for the source user in session table to get the PID
            pid_t source_user_pid = -1;
            int source_user_online = 0;
            int found = shm_session_find(shared_memory, sem_id, request_copy.source_username, &source_user_pid, &source_user_online);

            if(found < 0) {
                send_err(clientSocket, -found, "Failed to look up source user");
                return;
            }

            if(found == 0){
                send_err(clientSocket, ENOENT, "Source user not found");
                return;
            }

            if(source_user_online){
                r = notify_pid(session->root_path, source_user_pid, "REJECT|%d\n", request_copy.id);
                if(r < 0){
                    send_err(clientSocket, -r, "Failed to notify source user about rejection");
                    return;
                }
            }

            send_ok_str(clientSocket, "Transfer request rejected successfully");

            break;
        }
        case(CMD_TRANSFER_REQ):{
            cmd_t transfer_command;
            if(cmd_parse(clientSocket, payload, &transfer_command) < 0) {
                return; 
            }

            // transfer_request <file> <dest_user>
            // validate source path against home_path, if not valid return error
            char full_src_path[PATH_MAX];
            if(cmd_resolve(clientSocket, transfer_command.buf, session->home_path, full_src_path) < 0) {
                return;
            }

            // validate if source file exists and is a regular file
            struct stat src_st;
            if(stat(full_src_path, &src_st) < 0) {
                send_err(clientSocket, errno, "Source file does not exist");
                return;
            }
            if(!S_ISREG(src_st.st_mode)) {
                send_err(clientSocket, EINVAL, "Only regular files can be transferred");
                return;
            }

            pid_t dest_user_pid = -1;
            int dest_online = 0;

            // look if dest user is online, if so get their PID, else return error
            int found = shm_session_find(shared_memory, sem_id, transfer_command.buf2, &dest_user_pid, &dest_online);

            if(found < 0) {
                send_err(clientSocket, ENOENT, "Destination user not found");
                return;
            }

            if(!found || !dest_online) {
                // dest. user is not online, request remains pending until they log in

                // verify if dest. user exists
                dest_online = 0;

                struct passwd *pwd = getpwnam(transfer_command.buf2);
                
                char dest_user_home[PATH_MAX + NAME_MAX + 2];
                struct stat dest_home_stat;
                snprintf(dest_user_home, sizeof(dest_user_home), "%s/%s", session->root_path, transfer_command.buf2);

                if(pwd == NULL || !is_csap_user(pwd) || stat(dest_user_home, &dest_home_stat) < 0 || !S_ISDIR(dest_home_stat.st_mode)) {
                    send_err(clientSocket, ENOENT, "Destination user does not exist");
                    return;
    
                }
            }

            int new_id = shm_request_add(shared_memory, sem_id, session->user, transfer_command.buf2, full_src_path);

            if(new_id < 0) {
                send_err(clientSocket, ENOMEM, "Failed to add transfer request");
                return;
            }

            if(dest_online){
                char base_name[NAME_MAX + 1];
                path_basename(base_name, sizeof(base_name), full_src_path);

                int r = notify_pid(session->root_path, dest_user_pid, "REQ|%d|%s\n", new_id, base_name);

                if(r < 0){
                    send_err(clientSocket, -r, "Failed to notify destination user about transfer request");
                    return;
                }
            }

            // no message sent here as the client remains pending until dest. user accepts or rejects

            break;
        }
        case(CMD_CREATE): {
            cmd_t create_command;
            if(cmd_parse(clientSocket, payload, &create_command) < 0) {
                return; 
            }

            // validate path against home_path, if not valid return error
            char full_path[PATH_MAX];
            if(cmd_resolve(clientSocket, create_command.buf, session->home_path, full_path) < 0) {
                return;
            }

            int r;

            mode_t perms = 0;
            if(cmd_perms(clientSocket, create_command.buf2, &perms) < 0) {
                return; 
            }

            int create_result = create_cmd(full_path, perms, create_command.is_dir);
            if(create_result < 0) {
                const char *reason = "Failed to create file/directory";
                send_err(clientSocket, -create_result, reason);
                return;
            }

            const char *reason = "File/Directory created successfully";
            send_ok_str(clientSocket, reason);

            break;
        }
        case(CMD_CHMOD): {

            cmd_t chmod_command;
            if(cmd_parse(clientSocket, payload, &chmod_command) < 0) {
                return;
            }

            // validate path against home_path, if not valid return error
            char full_path[PATH_MAX];
            if(cmd_resolve(clientSocket, chmod_command.buf, session->home_path, full_path) < 0) {
                return;
            }

            mode_t perms;
            if(cmd_perms(clientSocket, chmod_command.buf2, &perms) < 0) {
                return; 
            }

            int chmod_result = chmod_cmd(full_path, perms);
            if(chmod_result < 0) {
                const char *reason = "Failed to change permissions";
                send_err(clientSocket, -chmod_result, reason);
                return;
            }

            const char *reason = "Permissions changed successfully";
            send_ok_str(clientSocket, reason);

            break;
        }
        case(CMD_MOVE): {
            cmd_t move_command;
            if(cmd_parse(clientSocket, payload, &move_command) < 0) {
                return;
            }

            // validate source path against home_path, if not valid return error
            char full_src_path[PATH_MAX];
            if(cmd_resolve(clientSocket, move_command.buf, session->home_path, full_src_path) < 0) {
                return;
            }

            // validate destination path against home_path, if not valid return error
            char full_dest_path[PATH_MAX];
            if(cmd_resolve(clientSocket, move_command.buf2, session->home_path, full_dest_path) < 0) {
                return;
            }

            // obtain stats of source and destination
            struct stat src_stat, dest_stat;
            if(stat(full_src_path, &src_stat) < 0) {
                const char *reason = "Failed to get source file/directory info";
                send_err(clientSocket, errno, reason);
                return;
            }

            int src_fd = -1;
            int dest_fd = -1;

            if(S_ISREG(src_stat.st_mode)) {
                // if source is a regular file, acquire write lock on it
                src_fd = open(full_src_path, O_RDWR);
                if(src_fd < 0) {
                    const char *reason = "Failed to open source file for locking";
                    send_err(clientSocket, errno, reason);
                    return;
                }

                int r = acquire_write_lock(src_fd, 0, 0);
                if(r<0) {
                    const char *reason = "Failed to acquire write lock on source file";
                    send_err(clientSocket, -r, reason);
                    close(src_fd);
                    return;
                }
            }

            char dest_path_final[PATH_MAX + NAME_MAX + 2]; // +2 for '/' and '\0'
            strncpy(dest_path_final, full_dest_path, sizeof(dest_path_final) - 1);
            dest_path_final[sizeof(dest_path_final) - 1] = '\0';

            // no lock if src is a dir, as rename() of a dir is atomic

            // check if destination exists, if it's a file acquire write lock, if is a dir create a file with provided path + / + basename
            if(stat(dest_path_final, &dest_stat) == 0) {
                if(S_ISREG(dest_stat.st_mode)) {
                    dest_fd = open(dest_path_final, O_RDWR);
                    if(dest_fd < 0) {
                        const char *reason = "Failed to open destination file for locking";
                        send_err(clientSocket, errno, reason);
                        if(src_fd >= 0) {
                            release_lock(src_fd, 0, 0);
                            close(src_fd);
                        }
                        return;
                    }

                }

                if(S_ISDIR(dest_stat.st_mode) && S_ISREG(src_stat.st_mode)){ // check if source is a file, if it's a dir use rename()

                    char src_path_copy[PATH_MAX];
                    strncpy(src_path_copy, full_src_path, sizeof(src_path_copy) - 1);
                    src_path_copy[sizeof(src_path_copy) - 1] = '\0';
                    char *base_name = basename(src_path_copy);

                    char dest[PATH_MAX + NAME_MAX + 2]; // +2 for '/' and '\0'

                    snprintf(dest, sizeof(dest), "%s/%s", full_dest_path, base_name);
                    strncpy(dest_path_final, dest, sizeof(dest_path_final) - 1);
                    dest_path_final[sizeof(dest_path_final) - 1] = '\0';

                    // create the file in the destination directory with the same basename as the source file
                    dest_fd = open(dest_path_final, O_RDWR | O_CREAT | O_EXCL, src_stat.st_mode & 0777);
                    if(dest_fd < 0) {
                        const char *reason = "Failed to create destination file in directory";
                        send_err(clientSocket, errno, reason);
                        if(src_fd >= 0) {
                            release_lock(src_fd, 0, 0);
                            close(src_fd);
                        }
                        return;
                    }
                }

                
            }

            if(dest_fd >= 0) { // write lock only obtained if destination is a file, if it's a dir, we just created the file and we don't need to lock it
                int r = acquire_write_lock(dest_fd, 0, 0);
                if(r < 0) {
                    const char *reason = "Failed to acquire write lock on destination file";
                    send_err(clientSocket, -r, reason);
                    if(src_fd >= 0) {
                        release_lock(src_fd, 0, 0);
                        close(src_fd);
                    }
                    close(dest_fd);
                    return;
                }
            }
            

            int move_result = move_cmd(full_src_path, dest_path_final);
            if(move_result < 0) {

                if(src_fd >= 0) {
                    release_lock(src_fd, 0, 0);
                    close(src_fd);
                }
                if(dest_fd >= 0) {
                    release_lock(dest_fd, 0, 0);
                    close(dest_fd);
                }
                const char *reason = "Failed to move file/directory";
                send_err(clientSocket, -move_result, reason);
                return;
            }

            const char *reason = "File/Directory moved successfully";
            send_ok_str(clientSocket, reason);

            // unlock
            if(src_fd >= 0) {
                release_lock(src_fd, 0, 0);
                close(src_fd);
            }
            if(dest_fd >= 0) {
                release_lock(dest_fd, 0, 0);
                close(dest_fd);
            }

            break;
        }
        case(CMD_CD): {
            cmd_t cd_command;
            if(cmd_parse(clientSocket, payload, &cd_command) < 0) {
                return;
            }

            // validate path against home_path, if not valid return error
            char full_path[PATH_MAX];
            if(cmd_resolve(clientSocket, cd_command.buf, session->home_path, full_path) < 0) {
                return;
            }

            int cd_result = cd_cmd(full_path);
            if(cd_result < 0) {
                const char *reason = "Failed to change directory";
                send_err(clientSocket, -cd_result, reason);
                return;
            }

            const char *reason = "Directory changed successfully";
            send_ok_str(clientSocket, reason);

            break;
        }
        case(CMD_DOWNLOAD_BEGIN): {
            cmd_t download_command;
            if(cmd_parse(clientSocket, payload, &download_command) < 0) {
                return;
            }

            // validate path against home_path, if not valid return error
            char full_path[PATH_MAX];
            if(cmd_resolve(clientSocket, download_command.buf, session->home_path, full_path) < 0) {
                return;
            }

            ssize_t n = send_file(clientSocket, full_path, (off_t)0, CMD_DATA, CMD_DATA_END, error_msg, err_size);

            if(n < 0) {
                send_err(clientSocket, (int)-n, error_msg[0] ? error_msg : "Failed to download file");
                return;
            }

            break;
            
        }
        case(CMD_READ): {
            cmd_t read_command;
            if(cmd_parse(clientSocket, payload, &read_command) < 0) {
                return;
            }

            // validate path against home_path, if not valid return error
            char full_path[PATH_MAX];
            if(cmd_resolve(clientSocket, read_command.buf, session->home_path, full_path) < 0) {
                return;
            }

            ssize_t n = send_file(clientSocket, full_path, (off_t)read_command.offset, CMD_READ_DATA, CMD_READ_END, error_msg, err_size);

            if(n < 0) {
                send_err(clientSocket, (int)-n, error_msg[0] ? error_msg : "Failed to read file");
                return;
            }

            break;
        }
        case(CMD_UPLOAD_BEGIN): {
            cmd_t upload_command;
            if(cmd_parse(clientSocket, payload, &upload_command) < 0) {
                return;
            }

            // validate path against home_path, if not valid return error
            char full_path[PATH_MAX];
            if(cmd_resolve(clientSocket, upload_command.buf2, session->home_path, full_path) < 0) {
                return;
            }

            // target struct
            target_t target;
            target.data_code = CMD_DATA;
            target.end_code = CMD_DATA_END;
            target.path = full_path;
            target.offset = -1;

            ssize_t n = receive_into_file(clientSocket, &target, error_msg, err_size);
            if(n < 0) {
                send_err(clientSocket, (int)-n, error_msg[0] ? error_msg : "Failed to receive data into file");
                return; 
            }

            char payload[32];
            int payload_len = snprintf(payload, sizeof(payload), "%lld", (long long)n);
            send_ok(clientSocket, payload, payload_len);
            break;
        }
        case(CMD_WRITE): {
            cmd_t write_command;
            if(cmd_parse(clientSocket, payload, &write_command)) return;

            char full_path[PATH_MAX];
            if(cmd_resolve(clientSocket, write_command.buf, session->home_path, full_path)) return;

            // initialize target struct to hold information about write operation
            target_t target;
            target.data_code = CMD_WRITE_DATA;
            target.end_code = CMD_WRITE_END;

            target.path = full_path;
            target.offset = (off_t)write_command.offset;

            // receive into full_path content
            ssize_t n = receive_into_file(clientSocket, &target, error_msg, err_size);
            if(n < 0) {
                send_err(clientSocket, (int)-n, error_msg[0] ? error_msg : "Failed to receive data into file");
                return;
            }

            char payload[32];
            int payload_len = snprintf(payload, sizeof(payload), "%lld", (long long)n);
            send_ok(clientSocket, payload, payload_len);
            break;

        }
        case(CMD_DELETE): {
            cmd_t delete_command;
            if(cmd_parse(clientSocket, payload, &delete_command) < 0) {
                return;
            }

            // validate path against home_path, if not valid return error
            char full_path[PATH_MAX];
            if(cmd_resolve(clientSocket, delete_command.buf, session->home_path, full_path) < 0) {
                return;
            }

            // check stats of file
            struct stat path_stat;
            if(stat(full_path, &path_stat) < 0) {
                const char *reason = "Failed to get file/directory info";
                send_err(clientSocket, errno, reason);
                return;
            }

            int fd = -1;

            // if it's a regular file
            if(S_ISREG(path_stat.st_mode)) {
                fd = open(full_path, O_RDWR);
                if(fd < 0) {
                    const char *reason = "Failed to open file for locking";
                    send_err(clientSocket, errno, reason);
                    return;
                }

                int r = acquire_write_lock(fd, 0, 0);
                if(r < 0) {
                    const char *reason = "Failed to acquire write lock on file";
                    send_err(clientSocket, -r, reason);
                    close(fd);
                    return;
                }
            
            }

            // if it's a directory, acquire no lock as rmdir() is atomic and will fail if the directory is not empty, so no need to lock it
        
            int delete_result = delete_cmd(full_path);
            if(delete_result < 0) {
                const char *reason = "Failed to delete file/directory";
                send_err(clientSocket, -delete_result, reason);
                if(fd >= 0) {
                    release_lock(fd, 0, 0);
                    close(fd);
                }
                return;
            }
            const char *reason = "File/Directory deleted successfully";
            send_ok_str(clientSocket, reason);
            
            // release lock on parent directory
            if(fd >= 0) {
                release_lock(fd, 0, 0);
                close(fd);
            }

            break;
        }
        case(CMD_LIST): {
            // tokenize payload with parse_command()
            cmd_t list_command;
            if(cmd_parse(clientSocket, payload, &list_command) < 0) {
                return;
            }

            // if path is NULL (from payload), pass NULL to list() to list current working directory
            // validate cwd against root_path, if not valid return error
            char full_path[PATH_MAX];
            if(list_command.buf[0] == '\0') {
                // get current working directory
                #if DEBUG
                printf("[DEBUG]: User:%s Listing current working directory\n", session->user);
                #endif
                char cwd[PATH_MAX];
                if(getcwd(cwd, sizeof(cwd)) == NULL) {
                    const char *reason = "Failed to get current working directory";
                    send_err(clientSocket, EINVAL, reason);
                    return;
                }

                // copy cwd to full_path
                strncpy(full_path, cwd, sizeof(full_path) - 1);
                full_path[sizeof(full_path) - 1] = '\0'; // ensure null
            } else {
                #if DEBUG
                printf("[DEBUG]: User:%s Listing directory: %s\n", session->user, list_command.buf);
                #endif

                // allowed scope: root_path (for all other commands is home_path)
                if(cmd_resolve(clientSocket, list_command.buf, session->root_path, full_path) < 0) {
                    return;
                }
            }

            // call list() in fsops
            strbuf_t sb;
            memset(&sb, 0, sizeof(sb));
            int list_result = list(full_path, &sb);
            if(list_result < 0) {
                const char *reason = "Failed to list directory";
                sb_free(&sb);
                send_err(clientSocket, -list_result, reason);
                return;
            }
            // if empty
            if(sb.len == 0) {
                const char *reason = "Directory is empty\n";
                if(sb_append(&sb, reason, strlen(reason))<0){
                    sb_free(&sb);
                    send_err(clientSocket, ENOMEM, "Failed to allocate memory for response");
                    return;
                }
            }

            // first announce how many bytes will be sent, then send the actual data
            char list_size_payload[32];
            int list_size_len =  snprintf(list_size_payload, sizeof(list_size_payload), "%llu", (unsigned long long)sb.len);

            if(send_ok(clientSocket, list_size_payload, list_size_len) < 0) {
                sb_free(&sb);
                return;
            }

            ssize_t list_sent = send_stream_buf(clientSocket, sb.data, sb.len, CMD_LIST_DATA, CMD_LIST_END);

            sb_free(&sb);
            if(list_sent < 0) {
                const char *reason = "Failed to send directory listing";
                send_err(clientSocket, (int)-list_sent, reason);
                return;
            }

            break;
        }
        default: {
            // send error response to client indicating that the command is not recognized
            const char *reason = "Command not recognized";
            send_err(clientSocket, EINVAL, reason);
        }
    }



}


void handle_session(int clientSocket) {
    session_t session;
    memset(&session, 0, sizeof(session)); // initialize the session struct to zero
    session.logged_in = 0; // not logged in initially
    session.notify_fd = -1; // no notification pipe initially

    strncpy(session.root_path, root_directory, PATH_MAX - 1); // set the root path for the session
    session.root_path[PATH_MAX - 1] = '\0'; 
    session.uid = (uid_t)-1; // invalid uid initially

    fd_set readfds;

    for(;;){
        // configure the readfds set for select(), used to check if there is activity on the socket
        FD_ZERO(&readfds);
        FD_SET(clientSocket, &readfds);
        
        int max_fd = clientSocket;

        if(session.notify_fd >= 0) {
            FD_SET(session.notify_fd, &readfds);
            if(session.notify_fd > max_fd) {
                max_fd = session.notify_fd;
            }
        }

        if(select(max_fd + 1, &readfds, NULL, NULL, NULL) < 0){
            if(errno == EINTR) continue; // if interrupted by signal, retry
            perror("select");
            break;
        }

        if(FD_ISSET(clientSocket, &readfds)) {
            // save command characteristics
            uint8_t command;
            void *payload = NULL;
            uint32_t payload_len;

            // receive packet
            if(recv_packet(clientSocket, (char**)&payload, &command, &payload_len) < 0){
                // error receiving packet, close the session
                free(payload);
                break;
            }

            #if DEBUG
            printf("[DEBUG]: Received command, payload content: %s\n", (char*)payload);
            #endif


            dispatch(&session, clientSocket, command, payload, payload_len);
            free(payload); // free the payload after dispatching
        }

        if(session.notify_fd >= 0 && FD_ISSET(session.notify_fd, &readfds)) {
            // handle transfer requests
            char notify_buffer[PATH_MAX+64];
            int eof_flag = 0;
            ssize_t n = read_line(session.notify_fd, notify_buffer, sizeof(notify_buffer)-1, &eof_flag);
            if(n < 0) {
                perror("read from pipe");
                break;
            }
            
            notify_buffer[n] = '\0'; // null-terminate the string

            // check if the buffer contains ACCEPT, REJECT or REQ
            if(strncmp(notify_buffer, "ACCEPT|", 7) == 0) {

                // extrapolate ID
                char *request_id = strtok(notify_buffer + 7, "|");

                char message[PATH_MAX + 64];
                snprintf(message, sizeof(message), "Transfer request with ID %s accepted",  request_id);
                send_ok(clientSocket, message, strlen(message));

                #if DEBUG
                printf("[DEBUG]: Received ACCEPT notification: %s\n", notify_buffer);
                #endif
            } else if(strncmp(notify_buffer, "REJECT|", 7) == 0) {
                char *request_id = strtok(notify_buffer + 7, "|");
                char message[PATH_MAX + 64];
                snprintf(message, sizeof(message), "Transfer request with ID %s rejected",  request_id);
                send_err(clientSocket, ECANCELED, message);

                #if DEBUG
                printf("[DEBUG]: Received REJECT notification: %s\n", notify_buffer);
                #endif
            } else if(strncmp(notify_buffer, "REQ|", 4) == 0) {

                char *request_id = strtok(notify_buffer + 4, "|");

                char message[PATH_MAX + 64];
                snprintf(message, sizeof(message), "Transfer request with ID %s received",  request_id);

                send_packet(clientSocket, RSP_NOTIFY, message, strlen(message));

                #if DEBUG
                printf("[DEBUG]: Received REQ notification: %s\n", notify_buffer);
                #endif
            } else if(strncmp(notify_buffer, "FAIL|", 5) == 0){
                // request was accepted but failed to complete

                char *request_id = strtok(notify_buffer + 5, "|");
                char *err_str = strtok(NULL, "|");
                int err_code = err_str ? atoi(err_str) : EIO;

                char message[PATH_MAX + 64];
                snprintf(message, sizeof(message), "Transfer request with ID %s failed",  request_id);
                send_err(clientSocket, err_code, message);

                #if DEBUG
                printf("[DEBUG]: Received FAIL notification: %s\n", notify_buffer);
                #endif


            } else {
                // unknown

                char message[PATH_MAX + 64];
                snprintf(message, sizeof(message), "Unknown notification received: %.128s",  notify_buffer);
                send_err(clientSocket, EPROTO, message);

                #if DEBUG
                printf("[DEBUG]: Received unknown notification: %s\n", notify_buffer);
                #endif
            }
            

            #if DEBUG
            printf("[DEBUG]: Received notification for a transfer request: %s\n", notify_buffer);
            #endif

        }
    }

    // remove any stale requests
    if(session.logged_in){
        shm_requests_set_failed(shared_memory, sem_id, session.user);
    }

    // cleanup session resources
    if(session.notify_fd >= 0) {
        close(session.notify_fd);
        char fifo_name[PATH_MAX + 64];
        session_fifo_path(fifo_name, sizeof(fifo_name), session.root_path, getpid());
        seteuid(0); // set effective UID to root to allow unlinking the fifo
        unlink(fifo_name); // remove the fifo
    }
    session.logged_in = 0; // mark as logged out
}


void create_root_directory(char *root_directory) {
    char sessions_dir[PATH_MAX];

    if(mkdir(root_directory, 0755) < 0 && errno != EEXIST) {
        perror("mkdir root_directory");
        exit(EXIT_FAILURE);
    }

    printf("[SETUP]: Root directory created and set to %s\n", root_directory);

    char resolved_root[PATH_MAX];
    if(realpath(root_directory, resolved_root) == NULL) {
        perror("realpath");
        exit(EXIT_FAILURE);
    }

    printf("[SETUP]: Resolved root directory path: %s\n", resolved_root);

    size_t root_len = strlen(resolved_root);
    while(root_len > 0 && resolved_root[root_len - 1] == '/') {
        resolved_root[root_len - 1] = '\0';
        root_len--;
    } // remove trailing slashes for the correct funcionality of validate_path

    // save resolved root path back to the global variable
    strncpy(root_directory, resolved_root, PATH_MAX - 1);
    root_directory[PATH_MAX - 1] = '\0';   

    // create .sessions directory inside the root directory
    snprintf(sessions_dir, sizeof(sessions_dir), "%s/.sessions", root_directory);
    if(mkdir(sessions_dir, 0770) < 0 && errno != EEXIST) { // 0770 to allow group members to access the directory
        perror("mkdir .sessions");
        exit(EXIT_FAILURE);
    }

    // clean any remaining enty in .sessions directory from previous runs
    DIR *dir = opendir(sessions_dir);
    if(dir) {
        struct dirent *entry;
        while((entry = readdir(dir)) != NULL) {
            if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            // if entry name begins with "fifo_"
            if(strncmp(entry->d_name, "fifo_", 5) == 0) {
                // extract the PID from the entry name
                pid_t pid = atoi(entry->d_name + 5);
                // check if the process with that PID is still running (0 is commonly used to check for existence)
                if(kill(pid, 0) == -1 && errno == ESRCH) { // ESRCH means the process does not exist
                    // process does not exist, safe to remove the fifo
                    char fifo_path[PATH_MAX + 320];
                    snprintf(fifo_path, sizeof(fifo_path), "%s/%s", sessions_dir, entry->d_name);
                    if(unlink(fifo_path) < 0) {
                        perror("unlink fifo");
                    } else {
                        printf("[SETUP]: Removed stale FIFO: %s\n", fifo_path);
                    }
                }
            }
        }
        closedir(dir);
    } else {
        perror("opendir .sessions");
        exit(EXIT_FAILURE);
    }

    printf("[SETUP]: .sessions directory created at %s\n", sessions_dir);
}


/*
Main

takes three arguments:
- root directory for the server to serve files from;
- IP address to bind to, default is 127.0.0.1;
- port number to bind to, default is 8080.

server is started with sudo, so I can create users through adduser and add them in the servers' group
*/
int main(int argc, char *argv[]) {

    umask(0); // set umask to 0 to allow full permissions for created files and directories

    setpgid(0, 0); // set the process group ID of the calling process to its own PID, so that all child processes are in the same group

    if(argc < 2) {
        fprintf(stderr, "Usage: %s <root_directory> [<IP>] [<port>]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    strncpy(root_directory, argv[1], PATH_MAX - 1);
    root_directory[PATH_MAX - 1] = '\0'; 
    char *ip_address = (argc > 2) ? argv[2] : "127.0.0.1";
    char *port_number = (argc > 3) ? argv[3] : "8080";

    create_root_directory(root_directory);
    
    // ignore SIGPIPE to avoid server crash when writing to a closed socket
    signal(SIGPIPE, SIG_IGN);

    // declare fd_set for select()
    fd_set readfds;

    // variables for socket and select()
    struct sockaddr_in clientAddress;
    socklen_t clientAddressLen = sizeof(clientAddress);
    int clientSocket;
    int select_result;
    char buffer[MAX_CLIENT_BUFFER];

    char err_msg[256]; // buffer for error messages
    uint32_t err_size = sizeof(err_msg);

    // first initialize group id 
    if(server_gid == 0){
        int result = setup_server_gid(err_msg, err_size);
        printf("[SETUP]: Server group ID initialized to %d\n", server_gid);
        if(result < 0){
            fprintf(stderr, "[SETUP]: %s\n", err_msg);
            exit(EXIT_FAILURE);

        }
    }

    // setup shared memory and semaphore for inter-process communication
    int r = setup_shmem(&shm_id, &sem_id, &shared_memory, server_gid);
    if(r < 0) {
        fprintf(stderr, "[SETUP]: Failed to setup shared memory and semaphore\n");
        exit(EXIT_FAILURE);
    }

    // setup signal handlers
    if(setup_signal_handler() < 0) {
        fprintf(stderr, "[SETUP]: Failed to setup signal handlers\n");
        exit(EXIT_FAILURE);
    }
    signal(SIGTERM, handle_termination); // handle SIGTERM to cleanup before exiting
    signal(SIGINT, handle_termination); 

    setup_unprivileged_user(); // setup unprivileged user for handling client requests

    // setup server socket
    int s = start_server(ip_address, port_number);

    printf("[SETUP]: Server started on %s:%s\n", ip_address, port_number);

    // determine max fd for select()
    int max_fd = (s > STDIN_FILENO) ? s : STDIN_FILENO;

    printf("[SETUP]: Server loop started. \tType 'exit' to close\n");

    while(1){
        // clean sessions table outside signal handler for SIGCHLD
        if(sessions_cleanup){
            collect_stale_sessions();
        }

        // clean file descriptor set
        configure_read_set(&readfds, s);

        // select() will block, waiting for activity
        select_result = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if (select_result < 0) {
            if (errno == EINTR) continue;
            perror("select"); break;
        }

        // check first if there is activity on stdin (server operator input)
        if(FD_ISSET(STDIN_FILENO, &readfds)) {
            // read content from stdin
            ssize_t bytes_read = read(STDIN_FILENO, buffer, sizeof(buffer) - 1);
            if(bytes_read < 0) {
                if(errno == EINTR) continue; // if interrupted by signal, retry
                perror("read from stdin");
                break;
            }
            if(bytes_read == 0) {
                // EOF, stdin closed
                printf("Stdin closed. Exiting server.\n");
                break;
            }
            buffer[bytes_read] = '\0';
            buffer[strcspn(buffer, "\n")] = '\0';
            if(strcmp(buffer, "exit") == 0) {
                printf("[CLOSE] Exiting server.\n");
                signal(SIGTERM, SIG_IGN); // ignore SIGTERM to avoid double handling
                kill(0, SIGTERM); // send SIGTERM to all processes in the same process group
                while(waitpid(-1, NULL, 0) > 0); // wait for all child processes to terminate
                break; // exit the loop on "exit" command
            } else {
                printf("Unknown command: %s\n", buffer);
            }
        }

        if(FD_ISSET(s, &readfds)) {
            // there is a new incoming connection on the listening socket
            // accept the connection and handle it in a new process

            // reset clientAddressLen before each accept call
            clientAddressLen = sizeof(clientAddress);

            clientSocket = accept(s, (struct sockaddr*)&clientAddress, &clientAddressLen);

            if(clientSocket < 0) {
                if(errno == EINTR) continue; // if interrupted by signal, retry
                perror("accept"); continue;
            }

            printf("[INFO]: New connection accepted from %s:%d\n", inet_ntoa(clientAddress.sin_addr), ntohs(clientAddress.sin_port));

            // fork a new process to handle the client
            fflush(NULL);
            pid_t pid = fork();
            if(pid < 0) {
                perror("fork");
                close(clientSocket);
                continue;
            } else if(pid == 0) { // child process
                close(s); // child does not need the listening socket

                // as soon as the fork is executed, drop privileges to nobody/nogroup 
                if(drop_privileges()<0){
                    perror("drop_privileges");
                    close(clientSocket);
                    _exit(EXIT_FAILURE);
                }

                // handle the client session and the exit
                handle_session(clientSocket);

                close(clientSocket); // close the connected socket in the child

                _exit(0); // after client session is handled, exit the child process
            } else { // parent process
                close(clientSocket); // parent does not need the connected socket
            }
        }

        
    }

    cleanup_ipc();

    close(s);

    return 0;
}