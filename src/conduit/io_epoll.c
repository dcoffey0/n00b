/*
 * io_epoll.c - Linux epoll I/O backend for conduit
 *
 * Level-triggered epoll with the same Linux FD primitives (timerfd, signalfd,
 * pidfd, inotify, eventfd) used by the io_uring backend.  Fills the gap for
 * Linux systems where io_uring is unavailable (older kernels, containers).
 */

#include "conduit/io.h"
#include "conduit/timer.h"
#include "conduit/signal.h"
#include "conduit/user_event.h"
#include "conduit/proc_lifecycle_internal.h"
#include "internal/conduit/io_wait_set.h"
#include "core/stw.h"
#include "core/syscall.h"

#ifdef __linux__

#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <sys/inotify.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#ifndef P_PIDFD
#define P_PIDFD 3
#endif

// ============================================================================
// Entry type and dispatch structures
// ============================================================================

typedef enum {
    EPOLL_ENTRY_FD_POLL,
    EPOLL_ENTRY_TIMER,
    EPOLL_ENTRY_SIGNAL,
    EPOLL_ENTRY_PROC,
    EPOLL_ENTRY_VNODE,
    EPOLL_ENTRY_USER_EVENT,
    EPOLL_ENTRY_WAKE,
} epoll_entry_type_t;

typedef struct epoll_entry {
    epoll_entry_type_t   type;
    int                  backing_fd; // timerfd/signalfd/pidfd/eventfd/-1
    int                  user_fd;    // original FD for FD_POLL, inotify wd for VNODE
    n00b_conduit_io_op_t poll_mask;  // requested ops for FD_POLL
    n00b_conduit_io_target_t *target; // dispatch target for FD_POLL
    union {
        n00b_conduit_timer_t        *timer;
        n00b_conduit_signal_watch_t *signal_watch;
        n00b_conduit_proc_watch_t   *proc_watch;
        n00b_conduit_vnode_watch_t  *vnode_watch;
        n00b_conduit_user_event_t   *user_event;
    };
    struct epoll_entry *next;
} epoll_entry_t;

// ============================================================================
// Backend context
// ============================================================================

typedef struct {
    int              epfd;
    n00b_conduit_t  *conduit;
    epoll_entry_t   *entries;            // linked list of all registrations
    int              inotify_fd;         // shared inotify instance, -1 if not init
    epoll_entry_t   *inotify_entry;      // poll entry for inotify_fd
    bool             inotify_registered;
    int              wake_fd;
    epoll_entry_t   *wake_entry;
} epoll_ctx_t;

// ============================================================================
// Helpers
// ============================================================================

static_assert(EPOLLIN == POLLIN && EPOLLOUT == POLLOUT,
              "n00b_conduit_io_wait_events() output is used as an epoll mask");

static bool
epoll_register(epoll_ctx_t *ctx, epoll_entry_t *entry, int fd, uint32_t events)
{
    struct epoll_event ev = {
        .events   = events,
        .data.ptr = entry,
    };
    return epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, fd, &ev) == 0;
}

static long
epoll_raw_ctl(epoll_ctx_t *ctx, int op, int fd, struct epoll_event *ev)
{
    return _n00b_raw_linux_syscall4(SYS_epoll_ctl,
                                    (long)ctx->epfd,
                                    (long)op,
                                    (long)fd,
                                    (long)(uintptr_t)ev);
}

/*
 * Apply an FD_POLL entry's mask to the kernel set: EPOLL_CTL_DEL when
 * n00b_conduit_io_wait_events() is empty, otherwise MOD, falling back to ADD
 * for an fd that is not in the set.
 */
