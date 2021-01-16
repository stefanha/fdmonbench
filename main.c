// SPDX-License-Identifier: GPL-3.0-or-later
#include <getopt.h>
#include <signal.h>
#include "fdmonbench.h"

enum {
    OPTION_ENGINE = 256,
    OPTION_NUM_ENGINES,
    OPTION_NUM_FDS,
    OPTION_MSG_SIZE,
    OPTION_EXCLUSIVE,
    OPTION_DURATION_SECS,
};

static const struct option longopts[] = {
    {"duration-secs", required_argument, NULL, OPTION_DURATION_SECS},
    {"engine", required_argument, NULL, OPTION_ENGINE},
    {"exclusive", required_argument, NULL, OPTION_EXCLUSIVE},
    {"help", no_argument, NULL, '?'},
    {"msg-size", required_argument, NULL, OPTION_MSG_SIZE},
    {"num-engines", required_argument, NULL, OPTION_NUM_ENGINES},
    {"num-fds", required_argument, NULL, OPTION_NUM_FDS},
    {NULL, 0, NULL, 0},
};

static void usage(const char *argv0)
{
    fprintf(stderr, "Usage: %s [OPTION]...\n", argv0);
    fprintf(stderr, "Perform file descriptor monitoring benchmarking.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  --duration-secs=<int>  run for number of seconds (default: 30)\n");
    fprintf(stderr, "  --engine=epoll|poll|select\n");
    fprintf(stderr, "                         set fd monitoring engine (default: select)\n");
    fprintf(stderr, "  --exclusive=0|1        use EPOLLEXCLUSIVE (default: 0)\n");
    fprintf(stderr, "  --help                 print this help\n");
    fprintf(stderr, "  --msg-size             number of bytes per message (default: 1)\n");
    fprintf(stderr, "  --num-engines          number of engine instances (default: 1)\n");
    fprintf(stderr, "  --num-fds              number of file descriptors (default: 1)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "This program is released under the GNU General Public License v3.0 or later.\n");
}

static bool parse_options(struct options *opts, int argc, char **argv)
{
    const struct engine_ops *engines[] = {
        &epoll_engine_ops,
        &poll_engine_ops,
        &select_engine_ops,
        NULL,
    };

    /* Set default option values */
    *opts = (struct options){
        .engine_ops = &select_engine_ops,
        .num_engines = 1,
        .num_fds = 1,
        .msg_size = 1,
        .exclusive = false,
        .duration_secs = 30,
    };

    for (;;) {
        int c;

        c = getopt_long(argc, argv, "", longopts, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
        case OPTION_ENGINE: {
            int i;

            for (i = 0; engines[i]; i++) {
                if (strcmp(optarg, engines[i]->name) == 0) {
                    break;
                }
            }

            if (engines[i] == 0) {
                fprintf(stderr, "Unknown engine\n");
                usage(argv[0]);
                return false;
            }

            opts->engine_ops = engines[i];
        } break;

        case OPTION_NUM_ENGINES: {
            unsigned long ret = strtoul(optarg, NULL, 10);

            if (ret > (unsigned long)LONG_MAX || ret == 0) {
                fprintf(stderr, "Invalid number of engines\n");
                usage(argv[0]);
                return false;
            }

            opts->num_engines = ret;
        } break;

        case OPTION_NUM_FDS: {
            unsigned long ret = strtoul(optarg, NULL, 10);

            if (ret > (unsigned long)LONG_MAX || ret == 0) {
                fprintf(stderr, "Invalid number of fds\n");
                usage(argv[0]);
                return false;
            }

            opts->num_fds = ret;
        } break;

        case OPTION_MSG_SIZE: {
            unsigned long ret = strtoul(optarg, NULL, 10);

            if (ret > (unsigned long)LONG_MAX || ret == 0) {
                fprintf(stderr, "Invalid message size\n");
                usage(argv[0]);
                return false;
            }

            opts->msg_size = ret;
        } break;

        case OPTION_EXCLUSIVE:
            if (strcmp(optarg, "0") == 0) {
                opts->exclusive = 0;
            } else if (strcmp(optarg, "1") == 0) {
                opts->exclusive = 1;
            } else {
                fprintf(stderr, "The value of exclusive must be 0 or 1\n");
                usage(argv[0]);
                return false;
            }
            break;

        case OPTION_DURATION_SECS: {
            unsigned long ret = strtoul(optarg, NULL, 10);

            if (ret > (unsigned long)LONG_MAX || ret == 0) {
                fprintf(stderr, "Invalid duration-secs value\n");
                usage(argv[0]);
                return false;
            }

            opts->duration_secs = ret;
        } break;

        case '?':
            usage(argv[0]);
            return false;

        default:
            fprintf(stderr, "unhandled getopt_long() return value %d\n", c);
            return false;
        }
    }

    if (optind != argc) {
        usage(argv[0]);
        return false;
    }

    if (opts->exclusive && !opts->engine_ops->supports_exclusive) {
        fprintf(stderr, "%s engine does not support exclusive=1\n",
                opts->engine_ops->name);
        return NULL;
    }

    return true;
}

/* Flag that tells iogen_run() to stop, set by SIGALRM handler */
static volatile bool sigalrm_triggered;

static void sigalrm_handler(__attribute__((unused)) int signo)
{
    sigalrm_triggered = true;
}

static void register_sigalrm_handler(void)
{
    struct sigaction act = {
        .sa_handler = sigalrm_handler,
    };

    sigaction(SIGALRM, &act, NULL);
}

static void set_signal_blocked(int signum, bool block)
{
    sigset_t set;

    sigemptyset(&set);
    sigaddset(&set, signum);

    pthread_sigmask(block ? SIG_BLOCK : SIG_UNBLOCK, &set, NULL);
}

struct engine **create_engines(const struct options *opts,
                               int *fds,
                               char **errmsg)
{
    struct engine **engines;

    engines = malloc(sizeof(engines[0]) * opts->num_engines);
    if (!engines) {
        *errmsg = strdup("Out of memory");
        return NULL;
    }

    for (int i = 0; i < opts->num_engines; i++) {
        engines[i] = opts->engine_ops->create(opts, fds, errmsg);
        if (!engines[i]) {
            while (i-- > 0) {
                opts->engine_ops->destroy(engines[i]);
            }
            free(engines);
            return NULL;
        }
    }

    return engines;
}

void destroy_engines(struct engine **engines, int count)
{
    for (int i = 0; i < count; i++) {
        struct engine *e = engines[i];

        e->ops->destroy(e);
    }

    free(engines);
}

int main(int argc, char **argv)
{
    struct options opts;
    struct iogen iogen;
    struct engine **engines = NULL;
    char *errmsg = NULL;

    /* Spawned threads should not handle SIGALRM */
    set_signal_blocked(SIGALRM, true);

    register_sigalrm_handler();

    if (!parse_options(&opts, argc, argv)) {
        return EXIT_FAILURE;
    }

    errmsg = iogen_init(&iogen, &opts);
    if (errmsg) {
        goto err;
    }

    engines = create_engines(&opts, iogen.engine_fds, &errmsg);
    if (errmsg) {
        iogen_cleanup(&iogen);
        goto err;
    }

    set_signal_blocked(SIGALRM, false);
    alarm(opts.duration_secs);

    iogen_run(&iogen, &sigalrm_triggered);

    alarm(0); /* in case iogen_run() returned early */

    destroy_engines(engines, opts.num_engines);
    iogen_cleanup(&iogen);
    return EXIT_SUCCESS;

err:
    fprintf(stderr, "%s\n", errmsg);
    free(errmsg);
    return EXIT_FAILURE;
}
