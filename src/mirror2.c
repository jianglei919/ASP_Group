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

/* Mirror server 2 basic configuration */
#define NODE_NAME "mirror2"
#define DEFAULT_PORT 5002
#define PRIMARY_HOST "127.0.0.1"
#define PRIMARY_PORT 5000
#define HEARTBEAT_INTERVAL_SEC 2
#define BACKLOG 16
#define MAX_COMMAND_LEN 512
#define DEFAULT_MAX_SCAN_DEPTH 8

// Server configuration structure
typedef struct server_config {
    const char *bind_host; // Bind address, usually 0.0.0.0 to bind all interfaces
    int bind_port; // Bind port, used by clients and mirrors to access primary service
} server_config_t;

// Directory item structure
typedef struct dir_item {
    char *name; // Directory name (without parent path), for dirlist output
    time_t ctime; // Directory timestamp, for dirlist -t sorting
} dir_item_t;

// File list structure
typedef struct file_list {
    char **paths; // Dynamic path array, each element is absolute path of matched file
    size_t count; // Current number of matched files
} file_list_t;

// Size filter structure
typedef struct size_filter {
    off_t min_size; // Minimum file size (inclusive)
    off_t max_size; // Maximum file size (inclusive)
} size_filter_t;

// Extension filter structure
typedef struct ext_filter {
    const char *exts[3]; // Allowed extensions set, max 3
    int count; // Number of valid extensions
} ext_filter_t;

// Date filter structure
typedef struct date_filter {
    time_t threshold; // Date threshold timestamp (local timezone)
    int before; // 1 for fdb (before), 0 for fda (after or equal)
} date_filter_t;

/*
 * Function: Reliably send data of specified length.
 * Principle: TCP send has short-write risk, so maintain sent offset and loop until target length is written;
 * Retry on EINTR if interrupted, return failure on 0 or unrecoverable error as connection exception.
 */
static int send_all(int fd, const char *data, size_t len) {
    /* Loop to send, avoid data loss caused by incomplete send */
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        sent += (size_t) n;
    }

    return 0;
}

/*
 * Function: Receive text response line by line.
 * Principle: Read byte by byte until '\n', filter '\r' in CRLF, ensure output is standard C string.
 * Used for short text protocol (like heartbeat reply) to avoid parsing ambiguity from recv packet joining.
 */
static int recv_line(int fd, char *buf, size_t size) {
    size_t idx = 0;

    if (buf == NULL || size == 0) {
        return -1;
    }

    while (idx + 1 < size) {
        char ch;
        ssize_t n = recv(fd, &ch, 1, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            break;
        }
        if (ch == '\n') {
            break;
        }
        if (ch != '\r') {
            buf[idx++] = ch;
        }
    }

    buf[idx] = '\0';
    return (idx > 0) ? 0 : -1;
}

/*
 * Function: Send a heartbeat to the primary server.
 * Principle: Create short connection per heartbeat, send fixed HEARTBEAT message, read reply and close immediately.
 * This stateless reporting model avoids long connection failure accumulation, recovered by next cycle on error.
 */