static bool
epoll_apply_fd_mask(epoll_ctx_t *ctx, epoll_entry_t *entry)
{
    int      fd     = entry->user_fd;
    uint32_t events = (uint32_t)n00b_conduit_io_wait_events(entry->poll_mask);

    if (!events) {
        long rc = epoll_raw_ctl(ctx, EPOLL_CTL_DEL, fd, nullptr);
        return rc == 0 || rc == -ENOENT;
    }

    struct epoll_event ev = {
        .events   = events,
        .data.ptr = entry,
    };
    long rc = epoll_raw_ctl(ctx, EPOLL_CTL_MOD, fd, &ev);
    if (rc == -ENOENT) {
        rc = epoll_raw_ctl(ctx, EPOLL_CTL_ADD, fd, &ev);
        if (rc == -EEXIST) {
            rc = epoll_raw_ctl(ctx, EPOLL_CTL_MOD, fd, &ev);
        }
    }
    return rc == 0;
}

static void
epoll_raw_write_u64(int fd, uint64_t val)
{
    (void)_n00b_raw_linux_syscall3(SYS_write,
                                   (long)fd,
                                   (long)(uintptr_t)&val,
                                   (long)sizeof(val));
}

static void
epoll_raw_close(int fd)
{
    if (fd >= 0) {
        (void)_n00b_raw_linux_syscall1(SYS_close, (long)fd);
    }
}

static void
epoll_unlink_entry(epoll_ctx_t *ctx, epoll_entry_t *entry)
{
    epoll_entry_t **pp = &ctx->entries;
    while (*pp) {
        if (*pp == entry) {
            *pp         = entry->next;
            entry->next = nullptr;
            return;
        }
        pp = &(*pp)->next;
    }
}

static void
epoll_signal_restore_mask(n00b_conduit_signal_watch_t *watch)
{
    if (!watch || watch->mask_was_blocked) {
        return;
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, watch->signum);
    sigprocmask(SIG_UNBLOCK, &mask, nullptr);
}

// ============================================================================
// Inotify <-> conduit vnode mask conversion
// ============================================================================

static uint32_t
inotify_mask_to_vnode_ops(uint32_t mask)
{
    uint32_t ops = 0;
    if (mask & IN_DELETE_SELF)
        ops |= N00B_CONDUIT_VNODE_DELETE;
    if (mask & IN_MODIFY)
        ops |= N00B_CONDUIT_VNODE_WRITE | N00B_CONDUIT_VNODE_EXTEND;
    if (mask & IN_ATTRIB)
        ops |= N00B_CONDUIT_VNODE_ATTRIB | N00B_CONDUIT_VNODE_LINK;
    if (mask & IN_MOVE_SELF)
        ops |= N00B_CONDUIT_VNODE_RENAME;
    return ops;
}

static uint32_t
vnode_ops_to_inotify_mask(uint32_t ops)
{
    uint32_t mask = 0;
    if (ops & N00B_CONDUIT_VNODE_DELETE)
        mask |= IN_DELETE_SELF;
    if (ops & (N00B_CONDUIT_VNODE_WRITE | N00B_CONDUIT_VNODE_EXTEND))
        mask |= IN_MODIFY;
    if (ops & (N00B_CONDUIT_VNODE_ATTRIB | N00B_CONDUIT_VNODE_LINK))
        mask |= IN_ATTRIB;
    if (ops & N00B_CONDUIT_VNODE_RENAME)
        mask |= IN_MOVE_SELF;
    if (ops & N00B_CONDUIT_VNODE_REVOKE)
        mask |= IN_DELETE_SELF;
    return mask;
}

// ============================================================================
// init / cleanup
// ============================================================================

