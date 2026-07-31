/* fsops.c

Responsible for:
- create, chmod, move, delete, cd, list ✅, read, write.
*/
#include"fsops.h"


// -----------------------------------------------------------------------------------------------------
// ----------------------------------------- list() ----------------------------------------------------
// -----------------------------------------------------------------------------------------------------

// Construct file info, given a path and a stat struct, and append it to the output buffer
int constructFileInfo(strbuf_t *sb, struct stat *fileStat, const char *filename) {
    // build the permission string
    char perms[11];
    switch(fileStat->st_mode & S_IFMT){
        case S_IFREG: perms[0] = '-'; break;
        case S_IFDIR: perms[0] = 'd'; break;
        case S_IFLNK: perms[0] = 'l'; break;
        case S_IFIFO: perms[0] = 'p'; break;
        case S_IFSOCK: perms[0] = 's'; break;
        case S_IFCHR: perms[0] = 'c'; break;
        case S_IFBLK: perms[0] = 'b'; break;
        default: perms[0] = '?'; break;
    }
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

    char line[1024]; // temporary buffer to hold the formatted line

    // append remaining details to the output buffer
    int written = snprintf(line, sizeof(line),
                           "%s %u %u %lu %lld %s %s\n",
                           perms,
                           (unsigned int)fileStat->st_uid,
                           (unsigned int)fileStat->st_gid,
                           (unsigned long)fileStat->st_nlink,
                           (long long)fileStat->st_size,
                           time_buf,
                           filename);

    if (written < 0 || written >= sizeof(line)) {
        // error
        return -1;
    }

    // Append string using sb_append()
    if(sb_append(sb, line, strlen(line)) < 0) {
        return -ENOMEM; // memory allocation error
    }

    
}


/*
list() function:
- list the files in a directory;
- prints permissions and logical size;
- takes an optional path argument; if no path is given, lists the current working directory; path can be a single file or directory;
- allow navigation to other users' home directories.

- fills a custom str_buffer with the list info
- returns 0 on success, -1 on error
*/
int list(char *path, strbuf_t *sb) {

    if(sb == NULL) {
        return -EINVAL; // invalid argument
    }

    if(sb->data == NULL) {
        sb_init(sb);
        if(sb->data == NULL) {
            return -ENOMEM; // memory allocation error[cite: 1]
        }
    }

    struct stat fileStat; // struct to hold file information

    // if no path is given, use current working directory
    if(path == NULL){
        path = ".";
    }

    // open path with lstat to get file information
    if(lstat(path, &fileStat) < 0){
        return -errno;
    }

    // check if path is a directory or a file
    if(S_ISDIR(fileStat.st_mode)){
        // if path is a directory, open it
        DIR *dir = opendir(path);
        if(dir == NULL){
            return -errno;
        }

        struct dirent *entry; // struct to hold directory entry information

        // read entries in the directory
        while((entry = readdir(dir)) != NULL){
            // skip "." and ".." entries
            if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0){
                continue;
            }
            // create full path for the entry
            char fullPath[PATH_MAX];
            snprintf(fullPath, sizeof(fullPath), "%s/%s", path, entry->d_name);
            // get file information for the entry
            if(lstat(fullPath, &fileStat) < 0){
                continue; // skip this entry on error
            }
            // construct buffer with file information
            switch(constructFileInfo(sb, &fileStat, entry->d_name)){
                case -ENOMEM:
                    closedir(dir);
                    return -ENOMEM; 
                case -1:
                    closedir(dir);
                    return -1; 
                default:
                    break; 
            }

        }
        closedir(dir); // close the directory
    } else {
        // if path is a file, print its information
        switch(constructFileInfo(sb, &fileStat, path)){
            case -ENOMEM:
                return -ENOMEM; 
            case -EINVAL:
                return -EINVAL; 
            case -1:
                return -1; 
            default:
                break; 
        }
    }

    return 0; // success

}
