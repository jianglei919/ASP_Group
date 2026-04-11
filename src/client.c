#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Client default connects to primary server */
#define PRIMARY_HOST "127.0.0.1"
#define PRIMARY_PORT 5000
#define MIRROR1_PORT 5001
#define MIRROR2_PORT 5002
#define MAX_COMMAND_LEN 512
#define MAX_REDIRECT_HOPS 4

/*
 * Function: Establish TCP connection to the server.
 * Principle: Create a socket and call connect, close fd and return an error on failure.
 */
static int connect_to_server(const char *host, int port) {
    int sock_fd;
    struct sockaddr_in addr;

    // create TCP socket
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        return -1;
    }

    // set a server address
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);

    // convert IP address from text to binary
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(sock_fd);
        return -1;
    }

    // Connect to the target server
    if (connect(sock_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        close(sock_fd);
        return -1;
    }

    return sock_fd;
}

/*
 * Function: Validate whether the user input command is legal.
 * Principle: checks empty command first, and extends full syntax rules.
 */
static int validate_command(const char *line, char *err, size_t err_size) {
    char cmd[32];
    char arg1[256];
    char arg2[256];
    int parts;

    if (line == NULL || err == NULL || err_size == 0) {
        return -1;
    }

    if (line[0] == '\0') {
        snprintf(err, err_size, "empty command");
        return -1;
    }

    // Control command: quitc, dirlist -a, dirlist -t
    if (strcmp(line, "quitc") == 0) {
        err[0] = '\0';
        return 0;
    }

    if (strcmp(line, "dirlist -a") == 0 || strcmp(line, "dirlist -t") == 0) {
        err[0] = '\0';
        return 0;
    }

    cmd[0] = '\0';
    arg1[0] = '\0';
    arg2[0] = '\0';
    // Parse the command into cmd, arg1, arg2
    parts = sscanf(line, "%31s %255s %255s", cmd, arg1, arg2);

    // Business command: fn <filename>, fz <size1> <size2>, ft <ext1> [ext2] [ext3], fdb YYYY-MM-DD, fda YYYY-MM-DD
    if (parts == 2 && strcmp(cmd, "fn") == 0) {
        err[0] = '\0';
        return 0;
    }

    if (parts == 3 && strcmp(cmd, "fz") == 0) {
        long n1;
        long n2;
        // Validate that size1 and size2 are valid long integers, with size2 >= size1 >= 0
        if (sscanf(line, "fz %ld %ld", &n1, &n2) == 2 && n1 >= 0 && n2 >= n1) {
            err[0] = '\0';
            return 0;
        }
        snprintf(err, err_size, "usage: fz <size1> <size2> (size2 >= size1 >= 0)");
        return -1;
    }
    // ft command supports 1 to 3 extensions
    if (parts >= 2 && parts <= 4 && strcmp(cmd, "ft") == 0) {
        err[0] = '\0';
        return 0;
    }

    if (parts == 2 && (strcmp(cmd, "fdb") == 0 || strcmp(cmd, "fda") == 0)) {
        int y;
        int m;
        int d;
        // Validate that the date is in YYYY-MM-DD format, with basic range checks for month and day.
        if (sscanf(arg1, "%d-%d-%d", &y, &m, &d) == 3 && m >= 1 && m <= 12 && d >= 1 && d <= 31) {
            err[0] = '\0';
            return 0;
        }
        snprintf(err, err_size, "usage: %s YYYY-MM-DD", cmd);
        return -1;
    }

    // For unsupported commands, provide a general usage hint
    if (parts >= 1 && strcmp(cmd, "dirlist") == 0) {
        snprintf(err, err_size, "currently supports only: dirlist -a | dirlist -t");
        return -1;
    }

    if (parts >= 1 && strcmp(cmd, "fn") == 0) {
        snprintf(err, err_size, "usage: fn <filename>");
        return -1;
    }

    if (parts >= 1 && strcmp(cmd, "fz") == 0) {
        snprintf(err, err_size, "usage: fz <size1> <size2>");
        return -1;
    }

    if (parts >= 1 && strcmp(cmd, "ft") == 0) {
        snprintf(err, err_size, "usage: ft <ext1> [ext2] [ext3]");
        return -1;
    }

    if (parts >= 1 && (strcmp(cmd, "fdb") == 0 || strcmp(cmd, "fda") == 0)) {
        snprintf(err, err_size, "usage: %s YYYY-MM-DD", cmd);
        return -1;
    }

    snprintf(err, err_size, "unsupported command (allowed: dirlist -a|-t, fn, fz, ft, fdb, fda, quitc)");
    return -1;
}

/*
 * Function: Reliably send data of a specified length.
 * Principle: Call send in loop, handle EINTR interrupt, until all bytes are sent.
 */
