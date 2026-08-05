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
#include"transfer/transfer.h"

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
                const char *reason = "Path outside of allowed scope";
                send_err(clientSocket, EINVAL, reason);
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
                const char *reason = "Path outside of allowed scope";
                send_err(clientSocket, EINVAL, reason);
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
                const char *reason = "Source path outside of allowed scope";
                send_err(clientSocket, EINVAL, reason);
                return;
            }

            // validate destination path against home_path, if not valid return error
            char full_dest_path[PATH_MAX];
            r = validate_path(move_command.buf2, session->home_path, full_dest_path);
            if(r < 0) {
                const char *reason = "Destination path outside of allowed scope";
                send_err(clientSocket, EINVAL, reason);
                return;
            }

            int move_result = move_cmd(full_src_path, full_dest_path);
            if(move_result < 0) {
                const char *reason = "Failed to move file/directory";
                send_err(clientSocket, -move_result, reason);
                return;
            }

            const char *reason = "File/Directory moved successfully";
            send_ok(clientSocket, reason, strlen(reason));

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
                const char *reason = "Path outside of allowed scope";
                send_err(clientSocket, EINVAL, reason);
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
                const char *reason = "Path outside of allowed scope";
                send_err(clientSocket, EINVAL, reason);
                return;
            }

            // open file contained in validated path
            int file_fd = open(full_path, O_RDONLY);
            if(file_fd < 0) {
                const char *reason = "Failed to open file";
                send_err(clientSocket, -errno, reason);
                return;
            }

            // TODO: lock

            // obtain info about file
            struct stat file_stat;
            if(fstat(file_fd, &file_stat) < 0) {
                const char *reason = "Failed to get file info";
                send_err(clientSocket, -errno, reason);
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
                const char *reason = "Path outside of allowed scope";
                send_err(clientSocket, EINVAL, reason);
                return;
            }

            // open file contained in validated path
            int file_fd = open(full_path, O_RDONLY);
            if(file_fd < 0) {
                const char *reason = "Failed to open file";
                send_err(clientSocket, -errno, reason);
                return;
            }

            // TODO: lock with fnctl() to prevent concurrent reads/writes

            // obtain info about file
            struct stat file_stat;
            if(fstat(file_fd, &file_stat) < 0) {
                const char *reason = "Failed to get file info";
                send_err(clientSocket, -errno, reason);
                close(file_fd);
                return;
            }

            // compute file size
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
                const char *reason = "Path outside of allowed scope";
                send_err(clientSocket, EINVAL, reason);
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

            // lock

            // tell client to start sending data
            const char *reason = "Ready to receive data";
            send_ok(clientSocket, reason, strlen(reason));

            // start receiving stream
            uint8_t data_code = CMD_DATA;
            uint8_t end_code = CMD_DATA_END;

            // offset -1: never seek, the temporary file is empty
            ssize_t stream_result = recv_stream(clientSocket, file_fd, -1, -1, data_code, end_code, error_msg, err_size);
            if(stream_result < 0) {
                close(file_fd);
                unlink(temp_path); // remove temporary file if upload failed
                send_err(clientSocket, (int)-stream_result,
                         error_msg[0] ? error_msg : "Failed to receive file stream");
                return;
            }

            // restore the permissions of the destination on the temporary file
            if(fchmod(file_fd, dest_mode) < 0) {
                int saved = errno;
                close(file_fd);
                unlink(temp_path);
                send_err(clientSocket, saved, "Failed to set permissions on uploaded file");
                return;
            }

            if(close(file_fd) < 0) {
                int saved = errno;
                unlink(temp_path);
                send_err(clientSocket, saved, "Failed to close temporary file");
                return;
            }

            // update final dest
            if(rename(temp_path, full_path) < 0) {
                int saved = errno;
                unlink(temp_path);
                send_err(clientSocket, saved, "Failed to install uploaded file");
                return;
            }

            // unlock

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
                const char *reason = "Path outside of allowed scope";
                send_err(clientSocket, EINVAL, reason);
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
                file_fd = open(full_path, O_WRONLY | O_CREAT, 0700);
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

            // set lock

            // tell client to start sending data
            const char *reason = "Ready to receive data";
            send_ok(clientSocket, reason, strlen(reason));

            // start receiving stream
            uint8_t data_code = CMD_WRITE_DATA;
            uint8_t end_code = CMD_WRITE_END;

            ssize_t stream_result = recv_stream(clientSocket, file_fd, offset, -1, data_code, end_code, error_msg, err_size);
            if(stream_result < 0) {
                close(file_fd);
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
                    close(file_fd);
                    unlink(temp_path);
                    send_err(clientSocket, saved, "Failed to set permissions on written file");
                    return;
                }
                if(close(file_fd) < 0) {
                    int saved = errno;
                    unlink(temp_path);
                    send_err(clientSocket, saved, "Failed to close temporary file");
                    return;
                }
                if(rename(temp_path, full_path) < 0) {
                    int saved = errno;
                    unlink(temp_path);
                    send_err(clientSocket, saved, "Failed to install written file");
                    return;
                }
            } else {
                close(file_fd);
            }

            // release lock

            // send confirmation with N bytes written
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
                const char *reason = "Path outside of allowed scope";
                send_err(clientSocket, EINVAL, reason);
                return;
            }

            int delete_result = delete_cmd(full_path);
            if(delete_result < 0) {
                const char *reason = "Failed to delete file/directory";
                send_err(clientSocket, -delete_result, reason);
                return;
            }
            const char *reason = "File/Directory deleted successfully";
            send_ok(clientSocket, reason, strlen(reason));

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
                const char *reason = "Directory is empty";
                send_ok(clientSocket, reason, strlen(reason));
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
            ssize_t n = read(session.notify_fd, notify_buffer, sizeof(notify_buffer)-1);
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