#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "http.h"
#include "logger.h"
#include "timer.h"

#define MAXLINE 8192
#define SHORTLINE 512

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/**
 * @brief Writes 'n' bytes to a descriptor.
 *
 * Handles partial writes (common in non-blocking I/O or signals).
 *
 * @param fd File descriptor to write to.
 * @param usrbuf Buffer containing data.
 * @param n Number of bytes to write.
 * @return ssize_t Number of bytes written, or -1 on error.
 */
static ssize_t writen(int fd, void *usrbuf, size_t n)
{
    ssize_t nwritten;
    char *bufp = usrbuf;

    for (size_t nleft = n; nleft > 0; nleft -= nwritten) {
        if ((nwritten = write(fd, bufp, nleft)) <= 0) {
            if (errno == EINTR) /* interrupted by sig handler return */
                nwritten = 0;   /* and call write() again */
            else {
                log_err("errno == %d\n", errno);
                return -1; /* errrno set by write() */
            }
        }
        bufp += nwritten;
    }

    return n;
}

static char *webroot = NULL;

typedef struct {
    const char *type;
    const char *value;
} mime_type_t;

static mime_type_t mime[] = {{".html", "text/html"},
                             {".xml", "text/xml"},
                             {".xhtml", "application/xhtml+xml"},
                             {".txt", "text/plain"},
                             {".pdf", "application/pdf"},
                             {".png", "image/png"},
                             {".gif", "image/gif"},
                             {".jpg", "image/jpeg"},
                             {".css", "text/css"},
                             {NULL, "text/plain"}};

/**
 * @brief Parses the URI to resolve the filename.
 *
 * Constructs the full local file path from the web root and the requested URI.
 * Handles adding "index.html" if the URI ends in a slash.
 *
 * @param uri The requested URI.
 * @param uri_length Length of the URI.
 * @param filename Buffer to store the resolved filename.
 */
static void parse_uri(char *uri, int uri_length, char *filename)
{
    assert(uri && "parse_uri: uri is NULL");
    uri[uri_length] = '\0';

    /* TODO: support query string, i.e.
     *       https://example.com/over/there?name=ferret
     * Reference: https://en.wikipedia.org/wiki/Query_string
     */
    char *question_mark = strchr(uri, '?');
    int file_length;
    if (question_mark) {
        file_length = (int) (question_mark - uri);
        debug("file_length = (question_mark - uri) = %d", file_length);
    } else {
        file_length = uri_length;
        debug("file_length = uri_length = %d", file_length);
    }

    /* uri_length can not be too long */
    if (uri_length > (SHORTLINE >> 1)) {
        log_err("uri too long: %.*s", uri_length, uri);
        return;
    }

    strcpy(filename, webroot);
    debug("before strncat, filename = %s, uri = %.*s, file_len = %d", filename,
          file_length, uri, file_length);
    strncat(filename, uri, file_length);

    char *last_comp = strrchr(filename, '/');
    char *last_dot = strrchr(last_comp, '.');
    if (!last_dot && filename[strlen(filename) - 1] != '/')
        strcat(filename, "/");

    if (filename[strlen(filename) - 1] == '/')
        strcat(filename, "index.html");

    debug("served filename = %s", filename);
}

/**
 * @brief Sends an error response to the client.
 *
 * @param fd Client socket.
 * @param cause The cause of the error.
 * @param errnum HTTP error code (e.g., "404").
 * @param shortmsg Short error message.
 * @param longmsg Detailed error message.
 */
static void do_error(int fd,
                     char *cause,
                     char *errnum,
                     char *shortmsg,
                     char *longmsg)
{
    char header[MAXLINE], body[MAXLINE];

    sprintf(body,
            "<html><title>Server Error</title>"
            "<body>\n%s: %s\n<p>%s: %s\n</p>"
            "<hr><em>web server</em>\n</body></html>",
            errnum, shortmsg, longmsg, cause);

    sprintf(header,
            "HTTP/1.1 %s %s\r\n"
            "Server: seHTTPd\r\n"
            "Content-type: text/html\r\n"
            "Connection: close\r\n"
            "Content-length: %d\r\n\r\n",
            errnum, shortmsg, (int) strlen(body));

    writen(fd, header, strlen(header));
    writen(fd, body, strlen(body));
}

static const char *get_file_type(const char *type)
{
    if (!type)
        return "text/plain";

    int i;
    for (i = 0; mime[i].type; ++i) {
        if (!strcmp(type, mime[i].type))
            return mime[i].value;
    }
    return mime[i].value;
}

static const char *get_msg_from_status(int status_code)
{
    if (status_code == HTTP_OK)
        return "OK";

    if (status_code == HTTP_NOT_MODIFIED)
        return "Not Modified";

    if (status_code == HTTP_NOT_FOUND)
        return "Not Found";

    return "Unknown";
}

/**
 * @brief Serves a static file to the client.
 *
 * Uses mmap to map the file into memory and then writes it to the socket.
 * This is more efficient than read/write cycle as it reduces user-kernel context switches
 * and data copying.
 *
 * @param fd Client socket.
 * @param filename Path to the file.
 * @param filesize Size of the file.
 * @param out Output metadata (headers).
 */
