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
        ssize_t n = send(sockfd, ptr, len, 0);
        if(n < 0){
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
        ssize_t n = recv(sockfd, ptr, len, 0);
        if (n == 0) { // EOF, connection closed by the peer
            return -1;
        }
        if(n < 0){
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
    // write preamble in big-endian
    uint16_t preamble = htons(PREAMBLE);
    memcpy(buf, &preamble, sizeof(preamble));
    buf += sizeof(preamble);
    memcpy(buf, &command, sizeof(command));
    buf += sizeof(command);
    uint32_t payload_len_network = htonl(payload_len); // similarly convert payload_len to network byte order
    memcpy(buf, &payload_len_network, sizeof(payload_len_network));
    buf += sizeof(payload_len_network);
    if(payload_len > 0 && payload != NULL)
        memcpy(buf, payload, payload_len);
    return 0;
}

int send_packet(int sockfd, uint8_t command, const char *payload, uint32_t payload_len){
    // check payload_len is not too large
    if(payload_len > MAX_PAYLOAD_SIZE){
        return -1;
    }
    char *packet = malloc(sizeof(uint16_t) + sizeof(uint8_t) + sizeof(uint32_t) + payload_len);
    if(packet == NULL){
        return -1;
    }
    build_packet(packet, command, payload, payload_len);
    int ret = send_data(sockfd, packet, sizeof(uint16_t) + sizeof(uint8_t) + sizeof(uint32_t) + payload_len);
    free(packet);
    return ret;
}

int recv_header(int sockfd, uint8_t *command, uint32_t *payload_len){
    uint16_t preamble;
    if(receive_data(sockfd, &preamble, sizeof(preamble)) < 0){
        return -1;
    }
    preamble = ntohs(preamble); // convert from network to host byte order
    if(preamble != PREAMBLE){
        return -1;
    }
    if(receive_data(sockfd, command, sizeof(*command)) < 0){
        return -1;
    }
    if(receive_data(sockfd, payload_len, sizeof(*payload_len)) < 0){
        return -1;
    }
    *payload_len = ntohl(*payload_len); // convert payload_len from network to host byte order
    if(*payload_len > MAX_PAYLOAD_SIZE){ // check if payload_len is corrupted or too large
        return -1;
    }
    return 0;
}

int recv_packet(int sockfd, char **buf, uint8_t *status, uint32_t *payload_len){
    if(recv_header(sockfd, status, payload_len) < 0){
        return -1;
    }
    if(*payload_len > 0){
        *buf = malloc(*payload_len + 1); // +1 for null terminator
        if(*buf == NULL){
            return -1;
        }
        if(receive_data(sockfd, *buf, *payload_len) < 0){
            free(*buf);
            return -1;
        }
        (*buf)[*payload_len] = '\0'; // null terminate the payload
    } else {
        *buf = NULL; // no payload
    }
    return 0;
}

int send_ok(int fd, const char *payload, uint32_t payload_len){
    return send_packet(fd, RSP_OK, payload, payload_len);
}

int send_err(int fd, int err_code, const char *payload){ 
    // prepend err_code to the payload
    if (!payload) payload = "";
    int need = snprintf(NULL, 0, "%d %s", err_code, payload);
    char *buf = malloc(need + 1);
    if (!buf) return -1;
    snprintf(buf, need + 1, "%d %s", err_code, payload);
    int ret = send_packet(fd, RSP_ERR, buf, need); 
    free(buf);
    return ret;
}

int recv_frame_into(int sockfd, void *buf, size_t buf_cap, uint8_t *status, uint32_t *payload_len){
    if(recv_header(sockfd, status, payload_len) < 0){
        return -1;
    }

    if(*payload_len > buf_cap){
        return -1; // buffer too small
    }

    if(*payload_len > 0){
        if(receive_data(sockfd, buf, *payload_len) < 0){
            return -1;
        }
    }
    return 0;

}