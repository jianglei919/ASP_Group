#include "protocol.h"

int protocol_send_header(int fd, const message_header_t *header)
{
    (void)fd;
    (void)header;
    /* TODO: implement framed header send (network byte order). */
    return -1;
}

int protocol_recv_header(int fd, message_header_t *header)
{
    (void)fd;
    (void)header;
    /* TODO: implement framed header receive (network byte order). */
    return -1;
}

int protocol_send_buffer(int fd, const void *buf, size_t len)
{
    (void)fd;
    (void)buf;
    (void)len;
    /* TODO: implement robust write loop. */
    return -1;
}

int protocol_recv_buffer(int fd, void *buf, size_t len)
{
    (void)fd;
    (void)buf;
    (void)len;
    /* TODO: implement robust read loop. */
    return -1;
}
