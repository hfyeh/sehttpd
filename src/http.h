#ifndef HTTP_H
#define HTTP_H

#include <errno.h>
#include <stdbool.h>
#include <time.h>

#include "list.h"

/**
 * Return codes for the HTTP parser.
 */
enum http_parser_retcode {
    HTTP_PARSER_INVALID_METHOD = 10,
    HTTP_PARSER_INVALID_REQUEST,
    HTTP_PARSER_INVALID_HEADER
};

/**
 * Supported HTTP methods.
 * Represented as bitmasks, though currently used as simple enum values.
 */
enum http_method {
    HTTP_UNKNOWN = 0x0001,
    HTTP_GET = 0x0002,
    HTTP_HEAD = 0x0004,
    HTTP_POST = 0x0008,
};

/**
 * HTTP Status codes.
 */
enum http_status {
    HTTP_OK = 200,
    HTTP_NOT_MODIFIED = 304,
    HTTP_NOT_FOUND = 404,
};

#define MAX_BUF 8124

/**
 * @brief Represents an active HTTP client connection.
 *
 * This structure holds the state of the connection, including the file descriptors,
 * the read buffer, parsing state, and pointers to parsed data.
 */
typedef struct {
    void *root;         /* Web root directory */
    int fd;             /* Client socket file descriptor */
    int epfd;           /* Epoll file descriptor (to modify events) */

    char buf[MAX_BUF];  /* Ring buffer for reading requests */
    size_t pos;         /* Current parsing position in buf */
    size_t last;        /* End of data position in buf */

    int state;          /* Current state of the parser FSM */

    /* Pointers into 'buf' marking parts of the request.
     * This avoids copying strings (zero-copy parsing). */
    void *request_start;
    int method;         /* HTTP_GET, etc. */
    void *uri_start;
    void *uri_end;
    int http_major;     /* HTTP version major (e.g., 1) */
    int http_minor;     /* HTTP version minor (e.g., 1) */
    void *request_end;

    struct list_head list; /* Linked list to store parsed HTTP headers */

    /* Pointers for the current header being parsed */
    void *cur_header_key_start;
    void *cur_header_key_end;
    void *cur_header_value_start;
    void *cur_header_value_end;

    void *timer;        /* Pointer to the timer node for this connection */
} http_request_t;

/**
 * @brief Represents the HTTP response to be sent.
 */
typedef struct {
    int fd;             /* Client socket file descriptor */
    bool keep_alive;    /* Whether to keep connection open (Connection: keep-alive) */
    time_t mtime;       /* Last modification time of the requested file */
    bool modified;      /* False if 304 Not Modified */
    int status;         /* HTTP status code (200, 404, etc.) */
} http_out_t;

/**
 * @brief Represents a single HTTP header (Key: Value).
 */
typedef struct {
    void *key_start, *key_end;     /* Pointers to Key string in request buffer */
    void *value_start, *value_end; /* Pointers to Value string in request buffer */
    list_head list;                /* Linked list node */
} http_header_t;

/**
 * @brief Function pointer type for handling specific HTTP headers.
 */
typedef int (*http_header_handler)(http_request_t *r,
                                   http_out_t *o,
                                   char *data,
                                   int len);

/**
 * @brief Map between a header name and its handler function.
 */
typedef struct {
    char *name;
    http_header_handler handler;
} http_header_handle_t;

void http_handle_header(http_request_t *r, http_out_t *o);
int http_close_conn(http_request_t *r);

/**
 * @brief Initializes an http_request_t structure.
 *
 * @param r Pointer to request structure.
 * @param fd Client socket descriptor.
 * @param epfd Epoll descriptor.
 * @param root Web root path.
 */
static inline void init_http_request(http_request_t *r,
                                     int fd,
                                     int epfd,
                                     char *root)
{
    r->fd = fd, r->epfd = epfd;
    r->pos = r->last = 0;
    r->state = 0;
    r->root = root;
    INIT_LIST_HEAD(&(r->list));
}

/* TODO: public functions should have conventions to prefix http_ */
void do_request(void *infd);

int http_parse_request_line(http_request_t *r);
int http_parse_request_body(http_request_t *r);

#endif
