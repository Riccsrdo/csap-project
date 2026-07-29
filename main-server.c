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

typedef struct {
    int sockfd;
    struct sockaddr_in address;
} peer_t;

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

int start_server(){

    // create TCP socket with SO_LINGER option set to 1, which means that when the socket is closed, 
    // the system will try to send any remaining data for a short period of time before forcefully 
    // closing the connection.
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    int linger = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger));

    struct sockaddr_in server_address;
    memset(&server_address, 0, sizeof(server_address));

    // Initial message
    char *msg = "Hello client! :D\n";

    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(8080);
    inet_aton("127.0.0.1", &server_address.sin_addr);

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

int build_fd_set(fd_set *readfds, fd_set *writefds, fd_set *exceptfds) {
    int i;

    FD_ZERO(readfds);
    FD_ZERO(writefds);
    FD_ZERO(exceptfds);

    // add entries to the sets

    return 0;
}

int handle_new_connection(int sockfd, peer_t *clients){

    struct sockaddr_in client_address;
    memset(&client_address, 0, sizeof(client_address));
    socklen_t client_address_len = sizeof(client_address);
    int client_sock = accept(sockfd, (struct sockaddr*)&client_address, &client_address_len);
    if(client_sock < 0) {
        perror("accept");
        return -1;
    }

    // look for an empty slot in the clients array
    for(int i = 0; i < MAX_CLIENTS; i++) {
        if(clients[i].sockfd == 0) {
            clients[i].sockfd = client_sock;
            clients[i].address = client_address;
            return 0;
        }
    }

    close(client_sock); // no empty slot, close the new connection
    fprintf(stderr, "Max clients reached, rejecting new connection.\n");

    return -1;
}


int main(int argc, char *argv[]) {

    if(setup_signal_handler() < 0) {
        fprintf(stderr, "Failed to setup signal handler.\n");
        exit(EXIT_FAILURE);
    }

    int s = start_server();

    // handle new connections through accept, without continuously polling, and assigning a new process to each connection through fork.
    // use select() for handling read()/write() on multiple sockets, including the listening socket and the connected sockets.
    fd_set readfds; // used for monitoring multiple file descriptors to see if they are ready for reading
    fd_set writefds; // used for monitoring multiple file descriptors to see if they are ready for writing
    fd_set exceptfds; // used for monitoring multiple file descriptors to see if they have an exceptional condition pending


    build_fd_set(&readfds, &writefds, &exceptfds);

    peer_t clients[MAX_CLIENTS];
    memset(clients, 0, sizeof(clients));

    while(1){
        // use select() to wait for activity on the listening socket or any of the connected sockets
        int activity = select(s + 1, &readfds, &writefds, &exceptfds, NULL);
        if(activity < 0) {
            perror("select");
            break;
        }

        // check if there is a new connection on the listening socket
        if(FD_ISSET(s, &readfds)) {
            handle_new_connection(s, clients); // accept new connection and add to clients array
        }

        // check for activity on each connected socket, spawn a process with fork() to handle the connection
        for(int i = 0; i < MAX_CLIENTS; i++) {
            if(clients[i].sockfd > 0 && FD_ISSET(clients[i].sockfd, &readfds)) {
                // spawn a new process to handle the connection
                pid_t pid = fork();
                if(pid < 0) {
                    perror("fork");
                    continue;
                }
                if(pid == 0) { // child process
                    close(s); // close the listening socket in the child process
                    char buffer[MAX_CLIENT_BUFFER];
                    int bytes_received = recv(clients[i].sockfd, buffer, sizeof(buffer), 0);
                    if(bytes_received < 0) {
                        perror("recv");
                        exit(EXIT_FAILURE);
                    }
                    buffer[bytes_received] = '\0'; // null-terminate the received data
                    printf("Received from client: %s", buffer);
                    send(clients[i].sockfd, buffer, bytes_received, 0); // echo back to client
                    close(clients[i].sockfd); // close the connected socket in the child process
                    exit(EXIT_SUCCESS);
                } else { // parent process
                    close(clients[i].sockfd); // close the connected socket in the parent process
                    clients[i].sockfd = 0; // mark the slot as empty
                }
            }
        }

    }

    close(s);

    return 0;
}