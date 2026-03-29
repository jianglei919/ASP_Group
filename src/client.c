#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* 客户端默认连接主服务端 */
#define PRIMARY_HOST "127.0.0.1"
#define PRIMARY_PORT 5000
#define MAX_COMMAND_LEN 512

/*
 * 功能：建立到服务端的 TCP 连接。
 * 实现原理：创建套接字并调用 connect，失败时关闭 fd 并返回错误。
 */
static int connect_to_server(const char *host, int port)
{
    int sock_fd;
    struct sockaddr_in addr;

    /* 1) 创建 TCP 套接字 */
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0)
    {
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
    {
        close(sock_fd);
        return -1;
    }

    /* 2) 连接到目标服务端 */
    if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(sock_fd);
        return -1;
    }

    return sock_fd;
}

/*
 * 功能：校验用户输入命令是否合法。
 * 实现原理：P0 阶段先做空命令检查，P1 再扩展完整语法规则。
 */
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

    /* TODO: 按作业规范实现完整语法校验（参数数量、类型、范围）。 */
    /* TODO: 对 fz 检查 size1 <= size2 且均为非负整数。 */
    /* TODO: 对 ft 限制扩展名个数为 1~3 且互不重复。 */
    /* TODO: 对 fdb/fda 校验日期格式 YYYY-MM-DD。 */
    /* TODO: 当前 P0 仅做非空检查，后续升级到严格校验。 */
    err[0] = '\0';
    return 0;
}

/*
 * 功能：可靠发送指定长度的数据。
 * 实现原理：循环调用 send，处理 EINTR 中断，直到所有字节发送完成。
 */
static int send_all(int fd, const char *data, size_t len)
{
    /* 循环发送，确保完整写出请求 */
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
 * 功能：发送一条命令到服务端。
 * 实现原理：发送命令内容并追加换行，供服务端按行读取。
 */
static int send_command(int server_fd, const char *line)
{
    size_t len;

    if (line == NULL)
    {
        return -1;
    }

    /* 以换行结尾，服务端按行解析请求 */
    len = strlen(line);
    if (send_all(server_fd, line, len) != 0)
    {
        return -1;
    }

    return send_all(server_fd, "\n", 1);
}

/*
 * 功能：接收并打印服务端响应。
 * 实现原理：按行 recv 响应数据，遇到换行结束后输出到终端。
 */
static int receive_response(int server_fd)
{
    char buf[MAX_COMMAND_LEN + 128];
    size_t idx = 0;

    /* TODO: 扩展为支持二进制文件响应并保存到 ~/project/temp.tar.gz。 */
    /* TODO: 区分文本消息与文件流协议，避免把二进制当文本打印。 */
    /* 按行读取服务端响应，便于快速验证通信链路 */
    while (idx + 1 < sizeof(buf))
    {
        char ch;
        ssize_t n = recv(server_fd, &ch, 1, 0);

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
    printf("%s\n", buf);
    return 0;
}

/*
 * 功能：客户端程序入口与交互主循环。
 * 实现原理：建立连接后循环执行 读取输入 -> 校验 -> 发送 -> 接收，直到 quitc。
 */
int main(void)
{
    int server_fd;
    char line[MAX_COMMAND_LEN];
    char err[128];

    /* TODO: 按连接序号实现 w26server/mirror1/mirror2 的分流策略。 */
    /* TODO: 增加可配置服务端地址与端口（命令行参数或配置文件）。 */
    server_fd = connect_to_server(PRIMARY_HOST, PRIMARY_PORT);
    if (server_fd < 0)
    {
        fprintf(stderr, "client: cannot connect to %s:%d (skeleton)\n", PRIMARY_HOST, PRIMARY_PORT);
        return 1;
    }

    /* 交互式命令循环：读入、校验、发送、接收 */
    while (fgets(line, sizeof(line), stdin) != NULL)
    {
        /* 去掉输入末尾换行，统一使用 send_command 追加 '\n' */
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
            /* quitc 的响应已接收，结束客户端循环 */
            break;
        }
    }

    close(server_fd);
    return 0;
}
