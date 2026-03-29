#include "network.h"

int net_create_server_socket(const char *host, int port)
{
    (void)host;
    (void)port;
    /* TODO: create, bind, and listen on a TCP socket. */
    return -1;
}

int net_accept_client(int server_fd)
{
    (void)server_fd;
    /* TODO: accept a client connection and return client fd. */
    return -1;
}

int net_connect_to_server(const char *host, int port)
{
    (void)host;
    (void)port;
    /* TODO: connect to remote TCP endpoint and return socket fd. */
    return -1;
}

void net_close_socket(int fd)
{
    (void)fd;
    /* TODO: close socket fd safely. */
}
