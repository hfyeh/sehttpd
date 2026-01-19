/**
 * http_parser.c - A simple non-blocking HTTP parser.
 *
 * This parser uses a Finite State Machine (FSM) to parse HTTP requests.
 *
 * Why FSM?
 * In a non-blocking environment (like epoll), data arrives in chunks. We might
 * get half of a request line in one read(), and the rest later. A traditional
 * linear parser would block waiting for the rest, which defeats the purpose of
 * non-blocking I/O.
 *
 * An FSM processes one character at a time and transitions between states.
 * If we run out of data, we simply return EAGAIN and save the current state.
 * When more data arrives, we resume exactly where we left off.
 */

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

#include "http.h"

/* Constant-time string comparison macro.
 *
 * Instead of comparing char by char, this casts the 4 characters to a uint32_t
 * and compares them in a single CPU instruction (on supported architectures).
 *
 * m: pointer to the string in memory
 * c0, c1, c2, c3: the characters to compare against (e.g., 'G', 'E', 'T', ' ')
 *
 * Note: This relies on little-endian architecture (common on x86/ARM).
 * (c3 << 24) puts the last char in the most significant byte.
 */
#define cst_strcmp(m, c0, c1, c2, c3) \
    *(uint32_t *) m == ((c3 << 24) | (c2 << 16) | (c1 << 8) | c0)

#define CR '\r'
#define LF '\n'
#define CRLFCRLF "\r\n\r\n"

/**
 * @brief Parses the HTTP Request Line (e.g., "GET /index.html HTTP/1.1").
 *
 * @param r The request structure containing the buffer and state.
 * @return int 0 on success, EAGAIN if more data is needed, or error code.
 */
int http_parse_request_line(http_request_t *r)
{
    uint8_t ch, *p, *m;
    size_t pi;

    enum {
        s_start = 0,
        s_method,
        s_spaces_before_uri,
        s_after_slash_in_uri,
        s_http,
        s_http_H,
        s_http_HT,
        s_http_HTT,
        s_http_HTTP,
        s_first_major_digit,
        s_major_digit,
        s_first_minor_digit,
        s_minor_digit,
        s_spaces_after_digit,
        s_almost_done
    } state;

    /* Resume from saved state */
    state = r->state;

    /* Iterate over the available data in the ring buffer */
    for (pi = r->pos; pi < r->last; pi++) {
        p = (uint8_t *) &r->buf[pi % MAX_BUF];
        ch = *p;

        /* TODO: use computed goto for efficient dispatching */
        switch (state) {
        /* HTTP methods: GET, HEAD, POST */
        case s_start:
            r->request_start = p;

            if (ch == CR || ch == LF) /* Skip empty lines */
                break;

            if ((ch < 'A' || ch > 'Z') && ch != '_')
                return HTTP_PARSER_INVALID_METHOD;

            state = s_method;
            break;

        case s_method:
            if (ch == ' ') { /* Space after method */
                m = r->request_start;

                /* Optimization: Check method length and use fast compare */
                switch (p - m) {
                case 3:
                    if (cst_strcmp(m, 'G', 'E', 'T', ' ')) {
                        r->method = HTTP_GET;
                        break;
                    }
                    break;

                case 4:
                    if (cst_strcmp(m, 'P', 'O', 'S', 'T')) {
                        r->method = HTTP_POST;
                        break;
                    }

                    if (cst_strcmp(m, 'H', 'E', 'A', 'D')) {
                        r->method = HTTP_HEAD;
                        break;
                    }
                    break;

                default:
                    r->method = HTTP_UNKNOWN;
                    break;
                }
                state = s_spaces_before_uri;
                break;
            }

            if ((ch < 'A' || ch > 'Z') && ch != '_')
                return HTTP_PARSER_INVALID_METHOD;
            break;

        /* space* before URI */
        case s_spaces_before_uri:
            if (ch == '/') {
                r->uri_start = p;
                state = s_after_slash_in_uri;
                break;
            }

            switch (ch) {
            case ' ':
                break;
            default:
                return HTTP_PARSER_INVALID_REQUEST;
            }
            break;

        case s_after_slash_in_uri:
            switch (ch) {
            case ' ':
                r->uri_end = p;
                state = s_http;
                break;
            default:
                break;
            }
            break;

        /* space+ after URI. Expecting "HTTP/..." */
        case s_http:
            switch (ch) {
            case ' ':
                break;
            case 'H':
                state = s_http_H;
                break;
            default:
                return HTTP_PARSER_INVALID_REQUEST;
            }
            break;

        case s_http_H:
            switch (ch) {
            case 'T':
                state = s_http_HT;
                break;
            default:
                return HTTP_PARSER_INVALID_REQUEST;
            }
            break;

        case s_http_HT:
            switch (ch) {
            case 'T':
                state = s_http_HTT;
                break;
            default:
                return HTTP_PARSER_INVALID_REQUEST;
            }
            break;

        case s_http_HTT:
            switch (ch) {
            case 'P':
                state = s_http_HTTP;
                break;
            default:
                return HTTP_PARSER_INVALID_REQUEST;
            }
            break;

        case s_http_HTTP:
            switch (ch) {
            case '/':
                state = s_first_major_digit;
                break;
            default:
                return HTTP_PARSER_INVALID_REQUEST;
            }
            break;

        /* first digit of major HTTP version */
        case s_first_major_digit:
            if (ch < '1' || ch > '9')
                return HTTP_PARSER_INVALID_REQUEST;

            r->http_major = ch - '0';
            state = s_major_digit;
            break;

        /* major HTTP version or dot */
        case s_major_digit:
            if (ch == '.') {
                state = s_first_minor_digit;
                break;
            }

            if (ch < '0' || ch > '9')
                return HTTP_PARSER_INVALID_REQUEST;

            r->http_major = r->http_major * 10 + ch - '0';
            break;

        /* first digit of minor HTTP version */
        case s_first_minor_digit:
            if (ch < '0' || ch > '9')
                return HTTP_PARSER_INVALID_REQUEST;

            r->http_minor = ch - '0';
            state = s_minor_digit;
            break;

        /* minor HTTP version or end of request line */
        case s_minor_digit:
            if (ch == CR) {
                state = s_almost_done;
                break;
            }

            if (ch == LF)
                goto done;

            if (ch == ' ') {
                state = s_spaces_after_digit;
                break;
            }

            if (ch < '0' || ch > '9')
                return HTTP_PARSER_INVALID_REQUEST;

            r->http_minor = r->http_minor * 10 + ch - '0';
            break;

        case s_spaces_after_digit:
            switch (ch) {
            case ' ':
                break;
            case CR:
                state = s_almost_done;
                break;
            case LF:
                goto done;
            default:
                return HTTP_PARSER_INVALID_REQUEST;
            }
            break;

        /* end of request line */
        case s_almost_done:
            r->request_end = p - 1;
            switch (ch) {
            case LF:
                goto done;
            default:
                return HTTP_PARSER_INVALID_REQUEST;
            }
        }
    }

    /* Run out of data, save state and return EAGAIN */
    r->pos = pi;
    r->state = state;

    return EAGAIN;

done:
    r->pos = pi + 1;

    if (!r->request_end)
        r->request_end = p;

    r->state = s_start;

    return 0;
}

