#ifndef _STATS_H
#define _STATS_H

#include "relay_common.h"
#include "relay_threads.h"
#include "setproctitle.h"
#include <stdio.h>


#define STATSfmt "%lu"
typedef uint64_t stats_count_t;

struct stats_basic_counters {
    volatile stats_count_t received_count;       /* number of items we have received */
    volatile stats_count_t sent_count;           /* number of items we have sent */
    volatile stats_count_t partial_count;        /* number of items we have spilled */
    volatile stats_count_t spilled_count;        /* number of items we have spilled */
    volatile stats_count_t error_count;          /* number of items that had an error */
    volatile stats_count_t disk_count;           /* number of items we have written to disk */
    volatile stats_count_t disk_error_count;     /* number of items we failed to write to disk properly */

    volatile stats_count_t send_elapsed_usec;    /* elapsed time in microseconds that we spent sending data */
    volatile stats_count_t active_connections;   /* current number of active inbound tcp connections */
};
typedef struct stats_basic_counters stats_basic_counters_t;

void snapshot_stats(stats_basic_counters_t *counters, stats_basic_counters_t *totals);

#endif
