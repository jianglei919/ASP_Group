#include <stdio.h>
#include <string.h>

#define PRIMARY_HOST "127.0.0.1"
#define PRIMARY_PORT 5000
#define MAX_COMMAND_LEN 512

static int connect_to_server(const char *host, int port)
{
    (void)host;
    (void)port;
    /* TODO: create socket and connect() to target server. */
    return -1;
}

static int validate_command(const char *line, char *err, size_t err_size)
{
    if (line == NULL || err == NULL || err_size == 0)
    {
        return -1;
    }

    if (line[0] == '\0')
    {
        snprintf(err, err_size, "empty command");
        return -1;
    }

    /* TODO: enforce command grammar for dirlist/fn/fz/ft/fdb/fda/quitc. */
    err[0] = '\0';
    return 0;
}

static int send_command(int server_fd, const char *line)
{
    (void)server_fd;
    (void)line;
    /* TODO: send encoded request frame. */
    return 0;
}

static int receive_response(int server_fd)
{
    (void)server_fd;
    /* TODO: print text response or save temp.tar.gz to ~/project. */
    return 0;
}

int main(void)
{
    int server_fd;
    char line[MAX_COMMAND_LEN];
    char err[128];

    server_fd = connect_to_server(PRIMARY_HOST, PRIMARY_PORT);
    if (server_fd < 0)
    {
        fprintf(stderr, "client: cannot connect to %s:%d (skeleton)\n", PRIMARY_HOST, PRIMARY_PORT);
        return 1;
    }

    while (fgets(line, sizeof(line), stdin) != NULL)
    {
        line[strcspn(line, "\n")] = '\0';

        if (validate_command(line, err, sizeof(err)) != 0)
        {
            fprintf(stderr, "Invalid command: %s\n", err);
            continue;
        }

        if (send_command(server_fd, line) != 0)
        {
            fprintf(stderr, "client: send failed\n");
            break;
        }

        if (receive_response(server_fd) != 0)
        {
            fprintf(stderr, "client: receive failed\n");
            break;
        }

        if (strcmp(line, "quitc") == 0)
        {
            break;
        }
    }

    return 0;
}
