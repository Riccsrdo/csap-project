/* protocol.h */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include<limits.h>
#include"../network/network.h" // da fixare con makefile, -I

#define MAX_ARGS 8

typedef struct{
    uint8_t code;  //command
    int is_background; // 1 if background operation, 0 otherwise
    int is_dir; // 1 if directory, 0 if file
    long offset; // offset for read/write operations, -1 if not applicable
    int argc; // number of arguments
    char *argv[MAX_ARGS]; // array of argument strings
    char buf[2 * PATH_MAX]; // buffer for additional data
} cmd_t; // reminder for me: copy cmd_t using pointer



int parse_command(const char *line, cmd_t *command);

// useful to associate command names with their codes, for parsing and serialization
static const struct 
{ 
    const char *name; 
    uint8_t code; 
} CMDS[] = {
    {"login", CMD_LOGIN}, {"list", CMD_LIST}, {"cd", CMD_CD}, {"read", CMD_READ},
    {"exit", CMD_EXIT}
};
#endif