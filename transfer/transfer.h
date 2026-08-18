/* transfer.h */
#ifndef TRANSFER_H
#define TRANSFER_H

#include<netinet/in.h> // Address information
#include"../network/network.h" // fix with makefile -I
#include <stdint.h>
#include <sys/types.h>
#include"../locks/locks.h"
#include<sys/stat.h>
#include<limits.h>
#include"../utils/utils.h"

typedef struct {
    int sockfd;
    struct sockaddr_in address;
} peer_t;

/*
Struct used to describe target for a stream.
With offset < 0, write file from scratch, with temp file and rename.
*/
typedef struct{
    const char *path; // path to the file
    off_t offset; // offset to write to, if < 0, write from scratch
    uint8_t data_code; // code to use for data packets
    uint8_t end_code; // code to use for end packet
} target_t;

ssize_t receive_into_file(int sockfd, const target_t *target, char *error_msg, size_t err_size);
ssize_t send_file(int sockfd, const char *path, off_t offset, uint8_t data_code, uint8_t end_code, char *error_msg, size_t err_size);

ssize_t send_stream(int sockfd, int fd, off_t offset, uint8_t data_code, uint8_t end_code);
ssize_t recv_stream(int sockfd, int fd, off_t offset, int64_t expected_total, uint8_t data_code, uint8_t end_code, char *error_msg, size_t err_size);

int execute_transfer_copy(const char *source_path, const char *dest_path);

ssize_t send_stream_buf(int sockfd, const char *buf, size_t len, uint8_t data_code, uint8_t end_code);

#endif