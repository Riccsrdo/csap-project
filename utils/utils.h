/* utils.h */
#ifndef UTILS_H
#define UTILS_H

#include<string.h>
#include<stdlib.h>

typedef struct {
    char *data; // pointer to the string data
    size_t len; // current length of the string 
    size_t cap; // capacity of the buffer (allocated size)
} strbuf_t; 

void sb_init(strbuf_t *sb);

// puts n bytes in the buf
int sb_append(strbuf_t *sb, const char *s, size_t n);

// free memory allocated for the buffer
void sb_free(strbuf_t *sb);

#endif