static void *
epoll_io_init(n00b_conduit_t *c)
{
    // Allocate from the system pool (hidden from GC) so the copying
    // collector never relocates these — the kernel's epoll set holds
    // raw data.ptr pointers to epoll_entry_t objects.
    n00b_allocator_t *sp = (n00b_allocator_t *)&n00b_get_runtime()->system_pool;

    epoll_ctx_t *ctx = n00b_alloc_with_opts(epoll_ctx_t,
                           &(n00b_alloc_opts_t){.allocator = sp});
    if (!ctx)
        return nullptr;

    ctx->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (ctx->epfd < 0)
        return nullptr;

    ctx->conduit            = c;
    ctx->entries            = nullptr;
    ctx->inotify_fd         = -1;
    ctx->inotify_entry      = nullptr;
    ctx->inotify_registered = false;
    ctx->wake_fd            = -1;
    ctx->wake_entry         = nullptr;

    int wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd >= 0) {
        epoll_entry_t *entry = n00b_alloc_with_opts(
            epoll_entry_t,
            &(n00b_alloc_opts_t){.allocator = sp});
        if (entry) {
            entry->type       = EPOLL_ENTRY_WAKE;
            entry->backing_fd = wake_fd;
            entry->user_fd    = -1;
            entry->next       = ctx->entries;
            ctx->entries      = entry;
            if (epoll_register(ctx, entry, wake_fd, EPOLLIN)) {
                ctx->wake_fd    = wake_fd;
                ctx->wake_entry = entry;
            }
            else {
                ctx->entries = entry->next;
                epoll_raw_close(wake_fd);
            }
        }
        else {
            epoll_raw_close(wake_fd);
        }
    }
    return ctx;
}

static void
epoll_io_cleanup(void *vctx)
{
    epoll_ctx_t *ctx = vctx;
    if (!ctx)
        return;

    // Close all backing FDs
    epoll_entry_t *e;
    for (e = ctx->entries; e; e = e->next) {
        if (e->type == EPOLL_ENTRY_SIGNAL && e->signal_watch) {
            epoll_signal_restore_mask(e->signal_watch);
        }
        if (e->backing_fd >= 0) {
            epoll_raw_close(e->backing_fd);
            e->backing_fd = -1;
        }
    }

    if (ctx->inotify_fd >= 0) {
        epoll_raw_close(ctx->inotify_fd);
        ctx->inotify_fd = -1;
    }

    if (ctx->epfd >= 0) {
        epoll_raw_close(ctx->epfd);
        ctx->epfd = -1;
    }
}

// ============================================================================
// FD poll: add / modify / remove
// ============================================================================

static bool
epoll_io_add(void *vctx, int fd, n00b_conduit_io_op_t ops,
             n00b_conduit_io_target_t *target)
{
    epoll_ctx_t *ctx = vctx;
    if (!ctx)
        return false;

    n00b_allocator_t *_sp = (n00b_allocator_t *)&n00b_get_runtime()->system_pool;
    epoll_entry_t *entry = n00b_alloc_with_opts(epoll_entry_t,
                               &(n00b_alloc_opts_t){.allocator = _sp});
    if (!entry)
        return false;

    entry->type       = EPOLL_ENTRY_FD_POLL;
    entry->backing_fd = -1;
    entry->user_fd    = fd;
    entry->poll_mask  = ops;
    entry->target     = target;
    entry->next       = ctx->entries;
    ctx->entries      = entry;

    if (!n00b_conduit_io_wait_events(ops))
        return true;
    return epoll_apply_fd_mask(ctx, entry);
}

static bool
epoll_io_modify(void *vctx, int fd, n00b_conduit_io_op_t ops,
                n00b_conduit_io_target_t *target)
{
    (void)target; // epoll preserves target via the entry linked list
    epoll_ctx_t *ctx = vctx;
    if (!ctx)
        return false;

    epoll_entry_t *entry = nullptr;
    epoll_entry_t *e;
    for (e = ctx->entries; e; e = e->next) {
        if (e->type == EPOLL_ENTRY_FD_POLL && e->user_fd == fd) {
            entry = e;
            break;
        }
    }
    if (!entry)
        return false;

    entry->poll_mask = ops;
    return epoll_apply_fd_mask(ctx, entry);
}

