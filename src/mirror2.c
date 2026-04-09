#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>

/* 镜像服务端2基础配置 */
#define NODE_NAME "mirror2"
#define DEFAULT_PORT 5002
#define PRIMARY_HOST "127.0.0.1"
#define PRIMARY_PORT 5000
#define HEARTBEAT_INTERVAL_SEC 2
#define BACKLOG 16
#define MAX_COMMAND_LEN 512
#define DEFAULT_MAX_SCAN_DEPTH 8

typedef struct server_config
{
    /* 镜像服务监听地址，默认绑定全部网卡。 */
    const char *bind_host;
    /* 镜像服务监听端口。 */
    int bind_port;
} server_config_t;

typedef struct dir_item
{
    /* 目录名（不含父路径）。 */
    char *name;
    /* 目录时间戳，用于时间排序输出。 */
    time_t ctime;
} dir_item_t;

typedef struct file_list
{
    /* 匹配文件路径动态数组。 */
    char **paths;
    /* 数组中的有效路径数量。 */
    size_t count;
} file_list_t;

typedef struct size_filter
{
    /* 文件大小下界（含）。 */
    off_t min_size;
    /* 文件大小上界（含）。 */
    off_t max_size;
} size_filter_t;

typedef struct ext_filter
{
    /* 可接受的扩展名，最多 3 个。 */
    const char *exts[3];
    /* 当前已解析出的扩展名个数。 */
    int count;
} ext_filter_t;

typedef struct date_filter
{
    /* 日期阈值时间戳。 */
    time_t threshold;
    /* 1: 早于阈值(fdb)；0: 晚于等于阈值(fda)。 */
    int before;
} date_filter_t;

/*
 * 功能：可靠发送指定长度的数据。
 * 实现原理：TCP 发送存在短写风险，因此维护 sent 偏移循环发送直到写满目标长度；
 * send 被信号打断时按 EINTR 重试，返回 0 或不可恢复错误则视为连接异常并向上返回失败。
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
 * 实现原理：逐字节读取直到 '\n'，并过滤 CRLF 中的 '\r'，保证输出是标准 C 字符串。
 * 该函数专门用于短文本协议（如心跳应答），避免一次 recv 粘包导致解析歧义。
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
 * 实现原理：每次心跳建立短连接，发送固定格式 HEARTBEAT 消息并读取单行应答后立即关闭。
 * 这种无状态上报模型可避免长连接故障积累，异常时由下一周期重试恢复。
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
 * 实现原理：在线程内执行无限循环：上报一次心跳后 sleep 固定周期。
 * 即使某次上报失败也不会终止线程，从而保持持续自愈的在线状态上报能力。
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
 * 实现原理：创建后台线程并立即 detach，使其生命周期独立于主线程 join；
 * 主线程可无阻塞进入监听循环，心跳与服务并发执行。
 */
static void start_heartbeat_thread(void)
{
    pthread_t tid;
    if (pthread_create(&tid, NULL, heartbeat_thread_main, NULL) == 0)
    {
        (void)pthread_detach(tid);
    }
}

/* 功能：将文件权限位转换为 rwx 字符串。实现原理：按 owner/group/other 顺序逐位映射，生成固定 9 字符权限串。 */
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

/* 功能：目录名升序比较器。实现原理：作为 qsort 回调按字典序比较 name 字段，供 dirlist -a 复用。 */
static int cmp_dir_by_name(const void *a, const void *b)
{
    const dir_item_t *da = (const dir_item_t *)a;
    const dir_item_t *db = (const dir_item_t *)b;
    return strcmp(da->name, db->name);
}

/* 功能：目录时间升序比较器。实现原理：先按 ctime 排序实现最老优先，若时间相同则按名称比较保证结果稳定。 */
static int cmp_dir_by_ctime(const void *a, const void *b)
{
    const dir_item_t *da = (const dir_item_t *)a;
    const dir_item_t *db = (const dir_item_t *)b;

    if (da->ctime < db->ctime)
    {
        return -1;
    }
    if (da->ctime > db->ctime)
    {
        return 1;
    }
    return strcmp(da->name, db->name);
}

