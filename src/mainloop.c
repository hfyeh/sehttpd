/**
 * mainloop.c - The main event loop of the server.
 *
 * This file implements the "Reactor Pattern" using the Linux epoll(7) system call.
 * The Reactor pattern allows a single thread to handle multiple concurrent
 * connections efficiently.
 *
 * Key Concepts:
 * 1. Non-blocking I/O: Operations like accept() and read() return immediately
 *    even if there's no data, allowing the thread to do other work.
 * 2. Event-driven: We ask the kernel to tell us when a file descriptor (socket)
 *    is ready for reading or writing.
 * 3. epoll: A scalable I/O event notification mechanism. Unlike select/poll,
 *    it doesn't need to scan all file descriptors, making it O(1) mostly.
 * 4. Edge Triggered (EPOLLET): Events are delivered only when the state changes.
 *    This requires us to read/write *everything* until EAGAIN is returned.
 */

#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "http.h"
#include "logger.h"
#include "timer.h"

/* The maximum number of events to process at once in the event loop */
#define MAXEVENTS 1024
/* The backlog size for the listen socket (pending connection queue) */
#define LISTENQ 1024

/**
 * @brief Opens a listening socket on the specified port.
 *
 * @param port The port number to listen on.
 * @return int The file descriptor of the listening socket, or -1 on error.
 */
static int open_listenfd(int port)
{
    int listenfd, optval = 1;

    /* Create a socket descriptor of type IPv4 (AF_INET) and TCP (SOCK_STREAM) */
    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return -1;

    /* Eliminate "Address already in use" error from bind.
     * This allows us to restart the server immediately even if connections
     * are in TIME_WAIT state.
     */
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const void *) &optval,
                   sizeof(int)) < 0)
        return -1;

    /* Listenfd will be an endpoint for all requests to given port. */
    struct sockaddr_in serveraddr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY), /* Accept connections from any IP */
        .sin_port = htons((unsigned short) port),
        .sin_zero = {0},
    };

    /* Bind the socket to the address and port */
    if (bind(listenfd, (struct sockaddr *) &serveraddr, sizeof(serveraddr)) < 0)
        return -1;

    /* Make it a listening socket ready to accept connection requests.
     * LISTENQ is the maximum number of pending connections. */
    if (listen(listenfd, LISTENQ) < 0)
        return -1;

    return listenfd;
}

/**
 * @brief Sets a socket file descriptor to non-blocking mode.
 *
 * In non-blocking mode, functions like read() and accept() return immediately
 * with error EAGAIN or EWOULDBLOCK if they cannot complete the action.
 * This is crucial for event loops to avoid getting stuck waiting for one connection.
 *
 * @param fd The file descriptor.
 * @return int 0 on success, -1 on error.
 */
static int sock_set_non_blocking(int fd)
{
    /* Get current flags */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        log_err("fcntl");
        return -1;
    }

    /* Add O_NONBLOCK flag */
    flags |= O_NONBLOCK;
    int s = fcntl(fd, F_SETFL, flags);
    if (s == -1) {
        log_err("fcntl");
        return -1;
    }
    return 0;
}

#define DEFAULT_PORT 8081
#define DEFAULT_WEBROOT "./www"

/**
 * @brief Helper to parse port number from string.
 *
 * @param arg_port The string argument.
 * @return int The port number or exits on failure.
 */
