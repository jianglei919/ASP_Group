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
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>

/* 主服务端基础配置 */
#define NODE_NAME "w26server"
#define DEFAULT_PORT 5000
#define BACKLOG 16
#define MAX_COMMAND_LEN 512
#define STATUS_FILE "/tmp/w26_nodes_status.txt"
#define HEARTBEAT_TTL_SEC 6
#define DEFAULT_MAX_SCAN_DEPTH 8
#define MIRROR1_HOST "127.0.0.1"
#define MIRROR1_PORT 5001
#define MIRROR2_HOST "127.0.0.1"
#define MIRROR2_PORT 5002

// 服务端配置结构体
typedef struct server_config
{
    const char *bind_host; // 监听地址，通常使用 0.0.0.0 绑定所有网卡
    int bind_port;         // 监听端口，客户端与镜像节点都通过该端口访问主服务
} server_config_t;

// 目录项结构体
typedef struct dir_item
{
    char *name;   // 目录名（不含父路径），用于输出 dirlist 结果
    time_t ctime; // 目录时间戳，用于 dirlist -t 排序
} dir_item_t;

// 文件列表结构体
typedef struct file_list
{
    char **paths; // 动态路径数组，每个元素是命中文件的绝对路径
    size_t count; // 当前命中文件数量
} file_list_t;

// 大小过滤器结构体
typedef struct size_filter
{
    off_t min_size; // 文件大小下界（含）
    off_t max_size; // 文件大小上界（含）
} size_filter_t;

// 扩展名过滤器结构体
typedef struct ext_filter
{
    const char *exts[3]; // 允许的扩展名集合，最多 3 个
    int count;           // 有效扩展名个数。
} ext_filter_t;

// 日期过滤器结构体
typedef struct date_filter
{
    time_t threshold; // 日期阈值时间戳（本地时区）
    int before;       // 1 表示 fdb(早于阈值)，0 表示 fda(晚于等于阈值)。
} date_filter_t;

typedef struct route_node
{
    const char *name;
    const char *host;
    int port;
    int online;
} route_node_t;

/*
 * 功能：可靠发送指定长度的数据。
 * 实现原理：TCP 的 send 可能发生“短写”，一次调用只发送部分数据；因此这里维护 sent 偏移并循环重试，
 * 直到累计发送字节数达到目标长度。若 send 被信号中断（EINTR）则继续重试；若返回 0 或不可恢复错误，
 * 视为连接异常并返回失败，交由上层结束当前会话。
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
 * 功能：从状态文件读取镜像最近心跳时间。
 * 实现原理：从固定状态文件读取两个 epoch 秒值，分别对应 mirror1 与 mirror2 的“最近一次上报时间”。
 * 若文件不存在或格式非法，函数将两个时间戳重置为 0 并返回失败，让调用方按“离线”语义处理，
 * 从而保证状态读取异常不会导致服务端崩溃。
 */
static int load_heartbeat_status(time_t *mirror1_ts, time_t *mirror2_ts)
{
    FILE *fp;
    long t1 = 0;
    long t2 = 0;

    if (mirror1_ts == NULL || mirror2_ts == NULL)
    {
        return -1;
    }

    fp = fopen(STATUS_FILE, "r");
    if (fp == NULL)
    {
        *mirror1_ts = (time_t)0;
        *mirror2_ts = (time_t)0;
        return -1;
    }

    if (fscanf(fp, "%ld %ld", &t1, &t2) != 2)
    {
        fclose(fp);
        *mirror1_ts = (time_t)0;
        *mirror2_ts = (time_t)0;
        return -1;
    }

    fclose(fp);
    *mirror1_ts = (time_t)t1;
    *mirror2_ts = (time_t)t2;
    return 0;
}

/*
 * 功能：写入镜像最近心跳时间到状态文件。
 * 实现原理：每次更新时使用覆盖写，确保状态文件始终只有一行最新快照，避免增量追加造成旧值干扰。
 * GET_NODES 在任何时刻读取的都是“最后一次成功更新”的一致状态。
 */