static int send_all(int fd, const char *data, size_t len) {
    // Send in loop to ensure a complete request is written
    size_t sent = 0;
    while (sent < len) {
        // send may be interrupted by signals (EINTR) or may write fewer bytes than requested, so loop until all data is sent.
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
        // Move the sent offset forward by the number of bytes successfully sent.
        sent += (size_t) n;
    }

    return 0;
}

/*
 * Function: Send a command to the server.
 * Principle: Send command content and append a newline for the server to read by line.
 */
static int send_command(int server_fd, const char *line) {
    size_t len;

    if (line == NULL) {
        return -1;
    }

    // End with a newline, server parses requests line by line
    len = strlen(line);
    if (send_all(server_fd, line, len) != 0) {
        return -1;
    }
    // Append newline to indicate end of command, server reads by line
    return send_all(server_fd, "\n", 1);
}

/*
 * Function: Receive and print server response.
 * Principle: Read the first line to distinguish REDIRECT or FILE protocol; if REDIRECT,
 * pass the target address back to the caller to reconnect and resend the command.
 * If FILE, keep reading binary archive based on length; otherwise print text response.
 * print_output: whether to print text responses and FILE receive messages
 */
static int receive_response(int server_fd, char *redirect_host, size_t redirect_host_size, int *redirect_port,
                            int *redirected, int print_output) {
    char line[MAX_COMMAND_LEN + 128];
    const char *home;
    char out_dir[PATH_MAX];
    char out_path[PATH_MAX];
    long file_size;
    FILE *fp;
    char buf[4096];
    long received;
    size_t idx = 0;

    /* Read the first line of response first to distinguish between text response and file stream response. */
    while (idx + 1 < sizeof(line)) {
        char ch;
        // Read byte by byte until newline
        ssize_t n = recv(server_fd, &ch, 1, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }

        // Newline indicates the end of the first line of response, which is the protocol header for both REDIRECT and FILE.
        if (ch == '\n') {
            break;
        }
        // Filter out carriage return in the case of CRLF, ensure the output is a standard C string.
        if (ch != '\r') {
            line[idx++] = ch;
        }
    }

    line[idx] = '\0';

    if (redirected != NULL) {
        *redirected = 0;
    }

    /*
     * Protocol branch 1: REDIRECT
     * Format: "REDIRECT <host> <port>"
     * Meaning: w26server informs a client to reconnect to the specified mirror node.
     * After successful parse, sets redirected=1 and returns, the caller disconnects and reconnects.
     */
    if (redirect_host != NULL && redirect_host_size > 0 && redirect_port != NULL) {
        char redirect_fmt[32];
        size_t host_width = redirect_host_size - 1;
        // To prevent buffer overflow in sscanf, limit the host width to redirect_host_size - 1
        if (host_width > 127) {
            host_width = 127;
        }

        // Use snprintf to construct the format string for sscanf, ensuring we do not read more characters than redirect_host can hold
        if (snprintf(redirect_fmt, sizeof(redirect_fmt), "REDIRECT %%%zus %%d", host_width) >= 0 &&
            sscanf(line, redirect_fmt, redirect_host, redirect_port) == 2) {
            if (redirected != NULL) {
                *redirected = 1;
            }
            return 0;
        }
    }

    /*
     * Protocol branch 2: FILE
     * Format: "FILE <size>"  followed by size bytes of binary tar.gz data
     * Meaning: Server packs matched files into archive and streams to client.
     * Client reads strictly according to byte count in protocol header, writes to ~/project/temp.tar.gz.
     */
    if (sscanf(line, "FILE %ld", &file_size) == 1 && file_size >= 0) {
        /* Convention: Archive is always written to ~/project/temp.tar.gz for standardized script checks. */
        home = getenv("HOME");
        if (home == NULL || home[0] == '\0') {
            home = ".";
        }
        // Create output directory if it does not exist, ignore EEXIST error if it already exists.
        if (snprintf(out_dir, sizeof(out_dir), "%s/project", home) < 0) {
            return -1;
        }
        if (mkdir(out_dir, 0755) != 0 && errno != EEXIST) {
            return -1;
        }
        if (snprintf(out_path, sizeof(out_path), "%s/temp.tar.gz", out_dir) < 0) {
            return -1;
        }
        // Open the output file for writing binary data, return error if failed to open.
        fp = fopen(out_path, "wb");
        if (fp == NULL) {
            return -1;
        }

        received = 0;
        /* Read strictly according to the byte count in the protocol header to prevent over-reading into subsequent command responses. */
        while (received < file_size) {
            // Calculate how many bytes to read in this iteration
            size_t need = file_size - received > (long) sizeof(buf) ? sizeof(buf) : (size_t) (file_size - received);
            ssize_t n = recv(server_fd, buf, need, 0);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                fclose(fp);
                return -1;
            }
            if (n == 0) {
                fclose(fp);
                return -1;
            }
            // Write the received chunk to the output file, ensuring we write exactly n bytes.
            if (fwrite(buf, 1, (size_t) n, fp) != (size_t) n) {
                fclose(fp);
                return -1;
            }
            received += n;
        }

        fclose(fp);
        if (print_output) {
            printf("Received temp.tar.gz (%ld bytes) -> %s\n", file_size, out_path);
        }
        return 0;
    }

    if (print_output) {
        printf("%s\n", line);
    }
    return 0;
}

