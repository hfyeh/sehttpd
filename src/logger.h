#ifndef LOGGER_H
#define LOGGER_H

#include <errno.h>
#include <stdio.h>

/**
 * @brief Debug logging macro.
 *
 * If NDEBUG is defined (Release build), this macro expands to nothing,
 * removing the logging code entirely.
 *
 * If NDEBUG is NOT defined (Debug build), it prints:
 * [DEBUG] (filename:line): message
 *
 * __FILE__: Standard macro for current file name.
 * __LINE__: Standard macro for current line number.
 * __VA_ARGS__: Variable arguments passed to the macro.
 * ##__VA_ARGS__: GCC extension to handle empty variable arguments gracefully.
 */
#ifdef NDEBUG
#define debug(MSG, ...)
#else
#define debug(MSG, ...)                                               \
    fprintf(stderr, "[DEBUG] (%s:%d): " MSG "\n", __FILE__, __LINE__, \
            ##__VA_ARGS__)
#endif

/**
 * @brief Error logging macro.
 *
 * Always prints errors to stderr, including the current errno string.
 */
#define log_err(MSG, ...)                                             \
    fprintf(stderr, "[ERROR] (%s:%d: errno: %s) " MSG "\n", __FILE__, \
            __LINE__, errno == 0 ? "None" : strerror(errno), ##__VA_ARGS__)

#endif
