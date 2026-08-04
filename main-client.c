/* main-client.c

Responsible for:
- Starting;
- Loop select of stdin, socket and pipe;
- management of child process;
*/
#include<netinet/in.h> // Address information
#include<stdio.h>
#include<stdlib.h>
#include<sys/socket.h> // Socket APIs
#include<sys/types.h>
#include<arpa/inet.h> // inet_pton
#include<string.h>
#include<unistd.h>
#include<limits.h>
#include<signal.h>
#include<fcntl.h> // fcntl
#include<sys/wait.h> // waitpid
#include<errno.h> // EINTR
//#include"network.h"
#include"network/network.h" // fix with -I
//#include"protocol.h"
#include"protocol/protocol.h" // fix with -I
#include"utils/utils.h" // fix with -I

#define MAX_CLIENT_BUFFER 1024
#define MAX_SERVER_BUFFER 1024


char *ip_address; // global variable to hold the IP address
char *port_number; // global variable to hold the port number

char username[64]; // global variable to hold the username
int background_operations; // global variable to hold the number of background operations

// Handle SIGCHLD to reap child processes and avoid zombies
void handle_signal_child(int sig) {
    (void)sig;
    // wait for all dead processes (SIGCHLD) without blocking
    int saved_errno = errno; 
    while(waitpid(-1, NULL, WNOHANG) > 0);
    errno = saved_errno; // restore errno
}

