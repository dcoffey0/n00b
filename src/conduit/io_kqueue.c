/*
 * io_kqueue.c - kqueue I/O backend for macOS/BSD
 *
 * Uses kqueue for efficient I/O event notification.
 */

#include "conduit/io.h"
#include "conduit/socket.h"
#include "conduit/timer.h"
#include "conduit/signal.h"
#include "conduit/user_event.h"
#include "core/stw.h"

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)

#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>

#ifdef NSIG
#define KQUEUE_SIGNAL_MAX NSIG
#else
#define KQUEUE_SIGNAL_MAX 128
#endif
#define KQUEUE_WAKE_IDENT ((uintptr_t)-1)

// ============================================================================
// kqueue fflags conversion
// ============================================================================

static uint32_t
proc_ops_to_kqueue_fflags(uint32_t ops)
{
    uint32_t fflags = 0;
    // NOTE_EXITSTATUS puts the wait(2) status in kev->data, which is the only
    // way to learn it before the child is reapable (n00b-lang/n00b#373). Only
    // valid for a child (or a pid we may signal); kqueue_proc_add falls back
    // to a bare NOTE_EXIT when EV_ADD rejects it.
    if (ops & N00B_CONDUIT_PROC_EXIT)   fflags |= NOTE_EXIT | NOTE_EXITSTATUS;
    if (ops & N00B_CONDUIT_PROC_FORK)   fflags |= NOTE_FORK;
    if (ops & N00B_CONDUIT_PROC_EXEC)   fflags |= NOTE_EXEC;
    if (ops & N00B_CONDUIT_PROC_SIGNAL) fflags |= NOTE_SIGNAL;
    return fflags;
}

static uint32_t
kqueue_fflags_to_proc_ops(uint32_t fflags)
{
    uint32_t ops = 0;
    if (fflags & NOTE_EXIT)   ops |= N00B_CONDUIT_PROC_EXIT;
    if (fflags & NOTE_FORK)   ops |= N00B_CONDUIT_PROC_FORK;
    if (fflags & NOTE_EXEC)   ops |= N00B_CONDUIT_PROC_EXEC;
    if (fflags & NOTE_SIGNAL) ops |= N00B_CONDUIT_PROC_SIGNAL;
    return ops;
}

static uint32_t
vnode_ops_to_kqueue_fflags(uint32_t ops)
{
    uint32_t fflags = 0;
    if (ops & N00B_CONDUIT_VNODE_DELETE)  fflags |= NOTE_DELETE;
    if (ops & N00B_CONDUIT_VNODE_WRITE)   fflags |= NOTE_WRITE;
    if (ops & N00B_CONDUIT_VNODE_EXTEND)  fflags |= NOTE_EXTEND;
    if (ops & N00B_CONDUIT_VNODE_ATTRIB)  fflags |= NOTE_ATTRIB;
    if (ops & N00B_CONDUIT_VNODE_LINK)    fflags |= NOTE_LINK;
    if (ops & N00B_CONDUIT_VNODE_RENAME)  fflags |= NOTE_RENAME;
    if (ops & N00B_CONDUIT_VNODE_REVOKE)  fflags |= NOTE_REVOKE;
#ifdef NOTE_FUNLOCK
    if (ops & N00B_CONDUIT_VNODE_FUNLOCK) fflags |= NOTE_FUNLOCK;
#endif
    return fflags;
}

