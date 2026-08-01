/* protocol.c

Responsible for:
- Parsing of commands and options;
- response serialization;
*/

#include"protocol.h"

// useful to associate command names with their codes, for parsing and serialization
static const struct 
{ 
    const char *name; 
    uint8_t code; 
} CMDS[] = {
    {"login", CMD_LOGIN}, {"list", CMD_LIST}, {"cd", CMD_CD}, {"read", CMD_READ}
};
