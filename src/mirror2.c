#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>

/* 镜像服务端2基础配置 */
#define NODE_NAME "mirror2"
#define DEFAULT_PORT 5002
#define PRIMARY_HOST "127.0.0.1"
#define PRIMARY_PORT 5000
#define HEARTBEAT_INTERVAL_SEC 2
#define BACKLOG 16
#define MAX_COMMAND_LEN 512

typedef struct server_config
{
    const char *bind_host;
    int bind_port;
} server_config_t;

/*
 * 功能：可靠发送指定长度的数据。
 * 实现原理：循环调用 send，处理 EINTR 中断，直到所有字节发送完成。
 */
static int send_all(int fd, const char *data, size_t len)
{
    /* 循环发送，避免一次 send 未发完导致数据丢失 */
    size_t sent = 0;

    while (sent < len)
    {
        ssize_t n = send(fd, data + sent, len - sent, 0);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -1;
        }
        if (n == 0)
        {
            return -1;
        }
        sent += (size_t)n;
    }

    return 0;
}

/*
 * 功能：按行接收文本响应。
 * 实现原理：逐字节读取直到换行，用于心跳应答消费。
 */
static int recv_line(int fd, char *buf, size_t size)
{
    size_t idx = 0;

    if (buf == NULL || size == 0)
    {
        return -1;
    }

    while (idx + 1 < size)
    {
        char ch;
        ssize_t n = recv(fd, &ch, 1, 0);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -1;
        }
        if (n == 0)
        {
            break;
        }
        if (ch == '\n')
        {
            break;
        }
        if (ch != '\r')
        {
            buf[idx++] = ch;
        }
    }

    buf[idx] = '\0';
    return (idx > 0) ? 0 : -1;
}

/*
 * 功能：向主服务端发送一次心跳。
 * 实现原理：短连接发送 HEARTBEAT mirror2，并读取一行应答。
 */
static int send_heartbeat_once(void)
{
    int fd;
    struct sockaddr_in addr;
    char line[64];

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)PRIMARY_PORT);
    if (inet_pton(AF_INET, PRIMARY_HOST, &addr.sin_addr) != 1)
    {
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(fd);
        return -1;
    }

    if (send_all(fd, "HEARTBEAT mirror2\n", 18) != 0)
    {
        close(fd);
        return -1;
    }

    (void)recv_line(fd, line, sizeof(line));
    close(fd);
    return 0;
}

/*
 * 功能：后台线程周期发送心跳。
 * 实现原理：循环执行 send_heartbeat_once，间隔固定秒数。
 */
static void *heartbeat_thread_main(void *arg)
{
    (void)arg;

    for (;;)
    {
        (void)send_heartbeat_once();
        sleep(HEARTBEAT_INTERVAL_SEC);
    }

    return NULL;
}

/*
 * 功能：启动心跳线程。
 * 实现原理：创建并分离 pthread，让主线程继续进入服务循环。
 */
static void start_heartbeat_thread(void)
{
    pthread_t tid;
    if (pthread_create(&tid, NULL, heartbeat_thread_main, NULL) == 0)
    {
        (void)pthread_detach(tid);
    }
}

/*
 * 功能：创建并返回服务端监听套接字。
 * 实现原理：按 socket -> setsockopt -> bind -> listen 的顺序初始化 TCP 监听端口。
 */
static int create_listen_socket(const server_config_t *cfg)
{
    int sock_fd;
    int opt = 1;
    struct sockaddr_in addr;

    if (cfg == NULL)
    {
        return -1;
    }

    /* 1) 创建 TCP 套接字 */
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0)
    {
        return -1;
    }

    /* 2) 允许端口快速复用，方便重启服务 */
    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        close(sock_fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)cfg->bind_port);

    if (cfg->bind_host != NULL && strcmp(cfg->bind_host, "0.0.0.0") != 0)
    {
        if (inet_pton(AF_INET, cfg->bind_host, &addr.sin_addr) != 1)
        {
            close(sock_fd);
            return -1;
        }
    }
    else
    {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    /* 3) 绑定地址与端口 */
    if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(sock_fd);
        return -1;
    }

    /* 4) 开始监听客户端连接 */
    if (listen(sock_fd, BACKLOG) < 0)
    {
        close(sock_fd);
        return -1;
    }

    return sock_fd;
}

/*
 * 功能：从客户端连接读取一条命令行。
 * 实现原理：逐字节 recv，遇到换行结束，过滤 '\r'，并确保缓冲区以 '\0' 结尾。
 */
