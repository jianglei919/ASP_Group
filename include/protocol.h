#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>

#define MSG_TYPE_REQUEST 1
#define MSG_TYPE_TEXT_RESPONSE 2
#define MSG_TYPE_FILE_RESPONSE 3

typedef struct message_header
{
    int message_type;
    int request_id;
    int payload_length;
} message_header_t;

typedef struct file_header
{
    int status_code;
    int file_name_length;
    long file_size;
} file_header_t;

int protocol_send_header(int fd, const message_header_t *header);
int protocol_recv_header(int fd, message_header_t *header);
int protocol_send_buffer(int fd, const void *buf, size_t len);
int protocol_recv_buffer(int fd, void *buf, size_t len);

#endif
