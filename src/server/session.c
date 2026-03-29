#include "session.h"

#include "command_dispatch.h"
#include "logging.h"

int session_run(session_ctx_t *session)
{
    command_request_t request;

    if (session == 0)
    {
        return -1;
    }

    log_info("Session started for role=%d", (int)session->role);

    /*
     * TODO:
     * 1) Receive command line from client
     * 2) Parse command
     * 3) Dispatch to command handlers
     * 4) Exit loop on quitc
     */

    request.type = CMD_INVALID;
    request.raw[0] = '\0';
    (void)dispatch_command(session, &request);

    log_info("Session ended");
    return 0;
}
