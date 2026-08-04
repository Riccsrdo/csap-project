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
#include<sys/types.h>
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

#define MAX_CLIENT_BUFFER 1024

char root_directory[PATH_MAX]; // global variable to hold the root directory path

void handle_signals(int sig) {
    // wait for all dead processes (SIGCHLD) without blocking
    (void)sig; 
    int saved_errno = errno;
    while(waitpid(-1, NULL, WNOHANG) > 0){
    }
    errno = saved_errno;
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




void dispatch(session_t *session, int clientSocket, uint8_t command, void *payload, uint32_t payload_len) {
    
    char error_msg[256];
    error_msg[0] = '\0'; // initialize error message to empty string

    // Commands without login
    if(command == CMD_LOGIN) {
        
        // parse payload to extract username
        cmd_t login_command;
        login_command.code = CMD_LOGIN;
        if(parse_command(payload, &login_command) < 0) {
            const char *reason = "Failed to parse command";
            send_err(clientSocket, EINVAL, reason);
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
        }

        #if DEBUG
        printf("[DEBUG]: User %s logged in with UID %d\n", session->user, session->uid);
        #endif

        return;
    }
    if(command == CMD_CREATE_USER) {
    
        cmd_t cu_command;
        if(parse_command(payload, &cu_command) < 0) {
            const char *reason = "Failed to parse command";
            send_err(clientSocket, EINVAL, reason);
            return;
        }

        if(cu_command.argc < 2) {
            const char *reason = "Insufficient arguments for create_user";
            send_err(clientSocket, EINVAL, reason);
            return;
        }

        // permissions in octal
        char *end;
        errno = 0;
        long perms = strtol(cu_command.buf2, &end, 8);
        if(errno != 0 || *end != '\0' || perms < 0 || perms > 0777) {
            const char *reason = "Invalid permissions format";
            send_err(clientSocket, EINVAL, reason);
            return;
        }

        uint32_t err_size = sizeof(error_msg);
        int result = handle_create_user(cu_command.buf, perms, session->root_path, error_msg, err_size);
        // check if err_message is not empty, if so send it to the client
        if(strlen(error_msg) > 0) {
            send_err(clientSocket, EACCES, error_msg);
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
        case(CMD_LIST): {
            // tokenize payload with parse_command()
            cmd_t list_command;
            if(parse_command(payload, &list_command) < 0) {
                const char *reason = "Failed to parse command";
                send_err(clientSocket, EINVAL, reason);
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
                #if 0 // removed for now, as it is not needed, but can be useful for future debugging
                // validate cwd against root_path

                printf("[DEBUG]: Validating current working directory: %s against root path: %s\n", cwd, session->root_path);
                r = validate_path(cwd, session->root_path, full_path);
                if(r < 0) {
                    printf("[CHECKPOINT]\n");
                    const char *reason = "Invalid current working directory";
                    send_err(clientSocket, EINVAL, reason);
                    return;
                }
                #endif
            } else {
                #if DEBUG
                printf("[DEBUG]: User:%s Listing directory: %s\n", session->user, list_command.buf);
                #endif

                // allowed scope: root_path (for all other commands is home_path)
                r = validate_path(list_command.buf, session->root_path, full_path);
                if(r < 0) {
                    const char *reason = "Path outside of allowed scope";
                    send_err(clientSocket, EINVAL, reason);
                    return;
                }
            }



            // TODO: lock on file

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
                send_ok(clientSocket, "Directory is empty", strlen("Directory is empty"));
                sb_free(&sb);
                return;
            }
            send_ok(clientSocket, sb.data, sb.len);
            sb_free(&sb);

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
            char notify_buffer[256];
            ssize_t n = read(session.notify_fd, notify_buffer, sizeof(notify_buffer));
            if(n < 0) {
                perror("read from pipe");
                break;
            }
            notify_buffer[n] = '\0'; // null-terminate the string
            // process the notification

            #if DEBUG
            printf("[DEBUG]: Received notification for a transfer request: %s\n", notify_buffer);
            #endif

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
    // TODO: any other cleanup if necessary
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
    if(mkdir(sessions_dir, 0700) < 0 && errno != EEXIST) {
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
                if(kill(pid, 0) == -1 && errno == ESRCH) {
                    // process does not exist, safe to remove the fifo
                    char fifo_path[PATH_MAX + 64];
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
    

    if(setup_signal_handler() < 0) {
        fprintf(stderr, "Failed to setup signal handler.\n");
        exit(EXIT_FAILURE);
    }

    signal(SIGPIPE, SIG_IGN); // ignore SIGPIPE to prevent the server from crashing when trying to write to a closed socket

    int s = start_server(ip_address, port_number);

    printf("[SETUP]: Server started on %s:%s\n", ip_address, port_number);

    fd_set readfds;

    // determine max file descriptor for select() between stdin and listen socket
    int max_fd = (s > STDIN_FILENO) ? s : STDIN_FILENO;

    struct sockaddr_in clientAddress; // address struct to hold the client address information
    socklen_t clientAddressLen = sizeof(clientAddress); // len of the client address struct
    int clientSocket; // fd used for the accepted connection
    int select_result; // result of select()
    char buffer[MAX_CLIENT_BUFFER]; // buffer for reading data from clients or stdin

    char err_msg[256]; // buffer for error messages
    uint32_t err_size = sizeof(err_msg);

    if(server_gid == 0){
        int result = setup_server_gid(err_msg, err_size);
        printf("[SETUP]: Server GID set to %d\n", server_gid);
        if(result < 0) {
            fprintf(stderr, "[SETUP] %s\n", err_msg);
            exit(EXIT_FAILURE);
        }
    }

    printf("[SETUP]: Server loop started. \tType 'exit' to close\n");

    while(1){
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

            printf("[INFO]: New connection accepted from %s:%d\n", inet_ntoa(clientAddress.sin_addr), ntohs(clientAddress.sin_port));

            if(clientSocket < 0) {
                if(errno == EINTR) continue; // if interrupted by signal, retry
                perror("accept"); continue;
            }

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
                // TODO: implement parent process logic
            }
        }

        
    }
    // kill all child processes before exiting, as if we're out the loop the server is shutting / there's been an error
    
    signal(SIGTERM, SIG_IGN); // ignore SIGTERM in the parent to avoid killing myself
    kill(0, SIGTERM); // send SIGTERM, 0 indicates all processes in the same process group
    
    printf("[CLOSE] All children have been killed. \n");

    close(s);

    return 0;
}