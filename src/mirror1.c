#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

/* 镜像服务端1基础配置 */
#define NODE_NAME "mirror1"
#define DEFAULT_PORT 5001
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
 * 实现原理：短连接发送 HEARTBEAT mirror1，并读取一行应答。
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

    if (send_all(fd, "HEARTBEAT mirror1\n", 18) != 0)
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

/* 功能：将文件权限位转换为 rwx 字符串。实现原理：按 mode 位逐位映射。 */
static void format_permissions(mode_t mode, char out[10])
{
    out[0] = (mode & S_IRUSR) ? 'r' : '-';
    out[1] = (mode & S_IWUSR) ? 'w' : '-';
    out[2] = (mode & S_IXUSR) ? 'x' : '-';
    out[3] = (mode & S_IRGRP) ? 'r' : '-';
    out[4] = (mode & S_IWGRP) ? 'w' : '-';
    out[5] = (mode & S_IXGRP) ? 'x' : '-';
    out[6] = (mode & S_IROTH) ? 'r' : '-';
    out[7] = (mode & S_IWOTH) ? 'w' : '-';
    out[8] = (mode & S_IXOTH) ? 'x' : '-';
    out[9] = '\0';
}

/* 功能：目录名排序比较器。实现原理：qsort 回调，内部调用 strcmp。 */
static int cmp_string_ptr(const void *a, const void *b)
{
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;
    return strcmp(*sa, *sb);
}

/* 功能：复制字符串。实现原理：手动申请内存并拷贝，避免依赖 strdup。 */
static char *dup_string(const char *s)
{
    size_t len;
    char *p;

    if (s == NULL)
    {
        return NULL;
    }

    len = strlen(s);
    p = (char *)malloc(len + 1);
    if (p == NULL)
    {
        return NULL;
    }
    memcpy(p, s, len + 1);
    return p;
}

/* 功能：返回搜索根目录。实现原理：优先 HOME，不存在则回退当前目录。 */
static const char *get_search_root(void)
{
    const char *home = getenv("HOME");
    return (home != NULL && home[0] != '\0') ? home : ".";
}

/* 功能：处理 dirlist -a。实现原理：读取一级子目录并按字母序返回。 */
static int handle_dirlist_a(int client_fd)
{
    const char *root = get_search_root();
    DIR *dir;
    struct dirent *ent;
    char **names = NULL;
    size_t count = 0;
    size_t i;
    int rc = 0;

    dir = opendir(root);
    if (dir == NULL)
    {
        return send_all(client_fd, "No directory found\n", 19);
    }

    while ((ent = readdir(dir)) != NULL)
    {
        char full[PATH_MAX];
        struct stat st;
        char *name_copy;
        char **tmp;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
        {
            continue;
        }
        if (snprintf(full, sizeof(full), "%s/%s", root, ent->d_name) < 0)
        {
            continue;
        }
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode))
        {
            continue;
        }

        name_copy = dup_string(ent->d_name);
        if (name_copy == NULL)
        {
            rc = -1;
            break;
        }
        tmp = (char **)realloc(names, (count + 1) * sizeof(char *));
        if (tmp == NULL)
        {
            free(name_copy);
            rc = -1;
            break;
        }
        names = tmp;
        names[count++] = name_copy;
    }
    closedir(dir);

    if (rc != 0)
    {
        for (i = 0; i < count; ++i)
        {
            free(names[i]);
        }
        free(names);
        return -1;
    }

    if (count == 0)
    {
        free(names);
        return send_all(client_fd, "No directory found\n", 19);
    }

    qsort(names, count, sizeof(char *), cmp_string_ptr);
    for (i = 0; i < count; ++i)
    {
        if (send_all(client_fd, names[i], strlen(names[i])) != 0 || send_all(client_fd, "\n", 1) != 0)
        {
            rc = -1;
            break;
        }
    }

    for (i = 0; i < count; ++i)
    {
        free(names[i]);
    }
    free(names);
    return rc;
}

/* 功能：递归查找首个同名文件。实现原理：目录树深度优先遍历。 */
static int find_first_file(const char *dir_path, const char *target_name, char *out_path, size_t out_size)
{
    DIR *dir = opendir(dir_path);
    struct dirent *ent;

    if (dir == NULL)
    {
        return 0;
    }

    while ((ent = readdir(dir)) != NULL)
    {
        char full[PATH_MAX];
        struct stat st;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
        {
            continue;
        }

        if (snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name) < 0)
        {
            continue;
        }
        if (stat(full, &st) != 0)
        {
            continue;
        }

        if (S_ISDIR(st.st_mode))
        {
            if (find_first_file(full, target_name, out_path, out_size))
            {
                closedir(dir);
                return 1;
            }
        }
        else if (S_ISREG(st.st_mode) && strcmp(ent->d_name, target_name) == 0)
        {
            if (snprintf(out_path, out_size, "%s", full) >= 0)
            {
                closedir(dir);
                return 1;
            }
        }
    }

    closedir(dir);
    return 0;
}

/* 功能：处理 fn filename。实现原理：查找文件并返回元信息。 */
static int handle_fn(int client_fd, const char *filename)
{
    const char *root = get_search_root();
    char path[PATH_MAX];
    struct stat st;
    char perm[10];
    char time_buf[32];
    struct tm *tm_info;
    char resp[1024];
    const char *base;

    if (filename == NULL || filename[0] == '\0')
    {
        return send_all(client_fd, "File not found\n", 15);
    }
    if (!find_first_file(root, filename, path, sizeof(path)) || stat(path, &st) != 0)
    {
        return send_all(client_fd, "File not found\n", 15);
    }

    format_permissions(st.st_mode, perm);
    tm_info = localtime(&st.st_ctime);
    if (tm_info == NULL)
    {
        snprintf(time_buf, sizeof(time_buf), "unknown");
    }
    else
    {
        (void)strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
    }

    base = strrchr(path, '/');
    base = (base != NULL) ? (base + 1) : path;
    if (snprintf(resp, sizeof(resp), "filename=%s size=%ld created=%s permissions=%s\n", base, (long)st.st_size, time_buf, perm) < 0)
    {
        return -1;
    }

    return send_all(client_fd, resp, strlen(resp));
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
        return send_all(client_fd, "PONG mirror1\n", 13);
    }
    if (strcmp(cmd, "dirlist -a") == 0)
    {
        return handle_dirlist_a(client_fd);
    }

    if (strncmp(cmd, "fn ", 3) == 0)
    {
        const char *filename = cmd + 3;
        while (*filename == ' ')
        {
            filename++;
        }
        return handle_fn(client_fd, filename);
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
 * 功能：程序入口，初始化配置并启动镜像服务端1。
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