static bool
epoll_io_remove(void *vctx, int fd)
{
    epoll_ctx_t *ctx = vctx;
    if (!ctx)
        return false;

    epoll_entry_t *entry = nullptr;
    epoll_entry_t *e;
    for (e = ctx->entries; e; e = e->next) {
        if (e->type == EPOLL_ENTRY_FD_POLL && e->user_fd == fd) {
            entry = e;
            break;
        }
    }
    if (!entry)
        return false;

    (void)epoll_raw_ctl(ctx, EPOLL_CTL_DEL, fd, nullptr);
    epoll_unlink_entry(ctx, entry);
    return true;
}

// ============================================================================
// wait - epoll dispatch by entry type
// ============================================================================

static int
epoll_io_wait(void *vctx, n00b_conduit_io_event_t *events, int max_events,
              int timeout_ms)
{
    epoll_ctx_t *ctx = vctx;
    if (!ctx || !events || max_events <= 0)
        return -1;

    struct epoll_event ep_events[64];
    int ep_max = max_events < 64 ? max_events : 64;

    long raw_n;
    // eintr: EINTR returns no events, and the event loop waits again.
    n00b_run_blocking(
        raw_n = _n00b_raw_linux_syscall6(SYS_epoll_pwait,
                                         (long)ctx->epfd,
                                         (long)(uintptr_t)ep_events,
                                         (long)ep_max,
                                         (long)timeout_ms,
                                         0,
                                         0));
    if (raw_n < 0) {
        int err = (int)-raw_n;
        if (err == EINTR)
            return 0;
        return -err;
    }
    int n = (int)raw_n;
    if (n == 0)
        return 0;

    int event_count = 0;

    for (int i = 0; i < n; i++) {
        epoll_entry_t *entry = ep_events[i].data.ptr;
        if (!entry)
            continue;

        uint32_t ep_ev = ep_events[i].events;

        switch (entry->type) {
        case EPOLL_ENTRY_FD_POLL: {
            n00b_conduit_io_op_t ops = 0;
            if (ep_ev & EPOLLIN)
                ops |= N00B_CONDUIT_IO_READ;
            if (ep_ev & EPOLLOUT)
                ops |= N00B_CONDUIT_IO_WRITE;
            if (ep_ev & EPOLLERR)
                ops |= N00B_CONDUIT_IO_ERROR;
            if (ep_ev & EPOLLHUP)
                ops |= N00B_CONDUIT_IO_HUP;

            if (event_count < max_events && ops) {
                events[event_count].fd        = entry->user_fd;
                events[event_count].ops       = ops;
                events[event_count].topic     = nullptr;
                events[event_count].target    = entry->target;
                event_count++;
            }
            break;
        }

        case EPOLL_ENTRY_TIMER: {
            if (entry->backing_fd >= 0) {
                uint64_t expirations;
                (void)_n00b_raw_linux_syscall3(SYS_read,
                                               (long)entry->backing_fd,
                                               (long)(uintptr_t)&expirations,
                                               (long)sizeof(expirations));
                if (entry->timer && !entry->timer->cancelled) {
                    n00b_conduit_timer_fire(entry->timer);
                }
            }
            break;
        }

        case EPOLL_ENTRY_SIGNAL: {
            if (entry->backing_fd >= 0) {
                struct signalfd_siginfo info;
                (void)_n00b_raw_linux_syscall3(SYS_read,
                                               (long)entry->backing_fd,
                                               (long)(uintptr_t)&info,
                                               (long)sizeof(info));
                if (entry->signal_watch) {
                    n00b_conduit_signal_fire(entry->signal_watch);
                }
            }
            break;
        }

        case EPOLL_ENTRY_PROC: {
            if (entry->backing_fd >= 0 && entry->proc_watch) {
                siginfo_t info;
                memset(&info, 0, sizeof(info));
                int exit_status = 0;

                if (syscall(SYS_waitid,
                            P_PIDFD,
                            entry->backing_fd,
                            &info,
                            WEXITED | WNOHANG,
                            nullptr)
                        == 0
                    && info.si_pid > 0) {
                    exit_status = n00b_conduit_proc_wait_status_from_siginfo(&info);
                }

                n00b_conduit_proc_fire(entry->proc_watch,
                                       N00B_CONDUIT_PROC_EXIT, exit_status);
            }
            break;
        }

        case EPOLL_ENTRY_VNODE: {
            // inotify fd became readable -- drain all events
            if (ctx->inotify_fd >= 0) {
                char    buf[4096]
                    __attribute__((aligned(__alignof__(struct inotify_event))));
                ssize_t len;
                while ((len = _n00b_raw_linux_syscall3(
                            SYS_read,
                            (long)ctx->inotify_fd,
                            (long)(uintptr_t)buf,
                            (long)sizeof(buf))) > 0) {
                    char *ptr = buf;
                    while (ptr < buf + len) {
                        struct inotify_event *ev = (struct inotify_event *)ptr;
                        epoll_entry_t *ve;
                        for (ve = ctx->entries; ve; ve = ve->next) {
                            if (ve->type == EPOLL_ENTRY_VNODE
                                && ve->user_fd == ev->wd) {
                                uint32_t vops =
                                    inotify_mask_to_vnode_ops(ev->mask);
                                if (vops && ve->vnode_watch) {
                                    n00b_conduit_vnode_fire(ve->vnode_watch,
                                                           vops);
                                }
                                break;
                            }
                        }
                        ptr += sizeof(struct inotify_event) + ev->len;
                    }
                }
            }
            break;
        }

        case EPOLL_ENTRY_USER_EVENT: {
            if (entry->backing_fd >= 0) {
                uint64_t val;
                (void)_n00b_raw_linux_syscall3(SYS_read,
                                               (long)entry->backing_fd,
                                               (long)(uintptr_t)&val,
                                               (long)sizeof(val));
                if (entry->user_event) {
                    n00b_conduit_user_event_fire(entry->user_event);
                }
            }
            break;
        }

        case EPOLL_ENTRY_WAKE: {
            if (entry->backing_fd >= 0) {
                uint64_t val;
                while (_n00b_raw_linux_syscall3(SYS_read,
                                                (long)entry->backing_fd,
                                                (long)(uintptr_t)&val,
                                                (long)sizeof(val)) > 0) {}
            }
            break;
        }
        }
    }

    return event_count;
}

