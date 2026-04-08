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

typedef struct server_config
{
    const char *bind_host;
    int bind_port;
} server_config_t;

typedef struct dir_item
{
    char *name;
    time_t ctime;
} dir_item_t;

typedef struct file_list
{
    char **paths;
    size_t count;
} file_list_t;

typedef struct size_filter
{
    off_t min_size;
    off_t max_size;
} size_filter_t;

typedef struct ext_filter
{
    const char *exts[3];
    int count;
} ext_filter_t;

typedef struct date_filter
{
    time_t threshold;
    int before;
} date_filter_t;

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
 * 功能：从状态文件读取镜像最近心跳时间。
 * 实现原理：读取 /tmp 文本文件中的两个 epoch 秒值（mirror1 与 mirror2）。
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
 * 实现原理：覆盖写入两个 epoch 秒值，供后续 GET_NODES 读取。
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
 * 实现原理：读取当前状态后仅更新目标节点时间戳，再写回状态文件。
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
 * 实现原理：主服务恒定在线，镜像按“当前时间 - 最近心跳时间 <= TTL”判定在线。
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
 * 实现原理：按 POSIX mode 位逐位映射为 9 字符权限表示。
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

/* 功能：目录名升序比较器。实现原理：qsort 回调，按 name 字段 strcmp。 */
static int cmp_dir_by_name(const void *a, const void *b)
{
    const dir_item_t *da = (const dir_item_t *)a;
    const dir_item_t *db = (const dir_item_t *)b;
    return strcmp(da->name, db->name);
}

/* 功能：目录时间升序比较器。实现原理：先按 ctime 比较，若相同再按名称比较。 */
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

/*
 * 功能：返回服务端搜索根目录。
 * 实现原理：优先使用 HOME 环境变量，不存在时回退当前目录。
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

/* 功能：返回递归扫描最大深度。实现原理：读取环境变量并做范围校验。 */
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

/* 功能：收集 HOME 下一级子目录。实现原理：遍历目录并记录目录名与时间字段。 */
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

/* 功能：以单行文本返回目录列表。实现原理：按指定顺序拼接目录名并换行结束。 */
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

/* 功能：释放目录集合内存。实现原理：逐个释放 name，再释放数组本体。 */
static void free_dir_items(dir_item_t *items, size_t count)
{
    size_t i;
    for (i = 0; i < count; ++i)
    {
        free(items[i].name);
    }
    free(items);
}

/* 功能：处理 dirlist -a 命令。实现原理：收集目录后按名称升序输出。 */
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

/* 功能：处理 dirlist -t 命令。实现原理：收集目录后按时间升序（最老优先）输出。 */
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
 * 实现原理：深度优先遍历目录树，命中即返回并终止后续搜索。
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
 * 实现原理：递归查找首个同名文件，返回名称/大小/时间/权限等元数据。
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

/* 功能：向文件列表追加一条路径。实现原理：realloc 扩容并复制字符串。 */
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

/* 功能：释放文件列表。实现原理：逐项释放路径字符串，再释放数组。 */
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

/* 功能：判断文件是否匹配大小区间。实现原理：比较 st_size 是否位于 [min, max]。 */
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

/* 功能：判断文件是否匹配扩展名集合。实现原理：从文件名尾部提取后缀并做精确比对。 */
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

/* 功能：判断文件是否匹配日期条件。实现原理：比较 st_ctime 与阈值时间。 */
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

/* 功能：递归收集匹配文件。实现原理：DFS 遍历目录树，对常规文件应用过滤函数。 */
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

/* 功能：解析 YYYY-MM-DD 日期。实现原理：拆分年月日并通过 mktime 转为 time_t。 */
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

/* 功能：将文件列表打包为 tar.gz。实现原理：写入临时列表后调用 tar -T。 */
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

/* 功能：发送压缩包给客户端。实现原理：先发 FILE 头，再按块发送二进制。 */
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

/* 功能：执行过滤打包并回传。实现原理：收集匹配文件，打包后发送 FILE 流。 */
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

/* 功能：处理 fz size1 size2。实现原理：按文件大小区间筛选并回传压缩包。 */
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

/* 功能：处理 ft ext...。实现原理：按最多 3 个扩展名筛选并回传压缩包。 */
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

/* 功能：处理 fdb/fda 日期命令。实现原理：按 ctime 与阈值的前后关系筛选。 */
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
    char nodes_line[128];
    char date_buf[32];

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

    /* 未识别命令返回 ACK，便于调试扩展命令分发。 */
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
 * 功能：程序入口，初始化配置并启动主服务端。
 * 实现原理：构造 server_config 后调用 run_server。
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
