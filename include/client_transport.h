#ifndef CLIENT_TRANSPORT_H
#define CLIENT_TRANSPORT_H

int client_send_request(int fd, const char *line);
int client_receive_response(int fd);

#endif
