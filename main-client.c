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

#define MAX_CLIENT_BUFFER 1024
#define MAX_SERVER_BUFFER 1024

// Handle SIGCHLD to reap child processes and avoid zombies
void handle_signal_child(int sig) {
    // wait for all dead processes (SIGCHLD) without blocking
    while(waitpid(-1, NULL, WNOHANG) > 0);
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

    struct sockaddr_in server_address; // struct to memorize server address information
    memset(&server_address, 0, sizeof(server_address)); // prepare memory for the struct

    server_address.sin_family = AF_INET; // IPv4
    server_address.sin_port = htons(atoi(port_number)); // port number, converted to network byte order

    inet_aton(ip_address, &server_address.sin_addr); // convert string to network address

    if(connect(sockfd, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) {
        perror("connect");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    return sockfd;
}

// takes the readfds set, the socket_fd and the pipe of child processes running commands in background
void configure_read_set(fd_set *readfds, int socket_fd, int pipe_fd) {
    FD_ZERO(readfds);
    FD_SET(STDIN_FILENO, readfds); // add stdin to the set
    FD_SET(socket_fd, readfds); // add the socket to the set
    if(pipe_fd >= 0) {
        FD_SET(pipe_fd, readfds); // add the pipe to the set if valid
    }
}

// default <IP> is 127.0.0.1, default <port> is 8080
int main(int argc, char *argv[]) {

    // configure a handler for SIGCHLD to reap child processes and avoid zombies
    if(setup_signal_handler() < 0) {
        fprintf(stderr, "Failed to setup signal handler.\n");
        exit(EXIT_FAILURE);
    }

    // ignore SIGPIPE coming from children performing background operations
    // to avoid termination of the client when a child process tries to write to a closed socket
    signal(SIGPIPE, SIG_IGN);

    // Check command line arguments for IP and port, set defaults if not provided
    if(argc < 1) {
        fprintf(stderr, "Usage: %s [<IP>] [<port>]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    char *ip_address = (argc > 1) ? argv[1] : "127.0.0.1";
    char *port_number = (argc > 2) ? argv[2] : "8080";

    // Configure the readfds set for select(), used to check if there is activity on stdin, the socket, or the pipe
    fd_set readfds;

    int s = start_client(ip_address, port_number); // start the client and get the socket descriptor

    // create a pipe for communication with child processes, which handles background operations
    int pipe_fd[2];
    if(pipe(pipe_fd) < 0) {
        perror("pipe");
        close(s);
        exit(EXIT_FAILURE);
    }

    configure_read_set(&readfds, s, pipe_fd[0]);

    int select_result; // result of the select() call in the main loop

    while(1){
        // reset readfds before each select call
        configure_read_set(&readfds, s, pipe_fd[0]);

        // select() blocks, waiting for activity either on stdin, the socket, or the pipe
        select_result = select(FD_SETSIZE, &readfds, NULL, NULL, NULL);

        if(select_result < 0) {
            perror("select");
            break; // exit the loop on error
        }

        // check if there is activity on stdin (user input)
        if(FD_ISSET(STDIN_FILENO, &readfds)) {
            
            // parse command, using appropriate function, and then send it to server

            // if -b option is contained in upload/download command, fork a child 
            // process to handle the command in background, and use the pipe to communicate with the main process

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
                close(pipe_fd[1]); // close write end of the pipe in the parent
                // continue to next iteration to wait for more input or server response
            }

        }

        // check if there is activity on the socket (server response)
        if(FD_ISSET(s, &readfds)) {
            // read the server response and print it to stdout
            // TODO: implement a chunking system to handle large responses, if necessary
        }


        // check if there is activity on the pipe (child process output)
        if(FD_ISSET(pipe_fd[0], &readfds)) {
            // read the output from the child process and print it to stdout
            // TODO: implement pipe reading logic
        }
    }


    close(s);
    return 0;
}