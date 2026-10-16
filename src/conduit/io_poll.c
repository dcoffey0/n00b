/*
 * io_poll.c - poll I/O backend (POSIX fallback)
 *
 * Uses poll() for I/O event notification. Works on all POSIX systems
 * but is less efficient than kqueue or epoll for large numbers of FDs.
 */

#include "conduit/io.h"
#include "conduit/timer.h"
#include "conduit/signal.h"
#include "conduit/user_event.h"
#include "conduit/proc_lifecycle_internal.h"
#include "internal/conduit/io_wait_set.h"
#include "core/stw.h"
#include "core/syscall.h"

#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif
#ifdef __linux__
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#endif
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef __linux__
#ifndef P_PIDFD
#define P_PIDFD 3
#endif
#endif

/*
 * Timer tracking entry for poll backend
 */
typedef struct poll_timer {
    n00b_conduit_timer_t *timer;     // The timer
    uint64_t              next_fire; // Next fire time (monotonic ms)
    struct poll_timer     *next;
} poll_timer_t;

// Forward declarations
typedef struct poll_ctx poll_ctx_t;
static bool poll_add(void *vctx, int fd, n00b_conduit_io_op_t ops,
                     n00b_conduit_io_target_t *target);
static bool poll_remove(void *vctx, int fd);
#ifndef _WIN32
static void poll_process_signals(poll_ctx_t *ctx);
static void poll_signal_remove(void *vctx, n00b_conduit_signal_watch_t *watch);
#endif

#ifndef _WIN32
#ifdef NSIG
#define POLL_SIGNAL_MAX NSIG
#else
#define POLL_SIGNAL_MAX 128
#endif

/*
 * Signal watch entry for poll backend
 * Uses a process self-pipe to convert signals to poll events.
 */
typedef struct poll_signal {
    n00b_conduit_signal_watch_t *watch;
    struct poll_signal          *next;        // Context-local linked list
    sig_atomic_t                 seen_count;
} poll_signal_t;

typedef struct {
    uint32_t         refs;
    bool             old_action_saved;
    struct sigaction old_action;
} poll_signal_state_t;

static volatile sig_atomic_t g_signal_counts[POLL_SIGNAL_MAX] = {0};
static int                   g_signal_pipe[2]                 = {-1, -1};
static poll_signal_state_t   g_signal_states[POLL_SIGNAL_MAX];

/*
 * Signal handler - records the signal and wakes poll waiters.
 */
static void
poll_signal_handler(int signum)
{
    if (signum <= 0 || signum >= POLL_SIGNAL_MAX) {
        return;
    }

    g_signal_counts[signum]++;
    int fd = g_signal_pipe[1];
    if (fd >= 0) {
        unsigned char c = (unsigned char)signum;
        (void)write(fd, &c, 1);
    }
}
#endif

#ifdef __linux__
/*
 * Process watch entry for poll backend (Linux)
 */
typedef struct poll_proc {
    n00b_conduit_proc_watch_t *watch;
    int                        pidfd;
    struct poll_proc          *next;
} poll_proc_t;

/*
 * Vnode watch entry for poll backend (Linux)
 */
typedef struct poll_vnode {
    n00b_conduit_vnode_watch_t *watch;
    int                         wd; // inotify watch descriptor
    struct poll_vnode           *next;
} poll_vnode_t;

/*
 * User event entry for poll backend (Linux)
 */
typedef struct poll_user_event {
    n00b_conduit_user_event_t  *event;
    int                         efd; // eventfd
    struct poll_user_event     *next;
} poll_user_event_t;
#endif

/*
 * poll backend context
 */
struct poll_ctx {
    n00b_conduit_t *conduit;   // Parent conduit instance
    struct pollfd  *fds;       // Array of pollfd structures
    n00b_conduit_io_target_t **targets; // Parallel array of dispatch targets
    int             capacity;  // Current array capacity
    int             count;     // Number of active entries
    poll_timer_t   *timers;    // Linked list of timers
#ifndef _WIN32
    poll_signal_t  *signals;   // Linked list of signal watches
    bool            signal_pipe_registered;
    int             wake_pipe[2];
    bool            wake_pipe_registered;
#endif
#ifdef __linux__
    poll_proc_t       *procs;       // Linked list of process watches
    poll_vnode_t      *vnodes;      // Linked list of vnode watches
    int                inotify_fd;  // Shared inotify fd
    poll_user_event_t *user_events; // Linked list of user events
#endif
};

/*
 * Get current monotonic time in milliseconds
 */
static uint64_t
poll_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

#define POLL_INITIAL_CAPACITY 64

/*
 * Find FD index in poll array, returns -1 if not found. A slot outside the
 * wait set stores its fd complemented, so both forms match.
 */