static uint32_t
kqueue_fflags_to_vnode_ops(uint32_t fflags)
{
    uint32_t ops = 0;
    if (fflags & NOTE_DELETE)  ops |= N00B_CONDUIT_VNODE_DELETE;
    if (fflags & NOTE_WRITE)   ops |= N00B_CONDUIT_VNODE_WRITE;
    if (fflags & NOTE_EXTEND)  ops |= N00B_CONDUIT_VNODE_EXTEND;
    if (fflags & NOTE_ATTRIB)  ops |= N00B_CONDUIT_VNODE_ATTRIB;
    if (fflags & NOTE_LINK)    ops |= N00B_CONDUIT_VNODE_LINK;
    if (fflags & NOTE_RENAME)  ops |= N00B_CONDUIT_VNODE_RENAME;
    if (fflags & NOTE_REVOKE)  ops |= N00B_CONDUIT_VNODE_REVOKE;
#ifdef NOTE_FUNLOCK
    if (fflags & NOTE_FUNLOCK) ops |= N00B_CONDUIT_VNODE_FUNLOCK;
#endif
    return ops;
}

static uint16_t
fd_event_flags(n00b_conduit_io_target_t *target)
{
    /*
     * Accept listeners must be level-triggered.  If a readiness edge is stale
     * or missed, EV_CLEAR can leave queued clients invisible forever.
     */
    if (target && _n00b_variant_is_type_ptr(target, n00b_conduit_listener_t *)) {
        return EV_ADD;
    }

    return EV_ADD | EV_CLEAR;
}

/*
 * kqueue backend context
 */
typedef struct {
    int                             kq;           // kqueue file descriptor
    n00b_conduit_t                 *conduit;      // Parent conduit instance
    n00b_conduit_timer_t           *timers;       // Linked list of active timers
    n00b_conduit_signal_watch_t    *signals;      // Linked list of signal watches
    n00b_conduit_proc_watch_t      *procs;        // Linked list of proc watches
    n00b_conduit_vnode_watch_t     *vnodes;       // Linked list of vnode watches
    n00b_conduit_user_event_t      *user_events;  // Linked list of user events
} kqueue_ctx_t;

typedef struct {
    uint32_t         refs;
    bool             old_action_saved;
    struct sigaction old_action;
} kqueue_signal_state_t;

static kqueue_signal_state_t g_kqueue_signal_states[KQUEUE_SIGNAL_MAX];

static void kqueue_signal_remove(void *vctx,
                                 n00b_conduit_signal_watch_t *watch);

/*
 * Initialize kqueue backend
 */
static void *
kqueue_init(n00b_conduit_t *c)
{
    n00b_allocator_t *sp = (n00b_allocator_t *)&n00b_get_runtime()->system_pool;
    kqueue_ctx_t *ctx = n00b_alloc_with_opts(kqueue_ctx_t,
                            &(n00b_alloc_opts_t){.allocator = sp});
    if (!ctx) {
        return nullptr;
    }

    ctx->kq = kqueue();
    if (ctx->kq < 0) {
        return nullptr;
    }

    ctx->conduit     = c;
    ctx->timers      = nullptr;
    ctx->signals     = nullptr;
    ctx->procs       = nullptr;
    ctx->vnodes      = nullptr;
    ctx->user_events = nullptr;

    struct kevent wake_kev;
    EV_SET(&wake_kev, KQUEUE_WAKE_IDENT, EVFILT_USER,
           EV_ADD | EV_CLEAR, 0, 0, nullptr);
    (void)kevent(ctx->kq, &wake_kev, 1, nullptr, 0, nullptr);
    return ctx;
}

/*
 * Cleanup kqueue backend
 */
static void
kqueue_cleanup(void *vctx)
{
    kqueue_ctx_t *ctx = vctx;
    if (!ctx) {
        return;
    }

    while (ctx->signals) {
        kqueue_signal_remove(ctx, ctx->signals);
    }

    if (ctx->kq >= 0) {
        close(ctx->kq);
        ctx->kq = -1;
    }
}

/*
 * Add FD to kqueue
 */
