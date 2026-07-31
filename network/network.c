/* network.c

Responsible for:
- send_all/recv_all/recv_line;
- protocol framing.
*/

#include"network.h"

/*
send_data

Function used to send data through the stream socket.
Returns 0 on success, -1 on error.
*/
int send_data(int sockfd, const void *buf, size_t len){
    const char *ptr = buf; 
    while(len > 0){
        int n = send(sockfd, ptr, len, 0);
        if(n<0){
            if(errno == EINTR) // if interrupted by signal, retry
                continue;
            return -1;
        }
        ptr += n;
        len -= n;
    }
    return 0;
}

/*
recv_data

Function used to receive data through the stream socket.
Returns 0 on success, -1 on error.
*/
int receive_data(int sockfd, void *buf, size_t len){
    char *ptr = buf;

    while(len > 0){
        int n = recv(sockfd, ptr, len, 0);
        if(n<=0){
            if(errno == EINTR) // if interrupted by signal, retry
                continue;
            return -1;
        }
        ptr += n;
        len -= n;
    }
    return 0;
}

/* Packet format (client -> server)
| preamble (x2 Byte) | command (x1 Byte)| len of payload (x4 Byte) | payload (xLen Bytes) |

Packet format (server -> client)
| preamble (x2 Byte) | status (x1 Byte, OK, ERR)| len of payload (x4 Byte) | payload (xLen Bytes, either payload for OK, or err_code+ payload)|
*/

int build_packet(char *buf, uint8_t command, const char *payload, uint32_t payload_len){
    uint16_t preamble = PREAMBLE;
    memcpy(buf, &preamble, sizeof(preamble));
    buf += sizeof(preamble);
    memcpy(buf, &command, sizeof(command));
    buf += sizeof(command);
    memcpy(buf, &payload_len, sizeof(payload_len));
    buf += sizeof(payload_len);
    memcpy(buf, payload, payload_len);
    return 0;
}

int send_all(int sockfd, const void *buf, size_t len, uint8_t command, const char *payload, uint32_t payload_len){
    char *packet = malloc(sizeof(uint16_t) + sizeof(uint8_t) + sizeof(uint32_t) + payload_len);
    if(packet == NULL){
        free(packet);
        return -1;
    }
    build_packet(packet, command, payload, payload_len);
    int ret = send_data(sockfd, packet, sizeof(uint16_t) + sizeof(uint8_t) + sizeof(uint32_t) + payload_len);
    free(packet);
    return ret;
}

int recv_all(int sockfd, char **buf, uint8_t *status, uint32_t *payload_len){
    uint16_t preamble;
    if(receive_data(sockfd, &preamble, sizeof(preamble)) < 0){
        return -1;
    }
    if(preamble != PREAMBLE){
        return -1;
    }
    if(receive_data(sockfd, status, sizeof(*status)) < 0){
        return -1;
    }
    if(receive_data(sockfd, payload_len, sizeof(*payload_len)) < 0){
        return -1;
    }
    *buf = malloc(*payload_len);
    if(*buf == NULL){
        return -1;
    }
    if(receive_data(sockfd, *buf, *payload_len) < 0){
        free(*buf);
        return -1;
    }
    return 0;
}