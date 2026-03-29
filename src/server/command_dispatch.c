#include "command_dispatch.h"

#include <string.h>

#include "archive_service.h"
#include "file_service.h"
#include "logging.h"

int parse_command_line(const char *line, command_request_t *request)
{
    if (line == 0 || request == 0)
    {
        return -1;
    }

    memset(request, 0, sizeof(*request));
    request->type = CMD_INVALID;
    strncpy(request->raw, line, sizeof(request->raw) - 1);

    /* TODO: tokenize and parse command according to project grammar. */
    return 0;
}

int dispatch_command(session_ctx_t *session, const command_request_t *request)
{
    char list_path[4096] = {0};

    if (session == 0 || request == 0)
    {
        return -1;
    }

    switch (request->type)
    {
    case CMD_DIRLIST_A:
    case CMD_DIRLIST_T:
    case CMD_FN:
    case CMD_FZ:
    case CMD_FT:
    case CMD_FDB:
    case CMD_FDA:
        /* TODO: map each command to dedicated handler and response path. */
        (void)file_service_collect_matches(session->server_root, request, list_path, (int)sizeof(list_path));
        break;
    case CMD_QUITC:
        /* TODO: send acknowledgement before ending session. */
        break;
    default:
        log_warn("Invalid command received: %s", request->raw);
        break;
    }

    return 0;
}
