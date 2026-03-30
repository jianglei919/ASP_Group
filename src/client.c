#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* 客户端默认连接主服务端 */
#define PRIMARY_HOST "127.0.0.1"
#define PRIMARY_PORT 5000
#define MIRROR1_HOST "127.0.0.1"
#define MIRROR1_PORT 5001
#define MIRROR2_HOST "127.0.0.1"
#define MIRROR2_PORT 5002
#define MAX_COMMAND_LEN 512
#define NODES_CACHE_TTL_SEC 2

typedef struct node_info
{
    const char *name;
    const char *host;
    int port;
    int online;
} node_info_t;

typedef struct nodes_cache
{
    time_t last_refresh_ts;
} nodes_cache_t;

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

    if (strcmp(line, "GET_NODES") == 0)
    {
        err[0] = '\0';
        return 0;
    }

    snprintf(err, err_size, "unsupported command in P1 (allowed: dirlist -a, fn <filename>, quitc)");
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
 * 功能：按行接收一条文本响应。
 * 实现原理：逐字节读取直到换行，便于处理 GET_NODES 返回。
 */
static int receive_line_only(int server_fd, char *buf, size_t size)
{
    size_t idx = 0;

    if (buf == NULL || size == 0)
    {
        return -1;
    }

    while (idx + 1 < size)
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
 * 功能：解析在线表中的节点状态。
 * 实现原理：通过字符串匹配提取 w26server/mirror1/mirror2 的 0/1 标记。
 */
static void apply_nodes_status(const char *line, node_info_t nodes[3])
{
    nodes[0].online = (strstr(line, "w26server=1") != NULL) ? 1 : 0;
    nodes[1].online = (strstr(line, "mirror1=1") != NULL) ? 1 : 0;
    nodes[2].online = (strstr(line, "mirror2=1") != NULL) ? 1 : 0;
}

/*
 * 功能：从主服务端获取节点在线表。
 * 实现原理：短连接发送 GET_NODES 并解析返回文本；失败时退化为仅主服务可用。
 */
static void fetch_nodes_status(node_info_t nodes[3])
{
    int fd;
    char line[128];

    fd = connect_to_server(PRIMARY_HOST, PRIMARY_PORT);
    if (fd < 0)
    {
        nodes[0].online = 1;
        nodes[1].online = 0;
        nodes[2].online = 0;
        return;
    }

    if (send_command(fd, "GET_NODES") != 0 || receive_line_only(fd, line, sizeof(line)) != 0)
    {
        close(fd);
        nodes[0].online = 1;
        nodes[1].online = 0;
        nodes[2].online = 0;
        return;
    }

    close(fd);
    apply_nodes_status(line, nodes);
}

/*
 * 功能：按缓存策略刷新在线表。
 * 实现原理：在 TTL 内直接复用缓存；超时或强制刷新时重新请求 GET_NODES。
 */
static void refresh_nodes_status(node_info_t nodes[3], nodes_cache_t *cache, int force)
{
    time_t now;

    if (cache == NULL)
    {
        fetch_nodes_status(nodes);
        return;
    }

    now = time(NULL);
    if (!force && cache->last_refresh_ts > 0 && (now - cache->last_refresh_ts) < NODES_CACHE_TTL_SEC)
    {
        return;
    }

    fetch_nodes_status(nodes);
    cache->last_refresh_ts = now;
}

/*
 * 功能：根据题目连接序号规则计算优先节点。
 * 实现原理：1-2 主服、3-4 mirror1、5-6 mirror2，7 开始按 1-2-3 循环。
 */
static int preferred_index_by_seq(long seq)
{
    if (seq <= 2)
    {
        return 0;
    }
    if (seq <= 4)
    {
        return 1;
    }
    if (seq <= 6)
    {
        return 2;
    }
    return (int)((seq - 7) % 3);
}

/*
 * 功能：按“优先节点 + 故障跳过”选择可用节点。
 * 实现原理：从优先下标开始顺序探测 3 个节点，返回第一个 online 节点。
 */
static int choose_node_index(const node_info_t nodes[3], int preferred)
{
    int i;

    for (i = 0; i < 3; ++i)
    {
        int idx = (preferred + i) % 3;
        if (nodes[idx].online)
        {
            return idx;
        }
    }

    return -1;
}

/*
 * 功能：客户端程序入口与交互主循环。
 * 实现原理：建立连接后循环执行 读取输入 -> 校验 -> 发送 -> 接收，直到 quitc。
 */
int main(void)
{
    long request_seq = 1;
    char line[MAX_COMMAND_LEN];
    char err[128];
    nodes_cache_t cache = {0};
    node_info_t nodes[3] = {
        {"w26server", PRIMARY_HOST, PRIMARY_PORT, 1},
        {"mirror1", MIRROR1_HOST, MIRROR1_PORT, 0},
        {"mirror2", MIRROR2_HOST, MIRROR2_PORT, 0}};

    /* TODO: 增加可配置服务端地址与端口（命令行参数或配置文件）。 */

    /* 交互式命令循环：读入、校验、发送、接收 */
    while (fgets(line, sizeof(line), stdin) != NULL)
    {
        int preferred_idx;
        int chosen_idx;
        int attempt;
        int sent_ok = 0;

        /* 去掉输入末尾换行，统一使用 send_command 追加 '\n' */
        line[strcspn(line, "\n")] = '\0';

        if (validate_command(line, err, sizeof(err)) != 0)
        {
            fprintf(stderr, "Invalid command: %s\n", err);
            continue;
        }

        refresh_nodes_status(nodes, &cache, 0);
        preferred_idx = preferred_index_by_seq(request_seq);
        chosen_idx = choose_node_index(nodes, preferred_idx);

        if (chosen_idx < 0)
        {
            refresh_nodes_status(nodes, &cache, 1);
            chosen_idx = choose_node_index(nodes, preferred_idx);
            if (chosen_idx < 0)
            {
                fprintf(stderr, "client: no online server available\n");
                break;
            }
        }

        for (attempt = 0; attempt < 3; ++attempt)
        {
            int idx = (chosen_idx + attempt) % 3;
            int server_fd;

            if (!nodes[idx].online)
            {
                continue;
            }

            server_fd = connect_to_server(nodes[idx].host, nodes[idx].port);
            if (server_fd < 0)
            {
                nodes[idx].online = 0;
                continue;
            }

            if (send_command(server_fd, line) == 0 && receive_response(server_fd) == 0)
            {
                sent_ok = 1;
                close(server_fd);
                break;
            }

            close(server_fd);
            nodes[idx].online = 0;
        }

        if (!sent_ok)
        {
            refresh_nodes_status(nodes, &cache, 1);
            fprintf(stderr, "client: send/receive failed on all available nodes\n");
            break;
        }

        request_seq++;

        if (strcmp(line, "quitc") == 0)
        {
            /* quitc 的响应已接收，结束客户端循环 */
            break;
        }
    }
    return 0;
}
