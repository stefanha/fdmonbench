// SPDX-License-Identifier: GPL-3.0-or-later
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/eventfd.h>
#include "fdmonbench.h"

struct poll_engine {
    struct engine engine;
    pthread_t thread;
    uint8_t *msgbuf;
    size_t msg_size;
    struct pollfd *pollfds;
    int num_fds;
    sem_t startup_semaphore;
};

static void *poll_thread(void *opaque)
{
    struct poll_engine *pe = opaque;

    /* Ready! */
    sem_post(&pe->startup_semaphore);

    for (;;) {
        int ret;

        ret = poll(pe->pollfds, pe->num_fds, -1);

        for (int i = 0; ret > 0 && i < pe->num_fds; i++) {
            struct pollfd *pfd = &pe->pollfds[i];
            int fd = pfd->fd;

            if (!(pfd->revents & POLLIN)) {
                continue;
            }

            /* Handle our eventfd */
            if (i == 0) {
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
            ret--;
        }
    }

    return NULL;
}

static struct engine *poll_create(const struct options *opts,
                                  int *fds,
                                  char **errmsg)
{
    const char *err = NULL;
    struct poll_engine *pe;
    int ret;

    pe = malloc(sizeof(*pe));
    if (!pe) {
        *errmsg = strdup("Out of memory");
        return NULL;
    }

    pe->engine.ops = &poll_engine_ops;

    pe->msg_size = opts->msg_size;
    pe->msgbuf = calloc(1, opts->msg_size);
    if (!pe->msgbuf) {
        err = "Out of memory";
        goto err_free_se;
    }

    pe->pollfds = calloc(opts->num_fds + 1, sizeof(pe->pollfds[0]));
    if (!pe->pollfds) {
        err = "Out of memory";
        goto err_free_msgbuf;
    }

    for (int i = 0; i < opts->num_fds; i++) {
        struct pollfd *pfd = &pe->pollfds[i + 1];

        pfd->fd = fds[i];
        pfd->events = POLLIN;
    }

    /* The eventfd is used to tell the thread to stop */
    pe->pollfds[0].fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (pe->pollfds[0].fd < 0) {
        err = "Eventfd creation failed";
        goto err_free_pollfds;
    }

    pe->pollfds[0].events = POLLIN;

    pe->num_fds = opts->num_fds + 1;

    /* The semaphore is used to wait for the thread to become ready */
    if (sem_init(&pe->startup_semaphore, 0, 0) < 0) {
        err = "Failed to create startup semaphore";
        goto err_close_eventfd;
    }

    /* Start thread */
    if (pthread_create(&pe->thread, NULL, poll_thread, pe) != 0) {
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
    close(pe->pollfds[0].fd);
err_free_pollfds:
    free(pe->pollfds);
err_free_msgbuf:
    free(pe->msgbuf);
err_free_se:
    free(pe);
    *errmsg = strdup(err);
    return NULL;
}

static void poll_destroy(struct engine *e)
{
    struct poll_engine *pe = (struct poll_engine *)e;
    uint64_t eventfd_val = 1;

    write(pe->pollfds[0].fd, &eventfd_val, sizeof(eventfd_val));
    pthread_join(pe->thread, NULL);

    sem_destroy(&pe->startup_semaphore);

    close(pe->pollfds[0].fd);
    free(pe->pollfds);
    free(pe->msgbuf);
    free(pe);
}

const struct engine_ops poll_engine_ops = {
    .name = "poll",
    .create = poll_create,
    .destroy = poll_destroy,
};
