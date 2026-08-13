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
#include "transfer/transfer.h"
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


int recv_response(int fd, char *out, size_t out_size, uint32_t *out_len, int print_ok){
    if(out && out_size > 0) out[0] = '\0';
    if(out_len) *out_len = 0;

    for(;;){
        uint8_t command;
        char *payload = NULL;
        uint32_t payload_len;

        // receive a packet from the server, which could be a notification, an OK response, or an error response
        if(recv_packet(fd, (char **)&payload, &command, &payload_len) < 0) {
            fprintf(stderr, "Error receiving packet from server\n");
            free(payload);
            return -1;
        }

        #if DEBUG
        printf("[DEBUG]: Received command: %d, payload content: %s\n", command, (char*)payload);
        #endif

        if(command == RSP_NOTIFY){ // if it's a notification, print it to stdout and continue waiting for the actual response
            printf("[NOTIFY]: %s \n", (char *)payload);
            free(payload);
            continue; // asynchronous notification, keep waiting for the real answer
        } else if(command == RSP_OK) {
            if(out && out_size > 0){ // if the caller provided a buffer, copy the payload into it
                size_t n = (payload_len < out_size - 1) ? payload_len : out_size - 1;
                if(payload && n > 0) memcpy(out, payload, n);
                out[n] = '\0';
            }
            if(out_len) *out_len = payload_len;
            //if print ok
            if(print_ok){
                if(payload_len == 0 || payload == NULL) {
                    printf("[Server]: OK\n");
                } else {
                    printf("[Server]: \n%s \n", (char *)payload);
                }
                fflush(stdout);
            }
            free(payload);
            return 0;
        } else if(command == RSP_ERR) {
            int code; char msg[256];
            if (payload && sscanf(payload, "%d %255[^\n]", &code, msg) == 2)
                fprintf(stderr, "[ERROR]: %s (%s)\n", msg, strerror(code));
            else
                fprintf(stderr, "[ERROR]: server refused the command\n");
            free(payload);
            return -1;
        } else {
            fprintf(stderr, "[ERROR]: Unknown response from server\n");
            free(payload);
            return -1;
        }
    }
}

int wait_response(int fd){
    return recv_response(fd, NULL, 0, NULL, 1);
}


/*
This function handles, client side, commands requiring the sending of a stream of data in chunks, or the receiving.
local_fd must be already opened and ready for reading (for upload/write) or writing (for download/read).
*/
int run_stream_command(int sockfd, cmd_t *command, int local_fd){
    // flush to avoid interferences with printf()
    fflush(stdout);

    if(command->code == CMD_READ || command->code == CMD_DOWNLOAD_BEGIN){
        // the server answers OK with the number of bytes that will follow
        char size_payload[64];
        uint32_t size_len = 0;
        if(recv_response(sockfd, size_payload, sizeof(size_payload), &size_len, 0) < 0){
            return -1;
        }

        int64_t expected_total = -1; // unknown
        if(size_len > 0){
            char *end = NULL;
            errno = 0;
            long long v = strtoll(size_payload, &end, 10); // convert the payload to a long long integer
            if(errno == 0 && end != size_payload && v >= 0) expected_total = (int64_t)v; // valid number of bytes expected
        }

        uint8_t data_code = (command->code == CMD_READ) ? CMD_READ_DATA : CMD_DATA; // distinguish between read and download
        uint8_t end_code  = (command->code == CMD_READ) ? CMD_READ_END  : CMD_DATA_END;

        char err[256];
        err[0] = '\0'; 
        ssize_t received = recv_stream(sockfd, local_fd, -1, expected_total,
                                      data_code, end_code, err, sizeof(err));
        if(received < 0){
            fprintf(stderr, "[ERROR]: transfer failed: %s%s%s\n",
                    strerror((int)-received), err[0] ? " - " : "", err);
            return -1;
        }

        // to avoid interferences with printf() in the main loop, print the number of bytes received in stderr
        fprintf(stderr, "[Server]: %lld bytes received\n", (long long)received);
        return 0;
    }

    if(command->code == CMD_WRITE || command->code == CMD_UPLOAD_BEGIN){
        // first OK: the server is ready to receive
        if(recv_response(sockfd, NULL, 0, NULL, 0) < 0){
            return -1;
        }

        if(command->code == CMD_WRITE){
            fprintf(stderr, "[INFO]: type the content, then press Ctrl-D to end the input\n");
        }

        uint8_t data_code = (command->code == CMD_WRITE) ? CMD_WRITE_DATA : CMD_DATA; // distinguish between write and upload
        uint8_t end_code  = (command->code == CMD_WRITE) ? CMD_WRITE_END  : CMD_DATA_END;

        ssize_t sent = send_stream(sockfd, local_fd, -1, data_code, end_code);
        if(sent < 0){
            fprintf(stderr, "[ERROR]: failed to send the local file: %s\n", strerror((int)-sent));
            // warn the server that the transfer failed, so it can clean up and not wait for more data
            send_packet(sockfd, RSP_ERR, NULL, 0);
            recv_response(sockfd, NULL, 0, NULL, 0); // consume the server's response to the error, if any
            return -1;
        }

        // second OK: outcome of the write, with the number of bytes written
        char written[64];
        uint32_t written_len = 0;
        if(recv_response(sockfd, written, sizeof(written), &written_len, 0) < 0){
            return -1;
        }
        printf("[Server]: %s bytes written\n", written_len > 0 ? written : "0");
        fflush(stdout);
        return 0;
    }

    return -1;
}

