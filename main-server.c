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

    int r = sem_lock(sem_id);
    if(r<0){
        fprintf(stderr, "Failed to lock semaphore for session cleanup: %s\n", strerror(-r));
        sessions_cleanup = 1; // set the flag to indicate that we need to clean up sessions
        return;
    }

    session_table_t *table = &shared_memory->session_table;
    for(int i = 0; i < table->count; i++) {
        if(!table->entries[i].in_use) {
            continue; // skip entries currenly being used
        }
        if(kill(table->entries[i].pid, 0) == -1 && errno == ESRCH) {
            // process does not exist, mark the session as not in use
            table->entries[i].in_use = 0;
            memset(&table->entries[i], 0, sizeof(session_entry_t)); // clear the entry
            table->entries[i].pid = -1; 
        }
    }

    r = sem_unlock(sem_id);
    if(r < 0){
        fprintf(stderr, "Failed to unlock semaphore for session cleanup: %s\n", strerror(-r));
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
    memset(&server_address, 0, sizeof(server_address));


    server_address.sin_family = AF_INET;
    int port = atoi(port_number); // convert port number from string to integer
    if(port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port number: %s\n", port_number);
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    server_address.sin_port = htons(port);
    if(inet_aton(ip_address, &server_address.sin_addr) == 0) {
        fprintf(stderr, "Invalid IP address: %s\n", ip_address);
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // Bind the socket to the address
    if(bind(sockfd, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) {
        perror("bind");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    printf("[SETUP]: Socket bound to %s:%d\n", ip_address, port);

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


void dispatch(session_t *session, int clientSocket, uint8_t command, void *payload, uint32_t payload_len) {
    (void)payload_len; // suppress warning
    
    char error_msg[256];
    error_msg[0] = '\0'; // initialize error message to empty string
    uint32_t err_size = sizeof(error_msg);

    // Commands without login
    if(command == CMD_LOGIN) {
        
        // parse payload to extract username
        cmd_t login_command;
        login_command.code = CMD_LOGIN;
        if(parse_command(payload, &login_command, error_msg, err_size) < 0) {
            send_err(clientSocket, EINVAL, error_msg);
            return;
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

            // fill sessions table with the new session information
            session_entry_t entry = {0};
            entry.pid = getpid();
            entry.in_use = 1;
            strncpy(entry.username, session->user, sizeof(entry.username) - 1);
            entry.username[sizeof(entry.username) - 1] = '\0';
            
            int r = sem_lock(sem_id);
            if(r < 0) {
                send_err(clientSocket, -r, "Failed to lock semaphore");
                return;
            }
            // look for first entry having in_use == 0, if found use it, else use the next available entry
            int found = 0;
            for(int i = 0; i < shared_memory->session_table.count; i++) {
                if(shared_memory->session_table.entries[i].in_use == 0) {
                    shared_memory->session_table.entries[i] = entry;
                    found = 1;
                    break;
                }
            }
            if(!found) {
                if(shared_memory->session_table.count >= MAX_ENTRIES) {
                    r = sem_unlock(sem_id);
                    if(r < 0) {
                        send_err(clientSocket, -r, "Failed to unlock semaphore");
                    }
                    send_err(clientSocket, ENOMEM, "Session table is full");
                    return;
                }
                shared_memory->session_table.entries[shared_memory->session_table.count++] = entry;
            }
            r = sem_unlock(sem_id);
            if(r < 0) {
                send_err(clientSocket, -r, "Failed to unlock semaphore");
                return;
            }

            // look for pending requests for this user and notify them
            r = sem_lock(sem_id);
            if(r < 0) {
                send_err(clientSocket, -r, "Failed to lock semaphore");
                return;
            }

            for(int i = 0; i < shared_memory->pending_requests_table.count; i++) {
                transfer_request_entry_t *request = &shared_memory->pending_requests_table.entries[i];
                if(strcmp(request->dest_username, session->user) == 0 && request->status == PENDING) {
                    // notify the user about the pending request through the FIFO
                    char fifo_path[PATH_MAX + 64];
                    snprintf(fifo_path, sizeof(fifo_path), "%s/.sessions/fifo_%d", session->root_path, getpid());
                    int fifo_fd = open(fifo_path, O_WRONLY | O_NONBLOCK);
                    if(fifo_fd < 0) {
                        fprintf(stderr, "Failed to open FIFO for user %s: %s\n", session->user, strerror(errno));
                        continue;
                    }
                    
                    // extract the basename of the source file
                    char src_path_copy[PATH_MAX];
                    strncpy(src_path_copy, request->file_path_absolute, sizeof(src_path_copy) - 1);
                    src_path_copy[sizeof(src_path_copy) - 1] = '\0';
                    char *base_name = basename(src_path_copy);

                    // notify through FIFO the dest user about the pending request, sending the request ID and the basename of the file
                    char request_msg[PATH_MAX + 64];
                    snprintf(request_msg, sizeof(request_msg), "REQ|%d|%s\n", request->id, base_name);
                    ssize_t bytes_written = write(fifo_fd, request_msg, strlen(request_msg));

                    if(bytes_written < 0) {
                        fprintf(stderr, "Failed to write to FIFO for user %s: %s\n", session->user, strerror(errno));
                    }

                    close(fifo_fd);
                }
            }

            r = sem_unlock(sem_id);
            if(r < 0) {
                send_err(clientSocket, -r, "Failed to unlock semaphore");
                return;
            }

        }


        #if DEBUG
        printf("[DEBUG]: User %s logged in with UID %d\n", session->user, session->uid);
        #endif

        return;
    }
    if(command == CMD_CREATE_USER) {
    
        cmd_t cu_command;
        if(parse_command(payload, &cu_command, error_msg, err_size) < 0) {
            send_err(clientSocket, EINVAL, error_msg);
            return;
        }

        if(cu_command.argc < 2) {
            const char *reason = "Insufficient arguments for create_user";
            send_err(clientSocket, EINVAL, reason);
            return;
        }

        long perms = 0;
        int r = validate_permissions(&perms, cu_command.buf2);
        if(r < 0) {
            const char *reason = "Invalid permissions format";
            send_err(clientSocket, EINVAL, reason);
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

            int request_failure = 0;

            cmd_t accept_command;
            if(parse_command(payload, &accept_command, error_msg, err_size) < 0) {
                send_err(clientSocket, EINVAL, error_msg);
                return;
            }

            // accept <directory> <request_id>
            int request_id = atoi(accept_command.buf2);
            if(request_id <= 0) {
                const char *reason = "Invalid request ID";
                send_err(clientSocket, EINVAL, reason);
                return;
            }

            // check if we're the destination user for this request, if not send error (otherwise, the user could accept requests not meant for them)
            int r = sem_lock(sem_id);
            if(r < 0) {
                send_err(clientSocket, -r, "Failed to lock semaphore");
                return;
            }
            
            int found = 0;
            // copy locally the entry of shmem
            transfer_request_entry_t request_copy;
            int request_index = -1;
            memset(&request_copy, 0, sizeof(request_copy));
            for(int i = 0; i< shared_memory->pending_requests_table.count; i++){
                // found request, copy it and mark it as found
                if(shared_memory->pending_requests_table.entries[i].id == request_id) {
                    if(strcmp(shared_memory->pending_requests_table.entries[i].dest_username, session->user) != 0) {
                        r = sem_unlock(sem_id);
                        if(r < 0) {
                            send_err(clientSocket, -r, "Failed to unlock semaphore");
                        }
                        const char *reason = "You are not the destination user for this request";
                        send_err(clientSocket, EACCES, reason);
                        return;
                    }
                    request_copy = shared_memory->pending_requests_table.entries[i];
                    request_index = i;
                    found = 1;
                    break;
                }
            }

            r = sem_unlock(sem_id);
            if(r < 0) {
                send_err(clientSocket, -r, "Failed to unlock semaphore");
                return;
            }
            if(!found) {
                const char *reason = "Request ID not found";
                send_err(clientSocket, ENOENT, reason);
                return;
            }

            // validate the directory path against the user's home directory
            char full_dest_path[PATH_MAX];
            r = validate_path(accept_command.buf, session->home_path, full_dest_path);
            if(r < 0) {
                send_err(clientSocket, -r, path_error_reason(-r));
                return;
            }


            // update the request status to ACCEPTED
            r = sem_lock(sem_id);
            if(r < 0) {
                send_err(clientSocket, -r, "Failed to lock semaphore");
                return;
            }
            //  check if req is still valid, looking if it has not been accepted or rejected by another process in the meantime
            if(request_index < 0 ||
               shared_memory->pending_requests_table.entries[request_index].id != request_copy.id ||
               shared_memory->pending_requests_table.entries[request_index].status != PENDING) {
                sem_unlock(sem_id);
                send_err(clientSocket, ENOENT, "Request no longer available");
                return;
            }
            shared_memory->pending_requests_table.entries[request_index].status = ACCEPTED;

            r = sem_unlock(sem_id);
            if(r < 0) {
                send_err(clientSocket, -r, "Failed to unlock semaphore");
                return;
            }

            // notify the source user process about the acceptance of the request through the FIFO
            // look up for the source user in session table to get the PID
            int source_user_found = 0;
            int source_user_online = 0;
            int source_user_pid = -1;
            r = sem_lock(sem_id);
            if(r < 0) {
                send_err(clientSocket, -r, "Failed to lock semaphore");
                return;
            }
            for(int i = 0; i < shared_memory->session_table.count; i++) {
                if(strcmp(shared_memory->session_table.entries[i].username, request_copy.source_username) == 0) { 
                    source_user_found = 1;
                    source_user_pid = shared_memory->session_table.entries[i].pid;
                    if(shared_memory->session_table.entries[i].in_use == 1) {
                        source_user_online = 1;
                    }
                break;
                }
            }

            r = sem_unlock(sem_id);
            if(r < 0) {
                send_err(clientSocket, -r, "Failed to unlock semaphore");
                return;
            }

            if(!source_user_found) {
                const char *reason = "Source user not found";
                send_err(clientSocket, ENOENT, reason);
                return;
            }

            // extract the basename of the source file
            char src_path_copy[PATH_MAX];
            strncpy(src_path_copy, request_copy.file_path_absolute, sizeof(src_path_copy) - 1);
            src_path_copy[sizeof(src_path_copy) - 1] = '\0';
            char *base_name = basename(src_path_copy);

            // construct full dest path adding the basename of the source file to the destination directory
            char full_dest_file_path[PATH_MAX + NAME_MAX + 2]; // +2 for '/' and '\0', NAME_MAX is the maximum length of a filename component
            snprintf(full_dest_file_path, sizeof(full_dest_file_path), "%s/%.*s", full_dest_path, NAME_MAX, base_name);

            // copy the file from source to destination
            int copy_result = execute_transfer_copy(request_copy.file_path_absolute, full_dest_file_path);
            if(copy_result < 0) {
                request_failure = 1;
            }
            
            // if request has failed, update the request status to FAILED
            if(request_failure) {
                r = sem_lock(sem_id);
                if(r < 0) {
                    send_err(clientSocket, -r, "Failed to lock semaphore");
                    return;
                }
                if(request_index >= 0 &&
                   shared_memory->pending_requests_table.entries[request_index].id == request_copy.id) {
                    shared_memory->pending_requests_table.entries[request_index].status = FAILED;
                }
                r = sem_unlock(sem_id);
                if(r < 0) {
                    send_err(clientSocket, -r, "Failed to unlock semaphore");
                    return;
                }
            }

            // if source user is online, notify them about the acceptance or failure of the request through the FIFO
            if(source_user_online){
                // construct FIFO path based on the source_user PID
                char fifo_path[PATH_MAX + 64];
                snprintf(fifo_path, sizeof(fifo_path), "%s/.sessions/fifo_%d", session->root_path, source_user_pid);

                int fifo_fd = open(fifo_path, O_WRONLY | O_NONBLOCK);
                if(fifo_fd < 0) {
                    const char *reason = "Failed to open FIFO for source user";
                    send_err(clientSocket, errno, reason);
                    return;
                }

                // depending on the result of the copy operation, send either ACCEPT or FAIL message to the source user process through the FIFO
                char request_status_str[PATH_MAX + 64];
                if(request_failure) {
                    snprintf(request_status_str, sizeof(request_status_str), "FAIL|%d|%d\n", request_copy.id, -copy_result);
                } else {
                    snprintf(request_status_str, sizeof(request_status_str), "ACCEPT|%d|%s\n", request_copy.id, request_copy.file_path_absolute);
                }

                ssize_t bytes_written = write(fifo_fd, request_status_str, strlen(request_status_str));
                if(bytes_written < 0) {
                    const char *reason = "Failed to write to FIFO for source user";
                    send_err(clientSocket, errno, reason);
                    close(fifo_fd);
                    return;
                }

                close(fifo_fd);
            }

            // send error message only after unlocking the destination user through the FIFO
            if(request_failure){
                const char *reason = "File transfer failed";
                send_err(clientSocket, -copy_result, reason);
                return;
            }

            char msg[128 + PATH_MAX];
            snprintf(msg, sizeof(msg), "Transfer request accepted and file copied to %s", base_name);
            send_ok(clientSocket, msg, strlen(msg));


            break;
        }
        case(CMD_REJECT):{
            cmd_t reject_command;
            if(parse_command(payload, &reject_command, error_msg, err_size) < 0) {
                send_err(clientSocket, EINVAL, error_msg);
                return;
            }

            // reject <request_id>

            int request_id = atoi(reject_command.buf);
            if(request_id <= 0) {
                const char *reason = "Invalid request ID";
                send_err(clientSocket, EINVAL, reason);
                return;
            }

            // check if we're the destination user for this request, if not send error (otherwise, the user could reject requests not meant for them)
            int r = sem_lock(sem_id);
            if(r < 0) {
                send_err(clientSocket, -r, "Failed to lock semaphore");
                return;
            }
            int found = 0;
            // copy locally the entry of the request to avoid holding the semaphore while notifying the source user
            transfer_request_entry_t request_copy;
            int request_index = -1;
            memset(&request_copy, 0, sizeof(request_copy));

            for(int i = 0; i< shared_memory->pending_requests_table.count; i++){
                // if it's the request we're looking for, copy it and mark it as found
                if(shared_memory->pending_requests_table.entries[i].id == request_id) {
                    if(strcmp(shared_memory->pending_requests_table.entries[i].dest_username, session->user) != 0) {
                        r = sem_unlock(sem_id);
                        if(r < 0) {
                            send_err(clientSocket, -r, "Failed to unlock semaphore");
                        }
                        const char *reason = "You are not the destination user for this request";
                        send_err(clientSocket, EACCES, reason);
                        return;
                    }
                    request_copy = shared_memory->pending_requests_table.entries[i];
                    request_index = i;
                    found = 1;
                    break;
                }
            }

            r = sem_unlock(sem_id);
            if(r < 0) {
                send_err(clientSocket, -r, "Failed to unlock semaphore");
                return;
            } 

            if(!found) {
                const char *reason = "Request ID not found";
                send_err(clientSocket, ENOENT, reason);
                return;
            }

            // update the request status to REJECTED
            r = sem_lock(sem_id);
            if(r < 0) {
                send_err(clientSocket, -r, "Failed to lock semaphore");
                return;
            }

            // check if the request is still valid, for example if it has not been accepted or rejected by another process in the meantime
            if(request_index < 0 ||
               shared_memory->pending_requests_table.entries[request_index].id != request_copy.id ||
               shared_memory->pending_requests_table.entries[request_index].status != PENDING) {
                sem_unlock(sem_id);
                send_err(clientSocket, ENOENT, "Request no longer available");
                return;
            }
            shared_memory->pending_requests_table.entries[request_index].status = REJECTED;

            r = sem_unlock(sem_id);
            if(r < 0) {
                send_err(clientSocket, -r, "Failed to unlock semaphore");
                return;
            }

            // notify the source user process about the rejection of the request through the FIFO
            // look up for the source user in session table to get the PID
            int source_user_found = 0;
            int source_user_online = 0;
            int source_user_pid = -1;
            r = sem_lock(sem_id);
            if(r < 0) {
                send_err(clientSocket, -r, "Failed to lock semaphore");
                return;
            }
            for(int i = 0; i < shared_memory->session_table.count; i++) {
                if(strcmp(shared_memory->session_table.entries[i].username, request_copy.source_username) == 0) {
                    source_user_found = 1;
                    source_user_pid = shared_memory->session_table.entries[i].pid;
                    if(shared_memory->session_table.entries[i].in_use == 1) {
                        source_user_online = 1;
                    }

                    break;
                }
            }
            r = sem_unlock(sem_id);
            if(r < 0) {
                send_err(clientSocket, -r, "Failed to unlock semaphore");
                return;
            }

            if(!source_user_found) {
                const char *reason = "Source user not found";
                send_err(clientSocket, ENOENT, reason);
                return;
            }

            if(source_user_online) {
                // construct FIFO path based on the source_user PID
                char fifo_path[PATH_MAX + 64];
                snprintf(fifo_path, sizeof(fifo_path), "%s/.sessions/fifo_%d", session->root_path, source_user_pid);
                // open the FIFO for writing
                int fifo_fd = open(fifo_path, O_WRONLY | O_NONBLOCK);
                if(fifo_fd < 0) {
                    const char *reason = "Failed to open FIFO for source user";
                    send_err(clientSocket, errno, reason);
                    return;
                }

                // send the transfer request ID and the file path to the source user process through the FIFO
                char request_id_str[PATH_MAX + 64];
                snprintf(request_id_str, sizeof(request_id_str), "REJECT|%d\n", request_copy.id);
                ssize_t bytes_written = write(fifo_fd, request_id_str, strlen(request_id_str));
                if(bytes_written < 0) {
                    const char *reason = "Failed to write to FIFO for source user";
                    send_err(clientSocket, errno, reason);
                    close(fifo_fd);
                    return;
                }
                close(fifo_fd);
            }

            // notify the client
            char *msg = "Transfer request rejected successfully";
            send_ok(clientSocket, msg, strlen(msg));

            break;
        }
        case(CMD_TRANSFER_REQ):{
            cmd_t transfer_command;
            if(parse_command(payload, &transfer_command, error_msg, err_size) < 0) {
                send_err(clientSocket, EINVAL, error_msg);
                return;
            }

            // transfer_request <file> <dest_user>
            // validate source path against home_path, if not valid return error
            char full_src_path[PATH_MAX];
            int r = validate_path(transfer_command.buf, session->home_path, full_src_path);
            if(r < 0) {
                send_err(clientSocket, -r, path_error_reason(-r));
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

            // look for the user in the sessions table
            int dest_user_online = 0;
            int dest_user_pid = -1;
            int r2 = sem_lock(sem_id);
            if(r2 < 0) {
                send_err(clientSocket, -r2, "Failed to lock semaphore");
                return;
            }
            for(int i = 0; i < shared_memory->session_table.count; i++) {
                if(shared_memory->session_table.entries[i].in_use == 1 && strcmp(shared_memory->session_table.entries[i].username, transfer_command.buf2) == 0) {
                    dest_user_online = 1;
                    dest_user_pid = shared_memory->session_table.entries[i].pid;
                    break;
                }
            }
            r2 = sem_unlock(sem_id);
            if(r2 < 0) {
                send_err(clientSocket, -r2, "Failed to unlock semaphore");
                return;
            }

            if(dest_user_online == 0){
                // sender remains blocked until dest. user logs in 
                struct passwd *pwd = getpwnam(transfer_command.buf2);
                char dest_user_home[PATH_MAX + NAME_MAX + 2]; // +2 for '/' and '\0'
                struct stat dest_home_stat;
                snprintf(dest_user_home, sizeof(dest_user_home), "%s/%.32s", session->root_path, transfer_command.buf2);

                if(pwd == NULL || !is_csap_user(pwd) || stat(dest_user_home, &dest_home_stat) < 0 || !S_ISDIR(dest_home_stat.st_mode)) {
                    const char *reason = "Destination user does not exist";
                    send_err(clientSocket, ENOENT, reason);
                    return;

                }
            }
            // insert the transfer request into the pending requests table in shared memory, looking for the first one available (either ACCEPTED or REJECTED) or if none is found, use the next available entry

            transfer_request_entry_t new_request = {0};
            strncpy(new_request.source_username, session->user, sizeof(new_request.source_username) - 1);
            new_request.source_username[sizeof(new_request.source_username) - 1] = '\0';
            strncpy(new_request.dest_username, transfer_command.buf2, sizeof(new_request.dest_username) - 1);
            new_request.dest_username[sizeof(new_request.dest_username) - 1] = '\0';
            strncpy(new_request.file_path_absolute, full_src_path, sizeof(new_request.file_path_absolute) - 1);
            new_request.file_path_absolute[sizeof(new_request.file_path_absolute) - 1] = '\0';
            new_request.status = PENDING;

            r2 = sem_lock(sem_id);
            if(r2 < 0) {
                send_err(clientSocket, -r2, "Failed to lock semaphore");
                return;
            }
            int found = 0;
            int index_to_insert = -1;
            for(int i = 0; i < shared_memory->pending_requests_table.count; i++) {
                if(shared_memory->pending_requests_table.entries[i].status != PENDING) {
                    shared_memory->pending_requests_table.entries[i] = new_request;
                    index_to_insert = i;
                    found = 1;
                    break;
                }
            }
            if(!found) {
                if(shared_memory->pending_requests_table.count >= MAX_ENTRIES) {
                    r2 = sem_unlock(sem_id);
                    if(r2 < 0) {
                        send_err(clientSocket, -r2, "Failed to unlock semaphore");
                    }
                    const char *reason = "Pending requests table is full";
                    send_err(clientSocket, ENOMEM, reason);
                    return;
                }

                index_to_insert = shared_memory->pending_requests_table.count;
                shared_memory->pending_requests_table.entries[shared_memory->pending_requests_table.count++] = new_request;
            }

            int assigned_id = shared_memory->pending_requests_table.next_id++;
            shared_memory->pending_requests_table.entries[index_to_insert].id = assigned_id;
            
            r2 = sem_unlock(sem_id);
            if(r2 < 0) {
                send_err(clientSocket, -r2, "Failed to unlock semaphore");
                return;
            }

            // notify dest user through FIFO if they're online
            // if offline, the request will be pending until they log in and the server will notify them of the pending request
            if(dest_user_online) {

                // construct FIFO path based on the dest_user PID
                char fifo_path[PATH_MAX + 64];
                snprintf(fifo_path, sizeof(fifo_path), "%s/.sessions/fifo_%d", session->root_path, dest_user_pid);
                // open the FIFO for writing
                int fifo_fd = open(fifo_path, O_WRONLY | O_NONBLOCK);
                if(fifo_fd < 0) {
                    const char *reason = "Failed to open FIFO for destination user";
                    send_err(clientSocket, errno, reason);
                    return;
                }

                // send the transfer request ID and the file path to the destination user process through the FIFO

                // first obtain the basename of the source file path using a copy of the string to avoid modifying the original
                char src_path_copy[PATH_MAX];
                strncpy(src_path_copy, full_src_path, sizeof(src_path_copy) - 1);
                src_path_copy[sizeof(src_path_copy) - 1] = '\0';
                char *base_name = basename(src_path_copy);
                if(base_name == NULL) {
                    const char *reason = "Failed to extract basename from source file path";
                    send_err(clientSocket, EINVAL, reason);
                    close(fifo_fd);
                    return;
                }

                char request_id_str[PATH_MAX + 64];
                // send the ID and the basename of the file to the destination user process through the FIFO
                snprintf(request_id_str, sizeof(request_id_str), "REQ|%d|%s\n", assigned_id, base_name);
                ssize_t bytes_written = write(fifo_fd, request_id_str, strlen(request_id_str));
                if(bytes_written < 0) {
                    const char *reason = "Failed to write to FIFO for destination user";
                    send_err(clientSocket, errno, reason);
                    close(fifo_fd);
                    return;
                }
                close(fifo_fd);
            }

            break;
        }
        case(CMD_CREATE): {
            cmd_t create_command;
            if(parse_command(payload, &create_command, error_msg, err_size) < 0) {
                send_err(clientSocket, EINVAL, error_msg);
                return;
            }

            // validate path against home_path, if not valid return error
            char full_path[PATH_MAX];
            int r = validate_path(create_command.buf, session->home_path, full_path);
            if(r < 0) {
                send_err(clientSocket, -r, path_error_reason(-r));
                return;
            }

            long perms = 0;
            r = validate_permissions(&perms, create_command.buf2);
            if(r < 0) {
                const char *reason = "Invalid permissions format";
                send_err(clientSocket, EINVAL, reason);
                return;
            }

            int create_result = create_cmd(full_path, perms, create_command.is_dir);
            if(create_result < 0) {
                const char *reason = "Failed to create file/directory";
                send_err(clientSocket, -create_result, reason);
                return;
            }

            const char *reason = "File/Directory created successfully";
            send_ok(clientSocket, reason, strlen(reason));

            break;
        }
        case(CMD_CHMOD): {

            cmd_t chmod_command;
            if(parse_command(payload, &chmod_command, error_msg, err_size) < 0) {
                send_err(clientSocket, EINVAL, error_msg);
                return;
            }

            // validate path against home_path, if not valid return error
            char full_path[PATH_MAX];
            int r = validate_path(chmod_command.buf, session->home_path, full_path);
            if(r < 0) {
                send_err(clientSocket, -r, path_error_reason(-r));
                return;
            }

            long perms = 0;
            r = validate_permissions(&perms, chmod_command.buf2);
            if(r < 0) {
                const char *reason = "Invalid permissions format";
                send_err(clientSocket, EINVAL, reason);
                return;
            }

            int chmod_result = chmod_cmd(full_path, perms);
            if(chmod_result < 0) {
                const char *reason = "Failed to change permissions";
                send_err(clientSocket, -chmod_result, reason);
                return;
            }

            const char *reason = "Permissions changed successfully";
            send_ok(clientSocket, reason, strlen(reason));

            break;
        }
        case(CMD_MOVE): {
            cmd_t move_command;
            if(parse_command(payload, &move_command, error_msg, err_size) < 0) {
                send_err(clientSocket, EINVAL, error_msg);
                return;
            }

            // validate source path against home_path, if not valid return error
            char full_src_path[PATH_MAX];
            int r = validate_path(move_command.buf, session->home_path, full_src_path);
            if(r < 0) {
                send_err(clientSocket, -r, path_error_reason(-r));
                return;
            }

            // validate destination path against home_path, if not valid return error
            char full_dest_path[PATH_MAX];
            r = validate_path(move_command.buf2, session->home_path, full_dest_path);
            if(r < 0) {
                send_err(clientSocket, -r, path_error_reason(-r));
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

                if(S_ISDIR(dest_stat.st_mode)){

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

            r = acquire_write_lock(dest_fd, 0, 0);
            if(r<0) {
                const char *reason = "Failed to acquire write lock on destination file";
                send_err(clientSocket, -r, reason);
                if(src_fd >= 0) {
                    release_lock(src_fd, 0, 0);
                    close(src_fd);
                }
                if(dest_fd >= 0) {
                    release_lock(dest_fd, 0, 0);
                    close(dest_fd);
                }
                return;
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
            send_ok(clientSocket, reason, strlen(reason));

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
            if(parse_command(payload, &cd_command, error_msg, err_size) < 0) {
                send_err(clientSocket, EINVAL, error_msg);
                return;
            }

            // validate path against home_path, if not valid return error
            char full_path[PATH_MAX];
            int r = validate_path(cd_command.buf, session->home_path, full_path);
            if(r < 0) {
                send_err(clientSocket, -r, path_error_reason(-r));
                return;
            }

            int cd_result = cd_cmd(full_path);
            if(cd_result < 0) {
                const char *reason = "Failed to change directory";
                send_err(clientSocket, -cd_result, reason);
                return;
            }

            const char *reason = "Directory changed successfully";
            send_ok(clientSocket, reason, strlen(reason));

            break;
        }
        case(CMD_DOWNLOAD_BEGIN): {
            cmd_t download_command;
            if(parse_command(payload, &download_command, error_msg, err_size) < 0) {
                send_err(clientSocket, EINVAL, error_msg);
                return;
            }

            // validate path against home_path, if not valid return error
            char full_path[PATH_MAX];
            int r = validate_path(download_command.buf, session->home_path, full_path);
            if(r < 0) {
                send_err(clientSocket, -r, path_error_reason(-r));
                return;
            }

            // open file contained in validated path
            int file_fd = open(full_path, O_RDONLY);
            if(file_fd < 0) {
                const char *reason = "Failed to open file";
                send_err(clientSocket, errno, reason);
                return;
            }

            // acquire read lock, to prevent other processes from writing to the file while it is being downloaded
            int rlk = acquire_read_lock(file_fd, 0, 0);
            if(rlk < 0) {
                const char *reason = "Failed to acquire read lock";
                send_err(clientSocket, -rlk, reason);
                close(file_fd);
                return;
            }

            // obtain info about file
            struct stat file_stat;
            if(fstat(file_fd, &file_stat) < 0) {
                const char *reason = "Failed to get file info";
                send_err(clientSocket, errno, reason);
                release_lock(file_fd, 0, 0);
                close(file_fd);
                return;
            }

            // check if a regular file, if not return error
            if(!S_ISREG(file_stat.st_mode)) {
                send_err(clientSocket, EISDIR, "Not a regular file");
                release_lock(file_fd, 0, 0);
                close(file_fd);
                return;
            }

            // compute file size
            uint64_t file_size = (uint64_t)file_stat.st_size;

            // send an initial OK to client to indicate that the download operation is starting, with the size of the file as payload
            char size_payload[32];
            int size_payload_len = snprintf(size_payload, sizeof(size_payload), "%llu", (unsigned long long)file_size);
            send_ok(clientSocket, size_payload, size_payload_len);

            uint8_t data_code = CMD_DATA;
            uint8_t end_code = CMD_DATA_END;

            // send the file stream to the client
            ssize_t stream_result = send_stream(clientSocket, file_fd, 0, data_code, end_code);

            // close descriptor
            release_lock(file_fd, 0, 0);
            close(file_fd);

            if(stream_result < 0) {
                const char *reason = "Failed to send file stream";
                send_err(clientSocket, (int)-stream_result, reason);
                return;
            }
            break;
        }
        case(CMD_READ): {
            cmd_t read_command;
            if(parse_command(payload, &read_command, error_msg, err_size) < 0) {
                send_err(clientSocket, EINVAL, error_msg);
                return;
            }

            if(read_command.offset < 0){
                read_command.offset = 0; // default offset to 0
            }

            // validate path against home_path, if not valid return error
            char full_path[PATH_MAX];
            int r = validate_path(read_command.buf, session->home_path, full_path);
            if(r < 0) {
                send_err(clientSocket, -r, path_error_reason(-r));
                return;
            }

            // open file contained in validated path
            int file_fd = open(full_path, O_RDONLY);
            if(file_fd < 0) {
                const char *reason = "Failed to open file";
                send_err(clientSocket, errno, reason);
                return;
            }

            // acquire read lock, to avoid other processes writing to the file while it is being read
            int rlk = acquire_read_lock(file_fd, read_command.offset, 0);
            if(rlk < 0) {
                const char *reason = "Failed to acquire read lock";
                send_err(clientSocket, -rlk, reason);
                close(file_fd);
                return;
            }

            // obtain info about file
            struct stat file_stat;
            if(fstat(file_fd, &file_stat) < 0) {
                const char *reason = "Failed to get file info";
                send_err(clientSocket, errno, reason);
                release_lock(file_fd, read_command.offset, 0);
                close(file_fd);
                return;
            }

            // check if file is a regular file, if not return error
            if(!S_ISREG(file_stat.st_mode)) {
                send_err(clientSocket, EISDIR, "Not a regular file");
                release_lock(file_fd, read_command.offset, 0);
                close(file_fd);
                return;
            }

            // compute file_size
            uint64_t file_size = (uint64_t)file_stat.st_size;

            // obtain offset from read_command
            off_t offset = (off_t)read_command.offset;
            // determine amount of bytes to send, size - offset, if offset is greater than size, send 0 bytes
            uint64_t bytes_to_send = 0;
            if(file_size > (uint64_t)offset) {
                bytes_to_send = file_size - (uint64_t)offset;
            }

            // send OK to client, indicating that the read operation is starting, with the size of the data to be sent as payload
            // size is computed removing the offset from the total size of the file, so client can detect truncated answer
            char size_payload[32];
            int size_payload_len = snprintf(size_payload, sizeof(size_payload), "%llu", (unsigned long long)bytes_to_send);
            send_ok(clientSocket, size_payload, size_payload_len);

            uint8_t data_code = CMD_READ_DATA;
            uint8_t end_code = CMD_READ_END;

            // send the file stream to the client
            ssize_t stream_result = send_stream(clientSocket, file_fd, offset, data_code, end_code);

            // release lock
            release_lock(file_fd, read_command.offset, 0);

            // close descriptor
            close(file_fd);

            if(stream_result < 0) {
                const char *reason = "Failed to send file stream";
                send_err(clientSocket, (int)-stream_result, reason);
                return;
            }
            
            break;
        }
        case(CMD_UPLOAD_BEGIN): {
            cmd_t upload_command;
            if(parse_command(payload, &upload_command, error_msg, err_size) < 0) {
                send_err(clientSocket, EINVAL, error_msg);
                return;
            }

            // validate path against home_path, if not valid return error
            char full_path[PATH_MAX];
            int r = validate_path(upload_command.buf2, session->home_path, full_path);
            if(r < 0) {
                send_err(clientSocket, -r, path_error_reason(-r));
                return;
            }

            
            mode_t dest_mode = 0700; // default permissions for file
            struct stat dest_stat;
            if(stat(full_path, &dest_stat) == 0) {
                dest_mode = dest_stat.st_mode & 07777; // retrieve permissions of existing file
            } // I obtain permissions as mkstemp() creates temp file with 0600 permissions, and after succesful upload I restore the permissions of dest. file

            // write in a temp file until successful upload, then rename it, avoiding problems due to crashes
            char temp_path[PATH_MAX];
            temp_path[0] = '\0';
            int file_fd = open_temp_for_upload(full_path, temp_path, sizeof(temp_path));
            if(file_fd < 0) {
                const char *reason = "Failed to open temporary file for upload";
                send_err(clientSocket, -file_fd, reason);
                return;
            }

            // check if destination file exists
            int existed = (stat(full_path, &dest_stat) == 0);

            // open the destination with O_WRONLY | O_CREAT | O_NOFOLLOW to avoid following symlinks, and with 0700 permissions
            int lock_fd = open(full_path, O_WRONLY | O_CREAT | O_NOFOLLOW, 0700);
            if(lock_fd < 0) {
                const char *reason = "Failed to open destination file for locking";
                send_err(clientSocket, errno, reason);
                close(file_fd);
                unlink(temp_path); // remove temporary file
                return;
            }

            int rl = acquire_write_lock(lock_fd, 0, 0);
            if(rl < 0) {
                const char *reason = "Failed to acquire write lock on destination file";
                send_err(clientSocket, -rl, reason);
                close(file_fd);
                close(lock_fd);
                unlink(temp_path); // remove temporary file
                if(!existed) unlink(full_path); // remove destination file if it did not exist before
                return;
            }


            // tell client to start sending data
            const char *reason = "Ready to receive data";
            send_ok(clientSocket, reason, strlen(reason));

            // start receiving stream
            uint8_t data_code = CMD_DATA;
            uint8_t end_code = CMD_DATA_END;

            // offset -1: never seek, the temporary file is empty
            ssize_t stream_result = recv_stream(clientSocket, file_fd, -1, -1, data_code, end_code, error_msg, err_size);
            if(stream_result < 0) {
                release_lock(lock_fd, 0, 0);
                close(lock_fd);
                unlink(temp_path); // remove temporary file if upload failed
                if(!existed) unlink(full_path);
                send_err(clientSocket, (int)-stream_result,
                         error_msg[0] ? error_msg : "Failed to receive file stream");
                return;
            }

            // restore the permissions of the destination on the temporary file
            if(fchmod(file_fd, dest_mode) < 0) {
                int saved = errno;
                release_lock(lock_fd, 0, 0);
                close(lock_fd);
                unlink(temp_path);
                if(!existed) unlink(full_path);
                send_err(clientSocket, saved, "Failed to set permissions on uploaded file");
                return;
            }

            if(close(file_fd) < 0) {
                int saved = errno;
                release_lock(lock_fd, 0, 0);
                close(lock_fd);
                unlink(temp_path);
                if(!existed) unlink(full_path);
                send_err(clientSocket, saved, "Failed to close temporary file");
                return;
            }

            // update final dest
            if(rename(temp_path, full_path) < 0) {
                int saved = errno;
                release_lock(lock_fd, 0, 0);
                close(lock_fd);
                unlink(temp_path);
                if(!existed) unlink(full_path);
                send_err(clientSocket, saved, "Failed to install uploaded file");
                return;
            }

            // unlock
            release_lock(lock_fd, 0, 0);
            close(lock_fd);

            // send confirmation with N bytes written
            char bytes_written_payload[32];
            int bytes_written_payload_len = snprintf(bytes_written_payload, sizeof(bytes_written_payload), "%lld", (long long)stream_result);
            send_ok(clientSocket, bytes_written_payload, bytes_written_payload_len);
            break;
        }
        case(CMD_WRITE): {
            cmd_t write_command;
            if(parse_command(payload, &write_command, error_msg, err_size) < 0) {
                send_err(clientSocket, EINVAL, error_msg);
                return;
            }

            // validate path against home_path, if not valid return error
            char full_path[PATH_MAX];
            int r = validate_path(write_command.buf, session->home_path, full_path);
            if(r < 0) {
                send_err(clientSocket, -r, path_error_reason(-r));
                return;
            }

            // open file contained in validated path
            off_t offset = (off_t)write_command.offset;

            // save permissions (like in upload) to restore them after writing, as the file may be truncated
            mode_t dest_mode = 0700; // default permissions
            struct stat dest_stat;
            if(stat(full_path, &dest_stat) == 0) {
                dest_mode = dest_stat.st_mode & 07777;
            }

            // with specified offset, open file in write mode with O_CREAT and 0700 permissions, otherwise open in truncate mode (O_TRUNC)
            char temp_path[PATH_MAX];
            temp_path[0] = '\0';
            int file_fd;
            if(offset >= 0) {
                file_fd = open(full_path, O_WRONLY | O_CREAT | O_NOFOLLOW, 0700);
                if(file_fd < 0) {
                    const char *reason = "Failed to open file for writing";
                    send_err(clientSocket, errno, reason);
                    return;
                }
            } else {
                // open with temporary file to avoid truncating the original file until the write is complete
                file_fd = open_temp_for_upload(full_path, temp_path, sizeof(temp_path));
                if(file_fd < 0) {
                    const char *reason = "Failed to open temporary file for writing";
                    send_err(clientSocket, -file_fd, reason);
                    return;
                }
            }

            // check if file existed
            int existed = (stat(full_path, &dest_stat) == 0);

            int lock_fd = -1;

            // I set the lock on the actual file on which we want to write, and to check which one to lock
            // I check trhough the temp_path, if it is empty, it means that we are writing to the actual file, 
            // otherwise we are writing to a temporary file and we need to lock the actual file
            if(temp_path[0] == '\0'){
                // there is an offset, so lock_fd = file_fd
                lock_fd = file_fd;
            } else {
                lock_fd = open(full_path, O_WRONLY | O_CREAT | O_NOFOLLOW, 0700);
                if(lock_fd < 0) {
                    const char *reason = "Failed to open file for locking";
                    send_err(clientSocket, errno, reason);
                    close(file_fd);
                    if(temp_path[0] != '\0') {
                        unlink(temp_path); // remove temp file
                    }                   
                    return;
                }
            }

            // take lock 
            int rl = acquire_write_lock(lock_fd, 0, 0);
            if(rl < 0) {
                const char *reason = "Failed to acquire write lock on file";
                send_err(clientSocket, -rl, reason);
                close(file_fd);
                if(lock_fd != file_fd) {
                    close(lock_fd);
                }
                if(temp_path[0] != '\0') {
                    unlink(temp_path); // remove temp file
                }
                if(!existed) unlink(full_path); // remove destination if it did not exist before 
                return;
            }


            // tell client to start sending data
            const char *reason = "Ready to receive data";
            send_ok(clientSocket, reason, strlen(reason));

            // start receiving stream
            uint8_t data_code = CMD_WRITE_DATA;
            uint8_t end_code = CMD_WRITE_END;

            ssize_t stream_result = recv_stream(clientSocket, file_fd, offset, -1, data_code, end_code, error_msg, err_size);
            if(stream_result < 0) {
                if(!existed) unlink(full_path);
                if(lock_fd >=0 && lock_fd != file_fd) {
                    release_lock(lock_fd, 0, 0);
                    close(lock_fd);
                    close(file_fd);
                } else if(lock_fd == file_fd) {
                    release_lock(lock_fd, 0, 0);
                    close(lock_fd);
                }
                if(temp_path[0] != '\0') {
                    unlink(temp_path); // remove temp file
                }
                send_err(clientSocket, (int)-stream_result,
                         error_msg[0] ? error_msg : "Failed to receive file stream");
                return;
            }

            if(temp_path[0] != '\0') { // if a temporary file was used
                // restore the permissions of the destination and commit to effective file
                if(fchmod(file_fd, dest_mode) < 0) {
                    int saved = errno;
                    release_lock(lock_fd, 0, 0);
                    close(lock_fd);
                    close(file_fd);
                    unlink(temp_path);
                    if(!existed) unlink(full_path);
                    send_err(clientSocket, saved, "Failed to set permissions on written file");
                    return;
                }
                if(close(file_fd) < 0) {
                    int saved = errno;
                    release_lock(lock_fd, 0, 0);
                    close(lock_fd);
                    unlink(temp_path);
                    if(!existed) unlink(full_path);
                    send_err(clientSocket, saved, "Failed to close temporary file");
                    return;
                }
                if(rename(temp_path, full_path) < 0) {
                    int saved = errno;
                    release_lock(lock_fd, 0, 0);
                    close(lock_fd);
                    unlink(temp_path);
                    if(!existed) unlink(full_path);
                    send_err(clientSocket, saved, "Failed to install written file");
                    return;
                }
            } else {    
                release_lock(lock_fd, 0, 0);
                close(lock_fd);
                close(file_fd);
                lock_fd = -1;
            }

            // if lock_fd > 0, a temp file was used
            if(lock_fd >= 0){
                release_lock(lock_fd, 0, 0);
                close(lock_fd);
            }

            // send confirmation with N B
            char bytes_written_payload[32];
            int bytes_written_payload_len = snprintf(bytes_written_payload, sizeof(bytes_written_payload), "%lld", (long long)stream_result);
            send_ok(clientSocket, bytes_written_payload, bytes_written_payload_len);

            break;
        }
        case(CMD_DELETE): {
            cmd_t delete_command;
            if(parse_command(payload, &delete_command, error_msg, err_size) < 0) {
                send_err(clientSocket, EINVAL, error_msg);
                return;
            }

            // validate path against home_path, if not valid return error
            char full_path[PATH_MAX];
            int r = validate_path(delete_command.buf, session->home_path, full_path);
            if(r < 0) {
                send_err(clientSocket, -r, path_error_reason(-r));
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
            send_ok(clientSocket, reason, strlen(reason));
            
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
            if(parse_command(payload, &list_command, error_msg, err_size) < 0) {
                send_err(clientSocket, EINVAL, error_msg);
                return;
            }

            // if path is NULL (from payload), pass NULL to list() to list current working directory
            // validate cwd against root_path, if not valid return error
            char full_path[PATH_MAX];
            int r;
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
                r = validate_path(list_command.buf, session->root_path, full_path);
                if(r < 0) {
                    send_err(clientSocket, -r, path_error_reason(-r));
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

    // cleanup any pending requests made by this user
    if(session.logged_in){
        int rr = sem_lock(sem_id);
        if(rr == 0){
            for(int i = 0; i < shared_memory->pending_requests_table.count; i++){
                transfer_request_entry_t *entry = &shared_memory->pending_requests_table.entries[i];
                if(entry->status == PENDING && strcmp(entry->source_username, session.user) == 0){
                    // mark as failed
                    entry->status = FAILED;
                }
            }
            sem_unlock(sem_id);
        }

    }

    // cleanup session resources
    if(session.notify_fd >= 0) {
        close(session.notify_fd);
        char fifo_name[PATH_MAX + 64];
        snprintf(fifo_name, sizeof(fifo_name), "%s/.sessions/fifo_%d", session.root_path, getpid());
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