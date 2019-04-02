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
#include <sys/types.h>
#include <sys/sysctl.h>
#include <pthread_np.h>

/*
 * The ioctl to set multithreaded mode
 */
#define	FKQMULTI	_IOW('f', 89, int)

/*
 * KQ scheduler flags
 */
#define KQ_SCHED_QUEUE 	0x1 /* make kq affinitize the knote depending on the cpu it's scheduled */

//#define TEST_DEBUG

struct thread_info {
    pthread_t thrd;
    int can_crash;
    pthread_mutex_t lock;
    int group_id;
    int evcnt;
    int tid;
};

/*
 * Read test
 */

#define THREAD_CNT (16)
#define PACKET_CNT (1600)

int g_kqfd;
int g_sockfd[2];
struct thread_info g_thrd_info[THREAD_CNT];
/* Test threads signals this upon receiving events */
sem_t g_sem_driver;

static void
check_sched(struct thread_info *info, int size)
{
    int max = 0, min = 999999;

    for(int i = 0; i < size; i++) {
        int cur = info[i].evcnt;
        if (cur > max) {
            max = cur;
        }
        if (cur < min) {
            min = cur;
        }
    }

    if ((max - min) > 1) {
#ifdef TEST_DEBUG
        printf("READ_M: check_sched: max difference is %d\n", max - min);
#endif
        abort();
    }
}

static char
socket_pop(int sockfd)
{
    char buf;

    /* Drain the read buffer, then make sure there are no more events. */
#ifdef TEST_DEBUG
    printf("READ_M: popping the read buffer\n");
#endif
    if (read(sockfd, &buf, 1) < 1)
        err(1, "read(2)");

    return buf;
}

static void
socket_push(int sockfd, char ch)
{
#ifdef TEST_DEBUG
    printf("READ_M: pushing to socket %d\n", sockfd);
#endif
    if (write(sockfd, &ch, 1) < 1) {
#ifdef TEST_DEBUG
    printf("READ_M: write failed with %d\n", errno);
#endif     
        err(1, "write(2)");
    }
}

/***************************
 * Read test
 ***************************/
static void*
test_socket_read_thrd(void* args)
{
    struct thread_info *info = (struct thread_info *) args;
    char dat;
    struct kevent *ret;

    while (1) {
#ifdef TEST_DEBUG
        printf("READ_M: thread %d waiting for events\n", info->tid); 
#endif
        ret = kevent_get(g_kqfd);
#ifdef TEST_DEBUG
        printf("READ_M: thread %d woke up\n", info->tid); 
#endif

        dat = socket_pop(ret->ident);
        free(ret);

        if (dat == 'e')
            break;

        info->evcnt++;
        
        /* signal the driver */
        sem_post(&g_sem_driver);
    }
    
#ifdef TEST_DEBUG
        printf("READ_M: thread %d exiting\n", info->tid); 
#endif

    sem_post(&g_sem_driver);
    pthread_exit(0);
}

static void
test_socket_read(void)
{
    int error = 0;
    const char *test_id = "[Multi]kevent(EVFILT_READ)";
    test_begin(test_id);    

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, &g_sockfd[0]) < 0) 
        err(1, "kevent_read socket");

    struct kevent kev;
    EV_SET(&kev, g_sockfd[0], EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, &g_sockfd[0]);

    sem_init(&g_sem_driver, 0, 0);

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
        socket_push(g_sockfd[1], '.');
        /* wait for thread events */
        sem_wait(&g_sem_driver);

        check_sched(g_thrd_info, THREAD_CNT);
    }


#ifdef TEST_DEBUG
    printf("READ_M: finished testing, system shutting down...\n");
#endif
    for(int i = 0; i < THREAD_CNT; i++) {
        socket_push(g_sockfd[1], 'e');

        sem_wait(&g_sem_driver);
    }

    for (int i = 0; i < THREAD_CNT; i++) {
        pthread_join(g_thrd_info[i].thrd, NULL);
    }

#ifdef TEST_DEBUG
    printf("READ_M: clearing kevent...\n");