/**
 * @brief Parses the HTTP Request Headers (Key: Value).
 *
 * @param r The request structure.
 * @return int 0 on success, EAGAIN if more data needed, or error.
 */
int http_parse_request_body(http_request_t *r)
{
    uint8_t ch, *p;
    size_t pi;

    enum {
        s_start = 0,
        s_key,
        s_spaces_before_colon,
        s_spaces_after_colon,
        s_value,
        s_cr,
        s_crlf,
        s_crlfcr
    } state;

    state = r->state;
    assert(state == 0 && "state should be 0");

    http_header_t *hd;
    for (pi = r->pos; pi < r->last; pi++) {
        p = (uint8_t *) &r->buf[pi % MAX_BUF];
        ch = *p;

        switch (state) {
        case s_start:
            if (ch == CR || ch == LF)
                break;

            r->cur_header_key_start = p;
            state = s_key;
            break;

        case s_key:
            if (ch == ' ') {
                r->cur_header_key_end = p;
                state = s_spaces_before_colon;
                break;
            }

            if (ch == ':') {
                r->cur_header_key_end = p;
                state = s_spaces_after_colon;
                break;
            }
            break;

        case s_spaces_before_colon:
            if (ch == ' ')
                break;
            if (ch == ':') {
                state = s_spaces_after_colon;
                break;
            }
            return HTTP_PARSER_INVALID_HEADER;

        case s_spaces_after_colon:
            if (ch == ' ')
                break;

            state = s_value;
            r->cur_header_value_start = p;
            break;

        case s_value:
            if (ch == CR) {
                r->cur_header_value_end = p;
                state = s_cr;
            }

            if (ch == LF) {
                r->cur_header_value_end = p;
                state = s_crlf;
            }
            break;

        case s_cr:
            if (ch == LF) {
                state = s_crlf;
                /* save the current HTTP header */
                hd = malloc(sizeof(http_header_t));
                hd->key_start = r->cur_header_key_start;
                hd->key_end = r->cur_header_key_end;
                hd->value_start = r->cur_header_value_start;
                hd->value_end = r->cur_header_value_end;

                list_add(&(hd->list), &(r->list));
                break;
            }
            return HTTP_PARSER_INVALID_HEADER;

        case s_crlf:
            if (ch == CR) {
                state = s_crlfcr;
            } else {
                r->cur_header_key_start = p;
                state = s_key;
            }
            break;

        case s_crlfcr:
            switch (ch) {
            case LF:
                goto done;
            default:
                return HTTP_PARSER_INVALID_HEADER;
            }
            break;
        }
    }

    r->pos = pi;
    r->state = state;

    return EAGAIN;

done:
    r->pos = pi + 1;
    r->state = s_start;

    return 0;
}