static int
poll_find_fd(poll_ctx_t *ctx, int fd)
{
    if (fd < 0) {
        return -1;
    }
    for (int i = 0; i < ctx->count; i++) {
        if (ctx->fds[i].fd == fd || ctx->fds[i].fd == ~fd) {
            return i;
        }
    }
    return -1;
}

/*
 * Grow poll arrays if needed
 */
static bool
poll_grow(poll_ctx_t *ctx)
{
    int new_capacity = ctx->capacity * 2;
    if (new_capacity < POLL_INITIAL_CAPACITY) {
        new_capacity = POLL_INITIAL_CAPACITY;
    }

    n00b_allocator_t *sp = (n00b_allocator_t *)&n00b_get_runtime()->system_pool;
    struct pollfd *new_fds = n00b_alloc_array_with_opts(struct pollfd, new_capacity,
                                 &(n00b_alloc_opts_t){.allocator = sp});

    n00b_conduit_io_target_t **new_targets =
        n00b_alloc_array_with_opts(n00b_conduit_io_target_t *, new_capacity,
            &(n00b_alloc_opts_t){.allocator = sp});

    // Copy existing data
    if (ctx->fds && ctx->count > 0) {
        memcpy(new_fds, ctx->fds, ctx->count * sizeof(struct pollfd));
        memcpy(new_targets, ctx->targets,
               ctx->count * sizeof(n00b_conduit_io_target_t *));
    }

    n00b_free(ctx->fds);
    n00b_free(ctx->targets);

    ctx->fds      = new_fds;
    ctx->targets  = new_targets;
    ctx->capacity = new_capacity;
    return true;
}

/*
 * Set a slot's requested ops. A slot whose n00b_conduit_io_wait_events() is
 * empty stores its fd complemented, which poll() skips.
 */
static void
poll_set_slot_ops(poll_ctx_t *ctx, int idx, int fd, n00b_conduit_io_op_t ops)
{
    short events          = n00b_conduit_io_wait_events(ops);
    ctx->fds[idx].fd      = events ? fd : ~fd;
    ctx->fds[idx].events  = events;
    ctx->fds[idx].revents = 0;
}

/*
 * Convert poll events to conduit I/O ops
 */
static n00b_conduit_io_op_t
poll_events_to_ops(short revents)
{
    n00b_conduit_io_op_t ops = 0;
    if (revents & POLLIN)
        ops |= N00B_CONDUIT_IO_READ;
    if (revents & POLLOUT)
        ops |= N00B_CONDUIT_IO_WRITE;
    if (revents & POLLERR)
        ops |= N00B_CONDUIT_IO_ERROR;
    if (revents & POLLHUP)
        ops |= N00B_CONDUIT_IO_HUP;
    if (revents & POLLNVAL)
        ops |= N00B_CONDUIT_IO_ERROR;
    return ops;
}

#ifdef __linux__
/*
 * Check if an FD is internal (pidfd, inotify_fd, or eventfd)
 */
static bool
poll_is_internal_fd(poll_ctx_t *ctx, int fd)
{
    // Check if it's a pidfd. The for-init declaration form trips ncc's
    // GC stack-map analysis (it only tracks roots declared as block
    // items, not in for-init statement contexts), so the GC-tracked
    // pointers are hoisted to block scope.
    poll_proc_t *pp;
    for (pp = ctx->procs; pp; pp = pp->next) {
        if (pp->pidfd == fd) {
            return true;
        }
    }

    // Check if it's the inotify fd
    if (ctx->inotify_fd >= 0 && fd == ctx->inotify_fd) {
        return true;
    }

    // Check if it's an eventfd
    poll_user_event_t *pe;
    for (pe = ctx->user_events; pe; pe = pe->next) {
        if (pe->efd == fd) {
            return true;
        }
    }

    return false;
}
#endif

#ifndef _WIN32
static void
poll_set_nonblocking_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}
#endif

/*
 * Initialize poll backend
 */
static void *
poll_init(n00b_conduit_t *c)
{
    n00b_allocator_t *sp = (n00b_allocator_t *)&n00b_get_runtime()->system_pool;
    poll_ctx_t *ctx = n00b_alloc_with_opts(poll_ctx_t,
                          &(n00b_alloc_opts_t){.allocator = sp});
    if (!ctx) {
        return nullptr;
    }

    ctx->conduit  = c;
    ctx->fds      = nullptr;
    ctx->targets  = nullptr;
    ctx->capacity = 0;
    ctx->count     = 0;
    ctx->timers    = nullptr;
#ifndef _WIN32
    ctx->signals                = nullptr;
    ctx->signal_pipe_registered = false;
    ctx->wake_pipe[0]           = -1;
    ctx->wake_pipe[1]           = -1;
    ctx->wake_pipe_registered   = false;
#endif
#ifdef __linux__
    ctx->procs       = nullptr;
    ctx->vnodes      = nullptr;
    ctx->inotify_fd  = -1;
    ctx->user_events = nullptr;
#endif

#ifndef _WIN32
    if (pipe(ctx->wake_pipe) == 0) {
        poll_set_nonblocking_cloexec(ctx->wake_pipe[0]);
        poll_set_nonblocking_cloexec(ctx->wake_pipe[1]);
        if (poll_add(ctx, ctx->wake_pipe[0], N00B_CONDUIT_IO_READ, nullptr)) {
            ctx->wake_pipe_registered = true;
        }
        else {
            close(ctx->wake_pipe[0]);
            close(ctx->wake_pipe[1]);
            ctx->wake_pipe[0] = -1;
            ctx->wake_pipe[1] = -1;
        }
    }
#endif

    return ctx;
}

