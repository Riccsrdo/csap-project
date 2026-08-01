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
#include"network/network.h"
#include<sys/types.h>
#include<sys/stat.h>

#define MAX_CLIENT_BUFFER 1024

char *root_directory; // global variable to hold the root directory path

void handle_signals(int sig) {
    // wait for all dead processes (SIGCHLD) without blocking
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

    // create TCP socket with SO_LINGER option set to 1, which means that when the socket is closed, 
    // the system will try to send any remaining data for a short period of time before forcefully 
    // closing the connection.
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if(sockfd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    int opt1  = 1;
    // Set SO_REUSEADDR to allow the socket to be bound to an address that is already in use.
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt1, sizeof(opt1)); 


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

    // listen for incoming connections
    if(listen(sockfd, 10) < 0) {
        perror("listen");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    return sockfd;

}

void configure_read_set(fd_set *readfds, int socket_fd) {
    FD_ZERO(readfds);
    FD_SET(STDIN_FILENO, readfds); // add stdin to the set
    FD_SET(socket_fd, readfds); // add the socket to the set
}

void dispatch(session_t *session, int clientSocket, uint8_t command, void *payload, uint32_t payload_len) {
    // Commands without login
    if(command == CMD_LOGIN) {
        //handle_login(session, clientSocket, payload, payload_len);
        // set session->logged_in = 1 if login is successful
        return;
    }
    if(command == CMD_CREATE_USER) {
        //handle_create_user(session, clientSocket, payload, payload_len);
        return;
    }

    // Commands with login

    // first check if user is logged in, if not send an error response to the client
    if(!session->logged_in) {
        // send error response to client indicating that login is required
        const char *error_msg = "Login required";
        send_err(clientSocket, EACCES, error_msg, strlen(error_msg));
        return;
    }

    switch(command){
        case(CMD_LIST): {
            break;
        }
        default: {
            // send error response to client indicating that the command is not recognized
            const char *error_msg = "Command not recognized";
            send_err(clientSocket, EINVAL, error_msg, strlen(error_msg));
        }
    }



}


void handle_session(int clientSocket) {
    session_t session;
    session.logged_in = 0; // not logged in initially
    session.notify_fd = -1; // no notification pipe initially

    strncpy(session.root_path, root_directory, PATH_MAX - 1); // set the root path for the session

    fd_set readfds;
    // initiate a fifo for communication between child processes handling different users connected
    // the fifo will be named according to child's pid, and other processes will write into it to notify
    char fifo_name[256];
    // it's created within the root directory of the server, and will be removed when the child process exits
    snprintf(fifo_name, sizeof(fifo_name), "%s/fifo_%d", session.root_path, getpid());

    if(mkfifo(fifo_name, 0666) < 0) {
        perror("mkfifo");
        close(clientSocket);
        return;
    }

    // the above fifo is used to send requests
    // I create another fifo to receive responses, following same naming convention, adding a _resp suffix
    char fifo_resp_name[256];
    snprintf(fifo_resp_name, sizeof(fifo_resp_name), "%s/fifo_%d_resp", session.root_path, getpid());

    if(mkfifo(fifo_resp_name, 0666) < 0) {
        perror("mkfifo");
        close(clientSocket);
        return;
    }
    session.notify_resp_fd = open(fifo_resp_name, O_RDONLY);

    for(;;){
        // configure the readfds set for select(), used to check if there is activity on the socket
        FD_ZERO(&readfds);
        FD_SET(clientSocket, &readfds);
        FD_SET(session.notify_fd, &readfds); // add the read end of the fifo to the set
        int max_fd = (clientSocket > session.notify_fd) ? clientSocket : session.notify_fd;

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

            dispatch(&session, clientSocket, command, payload, payload_len);
            free(payload); // free the payload after dispatching
        }

        if(FD_ISSET(session.notify_fd, &readfds)) {
            // handle transfer requests
            char notify_buffer[256];
            ssize_t n = read(session.notify_fd, notify_buffer, sizeof(notify_buffer));
            if(n < 0) {
                perror("read from pipe");
                break;
            }
            // process the notification

        }

    }

    // cleanup session resources
    if(session.notify_fd >= 0) {
        close(session.notify_fd);
    }
    session.logged_in = 0; // mark as logged out
    // TODO: any other cleanup if necessary
}


void create_root_directory(char *root_directory) {
}


/*
Main

takes three arguments:
- root directory for the server to serve files from;
- IP address to bind to, default is 127.0.0.1;
- port number to bind to, default is 8080.
*/
int main(int argc, char *argv[]) {

    if(argc < 2) {
        fprintf(stderr, "Usage: %s <root_directory> [<IP>] [<port>]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    root_directory = argv[1];
    char *ip_address = (argc > 2) ? argv[2] : "127.0.0.1";
    char *port_number = (argc > 3) ? argv[3] : "8080";

    create_root_directory(root_directory); // TODO: implement this func to construct root path, validate it

    if(setup_signal_handler() < 0) {
        fprintf(stderr, "Failed to setup signal handler.\n");
        exit(EXIT_FAILURE);
    }

    signal(SIGPIPE, SIG_IGN);

    int s = start_server(ip_address, port_number);

    fd_set readfds;

    // determine max file descriptor for select() between stdin and listen socket
    int max_fd = (s > STDIN_FILENO) ? s : STDIN_FILENO;

    struct sockaddr_in clientAddress; // address struct to hold the client address information
    socklen_t clientAddressLen = sizeof(clientAddress); // len of the client address struct
    int clientSocket; // fd used for the accepted connection
    int select_result; // result of select()
    char buffer[MAX_CLIENT_BUFFER]; // buffer for reading data from clients or stdin

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
                break; // exit the loop on error
            }

            buffer[bytes_read] = '\0';
            if(strcmp(buffer, "exit") == 0) {
                printf("Exiting server.\n");
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
    
    close(s);

    return 0;
}