static void
epoll_io_wake(void *vctx)
{
    epoll_ctx_t *ctx = vctx;
    if (!ctx || ctx->wake_fd < 0) {
        return;
    }

    uint64_t val = 1;
    epoll_raw_write_u64(ctx->wake_fd, val);
}

// ============================================================================
// name
// ============================================================================

static n00b_string_t *
epoll_io_name(void)
{
    return r"epoll";
}

// ============================================================================
// Timer support (timerfd)
// ============================================================================

static bool
epoll_timer_add(void *vctx, n00b_conduit_timer_t *timer)
{
    epoll_ctx_t *ctx = vctx;
    if (!ctx || !timer)
        return false;

    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0)
        return false;

    struct itimerspec its = {0};
    its.it_value.tv_sec   = timer->interval_ms / 1000;
    its.it_value.tv_nsec  = (long)(timer->interval_ms % 1000) * 1000000L;
    if (timer->repeating) {
        its.it_interval = its.it_value;
    }

    if (timerfd_settime(tfd, 0, &its, nullptr) < 0) {
        epoll_raw_close(tfd);
        return false;
    }

    n00b_allocator_t *_sp = (n00b_allocator_t *)&n00b_get_runtime()->system_pool;
    epoll_entry_t *entry = n00b_alloc_with_opts(epoll_entry_t,
                               &(n00b_alloc_opts_t){.allocator = _sp});
    if (!entry) {
        epoll_raw_close(tfd);
        return false;
    }

    entry->type       = EPOLL_ENTRY_TIMER;
    entry->backing_fd = tfd;
    entry->user_fd    = -1;
    entry->timer      = timer;
    entry->next       = ctx->entries;
    ctx->entries      = entry;

    return epoll_register(ctx, entry, tfd, EPOLLIN);
}

