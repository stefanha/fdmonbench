// SPDX-License-Identifier: GPL-3.0-or-later
#include <poll.h>
#include <liburing.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include "fdmonbench.h"

struct io_uring_engine {
    struct engine engine;
    pthread_t thread;
    uint8_t *msgbuf;
    size_t msg_size;
    sem_t startup_semaphore;
    struct io_uring ring;
    int efd; /* the eventfd */
    int poll_mask; /* the events we are monitoring */
};

static void io_uring_add_poll_sqe(struct io_uring_engine *pe, int fd)
{
    struct io_uring_sqe *sqe;

    sqe = io_uring_get_sqe(&pe->ring);
    if (!sqe) {
        int ret;

        /* We allocate sufficient resources upfront so this should not fail */

        ret = io_uring_submit(&pe->ring);
        if (ret < 0) {
            fprintf(stderr, "io_uring_submit failed with %d\n", ret);
            return;
        }

        sqe = io_uring_get_sqe(&pe->ring);
        if (!sqe) {
            fprintf(stderr, "io_uring_get_sqe failed\n");
            return;
        }
    }

    io_uring_prep_poll_add(sqe, fd, pe->poll_mask);
    io_uring_sqe_set_data(sqe, (void *)(uintptr_t)fd);
}

static void *io_uring_thread(void *opaque)
{
    struct io_uring_engine *pe = opaque;

    /* Ready! */
    sem_post(&pe->startup_semaphore);

    for (;;) {
        struct io_uring_cqe *cqe;
        unsigned head;

        io_uring_submit_and_wait(&pe->ring, 1);

        io_uring_for_each_cqe(&pe->ring, head, cqe) {
            int fd = cqe->user_data;

            /* Handle our eventfd */
            if (fd == pe->efd) {
                uint64_t eventfd_val;

                if (read(fd, &eventfd_val, sizeof(eventfd_val)) != sizeof(eventfd_val)) {
                    goto requeue;
                }

                /* Stop thread */
                return NULL;
            }

            if (read(fd, pe->msgbuf, pe->msg_size) <= 0) {
                goto requeue;
            }
            write(fd, pe->msgbuf, pe->msg_size);

requeue:    /* Submit another IORING_OP_POLL_ADD since it's a oneshot */
            io_uring_cq_advance(&pe->ring, 1);
            io_uring_add_poll_sqe(pe, fd);
        }
    }

    return NULL;
}

static struct engine *io_uring_create(const struct options *opts,
                                      int *fds,
                                      char **errmsg)
{
    const char *err = NULL;
    struct io_uring_engine *pe;
    int ret;

    pe = malloc(sizeof(*pe));
    if (!pe) {
        *errmsg = strdup("Out of memory");
        return NULL;
    }

    pe->engine.ops = &io_uring_engine_ops;

    pe->poll_mask = POLLIN;
    if (opts->exclusive) {
        pe->poll_mask |= EPOLLEXCLUSIVE;
    }

    pe->msg_size = opts->msg_size;
    pe->msgbuf = calloc(1, opts->msg_size);
    if (!pe->msgbuf) {
        err = "Out of memory";
        goto err_free_se;
    }

    ret = io_uring_queue_init(64, &pe->ring, 0);
    if (ret < 0) {
        err = "io_uring_queue_init failed (do you need to increase ulimit -l?)";
        goto err_free_msgbuf;
    }

    for (int i = 0; i < opts->num_fds; i++) {
        io_uring_add_poll_sqe(pe, fds[i]);
    }

    /* The eventfd is used to tell the thread to stop */
    pe->efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (pe->efd < 0) {
        err = "Eventfd creation failed";
        goto err_queue_exit;
    }

    io_uring_add_poll_sqe(pe, pe->efd);

    /* Flush pending sqes to kernel */
    io_uring_submit(&pe->ring);

    /* The semaphore is used to wait for the thread to become ready */
    if (sem_init(&pe->startup_semaphore, 0, 0) < 0) {
        err = "Failed to create startup semaphore";
        goto err_close_eventfd;
    }

    /* Start thread */
    if (pthread_create(&pe->thread, NULL, io_uring_thread, pe) != 0) {
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
err_queue_exit:
    io_uring_queue_exit(&pe->ring);
err_free_msgbuf:
    free(pe->msgbuf);
err_free_se:
    free(pe);
    *errmsg = strdup(err);
    return NULL;
}

static void io_uring_destroy(struct engine *e)
{
    struct io_uring_engine *pe = (struct io_uring_engine *)e;
    uint64_t eventfd_val = 1;

    write(pe->efd, &eventfd_val, sizeof(eventfd_val));
    pthread_join(pe->thread, NULL);

    sem_destroy(&pe->startup_semaphore);

    close(pe->efd);
    io_uring_queue_exit(&pe->ring);
    free(pe->msgbuf);
    free(pe);
}

const struct engine_ops io_uring_engine_ops = {
    .name = "io_uring",
    .create = io_uring_create,
    .destroy = io_uring_destroy,
    .supports_exclusive = true,
};
