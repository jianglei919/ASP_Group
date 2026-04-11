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

/* Primary server basic configuration */
#define NODE_NAME "w26server"
#define DEFAULT_PORT 5000
#define BACKLOG 16
#define MAX_COMMAND_LEN 512
#define STATUS_FILE "/tmp/w26_nodes_status.txt"
#define CLIENT_SEQ_FILE "/tmp/w26_client_seq.txt"
#define HEARTBEAT_TTL_SEC 6
#define DEFAULT_MAX_SCAN_DEPTH 8
#define MIRROR1_HOST "127.0.0.1"
#define MIRROR1_PORT 5001
#define MIRROR2_HOST "127.0.0.1"
#define MIRROR2_PORT 5002

// Server configuration structure
typedef struct server_config {
    const char *bind_host; // Listen address, typically 0.0.0.0
    int bind_port; // Listen port, clients and mirror nodes access primary service here
} server_config_t;

// Directory item structure
typedef struct dir_item {
    char *name; // Directory name (without parent path), for dirlist output
    time_t ctime; // Directory timestamp, for dirlist -t sorting
} dir_item_t;

// File list structure
typedef struct file_list {
    char **paths; // Dynamic path array, each element is the absolute path of a hit file
    size_t count; // Current number of hit files
} file_list_t;

// Size filter structure
typedef struct size_filter {
    off_t min_size; // Minimum file size (inclusive)
    off_t max_size; // Maximum file size (inclusive)
} size_filter_t;

// Extension filter structure
typedef struct ext_filter {
    const char *exts[3]; // Allowed extensions, up to 3
    int count; // Number of valid extensions.
} ext_filter_t;

// Date filter structure
typedef struct date_filter {
    time_t threshold; // Date threshold timestamp (local timezone)
    int before; // 1 for fdb (before threshold), 0 for fda (after or equal)
} date_filter_t;

// Routing node structure
typedef struct route_node {
    const char *name; // Node name
    const char *host; // Node listen address
    int port; // Node listen port
    int online; // Online flag, 1 for valid heartbeat, 0 for offline
} route_node_t;

/*
 * Function: Reliably send data of specified length.
 * Principle: TCP send may short-write, so loop and retry with an offset until the specified length is sent.
 * Handles EINTR interrupts; treats 0 or other errors as connection failures.
 */