static void
epoll_timer_remove(void *vctx, n00b_conduit_timer_t *timer)
{
    epoll_ctx_t *ctx = vctx;
    if (!ctx || !timer)
        return;

    epoll_entry_t *e;
    for (e = ctx->entries; e; e = e->next) {
        if (e->type == EPOLL_ENTRY_TIMER && e->timer == timer) {
            if (e->backing_fd >= 0) {
                epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, e->backing_fd, nullptr);
                epoll_raw_close(e->backing_fd);
                e->backing_fd = -1;
            }
            epoll_unlink_entry(ctx, e);
            timer->cancelled = true;
            return;
        }
    }
}

// ============================================================================
// Signal support (signalfd)
// ============================================================================

static bool
epoll_signal_add(void *vctx, n00b_conduit_signal_watch_t *watch)
{
    epoll_ctx_t *ctx = vctx;
    if (!ctx || !watch)
        return false;

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, watch->signum);
    sigset_t old_mask;
    if (sigprocmask(SIG_BLOCK, &mask, &old_mask) != 0) {
        return false;
    }
    watch->mask_was_blocked = sigismember(&old_mask, watch->signum) == 1;

    int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sfd < 0) {
        epoll_signal_restore_mask(watch);
        return false;
    }

    n00b_allocator_t *_sp = (n00b_allocator_t *)&n00b_get_runtime()->system_pool;
    epoll_entry_t *entry = n00b_alloc_with_opts(epoll_entry_t,
                               &(n00b_alloc_opts_t){.allocator = _sp});
    if (!entry) {
        epoll_raw_close(sfd);
        epoll_signal_restore_mask(watch);
        return false;
    }

    entry->type         = EPOLL_ENTRY_SIGNAL;
    entry->backing_fd   = sfd;
    entry->user_fd      = watch->signum;
    entry->signal_watch = watch;
    entry->next         = ctx->entries;
    ctx->entries        = entry;

    watch->next = nullptr;

    if (!epoll_register(ctx, entry, sfd, EPOLLIN)) {
        epoll_unlink_entry(ctx, entry);
        epoll_raw_close(sfd);
        n00b_free(entry);
        epoll_signal_restore_mask(watch);
        return false;
    }

    return true;
}

static void
epoll_signal_remove(void *vctx, n00b_conduit_signal_watch_t *watch)
{
    epoll_ctx_t *ctx = vctx;
    if (!ctx || !watch)
        return;

    epoll_entry_t *e;
    for (e = ctx->entries; e; e = e->next) {
        if (e->type == EPOLL_ENTRY_SIGNAL && e->signal_watch == watch) {
            if (e->backing_fd >= 0) {
                epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, e->backing_fd, nullptr);
                epoll_raw_close(e->backing_fd);
                e->backing_fd = -1;
            }
            epoll_unlink_entry(ctx, e);

            epoll_signal_restore_mask(watch);
            return;
        }
    }
}

// ============================================================================
// Process monitoring (pidfd)
// ============================================================================

static bool
epoll_proc_add(void *vctx, n00b_conduit_proc_watch_t *watch)
{
    epoll_ctx_t *ctx = vctx;
    if (!ctx || !watch)
        return false;

    int pidfd = (int)syscall(SYS_pidfd_open, watch->pid, 0);
    if (pidfd < 0)
        return false;

    n00b_allocator_t *_sp = (n00b_allocator_t *)&n00b_get_runtime()->system_pool;
    epoll_entry_t *entry = n00b_alloc_with_opts(epoll_entry_t,
                               &(n00b_alloc_opts_t){.allocator = _sp});
    if (!entry) {
        epoll_raw_close(pidfd);
        return false;
    }

    entry->type       = EPOLL_ENTRY_PROC;
    entry->backing_fd = pidfd;
    entry->user_fd    = -1;
    entry->proc_watch = watch;
    entry->next       = ctx->entries;
    ctx->entries      = entry;

    watch->next = nullptr;

    return epoll_register(ctx, entry, pidfd, EPOLLIN);
}

