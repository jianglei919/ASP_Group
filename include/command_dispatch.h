#ifndef COMMAND_DISPATCH_H
#define COMMAND_DISPATCH_H

#include "session.h"

typedef enum command_type
{
    CMD_DIRLIST_A,
    CMD_DIRLIST_T,
    CMD_FN,
    CMD_FZ,
    CMD_FT,
    CMD_FDB,
    CMD_FDA,
    CMD_QUITC,
    CMD_INVALID
} command_type_t;

typedef struct command_request
{
    command_type_t type;
    char raw[512];
    char arg1[256];
    char arg2[256];
    char arg3[256];
} command_request_t;

int parse_command_line(const char *line, command_request_t *request);
int dispatch_command(session_ctx_t *session, const command_request_t *request);

#endif
