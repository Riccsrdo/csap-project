/* locks.c 

Responsible for:
- Wrapping on fcntl locks;
*/
#include"locks.h"

/*
helper function to set a lock on a file descriptor using fcntl.
Takes as parameters:
- fd: the file descriptor on which to set the lock
- type: the type of lock (F_RDLCK, F_WRLCK, F_UNLCK)
- cmd: the command to perform (F_SETLK, F_SETLKW), the former is non-blocking, the latter is blocking
- offset: the starting offset for the lock. With no offset, the value passed must be 0.
- len: the length of the locked region, if len is 0, the lock extends to the end of the file.
Returns 0 on success, or a negative error code on failure.
*/
static int set_lock(int fd, int type, int cmd, off_t offset, off_t len) {
    struct flock lock = {0}; 
    lock.l_type = type; 
    lock.l_whence = SEEK_SET; 
    lock.l_start = offset;
    lock.l_len = len;

    if(fcntl(fd, cmd, &lock) == -1) {
        return -errno; // return negative error code
    }
    return 0;
}

int acquire_read_lock(int fd, off_t offset, off_t len) {
    return set_lock(fd, F_RDLCK, F_SETLK, offset, len); // non blocking read lock
}

int acquire_write_lock(int fd, off_t offset, off_t len) {
    return set_lock(fd, F_WRLCK, F_SETLK, offset, len); // non blocking write lock
}

int release_lock(int fd, off_t offset, off_t len) {
    return set_lock(fd, F_UNLCK, F_SETLK, offset, len); // non blocking unlock
}