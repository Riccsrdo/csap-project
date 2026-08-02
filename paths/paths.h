/* paths.h */
#ifndef PATHS_H
#define PATHS_H


#include<limits.h>
#include<stdlib.h>
#include<string.h>
#include<libgen.h>
#include<errno.h>
#include<stdio.h>

int validate_path(const char *path, const char *root, char *validated_path);

int check_user_scope(const char *path, const char *root);

#endif