/* utils.c 

Responsible for:
- octal parse;
- wrapper;
*/

#include"utils.h"

int sb_init(strbuf_t *sb) {
    sb->cap = 1024; // initial capacity
    sb->len = 0; // initial length of the string
    sb->data = malloc(sb->cap); // allocate memory for the string
    if (sb->data) {
        sb->data[0] = '\0';
    } else {
        return -ENOMEM; // ENOMEM
    }
    return 0;
}

int sb_append(strbuf_t *sb, const char *s, size_t n) {
    if (!sb || !s) return -1; // if the strbuf_t pointer or the string pointer is NULL, return error

    if (!sb->data) { // if the buffer is not initialized, initialize it
        int i;
        i = sb_init(sb);
        if(i < 0) return -1; // ENOMEM
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

int validate_permissions(long *perms, char *buf){
    char *end;
    errno = 0;

    if(buf == NULL || buf[0] == '\0') {
        return -1; // empty string is invalid
    }

    long val = strtol(buf, &end, 8);
    if(errno != 0 || *end != '\0' || val < 0 || val > 0777) {
        return -1;
    }
    *perms = val;
    return 0;
}

/*
Function used to handle writing of large files, it ensures that all bytes are written to the file descriptor, 
even if write() is interrupted or writes fewer bytes than requested.
*/
int write_all(int fd, const void *buf, ssize_t n){
    // current pointer in the file
    const char *ptr = buf;
    // remaining bytes to write
    ssize_t remaining = n;

    while(remaining > 0){
        ssize_t written = write(fd, ptr, remaining);
        if(written < 0){
            if(errno == EINTR) continue; // interrupted by signal, retry
            return -errno; // error
        }
        ptr += written;
        remaining -= written;
    }

    return 0; // success
}

// create a temporary file in the same directory as dest_path, return its fd
int open_temp_for_upload(const char *dest_path, char *temp_path_out, size_t out_size){
    if(dest_path == NULL || temp_path_out == NULL || out_size == 0){
        return -EINVAL; // the caller must provide a buffer to receive the temporary name
    }

    const char *dir_end = strrchr(dest_path, '/');
    if(dir_end == NULL){
        return -EINVAL; // invalid path
    }

    size_t dir_len = dir_end - dest_path + 1; // +1 for the '/' character
    char *dir_path = malloc(dir_len + 1);
    if(dir_path == NULL){
        return -ENOMEM;
    }
    strncpy(dir_path, dest_path, dir_len);
    dir_path[dir_len] = '\0';

    char temp_template[PATH_MAX];
    snprintf(temp_template, sizeof(temp_template), "%s.tempXXXXXX", dir_path);
    free(dir_path);

    // verify that the generated temp_template fits in temp_path_out
    if(strlen(temp_template) >= out_size){
        return -ENAMETOOLONG;
    }
    strncpy(temp_path_out, temp_template, out_size);

    int temp_fd = mkstemp(temp_path_out);
    if(temp_fd < 0){
        return -errno;
    }

    return temp_fd;
}

/*
Function used to read one line from stdin or socket, byte by byte.
*/
ssize_t read_line(int fd, char *buf, size_t size, int *eof_flag) {
    size_t i = 0;
    if(eof_flag) *eof_flag = 0; // initialize EOF flag to 0
    while(i + 1 < size){
        char c;
        ssize_t n = read(fd, &c, 1);
        if(n < 0){
            if(errno == EINTR) continue; // interrupted by a signal, retry
            return -1;
        }
        if(n == 0) {
            if(i==0 && eof_flag) *eof_flag = 1; // set EOF flag
            break;
        }    // EOF
        if(c == '\n') break; // end of the command, the newline is consumed
        buf[i++] = c;
    }
    buf[i] = '\0';
    return (ssize_t)i;
}