static int save_heartbeat_status(time_t mirror1_ts, time_t mirror2_ts)
{
    FILE *fp;

    fp = fopen(STATUS_FILE, "w");
    if (fp == NULL)
    {
        return -1;
    }

    if (fprintf(fp, "%ld %ld\n", (long)mirror1_ts, (long)mirror2_ts) < 0)
    {
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return 0;
}

/*
 * 功能：更新指定镜像节点的心跳时间。
 * 实现原理：先读取当前双节点时间戳，再只替换目标镜像的值为当前时间，最后整体写回文件。
 * 这种“读-改-写”方式能保留另一个镜像的已有状态，避免单节点上报时把另一节点误置零。
 */
static int update_heartbeat(const char *node_name)
{
    time_t mirror1_ts = (time_t)0;
    time_t mirror2_ts = (time_t)0;
    time_t now = time(NULL);

    if (node_name == NULL)
    {
        return -1;
    }

    (void)load_heartbeat_status(&mirror1_ts, &mirror2_ts);

    if (strcmp(node_name, "mirror1") == 0)
    {
        mirror1_ts = now;
    }
    else if (strcmp(node_name, "mirror2") == 0)
    {
        mirror2_ts = now;
    }
    else
    {
        return -1;
    }

    return save_heartbeat_status(mirror1_ts, mirror2_ts);
}

/*
 * 功能：生成节点在线表文本。
 * 实现原理：主服务端本地进程只要在运行即视为在线；镜像节点依据“当前时间与最近心跳时间差”
 * 是否在 TTL 窗口内判定在线。最终统一编码为单行协议文本，便于客户端快速解析并做选路决策。
 */
static int build_nodes_status_line(char *out, size_t out_size)
{
    time_t mirror1_ts = (time_t)0;
    time_t mirror2_ts = (time_t)0;
    time_t now = time(NULL);
    int mirror1_online;
    int mirror2_online;

    if (out == NULL || out_size == 0)
    {
        return -1;
    }

    (void)load_heartbeat_status(&mirror1_ts, &mirror2_ts);

    mirror1_online = (mirror1_ts > 0 && (now - mirror1_ts) <= HEARTBEAT_TTL_SEC) ? 1 : 0;
    mirror2_online = (mirror2_ts > 0 && (now - mirror2_ts) <= HEARTBEAT_TTL_SEC) ? 1 : 0;

    if (snprintf(out,
                 out_size,
                 "NODES w26server=1 mirror1=%d mirror2=%d\n",
                 mirror1_online,
                 mirror2_online) < 0)
    {
        return -1;
    }

    return 0;
}

/*
 * 功能：将文件权限位转换为 rwx 字符串。
 * 实现原理：按 owner/group/other 三组权限顺序读取 mode 位掩码，逐位映射为 rwx 或 -，
 * 生成固定长度 9 字符串并以 '\0' 结尾，供 fn 命令元数据输出直接复用。
 */
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

/* 功能：目录名升序比较器。实现原理：作为 qsort 回调按字典序比较 name 字段，用于 dirlist -a 稳定排序。 */
static int cmp_dir_by_name(const void *a, const void *b)
{
    const dir_item_t *da = (const dir_item_t *)a;
    const dir_item_t *db = (const dir_item_t *)b;
    return strcmp(da->name, db->name);
}

/* 功能：目录时间升序比较器。实现原理：先比较 ctime 实现“最老优先”，时间相同再按名称比较保证输出可重复。 */
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

/* 功能：复制字符串。实现原理：显式 malloc+memcpy 生成独立副本，避免依赖非标准环境下的 strdup 可用性。 */
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

/*
 * 功能：返回服务端搜索根目录。
 * 实现原理：优先读取 W26_SEARCH_ROOT 作为外部可控扫描根；若未设置则回退到 HOME；
 * 两者都不可用时最终回退当前目录，保证服务在最小环境下仍可执行。
 */
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

/* 功能：返回递归扫描最大深度。实现原理：读取 W26_MAX_SCAN_DEPTH 并做数值与边界校验，非法值统一回落默认值。 */
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

/* 功能：收集搜索根下一级子目录。实现原理：扫描根目录，仅挑选目录项并保存“目录名+ctime”到动态数组。 */
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

/* 功能：以单行文本返回目录列表。实现原理：按既定顺序把目录名用空格拼接后一次性换行，便于客户端按行读取。 */
static int send_dirlist_line(int client_fd, dir_item_t *items, size_t count)
{
    size_t i;

    if (count == 0)
    {
        return send_all(client_fd, "No directory found\n", 19);
    }

    for (i = 0; i < count; ++i)
    {
        size_t len = strlen(items[i].name);
        if (send_all(client_fd, items[i].name, len) != 0)
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

/* 功能：释放目录集合内存。实现原理：先释放每个 name，再释放承载数组，避免多次命令调用造成内存累积。 */
static void free_dir_items(dir_item_t *items, size_t count)
{
    size_t i;
    for (i = 0; i < count; ++i)
    {
        free(items[i].name);
    }
    free(items);
}

/* 功能：处理 dirlist -a 命令。实现原理：执行“收集->按名称排序->序列化发送->释放内存”的完整流程。 */
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

/* 功能：处理 dirlist -t 命令。实现原理：执行“收集->按时间排序->序列化发送->释放内存”的完整流程。 */
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

/*
 * 功能：递归查找首个同名文件。
 * 实现原理：对目录树执行 DFS，遇到目录继续下探，遇到常规文件则比对文件名；
 * 一旦命中立即逐层返回，避免无意义遍历，降低 fn 查询延迟。
 */
static int find_first_file(const char *dir_path, const char *target_name, char *out_path, size_t out_size)
{
    DIR *dir;
    struct dirent *ent;

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
            if (find_first_file(full, target_name, out_path, out_size))
            {
                closedir(dir);
                return 1;
            }
            continue;
        }

        if (S_ISREG(st.st_mode) && strcmp(ent->d_name, target_name) == 0)
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

/*
 * 功能：处理 fn filename 命令。
 * 实现原理：先调用递归查找定位首个命中文件，再读取 stat 生成元数据，
 * 最后格式化为单行协议文本返回。任一步失败统一返回 "File not found"，
 * 保持客户端处理分支简单。
 */
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

    if (!find_first_file(root, filename, path, sizeof(path)))
    {
        return send_all(client_fd, "File not found\n", 15);
    }

    if (stat(path, &st) != 0)
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

    if (snprintf(resp,
                 sizeof(resp),
                 "filename=%s size=%ld created=%s permissions=%s\n",
                 base,
                 (long)st.st_size,
                 time_buf,
                 perm) < 0)
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

/* 功能：释放文件列表。实现原理：与构建顺序相反逐项释放，确保打包失败或提前返回时也不泄漏。 */
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

/* 功能：判断文件是否匹配大小区间。实现原理：按闭区间比较 st_size，供 fz 过滤器作为统一回调复用。 */
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

/* 功能：判断文件是否匹配扩展名集合。实现原理：提取 basename 的最后扩展名，与 ext 列表逐项精确匹配。 */
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

/* 功能：判断文件是否匹配日期条件。实现原理：根据 before 标志选择“早于阈值”或“晚于等于阈值”比较。 */
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

/* 功能：递归收集匹配文件。实现原理：先做深度上限检查，再 DFS 遍历目录；目录节点递归下探，文件节点调用 matcher 判定。 */
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

/* 功能：解析 YYYY-MM-DD 日期。实现原理：先做格式和范围校验，再构造 struct tm 并用 mktime 统一转换成本地时区时间戳。 */
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

/* 功能：将文件列表打包为 tar.gz。实现原理：先生成临时清单文件，再 fork+execl 调用 tar；父进程 waitpid 校验退出码并清理临时文件。 */
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

/* 功能：发送压缩包给客户端。实现原理：先发送 "FILE <size>" 协议头定义文本/二进制边界，再分块 read+send 直到 EOF。 */
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

/* 功能：执行过滤打包并回传。实现原理：统一流水线为“扫描命中->空集快速返回->打包->发送->清理”，简化各命令实现。 */
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

/* 功能：处理 fz size1 size2。实现原理：解析并校验数值区间后构造 size_filter，再复用统一归档发送流水线。 */
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

/* 功能：处理 ft ext...。实现原理：解析最多 3 个扩展名并封装 ext_filter，随后复用统一归档发送流水线。 */
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

/* 功能：处理 fdb/fda 日期命令。实现原理：先把日期字符串转为阈值时间，再根据方向标志执行前后区间过滤并归档发送。 */
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
 * 实现原理：按 socket->setsockopt->bind->listen 典型流程创建监听端点；
 * 任一步失败立即关闭已申请资源并返回错误，避免 fd 泄漏。
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
 * 实现原理：逐字节读取直到换行，兼容 CRLF（忽略 '\r'）；连接关闭且未读到数据返回 0，
 * 正常读到一行返回 1，错误返回 -1，供会话循环做精确分支处理。
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

/* 功能：连接到指定后端节点。实现原理：创建 TCP 套接字并发起 connect，失败即关闭并返回错误。 */
static int connect_to_backend(const char *host, int port)
{
    int sock_fd;
    struct sockaddr_in addr;

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

    if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(sock_fd);
        return -1;
    }

    return sock_fd;
}

/* 功能：向后端发送一条命令行。实现原理：发送命令文本并追加换行，保持与前端协议一致。 */
static int send_backend_command(int backend_fd, const char *cmd)
{
    if (cmd == NULL)
    {
        return -1;
    }

    if (send_all(backend_fd, cmd, strlen(cmd)) != 0)
    {
        return -1;
    }
    return send_all(backend_fd, "\n", 1);
}

/* 功能：从后端读取一行文本。实现原理：逐字节读取直到换行，兼容 CRLF。 */
static int recv_backend_line(int backend_fd, char *buf, size_t size)
{
    size_t idx = 0;

    if (buf == NULL || size == 0)
    {
        return -1;
    }

    while (idx + 1 < size)
    {
        char ch;
        ssize_t n = recv(backend_fd, &ch, 1, 0);

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

/* 功能：按指定字节数中继后端二进制数据。实现原理：循环 recv->send，严格以协议大小为边界。 */
static int relay_backend_bytes(int backend_fd, int client_fd, long size)
{
    char buf[4096];
    long relayed = 0;

    while (relayed < size)
    {
        size_t need = (size_t)((size - relayed) > (long)sizeof(buf) ? sizeof(buf) : (size_t)(size - relayed));
        ssize_t n = recv(backend_fd, buf, need, 0);
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
        if (send_all(client_fd, buf, (size_t)n) != 0)
        {
            return -1;
        }
        relayed += (long)n;
    }

    return 0;
}

/* 功能：转发后端单次响应给客户端。实现原理：先读首行；文本直接回传，FILE 响应按大小透传二进制体。 */
static int forward_backend_response(int backend_fd, int client_fd)
{
    char line[MAX_COMMAND_LEN + 128];
    long file_size;

    if (recv_backend_line(backend_fd, line, sizeof(line)) != 0)
    {
        return -1;
    }

    if (sscanf(line, "FILE %ld", &file_size) == 1 && file_size >= 0)
    {
        char header[64];
        if (snprintf(header, sizeof(header), "FILE %ld\n", file_size) < 0)
        {
            return -1;
        }
        if (send_all(client_fd, header, strlen(header)) != 0)
        {
            return -1;
        }
        return relay_backend_bytes(backend_fd, client_fd, file_size);
    }

    if (send_all(client_fd, line, strlen(line)) != 0)
    {
        return -1;
    }
    return send_all(client_fd, "\n", 1);
}

/* 功能：把业务命令代理到镜像节点。实现原理：短连接发送命令后读取单次响应并透传给客户端。 */
static int proxy_business_command(int client_fd, const char *cmd, const route_node_t *node)
{
    int backend_fd;
    int rc;

    if (node == NULL || node->host == NULL)
    {
        return -1;
    }

    backend_fd = connect_to_backend(node->host, node->port);
    if (backend_fd < 0)
    {
        return -1;
    }

    rc = send_backend_command(backend_fd, cmd);
    if (rc == 0)
    {
        rc = forward_backend_response(backend_fd, client_fd);
    }

    close(backend_fd);
    return rc;
}

/* 功能：按题目连接序号规则计算优先节点。实现原理：1-2 主服、3-4 mirror1、5-6 mirror2，7 起循环。 */
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

/* 功能：判断命令是否属于需分流的业务命令。实现原理：按协议关键字匹配 P1/P2 文件检索命令。 */
static int is_business_command(const char *cmd)
{
    char date_buf[32];

    if (cmd == NULL)
    {
        return 0;
    }

    if (strcmp(cmd, "dirlist -a") == 0 || strcmp(cmd, "dirlist -t") == 0)
    {
        return 1;
    }
    if (strncmp(cmd, "fn ", 3) == 0 || strncmp(cmd, "fz ", 3) == 0 || strncmp(cmd, "ft ", 3) == 0)
    {
        return 1;
    }
    if (sscanf(cmd, "fdb %31s", date_buf) == 1 || sscanf(cmd, "fda %31s", date_buf) == 1)
    {
        return 1;
    }

    return 0;
}

/* 功能：在主服务本地执行业务命令。实现原理：复用现有处理函数，避免重复实现业务逻辑。 */
static int execute_local_business_command(int client_fd, const char *cmd)
{
    char date_buf[32];

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

    return -1;
}

/*
 * 功能：服务端分流
 * 实现原理：主服务端根据在线状态与连接序号先选优先节点，失败则在在线节点中故障跳过重试。
 */
static int route_business_command(int client_fd, const char *cmd, unsigned long *route_seq)
{
    route_node_t nodes[3] = {
        {"w26server", "127.0.0.1", DEFAULT_PORT, 1},
        {"mirror1", MIRROR1_HOST, MIRROR1_PORT, 0},
        {"mirror2", MIRROR2_HOST, MIRROR2_PORT, 0}};
    time_t mirror1_ts = (time_t)0;
    time_t mirror2_ts = (time_t)0;
    time_t now = time(NULL);
    int preferred;
    int attempt;

    if (!is_business_command(cmd))
    {
        return -2;
    }

    (void)load_heartbeat_status(&mirror1_ts, &mirror2_ts);
    nodes[1].online = (mirror1_ts > 0 && (now - mirror1_ts) <= HEARTBEAT_TTL_SEC) ? 1 : 0;
    nodes[2].online = (mirror2_ts > 0 && (now - mirror2_ts) <= HEARTBEAT_TTL_SEC) ? 1 : 0;

    preferred = preferred_index_by_seq((long)(*route_seq));
    (*route_seq)++;

    for (attempt = 0; attempt < 3; ++attempt)
    {
        int idx = (preferred + attempt) % 3;
        if (!nodes[idx].online)
        {
            continue;
        }

        if (idx == 0)
        {
            if (execute_local_business_command(client_fd, cmd) == 0)
            {
                return 0;
            }
            nodes[idx].online = 0;
            continue;
        }

        if (proxy_business_command(client_fd, cmd, &nodes[idx]) == 0)
        {
            return 0;
        }
        nodes[idx].online = 0;
    }

    return send_all(client_fd, "No server available\n", 20);
}

/*
 * 功能：处理单条客户端命令并返回响应。
 * 实现原理：按命令优先级进行字符串分发：先处理控制协议（PING/GET_NODES/HEARTBEAT），
 * 再处理目录与文件检索命令；未匹配命令统一走 ACK 回退，便于调试与协议扩展。
 */
static int process_command(int client_fd, const char *cmd, unsigned long *route_seq)
{
    char resp[MAX_COMMAND_LEN + 64];
    char nodes_line[128];
    int route_rc;

    if (cmd == NULL)
    {
        return -1;
    }

    if (strcmp(cmd, "PING") == 0)
    {
        return send_all(client_fd, "PONG w26server\n", 14);
    }

    if (strcmp(cmd, "GET_NODES") == 0)
    {
        if (build_nodes_status_line(nodes_line, sizeof(nodes_line)) != 0)
        {
            return send_all(client_fd, "NODES w26server=1 mirror1=0 mirror2=0\n", 38);
        }
        return send_all(client_fd, nodes_line, strlen(nodes_line));
    }

    if (strncmp(cmd, "HEARTBEAT ", 10) == 0)
    {
        const char *node_name = cmd + 10;
        if (update_heartbeat(node_name) == 0)
        {
            return send_all(client_fd, "HB_OK\n", 6);
        }
        return send_all(client_fd, "HB_ERR\n", 7);
    }

    route_rc = route_business_command(client_fd, cmd, route_seq);
    if (route_rc != -2)
    {
        return route_rc;
    }

    /* 未识别命令返回 ACK，便于调试扩展命令分发。 */
    if (snprintf(resp, sizeof(resp), "ACK from %s: %s\n", NODE_NAME, cmd) < 0)
    {
        return -1;
    }

    return send_all(client_fd, resp, strlen(resp));
}

/*
 * 功能：处理单个客户端会话生命周期。
 * 实现原理：在子进程内执行“读一行->分发->回包”的循环；收到 quitc 主动发送 BYE 后结束，
 * 或在读写错误/对端断开时退出，让父进程继续维持总服务可用性。
 */
static void crequest(int client_fd)
{
    /* 每个客户端连接由一个子进程独占处理 */
    char cmd[MAX_COMMAND_LEN];
    unsigned long route_seq = 1;

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

        if (process_command(client_fd, cmd, &route_seq) != 0)
        {
            break;
        }
    }
}

/*
 * 功能：启动服务端主循环并并发处理客户端。
 * 实现原理：主进程长期阻塞在 accept；每个新连接 fork 一个子进程独立处理，父进程立即关闭客户端 fd
 * 并回到 accept，形成进程级并发模型。通过忽略 SIGCHLD 自动回收僵尸子进程。
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
 * 功能：程序入口，初始化配置并启动主服务端。
 * 实现原理：设置监听参数与初始心跳状态，再进入 run_server 主循环；
 * 启动阶段只做最小必要初始化，避免把业务逻辑耦合到入口函数。
 */
int main(void)
{
    server_config_t cfg;
    /* TODO: 从命令行参数或配置文件读取监听地址与端口。 */
    /* TODO: 增加日志级别、守护进程模式等运行参数。 */
    /* 默认监听所有网卡 */
    cfg.bind_host = "0.0.0.0";
    cfg.bind_port = DEFAULT_PORT;

    /* 初始化状态文件，避免首次 GET_NODES 读取失败。 */
    (void)save_heartbeat_status((time_t)0, (time_t)0);

    printf("%s starting on port %d (skeleton)\n", NODE_NAME, cfg.bind_port);
    return run_server(&cfg);
}
