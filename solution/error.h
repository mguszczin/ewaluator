#ifndef ERROR_H
#define ERROR_H

#include <cstdio>
#include <cstdlib>
#include <cerrno>

#define syserr(fmt, ...) \
    do { \
        fprintf(stderr, "ERROR: " fmt " (errno %d: %s)\n", ##__VA_ARGS__, errno, strerror(errno)); \
        exit(1); \
    } while(0)


/* Assert that expression evaluates to zero (otherwise use result as error number, as in pthreads). */
#define ASSERT_ZERO(expr)                                                   \
    do {                                                                    \
        int errno = (expr);                                                 \
        if (errno != 0)                                                     \
            syserr(                                                         \
                "Failed: %s\n\tIn function %s() in %s line %d.\n\tErrno: ", \
                #expr, __func__, __FILE__, __LINE__);                       \
    } while (0)

/*
    Assert that expression doesn't evaluate to -1 (as almost every system function does in case of error).

    Use as a function, with a semicolon, like: ASSERT_SYS_OK(close(fd));
    (This is implemented with a 'do { ... } while(0)' block so that it can be used between if () and else.)
*/
#define ASSERT_SYS_OK(expr)                                                                \
    do {                                                                                   \
        if ((expr) == -1)                                                                  \
            syserr(                                                                        \
                "System command failed: %s\n\tIn function %s() in %s line %d.\n\tErrno: ", \
                #expr, __func__, __FILE__, __LINE__);                                      \
    } while (0)

#endif // ERR_H