/*
 * Cleanup poll backend
 */
static void
poll_cleanup(void *vctx)
{
    poll_ctx_t *ctx = vctx;
    if (!ctx) {
        return;
    }

#ifndef _WIN32
    while (ctx->signals) {
        poll_signal_remove(ctx, ctx->signals->watch);
    }
    if (ctx->signal_pipe_registered && g_signal_pipe[0] >= 0) {
        poll_remove(ctx, g_signal_pipe[0]);
        ctx->signal_pipe_registered = false;
    }
    if (ctx->wake_pipe_registered) {
        poll_remove(ctx, ctx->wake_pipe[0]);
        ctx->wake_pipe_registered = false;
    }
    if (ctx->wake_pipe[0] >= 0) {
        close(ctx->wake_pipe[0]);
        ctx->wake_pipe[0] = -1;
    }
    if (ctx->wake_pipe[1] >= 0) {
        close(ctx->wake_pipe[1]);
        ctx->wake_pipe[1] = -1;
    }
#endif

#ifdef __linux__
    // Close inotify fd
    if (ctx->inotify_fd >= 0) {
        close(ctx->inotify_fd);
        ctx->inotify_fd = -1;
    }

    // Close all pidfds — pointers hoisted out of the for-init to satisfy
    // ncc's GC stack-map analysis (see poll_is_internal_fd).
    poll_proc_t *pp;
    for (pp = ctx->procs; pp; pp = pp->next) {
        if (pp->pidfd >= 0) {
            close(pp->pidfd);
        }
    }

    // Close all eventfds
    poll_user_event_t *pe;
    for (pe = ctx->user_events; pe; pe = pe->next) {
        if (pe->efd >= 0) {
            close(pe->efd);
        }
    }
#endif

    n00b_free(ctx->fds);
    ctx->fds = nullptr;
    n00b_free(ctx->targets);
    ctx->targets = nullptr;
}

/*
 * Add FD to poll set
 */
static bool
poll_add(void *vctx, int fd, n00b_conduit_io_op_t ops,
         n00b_conduit_io_target_t *target)
{
    poll_ctx_t *ctx = vctx;
    if (!ctx) {
        return false;
    }

    // Check if FD already exists
    int idx = poll_find_fd(ctx, fd);
    if (idx >= 0) {
        // Update existing entry
        poll_set_slot_ops(ctx, idx, fd, ops);
        ctx->targets[idx] = target;
        return true;
    }

    // Need to add new entry
    if (ctx->count >= ctx->capacity) {
        if (!poll_grow(ctx)) {
            return false;
        }
    }

    idx               = ctx->count++;
    poll_set_slot_ops(ctx, idx, fd, ops);
    ctx->targets[idx] = target;

    return true;
}

/*
 * Modify FD operations
 */
static bool
poll_modify(void *vctx, int fd, n00b_conduit_io_op_t ops,
            n00b_conduit_io_target_t *target)
{
    (void)target; // poll preserves target via ctx->targets array
    poll_ctx_t *ctx = vctx;
    if (!ctx) {
        return false;
    }

    int idx = poll_find_fd(ctx, fd);
    if (idx < 0) {
        return false;
    }

    poll_set_slot_ops(ctx, idx, fd, ops);
    return true;
}

/*
 * Remove FD from poll set
 */
static bool
poll_remove(void *vctx, int fd)
{
    poll_ctx_t *ctx = vctx;
    if (!ctx) {
        return false;
    }

    int idx = poll_find_fd(ctx, fd);
    if (idx < 0) {
        return true; // Not found, already removed
    }

    // Move last entry to this slot (compaction)
    int last = ctx->count - 1;
    if (idx != last) {
        ctx->fds[idx]     = ctx->fds[last];
        ctx->targets[idx] = ctx->targets[last];
    }
    ctx->count--;

    return true;
}

/*
 * Get backend name
 */
static n00b_string_t *
poll_name(void)
{
    return r"poll";
}

// ============================================================================
// Timer support
// ============================================================================

/*
 * Add a timer
 */