#endif
    EV_SET(&kev, g_sockfd[0], EVFILT_READ, EV_DELETE, 0, 0, &g_sockfd[0]);

    error = kevent(g_kqfd, &kev, 1, NULL, 0, NULL);

    if (error == -1) {
#ifdef TEST_DEBUG
        printf("READ_M: kevent delete failed with %d\n", errno);
#endif   
        err(1, "kevent_delete");     
    }
#ifdef TEST_DEBUG
    printf("READ_M: closing sockets...\n");
#endif
    close(g_sockfd[0]);
    close(g_sockfd[1]);

    success();
}

/***************************
 * Queue test
 ***************************/

#define THREAD_QUEUE_CNT (4)
#define PACKET_QUEUE_CNT (1000)

static int
get_ncpu()
{
    int mib[4];
    int numcpu;
    size_t len = sizeof(numcpu);

    mib[0] = CTL_HW;
    mib[1] = HW_NCPU;

    sysctl(mib, 2, &numcpu, &len, NULL, 0);

    if (numcpu < 1)
    {
        err(1, "< 1 cpu detected");
    }

    return numcpu;
}

static void*
test_socket_queue_thrd(void* args)
{
    struct thread_info *info = (struct thread_info *) args;
    char dat;
    struct kevent *ret;

    while (1) {
#ifdef TEST_DEBUG
        printf("READ_M: thread %d waiting for events\n", info->tid); 
#endif
        ret = kevent_get(g_kqfd);
#ifdef TEST_DEBUG
        printf("READ_M: thread %d woke up\n", info->tid); 
#endif

        dat = socket_pop(ret->ident);
        free(ret);

        if (dat == 'e')
            break;

        info->evcnt++;
        
        /* signal the driver */
        sem_post(&g_sem_driver);
    }
    
#ifdef TEST_DEBUG
        printf("READ_M: thread %d exiting\n", info->tid); 
#endif

    sem_post(&g_sem_driver);
    pthread_exit(0);
}

static void
test_socket_queue(void)
{
    int error = 0;
    const char *test_id = "[Multi][Queue]kevent(EVFILT_READ)";

    test_begin(test_id);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, &g_sockfd[0]) < 0) 
        err(1, "kevent_read socket");

    struct kevent kev;
    EV_SET(&kev, g_sockfd[0], EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, &g_sockfd[0]);

    sem_init(&g_sem_driver, 0, 0);

    error = kevent(g_kqfd, &kev, 1, NULL, 0, NULL);

    if (error == -1) {
#ifdef TEST_DEBUG
        printf("READ_M: kevent add failed with %d\n", errno);
#endif   
        err(1, "kevent_add");     
    }

    cpuset_t cpuset;
    int ncpu = get_ncpu();
    int tid = 0;
#ifdef TEST_DEBUG
    printf("READ_M: detected %d cores...\n", ncpu);
#endif

    struct thread_info **group = malloc(sizeof(struct thread_info*) * ncpu);
    for (int i = 0; i < ncpu; i++) {
        group[i] = malloc(sizeof(struct thread_info) * THREAD_QUEUE_CNT);
        for (int j = 0; j < THREAD_QUEUE_CNT; j++) {
            group[i][j].tid = tid;
            tid++;
            group[i][j].evcnt = 0;
            group[i][j].group_id = i;
            pthread_create(&group[i][j].thrd, NULL, test_socket_queue_thrd, &group[i][j]);
            CPU_ZERO(&cpuset);
            CPU_SET(i, &cpuset);
            if (pthread_setaffinity_np(group[i][j].thrd, sizeof(cpuset_t), &cpuset) < 0) {
                err(1, "thread_affinity");
            }
#ifdef TEST_DEBUG
    printf("READ_M: created and affinitized thread %d to core group %d\n", group[i][j].tid, i);
#endif
        }
    }

#ifdef TEST_DEBUG
    printf("READ_M: waiting for threads to wait on KQ...\n");