/* 功能：复制字符串。实现原理：显式 malloc+memcpy 生成独立副本，避免对非标准函数 strdup 的可移植性依赖。 */
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

/* 功能：返回搜索根目录。实现原理：优先使用 W26_SEARCH_ROOT，未设置时回退 HOME，再回退当前目录。 */
static const char *get_search_root(void)
{
    const char *custom = getenv("W26_SEARCH_ROOT");
    const char *home = getenv("HOME");

    if (custom != NULL && custom[0] != '\0')
    {
        return custom;
    }
    return (home != NULL && home[0] != '\0') ? home : ".";
}

/* 功能：返回递归扫描最大深度。实现原理：读取 W26_MAX_SCAN_DEPTH 并进行数字与边界校验，非法值统一回落默认深度。 */
static int get_max_scan_depth(void)
{
    const char *s = getenv("W26_MAX_SCAN_DEPTH");
    char *end = NULL;
    long v;

    if (s == NULL || s[0] == '\0')
    {
        return DEFAULT_MAX_SCAN_DEPTH;
    }

    v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < 1 || v > 64)
    {
        return DEFAULT_MAX_SCAN_DEPTH;
    }

    return (int)v;
}

/* 功能：收集搜索根下一级子目录。实现原理：遍历根目录，仅提取目录项并记录目录名与时间戳，供后续排序输出。 */
static int collect_subdirs(dir_item_t **out_items, size_t *out_count)
{
    const char *root = get_search_root();
    DIR *dir;
    struct dirent *ent;
    dir_item_t *items = NULL;
    size_t count = 0;
    int ok = 0;

    if (out_items == NULL || out_count == NULL)
    {
        return -1;
    }

    *out_items = NULL;
    *out_count = 0;

    dir = opendir(root);
    if (dir == NULL)
    {
        return -1;
    }

    while ((ent = readdir(dir)) != NULL)
    {
        char full[PATH_MAX];
        struct stat st;
        char *name_copy;
        dir_item_t *tmp;

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
            ok = -1;
            break;
        }
        tmp = (dir_item_t *)realloc(items, (count + 1) * sizeof(dir_item_t));
        if (tmp == NULL)
        {
            free(name_copy);
            ok = -1;
            break;
        }
        items = tmp;
        items[count].name = name_copy;
        items[count].ctime = st.st_ctime;
        count++;
    }
    closedir(dir);

    if (ok != 0)
    {
        size_t i;
        for (i = 0; i < count; ++i)
        {
            free(items[i].name);
        }
        free(items);
        return -1;
    }

    *out_items = items;
    *out_count = count;
    return 0;
}

/* 功能：以单行文本返回目录列表。实现原理：按既定顺序将目录名以空格拼接并追加换行，便于客户端按行读取。 */
static int send_dirlist_line(int client_fd, dir_item_t *items, size_t count)
{
    size_t i;

    if (count == 0)
    {
        return send_all(client_fd, "No directory found\n", 19);
    }

    for (i = 0; i < count; ++i)
    {
        if (send_all(client_fd, items[i].name, strlen(items[i].name)) != 0)
        {
            return -1;
        }
        if (i + 1 < count)
        {
            if (send_all(client_fd, " ", 1) != 0)
            {
                return -1;
            }
        }
    }

    return send_all(client_fd, "\n", 1);
}

/* 功能：释放目录集合内存。实现原理：先释放每个 name，再释放承载数组，避免多次请求造成内存累积。 */
static void free_dir_items(dir_item_t *items, size_t count)
{
    size_t i;
    for (i = 0; i < count; ++i)
    {
        free(items[i].name);
    }
    free(items);
}

/* 功能：处理 dirlist -a。实现原理：执行“收集->按名称排序->序列化发送->释放”的完整流程。 */
static int handle_dirlist_a(int client_fd)
{
    dir_item_t *items = NULL;
    size_t count = 0;
    int rc;

    if (collect_subdirs(&items, &count) != 0)
    {
        return send_all(client_fd, "No directory found\n", 19);
    }

    qsort(items, count, sizeof(dir_item_t), cmp_dir_by_name);
    rc = send_dirlist_line(client_fd, items, count);
    free_dir_items(items, count);
    return rc;
}