// Handler for signals
int setup_signal_handler() {
    struct sigaction sa;
    sa.sa_handler = handle_signal_child; // set the signal handler function
    sigemptyset(&sa.sa_mask); // empty the signal mask
    sa.sa_flags = SA_RESTART; // restart interrupted system calls
    if(sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
    return 0;

}

/*
Start the client with socket, return the socket descriptor.
*/
int start_client(char *ip_address, char *port_number){
    int sockfd = socket(AF_INET, SOCK_STREAM, 0); // protocol is set to zero, associated automatically to
    // SOCK_STREAM, which is TCP.

    if(sockfd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    printf("[SETUP]: Socket created for client\n");

    struct sockaddr_in server_address; // struct to memorize server address information
    memset(&server_address, 0, sizeof(server_address)); // prepare memory for the struct

    server_address.sin_family = AF_INET; // IPv4
    int port = atoi(port_number); // convert port number from string to integer
    if(port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port number: %s\n", port_number);
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    server_address.sin_port = htons(port); // port number, converted to network byte order

    if(inet_aton(ip_address, &server_address.sin_addr) == 0) { // convert string to network address
        fprintf(stderr, "Invalid IP address: %s\n", ip_address);
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    if(connect(sockfd, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) {
        perror("connect");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    return sockfd;
}

// takes the readfds set, the socket_fd and the pipe of child processes running commands in background
int configure_read_set(fd_set *readfds, int socket_fd, int pipe_fd) {
    FD_ZERO(readfds);
    FD_SET(STDIN_FILENO, readfds); // add stdin to the set
    FD_SET(socket_fd, readfds); // add the socket to the set
    if(pipe_fd >= 0) {
        FD_SET(pipe_fd, readfds); // add the pipe to the set if valid
    }

    // look for the maximum file descriptor for select()
    int max_fd = (socket_fd > STDIN_FILENO) ? socket_fd : STDIN_FILENO;
    if(pipe_fd >= 0 && pipe_fd > max_fd) {
        max_fd = pipe_fd;
    }

    return max_fd;
    
}

// Waits for a response from the server, handling asynchronous notifications, printing them to stdout
int wait_response(int fd){
    for(;;){
        uint8_t command;
        char *payload = NULL;
        uint32_t payload_len;

        if(recv_packet(fd, (char **)&payload, &command, &payload_len) < 0) {
            fprintf(stderr, "Error receiving packet from server\n");
            free(payload);
            return -1;
        }

        #if DEBUG
        printf("[DEBUG]: Received command: %d, payload content: %s\n", command, (char*)payload);
        #endif

        // print, based on error code/success code, related message to stdout
        if(command == RSP_NOTIFY){
            printf("[NOTIFY]: %s \n", (char *)payload);
            free(payload);
            continue; // continue to wait for the actual response
        } else if(command == RSP_OK) {
            // check if payload is empty, if yes, print a default message, otherwise print the payload
            if(payload_len == 0 || payload == NULL) {
                printf("[Server]: OK\n");
            } else {
                printf("[Server]: \n%s \n", (char *)payload);
            }
            free(payload);
            return 0; // success
        } else if(command == RSP_ERR) {
            int code; char msg[256];
            if (sscanf(payload, "%d %255[^\n]", &code, msg) == 2)
                fprintf(stderr, "[ERROR]: %s (%s)\n", msg, strerror(code));
            free(payload);
            return -1; // error
        } else {
            fprintf(stderr, "[ERROR]: Unknown response from server\n");
            free(payload);
            return -1; // unknown response
        }
    }

    // should not reach here
    return -1;
}

void spawn_background(cmd_t *command, int* pipe_fd, int socket_fd) {
    (void)command;
    fflush(NULL);
    pid_t pid = fork();
    if(pid == 0){
        close(pipe_fd[0]); // close the read end of the pipe in the child
        close(socket_fd); // close the socket in the child, as it will not be used

        // open a connection to the server for the background operation
        int bg_socket_fd = start_client(ip_address, port_number);
        // need to authenticate the background operation with the server, using the username
        char payload[256];
        // concatenate "login [username]" to the payload
        snprintf(payload, sizeof(payload), "login %s", username);
        send_packet(bg_socket_fd, CMD_LOGIN, payload, strlen(payload));
        if(wait_response(bg_socket_fd) < 0) {
            fprintf(stderr, "[ERROR]: Background operation failed to authenticate with server.\n");
            close(bg_socket_fd);
            _exit(EXIT_FAILURE);
        }

        // upload content/download content
        // function()

        char *msg = "Background operation completed.\n";
        write(pipe_fd[1], msg, strlen(msg)); // write to the pipe to notify the parent
        close(bg_socket_fd);
        _exit(EXIT_SUCCESS);
    } else if (pid < 0) {
        perror("fork");
    } 
}

// default <IP> is 127.0.0.1, default <port> is 8080
int main(int argc, char *argv[]) {

    username[0] = '\0'; // initialize username to empty string

    // configure a handler for SIGCHLD to reap child processes and avoid zombies
    if(setup_signal_handler() < 0) {
        fprintf(stderr, "Failed to setup signal handler.\n");
        exit(EXIT_FAILURE);
    }

    // ignore SIGPIPE coming from children performing background operations
    // to avoid termination of the client when a child process tries to write to a closed socket
    signal(SIGPIPE, SIG_IGN);

    ip_address = (argc > 1) ? argv[1] : "127.0.0.1";
    port_number = (argc > 2) ? argv[2] : "8080";

    // Configure the readfds set for select(), used to check if there is activity on stdin, the socket, or the pipe
    fd_set readfds;

    int socket_fd = start_client(ip_address, port_number); // start the client and get the socket descriptor

    printf("[SETUP]: Client started. Connected to server at %s:%s\n", ip_address, port_number);

    // create a pipe for communication with child processes, which handles background operations
    int pipe_fd[2];
    if(pipe(pipe_fd) < 0) {
        perror("pipe");
        close(socket_fd);
        exit(EXIT_FAILURE);
    }

    int select_result; // result of the select() call in the main loop
    int max_fd;

    background_operations = 0;

    while(1){
        // reset readfds before each select call
        max_fd = configure_read_set(&readfds, socket_fd, pipe_fd[0]);

        // select() blocks, waiting for activity either on stdin, the socket, or the pipe
        select_result = select(max_fd + 1, &readfds, NULL, NULL, NULL);

        if (select_result < 0) {
            if (errno == EINTR) continue;
            perror("select"); break;
        }
        
        // check if there is activity on stdin (user input)
        if(FD_ISSET(STDIN_FILENO, &readfds)) {
            
            // parse command, using appropriate function, and then send it to server

            // if -b option is contained in upload/download command, fork a child 
            // process to handle the command in background, and use the pipe to communicate with the main process
            /* TODO: implement mechanism for -b
            fflush(NULL);
            pid_t pid = fork();
            if(pid < 0) {
                perror("fork");
                continue; // continue to next iteration on error
            } else if(pid == 0) { // child process
                close(pipe_fd[0]); // close read end of the pipe in the child
                // handle the command and write output to pipe_fd[1]
                // after handling, exit the child process
                _exit(0);
            } else { // parent process
                // continue to next iteration to wait for more input or server response
            }
                */

            char line[MAX_CLIENT_BUFFER];
            ssize_t bytes_read = read(STDIN_FILENO, line, sizeof(line) - 1);
            if(bytes_read <= 0) {
                break; // exit the loop on error
            }

            line[bytes_read] = '\0';
            line[strcspn(line, "\n")] = '\0'; // remove newline character

            // implement commands parsing and handling mechanism

            cmd_t command;

            int result = parse_command(line, &command);
            if(result < 0) {
                fprintf(stderr, "Invalid command: %s\n", line);
                continue; // continue to next iteration on error
            }

            if(command.code == CMD_EXIT) {
                if(background_operations > 0) {
                    printf("[INFO]: There are %d background operations running. Exiting is not permitted until they are completed.\n", background_operations);
                    continue; // continue to next iteration, do not exit
                }
                printf("[CLOSE]: Exiting client.\n");
                break; // exit the loop on 
            }

            // if -b option is set, spawn a background process to handle the command
            if(command.is_background) {
                spawn_background(&command, pipe_fd, socket_fd);
                background_operations++;
                printf("[INFO]: Background operation started. Total background operations: %d\n", background_operations);
                continue; // continue to next iteration to wait for more input or server response
            }

            // send the command to the server
            send_packet(socket_fd, command.code, line, strlen(line));

            
            int rc = wait_response(socket_fd);

            if(rc == 0 && command.code == CMD_LOGIN) {
                // if login was successful, store the username for future background operations
                strncpy(username, command.buf, sizeof(username) - 1);
                username[sizeof(username) - 1] = '\0'; // ensure null-termination
            }

            continue;
        }

        // check if there is activity on the socket (server response)
        if(FD_ISSET(socket_fd, &readfds)) {
            // server has sent something, without client first requesting somethign
            uint8_t command;
            char *payload = NULL;
            uint32_t payload_len;

            if(recv_packet(socket_fd, (char **)&payload, &command, &payload_len) < 0) {
                fputs("Server closed. \n", stdout);
                free(payload);
                break; // exit the loop on error
            }
            if(command == RSP_NOTIFY){
                printf("%s \n", (char *)payload);
                free(payload);
                continue;
            } else {
                free(payload);
            }
        }


        // check if there is activity on the pipe (child process output)
        if(FD_ISSET(pipe_fd[0], &readfds)) {
            // read the output from the child process and print it to stdout
            // TODO: implement pipe reading logic

            char buf[512];
            ssize_t bytes_read = read(pipe_fd[0], buf, sizeof(buf) -1);
            if(bytes_read > 0) {
                buf[bytes_read] = '\0';
                fputs(buf, stdout);
            }
            // decrease background_operations count, one decrease for each \n received from the child process, indicating that a background operation has completed
            for(int i = 0; i < bytes_read; i++) {
                if(buf[i] == '\n') {
                    background_operations--;
                }
            }
        }
    }


    close(socket_fd); // close the socket before exiting
    return 0;
}