static int send_heartbeat_once(void) {
    int fd;
    struct sockaddr_in addr;
    char line[64];

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)PRIMARY_PORT);
    if (inet_pton(AF_INET, PRIMARY_HOST, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    /*
     * Send "HEARTBEAT mirror2\n", w26server crequest() will recognize HEARTBEAT prefix,
     * update timestamp for mirror2 in state file and reply HB_OK, without consuming client connection sequence.
     */
    if (send_all(fd, "HEARTBEAT mirror2\n", 18) != 0) {
        close(fd);
        return -1;
    }

    (void) recv_line(fd, line, sizeof(line));
    close(fd);
    return 0;
}

/*
 * Function: Background thread periodically sends heartbeat.
 * Principle: Infinite loop in thread: send heartbeat and sleep for fixed interval.
 * Single failure won't terminate thread, maintaining continuous self-healing online reporting capability.
 */
static void *heartbeat_thread_main(void *arg) {
    (void) arg;

    for (;;) {
        (void) send_heartbeat_once();
        sleep(HEARTBEAT_INTERVAL_SEC);
    }

    return NULL;
}

/*
 * Function: Start heartbeat thread.
 * Principle: Create background thread and detach immediately, making its lifecycle independent of main thread join;
 * Main thread can unblock and enter listen loop, running heartbeat and service concurrently.
 */
static void start_heartbeat_thread(void) {
    pthread_t tid;
    if (pthread_create(&tid, NULL, heartbeat_thread_main, NULL) == 0) {
        (void) pthread_detach(tid);
    }
}

/* Function: Convert file permission bits to rwx string. Principle: Bitwise mapping by owner/group/other, generating fixed 9-char permission string. */
static void format_permissions(mode_t mode, char out[10]) {
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

/* Function: Directory name ascending comparator. Principle: qsort callback comparing name field lexicographically, reused for dirlist -a. */
static int cmp_dir_by_name(const void *a, const void *b) {
    const dir_item_t *da = (const dir_item_t *) a;
    const dir_item_t *db = (const dir_item_t *) b;
    return strcmp(da->name, db->name);
}

/* Function: Directory time ascending comparator. Principle: Sort by ctime for oldest-first, fallback to name comparison if time equals to ensure stability. */
static int cmp_dir_by_ctime(const void *a, const void *b) {
    const dir_item_t *da = (const dir_item_t *) a;
    const dir_item_t *db = (const dir_item_t *) b;

    if (da->ctime < db->ctime) {
        return -1;
    }
    if (da->ctime > db->ctime) {
        return 1;
    }
    return strcmp(da->name, db->name);
}

/* Function: Duplicate string. Principle: Explicit malloc+memcpy for independent copy, avoiding portability issues with non-standard strdup. */
static char *dup_string(const char *s) {
    size_t len;
    char *p;

    if (s == NULL) {
        return NULL;
    }

    len = strlen(s);
    p = (char *) malloc(len + 1);
    if (p == NULL) {
        return NULL;
    }
    memcpy(p, s, len + 1);
    return p;
}

/* Function: Get search root directory. Principle: Prefer W26_SEARCH_ROOT, fallback to HOME, then current directory. */
static const char *get_search_root(void) {
    const char *custom = getenv("W26_SEARCH_ROOT");
    const char *home = getenv("HOME");

    if (custom != NULL && custom[0] != '\0') {
        return custom;
    }
    return (home != NULL && home[0] != '\0') ? home : ".";
}

/* Function: Get max recursive scan depth. Principle: Read W26_MAX_SCAN_DEPTH with validation, fallback invalid values to default depth. */
static int get_max_scan_depth(void) {
    const char *s = getenv("W26_MAX_SCAN_DEPTH");
    char *end = NULL;
    long v;

    if (s == NULL || s[0] == '\0') {
        return DEFAULT_MAX_SCAN_DEPTH;
    }

    v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < 1 || v > 64) {
        return DEFAULT_MAX_SCAN_DEPTH;
    }

    return (int) v;
}

/* Function: Collect immediate subdirectories of search root. Principle: Traverse root, extract only directory items with name and timestamp for sorting output. */
static int collect_subdirs(dir_item_t **out_items, size_t *out_count) {
    const char *root = get_search_root();
    DIR *dir;
    struct dirent *ent;
    dir_item_t *items = NULL;
    size_t count = 0;
    int ok = 0;

    if (out_items == NULL || out_count == NULL) {
        return -1;
    }

    *out_items = NULL;
    *out_count = 0;

    dir = opendir(root);
    if (dir == NULL) {
        return -1;
    }

    while ((ent = readdir(dir)) != NULL) {
        char full[PATH_MAX];
        struct stat st;
        char *name_copy;
        dir_item_t *tmp;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (snprintf(full, sizeof(full), "%s/%s", root, ent->d_name) < 0) {
            continue;
        }
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }

        name_copy = dup_string(ent->d_name);
        if (name_copy == NULL) {
            ok = -1;
            break;
        }
        tmp = (dir_item_t *) realloc(items, (count + 1) * sizeof(dir_item_t));
        if (tmp == NULL) {
            free(name_copy);
            ok = -1;
            break;
        }
        items = tmp;
        items[count].name = name_copy;
        items[count].ctime = st.st_mtime;
        count++;
    }
    closedir(dir);

    if (ok != 0) {
        size_t i;
        for (i = 0; i < count; ++i) {
            free(items[i].name);
        }
        free(items);
        return -1;
    }

    *out_items = items;
    *out_count = count;
    return 0;
}

