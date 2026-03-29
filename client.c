#include <stdio.h>
#include <string.h>

/* 客户端默认连接主服务端 */
#define PRIMARY_HOST "127.0.0.1"
#define PRIMARY_PORT 5000
#define MAX_COMMAND_LEN 512

static int connect_to_server(const char *host, int port)
{
    (void)host;
    (void)port;
    /* TODO: 创建套接字并 connect() 到目标服务端 */
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

    /* TODO: 按作业要求校验 dirlist/fn/fz/ft/fdb/fda/quitc 语法 */
    err[0] = '\0';
    return 0;
}

static int send_command(int server_fd, const char *line)
{
    (void)server_fd;
    (void)line;
    /* TODO: 按协议编码并发送请求帧 */
    return 0;
}

static int receive_response(int server_fd)
{
    (void)server_fd;
    /* TODO: 接收响应，文本打印或将 temp.tar.gz 保存到 ~/project */
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

    /* 交互式命令循环：读入、校验、发送、接收 */
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
