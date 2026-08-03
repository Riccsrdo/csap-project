/* fsops.h */
#ifndef FSOPS_H
#define FSOPS_H

#include<stdio.h> // for printf()
#include<stdlib.h> // for exit()
#include <unistd.h> // for POSIX functions
#include <sys/types.h> // for data types for system calls: time_t, off_t, mode_t
#include <sys/stat.h> // for struct stat
#include <fcntl.h> // for opening files and controls
#include <time.h> // for time functions
#include<dirent.h> // for opening and managing directories
#include<string.h> // for string manipulation functions
#include<errno.h> // for error handling
//#include"utils.h" 
#include"../utils/utils.h" // fix with makefile -I

int list(char *path, strbuf_t *sb);


#endif