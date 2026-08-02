/* protocol.c

Responsible for:
- Parsing of commands and options;
- response serialization;
*/

#include"protocol.h"

// useful to associate command names with their codes, for parsing and serialization
static const struct 
{ 
    const char *name; 
    uint8_t code; 
} CMDS[] = {
    {"login", CMD_LOGIN}, {"list", CMD_LIST}, {"cd", CMD_CD}, {"read", CMD_READ},
    {"exit", CMD_EXIT}
};

// parse a command line into a cmd_t structure
// parse -b as background operation
// -d as directory option with create
// -offset=N for read and write operations
int parse_command(const char *line, cmd_t *command) {
    if(line == NULL || command == NULL){
        return -1;
    }

    command->offset = -1;

    char *copy = strdup(line);
    if(copy == NULL){
        return -1; // memory allocation failed
    }

    char *saveptr;
    char *token = strtok_r(copy, " ", &saveptr);
    if(token == NULL){
        free(copy);
        return -1; // empty command
    }

    if(strcmp(token, "login") == 0) {
        command->code = CMD_LOGIN;
    } else if(strcmp(token, "list") == 0) {
        command->code = CMD_LIST;
    } else if(strcmp(token, "cd") == 0) {
        command->code = CMD_CD;
    } else if(strcmp(token, "read") == 0) {
        command->code = CMD_READ;
    } else if(strcmp(token, "exit") == 0) {
        command->code = CMD_EXIT;
    } else {
        free(copy);
        return -1; // unknown command
    }

    int arg_count = 0;
    while((token = strtok_r(NULL, " ", &saveptr)) != NULL && arg_count < MAX_ARGS) {
        
        // It's an option
        if(token[0] == '-') {
            if(strcmp(token, "-b") == 0) {
                command->is_background = 1;
            } else if(strcmp(token, "-d") == 0) {
                command->is_dir = 1;
            } else if(strncmp(token, "-offset=", 8) == 0) {
                // check if the offset is a valid positive number
                if (sscanf(token + 8, "%ld", &command->offset) != 1) {
                    free(copy);
                    return -1; // error in number format
                }
            } else {
                free(copy);
                return -1; // unknown option
            }
        } else { // it's a positional argument
            if(arg_count == 0){
                strncpy(command->buf, token, PATH_MAX - 1);
                command->buf[PATH_MAX - 1] = '\0';
            } else if(arg_count == 1){
                strncpy(command->buf2, token, PATH_MAX - 1);
                command->buf2[PATH_MAX - 1] = '\0';
            } else {
                free(copy);
                return -1; // too many positional arguments
            }
            arg_count++;
        }
    
    }

    // final check for correct number of parameters passed
    

    command->argc = arg_count;
    free(copy);
    return 0; // success

}
