#include "server_runtime.h"

#include "logging.h"
#include "network.h"
#include "session.h"

int server_runtime_start(const app_config_t *config)
{
    int server_fd;

    if (config == 0)
    {
        return -1;
    }

    log_info("Starting server role=%d on %s:%d", (int)config->role, config->bind_host, config->bind_port);

    server_fd = net_create_server_socket(config->bind_host, config->bind_port);
    if (server_fd < 0)
    {
        log_error("Failed to create server socket");
        return -1;
    }

    /*
     * TODO:
     * 1) Loop on accept()
     * 2) Fork child per client
     * 3) In child, initialize session_ctx_t and call session_run()
     * 4) Parent closes client fd and continues
     */

    net_close_socket(server_fd);
    return 0;
}
