#ifndef NETWORK_H
#define NETWORK_H

int net_create_server_socket(const char *host, int port);
int net_accept_client(int server_fd);
int net_connect_to_server(const char *host, int port);
void net_close_socket(int fd);

#endif
