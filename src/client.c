#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* 客户端默认连接主服务端 */
#define PRIMARY_HOST "127.0.0.1"
#define PRIMARY_PORT 5000
#define MIRROR1_PORT 5001
#define MIRROR2_PORT 5002
#define MAX_COMMAND_LEN 512
#define MAX_REDIRECT_HOPS 4

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
    char cmd[32];
    char arg1[256];
    char arg2[256];
    int parts;

    if (line == NULL || err == NULL || err_size == 0)
    {
        return -1;
    }

    if (line[0] == '\0')
    {
        snprintf(err, err_size, "empty command");
        return -1;
    }

    if (strcmp(line, "quitc") == 0)
    {
        err[0] = '\0';
        return 0;
    }

    if (strcmp(line, "dirlist -a") == 0 || strcmp(line, "dirlist -t") == 0)
    {
        err[0] = '\0';
        return 0;
    }

    cmd[0] = '\0';
    arg1[0] = '\0';
    arg2[0] = '\0';
    parts = sscanf(line, "%31s %255s %255s", cmd, arg1, arg2);

    if (parts == 2 && strcmp(cmd, "fn") == 0)
    {
        err[0] = '\0';
        return 0;
    }

    if (parts == 3 && strcmp(cmd, "fz") == 0)
    {
        long n1;
        long n2;
        if (sscanf(line, "fz %ld %ld", &n1, &n2) == 2 && n1 >= 0 && n2 >= n1)
        {
            err[0] = '\0';
            return 0;
        }
        snprintf(err, err_size, "usage: fz <size1> <size2> (size2 >= size1 >= 0)");
        return -1;
    }

    if (parts >= 2 && parts <= 4 && strcmp(cmd, "ft") == 0)
    {
        err[0] = '\0';
        return 0;
    }

    if (parts == 2 && (strcmp(cmd, "fdb") == 0 || strcmp(cmd, "fda") == 0))
    {
        int y;
        int m;
        int d;
        if (sscanf(arg1, "%d-%d-%d", &y, &m, &d) == 3 && m >= 1 && m <= 12 && d >= 1 && d <= 31)
        {
            err[0] = '\0';
            return 0;
        }
        snprintf(err, err_size, "usage: %s YYYY-MM-DD", cmd);
        return -1;
    }

    if (parts >= 1 && strcmp(cmd, "dirlist") == 0)
    {
        snprintf(err, err_size, "P1 currently supports only: dirlist -a | dirlist -t");
        return -1;
    }

    if (parts >= 1 && strcmp(cmd, "fn") == 0)
    {
        snprintf(err, err_size, "usage: fn <filename>");
        return -1;
    }

    if (parts >= 1 && strcmp(cmd, "fz") == 0)
    {
        snprintf(err, err_size, "usage: fz <size1> <size2>");
        return -1;
    }

    if (parts >= 1 && strcmp(cmd, "ft") == 0)
    {
        snprintf(err, err_size, "usage: ft <ext1> [ext2] [ext3]");
        return -1;
    }

    if (parts >= 1 && (strcmp(cmd, "fdb") == 0 || strcmp(cmd, "fda") == 0))
    {
        snprintf(err, err_size, "usage: %s YYYY-MM-DD", cmd);
        return -1;
    }

    if (strcmp(line, "GET_NODES") == 0)
    {
        err[0] = '\0';
        return 0;
    }

    snprintf(err, err_size, "unsupported command (allowed: dirlist -a|-t, fn, fz, ft, fdb, fda, quitc)");
    return -1;
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
 * 实现原理：先读取首行判断是否是 REDIRECT 或 FILE 协议；若是 REDIRECT，则把目标地址回传给调用方，
 * 由上层重新连接目标节点并重发同一条命令。若是 FILE，则继续按长度读取二进制归档；否则直接打印文本响应。
 */
