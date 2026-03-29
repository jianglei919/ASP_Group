#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 镜像服务端2基础配置（骨架版） */
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
    /* TODO: 使用 socket() / bind() / listen() 创建监听套接字 */
    return -1;
}

static int read_command_line(int client_fd, char *buf, size_t size)
{
    (void)client_fd;
    (void)buf;
    (void)size;
    /* TODO: 从客户端连接读取一条完整命令 */
    return -1;
}

static int process_command(int client_fd, const char *cmd)
{
    (void)client_fd;
    (void)cmd;
    /* TODO: 实现 dirlist/fn/fz/ft/fdb/fda 命令处理 */
    return 0;
}

static void crequest(int client_fd)
{
    /* 每个客户端连接由一个子进程独占处理 */
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
    /* 仅为骨架阶段避免未使用函数告警 */
    (void)crequest;
    if (listen_fd < 0)
    {
        fprintf(stderr, "%s: failed to create listen socket (skeleton)\n", NODE_NAME);
        return 1;
    }

    /*
     * TODO:
     * 1) 实现 accept 循环
     * 2) 每个连接 fork 一个子进程
     * 3) 子进程调用 crequest(client_fd)
     * 4) 父进程继续监听新连接
     */

    close(listen_fd);
    return 0;
}

int main(void)
{
    server_config_t cfg;
    /* 骨架默认监听所有网卡 */
    cfg.bind_host = "0.0.0.0";
    cfg.bind_port = DEFAULT_PORT;

    printf("%s starting on port %d (skeleton)\n", NODE_NAME, cfg.bind_port);
    return run_server(&cfg);
}
