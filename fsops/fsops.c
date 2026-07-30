/* fsops.c

Responsible for:
- create, chmod, move, delete, cd, list ✅, read, write.
*/
#include<stdio.h> // for printf()
#include<stdlib.h> // for exit()
#include <unistd.h> // for POSIX functions
#include <sys/types.h> // for data types for system calls: time_t, off_t, mode_t
#include <sys/stat.h> // for struct stat
#include <fcntl.h> // for opening files and controls
#include <time.h> // for time functions
#include<dirent.h> // for opening and managing directories
#include<string.h> // for string manipulation functions


// -----------------------------------------------------------------------------------------------------
// ----------------------------------------- list() ----------------------------------------------------
// -----------------------------------------------------------------------------------------------------

// Construct file info, given a path and a stat struct, and append it to the output buffer
void constructFileInfo(char *out_buffer, size_t buffer_size, struct stat *fileStat, const char *filename) {
    size_t current_len = strlen(out_buffer);
    
    if (current_len >= buffer_size) {
        return;
    }

    // build the permission string
    char perms[11];
    perms[0] = (S_ISDIR(fileStat->st_mode)) ? 'd' : '-';
    perms[1] = (fileStat->st_mode & S_IRUSR) ? 'r' : '-';
    perms[2] = (fileStat->st_mode & S_IWUSR) ? 'w' : '-';
    perms[3] = (fileStat->st_mode & S_IXUSR) ? 'x' : '-';
    perms[4] = (fileStat->st_mode & S_IRGRP) ? 'r' : '-';
    perms[5] = (fileStat->st_mode & S_IWGRP) ? 'w' : '-';
    perms[6] = (fileStat->st_mode & S_IXGRP) ? 'x' : '-';
    perms[7] = (fileStat->st_mode & S_IROTH) ? 'r' : '-';
    perms[8] = (fileStat->st_mode & S_IWOTH) ? 'w' : '-';
    perms[9] = (fileStat->st_mode & S_IXOTH) ? 'x' : '-';
    perms[10] = '\0';

    // format time string
    char *time_str = ctime(&fileStat->st_mtime);
    char time_buf[64] = "";
    if (time_str) {
        strncpy(time_buf, time_str, sizeof(time_buf) - 1);
        size_t t_len = strlen(time_buf);
        if (t_len > 0 && time_buf[t_len - 1] == '\n') {
            time_buf[t_len - 1] = '\0';
        }
    }

    // append remaining details to the output buffer
    int written = snprintf(out_buffer + current_len, buffer_size - current_len,
                           "%s %u %u %lu %lld %s %s\n",
                           perms,
                           (unsigned int)fileStat->st_uid,
                           (unsigned int)fileStat->st_gid,
                           (unsigned long)fileStat->st_nlink,
                           (long long)fileStat->st_size,
                           time_buf,
                           filename);

    if (written < 0 || (size_t)written >= buffer_size - current_len) {
        // error
        return;
    }
}


/*
list() function:
- list the files in a directory;
- prints permissions and logical size;
- takes an optional path argument; if no path is given, lists the current working directory; path can be a single file or directory;
- allow navigation to other users' home directories.

- fills a buffer with the list info
- returns 0 on success, -1 on error
*/
int list(char *path, char *out_buffer, size_t buffer_size) {

    struct stat fileStat; // struct to hold file information

    // if no path is given, use current working directory
    if(path == NULL){
        path = ".";
    }

    // open path with lstat to get file information
    if(lstat(path, &fileStat) < 0){
        perror("lstat");
        return -1;
    }

    // check if path is a directory or a file
    if(S_ISDIR(fileStat.st_mode)){
        // if path is a directory, open it
        DIR *dir = opendir(path);
        if(dir == NULL){
            perror("opendir");
            return -1;
        }

        struct dirent *entry; // struct to hold directory entry information

        // read entries in the directory
        while((entry = readdir(dir)) != NULL){
            // skip "." and ".." entries
            if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0){
                continue;
            }
            // create full path for the entry
            char fullPath[1024];
            snprintf(fullPath, sizeof(fullPath), "%s/%s", path, entry->d_name);
            // get file information for the entry
            if(lstat(fullPath, &fileStat) < 0){
                perror("lstat");
                return -1;
            }
            // construct buffer with file information
            constructFileInfo(out_buffer, buffer_size, &fileStat, entry->d_name);

        }
        closedir(dir); // close the directory
    } else {
        // if path is a file, print its information
        constructFileInfo(out_buffer, buffer_size, &fileStat, path);
    }

    return 0; // success

}
