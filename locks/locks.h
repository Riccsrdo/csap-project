/* locks.h */
#ifndef LOCKS_H
#define LOCKS_H

#include<fcntl.h> // fcntl
#include<unistd.h> // close
#include<stdio.h>
#include<errno.h>

int acquire_read_lock(int fd, off_t offset, off_t len);
int acquire_write_lock(int fd, off_t offset, off_t len);
int release_lock(int fd, off_t offset, off_t len);

#endif