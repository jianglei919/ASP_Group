#include "file_service.h"

#include <stdio.h>
#include <string.h>

int file_service_handle_dirlist_a(const char *root, char *out, int out_size)
{
    (void)root;
    if (out == 0 || out_size <= 0)
    {
        return -1;
    }
    snprintf(out, (size_t)out_size, "TODO: dirlist -a not implemented yet");
    return 0;
}

int file_service_handle_dirlist_t(const char *root, char *out, int out_size)
{
    (void)root;
    if (out == 0 || out_size <= 0)
    {
        return -1;
    }
    snprintf(out, (size_t)out_size, "TODO: dirlist -t not implemented yet");
    return 0;
}

int file_service_handle_fn(const char *root, const char *filename, file_query_result_t *result)
{
    (void)root;
    (void)filename;
    if (result == 0)
    {
        return -1;
    }
    memset(result, 0, sizeof(*result));
    /* TODO: locate first matching file and populate metadata text. */
    return 0;
}

int file_service_collect_matches(const char *root, const command_request_t *request, char *list_path, int list_path_size)
{
    (void)root;
    (void)request;
    if (list_path == 0 || list_path_size <= 0)
    {
        return -1;
    }
    snprintf(list_path, (size_t)list_path_size, "/tmp/w26_files.list");
    /* TODO: generate temp list file consumed by archive builder. */
    return 0;
}