/*
 * Function: Client program entry point and main interactive loop.
 * Principle: After connection, loop execution: read input -> validate -> send -> receive, until quitc.
 */
int main(void) {
    // define some variables
    char line[MAX_COMMAND_LEN];
    char err[128];
    char current_host[64];
    int session_fd;
    int current_port = PRIMARY_PORT;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    snprintf(current_host, sizeof(current_host), "%s", PRIMARY_HOST);

    // connect to the primary server first
    session_fd = connect_to_server(PRIMARY_HOST, PRIMARY_PORT);
    if (session_fd < 0) {
        fprintf(stderr, "client: failed to connect w26server (%s:%d)\n", PRIMARY_HOST, PRIMARY_PORT);
        return 1;
    }

    /*
     * Node probing and route following process:
     *   1) Send CONNECT_PROBE to w26server
     *   2) w26server determines routing based on client sequence:
     *      - Belongs to local -> responds "CONNECTED w26server 127.0.0.1 5000" (not REDIRECT, loop ends)
     *      - Belongs to mirror -> responds "REDIRECT 127.0.0.1 5001"
     *   3) After REDIRECT, disconnect, reconnect to target mirror, send CONNECT_PROBE again
     *   4) mirror responds "CONNECTED mirrorN ..." (not REDIRECT, loop ends)
     *   After loop, session_fd is connected to target node, all subsequent commands sent there.
     */
    {
        char probe_host[64] = {0};
        int probe_port = 0;
        int probe_redirected = 0;
        int hops;


        for (hops = 0; hops < MAX_REDIRECT_HOPS; ++hops) {
            // Send CONNECT_PROBE to check if current node is the final service node or needs to be redirected, parse response for next hop if redirected.
            if (send_command(session_fd, "CONNECT_PROBE") != 0 ||
                receive_response(session_fd, probe_host, sizeof(probe_host),
                                 &probe_port, &probe_redirected, 0) != 0) {
                fprintf(stderr, "client: CONNECT_PROBE failed\n");
                close(session_fd);
                return 1;
            }

            /* CONNECTED response (not REDIRECT) indicates current node is the final service node */
            if (!probe_redirected) {
                break;
            }

            /* Received REDIRECT: disconnect current connection, reconnect to target mirror */
            close(session_fd);
            snprintf(current_host, sizeof(current_host), "%s", probe_host);
            current_port = probe_port;

            // try to connect to the redirected target, if failed, return error and exit
            session_fd = connect_to_server(current_host, current_port);
            if (session_fd < 0) {
                fprintf(stderr, "client: failed to connect %s:%d\n",
                        current_host, current_port);
                return 1;
            }
        }

        if (hops >= MAX_REDIRECT_HOPS) {
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

    /* Interactive command loop: maintain the same session connection until quitc or exception */
    while (fgets(line, sizeof(line), stdin) != NULL) {
        int redirect_hops;
        int done = 0;

        /* Remove trailing newline from input, use send_command to append '\n' uniformly */
        line[strcspn(line, "\n")] = '\0';

        // Validate user input command
        if (validate_command(line, err, sizeof(err)) != 0) {
            fprintf(stderr, "Invalid command: %s\n", err);
            continue;
        }

        for (redirect_hops = 0; redirect_hops < MAX_REDIRECT_HOPS; ++redirect_hops) {
            char next_host[64] = {0};
            int next_port = 0;
            int redirected = 0;

            // Send the validated command to the current session, receive response and check if redirected to another node
            if (send_command(session_fd, line) != 0 ||
                receive_response(session_fd, next_host, sizeof(next_host), &next_port, &redirected, 1) != 0) {
                fprintf(stderr, "client: send/receive failed with %s:%d\n", current_host, current_port);
                close(session_fd);
                session_fd = -1;
                break;
            }

            if (!redirected) {
                done = 1;
                break;
            }

            close(session_fd);

            snprintf(current_host, sizeof(current_host), "%s", next_host);
            current_port = next_port;

            session_fd = connect_to_server(current_host, current_port);
            if (session_fd < 0) {
                fprintf(stderr, "client: failed to connect redirected target %s:%d\n", current_host, current_port);
                break;
            }
        }

        if (!done) {
            if (redirect_hops >= MAX_REDIRECT_HOPS) {
                fprintf(stderr, "client: too many redirects\n");
            }
            break;
        }

        if (strcmp(line, "quitc") == 0) {
            break;
        }
    }

    if (session_fd >= 0) {
        close(session_fd);
    }

    return 0;
}