#endif
    
    sleep(3);

    int affinity_group = -1;
    for(int k = 1; k <= PACKET_QUEUE_CNT; k++) {
#ifdef TEST_DEBUG
        printf("READ_M: processing packet %d\n", k);
#endif        
        socket_push(g_sockfd[1], '.');
        /* wait for thread events */
        sem_wait(&g_sem_driver);

        /* basically only one group should get events, do this for now, ideally we can have a table that remembers each knote's affinity*/
        for(int i = 0; i < ncpu; i++) {
            int sum = 0;
            for (int j = 0; j < THREAD_QUEUE_CNT; j++) {
                sum += group[i][j].evcnt;
            }
            if (sum != 0 && affinity_group == -1) {
                affinity_group = i;
            }

#ifdef TEST_DEBUG
        printf("READ_M: group %d sum %d, affinity group: %d\n", i, sum, affinity_group);
#endif    

            if (i == affinity_group) {   
                if (sum != k) {
                    err(1, "affinity group sum != 1");
                }
            } else {
                if (sum != 0) {
                    err(1, "non-affinity group sum != 0");
                }
            }
        }   
    }


#ifdef TEST_DEBUG
    printf("READ_M: finished testing, system shutting down...\n");
#endif
    for(int i = 0; i < THREAD_QUEUE_CNT * ncpu; i++) {
        socket_push(g_sockfd[1], 'e');

        sem_wait(&g_sem_driver);
    }

    for (int i = 0; i < ncpu; i++) {
        for (int j = 0; j < THREAD_QUEUE_CNT; j++) {
            pthread_join(group[i][j].thrd, NULL);
        }
        free(group[i]);
    }
    free(group);

#ifdef TEST_DEBUG
    printf("READ_M: clearing kevent...\n");
#endif
    EV_SET(&kev, g_sockfd[0], EVFILT_READ, EV_DELETE, 0, 0, &g_sockfd[0]);

    error = kevent(g_kqfd, &kev, 1, NULL, 0, NULL);

    if (error == -1) {
#ifdef TEST_DEBUG
        printf("READ_M: kevent delete failed with %d\n", errno);
#endif   
        err(1, "kevent_delete");     
    }
#ifdef TEST_DEBUG
    printf("READ_M: closing sockets...\n");
#endif
    close(g_sockfd[0]);
    close(g_sockfd[1]);

    success();
}


/***************************
 * Brutal test
 ***************************/
#define THREAD_BRUTE_CNT (32)
#define SOCK_BRUTE_CNT (64)
#define PACKET_BRUTE_CNT (10000)
#define THREAD_EXIT_PROB (50)

#define RAND_SLEEP (29)
#define RAND_SEND_SLEEP (7)


int brute_sockfd[SOCK_BRUTE_CNT][2];
struct thread_info brute_threadinfo[THREAD_BRUTE_CNT];

static void*
test_socket_brutal_worker(void* args)
{
    struct thread_info *info = (struct thread_info *) args;
    char dat;
    struct kevent *ret;

    while (1) {
#ifdef TEST_DEBUG
        printf("READ_M: thread %d waiting for events\n", info->tid); 
#endif
        ret = kevent_get(g_kqfd);
#ifdef TEST_DEBUG
        printf("READ_M: thread %d woke up\n", info->tid); 
#endif

        if ((rand() % 100) < THREAD_EXIT_PROB) {
            pthread_mutex_lock(&info->lock);
#ifdef TEST_DEBUG
            printf("READ_M: thread %d trying to fake crash. Can crash: %d\n", info->tid, info->can_crash); 
#endif
            if (info->can_crash) {
                pthread_create(&info->thrd, NULL, test_socket_brutal_worker, info);
                pthread_mutex_unlock(&info->lock);
                free(ret);
                pthread_exit(0);
            }
            pthread_mutex_unlock(&info->lock);
        }

        dat = socket_pop(ret->ident);
        free(ret);

        if (dat == 'e')
            break;

        info->evcnt++;

        usleep(rand() % RAND_SLEEP);
    }
#ifdef TEST_DEBUG
        printf("READ_M: thread %d exiting...\n", info->tid); 
#endif
    pthread_exit(0);

    return NULL;
}

