/* utils.c 

Responsible for:
- logging;
- error management;
- octal parse;
- wrapper;
*/

#include"utils.h"

void sb_init(strbuf_t *sb) {
    sb->cap = 1024; // initial capacity
    sb->len = 0; // initial length of the string
    sb->data = malloc(sb->cap); // allocate memory for the string
    if (sb->data) {
        sb->data[0] = '\0';
    } else {
        return; // ENOMEM
    }
}

int sb_append(strbuf_t *sb, const char *s, size_t n) {
    if (!sb || !s) return -1; // if the strbuf_t pointer or the string pointer is NULL, return error

    if (!sb->data) { // if the buffer is not initialized, initialize it
        sb_init(sb);
        if (!sb->data) return -1; // ENOMEM
    }
    
    // check if additional space is necessary
    if (sb->len + n + 1 > sb->cap) { // if the new required length exceeds the current capacity...
        size_t new_cap = sb->cap > 0 ? sb->cap * 2 : 1024; // start with 1024 if cap is 0, else double
        
        // if not enough, keep doubling
        while (sb->len + n + 1 > new_cap) {
            new_cap *= 2;
        }
        
        char *new_data = realloc(sb->data, new_cap); // reallocate necessary space
        if (!new_data) {
            return -1; // ENOMEM
        }
        
        sb->data = new_data;
        sb->cap = new_cap;
    }
    
    // copy the new string into the buffer and update length
    memcpy(sb->data + sb->len, s, n);
    sb->len += n; // update current length of the string
    sb->data[sb->len] = '\0'; //null-terminate the string
    
    return 0;
}

void sb_free(strbuf_t *sb) {
    if (sb) {
        free(sb->data);
        sb->data = NULL;
        sb->len = 0;
        sb->cap = 0;
    }
}