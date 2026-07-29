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

/*
Utility function for list,
takes path and stat struct, prints file information in "ls -l" format.
*/
void printFileInfo(char *path, struct stat *fileStat){
    printf((S_ISDIR(fileStat->st_mode)) ? "d" : "-");
    printf((fileStat->st_mode & S_IRUSR) ? "r" : "-");
    printf((fileStat->st_mode & S_IWUSR) ? "w" : "-");
    printf((fileStat->st_mode & S_IXUSR) ? "x" : "-");
    printf((fileStat->st_mode & S_IRGRP) ? "r" : "-");
    printf((fileStat->st_mode & S_IWGRP) ? "w" : "-");
    printf((fileStat->st_mode & S_IXGRP) ? "x" : "-");
    printf((fileStat->st_mode & S_IROTH) ? "r" : "-");
    printf((fileStat->st_mode & S_IWOTH) ? "w" : "-");
    printf((fileStat->st_mode & S_IXOTH) ? "x" : "-");
    // owner name
    printf(" %d", fileStat->st_uid);
    // group name
    printf(" %d", fileStat->st_gid);
    printf(" %ld", fileStat->st_nlink);
    printf(" %lld", (long long)fileStat->st_size);
    printf(" %s", ctime(&fileStat->st_mtime));  
    printf(" %s\n", path);
}

/*
list() function:
- list the files in a directory;
- prints permissions and logical size;
- takes an optional path argument; if no path is given, lists the current working directory; path can be a single file or directory;
- allow navigation to other users' home directories.
*/
void list(char *path){

    int fd; // file descriptor

    struct stat fileStat; // struct to hold file information

    // if no path is given, use current working directory
    if(path == NULL){
        path = ".";
    }

    if((fd = open(path, O_RDONLY)) < 0){
        perror("open");
        exit(EXIT_FAILURE);
    }

    if((fstat(fd, &fileStat))<0){
        perror("fstat");
        exit(EXIT_FAILURE);
    }

    // The path can either be
    // - a single file, in which case we print its information
    // - a directory, in which case we list its contents, printing the information for each file
    if(S_ISDIR(fileStat.st_mode)){
        DIR *dir;
        struct dirent *entry;

        if((dir = opendir(path)) == NULL){
            perror("opendir");
            exit(EXIT_FAILURE);
        }

        while((entry = readdir(dir)) != NULL){
            if(strncmp(entry->d_name, ".", 1) == 0 || strncmp(entry->d_name, "..", 2) == 0){
                continue; // skip
            }
            char *filePath = malloc(strlen(path) + strlen(entry->d_name) + 2); // +2 for '/' and '\0'
            sprintf(filePath, "%s/%s", path, entry->d_name); // construct full path

            if(stat(filePath, &fileStat) < 0){
                perror("stat");
                free(filePath);
                continue;
            }

            printFileInfo(filePath, &fileStat);
            free(filePath);
        }

        closedir(dir);
    } else {
        printFileInfo(path, &fileStat);
    }

    if(close(fd)) perror("close");
}
