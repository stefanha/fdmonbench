// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

struct engine_ops;
extern const struct engine_ops select_engine_ops;

struct options {
    /* Engine type */
    const struct engine_ops *engine_ops;

    /* Number of engine instances */
    int num_engines;

    /* Number of file descriptors to monitor */
    int num_fds;

    /* Number of bytes to transfer in each message */
    size_t msg_size;

    /* Use EPOLLEXCLUSIVE? */
    bool exclusive;

    /* How long to run */
    int duration_secs;
};

/* An engine instance */
struct engine {
    const struct engine_ops *ops;
};

/* Engine operations */
struct engine_ops {
    const char *name;

    /* Create a new engine instance from given options */
    struct engine *(*create)(const struct options *opts,
                             int *fds,
                             char **errmsg);

    /* Destroy an engine instance and release its resources */
    void (*destroy)(struct engine *e);

    /* Is EPOLLEXCLUSIVE supported? */
    bool supports_exclusive;
};

/* I/O generator */
struct iogen {
    int *engine_fds;
    int *iogen_fds;
    int num_fds;

    uint8_t *msgbuf;
    size_t msg_size;

    struct random_data random_buf;
    char random_state[256];

    /* Number of completed I/O operations */
    unsigned long num_ios;
};

char *iogen_init(struct iogen *g, const struct options *opts);
void iogen_cleanup(struct iogen *g);
void iogen_run(struct iogen *g, volatile bool *stop);