/* Function: Return directory list as single text line. Principle: Concatenate names with spaces and append newline in order, for client to read by line. */
static int send_dirlist_line(int client_fd, dir_item_t *items, size_t count) {
    size_t i;

    if (count == 0) {
        return send_all(client_fd, "No directory found\n", 19);
    }

    for (i = 0; i < count; ++i) {
        if (send_all(client_fd, items[i].name, strlen(items[i].name)) != 0) {
            return -1;
        }
        if (i + 1 < count) {
            if (send_all(client_fd, " ", 1) != 0) {
                return -1;
            }
        }
    }

    return send_all(client_fd, "\n", 1);
}

/* Function: Free directory item collection memory. Principle: Free each name first, then array, avoiding memory leaks on repeated requests. */
static void free_dir_items(dir_item_t *items, size_t count) {
    size_t i;
    for (i = 0; i < count; ++i) {
        free(items[i].name);
    }
    free(items);
}

/* Function: Handle dirlist -a. Principle: Full pipeline of collect -> sort by name -> serialize send -> free. */
static int handle_dirlist_a(int client_fd) {
    dir_item_t *items = NULL;
    size_t count = 0;
    int rc;

    if (collect_subdirs(&items, &count) != 0) {
        return send_all(client_fd, "No directory found\n", 19);
    }

    qsort(items, count, sizeof(dir_item_t), cmp_dir_by_name);
    rc = send_dirlist_line(client_fd, items, count);
    free_dir_items(items, count);
    return rc;
}

/* Function: Handle dirlist -t. Principle: Full pipeline of collect -> sort by time -> serialize send -> free. */
static int handle_dirlist_t(int client_fd) {
    dir_item_t *items = NULL;
    size_t count = 0;
    int rc;

    if (collect_subdirs(&items, &count) != 0) {
        return send_all(client_fd, "No directory found\n", 19);
    }

    qsort(items, count, sizeof(dir_item_t), cmp_dir_by_ctime);
    rc = send_dirlist_line(client_fd, items, count);
    free_dir_items(items, count);
    return rc;
}

/* Function: Recursively find first file by name. Principle: DFS on directory tree, return immediately on hit, reducing meaningless traversal overhead. */
static int find_first_file(const char *dir_path, const char *target_name, char *out_path, size_t out_size) {
    DIR *dir = opendir(dir_path);
    struct dirent *ent;

    if (dir == NULL) {
        return 0;
    }

    while ((ent = readdir(dir)) != NULL) {
        char full[PATH_MAX];
        struct stat st;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        if (snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name) < 0) {
            continue;
        }
        if (stat(full, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (find_first_file(full, target_name, out_path, out_size)) {
                closedir(dir);
                return 1;
            }
        } else if (S_ISREG(st.st_mode) && strcmp(ent->d_name, target_name) == 0) {
            if (snprintf(out_path, out_size, "%s", full) >= 0) {
                closedir(dir);
                return 1;
            }
        }
    }

    closedir(dir);
    return 0;
}

/* Function: Handle fn filename. Principle: Locate first matched file, read stat metadata, format as single line protocol text and return. */
static int handle_fn(int client_fd, const char *filename) {
    const char *root = get_search_root();
    char path[PATH_MAX];
    struct stat st;
    char perm[10];
    char time_buf[32];
    struct tm *tm_info;
    char resp[1024];
    const char *base;

    if (filename == NULL || filename[0] == '\0') {
        return send_all(client_fd, "File not found\n", 15);
    }
    if (!find_first_file(root, filename, path, sizeof(path)) || stat(path, &st) != 0) {
        return send_all(client_fd, "File not found\n", 15);
    }

    format_permissions(st.st_mode, perm);
    tm_info = localtime(&st.st_ctime);
    if (tm_info == NULL) {
        snprintf(time_buf, sizeof(time_buf), "unknown");
    } else {
        (void) strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
    }

    base = strrchr(path, '/');
    base = (base != NULL) ? (base + 1) : path;
    if (snprintf(resp, sizeof(resp), "filename=%s size=%ld created=%s permissions=%s\n", base, (long)st.st_size,
                 time_buf, perm) < 0) {
        return -1;
    }

    return send_all(client_fd, resp, strlen(resp));
}