static int send_all(int fd, const char *data, size_t len) {
    /* Loop sending to avoid data loss on incomplete send */
    size_t sent = 0;

    while (sent < len) {
        // Send the data to the client
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
 * Function: Read the latest heartbeat times of mirrors from the state file.
 * Principle: Reads two epoch seconds from a fixed state file for mirror1 and mirror2.
 * If missing or invalid, initializes to 0 and returns safely to default to offline,
 * ensuring state read errors do not crash the server.
 */
static int load_heartbeat_status(time_t *mirror1_ts, time_t *mirror2_ts) {
    FILE *fp;
    long t1 = 0;
    long t2 = 0;

    if (mirror1_ts == NULL || mirror2_ts == NULL) {
        return -1;
    }
    // Open the status file for reading
    fp = fopen(STATUS_FILE, "r");
    if (fp == NULL) {
        *mirror1_ts = (time_t) 0;
        *mirror2_ts = (time_t) 0;
        return -1;
    }
    // Read the heartbeat times from the status file
    if (fscanf(fp, "%ld %ld", &t1, &t2) != 2) {
        fclose(fp);
        *mirror1_ts = (time_t) 0;
        *mirror2_ts = (time_t) 0;
        return -1;
    }

    fclose(fp);
    *mirror1_ts = (time_t) t1;
    *mirror2_ts = (time_t) t2;
    return 0;
}

/*
 * Function: Write mirror heartbeat times to the state file.
 * Principle: Overwrites the state file on every update to ensure a single up-to-date snapshot.
 * GET_NODES always reads the consistently updated state.
 */
static int save_heartbeat_status(time_t mirror1_ts, time_t mirror2_ts) {
    FILE *fp;
    // Open the status file for writing
    fp = fopen(STATUS_FILE, "w");
    if (fp == NULL) {
        return -1;
    }
    // Write the heartbeat times to the status file
    if (fprintf(fp, "%ld %ld\n", (long) mirror1_ts, (long) mirror2_ts) < 0) {
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return 0;
}

/*
 * Function: Update the heartbeat time of a specific mirror node.
 * Principle: Read-modify-write to update only the specific mirror without resetting the other.
 * This prevents false zeroing of the other node during updates.
 */
static int update_heartbeat(const char *node_name) {
    time_t mirror1_ts = (time_t) 0;
    time_t mirror2_ts = (time_t) 0;
    time_t now = time(NULL);

    if (node_name == NULL) {
        return -1;
    }

    // Load the heartbeat times from the status file
    (void) load_heartbeat_status(&mirror1_ts, &mirror2_ts);

    if (strcmp(node_name, "mirror1") == 0) {
        mirror1_ts = now;
    } else if (strcmp(node_name, "mirror2") == 0) {
        mirror2_ts = now;
    } else {
        return -1;
    }

    // Save the heartbeat times to the status file
    return save_heartbeat_status(mirror1_ts, mirror2_ts);
}

/*
 * Function: Generate node online status text block.
 * Principle: Checks heartbeat TTL for mirrors and formats into string for client routing decisions.
 * Encodes evenly into a single-line protocol text for quick parsing.
 */
static int build_nodes_status_line(char *out, size_t out_size) {
    time_t mirror1_ts = (time_t) 0;
    time_t mirror2_ts = (time_t) 0;
    time_t now = time(NULL);
    int mirror1_online;
    int mirror2_online;

    if (out == NULL || out_size == 0) {
        return -1;
    }

    (void) load_heartbeat_status(&mirror1_ts, &mirror2_ts);

    mirror1_online = (mirror1_ts > 0 && (now - mirror1_ts) <= HEARTBEAT_TTL_SEC) ? 1 : 0;
    mirror2_online = (mirror2_ts > 0 && (now - mirror2_ts) <= HEARTBEAT_TTL_SEC) ? 1 : 0;

    if (snprintf(out,
                 out_size,
                 "NODES w26server=1 mirror1=%d mirror2=%d\n",
                 mirror1_online,
                 mirror2_online) < 0) {
        return -1;
    }

    return 0;
}

/*
 * Function: Convert file permission bits to an rwx string.
 * Principle: Maps owner/group/other permissions into a 9-character rwx string,
 * generating a 9-char string null-terminated for fn output metadata.
 */
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

/* Function: Lexicographical comparator for dir_item by name. */
static int cmp_dir_by_name(const void *a, const void *b) {
    const dir_item_t *da = (const dir_item_t *) a;
    const dir_item_t *db = (const dir_item_t *) b;
    return strcmp(da->name, db->name);
}

/* Function: Ascending comparator for dir_item by creation time. Ties broken by name. */
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

/* Function: Duplicate a string using explicit malloc/memcpy. */
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

/*
 * Function: Return the server's search root directory.
 * Principle: Falls back predictably: W26_SEARCH_ROOT -> HOME -> current working directory.
 * Guarantees service functionality even in minimal environments.
 */
static const char *get_search_root(void) {
    const char *custom = getenv("W26_SEARCH_ROOT");
    const char *home = getenv("HOME");

    if (custom != NULL && custom[0] != '\0') {
        return custom;
    }
    return (home != NULL && home[0] != '\0') ? home : ".";
}

/* Function: Get maximum recursive scan depth, reads W26_MAX_SCAN_DEPTH from env. */
static int get_max_scan_depth(void) {
    const char *s = getenv("W26_MAX_SCAN_DEPTH");
    char *end = NULL;
    long v;
    // Check if the W26_MAX_SCAN_DEPTH is set
    if (s == NULL || s[0] == '\0') {
        return DEFAULT_MAX_SCAN_DEPTH;
    }
    // Convert the W26_MAX_SCAN_DEPTH into a long
    v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < 1 || v > 64) {
        return DEFAULT_MAX_SCAN_DEPTH;
    }

    return (int) v;
}

/* Function: Collect all subdirectories immediately under the search root. */
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
    // Open the root directory for reading
    dir = opendir(root);
    if (dir == NULL) {
        return -1;
    }
    // Read the directory entries
    while ((ent = readdir(dir)) != NULL) {
        char full[PATH_MAX];
        struct stat st;
        char *name_copy;
        dir_item_t *tmp;
        // Skip the current and parent directories
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        // Format the full path
        if (snprintf(full, sizeof(full), "%s/%s", root, ent->d_name) < 0) {
            continue;
        }
        // Check if the entry is a directory
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }

        name_copy = dup_string(ent->d_name);
        if (name_copy == NULL) {
            ok = -1;
            break;
        }
        // Reallocate the items array
        tmp = (dir_item_t *) realloc(items, (count + 1) * sizeof(dir_item_t));
        if (tmp == NULL) {
            free(name_copy);
            ok = -1;
            break;
        }
        // Set the items array
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

/* Function: Send directory list as a single space-separated text line. */
static int send_dirlist_line(int client_fd, dir_item_t *items, size_t count) {
    size_t i;
    // Check if the count is 0
    if (count == 0) {
        return send_all(client_fd, "No directory found\n", 19);
    }
    // Send the directory list
    for (i = 0; i < count; ++i) {
        size_t len = strlen(items[i].name);
        if (send_all(client_fd, items[i].name, len) != 0) {
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

/* Function: Free dynamically allocated directory collection memory. */
static void free_dir_items(dir_item_t *items, size_t count) {
    size_t i;
    for (i = 0; i < count; ++i) {
        free(items[i].name);
    }
    free(items);
}

/* Function: Handle dirlist -a command using a full collect-sort-send-free pipeline. */
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

/* Function: Handle dirlist -t command using a full collect-sort-send-free pipeline. */
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

/*
 * Function: Recursively find the first file matching the target name.
 * Principle: Executes DFS on the directory tree, returns early on the first match to minimize latency.
 * Returns immediately on hit, skipping meaningless traversal.
 */
static int find_first_file(const char *dir_path, const char *target_name, char *out_path, size_t out_size) {
    DIR *dir;
    struct dirent *ent;
    // Open the directory for reading
    dir = opendir(dir_path);
    if (dir == NULL) {
        return 0;
    }
    // Read the directory entries
    while ((ent = readdir(dir)) != NULL) {
        char full[PATH_MAX];
        struct stat st;
        // Skip the current and parent directories
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        // Format the full path
        if (snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name) < 0) {
            continue;
        }
        if (stat(full, &st) != 0) {
            continue;
        }
        // Check if the entry is a directory
        if (S_ISDIR(st.st_mode)) {
            if (find_first_file(full, target_name, out_path, out_size)) {
                closedir(dir);
                return 1;
            }
            continue;
        }
        // Check if the entry is a regular file and the name matches the target name
        if (S_ISREG(st.st_mode) && strcmp(ent->d_name, target_name) == 0) {
            if (snprintf(out_path, out_size, "%s", full) >= 0) {
                closedir(dir);
                return 1;
            }
        }
    }

    closedir(dir);
    return 0;
}

/*
 * Function: Handle the fn filename command.
 * Principle: Locates file, retrieves stat metadata, and returns formatted protocol response.
 * Returns "File not found" on failure at any step,
 * keeping client handling branch simple.
 */
static int handle_fn(int client_fd, const char *filename) {
    const char *root = get_search_root();
    char path[PATH_MAX];
    struct stat st;
    char perm[10];
    char time_buf[32];
    struct tm *tm_info;
    char resp[1024];
    const char *base;
    // Check if the filename is NULL or empty
    if (filename == NULL || filename[0] == '\0') {
        return send_all(client_fd, "File not found\n", 15);
    }
    // Find the first file matching the target name
    if (!find_first_file(root, filename, path, sizeof(path))) {
        return send_all(client_fd, "File not found\n", 15);
    }

    if (stat(path, &st) != 0) {
        return send_all(client_fd, "File not found\n", 15);
    }
    // Format the permissions
    format_permissions(st.st_mode, perm);
    tm_info = localtime(&st.st_ctime);
    if (tm_info == NULL) {
        snprintf(time_buf, sizeof(time_buf), "unknown");
    } else {
        (void) strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
    }

    // Get the base name of the file
    base = strrchr(path, '/');
    base = (base != NULL) ? (base + 1) : path;

    if (snprintf(resp,
                 sizeof(resp),
                 "filename=%s size=%ld created=%s permissions=%s\n",
                 base,
                 (long)st.st_size,
                 time_buf,
                 perm) < 0) {
        return -1;
    }

    return send_all(client_fd, resp, strlen(resp));
}

/* Function: Append a single path to the file_list structure. */
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

/* Function: Free the file_list structure and its contents. */
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

/* Function: Filter files strictly within the specified min/max size range (inclusive). */
static int match_size_filter(const char *path, const struct stat *st, void *ctx) {
    const size_filter_t *f = (const size_filter_t *) ctx;
    (void) path;

    if (st == NULL || f == NULL) {
        return 0;
    }
    return (st->st_size >= f->min_size && st->st_size <= f->max_size) ? 1 : 0;
}

/* Function: Filter files matching any of the specified extensions. */
static int match_ext_filter(const char *path, const struct stat *st, void *ctx) {
    const ext_filter_t *f = (const ext_filter_t *) ctx;
    const char *base;
    const char *dot;
    int i;
    (void) st;

    if (path == NULL || f == NULL || f->count <= 0) {
        return 0;
    }
    // Get the base name of the file and the dot
    base = strrchr(path, '/');
    base = (base != NULL) ? (base + 1) : path;
    dot = strrchr(base, '.');
    if (dot == NULL || dot[1] == '\0') {
        return 0;
    }
    // Check if the extension matches any of the specified extensions
    for (i = 0; i < f->count; ++i) {
        if (f->exts[i] != NULL && strcmp(dot + 1, f->exts[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Function: Filter files created before or strictly on/after a specific timestamp. */
static int match_date_filter(const char *path, const struct stat *st, void *ctx) {
    const date_filter_t *f = (const date_filter_t *) ctx;
    (void) path;

    if (st == NULL || f == NULL) {
        return 0;
    }
    // Check if the file was created before the threshold
    if (f->before) {
        return (st->st_mtime < f->threshold) ? 1 : 0;
    }
    return (st->st_mtime >= f->threshold) ? 1 : 0;
}

/* Function: Recursively collect files satisfying the given matcher callback. */
static int collect_matching_files_recursive(const char *dir_path,
                                            int (*matcher)(const char *, const struct stat *, void *),
                                            void *ctx,
                                            file_list_t *out,
                                            int depth,
                                            int max_depth) {
    DIR *dir;
    struct dirent *ent;
    // Check if the depth is greater than the max depth
    if (depth > max_depth) {
        return 0;
    }
    // Open the directory for reading
    dir = opendir(dir_path);
    if (dir == NULL) {
        return 0;
    }
    // Read the directory entries
    while ((ent = readdir(dir)) != NULL) {
        char full[PATH_MAX];
        struct stat st;
        // Skip the current and parent directories
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        // Format the full path
        if (snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name) < 0) {
            continue;
        }
        if (stat(full, &st) != 0) {
            continue;
        }
        // Check if the entry is a directory
        if (S_ISDIR(st.st_mode)) {
            if (collect_matching_files_recursive(full, matcher, ctx, out, depth + 1, max_depth) != 0) {
                closedir(dir);
                return -1;
            }
            continue;
        }
        // Check if the entry is a regular file and the matcher matches
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

/* Function: Parse a YYYY-MM-DD date string into a local timezone timestamp. */
static int parse_date_ymd(const char *s, time_t *out) {
    int y;
    int m;
    int d;
    struct tm tmv;
    time_t t;

    if (s == NULL || out == NULL) {
        return -1;
    }
    // Parse the date string into y, m, d
    if (sscanf(s, "%d-%d-%d", &y, &m, &d) != 3) {
        return -1;
    }
    // Check if the month is valid
    if (m < 1 || m > 12 || d < 1 || d > 31) {
        return -1;
    }
    // Initialize the tm structure
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

/* Function: Create a tar.gz archive from a list of files via external tar executable. */
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
    // Format the output path
    if (snprintf(out_path, out_size, "/tmp/w26_temp_%ld.tar.gz", (long)pid) < 0) {
        return -1;
    }
    // Format the list path for the temporary list file
    if (snprintf(list_path, sizeof(list_path), "/tmp/w26_list_%ld_%ld.txt", (long)pid, (long)now) < 0) {
        return -1;
    }
    // Open the list file for writing
    list_fp = fopen(list_path, "w");
    if (list_fp == NULL) {
        return -1;
    }
    // Write the list of files to the temporary list file
    for (i = 0; i < list->count; ++i) {
        if (fprintf(list_fp, "%s\n", list->paths[i]) < 0) {
            fclose(list_fp);
            unlink(list_path);
            return -1;
        }
    }

    // Close the list file
    if (fclose(list_fp) != 0) {
        unlink(list_path);
        return -1;
    }

    memset(&dfl_chld, 0, sizeof(dfl_chld));
    // Set the signal handler for the child process to the default handler
    dfl_chld.sa_handler = SIG_DFL;
    sigemptyset(&dfl_chld.sa_mask);
    if (sigaction(SIGCHLD, &dfl_chld, &old_chld) != 0) {
        unlink(list_path);
        unlink(out_path);
        return -1;
    }

    child = fork();
    // If the child process is not created, then return -1
    if (child < 0) {
        (void) sigaction(SIGCHLD, &old_chld, NULL);
        unlink(list_path);
        unlink(out_path);
        return -1;
    }

    // If the child process is created, then redirect the stdout and stderr to /dev/null
    if (child == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            (void) dup2(devnull, STDOUT_FILENO);
            (void) dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        // Execute the tar command to create the archive and exit the child process
        execl("/usr/bin/tar", "tar", "-czf", out_path, "-T", list_path, (char *) NULL);
        _exit(127);
    }

    // Wait for the child process to exit
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

/* Function: Send binary archive to client, prefixed by FILE size protocol header. */
static int send_archive_file(int client_fd, const char *archive_path) {
    int fd;
    struct stat st;
    char header[64];
    char buf[4096];

    if (archive_path == NULL) {
        return -1;
    }
    // Check if the archive path is a regular file
    if (stat(archive_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return -1;
    }
    // Format the header for the archive file
    if (snprintf(header, sizeof(header), "FILE %ld\n", (long)st.st_size) < 0) {
        return -1;
    }
    // Send the header to the client
    if (send_all(client_fd, header, strlen(header)) != 0) {
        return -1;
    }
    // Open the archive file for reading
    fd = open(archive_path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    // Read the archive file and send it to the client
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

/* Function: Execute full filter, pack, send, and cleanup pipeline for query commands. */
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

    // Send the archive file to the client
    rc = send_archive_file(client_fd, archive_path);
    unlink(archive_path);
    free_file_list(&list);

    if (rc != 0) {
        return -1;
    }
    return 0;
}

/* Function: Handle fz command by configuring size_filter and running query pipeline. */
static int handle_fz(int client_fd, const char *cmd) {
    long min_size;
    long max_size;
    size_filter_t f;
    // Parse the command into min_size and max_size
    if (sscanf(cmd, "fz %ld %ld", &min_size, &max_size) != 2 || min_size < 0 || max_size < min_size) {
        return send_all(client_fd, "No file found\n", 14);
    }

    f.min_size = (off_t) min_size;
    f.max_size = (off_t) max_size;
    return handle_archive_query(client_fd, match_size_filter, &f);
}

/* Function: Handle ft command by configuring ext_filter and running query pipeline. */
static int handle_ft(int client_fd, const char *cmd) {
    char e1[32];
    char e2[32];
    char e3[32];
    ext_filter_t f;
    int parts;

    e1[0] = '\0';
    e2[0] = '\0';
    e3[0] = '\0';
    // Parse the command into e1, e2, e3
    parts = sscanf(cmd, "ft %31s %31s %31s", e1, e2, e3);
    if (parts < 1) {
        return send_all(client_fd, "No file found\n", 14);
    }
    // Initialize the ext_filter
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

/* Function: Handle date filtering commands (fdb/fda) via query pipeline. */
static int handle_fdx(int client_fd, const char *date_str, int before) {
    date_filter_t f;
    // Parse the date string into the threshold
    if (parse_date_ymd(date_str, &f.threshold) != 0) {
        return send_all(client_fd, "No file found\n", 14);
    }
    f.before = before;
    return handle_archive_query(client_fd, match_date_filter, &f);
}

/*
 * Function: Create and return the server's listening socket.
 * Principle: Follows standard socket/bind/listen sequence with strict resource cleanup on error.
 * Immediately closes allocated resources on failure to avoid fd leaks.
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

    /* 2) Allow fast port reuse to facilitate easy service reloads */
    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        close(sock_fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)cfg->bind_port);

    // If the bind_host is not 0.0.0.0, then use the bind_host
    if (cfg->bind_host != NULL && strcmp(cfg->bind_host, "0.0.0.0") != 0) {
        if (inet_pton(AF_INET, cfg->bind_host, &addr.sin_addr) != 1) {
            close(sock_fd);
            return -1;
        }
    } else {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    /* 3) Bind the address and port */
    if (bind(sock_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        close(sock_fd);
        return -1;
    }

    /* 4) Start listening for incoming connections */
    if (listen(sock_fd, BACKLOG) < 0) {
        close(sock_fd);
        return -1;
    }

    return sock_fd;
}

/*
 * Function: Read a single command line from a client connection.
 * Principle: Reads character-by-character until newline, filtering out CR. Returns 0 on close.
 * Returns 1 on success, -1 on error, for precise session cycle handling.
 */
static int read_command_line(int client_fd, char *buf, size_t size) {
    /* Read by line, with newline defining request boundary */
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
 * Function: Calculate the target routing node based on the client connection sequence.
 * Routing rules:
 *   seq 1-2  -> index 0 (w26server local)
 *   seq 3-4  -> index 1 (mirror1)
 *   seq 5-6  -> index 2 (mirror2)
 *   seq >= 7 -> round-robin (seq-7) % 3: 0->primary, 1->mirror1, 2->mirror2
 * i.e. 7,10,13... go to primary; 8,11,14... go to mirror1; 9,12,15... go to mirror2.
 * Returns: 0=w26server, 1=mirror1, 2=mirror2
 */
static int preferred_index_by_seq(long seq) {
    if (seq <= 2) {
        return 0;
    }
    if (seq <= 4) {
        return 1;
    }
    if (seq <= 6) {
        return 2;
    }
    return (int) ((seq - 7) % 3);
}

/*
 * Function: Atomically increment client connection sequence and return the previous value.
 * Principle:
 *   w26server uses a fork-per-connection model, each client handled by an independent child process.
 *   Uses file CLIENT_SEQ_FILE for persistent state across child processes.
 *   Uses fcntl F_SETLKW write locks to ensure strict atomic increments for connection tracking.
 *     1) Acquire write lock
 *     2) Read current sequence
 *     3) Write back seq+1
 *     4) Unlock
 *   Returns the pre-increment value for preferred_index_by_seq() routing.
 *   The file is unlinked on w26server startup, resetting sequence on reboot.
 */
static long next_client_seq(void) {
    int fd;
    struct flock fl;
    char buf[32];
    long seq = 1;
    ssize_t n;

    fd = open(CLIENT_SEQ_FILE, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        return 1;
    }

    /* Initialize flock structure for write lock */
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_len = 0;

    /* Acquire write lock, block until other processes release */
    if (fcntl(fd, F_SETLKW, &fl) < 0) {
        close(fd);
        return seq;
    }

    /* Read current sequence number */
    n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        seq = strtol(buf, NULL, 10);
        if (seq < 1) {
            seq = 1;
        }
    }

    /* Write back seq+1 for the next client */
    if (lseek(fd, 0, SEEK_SET) == 0 && ftruncate(fd, 0) == 0) {
        snprintf(buf, sizeof(buf), "%ld\n", seq + 1);
        (void) write(fd, buf, strlen(buf));
    }

    /* Unlock and close */
    fl.l_type = F_UNLCK;
    (void) fcntl(fd, F_SETLK, &fl);
    close(fd);

    return seq;
}

/* Function: Determine if the command requires routing to workers. */
static int is_business_command(const char *cmd) {
    char date_buf[32];

    if (cmd == NULL) {
        return 0;
    }

    if (strcmp(cmd, "dirlist -a") == 0 || strcmp(cmd, "dirlist -t") == 0) {
        return 1;
    }
    if (strncmp(cmd, "fn ", 3) == 0 || strncmp(cmd, "fz ", 3) == 0 || strncmp(cmd, "ft ", 3) == 0) {
        return 1;
    }
    if (sscanf(cmd, "fdb %31s", date_buf) == 1 || sscanf(cmd, "fda %31s", date_buf) == 1) {
        return 1;
    }

    return 0;
}

/* Function: Execute business commands locally on the primary server, reusing logic. */
static int process_local_business(int client_fd, const char *cmd) {
    char date_buf[32];
    
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

    return -1;
}

/*
 * Function: Dispatch business commands according to connection attribution.
 * Principle: Connection node is assigned at accept and persists; local requests execute directly, mirror requests return REDIRECT.
 * Local handles directly; redirects trigger connection tear down and reconnect via client.
 */
static int dispatch_business_command(int client_fd, const char *cmd, int route_index) {
    route_node_t nodes[3] = {
        {"w26server", "127.0.0.1", DEFAULT_PORT, 1},
        {"mirror1", MIRROR1_HOST, MIRROR1_PORT, 0},
        {"mirror2", MIRROR2_HOST, MIRROR2_PORT, 0}
    };
    time_t mirror1_ts = (time_t) 0;
    time_t mirror2_ts = (time_t) 0;
    time_t now = time(NULL);

    /* Non-business commands return -2 indicating to handle differently */
    if (!is_business_command(cmd)) {
        return -2;
    }

    /* Use heartbeat TTL to verify if the mirror node is currently considered online */
    (void) load_heartbeat_status(&mirror1_ts, &mirror2_ts);
    nodes[1].online = (mirror1_ts > 0 && (now - mirror1_ts) <= HEARTBEAT_TTL_SEC) ? 1 : 0;
    nodes[2].online = (mirror2_ts > 0 && (now - mirror2_ts) <= HEARTBEAT_TTL_SEC) ? 1 : 0;

    /* route_index==0: Handles locally on the primary server */
    if (route_index == 0) {
        return process_local_business(client_fd, cmd);
    }

    /* Errors if target mirror is offline (secondary protection) */
    if (!nodes[route_index].online) {
        return send_all(client_fd, "Target mirror offline\n", 22);
    }

    /* Target mirror online: returns REDIRECT to client */
    char line_buf[128];

    if (snprintf(line_buf,
                 sizeof(line_buf),
                 "REDIRECT %s %d\n",
                 nodes[route_index].host,
                 nodes[route_index].port) < 0) {
        return -1;
    }

    return send_all(client_fd, line_buf, strlen(line_buf));
}

/*
 * Function: Process a single client command and return response.
 * Ensures no arbitrary mid-connection routing changes.
 */
static int process_command(int client_fd, const char *cmd, int route_index) {
    char resp[MAX_COMMAND_LEN + 64];
    char nodes_line[128];
    int route_rc;

    if (cmd == NULL) {
        return -1;
    }

    /*
     * CONNECT_PROBE: First command sent by the client to determine actual serving node.
     * Execution flow:
     *   1) Check mirror heartbeats and TTL online state
     *   2) If route_index==0 or target offline -> return CONNECTED
     *   3) If target online -> return REDIRECT, client reconnects
     * This ensures the client routes to an available node.
     */
    if (strcmp(cmd, "CONNECT_PROBE") == 0) {
        route_node_t nodes[3] = {
            {"w26server", "127.0.0.1", DEFAULT_PORT, 1},
            {"mirror1", MIRROR1_HOST, MIRROR1_PORT, 0},
            {"mirror2", MIRROR2_HOST, MIRROR2_PORT, 0}
        };
        time_t m1_ts = (time_t) 0;
        time_t m2_ts = (time_t) 0;
        time_t now = time(NULL);
        char line_buf[128];

        (void) load_heartbeat_status(&m1_ts, &m2_ts);
        nodes[1].online = (m1_ts > 0 && (now - m1_ts) <= HEARTBEAT_TTL_SEC) ? 1 : 0;
        nodes[2].online = (m2_ts > 0 && (now - m2_ts) <= HEARTBEAT_TTL_SEC) ? 1 : 0;

        if (route_index == 0 || !nodes[route_index].online) {
            snprintf(line_buf, sizeof(line_buf),
                     "CONNECTED %s %s %d\n",
                     nodes[0].name, nodes[0].host, nodes[0].port);
        } else {
            snprintf(line_buf, sizeof(line_buf),
                     "REDIRECT %s %d\n",
                     nodes[route_index].host, nodes[route_index].port);
        }
        return send_all(client_fd, line_buf, strlen(line_buf));
    }

    if (strcmp(cmd, "GET_NODES") == 0) {
        if (build_nodes_status_line(nodes_line, sizeof(nodes_line)) != 0) {
            return send_all(client_fd, "NODES w26server=1 mirror1=0 mirror2=0\n", 38);
        }
        return send_all(client_fd, nodes_line, strlen(nodes_line));
    }

    route_rc = dispatch_business_command(client_fd, cmd, route_index);
    if (route_rc != -2) {
        return route_rc;
    }

    /* Unrecognized commands reply with ACK formatting. */
    if (snprintf(resp, sizeof(resp), "ACK from %s: %s\n", NODE_NAME, cmd) < 0) {
        return -1;
    }

    return send_all(client_fd, resp, strlen(resp));
}

/*
 * Function: Handle single client session lifecycle.
 * Principle: Session acts as read/dispatch/reply loop, gracefully dying on disconnect or quitc.
 * Ends process on error to bubble limits safely.
 *
 * Core Design:
 *   Each TCP connection spawn its own child process. Origins:
 *   (a) Hearbeat - replies to HEARTBEAT without consuming client sequence.
 *   (b) Real client - assigns sequence atomically and computes mapping on first command.
 *
 *   route_index initialised as -1. First non-heartbeat command triggers sequence map assignment.
 *   Calls next_client_seq() and applies formula in preferred_index_by_seq().
 *   Delayed sequence allocation guarantees heartbeats do not pollute client sequence logic.
 *
 *   Termination vectors:
 *   - Peer disconnected (recv = 0)
 *   - quitc command triggered
 *   - Exception
 */
static void crequest(int client_fd) {
    char cmd[MAX_COMMAND_LEN];
    int route_index = -1;

    for (;;) {
        int rc = read_command_line(client_fd, cmd, sizeof(cmd));
        if (rc <= 0) {
            break;
        }

        /*
         * Hearbeat identifier checks logic without affecting sequences.
         * Short-lived internal protocol; terminates connection block directly on completion.
         * Avoids moving sequence increment blocks and breaking client routing formulas.
         */
        if (strncmp(cmd, "HEARTBEAT ", 10) == 0) {
            const char *node_name = cmd + 10;
            if (update_heartbeat(node_name) == 0) {
                (void) send_all(client_fd, "HB_OK\n", 6);
            } else {
                (void) send_all(client_fd, "HB_ERR\n", 7);
            }
            break;
        }

        /*
         * Delayed connection mapping assignment leveraging fcntl sequencing.
         * Uses file locking for cross-process atomic reads.
         * Resolves sequence into routing cluster indexing.
         */
        if (route_index < 0) {
            long seq = next_client_seq();
            route_index = preferred_index_by_seq(seq);
            printf("%s client #%ld routed to %s\n",
                   NODE_NAME, seq,
                   (route_index == 0) ? "w26server" : (route_index == 1) ? "mirror1"
                                                                         : "mirror2");
            fflush(stdout);
        }

        if (strcmp(cmd, "quitc") == 0) {
            (void) send_all(client_fd, "BYE\n", 4);
            break;
        }

        if (process_command(client_fd, cmd, route_index) != 0) {
            break;
        }
    }
}

/*
 * Function: Start the main server loop to concurrently handle connections.
 * Principle: Standard parent-accepts-and-forks server pattern using SIGCHLD ignoration.
 * Returns rapidly to accepting on parent via process concurrency, bypassing zombie threads.
 */
static int run_server(const server_config_t *cfg) {
    int listen_fd = create_listen_socket(cfg);
    // If the listen_fd is less than 0, then return 1
    if (listen_fd < 0) {
        fprintf(stderr, "%s: failed to create listen socket\n", NODE_NAME);
        return 1;
    }

    /* Auto reap child process exit to avoid zombies */
    signal(SIGCHLD, SIG_IGN);

    for (;;) {
        // Accept a new client connection
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *) &client_addr, &client_len);

        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            continue;
        }

        pid_t pid = fork();
        // If the pid is less than 0, then close the client_fd and continue
        if (pid < 0) {
            perror("fork");
            close(client_fd);
            continue;
        }
        // If the pid is 0, then close the listen_fd and call crequest to handle the client connection
        if (pid == 0) {
            close(listen_fd);
            crequest(client_fd);
            close(client_fd);
            _exit(0);
        }

        /* Parent process closes duplicated client fd and loops accept */
        close(client_fd);
    }

    close(listen_fd);
    return 0;
}

/*
 * Function: Application main entry, parses and starts primary server.
 * Principle: Instantiates baseline parameters and proceeds into listen loop safely.
 * Runs base init sequences before turning logic block to daemon server pool.
 */
int main(void) {
    server_config_t cfg;
    /* Listen on all interfaces */
    cfg.bind_host = "0.0.0.0";
    cfg.bind_port = DEFAULT_PORT;

    /* Seed state file to avoid initial GET_NODES errors */
    (void) save_heartbeat_status((time_t) 0, (time_t) 0);

    /* Reset connection sequence loop counter file */
    (void) unlink(CLIENT_SEQ_FILE);

    printf("%s starting on port %d\n", NODE_NAME, cfg.bind_port);
    return run_server(&cfg);
}
