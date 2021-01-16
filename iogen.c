// SPDX-License-Identifier: GPL-3.0-or-later
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include "fdmonbench.h"

char *iogen_init(struct iogen *g, const struct options *opts)
{
    g->num_fds = opts->num_fds;
    g->msg_size = opts->msg_size;

    memset(&g->random_buf, 0, sizeof(g->random_buf));
    initstate_r(gettid(), g->random_state, sizeof(g->random_state), &g->random_buf);

    g->engine_fds = malloc(sizeof(g->engine_fds[0]) * opts->num_fds);
    g->iogen_fds = malloc(sizeof(g->engine_fds[0]) * opts->num_fds);
    g->msgbuf = calloc(1, opts->msg_size);
    if (!g->engine_fds || !g->iogen_fds || !g->msgbuf) {
        free(g->engine_fds);
        free(g->iogen_fds);
        free(g->msgbuf);
        return strdup("Out of memory");
    }

    for (int i = 0; i < opts->num_fds; i++) {
        int fds[2];
        int ret;

        ret = socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds);
        if (ret < 0) {
            while (i-- > 0) {
                close(g->engine_fds[i]);
                close(g->iogen_fds[i]);
                free(g->engine_fds);
                free(g->iogen_fds);
                free(g->msgbuf);
                return strdup("socketpair failed\n");
            }
        }

        g->engine_fds[i] = fds[0];
        g->iogen_fds[i] = fds[1];

        /* The engine fd is non-blocking, the iogen fd is blocking */
        fcntl(g->engine_fds[i], F_SETFL,
              O_NONBLOCK | fcntl(g->engine_fds[i], F_GETFL, 0));
    }

    return NULL;
}

void iogen_cleanup(struct iogen *g)
{
    for (int i = 0; i < g->num_fds; i++) {
        close(g->engine_fds[i]);
        close(g->iogen_fds[i]);
    }

    free(g->engine_fds);
    free(g->iogen_fds);
    free(g->msgbuf);
}

static void iogen_print_stats(struct iogen *g,
                              struct rusage *start_rusage,
                              struct rusage *finish_rusage,
                              struct timespec *start_time,
                              struct timespec *finish_time)
{
    double duration_secs;
    double cpu_secs;
    double rtps;
    double rtpcs;

    duration_secs = finish_time->tv_sec + finish_time->tv_nsec / 1000000000.0 -
                    (start_time->tv_sec + start_time->tv_nsec / 1000000000.0);
    rtps = g->num_ios / duration_secs;

    cpu_secs = finish_rusage->ru_utime.tv_sec +
               finish_rusage->ru_utime.tv_usec / 1000000.0 +
               finish_rusage->ru_stime.tv_sec +
               finish_rusage->ru_stime.tv_usec / 1000000.0 -
               (start_rusage->ru_utime.tv_sec +
                start_rusage->ru_utime.tv_usec / 1000000.0 +
                start_rusage->ru_stime.tv_sec +
                start_rusage->ru_stime.tv_usec / 1000000.0);
    rtpcs = g->num_ios / cpu_secs;

    printf("Duration (s),Total Roundtrips,Roundtrips/sec,CPU usage (s),Roundtrips/cpusec\n");
    printf("%g,%lu,%g,%g,%g\n", duration_secs, g->num_ios, rtps, cpu_secs, rtpcs);
}

void iogen_run(struct iogen *g, volatile bool *stop)
{
    struct rusage start_rusage;
    struct rusage finish_rusage;
    struct timespec start_time;
    struct timespec finish_time;
    int fd = 0;

    getrusage(RUSAGE_SELF, &start_rusage);
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    while (!*stop) {
        ssize_t ret;
        int32_t r;

        ret = write(g->iogen_fds[fd], g->msgbuf, g->msg_size);
        if (*stop) { /* Expected EINTR */
            break;
        }
        if (ret != (ssize_t)g->msg_size) {
            fprintf(stderr, "Write failed ret %zd errno %d\n", ret, errno);
            break;
        }

        ret = read(g->iogen_fds[fd], g->msgbuf, g->msg_size);
        if (*stop) { /* Expected EINTR */
            break;
        }
        if (ret != (ssize_t)g->msg_size) {
            fprintf(stderr, "Read failed ret %zd errno %d\n", ret, errno);
            break;
        }

        g->num_ios++;

        random_r(&g->random_buf, &r);
        fd = r % g->num_fds;
    }

    clock_gettime(CLOCK_MONOTONIC, &finish_time);
    getrusage(RUSAGE_SELF, &finish_rusage);

    iogen_print_stats(g, &start_rusage, &finish_rusage,
                      &start_time, &finish_time);
}
