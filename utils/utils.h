/* utils.h */
#ifndef UTILS_H
#define UTILS_H

#include<string.h>
#include<stdlib.h>
#include<sys/types.h>
#include<limits.h>
#include<errno.h>
#include<unistd.h>
#include<stdio.h>

#define DEBUG 0

// This struct is used to handle user session, each child will have its own
typedef struct {
    int logged_in; // 1 if logged, 0 otherwise
    char user[32]; // username of logged-in user
    uid_t uid; // saved user id of the logged-in user, used in the server
    char home_path[PATH_MAX]; // home directory of the logged-in user
    char root_path[PATH_MAX]; // root of the server
    int notify_fd; // file desc. for a pipe, used for handling transfer_requests between sessions
} session_t;

typedef struct {
    char *data; // pointer to the string data
    size_t len; // current length of the string 
    size_t cap; // capacity of the buffer (allocated size)
} strbuf_t; 

int sb_init(strbuf_t *sb);

// puts n bytes in the buf
int sb_append(strbuf_t *sb, const char *s, size_t n);

// free memory allocated for the buffer
void sb_free(strbuf_t *sb);

extern gid_t server_gid; // global variable to hold the server's group ID

int validate_permissions(long *perms, char *buf);

int write_all(int fd, const void *buf, ssize_t count);

int open_temp_for_upload(const char *dest_path, char *temp_path_out, size_t out_size);

ssize_t read_line(int fd, char *buf, size_t size);

#endif