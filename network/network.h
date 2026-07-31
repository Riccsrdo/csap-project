/* network.h

*/
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

#define PREAMBLE 0xABCD

int send_data(int sockfd, const void *buf, size_t len);
int receive_data(int sockfd, void *buf, size_t len);
int build_packet(char *buf, uint8_t command, const char *payload, uint32_t payload_len);