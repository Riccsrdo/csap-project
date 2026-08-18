/* transfer.c

Responsible for:
- upload;
- download;
- transfer_request/accept/reject;
*/
#include"transfer.h"

/*
Used to perform the copy of a file from src_path to dest_path, both absolute paths.
Returns 0 on success, -errno on failure.
Acquires a read lock on the source file and a write lock on the destination 
Releases the locks after the operation is complete.
*/
int execute_transfer_copy(const char *src_path, const char *dest_path){
    int src_fd = -1;
    int dest_fd = -1;

    struct stat src_stat;

    // temporarily elevate privileges to open source file contained in another virtual file system
    uid_t original_euid = geteuid();
    if(seteuid(0) < 0){
        return -errno;
    }

    src_fd = open(src_path, O_RDONLY | O_NOFOLLOW); // O_NOFOLLOW to prevent following symlinks
    int open_errno = errno; // capture errno in case of failure

    // restore original privileges
    if(seteuid(original_euid) < 0){
        int saved_errno = errno; // close may overwrite errno, so save it
        if(src_fd >= 0){
            close(src_fd);
        }
        return -saved_errno;
    }

    // check if source file was opened successfully
    if(src_fd < 0){
        return -open_errno; // return the error code from open
    }

    // check if source file is a regular file
    if(fstat(src_fd, &src_stat) < 0){
        int saved_errno = errno;
        close(src_fd);
        return -saved_errno;
    }

    if(!S_ISREG(src_stat.st_mode)){
        close(src_fd);
        return -EINVAL; // source is not a regular file
    }

    // open destination file for writing
    // if the destination file already exists, I don't overwrite it
    dest_fd = open(dest_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if(dest_fd < 0){
        close(src_fd);
        return -errno;
    }

    // acquire read lock on source file
    int r = acquire_read_lock(src_fd, 0, 0);
    if(r < 0){
        close(src_fd);
        close(dest_fd);
        return r; 
    }

    // acquire write lock on destination file
    r = acquire_write_lock(dest_fd, 0, 0);
    if(r < 0){
        release_lock(src_fd, 0, 0);
        close(src_fd);
        close(dest_fd);
        return r; 
    }

    // perform the copy
    char buffer[4096];
    ssize_t bytes_read;
    while(1){
        bytes_read = read(src_fd, buffer, sizeof(buffer));
        if(bytes_read < 0){
            if(errno == EINTR) continue; 
            break;
        }
        ssize_t bytes_written = write_all(dest_fd, buffer, bytes_read);
        if(bytes_written < 0){
            release_lock(src_fd, 0, 0);
            release_lock(dest_fd, 0, 0);
            close(src_fd);
            close(dest_fd);

            // remove dest file as the copy failed
            unlink(dest_path);
            return bytes_written; //err code
        }

    }

    // release locks and close file descriptors
    int read_errno = (bytes_read < 0) ? errno : 0; // capture read error if any
    release_lock(src_fd, 0, 0);
    release_lock(dest_fd, 0, 0);
    close(src_fd);
    close(dest_fd);

    if(read_errno != 0){
        // remove the destination file if the copy failed due to a read error
        unlink(dest_path); // as I use the O_EXCL flag, removing is safe as the file has been created here
        return -read_errno;
    } else {
        return 0; // success
    }



}

/*
similar to send_stream, but the source is a buffer in memory instead of a file descriptor.
Returns the number of bytes sent on success, or a negative error code on failure.
*/
ssize_t send_stream_buf(int sockfd, const char *buf, size_t len, uint8_t data_code, uint8_t end_code){
    if(buf == NULL && len > 0){
        return -EINVAL;
    }

    size_t sent = 0;
    while(sent < len){
        size_t chunk = (len - sent > CHUNK_SIZE) ? CHUNK_SIZE : (len - sent);
        if(send_packet(sockfd, data_code, buf + sent, (uint32_t)chunk) < 0){
            return -EIO;
        }
        sent += chunk;
    }

    // final frame with end_code and no payload to indicate the end of the stream
    if(send_packet(sockfd, end_code, NULL, 0) < 0){
        return -EIO;
    }

    return (ssize_t)sent;
}

ssize_t send_stream(int sockfd, int fd, off_t offset, uint8_t data_code, uint8_t end_code){
    char buffer[HDR_SIZE + CHUNK_SIZE];

    // if offset > 0, set lseek to offset
    if(offset > 0){ // SEEK_SET (from the beginning of the file), move pointer to offset
        off_t returned_offset = lseek(fd, offset, SEEK_SET);
        if(returned_offset == (off_t)-1){
            return -errno;
        }
    }

    ssize_t total_bytes_sent = 0;

    while(1){
        ssize_t bytes_read = read(fd, buffer + HDR_SIZE, CHUNK_SIZE);
        if(bytes_read < 0){
            if(errno == EINTR) continue; // if interrupted by signal, retry
            return -errno;
        }
        if(bytes_read == 0){
            // EOF reached
            break;
        }

        // send the data frame
        int ret = send_packet(sockfd, data_code, buffer + HDR_SIZE, (uint32_t)bytes_read);

        if(ret < 0){
            return -EIO;
        }
        total_bytes_sent += bytes_read;
    }

    // send a frame with end_code and no payload to indicate the end of the stream
    int ret = send_packet(sockfd, end_code, NULL, 0);
    if(ret < 0){
        return -EIO;
    }

    return total_bytes_sent;
}


/* if destination of stream is stdout, pass offset<0
if error, send_err to client with RSP_ERR
*/
ssize_t recv_stream(int sockfd, int fd, off_t offset, int64_t expected_total, uint8_t data_code, uint8_t end_code, char *error_msg, size_t err_size){
    // buffer for CHUNK
    char buffer[CHUNK_SIZE];

    if(offset > 0){ // SEEK_SET (from the beginning of the file), move pointer to offset
        off_t returned_offset = lseek(fd, offset, SEEK_SET);
        if(returned_offset == (off_t)-1){
            return -errno;
        }
    }

    ssize_t total_bytes_received = 0;

    int write_err = 0; // first error returned by write_all(), 0 if none

    while(1){
        uint8_t status;
        uint32_t payload_len;

        int ret = recv_frame_into(sockfd, buffer, CHUNK_SIZE, &status, &payload_len);
        if(ret < 0){
            return ret;
        }

        if(status == end_code){
            // end of stream
            break;
        }

        if(status == RSP_ERR){
            // failure in the middle of the stream, return error
            if(error_msg){
                snprintf(error_msg, err_size, "Error received from peer");
            }
            return -EIO;
        }

        if(status != data_code){
            return -EINVAL; // unexpected status code
        }

        // write the received chunk to the file descriptor
        if(write_err == 0){
            int r = write_all(fd, buffer, payload_len);
            if(r < 0){
                // if an error occurs during writing, store the error code and continue to read the rest of the stream to avoid leaving the socket in an inconsistent state
                write_err = r;
                if(error_msg){
                    snprintf(error_msg, err_size, "Error writing to file descriptor: %s", strerror(-r));
                }
            }
        }

        total_bytes_received += payload_len;

    }

    if(write_err != 0){
        return write_err; // if an error occurred during writing, return the first error encountered
    }

    if(expected_total>=0 && expected_total!=total_bytes_received){
        return -EINVAL; // total bytes received does not match expected total
    }

    return total_bytes_received;
}
