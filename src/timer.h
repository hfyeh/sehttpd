#ifndef TIMER_H
#define TIMER_H

#include <stdbool.h>
#include "http.h"

#define TIMEOUT_DEFAULT 500 /* ms */

typedef int (*timer_callback)(http_request_t *req);

/**
 * @brief Represents a timer event.
 */
typedef struct {
    size_t key;             /* Expiration time (in milliseconds) */
    bool deleted;           /* Lazy deletion flag. If true, this timer is ignored. */
    timer_callback callback;/* Function to call when timer expires */
    http_request_t *request;/* Argument to pass to the callback */
} timer_node;

/**
 * @brief Initializes the timer subsystem (priority queue).
 * @return int 0 on success.
 */
int timer_init();

/**
 * @brief Finds the time until the next timer expires.
 *
 * Used to determine the timeout for epoll_wait.
 *
 * @return int Time in milliseconds, or -1 (infinite) if no timers.
 */
int find_timer();

/**
 * @brief Processes and removes all expired timers.
 *
 * Checks the priority queue for timers with key <= current_time.
 */
void handle_expired_timers();

/**
 * @brief Adds a new timer.
 *
 * @param req The request associated with this timer.
 * @param timeout Timeout duration in milliseconds.
 * @param cb Callback function to execute on expiration.
 */
void add_timer(http_request_t *req, size_t timeout, timer_callback cb);

/**
 * @brief "Deletes" a timer.
 *
 * Note: This marks the timer as deleted (lazy deletion). It will be removed
 * from the queue when it reaches the top or during cleanup.
 *
 * @param req The request whose timer should be deleted.
 */
void del_timer(http_request_t *req);

#endif