static int cmd_get_port(char *arg_port)
{
    long ret;
    char *endptr;

    ret = strtol(arg_port, &endptr, 10);
    if ((errno == ERANGE && (ret == LONG_MAX || ret == LONG_MIN)) ||
        (errno != 0 && ret == 0)) {
        fprintf(stderr, "Failed to parse port number: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (endptr == arg_port) {
        fprintf(stderr, "No digits were found\n");
        exit(EXIT_FAILURE);
    }

    /* strtol successfully parse arg_port, and check boundary condition */
    if (ret <= 0 || ret > 65535) {
        ret = DEFAULT_PORT;
    }
    return ret;
}

struct runtime_conf {
    int port;
    char *web_root;
};

/**
 * @brief Parses command line arguments.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return struct runtime_conf* Pointer to configuration structure.
 */
static struct runtime_conf *parse_cmd(int argc, char **argv)
{
    int cmdopt = 0;
    struct runtime_conf *cfg = malloc(sizeof(struct runtime_conf));

    cfg->port = DEFAULT_PORT;
    cfg->web_root = DEFAULT_WEBROOT;

    while ((cmdopt = getopt(argc, argv, "p:r:")) != -1) {
        switch (cmdopt) {
        case 'p':
            cfg->port = cmd_get_port(optarg);
            break;
        case 'w':
            cfg->web_root = optarg;
            break;
        case '?':
            fprintf(stderr, "Illegal option: -%c\n",
                    isprint(optopt) ? optopt : '#');
            exit(EXIT_FAILURE);
            break;
        default:
            fprintf(stderr, "Not supported option\n");
            break;
        }
    }

    return cfg;
}

int main(int argc, char **argv)
{
    struct runtime_conf *cfg = parse_cmd(argc, argv);

    /* Ignore SIGPIPE signal.
     * When writing to a connection that the client has closed, the system
     * sends SIGPIPE. By default, this kills the process. We want to ignore it
     * and handle the error code (EPIPE) from write() instead.
     */
    if (sigaction(SIGPIPE,
                  &(struct sigaction){.sa_handler = SIG_IGN, .sa_flags = 0},
                  NULL)) {
        log_err("Failed to install signal handler for SIGPIPE");
        return 0;
    }

    /* 1. Initialize the listening socket */
    int listenfd = open_listenfd(cfg->port);
    int rc UNUSED = sock_set_non_blocking(listenfd);
    assert(rc == 0 && "sock_set_non_blocking");

    /* 2. Create an epoll instance */
    /* epoll_create1(0) is the newer version of epoll_create() */
    int epfd = epoll_create1(0 /* flags */);
    assert(epfd > 0 && "epoll_create1");

    /* Buffer to store events returned by epoll_wait */
    struct epoll_event *events = malloc(sizeof(struct epoll_event) * MAXEVENTS);
    assert(events && "epoll_event: malloc");

    /* Create the request object for the listening socket.
     * Even though it's not a client request, we use the structure to track it. */
    http_request_t *request = malloc(sizeof(http_request_t));
    init_http_request(request, listenfd, epfd, cfg->web_root);

    /* 3. Register the listening socket with epoll */
    struct epoll_event event = {
        .data.ptr = request,
        /* EPOLLIN: Ready to read (accept connection)
         * EPOLLET: Edge Triggered mode. We get notified only on state change. */
        .events = EPOLLIN | EPOLLET,
    };
    epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &event);

    /* Initialize the timer system */
    timer_init();

    printf("Web server started.\n");

    /* 4. The Main Event Loop */
    while (1) {
        /* Determine how long to wait for events based on the next timer expiration */
        int time = find_timer();
        debug("wait time = %d", time);

        /* Wait for events.
         * epoll_wait blocks until:
         * 1. A file descriptor is ready (returns number of events > 0)
         * 2. The timeout 'time' expires (returns 0)
         * 3. A signal interrupts it (returns -1)
         */
        int n = epoll_wait(epfd, events, MAXEVENTS, time);

        /* Process any expired timers */
        handle_expired_timers();

        /* Iterate over the ready events */
        for (int i = 0; i < n; i++) {
            http_request_t *r = events[i].data.ptr;
            int fd = r->fd;

            if (listenfd == fd) {
                /* Case 1: Notification on the listening socket -> New Connection(s) */

                /* Since we are in Edge Triggered (EPOLLET) mode, we must accept
                 * ALL pending connections until accept() returns EAGAIN.
                 * Otherwise, we might miss connections that arrived simultaneously. */
                while (1) {
                    socklen_t inlen = 1;
                    struct sockaddr_in clientaddr;
                    int infd = accept(listenfd, (struct sockaddr *) &clientaddr,
                                      &inlen);
                    if (infd < 0) {
                        if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
                            /* We have processed all incoming connections */
                            break;
                        }
                        log_err("accept");
                        break;
                    }

                    /* Make the new connection non-blocking */
                    rc = sock_set_non_blocking(infd);
                    assert(rc == 0 && "sock_set_non_blocking");

                    /* Create a new request object for this client */
                    request = malloc(sizeof(http_request_t));
                    if (!request) {
                        log_err("malloc");
                        break;
                    }

                    init_http_request(request, infd, epfd, cfg->web_root);

                    /* Register the new connection with epoll */
                    event.data.ptr = request;
                    /* EPOLLONESHOT: Disable the event after one notification.
                     * We will re-enable it later. This prevents race conditions
                     * if multiple threads were used (though this server is single-threaded). */
                    event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, infd, &event);

                    /* Add a timer to close the connection if idle for too long */
                    add_timer(request, TIMEOUT_DEFAULT, http_close_conn);
                }
            } else {
                /* Case 2: Notification on a client socket -> Data ready or Error */

                if ((events[i].events & EPOLLERR) ||
                    (events[i].events & EPOLLHUP) ||
                    (!(events[i].events & EPOLLIN))) {
                    /* An error occurred on this file descriptor */
                    log_err("epoll error fd: %d", r->fd);
                    close(fd);
                    continue;
                }

                /* Process the request (Read/Write) */
                do_request(events[i].data.ptr);
            }
        }
    }

    free(cfg);
    return 0;
}
