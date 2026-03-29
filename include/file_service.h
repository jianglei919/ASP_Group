#ifndef FILE_SERVICE_H
#define FILE_SERVICE_H

#include "command_dispatch.h"

typedef struct file_query_result
{
    int found;
    char file_path[4096];
    char description[1024];
} file_query_result_t;

int file_service_handle_dirlist_a(const char *root, char *out, int out_size);
int file_service_handle_dirlist_t(const char *root, char *out, int out_size);
int file_service_handle_fn(const char *root, const char *filename, file_query_result_t *result);
int file_service_collect_matches(const char *root, const command_request_t *request, char *list_path, int list_path_size);

#endif
