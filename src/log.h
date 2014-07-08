#ifndef _LOG_H
#define _LOG_H

#include <syslog.h>
#include <stdint.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <syslog.h>


#define OUR_FACILITY LOG_LOCAL5

#ifdef DEBUGGING
#define _DEBUG_ARGS ,pthread_self(), __func__, __FILE__, __LINE__
#define DEBUG_FMT  " th:%lu %s():%s:%d"
#else
#define _DEBUG_ARGS
#define DEBUG_FMT
#endif

#define TS_LEN 30

struct _ts {
    time_t t;
    struct tm tm;
    char str[TS_LEN];
};

#define _LOG(type, fmt, arg...) STMT_START {                                        \
    struct _ts *_ts;\
                                                                                    \
    _ts= malloc(sizeof(struct _ts));                                                \
    (void)time(&_ts->t);                                                            \
    (void)localtime_r(&_ts->t, &_ts->tm);                                           \
                                                                                    \
    strftime(_ts->str, TS_LEN, "%Y-%m-%d %H:%M:%S", &_ts->tm);                      \
    syslog(                                                                         \
            (OUR_FACILITY | (LOG_ ## type)),                                        \
            "[%4.4s %s" DEBUG_FMT "] " fmt "\n", "" #type, _ts->str _DEBUG_ARGS, ## arg \
    );                                                                              \
    free(_ts);                                                                      \
} STMT_END

#define WARN(fmt, arg...) _LOG(WARNING, fmt, ## arg)
#define SAY(fmt, arg...) _LOG(INFO, fmt, ## arg)

#define DIE_RC(rc, fmt, arg...) STMT_START {                    \
    if (rc == EXIT_FAILURE) {                                   \
        _LOG(CRIT, fmt, ## arg);                                \
    } else {                                                    \
        _LOG(NOTICE, fmt, ## arg);                              \
    }                                                           \
    exit(rc);                                                   \
} STMT_END


#define DIE(fmt, arg...) \
    DIE_RC(EXIT_FAILURE, fmt " { %s }", ##arg, errno ? strerror(errno) : "undefined error");

#define WARN_ERRNO(fmt, arg...) \
    WARN(fmt " { %s }", ## arg, errno ? strerror(errno) : "undefined error");

#define CONF_WARN(opt,fmt,a,b)                                               \
    STMT_START {\
        SAY("found different <" opt ">, restart is required for it to take effect. was: <" fmt ">, new: <" fmt ">",a,b); \
    } STMT_END


#endif