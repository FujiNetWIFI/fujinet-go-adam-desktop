/*
 * Small portability seams between Linux and macOS for the session core.
 * Three Linux-isms live behind these: clock_nanosleep(TIMER_ABSTIME)
 * (absent on macOS), pthread_condattr_setclock (absent on macOS, which
 * instead offers relative condvar waits), and pthread_setname_np's
 * signature (macOS names only the calling thread).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADAMSESSION_COMPAT_H
#define ADAMSESSION_COMPAT_H

#include <errno.h>
#include <pthread.h>
#include <time.h>

static inline void adam_thread_setname(const char *name)
{
#ifdef __APPLE__
    pthread_setname_np(name);
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

/* Sleep (don't spin) until an absolute CLOCK_MONOTONIC time in
 * microseconds. */
static inline void adam_sleep_until_us(long target_us)
{
#ifdef __APPLE__
    /* No absolute-deadline sleep: sleep the remaining delta and re-check,
     * so a signal-shortened nap can't overshoot the target. */
    for (;;) {
        struct timespec now, rel;
        long remain;
        clock_gettime(CLOCK_MONOTONIC, &now);
        remain = target_us -
                 (long)(now.tv_sec * 1000000L + now.tv_nsec / 1000L);
        if (remain <= 0)
            return;
        rel.tv_sec = remain / 1000000L;
        rel.tv_nsec = (remain % 1000000L) * 1000L;
        if (nanosleep(&rel, NULL) == 0)
            return;
    }
#else
    struct timespec ts;
    ts.tv_sec = target_us / 1000000L;
    ts.tv_nsec = (target_us % 1000000L) * 1000L;
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) ==
           EINTR)
        ;
#endif
}

/* A condvar whose timed waits cannot be skewed by wall-clock changes:
 * CLOCK_MONOTONIC-attributed on Linux, relative waits on macOS. */
static inline void adam_cond_init_monotonic(pthread_cond_t *cv)
{
#ifdef __APPLE__
    pthread_cond_init(cv, NULL);
#else
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(cv, &a);
    pthread_condattr_destroy(&a);
#endif
}

/* Wait on cv (initialized by adam_cond_init_monotonic) for at most ms
 * milliseconds. Returns 0 or ETIMEDOUT. */
static inline int adam_cond_timedwait_ms(pthread_cond_t *cv,
                                         pthread_mutex_t *mtx, int ms)
{
#ifdef __APPLE__
    struct timespec rel;
    rel.tv_sec = ms / 1000;
    rel.tv_nsec = (long)(ms % 1000) * 1000000L;
    return pthread_cond_timedwait_relative_np(cv, mtx, &rel);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(cv, mtx, &ts);
#endif
}

#endif /* ADAMSESSION_COMPAT_H */
