/* fsops.h */
#ifndef FSOPS_H
#define FSOPS_H

#include<stdio.h> 
#include<stdlib.h> 
#include <unistd.h> 
#include <sys/types.h> 
#include <sys/stat.h> 
#include <fcntl.h> 
#include <time.h> 
#include<dirent.h> 
#include<string.h> 
#include<errno.h> 
#include"../utils/utils.h"
#include<pwd.h>
#include<grp.h>
#include<libgen.h>

int list(char *path, strbuf_t *sb);

int create_cmd(const char *path, mode_t perms, int is_dir);

int chmod_cmd(const char *path, mode_t perms);

int move_cmd(const char *src, const char *dest);

int cd_cmd(const char *path);

int delete_cmd(const char *path);


#endif