static void
epoll_proc_remove(void *vctx, n00b_conduit_proc_watch_t *watch)
{
    epoll_ctx_t *ctx = vctx;
    if (!ctx || !watch)
        return;

    epoll_entry_t *e;
    for (e = ctx->entries; e; e = e->next) {
        if (e->type == EPOLL_ENTRY_PROC && e->proc_watch == watch) {
            if (e->backing_fd >= 0) {
                epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, e->backing_fd, nullptr);
                epoll_raw_close(e->backing_fd);
                e->backing_fd = -1;
            }
            epoll_unlink_entry(ctx, e);
            return;
        }
    }
}

// ============================================================================
// Vnode monitoring (inotify)
// ============================================================================

static bool
epoll_vnode_add(void *vctx, n00b_conduit_vnode_watch_t *watch)
{
    epoll_ctx_t *ctx = vctx;
    if (!ctx || !watch)
        return false;

    // Lazy inotify init
    if (ctx->inotify_fd < 0) {
        ctx->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (ctx->inotify_fd < 0)
            return false;
    }

    // Resolve FD to path via /proc/self/fd/<n>
    char    proc_path[64];
    char    real_path[4096];
    snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", watch->fd);
    ssize_t len = readlink(proc_path, real_path, sizeof(real_path) - 1);
    if (len < 0)
        return false;
    real_path[len] = '\0';

    uint32_t mask = vnode_ops_to_inotify_mask(watch->ops);
    int      wd   = inotify_add_watch(ctx->inotify_fd, real_path, mask);
    if (wd < 0)
        return false;

    n00b_allocator_t *_sp = (n00b_allocator_t *)&n00b_get_runtime()->system_pool;
    epoll_entry_t *entry = n00b_alloc_with_opts(epoll_entry_t,
                               &(n00b_alloc_opts_t){.allocator = _sp});
    if (!entry) {
        inotify_rm_watch(ctx->inotify_fd, wd);
        return false;
    }

    entry->type        = EPOLL_ENTRY_VNODE;
    entry->backing_fd  = -1; // uses shared inotify_fd
    entry->user_fd     = wd; // inotify watch descriptor
    entry->vnode_watch = watch;
    entry->next        = ctx->entries;
    ctx->entries       = entry;

    watch->next = nullptr;

    // Register inotify fd with epoll if not already done
    if (!ctx->inotify_registered) {
        n00b_allocator_t *_isp = (n00b_allocator_t *)&n00b_get_runtime()->system_pool;
        ctx->inotify_entry = n00b_alloc_with_opts(epoll_entry_t,
                                 &(n00b_alloc_opts_t){.allocator = _isp});
        if (!ctx->inotify_entry)
            return false;
        ctx->inotify_entry->type        = EPOLL_ENTRY_VNODE;
        ctx->inotify_entry->backing_fd  = -1;
        ctx->inotify_entry->user_fd     = -1; // sentinel: inotify dispatcher
        ctx->inotify_entry->vnode_watch = nullptr;
        ctx->inotify_entry->next        = ctx->entries;
        ctx->entries                     = ctx->inotify_entry;

        epoll_register(ctx, ctx->inotify_entry, ctx->inotify_fd, EPOLLIN);
        ctx->inotify_registered = true;
    }

    return true;
}

static void
epoll_vnode_remove(void *vctx, n00b_conduit_vnode_watch_t *watch)
{
    epoll_ctx_t *ctx = vctx;
    if (!ctx || !watch)
        return;

    epoll_entry_t *e;
    for (e = ctx->entries; e; e = e->next) {
        if (e->type == EPOLL_ENTRY_VNODE && e->vnode_watch == watch) {
            if (ctx->inotify_fd >= 0 && e->user_fd >= 0) {
                inotify_rm_watch(ctx->inotify_fd, e->user_fd);
            }
            epoll_unlink_entry(ctx, e);
            return;
        }
    }
}

