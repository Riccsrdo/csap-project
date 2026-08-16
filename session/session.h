/* session.h */
#ifndef SESSION_H
#define SESSION_H
#include<stdint.h>
#include<stdio.h>
#include<stdlib.h>
#include<signal.h>
#include"../utils/utils.h"
#include <unistd.h>
#include<sys/stat.h>
#include <fcntl.h>
#include<pwd.h>
#include<ctype.h>
#include<grp.h>
#include<sys/wait.h>
#include"../paths/paths.h"

int setup_server_gid(char *error_msg, uint32_t err_size);

int is_csap_user(const struct passwd *pwd);

int create_home_directory(const char *root, const char *username, char *err_msg, 
    uint32_t err_size, mode_t perms, struct passwd *new_pwd);

int handle_login(session_t *session, char *username, char *err_msg, uint32_t err_size);

int handle_create_user(const char* username, mode_t perms, const char *root, char *err_msg, uint32_t err_size);

#endif