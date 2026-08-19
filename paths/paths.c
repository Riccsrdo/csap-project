/* paths.c 

Responsible for:
- Canonicalization of paths;
- Sandbox validation;
*/
#include"paths.h"


/* To validate the scope of user operations,
call validate_path and look for the result

if != 0, the operation is outside the allowed scope, and should be rejected
*/
int validate_path(const char *path, const char *root, char *validated_path) {

    if(validated_path == NULL || path == NULL || root == NULL) {
        return -EINVAL; // invalid input
    }

    validated_path[0] = '\0';

    char nroot[PATH_MAX];
    strncpy(nroot, root, PATH_MAX - 1);
    nroot[PATH_MAX - 1] = '\0'; 

    size_t root_len = strlen(nroot);

    while(root_len > 0 && nroot[root_len - 1] == '/') {
        nroot[root_len - 1] = '\0';
        root_len--;
    } // remove trailing slashes for the correct funcionality of validate_path

    // first try to validate assuming file already exists
    if(realpath(path, validated_path) != NULL) {
        if(strncmp(validated_path, nroot, root_len) != 0 || (validated_path[root_len] != '/' && validated_path[root_len] != '\0')) {
            validated_path[0] = '\0';
            return -EPERM; // path is outside the root directory
        }
        return 0; // path is valid and within the root directory
    }

    // check if the file does not exist, but parent directory does
    if(errno != ENOENT) {
        // reset validated_path to empty string, avoiding any potential misuse of uninitialized data with errors
        int saved = errno; // save errno
        validated_path[0] = '\0';
        return -saved; 
    }

    // the file might not exist, so we check if the parent directory is valid (useful for create operations)
    char *path_copy = strdup(path);
    char *path_copy2 = strdup(path);

    if(path_copy == NULL || path_copy2 == NULL) {
        if(path_copy) {
            free(path_copy);
        }
        if(path_copy2) {
            free(path_copy2);
        }
        validated_path[0] = '\0';
        return -ENOMEM; // memory allocation failed
    }

    char *parent_dir = dirname(path_copy);
    char *base_name = basename(path_copy2);

    char resolved_parent[PATH_MAX];
    if(realpath(parent_dir, resolved_parent) == NULL) {
        int saved = errno;
        free(path_copy);
        free(path_copy2);
        validated_path[0] = '\0';
        return -saved; // parent directory does not exist or cannot be resolved
    }

    // if we're here, resolved_parent exists, so we validate it against the root
    if(strncmp(resolved_parent, nroot, root_len) != 0 || (resolved_parent[root_len] != '/' && resolved_parent[root_len] != '\0')) {
        free(path_copy);
        free(path_copy2);
        validated_path[0] = '\0';
        return -EPERM; // parent directory is outside the root directory
    }


    // reconstruct the full path
    int result = snprintf(validated_path, PATH_MAX, "%s%s%s",
        resolved_parent,
        (strcmp(resolved_parent, "/") == 0) ? "" : "/",
        base_name
    );

    if(result < 0 || result >= PATH_MAX) {
        free(path_copy);
        free(path_copy2);
        validated_path[0] = '\0';
        return -ENAMETOOLONG; // snprintf error or path too long
    }

    free(path_copy);
    free(path_copy2);
    return 0; // path is valid and within the root directory
}