static void serve_static(int fd,
                         char *filename,
                         size_t filesize,
                         http_out_t *out)
{
    char header[MAXLINE];
    int offset = 0;
    const char *dot_pos = strrchr(filename, '.');
    const char *file_type = get_file_type(dot_pos);

    offset += sprintf(header, "HTTP/1.1 %d %s\r\n", out->status,
                      get_msg_from_status(out->status));

    if (out->keep_alive) {
        offset += sprintf(header + offset, "Connection: keep-alive\r\n");
        offset += sprintf(header + offset, "Keep-Alive: timeout=%d\r\n",
                          TIMEOUT_DEFAULT);
    }

    if (out->modified) {
        char buf[SHORTLINE];
        offset += sprintf(header + offset, "Content-type: %s\r\n", file_type);
        offset += sprintf(header + offset, "Content-length: %zu\r\n", filesize);
        struct tm tm;
        localtime_r(&(out->mtime), &tm);
        strftime(buf, SHORTLINE, "%a, %d %b %Y %H:%M:%S GMT", &tm);
        offset += sprintf(header + offset, "Last-Modified: %s\r\n", buf);
    }

    sprintf(header + offset, "Server: seHTTPd\r\n\r\n");

    size_t n = (size_t) writen(fd, header, strlen(header));
    assert(n == strlen(header) && "writen error");
    if (n != strlen(header)) {
        log_err("n != strlen(header)");
        return;
    }

    if (!out->modified)
        return;

    int srcfd = open(filename, O_RDONLY, 0);
    assert(srcfd > 2 && "open error");
    /* TODO: use sendfile(2) for zero-copy support */
    /* mmap maps the file content to a memory address.
     * PROT_READ: Pages may be read.
     * MAP_PRIVATE: Create a private copy-on-write mapping.
     */
    char *srcaddr = mmap(NULL, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
    assert(srcaddr != (void *) -1 && "mmap error");
    close(srcfd);

    writen(fd, srcaddr, filesize);

    munmap(srcaddr, filesize);
}

static inline int init_http_out(http_out_t *o, int fd)
{
    o->fd = fd;
    o->keep_alive = false;
    o->modified = true;
    o->status = 0;
    return 0;
}

/**
 * @brief Core request handling logic.
 *
 * Called when the client socket is ready to be read (EPOLLIN).
 * It reads data from the socket, parses the request, and sends a response.
 *
 * @param ptr Pointer to http_request_t structure.
 */
void do_request(void *ptr)
{
    http_request_t *r = ptr;
    int fd = r->fd;
    int rc;
    char filename[SHORTLINE];
    webroot = r->root;

    /* Remove existing timer while processing the request */
    del_timer(r);
    for (;;) {
        /* Calculate available space in the ring buffer */
        char *plast = &r->buf[r->last % MAX_BUF];
        size_t remain_size =
            MIN(MAX_BUF - (r->last - r->pos) - 1, MAX_BUF - r->last % MAX_BUF);

        /* Read data from the socket */
        int n = read(fd, plast, remain_size);
        assert(r->last - r->pos < MAX_BUF && "request buffer overflow!");

        if (n == 0) /* EOF: Client closed connection */
            goto err;

        if (n < 0) {
            if (errno != EAGAIN) {
                log_err("read err, and errno = %d", errno);
                goto err;
            }
            /* EAGAIN: We have read all available data for now.
             * Break out of the loop and wait for more data (via epoll). */
            break;
        }

        r->last += n;
        assert(r->last - r->pos < MAX_BUF && "request buffer overflow!");

        /* Try to parse the request line (GET /path HTTP/1.1) */
        rc = http_parse_request_line(r);
        if (rc == EAGAIN) /* Incomplete request line, continue reading */
            continue;
        if (rc != 0) {
            log_err("rc != 0");
            goto err;
        }

        debug("uri = %.*s", (int) (r->uri_end - r->uri_start),
              (char *) r->uri_start);

        /* Try to parse headers */
        rc = http_parse_request_body(r);
        if (rc == EAGAIN) /* Incomplete headers, continue reading */
            continue;
        if (rc != 0) {
            log_err("rc != 0");
            goto err;
        }

        /* Handle http header processing and prepare response */
        http_out_t *out = malloc(sizeof(http_out_t));
        if (!out) {
            log_err("no enough space for http_out_t");
            exit(1);
        }

        init_http_out(out, fd);

        parse_uri(r->uri_start, r->uri_end - r->uri_start, filename);

        struct stat sbuf;
        if (stat(filename, &sbuf) < 0) {
            do_error(fd, filename, "404", "Not Found", "Can't find the file");
            continue;
        }

        if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) {
            do_error(fd, filename, "403", "Forbidden", "Can't read the file");
            continue;
        }

        out->mtime = sbuf.st_mtime;

        http_handle_header(r, out);
        assert(list_empty(&(r->list)) && "header list should be empty");

        if (!out->status)
            out->status = HTTP_OK;

        serve_static(fd, filename, sbuf.st_size, out);

        if (!out->keep_alive) {
            debug("no keep_alive! ready to close");
            free(out);
            goto close;
        }
        free(out);
    }

    /* Re-arm the epoll event.
     * We used EPOLLONESHOT, so we must manually re-enable the event.
     */
    struct epoll_event event = {
        .data.ptr = ptr,
        .events = EPOLLIN | EPOLLET | EPOLLONESHOT,
    };
    epoll_ctl(r->epfd, EPOLL_CTL_MOD, r->fd, &event);

    /* Reset the timeout timer */
    add_timer(r, TIMEOUT_DEFAULT, http_close_conn);
    return;

err:
close:
    /* TODO: handle the timeout raised by inactive connections */
    rc = http_close_conn(r);
    assert(rc == 0 && "do_request: http_close_conn");
}