static int read_command_line(int client_fd, char *buf, size_t size)
{
    /* 按行读取命令，以 '\n' 作为一条请求的边界 */
    size_t idx = 0;

    if (buf == NULL || size == 0)
    {
        return -1;
    }

    while (idx + 1 < size)
    {
        char ch;
        ssize_t n = recv(client_fd, &ch, 1, 0);

        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -1;
        }
        if (n == 0)
        {
            if (idx == 0)
            {
                return 0;
            }
            break;
        }

        if (ch == '\n')
        {
            break;
        }

        if (ch != '\r')
        {
            buf[idx++] = ch;
        }
    }

    buf[idx] = '\0';
    return 1;
}

/*
 * 功能：处理单条客户端命令并返回响应。
 * 实现原理：当前 P0 使用 ACK 占位响应，后续在此扩展真实命令逻辑。
 */
static int process_command(int client_fd, const char *cmd)
{
    char resp[MAX_COMMAND_LEN + 64];
    if (strcmp(cmd, "PING") == 0)
    {
        return send_all(client_fd, "PONG mirror2\n", 13);
    }

    if (cmd == NULL)
    {
        return -1;
    }

    /* TODO: 在此实现真实命令分发（dirlist/fn/fz/ft/fdb/fda）。 */
    /* TODO: 根据命令结果返回规范消息，如 File not found / No file found。 */
    /* TODO: 对打包类命令返回 temp.tar.gz（二进制传输）。 */
    /* P0 阶段返回占位 ACK，后续在此接入真实命令处理 */
    if (snprintf(resp, sizeof(resp), "ACK from %s: %s\n", NODE_NAME, cmd) < 0)
    {
        return -1;
    }

    return send_all(client_fd, resp, strlen(resp));
}

/*
 * 功能：处理单个客户端会话生命周期。
 * 实现原理：循环读取命令并分发，收到 quitc 或连接异常时结束会话。
 */
static void crequest(int client_fd)
{
    /* 每个客户端连接由一个子进程独占处理 */
    char cmd[MAX_COMMAND_LEN];

    for (;;)
    {
        /* 会话循环：持续读命令，直到断连或 quitc */
        int rc = read_command_line(client_fd, cmd, sizeof(cmd));
        if (rc <= 0)
        {
            break;
        }

        if (strcmp(cmd, "quitc") == 0)
        {
            /* 客户端主动结束会话 */
            (void)send_all(client_fd, "BYE\n", 4);
            break;
        }

        if (process_command(client_fd, cmd) != 0)
        {
            break;
        }
    }
}

/*
 * 功能：启动服务端主循环并并发处理客户端。
 * 实现原理：accept 新连接后 fork 子进程处理会话，父进程继续监听。
 */
static int run_server(const server_config_t *cfg)
{
    int listen_fd = create_listen_socket(cfg);
    if (listen_fd < 0)
    {
        fprintf(stderr, "%s: failed to create listen socket (skeleton)\n", NODE_NAME);
        return 1;
    }

    /* 子进程退出自动回收，避免僵尸进程 */
    signal(SIGCHLD, SIG_IGN);

    for (;;)
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        /* 主进程阻塞等待新连接 */
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);

        if (client_fd < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            perror("accept");
            continue;
        }

        /* 每个客户端连接 fork 一个子进程独占处理 */
        pid_t pid = fork();
        if (pid < 0)
        {
            perror("fork");
            close(client_fd);
            continue;
        }

        if (pid == 0)
        {
            /* 子进程只负责当前连接，处理完即退出 */
            close(listen_fd);
            crequest(client_fd);
            close(client_fd);
            _exit(0);
        }

        /* 父进程关闭已复制的客户端 fd，继续 accept */
        close(client_fd);
    }

    close(listen_fd);
    return 0;
}

/*
 * 功能：程序入口，初始化配置并启动镜像服务端2。
 * 实现原理：构造 server_config 后调用 run_server。
 */
int main(void)
{
    server_config_t cfg;
    /* TODO: 从命令行参数或配置文件读取监听地址与端口。 */
    /* TODO: 增加日志级别、守护进程模式等运行参数。 */
    /* 骨架默认监听所有网卡 */
    cfg.bind_host = "0.0.0.0";
    cfg.bind_port = DEFAULT_PORT;

    start_heartbeat_thread();

    printf("%s starting on port %d (skeleton)\n", NODE_NAME, cfg.bind_port);
    return run_server(&cfg);
}
