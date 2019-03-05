/*
 * Copyright (c) 2009 Mark Heily <mark@heily.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * $FreeBSD$
 */

#include "common.h"

#include <sys/ioctl.h>
#include <semaphore.h>
#include <pthread.h>

#define THREAD_CNT (100)
#define PACKET_CNT (5000)
#define	FKQMULTI	_IO('f', 89)
#define TEST_DEBUG

struct thread_info {
    pthread_t thrd;
    int evcnt;
    int tid;
};

int g_kqfd;
int g_sockfd[2];
int g_end;
struct thread_info g_thrd_info[THREAD_CNT];
/* Test threads signals this upon receiving events */
sem_t g_sem_driver;

static int
check_sched(void)
{
    int max = 0, min = 999999;

    for(int i = 0; i < THREAD_CNT; i++) {
        int cur = g_thrd_info[i].evcnt;
        if (cur > max) {
            max = cur;
        }
        if (cur < min) {
            min = cur;
        }
    }

#ifdef TEST_DEBUG
        printf("READ_M: check_sched: max difference is %d\n", max - min);
#endif

    return (max - min) <= 1;
}

static void
socket_pop()
{
    char buf[1];

    /* Drain the read buffer, then make sure there are no more events. */
#ifdef TEST_DEBUG
    printf("READ_M: popping the read buffer\n");
#endif
    if (read(g_sockfd[0], &buf[0], 1) < 1)
        err(1, "read(2)");
}

static void
socket_push()
{
#ifdef TEST_DEBUG
    printf("READ_M: pushing to socket\n");
#endif
    if (write(g_sockfd[1], ".", 1) < 1) {
#ifdef TEST_DEBUG
    printf("READ_M: write failed with %d\n", errno);
#endif     
        err(1, "write(2)");
    }
}

/* for multi threaded read */
static void*
test_socket_read_thrd(void* args)
{
    struct thread_info *info = (struct thread_info *) args;

    struct kevent kev;
    EV_SET(&kev, g_sockfd[0], EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, &g_sockfd[0]);
    kev.data = 1;

    while (1) {
#ifdef TEST_DEBUG
        printf("READ_M: thread %d waiting for events\n", info->tid); 
#endif
        kevent_cmp(&kev, kevent_get(g_kqfd));
#ifdef TEST_DEBUG
        printf("READ_M: thread %d woke up\n", info->tid); 
#endif
        socket_pop();
        info->evcnt++;

        /* signal the driver */
        sem_post(&g_sem_driver);

        if (g_end) {
#ifdef TEST_DEBUG
    printf("READ_M: thread %d exiting...\n", info->tid);
#endif   
            break;
        }
    }
    pthread_exit(0);
}

static void
test_socket_read(void)
{
    int error = 0;
    struct kevent kev;
    EV_SET(&kev, g_sockfd[0], EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, &g_sockfd[0]);

    const char *test_id = "[Multi]kevent(EVFILT_READ)";
    sem_init(&g_sem_driver, 0, 0);
    g_end = 0;

    test_begin(test_id);

    error = kevent(g_kqfd, &kev, 1, NULL, 0, NULL);

    if (error == -1) {
#ifdef TEST_DEBUG
        printf("READ_M: kevent add failed with %d\n", errno);
#endif   
        err(1, "kevent_add");     
    }

#ifdef TEST_DEBUG
    printf("READ_M: creating %d threads...\n", THREAD_CNT);
#endif
    for (int i = 0; i < THREAD_CNT; i++) {
        g_thrd_info[i].tid = i;
        g_thrd_info[i].evcnt = 0;
        pthread_create(&g_thrd_info[i].thrd, NULL, test_socket_read_thrd, &g_thrd_info[i]);
    }

#ifdef TEST_DEBUG
    printf("READ_M: waiting for threads to wait on KQ...\n");
#endif
    
    sleep(3);

    for(int i = 0; i < PACKET_CNT; i++) {
#ifdef TEST_DEBUG
    printf("READ_M: processing packet %d\n", i);
#endif        
        socket_push();
        /* wait for thread events */
        sem_wait(&g_sem_driver);

        if (!check_sched()) {
#ifdef TEST_DEBUG
            printf("READ_M: check_sched failed...\n");
#endif
            g_end = 1;
            error = 2;
            break;
        }

        if ((i + THREAD_CNT) == PACKET_CNT - 1) {
            g_end = 1;
        }
    }

    /* shutdown the systems */

#ifdef TEST_DEBUG
    printf("READ_M: finished testing, system shutting down...\n");
#endif   
    for (int i = 0; i < THREAD_CNT; i++) {
        pthread_join(g_thrd_info[i].thrd, NULL);
    }

    if (!error)
        success();
    else
        err(error, "kevent");
}


void
test_evfilt_read_m()
{
    /* Create a connected pair of full-duplex sockets for testing socket events */
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, &g_sockfd[0]) < 0) 
        abort();

    g_kqfd = kqueue();
    int error = ioctl(g_kqfd, FKQMULTI);
    if (error == -1) {
#ifdef TEST_DEBUG
        printf("READ_M: ioctl failed with %d\n", errno);
#endif
        err(1, "ioctl");
    }

    test_socket_read();

    close(g_kqfd);
    close(g_sockfd[0]);
    close(g_sockfd[1]);
}
