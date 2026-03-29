#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>

#define DEFAULT_PRIMARY_HOST "127.0.0.1"
#define DEFAULT_PRIMARY_PORT 5000
#define DEFAULT_MIRROR1_HOST "127.0.0.1"
#define DEFAULT_MIRROR1_PORT 5001
#define DEFAULT_MIRROR2_HOST "127.0.0.1"
#define DEFAULT_MIRROR2_PORT 5002

#define MAX_COMMAND_LEN 512
#define MAX_PATH_LEN 4096
#define MAX_EXTENSIONS 3
#define ARCHIVE_NAME "temp.tar.gz"

typedef enum node_role
{
    ROLE_PRIMARY = 0,
    ROLE_MIRROR1 = 1,
    ROLE_MIRROR2 = 2,
    ROLE_CLIENT = 3
} node_role_t;

typedef struct app_config
{
    node_role_t role;
    const char *bind_host;
    int bind_port;
    const char *primary_host;
    int primary_port;
    const char *mirror1_host;
    int mirror1_port;
    const char *mirror2_host;
    int mirror2_port;
} app_config_t;

#endif
