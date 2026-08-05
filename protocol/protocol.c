/* protocol.c

Responsible for:
- Parsing of commands and options;
- response serialization;
*/

#include"protocol.h"


// parse a command line into a cmd_t structure
// parse -b as background operation
// -d as directory option with create
// -offset=N for read and write operations
int parse_command(const char *line, cmd_t *command, char *error_msg, uint32_t err_size) {
    if(line == NULL || command == NULL){
        snprintf(error_msg, err_size, "Invalid arguments to parse_command");
        return -1;
    }

    command->offset = -1;
    command->is_background = 0;
    command->is_dir = 0;
    command->argc = 0;
    command->buf[0] = '\0';
    command->buf2[0] = '\0';

    char *copy = strdup(line);
    if(copy == NULL){
        snprintf(error_msg, err_size, "Failed to duplicate command line");
        return -1; // memory allocation failed
    }

    char *saveptr;
    char *token = strtok_r(copy, " ", &saveptr);
    if(token == NULL){
        free(copy);
        snprintf(error_msg, err_size, "Empty command");
        return -1; // empty command
    }

    // assign, given string, the corresponding command code
    if(strcmp(token, "create_user") == 0) {
        command->code = CMD_CREATE_USER;
    } else if(strcmp(token, "login") == 0) {
        command->code = CMD_LOGIN;
    } else if(strcmp(token, "list") == 0) {
        command->code = CMD_LIST;
    } else if(strcmp(token, "cd") == 0) {
        command->code = CMD_CD;
    } else if(strcmp(token, "read") == 0) {
        command->code = CMD_READ;
    } else if(strcmp(token, "exit") == 0) {
        command->code = CMD_EXIT;   
    } else if(strcmp(token, "create") == 0) {
        command->code = CMD_CREATE;
    } else if(strcmp(token, "chmod") == 0) {
        command->code = CMD_CHMOD;
    } else if(strcmp(token, "move") == 0) {
        command->code = CMD_MOVE;
    } else if(strcmp(token, "delete") == 0) {
        command->code = CMD_DELETE;
    } else if(strcmp(token, "write") == 0) {
        command->code = CMD_WRITE;
    } else if(strcmp(token, "upload") == 0) {
        command->code = CMD_UPLOAD_BEGIN;
    } else if(strcmp(token, "download") == 0) {
        command->code = CMD_DOWNLOAD_BEGIN;
    } else if(strcmp(token, "transfer_request") == 0) {
        command->code = CMD_TRANSFER_REQ;
    } else if(strcmp(token, "accept") == 0) {
        command->code = CMD_ACCEPT;
    } else if(strcmp(token, "reject") == 0) {
        command->code = CMD_REJECT;
    } else {
        free(copy);
        snprintf(error_msg, err_size, "Unknown command");
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
                    snprintf(error_msg, err_size, "Invalid offset format");
                    return -1; // error in number format
                }
                // if offset is negative, return error
                if (command->offset < 0) {
                    command->offset = -1; // reset to default
                    free(copy);
                    snprintf(error_msg, err_size, "Offset cannot be negative");
                    return -1;
                }
            } else {
                free(copy);
                snprintf(error_msg, err_size, "Unknown option");
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
                snprintf(error_msg, err_size, "Too many positional arguments");
                return -1; // too many positional arguments
            }
            arg_count++;
        }
    
    }

    // final check for correct number of parameters passed
    if(command->code == CMD_CREATE && arg_count!=2) {
        free(copy);
        snprintf(error_msg, err_size, "Invalid number of arguments for create command.\nSyntax: create <path> <permissions>\n");
        return -1;
        return -1;
    }
    if(command->code == CMD_READ && arg_count!=1) {
        snprintf(error_msg, err_size, "Invalid number of arguments for read command.\nSyntax: read <path>.\n");
        free(copy);
        return -1; 
    }
    if(command->code == CMD_CHMOD && arg_count!=2) {
        snprintf(error_msg, err_size, "Invalid number of arguments for chmod command.\nSyntax: chmod <path> <permissions>.\n");
        free(copy);
        return -1; 
    }
    if(command->code == CMD_MOVE && arg_count!=2) {
        snprintf(error_msg, err_size, "Invalid number of arguments for move command.\nSyntax: move <old_path> <new_path>.\n");
        free(copy);
        return -1; 
    }
    if(command->code == CMD_DELETE && arg_count!=1) {
        snprintf(error_msg, err_size, "Invalid number of arguments for delete command.\nSyntax: delete <path>.\n");
        free(copy);
        return -1;
    }
    if(command->code == CMD_WRITE && arg_count!=1) {
        snprintf(error_msg, err_size, "Invalid number of arguments for write command.\nSyntax: write <path>.\n");
        free(copy);
        return -1; 
    }
    if(command->code == CMD_UPLOAD_BEGIN && arg_count!=2) {
        snprintf(error_msg, err_size, "Invalid number of arguments for upload command.\nSyntax: upload <client_path> <server_path>.\n");
        free(copy);
        return -1; 
    }
    if(command->code == CMD_DOWNLOAD_BEGIN && arg_count!=2) {
        snprintf(error_msg, err_size, "Invalid number of arguments for download command.\nSyntax: download <server_path> <client_path>.\n");
        free(copy);
        return -1; 
    }
    if(command->code == CMD_TRANSFER_REQ && arg_count!=2) {
        snprintf(error_msg, err_size, "Invalid number of arguments for transfer request command.\nSyntax: transfer_request <file> <dest_user>.\n");
        free(copy);
        return -1; 
    }
    if(command->code == CMD_ACCEPT && arg_count!=2) {
        snprintf(error_msg, err_size, "Invalid number of arguments for accept command.\nSyntax: accept <directory> <id>.\n");
        free(copy);
        return -1; 
    }
    // reject does not require any argument
    if(command->code == CMD_CD && arg_count!=1) {
        snprintf(error_msg, err_size, "Invalid number of arguments for cd command.\nSyntax: cd <path>.\n");
        free(copy);
        return -1; 
    }
    // list can be called without any argument, so no check is needed
    if (command->code == CMD_CREATE_USER && arg_count != 2) { 
        snprintf(error_msg, err_size, "Invalid number of arguments for create_user command.\nSyntax: create_user <username> <permissions>.\n");
        free(copy); return -1; 
    }
    if (command->code == CMD_LOGIN       && arg_count != 1) { 
        snprintf(error_msg, err_size, "Invalid number of arguments for login command.\nSyntax: login <username>.\n");
        free(copy); return -1; 
    }



    command->argc = arg_count;
    free(copy);
    return 0; // success

}