/* Function: Append path to file list. Principle: Duplicate string, realloc array and append to end. */
static int file_list_add(file_list_t *list, const char *path) {
    char **tmp;
    char *copy;

    if (list == NULL || path == NULL) {
        return -1;
    }

    copy = dup_string(path);
    if (copy == NULL) {
        return -1;
    }

    tmp = (char **) realloc(list->paths, (list->count + 1) * sizeof(char *));
    if (tmp == NULL) {
        free(copy);
        return -1;
    }

    list->paths = tmp;
    list->paths[list->count++] = copy;
    return 0;
}

/* Function: Free file list. Principle: Release items in reverse order of construction, ensuring full memory recovery even on error paths. */
static void free_file_list(file_list_t *list) {
    size_t i;

    if (list == NULL) {
        return;
    }

    for (i = 0; i < list->count; ++i) {
        free(list->paths[i]);
    }
    free(list->paths);
    list->paths = NULL;
    list->count = 0;
}

/* Function: Check if file matches size range. Principle: Inclusive comparison on st_size, reused as filter callback for fz command. */
static int match_size_filter(const char *path, const struct stat *st, void *ctx) {
    const size_filter_t *f = (const size_filter_t *) ctx;
    (void) path;

    if (st == NULL || f == NULL) {
        return 0;
    }
    return (st->st_size >= f->min_size && st->st_size <= f->max_size) ? 1 : 0;
}

