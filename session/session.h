/* session.h */
#include<stdint.h>
#include<stdio.h>
#include<stdlib.h>
#include"../utils/utils.h"
#include <unistd.h>
#include<sys/stat.h>
#include <fcntl.h>
#include<pwd.h>
#include<ctype.h>
#include<grp.h>
#include<sys/wait.h>

int handle_login(session_t *session, char *username, char *err_msg, uint32_t err_size);

int handle_create_user(const char* username, mode_t perms, const char *root, char *err_msg, uint32_t err_size);