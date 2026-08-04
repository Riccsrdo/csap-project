/* protocol.h */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include<limits.h>
#include<stdlib.h>
#include<stdio.h>
#include"../network/network.h" // fix with makefile
//#include"network.h"

#define MAX_ARGS 8

typedef struct{
    uint8_t code;  //command
    int is_background; // 1 if background operation, 0 otherwise
    int is_dir; // 1 if directory, 0 if file
    long offset; // offset for read/write operations, -1 if not applicable
    int argc; // number of arguments
    char buf[2 * PATH_MAX]; // buffer for data
    char buf2[2 * PATH_MAX]; // additional buffer for data, if needed
} cmd_t; // reminder for me: copy cmd_t using pointer



int parse_command(const char *line, cmd_t *command);

#endif