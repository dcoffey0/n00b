/**
 * @file eintr.h
 * @brief Retrying system calls that a signal interrupts.
 *
 * Every n00b signal handler sets SA_RESTART, so the kernel restarts most
 * interrupted calls. Some it never restarts, and those fail with EINTR
 * whenever a signal lands: the poll family and sleeps everywhere, a blocking
 * socket call with SO_RCVTIMEO or SO_SNDTIMEO set on Linux, and a blocking
 * TCP connect on macOS. On Linux, n00b's stop-the-world signal reaches every
 * running thread, so these calls see EINTR in normal operation.
 * scripts/lint_eintr.py lists the calls and requires each to go through
 * these helpers or say how it handles EINTR.
 */
#pragma once

#include <errno.h>
#include <stdint.h>

#include "n00b.h"
#include "core/time.h"

/**
 * @brief Evaluate @p call until it does not fail with EINTR or
 * @p deadline_ns passes.
 *
 * For calls that fail with -1 and errno. @p deadline_ns is an
 * n00b_ns_timestamp value. Once it has passed, an EINTR ends the retries
 * with -1 and errno ETIMEDOUT.
 */
#define n00b_retry_eintr_until(deadline_ns, call)                             \
    ({                                                                        \
        uint64_t     _ei_deadline = (deadline_ns);                            \
        typeof(call) _ei_r;                                                   \
        for (;;) {                                                            \
            _ei_r = (call);                                                   \
            if (_ei_r >= 0 || errno != EINTR) {                               \
                break;                                                        \
            }                                                                 \
            if ((uint64_t)n00b_ns_timestamp() >= _ei_deadline) {              \
                errno = ETIMEDOUT;                                            \
                break;                                                        \
            }                                                                 \
        }                                                                     \
        _ei_r;                                                                \
    })

/**
 * @brief The same for a _n00b_raw_linux_syscallN() call, which returns
 * -errno because off-libc threads cannot touch errno.
 *
 * -EINTR is retried, and an EINTR after the deadline yields -ETIMEDOUT.
 */
#define n00b_retry_eintr_raw_until(deadline_ns, call)                         \
    ({                                                                        \
        uint64_t     _ei_deadline = (deadline_ns);                            \
        typeof(call) _ei_r;                                                   \
        for (;;) {                                                            \
            _ei_r = (call);                                                   \
            if (_ei_r != -EINTR) {                                            \
                break;                                                        \
            }                                                                 \
            if ((uint64_t)n00b_ns_timestamp() >= _ei_deadline) {              \
                _ei_r = -ETIMEDOUT;                                           \
                break;                                                        \
            }                                                                 \
        }                                                                     \
        _ei_r;                                                                \
    })

#ifndef _WIN32
#include <poll.h>

/**
 * @brief poll() that resumes after a signal for the time left.
 *
 * Returns poll's result, 0 once @p timeout_ms has passed. A negative
 * timeout waits until an event or an error other than EINTR.
 */
static inline int
n00b_poll_eintr(struct pollfd *fds, nfds_t nfds, int timeout_ms)
{
    uint64_t deadline = timeout_ms < 0
                          ? 0
                          : n00b_ns_timestamp()
                                + (uint64_t)timeout_ms * N00B_NS_PER_MS;
    for (;;) {
        int wait_ms = timeout_ms;
        if (timeout_ms >= 0) {
            uint64_t now = n00b_ns_timestamp();
            wait_ms      = now >= deadline
                             ? 0
                             : (int)((deadline - now + N00B_NS_PER_MS - 1)
                                     / N00B_NS_PER_MS);
        }
        // eintr: this loop resumes the wait for the time left.
        int r = poll(fds, nfds, wait_ms);
        if (r >= 0 || errno != EINTR) {
            return r;
        }
    }
}
#endif
