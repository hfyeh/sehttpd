#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* for the sake of strptime(3) */
#endif

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "http.h"

/**
 * @brief Closes a client connection.
 *
 * Closes the file descriptor and frees the request structure.
 * Note on epoll: When a file descriptor is closed, it is automatically removed
 * from the epoll set if no other file descriptors refer to the same open file description.
 *
 * @param r The request structure.
 * @return int 0 on success.
 */
int http_close_conn(http_request_t *r)
{
    close(r->fd);
    free(r);
    return 0;
}

/**
 * @brief Handler for ignored headers.
 */
static int http_process_ignore(http_request_t *r UNUSED,
                               http_out_t *out UNUSED,
                               char *data UNUSED,
                               int len UNUSED)
{
    return 0;
}

/**
 * @brief Handler for "Connection" header.
 *
 * Checks if the client requested "keep-alive".
 */
static int http_process_connection(http_request_t *r UNUSED,
                                   http_out_t *out,
                                   char *data,
                                   int len)
{
    if (!strncasecmp("keep-alive", data, len))
        out->keep_alive = true;
    return 0;
}

/**
 * @brief Handler for "If-Modified-Since" header.
 *
 * Used for caching. If the file hasn't changed since the date provided by
 * the client, we send a 304 Not Modified response (without body).
 */
static int http_process_if_modified_since(http_request_t *r UNUSED,
                                          http_out_t *out,
                                          char *data,
                                          int len UNUSED)
{
    struct tm tm;
    if (!strptime(data, "%a, %d %b %Y %H:%M:%S GMT", &tm))
        return 0;

    time_t client_time = mktime(&tm);
    double time_diff = difftime(out->mtime, client_time);

    /* Compare modified times.
     * If difference is less than 1 microsecond (effectively 0), it's not modified.
     */
    if (fabs(time_diff) < 1e-6) { /* Not modified */
        out->modified = false;
        out->status = HTTP_NOT_MODIFIED;
    }
    return 0;
}

/* Dispatch table mapping header names to handler functions */
static http_header_handle_t http_headers_in[] = {
    {"Host", http_process_ignore},
    {"Connection", http_process_connection},
    {"If-Modified-Since", http_process_if_modified_since},
    {"", http_process_ignore}};

/**
 * @brief Processes all parsed headers.
 *
 * Iterates through the list of headers found in the request and calls the
 * appropriate handler function from the dispatch table.
 *
 * @param r Request structure containing the list of headers.
 * @param o Output structure to update (e.g., keep_alive, status).
 */
void http_handle_header(http_request_t *r, http_out_t *o)
{
    list_head *pos, *n;
    /* Iterate over the list safely (safe against deletion) */
    list_for_each_safe (pos, n, &(r->list)) {
        http_header_t *header = list_entry(pos, http_header_t, list);

        /* Find the handler for this header */
        for (http_header_handle_t *header_in = http_headers_in;
             strlen(header_in->name) > 0; header_in++) {
            if (!strncmp(header->key_start, header_in->name,
                         header->key_end - header->key_start)) {
                int len = header->value_end - header->value_start;
                (*(header_in->handler))(r, o, header->value_start, len);
                break;
            }
        }

        /* Delete the header from the list and free memory */
        list_del(pos);
        free(header);
    }
}
