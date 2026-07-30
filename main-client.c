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

#define MAX_CLIENT_BUFFER 1024
#define MAX_SERVER_BUFFER 1024

/*
Start the client with socket, return the socket descriptor.
*/
int start_client() {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0); // protocol is set to zero, associated automatically to
    // SOCK_STREAM, which is TCP.

    if(sockfd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_address; // struct to memorize server address information
    memset(&server_address, 0, sizeof(server_address)); // prepare memory for the struct

    server_address.sin_family = AF_INET; // IPv4
    server_address.sin_port = htons(8080); // port number, converted to network byte order
    
    inet_aton("127.0.0.1", &server_address.sin_addr); // convert string to network address

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

int main(int argc, char *argv[]) {

    fd_set readfds;

    int s = start_client();

    // create a pipe for communication with child processes
    int pipe_fd[2];
    if(pipe(pipe_fd) < 0) {
        perror("pipe");
        close(s);
        exit(EXIT_FAILURE);
    }

    while(1){
        printf("Client: \t");
        char buffer_client[MAX_CLIENT_BUFFER];
        fgets(buffer_client, sizeof(buffer_client), stdin);

        send(s, buffer_client, strlen(buffer_client), 0);

        if(strncmp(buffer_client, "exit", 4) == 0) {
            break;
        }

        char buffer_server[MAX_SERVER_BUFFER];
        if(recv(s, buffer_server, sizeof(buffer_server), 0) < 0) {
            perror("recv");
            // handle error, maybe exit or continue
        }
        printf("Server: \t%s", buffer_server);

    }


    close(s);
    return 0;
}