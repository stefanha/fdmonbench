// SPDX-License-Identifier: GPL-3.0-or-later
#include <pthread.h>
#include <semaphore.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include "fdmonbench.h"

struct epoll_engine {
    struct engine engine;
    pthread_t thread;
    uint8_t *msgbuf;
    size_t msg_size;
    sem_t startup_semaphore;
    int efd; /* the eventfd */
    int epfd; /* the epoll fd */
};

static void *epoll_thread(void *opaque)
{
    const size_t maxevents = 2; /* we only expect 1 fd and maybe the eventfd */
    struct epoll_engine *pe = opaque;
    struct epoll_event events[maxevents];

    /* Ready! */
    sem_post(&pe->startup_semaphore);

    for (;;) {
        int ret;

        ret = epoll_wait(pe->epfd, events, maxevents, -1);

        for (int i = 0; i < ret; i++) {
            int fd = events[i].data.fd;

            /* Handle our eventfd */
            if (fd == pe->efd) {
                uint64_t eventfd_val;

                if (read(fd, &eventfd_val, sizeof(eventfd_val)) != sizeof(eventfd_val)) {
                    continue;
                }

                /* Stop thread */
                return NULL;
            }

            if (read(fd, pe->msgbuf, pe->msg_size) <= 0) {
                continue;
            }
            write(fd, pe->msgbuf, pe->msg_size);
        }
    }

    return NULL;
}

static struct engine *epoll_do_create(const struct options *opts,
                                      int *fds,
                                      char **errmsg)
{
    const char *err = NULL;
    struct epoll_engine *pe;
    int event_flags = opts->exclusive ? EPOLLEXCLUSIVE : 0;
    struct epoll_event event = {
        .events = EPOLLIN | event_flags,
    };
    int ret;

    pe = malloc(sizeof(*pe));
    if (!pe) {
        *errmsg = strdup("Out of memory");
        return NULL;
    }

    pe->engine.ops = &epoll_engine_ops;

    pe->msg_size = opts->msg_size;
    pe->msgbuf = calloc(1, opts->msg_size);
    if (!pe->msgbuf) {
        err = "Out of memory";
        goto err_free_se;
    }

    pe->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (pe->epfd < 0) {
        err = "epoll_create1 failed";
        goto err_free_msgbuf;
    }

    for (int i = 0; i < opts->num_fds; i++) {
        event.data.fd = fds[i],

        ret = epoll_ctl(pe->epfd, EPOLL_CTL_ADD, fds[i], &event);
        if (ret < 0) {
            err = "epoll_ctl failed";
            goto err_close_epfd;
        }
    }

    /* The eventfd is used to tell the thread to stop */
    pe->efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (pe->efd < 0) {
        err = "Eventfd creation failed";
        goto err_close_epfd;
    }

    event.data.fd = pe->efd;

    ret = epoll_ctl(pe->epfd, EPOLL_CTL_ADD, pe->efd, &event);
    if (ret < 0) {
        err = "epoll_ctl failed";
        goto err_close_eventfd;
    }

    /* The semaphore is used to wait for the thread to become ready */
    if (sem_init(&pe->startup_semaphore, 0, 0) < 0) {
        err = "Failed to create startup semaphore";
        goto err_close_eventfd;
    }

    /* Start thread */
    if (pthread_create(&pe->thread, NULL, epoll_thread, pe) != 0) {
        err = "pthread_create failed";
        goto err_sem_destroy;
    }

    /* Wait for thread to become ready */
    do {
        ret = sem_wait(&pe->startup_semaphore);
    } while (ret == -1 && errno == EINTR);

    if (ret < 0) {
        err = "sem_wait failed";
        goto err_pthread_join;
    }

    return &pe->engine;

err_pthread_join:
    pthread_join(pe->thread, NULL);
err_sem_destroy:
    sem_destroy(&pe->startup_semaphore);
err_close_eventfd:
    close(pe->efd);
err_close_epfd:
    close(pe->epfd);
err_free_msgbuf:
    free(pe->msgbuf);
err_free_se:
    free(pe);
    *errmsg = strdup(err);
    return NULL;
}

static void epoll_destroy(struct engine *e)
{
    struct epoll_engine *pe = (struct epoll_engine *)e;
    uint64_t eventfd_val = 1;

    write(pe->efd, &eventfd_val, sizeof(eventfd_val));
    pthread_join(pe->thread, NULL);

    sem_destroy(&pe->startup_semaphore);

    close(pe->efd);
    close(pe->epfd);
    free(pe->msgbuf);
    free(pe);
}

const struct engine_ops epoll_engine_ops = {
    .name = "epoll",
    .create = epoll_do_create,
    .destroy = epoll_destroy,
    .supports_exclusive = true,
};