/* Function: Check if file matches extension set. Principle: Extract last extension from basename and precisely match against filter list. */
static int match_ext_filter(const char *path, const struct stat *st, void *ctx) {
    const ext_filter_t *f = (const ext_filter_t *) ctx;
    const char *base;
    const char *dot;
    int i;
    (void) st;

    if (path == NULL || f == NULL || f->count <= 0) {
        return 0;
    }

    base = strrchr(path, '/');
    base = (base != NULL) ? (base + 1) : path;
    dot = strrchr(base, '.');
    if (dot == NULL || dot[1] == '\0') {
        return 0;
    }

    for (i = 0; i < f->count; ++i) {
        if (f->exts[i] != NULL && strcmp(dot + 1, f->exts[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Function: Check if file matches date condition. Principle: Perform before/after-or-equal comparison based on before flag. */
static int match_date_filter(const char *path, const struct stat *st, void *ctx) {
    const date_filter_t *f = (const date_filter_t *) ctx;
    (void) path;

    if (st == NULL || f == NULL) {
        return 0;
    }

    if (f->before) {
        return (st->st_mtime < f->threshold) ? 1 : 0;
    }
    return (st->st_mtime >= f->threshold) ? 1 : 0;
}

/* Function: Recursively collect matching files. Principle: Depth limit check, then DFS traversal; probe directories, match files, bubble up failures. */
static int collect_matching_files_recursive(const char *dir_path,
                                            int (*matcher)(const char *, const struct stat *, void *),
                                            void *ctx,
                                            file_list_t *out,
                                            int depth,
                                            int max_depth) {
    DIR *dir;
    struct dirent *ent;

    if (depth > max_depth) {
        return 0;
    }

    dir = opendir(dir_path);
    if (dir == NULL) {
        return 0;
    }

    while ((ent = readdir(dir)) != NULL) {
        char full[PATH_MAX];
        struct stat st;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name) < 0) {
            continue;
        }
        if (stat(full, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (collect_matching_files_recursive(full, matcher, ctx, out, depth + 1, max_depth) != 0) {
                closedir(dir);
                return -1;
            }
            continue;
        }

        if (S_ISREG(st.st_mode) && matcher(full, &st, ctx)) {
            if (file_list_add(out, full) != 0) {
                closedir(dir);
                return -1;
            }
        }
    }

    closedir(dir);
    return 0;
}

/* Function: Parse YYYY-MM-DD date. Principle: Format and range check, construct struct tm and convert to local timestamp using mktime. */
static int parse_date_ymd(const char *s, time_t *out) {
    int y;
    int m;
    int d;
    struct tm tmv;
    time_t t;

    if (s == NULL || out == NULL) {
        return -1;
    }

    if (sscanf(s, "%d-%d-%d", &y, &m, &d) != 3) {
        return -1;
    }
    if (m < 1 || m > 12 || d < 1 || d > 31) {
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
    if (t == (time_t) -1) {
        return -1;
    }

    *out = t;
    return 0;
}

/* Function: Pack file list to tar.gz. Principle: Write temp list, fork+execl tar, parent waitpid checks exit code and cleans up. */
static int create_temp_archive(const file_list_t *list, char *out_path, size_t out_size) {
    char list_path[PATH_MAX];
    FILE *list_fp;
    size_t i;
    pid_t pid;
    time_t now;
    pid_t child;
    int status;
    struct sigaction old_chld;
    struct sigaction dfl_chld;

    if (list == NULL || out_path == NULL || out_size == 0 || list->count == 0) {
        return -1;
    }

    pid = getpid();
    now = time(NULL);
    if (snprintf(out_path, out_size, "/tmp/w26_temp_%ld.tar.gz", (long)pid) < 0) {
        return -1;
    }

    if (snprintf(list_path, sizeof(list_path), "/tmp/w26_list_%ld_%ld.txt", (long)pid, (long)now) < 0) {
        return -1;
    }

    list_fp = fopen(list_path, "w");
    if (list_fp == NULL) {
        return -1;
    }

    for (i = 0; i < list->count; ++i) {
        if (fprintf(list_fp, "%s\n", list->paths[i]) < 0) {
            fclose(list_fp);
            unlink(list_path);
            return -1;
        }
    }

    if (fclose(list_fp) != 0) {
        unlink(list_path);
        return -1;
    }

    memset(&dfl_chld, 0, sizeof(dfl_chld));
    dfl_chld.sa_handler = SIG_DFL;
    sigemptyset(&dfl_chld.sa_mask);
    if (sigaction(SIGCHLD, &dfl_chld, &old_chld) != 0) {
        unlink(list_path);
        unlink(out_path);
        return -1;
    }

    child = fork();
    if (child < 0) {
        (void) sigaction(SIGCHLD, &old_chld, NULL);
        unlink(list_path);
        unlink(out_path);
        return -1;
    }

    if (child == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            (void) dup2(devnull, STDOUT_FILENO);
            (void) dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execl("/usr/bin/tar", "tar", "-czf", out_path, "-T", list_path, (char *) NULL);
        _exit(127);
    }

    if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        (void) sigaction(SIGCHLD, &old_chld, NULL);
        unlink(list_path);
        unlink(out_path);
        return -1;
    }

    (void) sigaction(SIGCHLD, &old_chld, NULL);

    unlink(list_path);
    return 0;
}

/* Function: Send archive to client. Principle: Send FILE size header to define text/binary boundary, then chunked read+send until EOF. */
static int send_archive_file(int client_fd, const char *archive_path) {
    int fd;
    struct stat st;
    char header[64];
    char buf[4096];

    if (archive_path == NULL) {
        return -1;
    }

    if (stat(archive_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return -1;
    }

    if (snprintf(header, sizeof(header), "FILE %ld\n", (long)st.st_size) < 0) {
        return -1;
    }
    if (send_all(client_fd, header, strlen(header)) != 0) {
        return -1;
    }

    fd = open(archive_path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return -1;
        }
        if (n == 0) {
            break;
        }
        if (send_all(client_fd, buf, (size_t) n) != 0) {
            close(fd);
            return -1;
        }
    }

    close(fd);
    return 0;
}

/* Function: Execute filtered packing and return. Principle: Unified pipeline of scan -> empty check -> pack -> send -> clean. */
static int handle_archive_query(int client_fd,
                                int (*matcher)(const char *, const struct stat *, void *),
                                void *ctx) {
    const char *root = get_search_root();
    int max_depth = get_max_scan_depth();
    file_list_t list = {0};
    char archive_path[PATH_MAX];
    int rc;

    rc = collect_matching_files_recursive(root, matcher, ctx, &list, 0, max_depth);
    if (rc != 0) {
        free_file_list(&list);
        return send_all(client_fd, "No file found\n", 14);
    }
    if (list.count == 0) {
        free_file_list(&list);
        return send_all(client_fd, "No file found\n", 14);
    }

    if (create_temp_archive(&list, archive_path, sizeof(archive_path)) != 0) {
        free_file_list(&list);
        return send_all(client_fd, "No file found\n", 14);
    }

    rc = send_archive_file(client_fd, archive_path);
    unlink(archive_path);
    free_file_list(&list);

    if (rc != 0) {
        return -1;
    }
    return 0;
}

/* Function: Handle fz size1 size2. Principle: Parse and validate range params, construct size_filter, reuse unified archive send pipeline. */
static int handle_fz(int client_fd, const char *cmd) {
    long min_size;
    long max_size;
    size_filter_t f;

    if (sscanf(cmd, "fz %ld %ld", &min_size, &max_size) != 2 || min_size < 0 || max_size < min_size) {
        return send_all(client_fd, "No file found\n", 14);
    }

    f.min_size = (off_t) min_size;
    f.max_size = (off_t) max_size;
    return handle_archive_query(client_fd, match_size_filter, &f);
}

/* Function: Handle ft ext... Principle: Parse up to 3 extensions, wrap in ext_filter, reuse unified archive send pipeline. */
static int handle_ft(int client_fd, const char *cmd) {
    char e1[32];
    char e2[32];
    char e3[32];
    ext_filter_t f;
    int parts;

    e1[0] = '\0';
    e2[0] = '\0';
    e3[0] = '\0';
    parts = sscanf(cmd, "ft %31s %31s %31s", e1, e2, e3);
    if (parts < 1) {
        return send_all(client_fd, "No file found\n", 14);
    }

    memset(&f, 0, sizeof(f));
    f.exts[0] = e1;
    f.count = 1;
    if (parts >= 2) {
        f.exts[1] = e2;
        f.count = 2;
    }
    if (parts >= 3) {
        f.exts[2] = e3;
        f.count = 3;
    }

    return handle_archive_query(client_fd, match_ext_filter, &f);
}

/* Function: Handle fdb/fda date commands. Principle: Convert date string to threshold time, filter by direction, reuse unified archive send pipeline. */
static int handle_fdx(int client_fd, const char *date_str, int before) {
    date_filter_t f;

    if (parse_date_ymd(date_str, &f.threshold) != 0) {
        return send_all(client_fd, "No file found\n", 14);
    }
    f.before = before;
    return handle_archive_query(client_fd, match_date_filter, &f);
}

/*
 * Function: Create and return server listen socket.
 * Principle: Typical pipeline of socket->setsockopt->bind->listen, release resources on any failure.
 */
static int create_listen_socket(const server_config_t *cfg) {
    int sock_fd;
    int opt = 1;
    struct sockaddr_in addr;

    if (cfg == NULL) {
        return -1;
    }

    /* 1) Create TCP socket */
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        return -1;
    }

    /* 2) Allow fast port reuse for easy service restart */
    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        close(sock_fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)cfg->bind_port);

    if (cfg->bind_host != NULL && strcmp(cfg->bind_host, "0.0.0.0") != 0) {
        if (inet_pton(AF_INET, cfg->bind_host, &addr.sin_addr) != 1) {
            close(sock_fd);
            return -1;
        }
    } else {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    /* 3) Bind address and port */
    if (bind(sock_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        close(sock_fd);
        return -1;
    }

    /* 4) Start listening for client connections */
    if (listen(sock_fd, BACKLOG) < 0) {
        close(sock_fd);
        return -1;
    }

    return sock_fd;
}

/*
 * Function: Read a command line from client connection.
 * Principle: Read byte by byte until newline, filter '\r'; return value distinguishes normal line/peer closed/error, for precise session loop branching.
 */
static int read_command_line(int client_fd, char *buf, size_t size) {
    /* Read command line by line, '\n' as request boundary */
    size_t idx = 0;

    if (buf == NULL || size == 0) {
        return -1;
    }

    while (idx + 1 < size) {
        char ch;
        ssize_t n = recv(client_fd, &ch, 1, 0);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            if (idx == 0) {
                return 0;
            }
            break;
        }

        if (ch == '\n') {
            break;
        }

        if (ch != '\r') {
            buf[idx++] = ch;
        }
    }

    buf[idx] = '\0';
    return 1;
}

/*
 * Function: Process single client command and return response.
 * Principle: mirror2 as real service node, locally completes control commands, directory retrieval and archiving;
 * Unmatched commands fallback to ACK, preserving debug and extension space.
 */
static int process_command(int client_fd, const char *cmd) {
    char resp[MAX_COMMAND_LEN + 64];
    char date_buf[32];

    /*
     * CONNECT_PROBE: Probe command sent by client after getting REDIRECT from w26server to this node.
     * Reply "CONNECTED mirrorN host port", client confirms reaching final node and shows node info.
     */
    if (strcmp(cmd, "CONNECT_PROBE") == 0) {
        char line_buf[128];
        snprintf(line_buf, sizeof(line_buf),
                 "CONNECTED %s %s %d\n",
                 NODE_NAME, "127.0.0.1", DEFAULT_PORT);
        return send_all(client_fd, line_buf, strlen(line_buf));
    }

    if (strcmp(cmd, "dirlist -a") == 0) {
        return handle_dirlist_a(client_fd);
    }

    if (strcmp(cmd, "dirlist -t") == 0) {
        return handle_dirlist_t(client_fd);
    }

    if (strncmp(cmd, "fn ", 3) == 0) {
        const char *filename = cmd + 3;
        while (*filename == ' ') {
            filename++;
        }
        return handle_fn(client_fd, filename);
    }

    if (strncmp(cmd, "fz ", 3) == 0) {
        return handle_fz(client_fd, cmd);
    }

    if (strncmp(cmd, "ft ", 3) == 0) {
        return handle_ft(client_fd, cmd);
    }

    if (sscanf(cmd, "fdb %31s", date_buf) == 1) {
        return handle_fdx(client_fd, date_buf, 1);
    }

    if (sscanf(cmd, "fda %31s", date_buf) == 1) {
        return handle_fdx(client_fd, date_buf, 0);
    }

    if (cmd == NULL) {
        return -1;
    }

    /* Unmatched commands return ACK, preserving debugging and extension entry. */
    if (snprintf(resp, sizeof(resp), "ACK from %s: %s\n", NODE_NAME, cmd) < 0) {
        return -1;
    }

    return send_all(client_fd, resp, strlen(resp));
}

/*
 * Function: Handle single client session lifecycle.
 * Principle: Each connection exclusively served by child process, loops read line -> local dispatch -> reply; quitc actively replies BYE,
 * ends session on exception or disconnect, no secondary routing within connection.
 */
static void crequest(int client_fd) {
    /* Each client connection exclusively handled by a child process. */
    char cmd[MAX_COMMAND_LEN];

    for (;;) {
        /* Session loop: continually read command until disconnect or quitc */
        int rc = read_command_line(client_fd, cmd, sizeof(cmd));
        if (rc <= 0) {
            break;
        }

        if (strcmp(cmd, "quitc") == 0) {
            /* Client actively ends session */
            (void) send_all(client_fd, "BYE\n", 4);
            break;
        }

        if (process_command(client_fd, cmd) != 0) {
            break;
        }
    }
}

/*
 * Function: Start server main loop and concurrently handle clients.
 * Principle: Parent long-term accept, fork child for new connection to handle independently based on node duty; parent closes duplicated
 * client fd and continues listening, achieving process-level concurrency and connection-level fixed attribution.
 */
static int run_server(const server_config_t *cfg) {
    int listen_fd = create_listen_socket(cfg);
    if (listen_fd < 0) {
        fprintf(stderr, "%s: failed to create listen socket\n", NODE_NAME);
        return 1;
    }

    /* Auto reap child process exit to avoid zombies */
    signal(SIGCHLD, SIG_IGN);

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        /* Main process blocks waiting for new connection */
        int client_fd = accept(listen_fd, (struct sockaddr *) &client_addr, &client_len);

        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            continue;
        }

        printf("%s: client connected\n", NODE_NAME);
        fflush(stdout);

        /* Fork a child process to exclusively handle each client connection */
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            close(client_fd);
            continue;
        }

        if (pid == 0) {
            /* Child process handles only current connection, exits when done */
            close(listen_fd);
            crequest(client_fd);
            close(client_fd);
            _exit(0);
        }

        /* Parent process closes duplicated client fd and continues accept */
        close(client_fd);
    }

    close(listen_fd);
    return 0;
}

/*
 * Function: Program entry, initialize config and start mirror2 server.
 * Principle: mirror2 starts heartbeat thread to report online status to primary server, then enters listen loop, as actual service node
 * in final design, directly handling its attributed connections.
 */
int main(void) {
    server_config_t cfg;
    /* Listen on all interfaces by default. */
    cfg.bind_host = "0.0.0.0";
    cfg.bind_port = DEFAULT_PORT;

    start_heartbeat_thread();

    printf("%s starting on port %d\n", NODE_NAME, cfg.bind_port);
    return run_server(&cfg);
}
