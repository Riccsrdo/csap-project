/* protocol.c

Responsible for:
- Parsing of commands and options;
- response serialization;
*/

#include"protocol.h"

/*
Commands table
*/
static const struct {
    const char *name;
    uint8_t code;
    int argc; // numb of arguments expected (-1 if variable)
    const char *syntax; // correct syntax for the command
} CMDS[] =  {
    { "create", CMD_CREATE, 2, "create [-d] <path> <permissions>" },
    { "chmod", CMD_CHMOD, 2, "chmod <path> <permissions>" },
    { "move", CMD_MOVE, 2, "move <old_path> <new_path>" },
    { "delete", CMD_DELETE, 1, "delete <path>" },
    { "cd", CMD_CD, 1, "cd <path>" },
    { "read", CMD_READ, 1, "read [-offset=N] <path>" },
    { "write", CMD_WRITE, 1, "write [-offset=N] <path>" },
    { "list", CMD_LIST, -1, "list [<path>]" },
    { "login", CMD_LOGIN, 1, "login <username>" },
    { "create_user", CMD_CREATE_USER, 2, "create_user <username> <permissions>" },
    { "upload", CMD_UPLOAD_BEGIN, 2, "upload [-b] <client_path> <server_path>" },
    { "download", CMD_DOWNLOAD_BEGIN, 2, "download [-b] <server_path> <client_path>" },
    { "transfer_request", CMD_TRANSFER_REQ, 2, "transfer_request <file> <dest_user>" },
    { "accept", CMD_ACCEPT, 2, "accept <directory> <id>" },
    { "reject", CMD_REJECT, 1, "reject <id>" },
    { "exit", CMD_EXIT, -1, "exit" }
};
#define NUM_CMDS (sizeof(CMDS) / sizeof(CMDS[0])) 


// parse a command line into a cmd_t structure
// parse -b as background operation
// -d as directory option with create
// -offset=N for read and write operations
int parse_command(const char *line, cmd_t *command, char *error_msg, uint32_t err_size) {
    char *copy = NULL;
    size_t idx = NUM_CMDS; // default to invalid command

    if(line == NULL || command == NULL){
        snprintf(error_msg, err_size, "Invalid arguments to parse_command");
        return -1;
    }

    command->offset = -1; // default 
    command->is_background = 0;
    command->is_dir = 0;
    command->argc = 0;
    command->buf[0] = '\0';
    command->buf2[0] = '\0';

    // make a copy of the line to tokenize
    copy = strdup(line);
    if(copy == NULL){
        snprintf(error_msg, err_size, "Memory allocation failed");
        return -1;
    }

    char *saveptr;
    char *token = strtok_r(copy, " ", &saveptr); // get the first token (command name)
    if(token == NULL){
        snprintf(error_msg, err_size, "Empty command line");
        free(copy);
        return -1;
    }

    // obtain code starting from the command name
    for(size_t i = 0; i < NUM_CMDS; i++){
        if(strcmp(token, CMDS[i].name) == 0){
            idx = i;
            break;
            command->code = CMDS[i].code;
        }
    }

    if(idx == NUM_CMDS){
        snprintf(error_msg, err_size, "Unknown command: %s", token);
        free(copy);
        return -1;
    }

    int arg_count = 0;
    while((token = strtok_r(NULL, " ", &saveptr)) != NULL && arg_count < MAX_ARGS){ // extract arguments 
        // option
        if(token[0] == '-'){

            if(strcmp(token, "-b") == 0){
                command->is_background = 1;
            } else if(strcmp(token, "-d") == 0){
                command->is_dir = 1;
            } else if(strncmp(token, "-offset=", 8) == 0){
                char *endptr;
                errno = 0;
                long offset = strtol(token + 8, &endptr, 10); // convert the offset value to a long integer
                if(errno!=0 || endptr == token + 8 || *endptr != '\0'){
                    command->offset = -1; // reset offset
                    snprintf(error_msg, err_size, "Invalid offset value");
                    free(copy);
                    return -1;
                }

                if(offset < 0){
                    command->offset = -1; // reset offset
                    snprintf(error_msg, err_size, "Offset cannot be negative");
                    free(copy);
                    return -1;
                }

                command->offset = offset;

            } else {
                snprintf(error_msg, err_size, "Unknown option: %s", token);
                free(copy);
                return -1;
            }
        } else { // argument
            if(arg_count == 0){
                snprintf(command->buf, PATH_MAX, "%s", token);
            } else if(arg_count == 1){
                snprintf(command->buf2, PATH_MAX, "%s", token);
            } else {
                snprintf(error_msg, err_size, "Too many arguments for command: %s", CMDS[idx].name);
                free(copy);
                return -1;
            }
            arg_count++;

        }
    }

    // check if argument count matches expected count
    if(CMDS[idx].argc >= 0 && arg_count != CMDS[idx].argc){
        snprintf(error_msg, err_size, "Incorrect number of arguments for command: %s. Syntax: %s", CMDS[idx].name, CMDS[idx].syntax);
        free(copy);
        return -1;
    }

    command->argc = arg_count;
    command->code = CMDS[idx].code;
    free(copy);
    return 0;
}