/* 功能：处理 dirlist -t。实现原理：执行“收集->按时间排序->序列化发送->释放”的完整流程。 */
static int handle_dirlist_t(int client_fd)
{
    dir_item_t *items = NULL;
    size_t count = 0;
    int rc;

    if (collect_subdirs(&items, &count) != 0)
    {
        return send_all(client_fd, "No directory found\n", 19);
    }

    qsort(items, count, sizeof(dir_item_t), cmp_dir_by_ctime);
    rc = send_dirlist_line(client_fd, items, count);
    free_dir_items(items, count);
    return rc;
}

/* 功能：递归查找首个同名文件。实现原理：对目录树做 DFS，命中即逐层返回，减少无意义遍历开销。 */
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

/* 功能：处理 fn filename。实现原理：先定位首个命中文件，再读取 stat 元数据并格式化为单行协议文本返回。 */
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

/* 功能：向文件列表追加一条路径。实现原理：先复制路径字符串，再通过 realloc 扩容数组并追加到尾部。 */
static int file_list_add(file_list_t *list, const char *path)
{
    char **tmp;
    char *copy;

    if (list == NULL || path == NULL)
    {
        return -1;
    }

    copy = dup_string(path);
    if (copy == NULL)
    {
        return -1;
    }

    tmp = (char **)realloc(list->paths, (list->count + 1) * sizeof(char *));
    if (tmp == NULL)
    {
        free(copy);
        return -1;
    }

    list->paths = tmp;
    list->paths[list->count++] = copy;
    return 0;
}

/* 功能：释放文件列表。实现原理：与构建顺序相反逐项释放，确保错误路径也能完整回收内存。 */
static void free_file_list(file_list_t *list)
{
    size_t i;

    if (list == NULL)
    {
        return;
    }

    for (i = 0; i < list->count; ++i)
    {
        free(list->paths[i]);
    }
    free(list->paths);
    list->paths = NULL;
    list->count = 0;
}

/* 功能：判断文件是否匹配大小区间。实现原理：按闭区间比较 st_size，作为 fz 命令过滤回调统一复用。 */
static int match_size_filter(const char *path, const struct stat *st, void *ctx)
{
    const size_filter_t *f = (const size_filter_t *)ctx;
    (void)path;

    if (st == NULL || f == NULL)
    {
        return 0;
    }
    return (st->st_size >= f->min_size && st->st_size <= f->max_size) ? 1 : 0;
}

/* 功能：判断文件是否匹配扩展名集合。实现原理：提取 basename 的最后扩展名并与过滤列表逐项精确匹配。 */
static int match_ext_filter(const char *path, const struct stat *st, void *ctx)
{
    const ext_filter_t *f = (const ext_filter_t *)ctx;
    const char *base;
    const char *dot;
    int i;
    (void)st;

    if (path == NULL || f == NULL || f->count <= 0)
    {
        return 0;
    }

    base = strrchr(path, '/');
    base = (base != NULL) ? (base + 1) : path;
    dot = strrchr(base, '.');
    if (dot == NULL || dot[1] == '\0')
    {
        return 0;
    }

    for (i = 0; i < f->count; ++i)
    {
        if (f->exts[i] != NULL && strcmp(dot + 1, f->exts[i]) == 0)
        {
            return 1;
        }
    }
    return 0;
}

/* 功能：判断文件是否匹配日期条件。实现原理：依据 before 标志执行“早于阈值”或“晚于等于阈值”比较。 */
static int match_date_filter(const char *path, const struct stat *st, void *ctx)
{
    const date_filter_t *f = (const date_filter_t *)ctx;
    (void)path;

    if (st == NULL || f == NULL)
    {
        return 0;
    }

    if (f->before)
    {
        return (st->st_ctime < f->threshold) ? 1 : 0;
    }
    return (st->st_ctime >= f->threshold) ? 1 : 0;
}

