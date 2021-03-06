#include "relay.h"

#include <dlfcn.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <unistd.h>

#include "config.h"
#include "control.h"
#include "daemonize.h"
#include "global.h"
#include "log.h"
#include "setproctitle.h"
#include "string_util.h"
#include "timer.h"
#include "socket_util.h"
#include "socket_worker_pool.h"

#define EXPECTED_HEADER_SIZE sizeof(blob_size_t)
#define ASYNC_BUFFER_SIZE (MAX_CHUNK_SIZE + EXPECTED_HEADER_SIZE)

struct tcp_client {
    unsigned char *buf;
    uint32_t pos;
};

#define PROCESS_STATUS_BUF_LEN 1024

static void sig_handler(int signum);
static void block_all_signals_inside_thread();
static void stop_listener(pthread_t server_tid);
static void final_shutdown(pthread_t server_tid);

stats_basic_counters_t RECEIVED_STATS = {
    .received_count = 0,        /* number of items we have received */
    .sent_count = 0,            /* number of items we have sent */
    .partial_count = 0,         /* number of items we have spilled */
    .spilled_count = 0,         /* number of items we have spilled */
    .dropped_count = 0,         /* number of items we have dropped */
    .error_count = 0,           /* number of items that had an error */
    .disk_count = 0,            /* number of items we have written to disk */

    .send_elapsed_usec = 0,     /* elapsed time in microseconds that we spent sending data */
    .tcp_connections = 0,       /* current number of active inbound tcp connections */
};

static void spawn(pthread_t * tid, void *(*func) (void *), void *arg, int type)
{
    pthread_t unused;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, type);
    pthread_create(tid ? tid : &unused, &attr, func, arg);
    pthread_attr_destroy(&attr);
}

static inline blob_t *buf_to_blob_enqueue(unsigned char *buf, size_t size)
{
    blob_t *b;
    if (size == 0) {
        if (0)
            WARN("Received 0 byte packet, not forwarding.");
        return NULL;
    }

    RELAY_ATOMIC_INCREMENT(RECEIVED_STATS.received_count, 1);
    b = blob_new(size);
    memcpy(BLOB_BUF_addr(b), buf, size);
    enqueue_blob_for_transmission(b);
    return b;
}

void *udp_server(void *arg)
{
    block_all_signals_inside_thread();

    relay_socket_t *s = (relay_socket_t *) arg;
#ifdef PACKETS_PER_SECOND
    uint32_t packets = 0, prev_packets = 0;
    uint32_t epoch, prev_epoch = 0;
#endif
    unsigned char buf[MAX_CHUNK_SIZE];
    while (control_is_not(RELAY_STOPPING)) {
        ssize_t received = recv(s->socket, buf, MAX_CHUNK_SIZE, 0);
#ifdef PACKETS_PER_SECOND
        if ((epoch = time(0)) != prev_epoch) {
            SAY("packets: %d", packets - prev_packets);
            prev_epoch = epoch;
            prev_packets = packets;
        }
        packets++;
#endif
        if (received < 0) {
            WARN_ERRNO("recv failed");
            break;
        }
        buf_to_blob_enqueue(buf, received);
    }
    if (control_is(RELAY_RELOADING)) {
        /* Race condition, but might help in debugging */
        WARN("udp server failed, but relay seemingly reloading");
    }
    pthread_exit(NULL);
}

/* The wire format is little-endian. */
#define EXPECTED_PACKET_SIZE(x) ((x)->buf[0] | (x)->buf[1] << 8 | (x)->buf[2] << 16 | (x)->buf[3] << 24)

/* The server socket and the client contexts. */
typedef struct {
    /* The number of clients. */
    volatile nfds_t nfds;

    /* The file descriptors.  The pfds[0] is the server socket,
     * the pfds[1...] are the client sockets. */
    struct pollfd *pfds;

    /* The clients[0] is unused (it is the server),
     * the pfds[1..] are the client contexts. */
    struct tcp_client *clients;
} tcp_server_context_t;

#define TCP_FAILURE 0
#define TCP_SUCCESS 1