static int receive_response(int server_fd, char *redirect_host, size_t redirect_host_size, int *redirect_port, int *redirected)
{
    char line[MAX_COMMAND_LEN + 128];
    const char *home;
    char out_dir[PATH_MAX];
    char out_path[PATH_MAX];
    long file_size;
    FILE *fp;
    char buf[4096];
    long received;
    size_t idx = 0;

    /* 先读取响应首行，用于区分文本响应与文件流响应。 */
    while (idx + 1 < sizeof(line))
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
            line[idx++] = ch;
        }
    }

    line[idx] = '\0';

    if (redirected != NULL)
    {
        *redirected = 0;
    }

    if (redirect_host != NULL && redirect_host_size > 0 && redirect_port != NULL)
    {
        char redirect_fmt[32];
        size_t host_width = redirect_host_size - 1;

        if (host_width > 127)
        {
            host_width = 127;
        }

        if (snprintf(redirect_fmt, sizeof(redirect_fmt), "REDIRECT %%%zus %%d", host_width) >= 0 &&
            sscanf(line, redirect_fmt, redirect_host, redirect_port) == 2)
        {
            if (redirected != NULL)
            {
                *redirected = 1;
            }
            return 0;
        }
    }

    if (sscanf(line, "FILE %ld", &file_size) == 1 && file_size >= 0)
    {
        /* 约定：归档始终写入 ~/project/temp.tar.gz，便于老师与脚本统一检查。 */
        home = getenv("HOME");
        if (home == NULL || home[0] == '\0')
        {
            home = ".";
        }

        if (snprintf(out_dir, sizeof(out_dir), "%s/project", home) < 0)
        {
            return -1;
        }
        if (mkdir(out_dir, 0755) != 0 && errno != EEXIST)
        {
            return -1;
        }
        if (snprintf(out_path, sizeof(out_path), "%s/temp.tar.gz", out_dir) < 0)
        {
            return -1;
        }

        fp = fopen(out_path, "wb");
        if (fp == NULL)
        {
            return -1;
        }

        received = 0;
        /* 以协议头中的字节数为准严格读取，防止多读到后续命令响应。 */
        while (received < file_size)
        {
            size_t need = (size_t)((file_size - received) > (long)sizeof(buf) ? sizeof(buf) : (size_t)(file_size - received));
            ssize_t n = recv(server_fd, buf, need, 0);
            if (n < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                fclose(fp);
                return -1;
            }
            if (n == 0)
            {
                fclose(fp);
                return -1;
            }
            if (fwrite(buf, 1, (size_t)n, fp) != (size_t)n)
            {
                fclose(fp);
                return -1;
            }
            received += (long)n;
        }

        fclose(fp);
        printf("Received temp.tar.gz (%ld bytes) -> %s\n", file_size, out_path);
        return 0;
    }

    printf("%s\n", line);
    return 0;
}

/*
 * 功能：客户端程序入口与交互主循环。
 * 实现原理：建立连接后循环执行 读取输入 -> 校验 -> 发送 -> 接收，直到 quitc。
 */
int main(void)
{
    char line[MAX_COMMAND_LEN];
    char err[128];
    char current_host[64];
    int startup_fd;

    /* TODO: 增加可配置服务端地址与端口（命令行参数或配置文件）。 */

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    snprintf(current_host, sizeof(current_host), "%s", PRIMARY_HOST);

    startup_fd = connect_to_server(PRIMARY_HOST, PRIMARY_PORT);
    if (startup_fd < 0)
    {
        fprintf(stderr, "client: failed to connect w26server (%s:%d)\n", PRIMARY_HOST, PRIMARY_PORT);
        return 1;
    }

    printf("client connected to w26server (%s:%d)\n", PRIMARY_HOST, PRIMARY_PORT);
    fflush(stdout);
    close(startup_fd);

    /* 交互式命令循环：读入、校验、发送、接收 */
    while (fgets(line, sizeof(line), stdin) != NULL)
    {
        int server_fd = -1;
        int final_server_fd = -1;

        /* 去掉输入末尾换行，统一使用 send_command 追加 '\n' */
        line[strcspn(line, "\n")] = '\0';

        /* 每条新命令都从主服务端重新起步。 */
        snprintf(current_host, sizeof(current_host), "%s", PRIMARY_HOST);

        if (validate_command(line, err, sizeof(err)) != 0)
        {
            fprintf(stderr, "Invalid command: %s\n", err);
            continue;
        }

        /* 严格服务端分流：先连主服务端，再按 REDIRECT 重连目标节点。 */
        int current_port = PRIMARY_PORT;
        int redirect_hops;
        int done = 0;

        for (redirect_hops = 0; redirect_hops < MAX_REDIRECT_HOPS; ++redirect_hops)
        {
            char next_host[64] = {0};
            int next_port = 0;
            int redirected = 0;

            server_fd = connect_to_server(current_host, current_port);
            if (server_fd < 0)
            {
                fprintf(stderr, "client: failed to connect %s:%d\n", current_host, current_port);
                break;
            }

            if (send_command(server_fd, line) != 0 ||
                receive_response(server_fd, next_host, sizeof(next_host), &next_port, &redirected) != 0)
            {
                fprintf(stderr, "client: send/receive failed with %s:%d\n", current_host, current_port);
                close(server_fd);
                break;
            }

            if (!redirected)
            {
                done = 1;
                final_server_fd = server_fd;
                break;
            }

            close(server_fd);

            snprintf(current_host, sizeof(current_host), "%s", next_host);
            current_port = next_port;
        }

        if (!done)
        {
            if (redirect_hops >= MAX_REDIRECT_HOPS)
            {
                fprintf(stderr, "client: too many redirects\n");
            }
            break;
        }

        if (send_command(final_server_fd, "PING") != 0 || receive_response(final_server_fd, NULL, 0, NULL, NULL) != 0)
        {
            fprintf(stderr, "client: failed to probe connected server %s:%d\n", current_host, current_port);
            if (final_server_fd >= 0)
            {
                close(final_server_fd);
            }
            break;
        }

        close(final_server_fd);

        if (strcmp(line, "quitc") == 0)
        {
            /* quitc 的响应已接收，结束客户端循环 */
            break;
        }
    }
    return 0;
}