static bool
poll_timer_add(void *vctx, n00b_conduit_timer_t *timer)
{
    poll_ctx_t *ctx = vctx;
    if (!ctx || !timer) {
        return false;
    }

    n00b_allocator_t *sp = (n00b_allocator_t *)&n00b_get_runtime()->system_pool;
    poll_timer_t *pt = n00b_alloc_with_opts(poll_timer_t,
                           &(n00b_alloc_opts_t){.allocator = sp});
    if (!pt) {
        return false;
    }

    pt->timer     = timer;
    pt->next_fire = poll_now_ms() + timer->interval_ms;
    pt->next      = ctx->timers;
    ctx->timers   = pt;

    return true;
}

/*
 * Remove a timer
 */
static void
poll_timer_remove(void *vctx, n00b_conduit_timer_t *timer)
{
    poll_ctx_t *ctx = vctx;
    if (!ctx || !timer) {
        return;
    }

    poll_timer_t **pp = &ctx->timers;
    while (*pp) {
        if ((*pp)->timer == timer) {
            poll_timer_t *pt = *pp;
            *pp              = pt->next;
            timer->cancelled = true;
            return;
        }
        pp = &(*pp)->next;
    }
}

/*
 * Process expired timers and return minimum timeout for poll()
 */
static int
poll_process_timers(poll_ctx_t *ctx, int timeout_ms)
{
    if (!ctx->timers) {
        return timeout_ms;
    }

    uint64_t now         = poll_now_ms();
    int      min_timeout = timeout_ms;

    poll_timer_t **pp = &ctx->timers;
    while (*pp) {
        poll_timer_t         *pt    = *pp;
        n00b_conduit_timer_t *timer = pt->timer;

        if (timer->cancelled) {
            // Remove cancelled timer
            *pp = pt->next;
            continue;
        }

        if (now >= pt->next_fire) {
            // Timer expired - fire it
            n00b_conduit_timer_fire(timer);

            if (timer->repeating && !timer->cancelled) {
                // Schedule next fire
                pt->next_fire = now + timer->interval_ms;
                pp            = &pt->next;
            }
            else {
                // Remove one-shot or cancelled timer
                *pp = pt->next;
                continue;
            }
        }
        else {
            // Calculate time until this timer fires
            int ms_until_fire = (int)(pt->next_fire - now);
            if (min_timeout < 0 || ms_until_fire < min_timeout) {
                min_timeout = ms_until_fire;
            }
            pp = &pt->next;
        }
    }

    return min_timeout;
}

#ifdef __linux__
// Forward declarations for Linux process functions
static void poll_process_procs(poll_ctx_t *ctx);
static void poll_process_vnodes(poll_ctx_t *ctx);
static void poll_process_user_events(poll_ctx_t *ctx);
#endif

/*
 * Wait for events (updated to handle timers)
 *
 * Backend is single-owner, thread-local -- no locking needed.
 */
static int
poll_wait_with_timers(void *vctx, n00b_conduit_io_event_t *events,
                      int max_events, int timeout_ms)
{
    poll_ctx_t *ctx = vctx;
    if (!ctx || !events || max_events <= 0) {
        return -1;
    }

    // Process any expired timers and adjust timeout
    int adjusted_timeout = poll_process_timers(ctx, timeout_ms);

#ifndef _WIN32
    // Process any pending signals
    poll_process_signals(ctx);
#endif

    if (ctx->count == 0 && !ctx->timers) {
        return 0;
    }

    if (ctx->count == 0) {
        if (adjusted_timeout > 0) {
            struct timespec ts = {.tv_sec  = adjusted_timeout / 1000,
                                  .tv_nsec = (adjusted_timeout % 1000) * 1000000};
            // eintr: woken early, this runs only the timers that are due, and
            // the event loop waits again.
            nanosleep(&ts, nullptr);
        }
        poll_process_timers(ctx, 0);
        return 0;
    }

    int n;
#ifdef __linux__
    long raw_n;
#if defined(SYS_poll)
    // eintr: EINTR is handled below as a wait with no events.
    n00b_run_blocking(
        raw_n = _n00b_raw_linux_syscall3(SYS_poll,
                                         (long)(uintptr_t)ctx->fds,
                                         (long)ctx->count,
                                         (long)adjusted_timeout));
#else
    struct timespec poll_timeout;
    struct timespec *poll_timeout_ptr = nullptr;
    if (adjusted_timeout >= 0) {
        poll_timeout = (struct timespec){
            .tv_sec  = adjusted_timeout / 1000,
            .tv_nsec = (adjusted_timeout % 1000) * 1000000,
        };
        poll_timeout_ptr = &poll_timeout;
    }
    // eintr: as above.
    n00b_run_blocking(
        raw_n = _n00b_raw_linux_syscall5(SYS_ppoll,
                                         (long)(uintptr_t)ctx->fds,
                                         (long)ctx->count,
                                         (long)(uintptr_t)poll_timeout_ptr,
                                         0,
                                         0));
#endif
    n = (int)raw_n;
#else
    // eintr: as above.
    n00b_run_blocking(n = poll(ctx->fds, ctx->count, adjusted_timeout));
#endif
    if (n < 0) {
        int err = (n < -1) ? -n : errno;
        if (err == EINTR) {
            poll_process_timers(ctx, 0);
#ifndef _WIN32
            poll_process_signals(ctx);
#endif
            return 0;
        }
        return -err;
    }

    // Process timers and signals after poll returns
    poll_process_timers(ctx, 0);
#ifndef _WIN32
    poll_process_signals(ctx);
    if (ctx->wake_pipe[0] >= 0) {
        unsigned char buf[64];
        while (read(ctx->wake_pipe[0], buf, sizeof(buf)) > 0) {}
    }
#endif

#ifdef __linux__
    // Process Linux-specific events
    poll_process_procs(ctx);
    poll_process_vnodes(ctx);
    poll_process_user_events(ctx);
#endif

    if (n == 0) {
        return 0;
    }

    // Convert results to conduit events (skip internal FDs)
    int num_events = 0;
    int total_seen = 0;
    for (int i = 0; i < ctx->count && num_events < max_events && total_seen < n;
         i++) {
        if (ctx->fds[i].revents == 0) {
            continue;
        }
        total_seen++;

#ifdef __linux__
        // Skip internal FDs
        if (poll_is_internal_fd(ctx, ctx->fds[i].fd)) {
            continue;
        }
#endif

#ifndef _WIN32
        if (ctx->wake_pipe_registered && ctx->fds[i].fd == ctx->wake_pipe[0]) {
            continue;
        }
        if (ctx->signal_pipe_registered && ctx->fds[i].fd == g_signal_pipe[0]) {
            continue;
        }
#endif

        n00b_conduit_io_event_t *ev = &events[num_events];
        ev->fd                      = ctx->fds[i].fd;
        ev->ops                     = poll_events_to_ops(ctx->fds[i].revents);
        ev->topic                   = nullptr;
        ev->target                  = ctx->targets[i];
        num_events++;
    }

    return num_events;
}

