#ifndef _COMMON_M_H
#define _COMMON_M_H

#include "common.h"

static inline char
socket_pop(int sockfd)
{
    char buf;
    
    if (read(sockfd, &buf, 1) < 1)
        err(1, "read(2)");

    return buf;
}

static inline char
socket_pop_igerr(int sockfd)
{
    char buf = 0;
    read(sockfd, &buf, 1);
    return buf;
}

static inline void
socket_push(int sockfd, char ch)
{
    if (write(sockfd, &ch, 1) < 1) {
        err(1, "write(2)");
    }
}

static inline void
socket_push_igerr(int sockfd, char ch)
{
    write(sockfd, &ch, 1);
}

#endif