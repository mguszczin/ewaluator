#ifndef ERR_H
#define ERR_H

#include <cstdio>
#include <cstdlib>
#include <cerrno>

#define syserr(fmt, ...) \
    do { \
        fprintf(stderr, "ERROR: " fmt " (errno %d: %s)\n", ##__VA_ARGS__, errno, strerror(errno)); \
        exit(1); \
    } while(0)

#endif // ERR_H