#ifndef _WIN32
static void
poll_wake(void *vctx)
{
    poll_ctx_t *ctx = vctx;
    if (!ctx || ctx->wake_pipe[1] < 0) {
        return;
    }

    unsigned char c = 0;
    (void)write(ctx->wake_pipe[1], &c, 1);
}
#endif

#ifndef _WIN32
// ============================================================================
// Signal support (Unix only)
// ============================================================================

/*
 * Process pending signals
 */
static void
poll_process_signals(poll_ctx_t *ctx)
{
    if (g_signal_pipe[0] >= 0) {
        unsigned char buf[64];
        while (read(g_signal_pipe[0], buf, sizeof(buf)) > 0) {}
    }

    poll_signal_t *ps;
    for (ps = ctx->signals; ps; ps = ps->next) {
        if (!ps->watch)
            continue;

        int signum = ps->watch->signum;
        if (signum > 0 && signum < POLL_SIGNAL_MAX
            && g_signal_counts[signum] != ps->seen_count) {
            ps->seen_count = g_signal_counts[signum];
            n00b_conduit_signal_fire(ps->watch);
        }
    }
}

static bool
poll_ensure_signal_pipe(void)
{
    if (g_signal_pipe[0] >= 0 && g_signal_pipe[1] >= 0) {
        return true;
    }

    if (pipe(g_signal_pipe) < 0) {
        g_signal_pipe[0] = -1;
        g_signal_pipe[1] = -1;
        return false;
    }

    poll_set_nonblocking_cloexec(g_signal_pipe[0]);
    poll_set_nonblocking_cloexec(g_signal_pipe[1]);
    return true;
}

static bool
poll_signal_acquire_action(int signum)
{
    if (signum <= 0 || signum >= POLL_SIGNAL_MAX) {
        return false;
    }

    poll_signal_state_t *state = &g_signal_states[signum];
    if (state->refs == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = poll_signal_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        if (sigaction(signum, &sa, &state->old_action) != 0) {
            return false;
        }
        state->old_action_saved = true;
    }
    state->refs++;
    return true;
}

static void
poll_signal_release_action(int signum)
{
    if (signum <= 0 || signum >= POLL_SIGNAL_MAX) {
        return;
    }

    poll_signal_state_t *state = &g_signal_states[signum];
    if (state->refs == 0) {
        return;
    }
    state->refs--;
    if (state->refs == 0 && state->old_action_saved) {
        sigaction(signum, &state->old_action, nullptr);
        state->old_action_saved = false;
    }
}

/*
 * Add a signal watch
 */
