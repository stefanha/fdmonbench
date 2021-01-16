// SPDX-License-Identifier: GPL-3.0-or-later
#include <pthread.h>
#include <semaphore.h>
#include <sys/select.h>
#include <sys/eventfd.h>
#include "fdmonbench.h"

struct select_engine {
    struct engine engine;
    pthread_t thread;
    uint8_t *msgbuf;
    size_t msg_size;
    int *fds;
    int num_fds;
    sem_t startup_semaphore;
};

static void *select_thread(void *opaque)
{
    struct select_engine *se = opaque;
    fd_set readfds;
    int nfds = 0;
    int ret;
    int i;

    /* Calculate nfds */
    for (i = 0; i < se->num_fds; i++) {
        if (se->fds[i] + 1 > nfds) {
            nfds = se->fds[i] + 1;
        }
    }

    /* Ready! */
    sem_post(&se->startup_semaphore);

    for (;;) {
        /* Initialize readfds */
        FD_ZERO(&readfds);
        for (i = 0; i < se->num_fds; i++) {
            FD_SET(se->fds[i], &readfds);
        }

        ret = select(nfds, &readfds, NULL, NULL, NULL);

        for (i = 0; ret > 0 && i < se->num_fds; i++) {
            int fd = se->fds[i];

            if (!FD_ISSET(fd, &readfds)) {
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

            if (read(fd, se->msgbuf, se->msg_size) <= 0) {
                continue;
            }
            write(fd, se->msgbuf, se->msg_size);
            ret--;
        }
    }

    return NULL;
}

static struct engine *select_create(const struct options *opts,
                                    int *fds,
                                    char **errmsg)
{
    const char *err = NULL;
    struct select_engine *se;
    int ret;

    if (opts->exclusive) {
        *errmsg = strdup("select engine does not support exclusive=1");
        return NULL;
    }

    for (int i = 0; i < opts->num_fds; i++) {
        if (fds[i] >= FD_SETSIZE) {
            *errmsg = strdup("Maximum number of fds exceeded for select engine");
            return NULL;
        }
    }

    se = malloc(sizeof(*se));
    if (!se) {
        *errmsg = strdup("Out of memory");
        return NULL;
    }

    se->engine.ops = &select_engine_ops;

    se->msg_size = opts->msg_size;
    se->msgbuf = calloc(1, opts->msg_size);
    if (!se->msgbuf) {
        err = "Out of memory";
        goto err_free_se;
    }

    se->fds = malloc(sizeof(fds[0]) * (opts->num_fds + 1));
    if (!se->fds) {
        err = "Out of memory";
        goto err_free_msgbuf;
    }
    memcpy(&se->fds[1], fds, sizeof(fds[0]) * opts->num_fds);

    /* The eventfd is used to tell the thread to stop */
    se->fds[0] = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (se->fds[0] < 0) {
        err = "Eventfd creation failed";
        goto err_free_fds;
    }
    if (se->fds[0] >= FD_SETSIZE) {
        err = "Maximum number of fds exceeded by eventfd";
        goto err_close_eventfd;
    }

    se->num_fds = opts->num_fds + 1;

    /* The semaphore is used to wait for the thread to become ready */
    if (sem_init(&se->startup_semaphore, 0, 0) < 0) {
        err = "Failed to create startup semaphore";
        goto err_close_eventfd;
    }

    /* Start thread */
    if (pthread_create(&se->thread, NULL, select_thread, se) != 0) {
        err = "pthread_create failed";
        goto err_sem_destroy;
    }

    /* Wait for thread to become ready */
    do {
        ret = sem_wait(&se->startup_semaphore);
    } while (ret == -1 && errno == EINTR);

    if (ret < 0) {
        err = "sem_wait failed";
        goto err_pthread_join;
    }

    return &se->engine;

err_pthread_join:
    pthread_join(se->thread, NULL);
err_sem_destroy:
    sem_destroy(&se->startup_semaphore);
err_close_eventfd:
    close(se->fds[0]);
err_free_fds:
    free(se->fds);
err_free_msgbuf:
    free(se->msgbuf);
err_free_se:
    free(se);
    *errmsg = strdup(err);
    return NULL;
}

static void select_destroy(struct engine *e)
{
    struct select_engine *se = (struct select_engine *)e;
    uint64_t eventfd_val = 1;

    write(se->fds[0], &eventfd_val, sizeof(eventfd_val));
    pthread_join(se->thread, NULL);

    sem_destroy(&se->startup_semaphore);

    close(se->fds[0]);
    free(se->fds);
    free(se->msgbuf);
    free(se);
}

const struct engine_ops select_engine_ops = {
    .name = "select",
    .create = select_create,
    .destroy = select_destroy,
};
