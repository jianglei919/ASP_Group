#ifndef SESSION_H
#define SESSION_H

#include "config.h"

typedef struct session_ctx
{
    int client_fd;
    node_role_t role;
    int request_count;
    char server_root[4096];
} session_ctx_t;

int session_run(session_ctx_t *session);

#endif