static bool
kqueue_add(void *vctx, int fd, n00b_conduit_io_op_t ops,
           n00b_conduit_io_target_t *target)
{
    kqueue_ctx_t *ctx = vctx;
    if (!ctx || ctx->kq < 0) {
        return false;
    }

    struct kevent changes[2];
    int nchanges = 0;
    uint16_t flags = fd_event_flags(target);

    if (ops & N00B_CONDUIT_IO_READ) {
        EV_SET(&changes[nchanges], fd, EVFILT_READ, flags, 0, 0, target);
        nchanges++;
    }

    if (ops & N00B_CONDUIT_IO_WRITE) {
        EV_SET(&changes[nchanges], fd, EVFILT_WRITE, flags, 0, 0, target);
        nchanges++;
    }

    if (nchanges == 0) {
        return true;  // Nothing to add
    }

    int ret = kevent(ctx->kq, changes, nchanges, nullptr, 0, nullptr);
    return ret >= 0;
}

/*
 * Modify FD operations in kqueue
 */
static bool
kqueue_modify(void *vctx, int fd, n00b_conduit_io_op_t ops,
              n00b_conduit_io_target_t *target)
{
    kqueue_ctx_t *ctx = vctx;
    if (!ctx || ctx->kq < 0) {
        return false;
    }

    // Each filter is submitted in its own kevent() call. Batching an
    // EV_DELETE for a filter that isn't currently registered (e.g. deleting
    // READ while only enabling WRITE during a connect) makes kevent() return
    // ENOENT and ABANDON the rest of the changelist when the eventlist is
    // zero-length — which would silently drop the EV_ADD that follows it.
    // Issuing the changes separately keeps a benign delete-ENOENT from
    // suppressing a real add (the bug that left async connect WRITE-readiness
    // unregistered, since EV_DELETE READ preceded EV_ADD WRITE).
    struct kevent change;
    uint16_t add_flags = fd_event_flags(target);

    if (ops & N00B_CONDUIT_IO_READ) {
        EV_SET(&change, fd, EVFILT_READ, add_flags, 0, 0, target);
    } else {
        EV_SET(&change, fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    }
    kevent(ctx->kq, &change, 1, nullptr, 0, nullptr);

    if (ops & N00B_CONDUIT_IO_WRITE) {
        EV_SET(&change, fd, EVFILT_WRITE, add_flags, 0, 0, target);
    } else {
        EV_SET(&change, fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    }
    kevent(ctx->kq, &change, 1, nullptr, 0, nullptr);

    return true;
}

/*
 * Remove FD from kqueue
 */
static bool
kqueue_remove(void *vctx, int fd)
{
    kqueue_ctx_t *ctx = vctx;
    if (!ctx || ctx->kq < 0) {
        return false;
    }

    struct kevent changes[2];
    EV_SET(&changes[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    EV_SET(&changes[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);

    // Ignore errors (filters may not exist)
    kevent(ctx->kq, changes, 2, nullptr, 0, nullptr);
    return true;
}

/*
 * Wait for events
 */
static int
kqueue_wait(void *vctx, n00b_conduit_io_event_t *events, int max_events,
            int timeout_ms)
{
    kqueue_ctx_t *ctx = vctx;
    if (!ctx || ctx->kq < 0 || !events || max_events <= 0) {
        return -1;
    }

    struct timespec ts;
    struct timespec *timeout_ptr = nullptr;

    if (timeout_ms >= 0) {
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000;
        timeout_ptr = &ts;
    }

    // Use stack allocation for kevent results
    struct kevent kevents[64];
    int kmax = max_events < 64 ? max_events : 64;

    int n;
    // eintr: EINTR returns no events, and the event loop waits again.
    n00b_run_blocking(n = kevent(ctx->kq, nullptr, 0, kevents, kmax, timeout_ptr));
    if (n < 0) {
        if (errno == EINTR) {
            return 0;  // Interrupted, treat as timeout
        }
        return -1;
    }

    // Convert kevent results to conduit events
    int event_count = 0;
    for (int i = 0; i < n; i++) {
        struct kevent *kev = &kevents[i];

        // Handle timer events separately
        if (kev->filter == EVFILT_TIMER) {
            n00b_conduit_timer_t *timer = (n00b_conduit_timer_t *)kev->udata;
            if (timer && !timer->cancelled) {
                n00b_conduit_timer_fire(timer);
            }
            continue;
        }

        // Handle signal events separately
        if (kev->filter == EVFILT_SIGNAL) {
            n00b_conduit_signal_watch_t *watch =
                (n00b_conduit_signal_watch_t *)kev->udata;
            if (watch) {
                n00b_conduit_signal_fire(watch);
            }
            continue;
        }

        // Handle process events
        if (kev->filter == EVFILT_PROC) {
            n00b_conduit_proc_watch_t *w =
                (n00b_conduit_proc_watch_t *)kev->udata;
            if (w) {
                uint32_t proc_ops = kqueue_fflags_to_proc_ops(kev->fflags);
                int exit_status = (proc_ops & N00B_CONDUIT_PROC_EXIT)
                                      ? (int)kev->data
                                      : 0;
                n00b_conduit_proc_fire(w, proc_ops, exit_status);
            }
            continue;
        }

        // Handle vnode events
        if (kev->filter == EVFILT_VNODE) {
            n00b_conduit_vnode_watch_t *w =
                (n00b_conduit_vnode_watch_t *)kev->udata;
            if (w) {
                uint32_t vnode_ops = kqueue_fflags_to_vnode_ops(kev->fflags);
                n00b_conduit_vnode_fire(w, vnode_ops);
            }
            continue;
        }

        // Handle user events
        if (kev->filter == EVFILT_USER) {
            if (kev->ident == KQUEUE_WAKE_IDENT) {
                continue;
            }
            n00b_conduit_user_event_t *e =
                (n00b_conduit_user_event_t *)kev->udata;
            if (e) {
                n00b_conduit_user_event_fire(e);
            }
            continue;
        }

        // Regular I/O event
        n00b_conduit_io_event_t *ev = &events[event_count];

        ev->fd     = (int)kev->ident;
        ev->ops    = 0;
        ev->topic  = nullptr;
        ev->target = (n00b_conduit_io_target_t *)kev->udata;

        // Map kevent filter to I/O ops
        if (kev->filter == EVFILT_READ) {
            ev->ops |= N00B_CONDUIT_IO_READ;
        }
        if (kev->filter == EVFILT_WRITE) {
            ev->ops |= N00B_CONDUIT_IO_WRITE;
        }

        // Check for errors
        if (kev->flags & EV_ERROR) {
            ev->ops |= N00B_CONDUIT_IO_ERROR;
        }
        if (kev->flags & EV_EOF) {
            ev->ops |= N00B_CONDUIT_IO_HUP;
        }

        event_count++;
    }

    return event_count;
}

/*
 * Get backend name
 */
static n00b_string_t *
kqueue_name(void)
{
    return r"kqueue";
}

static void
kqueue_wake(void *vctx)
{
    kqueue_ctx_t *ctx = vctx;
    if (!ctx || ctx->kq < 0) {
        return;
    }

    struct kevent kev;
    EV_SET(&kev, KQUEUE_WAKE_IDENT, EVFILT_USER,
           0, NOTE_TRIGGER, 0, nullptr);
    (void)kevent(ctx->kq, &kev, 1, nullptr, 0, nullptr);
}

// ============================================================================
// Timer support
// ============================================================================

/*
 * Add a timer to kqueue
 */
static bool
kqueue_timer_add(void *vctx, n00b_conduit_timer_t *timer)
{
    kqueue_ctx_t *ctx = vctx;
    if (!ctx || ctx->kq < 0 || !timer) {
        return false;
    }

    // Add timer to kqueue using EVFILT_TIMER
    // ident = timer ID, data = interval
    // Use microseconds for more portable behavior across BSDs
    struct kevent kev;
    uint16_t flags = EV_ADD;
    if (!timer->repeating) {
        flags |= EV_ONESHOT;
    }

    uint64_t usec = (uint64_t)timer->interval_ms * 1000;
    EV_SET(&kev, timer->id, EVFILT_TIMER, flags, NOTE_USECONDS,
           (intptr_t)usec, timer);

    int ret = kevent(ctx->kq, &kev, 1, nullptr, 0, nullptr);
    if (ret < 0) {
        return false;
    }

    // Add to timer list
    timer->next  = ctx->timers;
    ctx->timers  = timer;

    return true;
}

/*
 * Remove a timer from kqueue
 */
static void
kqueue_timer_remove(void *vctx, n00b_conduit_timer_t *timer)
{
    kqueue_ctx_t *ctx = vctx;
    if (!ctx || ctx->kq < 0 || !timer) {
        return;
    }

    // Remove from kqueue
    struct kevent kev;
    EV_SET(&kev, timer->id, EVFILT_TIMER, EV_DELETE, 0, 0, nullptr);
    kevent(ctx->kq, &kev, 1, nullptr, 0, nullptr);  // Ignore errors

    // Remove from timer list
    n00b_conduit_timer_t **pp = &ctx->timers;
    while (*pp) {
        if (*pp == timer) {
            *pp = timer->next;
            timer->next = nullptr;
            break;
        }
        pp = &(*pp)->next;
    }

    timer->cancelled = true;
}

// ============================================================================
// Signal support
// ============================================================================

/*
 * Add a signal watch to kqueue
 */
static bool
kqueue_signal_acquire_action(int signum)
{
    if (signum <= 0 || signum >= KQUEUE_SIGNAL_MAX) {
        return false;
    }

    kqueue_signal_state_t *state = &g_kqueue_signal_states[signum];
    if (state->refs == 0) {
        struct sigaction sa_ignore;
        memset(&sa_ignore, 0, sizeof(sa_ignore));
        sa_ignore.sa_handler = SIG_IGN;
        sigemptyset(&sa_ignore.sa_mask);
        sa_ignore.sa_flags = 0;
        if (sigaction(signum, &sa_ignore, &state->old_action) < 0) {
            return false;
        }
        state->old_action_saved = true;
    }

    state->refs++;
    return true;
}

static void
kqueue_signal_release_action(int signum)
{
    if (signum <= 0 || signum >= KQUEUE_SIGNAL_MAX) {
        return;
    }

    kqueue_signal_state_t *state = &g_kqueue_signal_states[signum];
    if (state->refs == 0) {
        return;
    }

    state->refs--;
    if (state->refs == 0 && state->old_action_saved) {
        sigaction(signum, &state->old_action, nullptr);
        state->old_action_saved = false;
    }
}

static bool
kqueue_signal_add(void *vctx, n00b_conduit_signal_watch_t *watch)
{
    kqueue_ctx_t *ctx = vctx;
    if (!ctx || ctx->kq < 0 || !watch) {
        return false;
    }

    // Ignore the signal to prevent default handling. kqueue still receives it.
    if (!kqueue_signal_acquire_action(watch->signum)) {
        return false;
    }

    // Add signal to kqueue using EVFILT_SIGNAL
    struct kevent kev;
    EV_SET(&kev, watch->signum, EVFILT_SIGNAL, EV_ADD | EV_CLEAR, 0, 0, watch);

    int ret = kevent(ctx->kq, &kev, 1, nullptr, 0, nullptr);
    if (ret < 0) {
        kqueue_signal_release_action(watch->signum);
        return false;
    }

    // Add to signal list
    watch->next  = ctx->signals;
    ctx->signals = watch;

    return true;
}

/*
 * Remove a signal watch from kqueue
 */
static void
kqueue_signal_remove(void *vctx, n00b_conduit_signal_watch_t *watch)
{
    kqueue_ctx_t *ctx = vctx;
    if (!ctx || ctx->kq < 0 || !watch) {
        return;
    }

    // Remove from kqueue
    struct kevent kev;
    EV_SET(&kev, watch->signum, EVFILT_SIGNAL, EV_DELETE, 0, 0, nullptr);
    kevent(ctx->kq, &kev, 1, nullptr, 0, nullptr);  // Ignore errors

    // Remove from signal list
    n00b_conduit_signal_watch_t **pp = &ctx->signals;
    while (*pp) {
        if (*pp == watch) {
            *pp = watch->next;
            watch->next = nullptr;
            kqueue_signal_release_action(watch->signum);
            break;
        }
        pp = &(*pp)->next;
    }
}

// ============================================================================
// Process monitoring
// ============================================================================

static bool
kqueue_proc_add(void *vctx, n00b_conduit_proc_watch_t *watch)
{
    kqueue_ctx_t *ctx = vctx;
    if (!ctx || ctx->kq < 0 || !watch) {
        return false;
    }

    uint32_t fflags = proc_ops_to_kqueue_fflags(watch->ops);
    struct kevent kev;
    EV_SET(&kev, watch->pid, EVFILT_PROC, EV_ADD | EV_CLEAR, fflags, 0, watch);

    int ret = kevent(ctx->kq, &kev, 1, nullptr, 0, nullptr);
    if (ret < 0 && (fflags & NOTE_EXITSTATUS)) {
        // Not our child (or not signalable): watch the exit without the
        // status. proc_fire's reap still recovers it when the pid is a child.
        fflags &= ~(uint32_t)NOTE_EXITSTATUS;
        EV_SET(&kev, watch->pid, EVFILT_PROC, EV_ADD | EV_CLEAR, fflags, 0, watch);
        ret = kevent(ctx->kq, &kev, 1, nullptr, 0, nullptr);
    }
    if (ret < 0) {
        return false;
    }

    watch->next = ctx->procs;
    ctx->procs  = watch;

    return true;
}

static void
kqueue_proc_remove(void *vctx, n00b_conduit_proc_watch_t *watch)
{
    kqueue_ctx_t *ctx = vctx;
    if (!ctx || ctx->kq < 0 || !watch) {
        return;
    }

    struct kevent kev;
    EV_SET(&kev, watch->pid, EVFILT_PROC, EV_DELETE, 0, 0, nullptr);
    kevent(ctx->kq, &kev, 1, nullptr, 0, nullptr);

    n00b_conduit_proc_watch_t **pp = &ctx->procs;
    while (*pp) {
        if (*pp == watch) {
            *pp = watch->next;
            watch->next = nullptr;
            break;
        }
        pp = &(*pp)->next;
    }
}

// ============================================================================
// Vnode monitoring
// ============================================================================

static bool
kqueue_vnode_add(void *vctx, n00b_conduit_vnode_watch_t *watch)
{
    kqueue_ctx_t *ctx = vctx;
    if (!ctx || ctx->kq < 0 || !watch) {
        return false;
    }

    uint32_t fflags = vnode_ops_to_kqueue_fflags(watch->ops);
    struct kevent kev;
    EV_SET(&kev, watch->fd, EVFILT_VNODE, EV_ADD | EV_CLEAR, fflags, 0, watch);

    int ret = kevent(ctx->kq, &kev, 1, nullptr, 0, nullptr);
    if (ret < 0) {
        return false;
    }

    watch->next = ctx->vnodes;
    ctx->vnodes = watch;

    return true;
}

static void
kqueue_vnode_remove(void *vctx, n00b_conduit_vnode_watch_t *watch)
{
    kqueue_ctx_t *ctx = vctx;
    if (!ctx || ctx->kq < 0 || !watch) {
        return;
    }

    struct kevent kev;
    EV_SET(&kev, watch->fd, EVFILT_VNODE, EV_DELETE, 0, 0, nullptr);
    kevent(ctx->kq, &kev, 1, nullptr, 0, nullptr);

    n00b_conduit_vnode_watch_t **pp = &ctx->vnodes;
    while (*pp) {
        if (*pp == watch) {
            *pp = watch->next;
            watch->next = nullptr;
            break;
        }
        pp = &(*pp)->next;
    }
}

// ============================================================================
// User events
// ============================================================================

static bool
kqueue_user_event_add(void *vctx, n00b_conduit_user_event_t *event)
{
    kqueue_ctx_t *ctx = vctx;
    if (!ctx || ctx->kq < 0 || !event) {
        return false;
    }

    struct kevent kev;
    EV_SET(&kev, event->event_id, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, event);

    int ret = kevent(ctx->kq, &kev, 1, nullptr, 0, nullptr);
    if (ret < 0) {
        return false;
    }

    event->next      = ctx->user_events;
    ctx->user_events = event;

    return true;
}

static void
kqueue_user_event_remove(void *vctx, n00b_conduit_user_event_t *event)
{
    kqueue_ctx_t *ctx = vctx;
    if (!ctx || ctx->kq < 0 || !event) {
        return;
    }

    struct kevent kev;
    EV_SET(&kev, event->event_id, EVFILT_USER, EV_DELETE, 0, 0, nullptr);
    kevent(ctx->kq, &kev, 1, nullptr, 0, nullptr);

    n00b_conduit_user_event_t **pp = &ctx->user_events;
    while (*pp) {
        if (*pp == event) {
            *pp = event->next;
            event->next = nullptr;
            break;
        }
        pp = &(*pp)->next;
    }
}

static void
kqueue_user_event_trigger(void *vctx, n00b_conduit_user_event_t *event)
{
    kqueue_ctx_t *ctx = vctx;
    if (!ctx || ctx->kq < 0 || !event) {
        return;
    }

    // Find the registered event to preserve udata pointer
    n00b_conduit_user_event_t *real = nullptr;
    n00b_conduit_user_event_t *e;
    for (e = ctx->user_events; e; e = e->next) {
        if (e->event_id == event->event_id) {
            real = e;
            break;
        }
    }
    if (!real) {
        return;
    }

    struct kevent kev;
    EV_SET(&kev, real->event_id, EVFILT_USER, 0, NOTE_TRIGGER, 0, real);
    kevent(ctx->kq, &kev, 1, nullptr, 0, nullptr);
}

/*
 * kqueue backend operations
 */
static const n00b_conduit_io_ops_t kqueue_ops = {
    .init                = kqueue_init,
    .cleanup             = kqueue_cleanup,
    .add                 = kqueue_add,
    .modify              = kqueue_modify,
    .remove              = kqueue_remove,
    .wait                = kqueue_wait,
    .wake                = kqueue_wake,
    .name                = kqueue_name,
    .timer_add           = kqueue_timer_add,
    .timer_remove        = kqueue_timer_remove,
    .signal_add          = kqueue_signal_add,
    .signal_remove       = kqueue_signal_remove,
    .proc_add            = kqueue_proc_add,
    .proc_remove         = kqueue_proc_remove,
    .vnode_add           = kqueue_vnode_add,
    .vnode_remove        = kqueue_vnode_remove,
    .user_event_add      = kqueue_user_event_add,
    .user_event_remove   = kqueue_user_event_remove,
    .user_event_trigger  = kqueue_user_event_trigger,
};

n00b_result_t(const n00b_conduit_io_ops_t *)
n00b_conduit_io_kqueue_ops(void)
{
    return n00b_result_ok(const n00b_conduit_io_ops_t *, &kqueue_ops);
}

#else

// Stub for non-kqueue platforms
n00b_result_t(const n00b_conduit_io_ops_t *)
n00b_conduit_io_kqueue_ops(void)
{
    return n00b_result_err(const n00b_conduit_io_ops_t *, N00B_CONDUIT_ERR_NOT_SUPPORTED);
}

#endif // __APPLE__ || BSD
