#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NODE_NAME "mirror2"
#define DEFAULT_PORT 5002
#define BACKLOG 16
#define MAX_COMMAND_LEN 512

typedef struct server_config
{
    const char *bind_host;
    int bind_port;
} server_config_t;

static int create_listen_socket(const server_config_t *cfg)
{
    (void)cfg;
    /* TODO: socket() + bind() + listen() */
    return -1;
}

static int read_command_line(int client_fd, char *buf, size_t size)
{
    (void)client_fd;
    (void)buf;
    (void)size;
    /* TODO: read one command from client */
    return -1;
}

static int process_command(int client_fd, const char *cmd)
{
    (void)client_fd;
    (void)cmd;
    /* TODO: implement dirlist/fn/fz/ft/fdb/fda handling */
    return 0;
}

static void crequest(int client_fd)
{
    char cmd[MAX_COMMAND_LEN];

    for (;;)
    {
        int rc = read_command_line(client_fd, cmd, sizeof(cmd));
        if (rc <= 0)
        {
            break;
        }

        if (strcmp(cmd, "quitc") == 0)
        {
            break;
        }

        if (process_command(client_fd, cmd) != 0)
        {
            break;
        }
    }
}

static int run_server(const server_config_t *cfg)
{
    int listen_fd = create_listen_socket(cfg);
    (void)crequest;
    if (listen_fd < 0)
    {
        fprintf(stderr, "%s: failed to create listen socket (skeleton)\n", NODE_NAME);
        return 1;
    }

    /*
     * TODO:
     * 1) accept loop
     * 2) fork per connection
     * 3) child calls crequest(client_fd)
     * 4) parent continues listening
     */

    close(listen_fd);
    return 0;
}

int main(void)
{
    server_config_t cfg;
    cfg.bind_host = "0.0.0.0";
    cfg.bind_port = DEFAULT_PORT;

    printf("%s starting on port %d (skeleton)\n", NODE_NAME, cfg.bind_port);
    return run_server(&cfg);
}