static bool
poll_signal_add(void *vctx, n00b_conduit_signal_watch_t *watch)
{
    poll_ctx_t *ctx = vctx;
    if (!ctx || !watch) {
        return false;
    }

    n00b_allocator_t *sp = (n00b_allocator_t *)&n00b_get_runtime()->system_pool;
    poll_signal_t *ps = n00b_alloc_with_opts(poll_signal_t,
                            &(n00b_alloc_opts_t){.allocator = sp});
    if (!ps) {
        return false;
    }

    if (!poll_ensure_signal_pipe()) {
        return false;
    }

    if (!poll_signal_acquire_action(watch->signum)) {
        return false;
    }

    if (!ctx->signal_pipe_registered) {
        if (!poll_add(ctx,
                      g_signal_pipe[0],
                      N00B_CONDUIT_IO_READ,
                      nullptr)) {
            poll_signal_release_action(watch->signum);
            return false;
        }
        ctx->signal_pipe_registered = true;
    }

    ps->watch      = watch;
    ps->seen_count = (watch->signum > 0 && watch->signum < POLL_SIGNAL_MAX)
                       ? g_signal_counts[watch->signum]
                       : 0;
    ps->next     = ctx->signals;
    ctx->signals = ps;

    return true;
}

/*
 * Remove a signal watch
 */
static void
poll_signal_remove(void *vctx, n00b_conduit_signal_watch_t *watch)
{
    poll_ctx_t *ctx = vctx;
    if (!ctx || !watch) {
        return;
    }

    // Find and remove from context list
    poll_signal_t **pp = &ctx->signals;
    while (*pp) {
        if ((*pp)->watch == watch) {
            poll_signal_t *ps = *pp;
            *pp               = ps->next;

            poll_signal_release_action(watch->signum);

            if (!ctx->signals && ctx->signal_pipe_registered) {
                poll_remove(ctx, g_signal_pipe[0]);
                ctx->signal_pipe_registered = false;
            }

            return;
        }
        pp = &(*pp)->next;
    }
}
#endif // !_WIN32

#ifdef __linux__
// ============================================================================
// Process support (Linux)
// ============================================================================

/*
 * Add a process watch (Linux pidfd)
 */
static bool
poll_proc_add(void *vctx, n00b_conduit_proc_watch_t *watch)
{
    poll_ctx_t *ctx = vctx;
    if (!ctx || !watch) {
        return false;
    }

    // Open pidfd using syscall (glibc wrapper may not exist)
    int pidfd = syscall(SYS_pidfd_open, watch->pid, 0);
    if (pidfd < 0) {
        return false;
    }

    n00b_allocator_t *sp = (n00b_allocator_t *)&n00b_get_runtime()->system_pool;
    poll_proc_t *pp = n00b_alloc_with_opts(poll_proc_t,
                          &(n00b_alloc_opts_t){.allocator = sp});
    if (!pp) {
        close(pidfd);
        return false;
    }

    pp->watch  = watch;
    pp->pidfd  = pidfd;
    pp->next   = ctx->procs;
    ctx->procs = pp;

    // Add pidfd to poll set (no IO target — procs dispatch via poll_process_procs)
    if (!poll_add(ctx, pidfd, N00B_CONDUIT_IO_READ, nullptr)) {
        close(pidfd);
        return false;
    }

    return true;
}

/*
 * Remove a process watch
 */
static void
poll_proc_remove(void *vctx, n00b_conduit_proc_watch_t *watch)
{
    poll_ctx_t *ctx = vctx;
    if (!ctx || !watch) {
        return;
    }

    poll_proc_t **pp = &ctx->procs;
    while (*pp) {
        if ((*pp)->watch == watch) {
            poll_proc_t *proc = *pp;
            *pp               = proc->next;

            // Remove from poll set and close pidfd
            poll_remove(ctx, proc->pidfd);
            close(proc->pidfd);

            return;
        }
        pp = &(*pp)->next;
    }
}

/*
 * Process pending process events
 */
