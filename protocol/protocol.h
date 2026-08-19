/* protocol.h */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include<limits.h>
#include<stdlib.h>
#include<stdio.h>
#include"../network/network.h"
#include<errno.h>

#define MAX_ARGS 8

typedef struct{
    uint8_t code;  //command
    int is_background; // 1 if background operation, 0 otherwise
    int is_dir; // 1 if directory, 0 if file
    long offset; // offset for read/write operations, -1 if not applicable
    int argc; // number of arguments
    char buf[PATH_MAX]; // buffer for data
    char buf2[PATH_MAX]; // additional buffer for data, if needed
} cmd_t;



int parse_command(const char *line, cmd_t *command, char *error_msg, uint32_t err_size);

#endif