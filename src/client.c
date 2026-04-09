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
 * print_output：是否打印响应（用于内部 PING 验证不显示响应）
 */
static int receive_response(int server_fd, char *redirect_host, size_t redirect_host_size, int *redirect_port, int *redirected, int print_output)
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

    /*
     * 协议分支 1: REDIRECT
     * 格式: "REDIRECT <host> <port>"
     * 含义: w26server 告知客户端应重连到指定的 mirror 节点。
     * 解析成功后设置 redirected=1 并返回，调用方据此断开当前连接并重连目标。
     */
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

    /*
     * 协议分支 2: FILE
     * 格式: "FILE <size>"  后跟 size 字节的二进制 tar.gz 数据
     * 含义: 服务端将匹配文件打包为归档并流式传输给客户端。
     * 客户端按协议头中的字节数严格读取，写入 ~/project/temp.tar.gz。
     */
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
        if (print_output)
        {
            printf("Received temp.tar.gz (%ld bytes) -> %s\n", file_size, out_path);
        }
        return 0;
    }

    if (print_output)
    {
        printf("%s\n", line);
    }
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
    int session_fd;
    int current_port = PRIMARY_PORT;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    snprintf(current_host, sizeof(current_host), "%s", PRIMARY_HOST);

    /* 启动时连接主服务端 */
    session_fd = connect_to_server(PRIMARY_HOST, PRIMARY_PORT);
    if (session_fd < 0)
    {
        fprintf(stderr, "client: failed to connect w26server (%s:%d)\n", PRIMARY_HOST, PRIMARY_PORT);
        return 1;
    }

    /*
     * 节点探测与路由跟随流程：
     *   1) 向 w26server 发送 CONNECT_PROBE
     *   2) w26server 根据客户端序号决定路由：
     *      - 归属本地 → 回复 "CONNECTED w26server 127.0.0.1 5000"（非 REDIRECT，循环结束）
     *      - 归属 mirror → 回复 "REDIRECT 127.0.0.1 5001"
     *   3) 收到 REDIRECT 后断开当前连接，重连到目标 mirror，再发 CONNECT_PROBE
     *   4) mirror 回复 "CONNECTED mirrorN ..."（非 REDIRECT，循环结束）
     *   循环结束后 session_fd 已连接到最终服务节点，后续所有命令都直接发往该节点。
     */
    {
        char probe_host[64] = {0};
        int probe_port = 0;
        int probe_redirected = 0;
        int hops;

        for (hops = 0; hops < MAX_REDIRECT_HOPS; ++hops)
        {
            if (send_command(session_fd, "CONNECT_PROBE") != 0 ||
                receive_response(session_fd, probe_host, sizeof(probe_host),
                                 &probe_port, &probe_redirected, 0) != 0)
            {
                fprintf(stderr, "client: CONNECT_PROBE failed\n");
                close(session_fd);
                return 1;
            }

            /* CONNECTED 响应(非 REDIRECT)表示当前节点即为最终服务节点 */
            if (!probe_redirected)
            {
                break;
            }

            /* 收到 REDIRECT: 断开当前连接，重连到目标 mirror */
            close(session_fd);
            snprintf(current_host, sizeof(current_host), "%s", probe_host);
            current_port = probe_port;

            session_fd = connect_to_server(current_host, current_port);
            if (session_fd < 0)
            {
                fprintf(stderr, "client: failed to connect %s:%d\n",
                        current_host, current_port);
                return 1;
            }
        }

        if (hops >= MAX_REDIRECT_HOPS)
        {
            fprintf(stderr, "client: too many redirects during probe\n");
            close(session_fd);
            return 1;
        }

        printf("client connected to w26server (%s:%d), NODE: %s\n",
               current_host, current_port,
               (current_port == PRIMARY_PORT) ? "w26server"
               : (current_port == MIRROR1_PORT) ? "mirror1"
               : "mirror2");
        fflush(stdout);
    }

    /* 交互式命令循环：保持同一会话连接，直到 quitc 或异常 */
    while (fgets(line, sizeof(line), stdin) != NULL)
    {
        int redirect_hops;
        int done = 0;

        /* 去掉输入末尾换行，统一使用 send_command 追加 '\n' */
        line[strcspn(line, "\n")] = '\0';

        if (validate_command(line, err, sizeof(err)) != 0)
        {
            fprintf(stderr, "Invalid command: %s\n", err);
            continue;
        }

        for (redirect_hops = 0; redirect_hops < MAX_REDIRECT_HOPS; ++redirect_hops)
        {
            char next_host[64] = {0};
            int next_port = 0;
            int redirected = 0;

            if (send_command(session_fd, line) != 0 ||
                receive_response(session_fd, next_host, sizeof(next_host), &next_port, &redirected, 1) != 0)
            {
                fprintf(stderr, "client: send/receive failed with %s:%d\n", current_host, current_port);
                close(session_fd);
                session_fd = -1;
                break;
            }

            if (!redirected)
            {
                done = 1;
                break;
            }

            close(session_fd);

            snprintf(current_host, sizeof(current_host), "%s", next_host);
            current_port = next_port;

            session_fd = connect_to_server(current_host, current_port);
            if (session_fd < 0)
            {
                fprintf(stderr, "client: failed to connect redirected target %s:%d\n", current_host, current_port);
                break;
            }
        }

        if (!done)
        {
            if (redirect_hops >= MAX_REDIRECT_HOPS)
            {
                fprintf(stderr, "client: too many redirects\n");
            }
            break;
        }

        if (strcmp(line, "quitc") == 0)
        {
            break;
        }
    }

    if (session_fd >= 0)
    {
        close(session_fd);
    }

    return 0;
}