static void
poll_process_procs(poll_ctx_t *ctx)
{
    poll_proc_t *pp;
    for (pp = ctx->procs; pp; pp = pp->next) {
        int idx = poll_find_fd(ctx, pp->pidfd);
        if (idx < 0 || !(ctx->fds[idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            continue;
        }

        // pidfd is readable => process exited.
        // Try waitid for exit status (best-effort -- may fail if already reaped).
        siginfo_t info;
        memset(&info, 0, sizeof(info));
        int exit_status = 0;

        int ret = syscall(SYS_waitid, P_PIDFD, pp->pidfd, &info,
                          WEXITED | WNOHANG, nullptr);
        if (ret == 0 && info.si_pid != 0) {
            exit_status = n00b_conduit_proc_wait_status_from_siginfo(&info);
        }

        n00b_conduit_proc_fire(pp->watch, N00B_CONDUIT_PROC_EXIT, exit_status);
    }
}

// ============================================================================
// Vnode support (Linux inotify)
// ============================================================================

/*
 * Add a vnode watch (Linux inotify)
 */
static bool
poll_vnode_add(void *vctx, n00b_conduit_vnode_watch_t *watch)
{
    poll_ctx_t *ctx = vctx;
    if (!ctx || !watch) {
        return false;
    }

    // Lazy initialize inotify fd
    if (ctx->inotify_fd < 0) {
        ctx->inotify_fd = inotify_init1(IN_NONBLOCK);
        if (ctx->inotify_fd < 0) {
            return false;
        }
    }

    // Resolve fd to path via /proc/self/fd
    char proc_path[64];
    char real_path[1024];
    snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", watch->fd);
    ssize_t len = readlink(proc_path, real_path, sizeof(real_path) - 1);
    if (len < 0) {
        return false;
    }
    real_path[len] = '\0';

    // Map conduit vnode ops to inotify mask
    uint32_t mask = 0;
    if (watch->ops & N00B_CONDUIT_VNODE_DELETE)
        mask |= IN_DELETE_SELF;
    if (watch->ops & N00B_CONDUIT_VNODE_WRITE)
        mask |= IN_MODIFY;
    if (watch->ops & N00B_CONDUIT_VNODE_EXTEND)
        mask |= IN_MODIFY;
    if (watch->ops & N00B_CONDUIT_VNODE_ATTRIB)
        mask |= IN_ATTRIB;
    if (watch->ops & N00B_CONDUIT_VNODE_LINK)
        mask |= IN_ATTRIB;
    if (watch->ops & N00B_CONDUIT_VNODE_RENAME)
        mask |= IN_MOVE_SELF;
    if (watch->ops & N00B_CONDUIT_VNODE_REVOKE)
        mask |= IN_DELETE_SELF;

    // Add inotify watch
    int wd = inotify_add_watch(ctx->inotify_fd, real_path, mask);
    if (wd < 0) {
        return false;
    }

    n00b_allocator_t *sp = (n00b_allocator_t *)&n00b_get_runtime()->system_pool;
    poll_vnode_t *pv = n00b_alloc_with_opts(poll_vnode_t,
                           &(n00b_alloc_opts_t){.allocator = sp});
    if (!pv) {
        inotify_rm_watch(ctx->inotify_fd, wd);
        return false;
    }

    pv->watch   = watch;
    pv->wd      = wd;
    pv->next    = ctx->vnodes;
    ctx->vnodes = pv;

    // Add inotify fd to poll set if first watch
    if (pv->next == nullptr) {
        if (!poll_add(ctx, ctx->inotify_fd, N00B_CONDUIT_IO_READ, nullptr)) {
            inotify_rm_watch(ctx->inotify_fd, wd);
            return false;
        }
    }

    return true;
}

/*
 * Remove a vnode watch
 */
static void
poll_vnode_remove(void *vctx, n00b_conduit_vnode_watch_t *watch)
{
    poll_ctx_t *ctx = vctx;
    if (!ctx || !watch) {
        return;
    }

    poll_vnode_t **pp = &ctx->vnodes;
    while (*pp) {
        if ((*pp)->watch == watch) {
            poll_vnode_t *vnode = *pp;
            *pp                = vnode->next;

            // Remove inotify watch
            inotify_rm_watch(ctx->inotify_fd, vnode->wd);

            // If no more vnodes, remove inotify fd from poll set
            if (ctx->vnodes == nullptr) {
                poll_remove(ctx, ctx->inotify_fd);
            }

            return;
        }
        pp = &(*pp)->next;
    }
}

/*
 * Process pending vnode events
 */
static void
poll_process_vnodes(poll_ctx_t *ctx)
{
    if (ctx->inotify_fd < 0) {
        return;
    }

    int idx = poll_find_fd(ctx, ctx->inotify_fd);
    if (idx < 0 || !(ctx->fds[idx].revents & POLLIN)) {
        return;
    }

    // Read inotify events
    char    buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t len = read(ctx->inotify_fd, buf, sizeof(buf));
    if (len <= 0) {
        return;
    }

    // Process each event. The pointer is hoisted out of the for-init
    // because ncc's stack-map analysis flags every pointer-typed root
    // declared in a statement context, even ones (like this) that
    // point into a stack buffer rather than the GC heap.
    char *ptr;
    for (ptr = buf; ptr < buf + len;) {
        struct inotify_event *event = (struct inotify_event *)ptr;

        // Find matching vnode watch
        poll_vnode_t *pv;
        for (pv = ctx->vnodes; pv; pv = pv->next) {
            if (pv->wd == event->wd) {
                // Map inotify mask back to conduit ops
                uint32_t ops = 0;
                if (event->mask & IN_MODIFY)
                    ops |= N00B_CONDUIT_VNODE_WRITE;
                if (event->mask & IN_ATTRIB)
                    ops |= N00B_CONDUIT_VNODE_ATTRIB;
                if (event->mask & IN_DELETE_SELF)
                    ops |= N00B_CONDUIT_VNODE_DELETE;
                if (event->mask & IN_MOVE_SELF)
                    ops |= N00B_CONDUIT_VNODE_RENAME;

                if (ops) {
                    n00b_conduit_vnode_fire(pv->watch, ops);
                }
                break;
            }
        }

        ptr += sizeof(struct inotify_event) + event->len;
    }
}

// ============================================================================
// User event support (Linux eventfd)
// ============================================================================

/*
 * Add a user event (Linux eventfd)
 */
static bool
poll_user_event_add(void *vctx, n00b_conduit_user_event_t *event)
{
    poll_ctx_t *ctx = vctx;
    if (!ctx || !event) {
        return false;
    }

    // Create eventfd
    int efd = eventfd(0, EFD_NONBLOCK);
    if (efd < 0) {
        return false;
    }

    n00b_allocator_t *sp = (n00b_allocator_t *)&n00b_get_runtime()->system_pool;
    poll_user_event_t *pe = n00b_alloc_with_opts(poll_user_event_t,
                                &(n00b_alloc_opts_t){.allocator = sp});
    if (!pe) {
        close(efd);
        return false;
    }

    pe->event        = event;
    pe->efd          = efd;
    pe->next         = ctx->user_events;
    ctx->user_events = pe;

    // Add eventfd to poll set (no IO target — user events dispatch via poll_process_user_events)
    if (!poll_add(ctx, efd, N00B_CONDUIT_IO_READ, nullptr)) {
        close(efd);
        return false;
    }

    return true;
}

/*
 * Remove a user event
 */
static void
poll_user_event_remove(void *vctx, n00b_conduit_user_event_t *event)
{
    poll_ctx_t *ctx = vctx;
    if (!ctx || !event) {
        return;
    }

    poll_user_event_t **pp = &ctx->user_events;
    while (*pp) {
        if ((*pp)->event == event) {
            poll_user_event_t *ue = *pp;
            *pp                   = ue->next;

            // Remove from poll set and close eventfd
            poll_remove(ctx, ue->efd);
            close(ue->efd);

            return;
        }
        pp = &(*pp)->next;
    }
}

/*
 * Trigger a user event
 */
static void
poll_user_event_trigger(void *vctx, n00b_conduit_user_event_t *event)
{
    poll_ctx_t *ctx = vctx;
    if (!ctx || !event) {
        return;
    }

    // Find the user event by event_id (may be a temporary struct)
    poll_user_event_t *pe;
    for (pe = ctx->user_events; pe; pe = pe->next) {
        if (pe->event->event_id == event->event_id) {
            uint64_t val = 1;
            (void)write(pe->efd, &val, sizeof(val));
            return;
        }
    }
}

/*
 * Process pending user events
 */
static void
poll_process_user_events(poll_ctx_t *ctx)
{
    poll_user_event_t *pe;
    for (pe = ctx->user_events; pe; pe = pe->next) {
        int idx = poll_find_fd(ctx, pe->efd);
        if (idx < 0 || !(ctx->fds[idx].revents & POLLIN)) {
            continue;
        }

        // Read and acknowledge the event
        uint64_t val;
        if (read(pe->efd, &val, sizeof(val)) == sizeof(val)) {
            n00b_conduit_user_event_fire(pe->event);
        }
    }
}
#endif // __linux__

/*
 * poll backend operations
 */
static const n00b_conduit_io_ops_t poll_ops = {
    .init         = poll_init,
    .cleanup      = poll_cleanup,
    .add          = poll_add,
    .modify       = poll_modify,
    .remove       = poll_remove,
    .wait         = poll_wait_with_timers,
#ifndef _WIN32
    .wake         = poll_wake,
#else
    .wake         = nullptr,
#endif
    .name         = poll_name,
    .timer_add    = poll_timer_add,
    .timer_remove = poll_timer_remove,
#ifndef _WIN32
    .signal_add    = poll_signal_add,
    .signal_remove = poll_signal_remove,
#else
    .signal_add    = nullptr,
    .signal_remove = nullptr,
#endif
#ifdef __linux__
    .proc_add           = poll_proc_add,
    .proc_remove        = poll_proc_remove,
    .vnode_add          = poll_vnode_add,
    .vnode_remove       = poll_vnode_remove,
    .user_event_add     = poll_user_event_add,
    .user_event_remove  = poll_user_event_remove,
    .user_event_trigger = poll_user_event_trigger,
#else
    .proc_add           = nullptr,
    .proc_remove        = nullptr,
    .vnode_add          = nullptr,
    .vnode_remove       = nullptr,
    .user_event_add     = nullptr,
    .user_event_remove  = nullptr,
    .user_event_trigger = nullptr,
#endif
};

n00b_result_t(const n00b_conduit_io_ops_t *)
n00b_conduit_io_poll_ops(void)
{
    return n00b_result_ok(const n00b_conduit_io_ops_t *, &poll_ops);
}
