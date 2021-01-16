// SPDX-License-Identifier: GPL-3.0-or-later
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include "fdmonbench.h"

struct threads_engine {
    struct engine engine;
    pthread_t *threads;
    uint8_t *msgbuf;
    size_t msg_size;
    sem_t startup_semaphore;
    int startup_fd;
    int num_fds;
};

static void *threads_fd_thread(void *opaque)
{
    struct threads_engine *te = opaque;
    int fd = te->startup_fd;

    /* Ready! */
    sem_post(&te->startup_semaphore);

    for (;;) {
        ssize_t ret;

        ret = read(fd, te->msgbuf, te->msg_size);
        if (ret != (ssize_t)te->msg_size) {
            continue;
        }
        write(fd, te->msgbuf, te->msg_size);
    }

    return NULL;
}

static struct engine *threads_create(const struct options *opts,
                                     int *fds,
                                     char **errmsg)
{
    const char *err = NULL;
    struct threads_engine *te;
    int ret;

    te = malloc(sizeof(*te));
    if (!te) {
        *errmsg = strdup("Out of memory");
        return NULL;
    }

    te->engine.ops = &threads_engine_ops;
    te->num_fds = opts->num_fds;

    te->msg_size = opts->msg_size;
    te->msgbuf = calloc(1, opts->msg_size);
    if (!te->msgbuf) {
        err = "Out of memory";
        goto err_free_se;
    }

    te->threads = malloc(sizeof(te->threads[0]) * te->num_fds);
    if (!te->threads) {
        err = "Out of memory";
        goto err_free_msgbuf;
    }

    /* The semaphore is used to wait for the thread to become ready */
    if (sem_init(&te->startup_semaphore, 0, 0) < 0) {
        err = "Failed to create startup semaphore";
        goto err_free_threads;
    }

    for (int i = 0; i < te->num_fds; i++) {
        te->startup_fd = fds[i]; /* stash it for the thread */

        /* The thread does blocking I/O */
        fcntl(fds[i], F_SETFL,
              fcntl(fds[i], F_GETFL, 0) & ~O_NONBLOCK);

        /* Start thread */
        if (pthread_create(&te->threads[i], NULL, threads_fd_thread, te) != 0) {
            err = "pthread_create failed";
err_cancel_threads:
            while (i-- > 0) {
                pthread_cancel(te->threads[i]);
                pthread_join(te->threads[i], NULL);
            }
            goto err_sem_destroy;
        }

        /* Wait for thread to become ready */
        do {
            ret = sem_wait(&te->startup_semaphore);
        } while (ret == -1 && errno == EINTR);

        if (ret < 0) {
            err = "sem_wait failed";
            pthread_join(te->threads[i], NULL);
            goto err_cancel_threads;
        }
    }

    return &te->engine;

err_sem_destroy:
    sem_destroy(&te->startup_semaphore);
err_free_threads:
    free(te->threads);
err_free_msgbuf:
    free(te->msgbuf);
err_free_se:
    free(te);
    *errmsg = strdup(err);
    return NULL;
}

static void threads_destroy(struct engine *e)
{
    struct threads_engine *te = (struct threads_engine *)e;

    for (int i = 0; i < te->num_fds; i++) {
        pthread_cancel(te->threads[i]);
        pthread_join(te->threads[i], NULL);
    }

    sem_destroy(&te->startup_semaphore);

    free(te->threads);
    free(te->msgbuf);
    free(te);
}

const struct engine_ops threads_engine_ops = {
    .name = "threads",
    .create = threads_create,
    .destroy = threads_destroy,
};