static void
test_socket_brutal()
{
    struct kevent kev;

    const char *test_id = "[Multi]kevent(brutal)";

    test_begin(test_id);

    for (int i = 0; i < SOCK_BRUTE_CNT; i++) {

        /* Create a connected pair of full-duplex sockets for testing socket events */
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, &brute_sockfd[i][0]) < 0) {
            err(1, "kevent_socket"); 
        }

        
        EV_SET(&kev, brute_sockfd[i][0], EVFILT_READ, EV_ADD, 0, 0, &brute_sockfd[i][0]);

        if (kevent(g_kqfd, &kev, 1, NULL, 0, NULL) == -1) {
            err(1, "kevent_brutal_add");     
        }
    }

    srand(time(NULL));

#ifdef TEST_DEBUG
    printf("READ_M: creating %d threads...\n", THREAD_BRUTE_CNT);
#endif
    for (int i = 0; i < THREAD_BRUTE_CNT; i++) {
        brute_threadinfo[i].tid = i;
        brute_threadinfo[i].evcnt = 0;
        brute_threadinfo[i].can_crash = ((i % 10) != 0);
        pthread_create(&brute_threadinfo[i].thrd, NULL, test_socket_brutal_worker, &brute_threadinfo[i]);
    }

    sleep(3);

    for(int i = 0; i < PACKET_BRUTE_CNT; i++) {
#ifdef TEST_DEBUG
    printf("READ_M: processing packet %d\n", i);
#endif
        socket_push(brute_sockfd[rand() % SOCK_BRUTE_CNT][1], '.');
        usleep(rand() % RAND_SEND_SLEEP);
    }

    while (1) {
        int sum = 0;
        for (int i = 0; i < THREAD_BRUTE_CNT; i++) {
            sum += brute_threadinfo[i].evcnt;
        }

        if (sum == PACKET_BRUTE_CNT) {
            break;
        }

#ifdef TEST_DEBUG
    printf("READ_M: waiting for all packets to finish processing. Cur: %d Tgt: %d\n", sum, PACKET_BRUTE_CNT);
#endif
        sleep(1);
    }

    /* shutdown the systems */
#ifdef TEST_DEBUG
    printf("READ_M: finished testing, system shutting down...\n");
#endif
    for (int i = 0; i < THREAD_BRUTE_CNT; i++) {
        pthread_mutex_lock(&brute_threadinfo[i].lock);
        brute_threadinfo[i].can_crash = 0;
        pthread_mutex_unlock(&brute_threadinfo[i].lock);
    }

    for(int i = 0; i < THREAD_BRUTE_CNT; i++) {
        socket_push(brute_sockfd[rand() % SOCK_BRUTE_CNT][1], 'e');
    }

    for (int i = 0; i < THREAD_BRUTE_CNT; i++) {
        pthread_join(brute_threadinfo[i].thrd, NULL);
    }

    for (int i = 0; i < SOCK_BRUTE_CNT; i++) {
        EV_SET(&kev, brute_sockfd[i][0], EVFILT_READ, EV_DELETE, 0, 0, &brute_sockfd[i][0]);

        if (kevent(g_kqfd, &kev, 1, NULL, 0, NULL) == -1) {
            err(1, "kevent_brutal_delete");     
        }
    }

    success();
}


void
test_evfilt_read_m()
{
    int flags = 0;
    g_kqfd = kqueue();
    int error = ioctl(g_kqfd, FKQMULTI, &flags);
    if (error == -1) {
        err(1, "ioctl");
    }

    test_socket_read();
    test_socket_brutal();

    close(g_kqfd);

    /* test scheduler */
    flags = KQ_SCHED_QUEUE;
    g_kqfd = kqueue();
    error = ioctl(g_kqfd, FKQMULTI, &flags);

    test_socket_queue();
    test_socket_brutal();

    close(g_kqfd);
}