#ifndef RELAY_SOCKET_WORKER_H
#define RELAY_SOCKET_WORKER_H

#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

#include "blob.h"
#include "config.h"
#include "control.h"
#include "disk_writer.h"
#include "relay.h"
#include "relay_threads.h"
#include "setproctitle.h"
#include "socket_util.h"
#include "stats.h"
#include "timer.h"
#include "worker_base.h"

struct socket_worker {
    struct worker_base base;

    queue_t queue;

    stats_basic_counters_t counters;
    stats_basic_counters_t recents;
    stats_basic_counters_t totals;

    relay_socket_t output_socket;

    volatile uint32_t exists;
    disk_writer_t *disk_writer;

     TAILQ_ENTRY(socket_worker) entries;
};
typedef struct socket_worker socket_worker_t;

socket_worker_t *socket_worker_create(const char *arg, const config_t * config);
void socket_worker_destroy(socket_worker_t * worker);
void *socket_worker_thread(void *arg);

/* worker sleeps while it waits for work
 * XXX this should be configurable */
static INLINE void worker_wait_millisec(unsigned int millisec)
{
    /* XXX should be replaced by more standard nanosleep() */
    usleep(millisec * 1000);
}

#endif				/* #ifndef RELAY_SOCKET_WORKER_H */