// helper function to compute the local file descriptor based on the command type
int compute_local_fd(cmd_t *command, int *local_fd) {
    const char *local_name = NULL;
    if(command->code == CMD_READ) {
        *local_fd = STDOUT_FILENO;
    } else if(command->code == CMD_WRITE) {
        *local_fd = STDIN_FILENO;
    } else if(command->code == CMD_UPLOAD_BEGIN) {
        local_name = command->buf;  // upload <client path> <server path>
        *local_fd = open(local_name, O_RDONLY);
    } else { // CMD_DOWNLOAD_BEGIN: download <server path> <client path>
        local_name = command->buf2;
        *local_fd = open(local_name, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    }

    if(*local_fd < 0) {
        fprintf(stderr, "[ERROR]: cannot open the local file '%s': %s\n",
                local_name ? local_name : "", strerror(errno));
        return -1; // the command is not sent to the server if the local file cannot be opened
    }
    return 0;
}

/*
Helper function to notify the parent process about the termination of a child process handling a background operation.
*/
void bg_end_notify(int pipe_write_fd, int socket_fd, int exit_code, const char *msg) {
    if (msg != NULL) {
        // write message on pipe to notify the parent process about the termination of the child process
        write(pipe_write_fd, msg, strlen(msg));
    } else {
        // Used when msg is NULL, to still allow the decrementing of the background_operations counter in the parent process
        write(pipe_write_fd, "Background operation terminated.\n", 33);
    }

    if (socket_fd >= 0) {
        close(socket_fd);
    }
    _exit(exit_code);
}


void spawn_background(cmd_t *command, const char *line, int* pipe_fd, int socket_fd) {
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

        // wait for the server's response to the login command
        if(recv_response(bg_socket_fd, NULL, 0, NULL, 0) < 0) {
            bg_end_notify(pipe_fd[1], bg_socket_fd, EXIT_FAILURE,
                "[ERROR]: Background login failed.\n");
        }

        // upload content/download content
        int fd = -1;

        int r = compute_local_fd(command, &fd);
        if(r < 0) {
            bg_end_notify(pipe_fd[1], bg_socket_fd, EXIT_FAILURE,
                "Background operation failed to open local file.\n");
        }

        send_packet(bg_socket_fd, command->code, line, strlen(line));


        int rc = run_stream_command(bg_socket_fd, command, fd);
        if(fd > STDERR_FILENO) close(fd); // check to avoid closing stdin/stdout/stderr
        if(rc < 0) {
            bg_end_notify(pipe_fd[1], bg_socket_fd, EXIT_FAILURE,
                "[ERROR]: Background operation failed during stream transfer.\n");
        }
        
        char success_msg[256];
        if(command->code == CMD_UPLOAD_BEGIN){
            snprintf(success_msg, sizeof(success_msg), "[Background] Command: upload %.90s %.90s concluded successfully.\n", command->buf, command->buf2);
        }
        if(command->code == CMD_DOWNLOAD_BEGIN){
            snprintf(success_msg, sizeof(success_msg), "[Background] Command: download %.90s %.90s concluded successfully.\n", command->buf, command->buf2);
        }

        bg_end_notify(pipe_fd[1], bg_socket_fd, EXIT_SUCCESS, success_msg);
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
            int stdin_eof = 0; // flag to indicate if EOF has been reached on stdin
            ssize_t bytes_read = read_line(STDIN_FILENO, line, sizeof(line), &stdin_eof);
            if(bytes_read < 0 || stdin_eof) {
                break; 
            }
            if(bytes_read == 0) {
                continue; 
            }

            // implement commands parsing and handling mechanism

            cmd_t command;

            char error_msg[256];
            error_msg[0] = '\0'; // initialize error message to empty string
            uint32_t err_size = sizeof(error_msg);

            int result = parse_command(line, &command, error_msg, err_size);
            if(result < 0) {
                fprintf(stderr, "Failed to parse command: %s\n", error_msg);
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

            // boolean condition to check if the command is a stream command (upload/download or read/write)
            int is_stream = (command.code == CMD_READ || command.code == CMD_WRITE ||
                             command.code == CMD_UPLOAD_BEGIN || command.code == CMD_DOWNLOAD_BEGIN);

            // open the local file first, to check if it is accessible, before sending the command to the server
            int local_fd = -1;
            if(is_stream) {
                int r = compute_local_fd(&command, &local_fd);
                if(r < 0) {
                    continue; // the command is not sent to the server if the local file cannot be opened
                }
            }

            // if -b option is set, spawn a background process to handle the command
            if(command.is_background) {
                if(command.code == CMD_UPLOAD_BEGIN || command.code == CMD_DOWNLOAD_BEGIN) {
                    spawn_background(&command, line, pipe_fd, socket_fd);
                    if(local_fd > STDERR_FILENO) close(local_fd); // check to avoid closing stdin/stdout/stderr
                    background_operations++;
                    printf("[INFO]: Background operation started. Total background operations: %d\n", background_operations);
                    continue; // continue to next iteration to wait for more input or server response
                } else {
                    fprintf(stderr, "[ERROR]: Background operation is only supported for upload and download commands.\n");
                    if(local_fd > STDERR_FILENO) close(local_fd); // check to avoid closing stdin/stdout/stderr
                    continue; // continue to next iteration
                }
            }

            // send the command to the server
            send_packet(socket_fd, command.code, line, strlen(line));

            int rc;
            if(is_stream) {
                rc = run_stream_command(socket_fd, &command, local_fd);
                if(local_fd > STDERR_FILENO) close(local_fd); //  check to avoid closing stdin/stdout/stderr
            } else {
                rc = wait_response(socket_fd);
            }

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