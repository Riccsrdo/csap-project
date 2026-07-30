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

#define MAX_CLIENTS 10
#define MAX_CLIENT_BUFFER 1024

void handle_signals(int sig) {
    // wait for all dead processes (SIGCHLD) without blocking
    while(waitpid(-1, NULL, WNOHANG) > 0);
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
    server_address.sin_port = htons(atoi(port_number));
    inet_aton(ip_address, &server_address.sin_addr);

    // Bind the socket to the address
    if(bind(sockfd, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) {
        perror("bind");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // listen for incoming connections
    if(listen(sockfd, MAX_CLIENTS) < 0) {
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

    char *root_directory = argv[1];
    char *ip_address = (argc > 2) ? argv[2] : "127.0.0.1";
    char *port_number = (argc > 3) ? argv[3] : "8080";

    if(setup_signal_handler() < 0) {
        fprintf(stderr, "Failed to setup signal handler.\n");
        exit(EXIT_FAILURE);
    }

    int s = start_server(ip_address, port_number);

    fd_set readfds;
    configure_read_set(&readfds, s);

    // determine max file descriptor for select() between stdin and listen socket
    int max_fd = (s > STDIN_FILENO) ? s : STDIN_FILENO;

    int clients = 0; // number of currently connected clients
    struct sockaddr_in clientAddress; // address struct to hold the client address information
    socklen_t clientAddressLen = sizeof(clientAddress); // len of the client address struct
    int clientSocket; // fd used for the accepted connection
    int select_result; // result of select()
    char buffer[MAX_CLIENT_BUFFER]; // buffer for reading data from clients or stdin

    /*
    loop:
    cfd = accept(listenfd)          
    pid = fork()
    if figlio:  close(listenfd); handle_client_session(cfd); _exit()
    if padre:   close(cfd), torna ad accept()
    */
    while(1){
        // clean file descriptor set
        configure_read_set(&readfds, s);

        // select() will block, waiting for activity
        select_result = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if(select_result < 0) {
            perror("select");
            break; // exit the loop on error
        }

        // check first if there is activity on stdin (server operator input)
        if(FD_ISSET(STDIN_FILENO, &readfds)) {
            if(fgets(buffer, sizeof(buffer), stdin) == NULL) {
                // error or EOF
                if(feof(stdin)) {
                    printf("EOF on stdin, exiting.\n");
                    break; // exit the loop on EOF
                } else {
                    perror("fgets");
                    continue; // continue to next iteration on error
                }
            }

            // handle command
            buffer[strcspn(buffer, "\n")] = 0; // remove newline character
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
            // accept a new connection
            clientSocket = accept(s, (struct sockaddr*)&clientAddress, &clientAddressLen);

            // if number of clients exceeds MAX_CLIENTS, reject the connection
            if(clients >= MAX_CLIENTS) {
                fprintf(stderr, "Max clients reached. Rejecting connection.\n");
                close(clientSocket);
                continue;
            }

            if(clientSocket < 0) {
                perror("accept");
                // TODO: appropriate error handling, maybe continue or exit
                continue;
            }

            // fork a new process to handle the client
            pid_t pid = fork();
            if(pid < 0) {
                perror("fork");
                close(clientSocket);
                continue;
            } else if(pid == 0) { // child process
                close(s); // child does not need the listening socket

                // handle the client session and the exit


            } else { // parent process
                close(clientSocket); // parent does not need the connected socket
                // TODO: implement parent process logic
                clients++;
            }
        }

        
    }

    close(s);

    return 0;
}