static void tcp_context_init(tcp_server_context_t * ctxt)
{
    ctxt->nfds = 0;

    /* Just the server socket. */
    ctxt->pfds = calloc_or_fatal(sizeof(struct pollfd));
    if (ctxt->pfds == NULL)
        return;

    /* tcp_add_fd() will set this right soon. */
    ctxt->pfds[0].fd = -1;

    /* The clients[0] is unused but let's make it a "null client" so
     * that looping over the contexts can have less special cases. */
    ctxt->clients = calloc_or_fatal(sizeof(struct tcp_client));
    if (ctxt->clients == NULL)
        return;

    ctxt->clients[0].buf = NULL;
    ctxt->clients[0].pos = 0;
}

static void tcp_add_fd(tcp_server_context_t * ctxt, int fd)
{
    setnonblocking(fd);
    ctxt->pfds[ctxt->nfds].fd = fd;
    ctxt->pfds[ctxt->nfds].events = POLLIN;
    ctxt->nfds++;
}

static void tcp_context_realloc(tcp_server_context_t * ctxt, nfds_t n)
{
    ctxt->pfds = realloc_or_fatal(ctxt->pfds, n * sizeof(struct pollfd));
    ctxt->clients = realloc_or_fatal(ctxt->clients, n * sizeof(struct tcp_client));
}

/* Returns TCP_FAILURE if failed, TCP_SUCCESS if successful.
 * If not successful the server should probably exit. */
static int tcp_accept(tcp_server_context_t * ctxt, int server_fd)
{
    int fd = accept(server_fd, NULL, NULL);
    if (fd == -1) {
        WARN_ERRNO("accept");
        return TCP_FAILURE;
    }
    RELAY_ATOMIC_INCREMENT(RECEIVED_STATS.tcp_connections, 1);

    tcp_context_realloc(ctxt, ctxt->nfds + 1);

    ctxt->clients[ctxt->nfds].pos = 0;
    ctxt->clients[ctxt->nfds].buf = calloc_or_fatal(ASYNC_BUFFER_SIZE);
    ctxt->pfds[ctxt->nfds].revents = 0;

    tcp_add_fd(ctxt, fd);

    /* WARN("CREATE %p fd: %d", ctxt->clients[ctxt->nfds].buf, fd); */

    return TCP_SUCCESS;
}

/* Returns TCP_FAILURE if failed, TCP_SUCCESS if successful.
 * If successful, we should move on to the next connection.
 * (Note that the success may be a full or a partial packet.)
 * If not successful, this connection should probably be removed. */