/* 功能：递归收集匹配文件。实现原理：先做深度上限检查，再 DFS 遍历；目录下探、文件匹配、失败上抛。 */
static int collect_matching_files_recursive(const char *dir_path,
                                            int (*matcher)(const char *, const struct stat *, void *),
                                            void *ctx,
                                            file_list_t *out,
                                            int depth,
                                            int max_depth)
{
    DIR *dir;
    struct dirent *ent;

    if (depth > max_depth)
    {
        return 0;
    }

    dir = opendir(dir_path);
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
            if (collect_matching_files_recursive(full, matcher, ctx, out, depth + 1, max_depth) != 0)
            {
                closedir(dir);
                return -1;
            }
            continue;
        }

        if (S_ISREG(st.st_mode) && matcher(full, &st, ctx))
        {
            if (file_list_add(out, full) != 0)
            {
                closedir(dir);
                return -1;
            }
        }
    }

    closedir(dir);
    return 0;
}

/* 功能：解析 YYYY-MM-DD 日期。实现原理：先做格式与范围校验，再构造 struct tm 并用 mktime 转为本地时间戳。 */
static int parse_date_ymd(const char *s, time_t *out)
{
    int y;
    int m;
    int d;
    struct tm tmv;
    time_t t;

    if (s == NULL || out == NULL)
    {
        return -1;
    }

    if (sscanf(s, "%d-%d-%d", &y, &m, &d) != 3)
    {
        return -1;
    }
    if (m < 1 || m > 12 || d < 1 || d > 31)
    {
        return -1;
    }

    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = y - 1900;
    tmv.tm_mon = m - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = 0;
    tmv.tm_min = 0;
    tmv.tm_sec = 0;
    tmv.tm_isdst = -1;

    t = mktime(&tmv);
    if (t == (time_t)-1)
    {
        return -1;
    }

    *out = t;
    return 0;
}

/* 功能：将文件列表打包为 tar.gz。实现原理：先写临时清单，再 fork+execl 调 tar，父进程 waitpid 校验退出码并清理。 */
static int create_temp_archive(const file_list_t *list, char *out_path, size_t out_size)
{
    char list_path[PATH_MAX];
    FILE *list_fp;
    size_t i;
    pid_t pid;
    time_t now;
    pid_t child;
    int status;
    struct sigaction old_chld;
    struct sigaction dfl_chld;

    if (list == NULL || out_path == NULL || out_size == 0 || list->count == 0)
    {
        return -1;
    }

    pid = getpid();
    now = time(NULL);
    if (snprintf(out_path, out_size, "/tmp/w26_temp_%ld.tar.gz", (long)pid) < 0)
    {
        return -1;
    }

    if (snprintf(list_path, sizeof(list_path), "/tmp/w26_list_%ld_%ld.txt", (long)pid, (long)now) < 0)
    {
        return -1;
    }

    list_fp = fopen(list_path, "w");
    if (list_fp == NULL)
    {
        return -1;
    }

    for (i = 0; i < list->count; ++i)
    {
        if (fprintf(list_fp, "%s\n", list->paths[i]) < 0)
        {
            fclose(list_fp);
            unlink(list_path);
            return -1;
        }
    }

    if (fclose(list_fp) != 0)
    {
        unlink(list_path);
        return -1;
    }

    memset(&dfl_chld, 0, sizeof(dfl_chld));
    dfl_chld.sa_handler = SIG_DFL;
    sigemptyset(&dfl_chld.sa_mask);
    if (sigaction(SIGCHLD, &dfl_chld, &old_chld) != 0)
    {
        unlink(list_path);
        unlink(out_path);
        return -1;
    }

    child = fork();
    if (child < 0)
    {
        (void)sigaction(SIGCHLD, &old_chld, NULL);
        unlink(list_path);
        unlink(out_path);
        return -1;
    }

    if (child == 0)
    {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0)
        {
            (void)dup2(devnull, STDOUT_FILENO);
            (void)dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execl("/usr/bin/tar", "tar", "-czf", out_path, "-T", list_path, (char *)NULL);
        _exit(127);
    }

    if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        (void)sigaction(SIGCHLD, &old_chld, NULL);
        unlink(list_path);
        unlink(out_path);
        return -1;
    }

    (void)sigaction(SIGCHLD, &old_chld, NULL);

    unlink(list_path);
    return 0;
}

