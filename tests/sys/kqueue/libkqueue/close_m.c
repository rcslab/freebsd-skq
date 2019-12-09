
#include "common.h"
#include "common_m.h"

#include <sys/event.h>
#include <sys/ioctl.h>
#include <semaphore.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <pthread_np.h>
#include <unistd.h>
#include <stdlib.h>


#define SOCK_CLOSE_PROB (30)
#define CLOSER_RAND_DELAY (17)
#define QUEUE_RAND_DELAY (7)
#define CLOSER_TOT_SOCK (8)
#define PACKET_CNT (CLOSER_TOT_SOCK * 1024)
#define THREAD_CNT (8)

static int close_socks[CLOSER_TOT_SOCK][2];
static volatile int close_stop;
static int g_kqfd;

static void *
socket_closer(void* args)
{
    struct kevent kev;

    while(!close_stop) {
        int ran = rand() % CLOSER_TOT_SOCK;
        printf("closed idx %d...\n", ran);

        close(close_socks[ran][0]);
        close(close_socks[ran][1]);

        /* events are supposed to clean up themselves after fd invalidates */

        if (socketpair(AF_UNIX, SOCK_STREAM, 0, &close_socks[ran][0]) < 0) {
            err(1, "kevent_socket"); 
        }

        EV_SET(&kev, close_socks[ran][0], EVFILT_READ, EV_ADD, 0, 0, &close_socks[ran][0]);
        
        if (kevent(g_kqfd, &kev, 1, NULL, 0, NULL) == -1) {
            err(1, "kevent_brutal_add");     
        }

        usleep(rand() % CLOSER_RAND_DELAY);
    }

    return NULL;
}

static void *
socket_worker(void* args)
{
    char dat;
    struct kevent *ret;

    while (1) {
        ret = kevent_get(g_kqfd);


        printf("processing packet...\n");

        dat = socket_pop_igerr(ret->ident);

        if (dat == 'e')
            break;

        free(ret);
    }

    return NULL;
}

void
test_socket_close(char* name)
{
    pthread_t workers[THREAD_CNT];
    pthread_t closer;
    
    char id[256];
    struct kevent kev;

    const char *test_id = "[Multi]kevent(close) - ";

    strcpy(id, test_id);
    strcat(id, name);

    test_begin(id);

    close_stop = 0;
    srand(time(NULL));

    int flags = KQSCHED_MAKE(KQ_SCHED_CPU, 2, 0, 0);
    g_kqfd = kqueue();
    int error = ioctl(g_kqfd, FKQMULTI, &flags);
    if (error == -1) {
        err(1, "ioctl");
    }

    for (int i = 0; i < CLOSER_TOT_SOCK; i++) {

        if (socketpair(AF_UNIX, SOCK_STREAM, 0, &close_socks[i][0]) < 0) {
            err(1, "kevent_socket"); 
        }

        EV_SET(&kev, close_socks[i][0], EVFILT_READ, EV_ADD, 0, 0, &close_socks[i][0]);

        if (kevent(g_kqfd, &kev, 1, NULL, 0, NULL) == -1) {
            err(1, "kevent_brutal_add");
        }
    }
    
    for (int i = 0; i < THREAD_CNT; i++) {
        pthread_create(&workers[i], NULL, socket_worker, NULL);
    }
    pthread_create(&closer, NULL, socket_closer, NULL);


    for(int i = 0; i < PACKET_CNT; i++) {
        socket_push_igerr(close_socks[rand() % CLOSER_TOT_SOCK][1], '.');
        usleep(rand() % QUEUE_RAND_DELAY);
    }

    printf("Stopping closer...\n");
    close_stop = 1;
    pthread_join(closer, NULL);
    printf("Closer stopped!\n");

    for (int i = 0; i < THREAD_CNT; i++) {
        socket_push(close_socks[rand() % CLOSER_TOT_SOCK][1], 'e');
    }
    for (int i = 0; i < THREAD_CNT; i++) {
        pthread_join(workers[i], NULL);
    }

    for (int i = 0; i < CLOSER_TOT_SOCK; i++) {
        close(close_socks[i][0]);
        close(close_socks[i][1]);
    }

    printf("Threads stopped!\n");

    success();
}

