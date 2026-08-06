/* transfer.c

Responsible for:
- upload;
- download;
- transfer_request/accept/reject;
*/
#include"transfer.h"

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