static int tcp_read(tcp_server_context_t * ctxt, nfds_t i)
{
    if (!(i < ctxt->nfds)) {
        WARN("Unexpected fd %d", (int) i);
        return TCP_FAILURE;
    }

    struct tcp_client *client = &ctxt->clients[i];

    /* try to read as much as possible */
    ssize_t try_to_read = ASYNC_BUFFER_SIZE - (int) client->pos;

    if (try_to_read <= 0) {
        WARN("Invalid length: %zd, pos: %u", try_to_read, client->pos);
        return TCP_FAILURE;
    }

    ssize_t received = recv(ctxt->pfds[i].fd, client->buf + client->pos, try_to_read, 0);
    if (received <= 0) {
        if (received == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return TCP_SUCCESS;

        return TCP_FAILURE;
    }

    client->pos += received;

    /* NOTE: the flow control of this loop is somewhat unusual. */
    for (;;) {
        /* Partial header: better to declare success and retry later. */
        if (client->pos < EXPECTED_HEADER_SIZE)
            return TCP_SUCCESS;

        blob_size_t expected_packet_size = EXPECTED_PACKET_SIZE(client);

        if (expected_packet_size > MAX_CHUNK_SIZE) {
            WARN("received frame (%d) > MAX_CHUNK_SIZE (%d)", expected_packet_size, MAX_CHUNK_SIZE);
            return TCP_FAILURE;
        }

        if (client->pos >= expected_packet_size + EXPECTED_HEADER_SIZE) {
            /* Since this packet came from a TCP connection, its first four
             * bytes are supposed to be the length, so let's skip them. */
            buf_to_blob_enqueue(client->buf + EXPECTED_HEADER_SIZE, expected_packet_size);

            client->pos -= expected_packet_size + EXPECTED_HEADER_SIZE;
            if (client->pos > 0) {
                /* [ h ] [ h ] [ h ] [ h ] [ D ] [ D ] [ D ] [ h ] [ h ] [ h ] [ h ] [ D ]
                 *                                                                     ^ pos(12)
                 * after we remove the first packet + header it becomes:
                 * [ h ] [ h ] [ h ] [ h ] [ D ] [ D ] [ D ] [ h ] [ h ] [ h ] [ h ] [ D ]
                 *                           ^ pos (5)
                 * and then we copy from header + data, to position 0, 5 bytes
                 *
                 * [ h ] [ h ] [ h ] [ h ] [ D ]
                 *                           ^ pos (5) */

                memmove(client->buf, client->buf + EXPECTED_HEADER_SIZE + expected_packet_size, client->pos);
                if (client->pos >= EXPECTED_HEADER_SIZE)
                    continue;   /* there is one more packet left in the buffer, consume it */
            }
        }

        return TCP_SUCCESS;
    }
}

/* Close the given client connection. */
static void tcp_client_close(tcp_server_context_t * ctxt, nfds_t i)
{
    if (!(i < ctxt->nfds)) {
        WARN("Unexpected fd %d", (int) i);
        return;
    }

    /* We could pass in both client and i, but then there's danger of mismatch. */
    struct tcp_client *client = &ctxt->clients[i];

    /* WARN("[%d] DESTROY %p %d %d fd: %d vs %d", i, client->buf, client->x, i, ctxt->pfds[i].fd, client->fd); */

    /* In addition to releasing resources (free, close) also reset
     * the various fields to invalid values (NULL, -1) just in case
     * someone accidentally tries using them. */
    shutdown(ctxt->pfds[i].fd, SHUT_RDWR);
    close(ctxt->pfds[i].fd);
    ctxt->pfds[i].fd = -1;
    free(client->buf);
    client->buf = NULL;
}

/* Remove the client connection (first closes it) */
static void tcp_client_remove(tcp_server_context_t * ctxt, nfds_t i)
{
    if (!(i < ctxt->nfds)) {
        WARN("Unexpected fd %d", (int) i);
        return;
    }

    tcp_client_close(ctxt, i);

    /* Remove the connection by shifting left
     * the connections coming after it. */
    {
        nfds_t tail = ctxt->nfds - i - 1;
        assert(tail < ctxt->nfds);
        memcpy(ctxt->pfds + i, ctxt->pfds + i + 1, tail * sizeof(struct pollfd));
        memcpy(ctxt->clients + i, ctxt->clients + i + 1, tail * sizeof(struct tcp_client));
    }

    ctxt->nfds--;
    tcp_context_realloc(ctxt, ctxt->nfds);
    RELAY_ATOMIC_DECREMENT(RECEIVED_STATS.tcp_connections, 1);
}

static void tcp_context_close(tcp_server_context_t * ctxt)
{
    for (nfds_t i = 0; i < ctxt->nfds; i++) {
        tcp_client_close(ctxt, i);
    }
    /* Release and reset. */
    free(ctxt->pfds);
    free(ctxt->clients);
    ctxt->nfds = 0;             /* Cannot be -1 since nfds_t is unsigned. */
    ctxt->pfds = NULL;
    ctxt->clients = NULL;
}

void *tcp_server(void *arg)
{
    block_all_signals_inside_thread();

    relay_socket_t *s = (relay_socket_t *) arg;
    tcp_server_context_t ctxt;

    tcp_context_init(&ctxt);

    tcp_add_fd(&ctxt, s->socket);

    RELAY_ATOMIC_AND(RECEIVED_STATS.tcp_connections, 0);

    for (;;) {
        int rc = poll(ctxt.pfds, ctxt.nfds, s->polling_interval_millisec);
        if (rc == -1) {
            if (errno == EINTR)
                continue;
            WARN_ERRNO("poll");
            goto out;
        } else {
            for (nfds_t i = 0; i < ctxt.nfds; i++) {
                if (!ctxt.pfds[i].revents)
                    continue;
                if (ctxt.pfds[i].fd == s->socket) {
                    if (!tcp_accept(&ctxt, s->socket))
                        goto out;
                } else {
                    if (!tcp_read(&ctxt, i))
                        tcp_client_remove(&ctxt, i);
                }
            }
        }
    }

  out:
    tcp_context_close(&ctxt);
    if (control_is(RELAY_RELOADING)) {
        /* Race condition, but might help in debugging */
        WARN("tcp server failed, but relay seemingly reloading");
    }
    pthread_exit(NULL);
}

pthread_t setup_listener(config_t * config)
{
    pthread_t server_tid = 0;

    if (config == NULL || config->argv == NULL || GLOBAL.listener == NULL
        || !socketize(config->argv[0], GLOBAL.listener, IPPROTO_UDP, RELAY_CONN_IS_INBOUND, "listener")) {
        FATAL("Failed to socketize listener");
        return 0;
    }

    GLOBAL.listener->polling_interval_millisec = config->polling_interval_millisec;

    /* must open the socket BEFORE we create the worker pool */
    open_socket(GLOBAL.listener, DO_BIND | DO_REUSEADDR |
#ifdef SO_REUSEPORT
                (GLOBAL.listener->proto == IPPROTO_TCP ? DO_REUSEPORT : 0) |
#endif
                DO_EPOLLFD, 0, config->server_socket_rcvbuf_bytes);

    /* create worker pool /after/ we open the socket, otherwise we
     * might leak worker threads. */

    if (GLOBAL.listener->proto == IPPROTO_UDP)
        spawn(&server_tid, udp_server, GLOBAL.listener, PTHREAD_CREATE_JOINABLE);
    else
        spawn(&server_tid, tcp_server, GLOBAL.listener, PTHREAD_CREATE_JOINABLE);

    return server_tid;
}

static struct graphite_config *graphite_config_clone(const struct graphite_config *old_config)
{
    struct graphite_config *new_config = (struct graphite_config *) malloc(sizeof(*new_config));
    memset(new_config, 0, sizeof(*new_config));
    new_config->dest_addr = strdup(old_config->dest_addr);
    new_config->path_root = strdup(old_config->path_root);
    new_config->send_interval_millisec = old_config->send_interval_millisec;
    new_config->sleep_poll_interval_millisec = old_config->sleep_poll_interval_millisec;
    return new_config;
}

static int graphite_config_changed(const struct graphite_config *old_config, const struct graphite_config *new_config)
{
    return
        STRNE(old_config->dest_addr, new_config->dest_addr) ||
        STRNE(old_config->path_root, new_config->path_root) ||
        old_config->send_interval_millisec != new_config->send_interval_millisec ||
        old_config->sleep_poll_interval_millisec != new_config->sleep_poll_interval_millisec;
}

static void graphite_config_destroy(struct graphite_config *config)
{
    if (config == NULL || config->dest_addr == NULL || config->path_root == NULL) {
        WARN("Invalid config");
        return;
    }
    free(config->dest_addr);
    free(config->path_root);
    /* Reset the config to trap use-after-free. */
    memset(config, 0, sizeof(*config));
    free(config);
}

/* Block locking the lock file.  Once successful, write our pid to it,
 * and return the lock fd.  Returns -1 on failure. */
static int highlander_blocking_lock(config_t * config)
{
    SAY("Attempting lock file %s", config->lock_file);

    int lockfd = open(config->lock_file, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (lockfd == -1) {
        WARN_ERRNO("Failed to open lock file %s", config->lock_file);
        return -1;
    }

    /* Using flock() instead of fcntl(F_SETLKW) because of nasty
     * feature of the latter: fcntl locks are not inherited across
     * fork (or another way to look at it, the locks are by process,
     * not by fd).
     *
     * Furthermore, one of the processes possibly closing the fd makes
     * all the processes to lose the lock.  These "features" make
     * fcntl locking quite broken for servers.
     *
     * flock() on the other hand is inherited across forks. */
    if (flock(lockfd, LOCK_EX) == -1) {
        /* Under normal circumstances this never returns -1, since
         * we block until we succeed.  This *can* fail, however,
         * for example by being interrupted by signals. */
        WARN_ERRNO("Failed to lock the lock file %s", config->lock_file);
        return -1;
    } else {
        SAY("Locked %s", config->lock_file);

        /* Write in our pid to the lock file. */
        char pidbuf[16];
        int wrote = snprintf(pidbuf, sizeof(pidbuf), "%d\n", getpid());
        if (wrote < 0 || wrote >= (int) sizeof(pidbuf)) {
            WARN_ERRNO("Failed to build pid buffer");
        } else {
            if (write(lockfd, pidbuf, wrote) != wrote) {
                WARN_ERRNO("Failed to write pid to %s", config->lock_file);
                return -1;
            } else {
                if (fsync(lockfd) != 0) {
                    WARN_ERRNO("Failed to fsync %s", config->lock_file);
                    return -1;
                }
            }
            /* Do not close() the fd, you'll lose the lock. */
        }
    }

    return lockfd;
}

/* Blocks waiting for the lock file, returns the lockfd once
 * successful.  Creates and removes a "wait file" (in the same directory
 * as the lock file) which exists only during the wait. */
static int highlander(config_t * config)
{
    if (config->lock_file == NULL) {
        FATAL("NULL lock_file");
        return -1;
    }

    char buf[PATH_MAX];
    int wrote;

    wrote = snprintf(buf, sizeof(buf), "locking %s", config->lock_file);
    if (wrote < 0 || wrote >= (int) sizeof(buf)) {
        WARN_ERRNO("Failed to build buf for %s", config->lock_file);
        return -1;
    }
    setproctitle(buf);

    /* We will create an empty "wait file" which records in a crude way
     * (in the filename) the process waiting for the lock.  Usually there
     * shouldn't be more than one of these. */
    /* Note that buf is reused here, and used later. */
    wrote = snprintf(buf, sizeof(buf), "%s.wait.%d", config->lock_file, getpid());
    if (wrote < 0 || wrote >= (int) sizeof(buf)) {
        WARN_ERRNO("Failed to build buf for %s", config->lock_file);
        return -1;
    }

    SAY("Creating wait file %s", buf);
    int waitfd = open(buf, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    if (waitfd == -1) {
        WARN_ERRNO("Failed to open wait file %s", buf);
        return -1;
    }

    int lockfd = highlander_blocking_lock(config);

    /* Remove our "waiting ticket". */
    SAY("Removing wait file %s", buf);
    if (close(waitfd)) {
        WARN_ERRNO("Failed to close wait fd for %s", buf);
    }
    if (unlink(buf)) {
        WARN_ERRNO("Failed to unlink wait file %s", buf);
    }

    return lockfd;
}

static void malloc_config(config_t * config)
{
    memset(&config->malloc, 0, sizeof(struct malloc_config));

    config->malloc.style = SYSTEM_MALLOC;

    void *soh = dlopen(NULL, RTLD_LAZY);
    assert(soh);

    int (*je_mcm) (const size_t *, size_t, void *, size_t *, void *, size_t);
    int (*tc_gnp) (const char *, size_t *);
    void (*tc_hpd) (const char *);

    const char *jestats = "mallctlbymib";
    if ((je_mcm = dlsym(soh, jestats))) {       /* jemalloc */
        int jerr;

        config->malloc.style = JEMALLOC;

        const char *nametomib = "mallctlnametomib";

        int (*nametomibfp) (const char *, size_t *, size_t *) = dlsym(soh, nametomib);
        const char *config_stats = "config.stats";
        size_t config_stats_offset;
        size_t config_stats_count = 2;
        jerr = (*nametomibfp) (config_stats, &config_stats_offset, &config_stats_count);
        if (jerr) {
            FATAL("%s %s: %s", nametomib, config_stats, strerror(jerr));
        }

        unsigned char enabled;
        size_t len = sizeof(enabled);
        jerr = (*je_mcm) (&config_stats_offset, config_stats_count, &enabled, &len, NULL, 0);
        if (jerr) {
            FATAL("%s %s: %s", jestats, config_stats, strerror(jerr));
        }
        if (enabled) {
            config->malloc.mallctlbymib = je_mcm;

            const char *const stats[] = {
                "stats.allocated",
                "stats.active",
                "stats.mapped",
                "stats.chunks.current",
                "stats.chunks.total",
                "stats.chunks.high",
                "stats.huge.allocated",
                "stats.huge.nmalloc",
                "stats.huge.ndalloc",
            };

            config->malloc.stats_mib_count = (int) (sizeof(stats) / sizeof(stats[0]));
            config->malloc.stats_mib = malloc_or_fatal(config->malloc.stats_mib_count * sizeof(struct mib_config));

            for (int i = 0; i < (int) config->malloc.stats_mib_count; i++) {
                config->malloc.stats_mib[i].name = stats[i];

                config->malloc.stats_mib[i].count = 1;
                for (const char *dot = stats[i]; (dot = strchr(dot, '.')); config->malloc.stats_mib[i].count++) {
                    dot++;
                }

                config->malloc.stats_mib[i].mib = malloc_or_fatal(config->malloc.stats_mib[i].count * sizeof(size_t));
                jerr = (*nametomibfp) (stats[i], config->malloc.stats_mib[i].mib, &config->malloc.stats_mib[i].count);
                if (jerr) {
                    FATAL("%s %s: %s", nametomib, stats[i], strerror(jerr));
                }
            }

            SAY("jemalloc stats enabled");
        } else {
            WARN("jemalloc stats DISABLED");
        }
    }

    if ((tc_gnp = dlsym(soh, "MallocExtension_GetNumericProperty"))) {  /* tcmalloc */
        config->malloc.style = TCMALLOC;

        config->malloc.get_numeric_property = tc_gnp;
    }

    if ((tc_hpd = dlsym(soh, "HeapProfilerDump"))) {    /* tcmalloc */
        config->malloc.style = TCMALLOC;

        config->malloc.heap_profiler_dump = tc_hpd;
    }

    SAY("malloc_style: %s", config->malloc.style == SYSTEM_MALLOC ? "system" :
        config->malloc.style == JEMALLOC ? "jemalloc" : config->malloc.style == TCMALLOC ? "tcmalloc" : "unknown");

    config->malloc.pagesize = sysconf(_SC_PAGESIZE);
    SAY("pagesize: %ld", config->malloc.pagesize);

    dlclose(soh);
}

static int serve(config_t * config)
{
    if (config->daemonize) {
        if (daemonize()) {
            printf("%s: daemonized, pid %d\n", OUR_NAME, getpid());
        } else {
            FATAL("Failed to daemonize");
            return 0;
        }

        WARN("Closing standard fds");
        if (!close_std_fds()) {
            FATAL("Failed to close standard fds");      /* We might not see stderr of this... */
            return 0;
        }

        /* Now the standard file descriptors are closed,
         * only the syslog is available. */
    } else {
        printf("%s: running, pid %d\n", OUR_NAME, getpid());
    }

    int lock_fd = highlander(config);
    if (lock_fd == -1) {
        WARN("Failed to become the highlander");
        return 0;
    }

    setproctitle("starting");

    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, sig_handler);

    GLOBAL.listener = calloc_or_fatal(sizeof(relay_socket_t));
    if (GLOBAL.listener == NULL)
        return EXIT_FAILURE;

    pthread_t server_tid = 0;

    worker_pool_init_static(config);
    server_tid = setup_listener(config);
    GLOBAL.graphite_worker = graphite_worker_create(config);
    pthread_create(&GLOBAL.graphite_worker->base.tid, NULL, graphite_worker_thread, GLOBAL.graphite_worker);

    fixed_buffer_t *process_status_buffer = fixed_buffer_create(PROCESS_STATUS_BUF_LEN);

    /* Every ALIVE_PERIOD second show the process status line also with syslog(). */
#define ALIVE_PERIOD 60
    time_t last_alive = 0;

    malloc_config(GLOBAL.config);

    control_set_bits(RELAY_RUNNING);

    SAY("Running");

    setproctitle("running");

    for (;;) {
        uint32_t control = control_get_bits();
        if (control & RELAY_STOPPING) {
            WARN("Stopping");
            break;
        } else if (control & RELAY_RELOADING) {
            WARN("Reloading");
            struct graphite_config *old_graphite_config = graphite_config_clone(&config->graphite);
            if (config_reload(config, config->config_file, time(NULL))) {
                SAY("Reloading the listener and worker pool");
                stop_listener(server_tid);
                server_tid = setup_listener(config);
                worker_pool_reload_static(config);
                SAY("Reloaded the listener and worker pool");
                if (graphite_config_changed(old_graphite_config, &config->graphite)) {
                    SAY("Graphite config changed, reloading the graphite worker");
                    graphite_worker_destroy(GLOBAL.graphite_worker);
                    GLOBAL.graphite_worker = graphite_worker_create(config);
                    pthread_create(&GLOBAL.graphite_worker->base.tid, NULL, graphite_worker_thread,
                                   GLOBAL.graphite_worker);
                    SAY("Reloaded the graphite worker");
                } else {
                    SAY("Graphite config unchanged, not reloading the graphite worker");
                }
            }
            graphite_config_destroy(old_graphite_config);
            control_unset_bits(RELAY_RELOADING);
        }

        update_process_status(process_status_buffer, config, RELAY_ATOMIC_READ(RECEIVED_STATS.received_count),
                              RELAY_ATOMIC_READ(RECEIVED_STATS.tcp_connections));

        sleep(1);

        time_t now = time(NULL);
        if (now - last_alive >= ALIVE_PERIOD) {
            SAY("%s", process_status_buffer->data);
            last_alive = now;
        }
    }

    update_process_status(process_status_buffer, config, RELAY_ATOMIC_READ(RECEIVED_STATS.received_count),
                          RELAY_ATOMIC_READ(RECEIVED_STATS.tcp_connections));

    SAY("%s", process_status_buffer->data);
    fixed_buffer_destroy(process_status_buffer);

    if (control_exit_code()) {
        WARN("Stopping");
    }

    setproctitle("stopping");

    final_shutdown(server_tid);

    SAY("Unlocking %s", config->lock_file);
    if (close(lock_fd) == -1) {
        WARN("Failed to unbecome the highlander");
    }

    if (control_exit_code()) {
        WARN("Failed");
    }

    SAY("Bye");

    return control_exit_code();
}

static void sig_handler(int signum)
{
    switch (signum) {
    case SIGHUP:
        control_set_bits(RELAY_RELOADING);
        break;
    case SIGTERM:
    case SIGINT:
        control_set_bits(RELAY_STOPPING);
        break;
    default:
        WARN("Received unexpected signal %d, ignoring", signum);
    }
}

static void block_all_signals_inside_thread()
{
    // blocking all signals in threads is a good practise
    // we let main thread receive all signals
    sigset_t sigs_to_block;
    sigfillset(&sigs_to_block);
    pthread_sigmask(SIG_BLOCK, &sigs_to_block, NULL);
}

static void stop_listener(pthread_t server_tid)
{
    if (GLOBAL.listener) {
        shutdown(GLOBAL.listener->socket, SHUT_RDWR);
        /* TODO: if the relay is interrupted rudely (^C), final_shutdown()
         * is called, which will call stop_listener(), and this close()
         * triggers the ire of the clang threadsanitizer, since the socket
         * was opened by a worker thread with a recv() in udp_server, but
         * the shutdown happens in the main thread. */
        close(GLOBAL.listener->socket);
    }
    if (server_tid)
        pthread_join(server_tid, NULL);
}

static void final_shutdown(pthread_t server_tid)
{
    /* Stop accepting more traffic. */
    stop_listener(server_tid);
    free(GLOBAL.listener);
    GLOBAL.listener = NULL;

    /* Stop socket workers and their disk writers. */
    worker_pool_destroy_static();
    sleep(1);                   /* TODO: should be O(#workers)+O(pending output) */

    /* Stop graphite output. */
    graphite_worker_destroy(GLOBAL.graphite_worker);
    sleep(1);
}

int main(int argc, char **argv)
{
    control_set_bits(RELAY_STARTING);
    config_init(argc, argv);
    initproctitle(argc, argv);
    int success = serve(GLOBAL.config);
    config_destroy(GLOBAL.config);
    GLOBAL.config = NULL;
    if (!success) {
        /* If the syslog was already closed, this will be going to /dev/null.
         * If the syslog was already closed, also stderr was already closed. */
        WARN("Failed");
    }
    destroy_proctitle();
    closelog();
    return success;
}