/* 功能：发送压缩包给客户端。实现原理：先发送 FILE 大小头定义文本/二进制边界，再分块 read+send 直到 EOF。 */
static int send_archive_file(int client_fd, const char *archive_path)
{
    int fd;
    struct stat st;
    char header[64];
    char buf[4096];

    if (archive_path == NULL)
    {
        return -1;
    }

    if (stat(archive_path, &st) != 0 || !S_ISREG(st.st_mode))
    {
        return -1;
    }

    if (snprintf(header, sizeof(header), "FILE %ld\n", (long)st.st_size) < 0)
    {
        return -1;
    }
    if (send_all(client_fd, header, strlen(header)) != 0)
    {
        return -1;
    }

    fd = open(archive_path, O_RDONLY);
    if (fd < 0)
    {
        return -1;
    }

    for (;;)
    {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            close(fd);
            return -1;
        }
        if (n == 0)
        {
            break;
        }
        if (send_all(client_fd, buf, (size_t)n) != 0)
        {
            close(fd);
            return -1;
        }
    }

    close(fd);
    return 0;
}

/* 功能：执行过滤打包并回传。实现原理：统一执行“扫描->空集判定->打包->发送->清理”的流水线。 */
static int handle_archive_query(int client_fd,
                                int (*matcher)(const char *, const struct stat *, void *),
                                void *ctx)
{
    const char *root = get_search_root();
    int max_depth = get_max_scan_depth();
    file_list_t list = {0};
    char archive_path[PATH_MAX];
    int rc;

    rc = collect_matching_files_recursive(root, matcher, ctx, &list, 0, max_depth);
    if (rc != 0)
    {
        free_file_list(&list);
        return send_all(client_fd, "No file found\n", 14);
    }
    if (list.count == 0)
    {
        free_file_list(&list);
        return send_all(client_fd, "No file found\n", 14);
    }

    if (create_temp_archive(&list, archive_path, sizeof(archive_path)) != 0)
    {
        free_file_list(&list);
        return send_all(client_fd, "No file found\n", 14);
    }

    rc = send_archive_file(client_fd, archive_path);
    unlink(archive_path);
    free_file_list(&list);

    if (rc != 0)
    {
        return -1;
    }
    return 0;
}

/* 功能：处理 fz size1 size2。实现原理：解析并校验区间参数后构造 size_filter，复用统一归档发送流程。 */
static int handle_fz(int client_fd, const char *cmd)
{
    long min_size;
    long max_size;
    size_filter_t f;

    if (sscanf(cmd, "fz %ld %ld", &min_size, &max_size) != 2 || min_size < 0 || max_size < min_size)
    {
        return send_all(client_fd, "No file found\n", 14);
    }

    f.min_size = (off_t)min_size;
    f.max_size = (off_t)max_size;
    return handle_archive_query(client_fd, match_size_filter, &f);
}

/* 功能：处理 ft ext...。实现原理：解析最多 3 个扩展名并封装 ext_filter，复用统一归档发送流程。 */
static int handle_ft(int client_fd, const char *cmd)
{
    char e1[32];
    char e2[32];
    char e3[32];
    ext_filter_t f;
    int parts;

    e1[0] = '\0';
    e2[0] = '\0';
    e3[0] = '\0';
    parts = sscanf(cmd, "ft %31s %31s %31s", e1, e2, e3);
    if (parts < 1)
    {
        return send_all(client_fd, "No file found\n", 14);
    }

    memset(&f, 0, sizeof(f));
    f.exts[0] = e1;
    f.count = 1;
    if (parts >= 2)
    {
        f.exts[1] = e2;
        f.count = 2;
    }
    if (parts >= 3)
    {
        f.exts[2] = e3;
        f.count = 3;
    }

    return handle_archive_query(client_fd, match_ext_filter, &f);
}