// ============================================================================
// User events (eventfd)
// ============================================================================

static bool
epoll_user_event_add(void *vctx, n00b_conduit_user_event_t *event)
{
    epoll_ctx_t *ctx = vctx;
    if (!ctx || !event)
        return false;

    int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (efd < 0)
        return false;

    n00b_allocator_t *_sp = (n00b_allocator_t *)&n00b_get_runtime()->system_pool;
    epoll_entry_t *entry = n00b_alloc_with_opts(epoll_entry_t,
                               &(n00b_alloc_opts_t){.allocator = _sp});
    if (!entry) {
        epoll_raw_close(efd);
        return false;
    }

    entry->type       = EPOLL_ENTRY_USER_EVENT;
    entry->backing_fd = efd;
    entry->user_fd    = -1;
    entry->user_event = event;
    entry->next       = ctx->entries;
    ctx->entries      = entry;

    event->next = nullptr;

    return epoll_register(ctx, entry, efd, EPOLLIN);
}

static void
epoll_user_event_remove(void *vctx, n00b_conduit_user_event_t *event)
{
    epoll_ctx_t *ctx = vctx;
    if (!ctx || !event)
        return;

    epoll_entry_t *e;
    for (e = ctx->entries; e; e = e->next) {
        if (e->type == EPOLL_ENTRY_USER_EVENT && e->user_event == event) {
            if (e->backing_fd >= 0) {
                epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, e->backing_fd, nullptr);
                epoll_raw_close(e->backing_fd);
                e->backing_fd = -1;
            }
            epoll_unlink_entry(ctx, e);
            return;
        }
    }
}

static void
epoll_user_event_trigger(void *vctx, n00b_conduit_user_event_t *event)
{
    epoll_ctx_t *ctx = vctx;
    if (!ctx || !event)
        return;

    epoll_entry_t *e;
    for (e = ctx->entries; e; e = e->next) {
        if (e->type == EPOLL_ENTRY_USER_EVENT && e->user_event
            && e->user_event->event_id == event->event_id) {
            epoll_raw_write_u64(e->backing_fd, 1);
            return;
        }
    }
}

// ============================================================================
// Vtable and accessor
// ============================================================================

static const n00b_conduit_io_ops_t epoll_ops = {
    .init               = epoll_io_init,
    .cleanup            = epoll_io_cleanup,
    .add                = epoll_io_add,
    .modify             = epoll_io_modify,
    .remove             = epoll_io_remove,
    .wait               = epoll_io_wait,
    .wake               = epoll_io_wake,
    .name               = epoll_io_name,
    .timer_add          = epoll_timer_add,
    .timer_remove       = epoll_timer_remove,
    .signal_add         = epoll_signal_add,
    .signal_remove      = epoll_signal_remove,
    .proc_add           = epoll_proc_add,
    .proc_remove        = epoll_proc_remove,
    .vnode_add          = epoll_vnode_add,
    .vnode_remove       = epoll_vnode_remove,
    .user_event_add     = epoll_user_event_add,
    .user_event_remove  = epoll_user_event_remove,
    .user_event_trigger = epoll_user_event_trigger,
};

n00b_result_t(const n00b_conduit_io_ops_t *)
n00b_conduit_io_epoll_ops(void)
{
    return n00b_result_ok(const n00b_conduit_io_ops_t *, &epoll_ops);
}

#else // !__linux__

n00b_result_t(const n00b_conduit_io_ops_t *)
n00b_conduit_io_epoll_ops(void)
{
    return n00b_result_err(const n00b_conduit_io_ops_t *, N00B_CONDUIT_ERR_NOT_SUPPORTED);
}

#endif // __linux__
