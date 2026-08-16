/* network.h

*/
#ifndef NETWORK_H
#define NETWORK_H
#include<netinet/in.h> // Address information
#include<stdio.h>
#include<stdlib.h>
#include<sys/socket.h> // Socket APIs
#include<sys/types.h>
#include<arpa/inet.h> // inet_pton
#include<string.h>
#include<unistd.h>
#include<fcntl.h> 
#include<errno.h>
#include<stdint.h>
#include"../utils/utils.h" // fix with makefile -I


#define MAX_PAYLOAD_SIZE (64 * 1024) // 64KB
#define CHUNK_SIZE (8 * 1024) // 8KB
#define HDR_SIZE (2 + 1 + 4) // preamble (2 bytes) + command/status (1 byte) + payload length (4 bytes)
#define PREAMBLE 0xABCD

// Commands for Client to Server communication
enum { CMD_LOGIN = 1, CMD_CREATE_USER, CMD_LIST, CMD_CD, CMD_CREATE, CMD_CHMOD,
       CMD_MOVE, CMD_DELETE, CMD_READ, CMD_WRITE,
       CMD_UPLOAD_BEGIN, CMD_DOWNLOAD_BEGIN, CMD_DATA, CMD_DATA_END,
       CMD_TRANSFER_REQ, CMD_ACCEPT, CMD_REJECT, CMD_EXIT, CMD_READ_DATA, CMD_READ_END, CMD_WRITE_DATA, CMD_WRITE_END,
       CMD_LIST_DATA, CMD_LIST_END};

// Answer codes for Server to Client communication
enum { RSP_OK = 100, RSP_ERR, RSP_DATA, RSP_DATA_END, RSP_NOTIFY };

       
int send_data(int sockfd, const void *buf, size_t len);
int receive_data(int sockfd, void *buf, size_t len);
int build_packet(char *buf, uint8_t command, const char *payload, uint32_t payload_len);
int send_packet(int sockfd, uint8_t command, const char *payload, uint32_t payload_len);

// Caller is responsible for freeing the buffer allocated by recv_packet
int recv_packet(int sockfd, char **buf, uint8_t *status, uint32_t *payload_len);

// Used by the server to notify about the status of a command, with an optional payload (e.g. error message)

int send_ok(int fd, const char *payload, uint32_t payload_len);
int send_err(int fd, int err_code, const char *payload);

int recv_frame_into(int sockfd, void *buf, size_t buf_cap, uint8_t *status, uint32_t *payload_len);

#endif