/* transfer.c

Responsible for:
- upload;
- download;
- transfer_request/accept/reject;
*/
#include<netinet/in.h> // Address information

typedef struct {
    int sockfd;
    struct sockaddr_in address;
} peer_t;
