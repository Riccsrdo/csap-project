/* transfer.h */
#ifndef TRANSFER_H
#define TRANSFER_H

#include<netinet/in.h> // Address information
#include"../network/network.h" // fix with makefile -I
#include <stdint.h>
#include <sys/types.h>

typedef struct {
    int sockfd;
    struct sockaddr_in address;
} peer_t;

ssize_t send_stream(int sockfd, int fd, off_t offset, uint8_t data_code, uint8_t end_code);
ssize_t recv_stream(int sockfd, int fd, off_t offset, int64_t expected_total, uint8_t data_code, uint8_t end_code, char *error_msg, size_t err_size);


#endif