/* 功能：处理 fdb/fda 日期命令。实现原理：将日期字符串转阈值时间后按方向过滤，再复用统一归档发送流程。 */
static int handle_fdx(int client_fd, const char *date_str, int before)
{
    date_filter_t f;

    if (parse_date_ymd(date_str, &f.threshold) != 0)
    {
        return send_all(client_fd, "No file found\n", 14);
    }
    f.before = before;
    return handle_archive_query(client_fd, match_date_filter, &f);
}

/*
 * 功能：创建并返回服务端监听套接字。
 * 实现原理：按 socket->setsockopt->bind->listen 典型流程创建监听端点，任一步失败都释放已申请资源。
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
 * 实现原理：逐字节读取到换行并过滤 '\r'；返回值区分“正常一行/对端关闭/错误”，供会话循环精确分支。
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
 * 实现原理：mirror2 作为最终设计中的真实业务节点，直接在本地完成控制命令、目录检索和归档命令处理；
 * 未匹配命令走 ACK 回退，便于保留调试与扩展空间。
 */
static int process_command(int client_fd, const char *cmd)
{
    char resp[MAX_COMMAND_LEN + 64];
    char date_buf[32];
    if (strcmp(cmd, "PING") == 0)
    {
        return send_all(client_fd, "PONG mirror2\n", 13);
    }
    if (strcmp(cmd, "dirlist -a") == 0)
    {
        return handle_dirlist_a(client_fd);
    }

    if (strcmp(cmd, "dirlist -t") == 0)
    {
        return handle_dirlist_t(client_fd);
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

    if (strncmp(cmd, "fz ", 3) == 0)
    {
        return handle_fz(client_fd, cmd);
    }

    if (strncmp(cmd, "ft ", 3) == 0)
    {
        return handle_ft(client_fd, cmd);
    }

    if (sscanf(cmd, "fdb %31s", date_buf) == 1)
    {
        return handle_fdx(client_fd, date_buf, 1);
    }

    if (sscanf(cmd, "fda %31s", date_buf) == 1)
    {
        return handle_fdx(client_fd, date_buf, 0);
    }

    if (cmd == NULL)
    {
        return -1;
    }

    /* 未匹配命令返回 ACK，保留调试与扩展入口。 */
    if (snprintf(resp, sizeof(resp), "ACK from %s: %s\n", NODE_NAME, cmd) < 0)
    {
        return -1;
    }

    return send_all(client_fd, resp, strlen(resp));
}

/*
 * 功能：处理单个客户端会话生命周期。
 * 实现原理：每个连接由子进程独占服务，循环执行“读一行->本地分发->回包”；quitc 主动回 BYE，
 * 异常或断连即结束会话，连接内不再做二次路由。
 */
static void crequest(int client_fd)
{
    /* 每个客户端连接由一个子进程独占处理。 */
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
 * 实现原理：父进程长期 accept，新连接按该节点的职责直接 fork 子进程独立处理；父进程关闭已复制的
 * 客户端 fd 后继续监听，实现进程级并发与连接级固定归属。
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
 * 功能：程序入口，初始化配置并启动 mirror2 服务端。
 * 实现原理：mirror2 先启动心跳线程向主服上报在线状态，再进入监听循环，作为最终设计中的实际业务节点
 * 直接处理其归属连接。
 */
int main(void)
{
    server_config_t cfg;
    /* TODO: 从命令行参数或配置文件读取监听地址与端口。 */
    /* TODO: 增加日志级别、守护进程模式等运行参数。 */
    /* 默认监听所有网卡。 */
    cfg.bind_host = "0.0.0.0";
    cfg.bind_port = DEFAULT_PORT;

    start_heartbeat_thread();

    printf("%s starting on port %d (skeleton)\n", NODE_NAME, cfg.bind_port);
    return run_server(&cfg);
}
