/*
 * metrics.c — Prometheus metrics registry + listener for n00b QUIC.
 *
 * Phase 5 § 5.1.  Registry + counter/gauge/histogram primitives,
 * plus a `/metrics` listener built on top of `conduit/socket.h`'s
 * TCP listen primitive.  No new socket code; all bookkeeping rides
 * the conduit IO loop the caller already has.
 *
 * Concurrency model:
 *   - The entries list and per-metric tuple lists are guarded by
 *     n00b_rwlock_t (n00b_data_lock_new()).  Adding a new metric
 *     or first-observed label tuple takes the write lock; all
 *     hot-path counter/gauge/histogram updates take the read lock
 *     and CAS-update atomic slots.
 *   - The listener uses the conduit's accept-inbox; per-connection
 *     handling is synchronous (read + encode + write + close).
 */

#define N00B_USE_INTERNAL_API
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef _WIN32
#include <fcntl.h>
#endif
#include <errno.h>
#include <ctype.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/eintr.h"
#include "core/time.h"
#include "core/buffer.h"
#include "core/string.h"
#include "core/data_lock.h"
#include "adt/result.h"
#include "adt/list.h"
#include "conduit/conduit.h"
#include "conduit/socket.h"
#include "net/quic/quic_types.h"
#include "net/quic/metrics.h"
#include "internal/net/quic/metrics_internal.h"
#if defined(__linux__)
#include "core/syscall.h"
#endif

#ifdef _WIN32
#define N00B_METRICS_CLOSE_SOCKET(fd) closesocket((SOCKET)(fd))
// eintr: Winsock calls never fail with EINTR.
#define N00B_METRICS_READ(deadline, fd, buf, len) \
    ((void)(deadline), recv((SOCKET)(fd), (buf), (int)(len), 0))
// eintr: as above.
#define N00B_METRICS_WRITE(deadline, fd, buf, len) \
    ((void)(deadline), send((SOCKET)(fd), (const char *)(buf), (int)(len), 0))
#define N00B_METRICS_SOCKOPT_PTR(ptr) ((const char *)(ptr))

static void
metrics_prepare_blocking_socket(base_socket_t fd)
{
    u_long mode = 0;
    (void)ioctlsocket((SOCKET)fd, FIONBIO, &mode);

    int timeout_ms = 1000;
    setsockopt((SOCKET)fd, SOL_SOCKET, SO_RCVTIMEO,
               N00B_METRICS_SOCKOPT_PTR(&timeout_ms), sizeof(timeout_ms));
    setsockopt((SOCKET)fd, SOL_SOCKET, SO_SNDTIMEO,
               N00B_METRICS_SOCKOPT_PTR(&timeout_ms), sizeof(timeout_ms));
}
#elif defined(__linux__)
#define N00B_METRICS_CLOSE_SOCKET(fd) \
    ((void)_n00b_raw_linux_syscall1(SYS_close, (long)(fd)))
#define N00B_METRICS_READ(deadline, fd, buf, len)                       \
    n00b_retry_eintr_raw_until(                                         \
        (deadline),                                                     \
        (ssize_t)_n00b_raw_linux_syscall3(SYS_read,                     \
                                          (long)(fd),                   \
                                          (long)(uintptr_t)(buf),       \
                                          (long)(len)))
#define N00B_METRICS_WRITE(deadline, fd, buf, len)                      \
    n00b_retry_eintr_raw_until(                                         \
        (deadline),                                                     \
        (ssize_t)_n00b_raw_linux_syscall3(SYS_write,                    \
                                          (long)(fd),                   \
                                          (long)(uintptr_t)(buf),       \
                                          (long)(len)))

static void
metrics_prepare_blocking_socket(base_socket_t fd)
{
    long fl = _n00b_raw_linux_syscall3(SYS_fcntl, (long)fd, F_GETFL, 0);
    if (fl >= 0) {
        (void)_n00b_raw_linux_syscall3(SYS_fcntl,
                                       (long)fd,
                                       F_SETFL,
                                       fl & ~O_NONBLOCK);
    }

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    (void)_n00b_raw_linux_syscall5(SYS_setsockopt,
                                   (long)fd,
                                   SOL_SOCKET,
                                   SO_RCVTIMEO,
                                   (long)(uintptr_t)&tv,
                                   (long)sizeof(tv));
    (void)_n00b_raw_linux_syscall5(SYS_setsockopt,
                                   (long)fd,
                                   SOL_SOCKET,
                                   SO_SNDTIMEO,
                                   (long)(uintptr_t)&tv,
                                   (long)sizeof(tv));
}
#else
#define N00B_METRICS_CLOSE_SOCKET(fd) close(fd)
#define N00B_METRICS_READ(deadline, fd, buf, len) \
    n00b_retry_eintr_until((deadline), read((fd), (buf), (len)))
#define N00B_METRICS_WRITE(deadline, fd, buf, len) \
    n00b_retry_eintr_until((deadline), write((fd), (buf), (len)))

static void
metrics_prepare_blocking_socket(base_socket_t fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) {
        (void)fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    }

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}
#endif

// On Linux, a signal interrupts a blocking call on a socket with a timeout
// set even under SA_RESTART, where macOS restarts it. N00B_METRICS_READ and
// N00B_METRICS_WRITE retry until `deadline`, which stands in for the
// socket's own timeout.
#define METRICS_IO_TIMEOUT_NS ((uint64_t)1000 * N00B_NS_PER_MS)

/* ===========================================================================
 * Allocator
 * =========================================================================== */

static n00b_allocator_t *
metrics_default_alloc(void)
{
    return (n00b_allocator_t *)&n00b_get_runtime()->conduit_pool;
}

static char *
metrics_strdup(n00b_allocator_t *al, const char *s)
{
    if (!s) return nullptr;
    size_t l = strlen(s);
    char  *o = n00b_alloc_array_with_opts(char, (int64_t)(l + 1),
                                          &(n00b_alloc_opts_t){
                                              .allocator = al,
                                              .no_scan   = true,
                                          });
    memcpy(o, s, l + 1);
    return o;
}

static n00b_buffer_t *
metrics_buffer_copy(n00b_allocator_t *al, n00b_buffer_t *src)
{
    if (!src) return nullptr;
    n00b_buffer_t *cp = n00b_buffer_empty(.allocator = al);
    n00b_buffer_resize(cp, src->byte_len);
    if (src->byte_len > 0) {
        memcpy(cp->data, src->data, src->byte_len);
    }
    return cp;
}

static n00b_list_t(n00b_buffer_t *) *
metrics_label_list_copy(n00b_allocator_t              *al,
                        n00b_list_t(n00b_buffer_t *) *src)
{
    if (!src) return nullptr;
    n00b_list_t(n00b_buffer_t *) *cp = n00b_alloc_with_opts(
        n00b_list_t(n00b_buffer_t *),
        &(n00b_alloc_opts_t){.allocator = al});
    *cp = n00b_list_new(n00b_buffer_t *, .allocator = al);
    size_t n = (size_t)n00b_list_len(*src);
    for (size_t i = 0; i < n; i++) {
        n00b_list_push(*cp, metrics_buffer_copy(al, n00b_list_get(*src, i)));
    }
    return cp;
}

/* ===========================================================================
 * Validation
 * =========================================================================== */

/* Prom name: [a-zA-Z_:][a-zA-Z0-9_:]*  (we forbid `:` because we
 * never namespace user metrics on top of Prom-internal ones, and
 * the codepath is simpler if names are pure C identifiers.) */
static bool
valid_prom_name(const char *n)
{
    if (!n || !*n) return false;
    char c0 = n[0];
    if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z') || c0 == '_')) {
        return false;
    }
    const char *p;
    for (p = n + 1; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
              || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

/* ===========================================================================
 * Registry construction
 * =========================================================================== */

n00b_quic_metric_registry_t *
n00b_quic_metrics_registry_new(n00b_allocator_t *allocator)
{
    n00b_allocator_t *al = allocator ? allocator : metrics_default_alloc();
    n00b_quic_metric_registry_t *r = n00b_alloc_with_opts(
        n00b_quic_metric_registry_t,
        &(n00b_alloc_opts_t){.allocator = al});
    r->allocator = al;
    r->entries = n00b_alloc_with_opts(
        n00b_list_t(n00b_quic_metric_entry_t *),
        &(n00b_alloc_opts_t){.allocator = al});
    *r->entries = n00b_list_new(n00b_quic_metric_entry_t *,
                                .allocator = al);
    r->lock = n00b_data_lock_new(.allocator = al);
    return r;
}

/* ===========================================================================
 * Lookup helpers
 * =========================================================================== */

static n00b_quic_metric_entry_t *
registry_find_entry(n00b_quic_metric_registry_t *r, const char *name)
{
    /* Caller holds at least the read lock. */
    size_t n = (size_t)n00b_list_len(*r->entries);
    for (size_t i = 0; i < n; i++) {
        n00b_quic_metric_entry_t *e = n00b_list_get(*r->entries, i);
        const char *en = nullptr;
        switch (e->selector) {
        case typehash(n00b_quic_metric_counter_t *):
            en = n00b_variant_get(*e, n00b_quic_metric_counter_t *)->name;
            break;
        case typehash(n00b_quic_metric_gauge_t *):
            en = n00b_variant_get(*e, n00b_quic_metric_gauge_t *)->name;
            break;
        case typehash(n00b_quic_metric_hist_t *):
            en = n00b_variant_get(*e, n00b_quic_metric_hist_t *)->name;
            break;
        }
        if (en && strcmp(en, name) == 0) return e;
    }
    return nullptr;
}

static bool
buf_eq(n00b_buffer_t *a, n00b_buffer_t *b)
{
    if (a == b) return true;
    if (!a || !b) return false;
    return a->byte_len == b->byte_len
           && memcmp(a->data, b->data, a->byte_len) == 0;
}

static bool
tuple_values_match(n00b_quic_metric_tuple_t      *t,
                   n00b_list_t(n00b_buffer_t *) *v)
{
    /* Both nullptr (unlabeled metric, single tuple) → match. */
    if (!t->values && !v) return true;
    if (!t->values || !v) return false;
    int64_t tn = n00b_list_len(*t->values);
    int64_t vn = n00b_list_len(*v);
    if (tn != vn) return false;
    for (int64_t i = 0; i < tn; i++) {
        if (!buf_eq(n00b_list_get(*t->values, i),
                    n00b_list_get(*v, i))) return false;
    }
    return true;
}

static n00b_quic_metric_tuple_t *
find_or_create_tuple(n00b_quic_metric_registry_t              *r,
                     n00b_list_t(n00b_quic_metric_tuple_t *) **tuples_p,
                     n00b_rwlock_t                            *lock,
                     n00b_list_t(n00b_buffer_t *)             *values,
                     size_t                                    n_buckets_for_hist)
{
    n00b_data_read_lock(lock);
    n00b_list_t(n00b_quic_metric_tuple_t *) *tuples = *tuples_p;
    if (tuples) {
        size_t n = (size_t)n00b_list_len(*tuples);
        for (size_t i = 0; i < n; i++) {
            n00b_quic_metric_tuple_t *t = n00b_list_get(*tuples, i);
            if (tuple_values_match(t, values)) {
                n00b_data_unlock(lock);
                return t;
            }
        }
    }
    n00b_data_unlock(lock);

    /* Promote: create + insert. */
    n00b_data_write_lock(lock);
    /* Re-check under write lock. */
    tuples = *tuples_p;
    if (tuples) {
        size_t n = (size_t)n00b_list_len(*tuples);
        for (size_t i = 0; i < n; i++) {
            n00b_quic_metric_tuple_t *t = n00b_list_get(*tuples, i);
            if (tuple_values_match(t, values)) {
                n00b_data_unlock(lock);
                return t;
            }
        }
    } else {
        *tuples_p = n00b_alloc_with_opts(
            n00b_list_t(n00b_quic_metric_tuple_t *),
            &(n00b_alloc_opts_t){.allocator = r->allocator});
        **tuples_p = n00b_list_new(n00b_quic_metric_tuple_t *,
                                   .allocator = r->allocator);
        tuples = *tuples_p;
    }

    n00b_quic_metric_tuple_t *t = n00b_alloc_with_opts(
        n00b_quic_metric_tuple_t,
        &(n00b_alloc_opts_t){.allocator = r->allocator});

    if (values) {
        /* Copy the value list into registry-allocator storage so
         * the caller can free its own. */
        t->values = n00b_alloc_with_opts(
            n00b_list_t(n00b_buffer_t *),
            &(n00b_alloc_opts_t){.allocator = r->allocator});
        *t->values = n00b_list_new(n00b_buffer_t *,
                                   .allocator = r->allocator);
        size_t vn = (size_t)n00b_list_len(*values);
        for (size_t i = 0; i < vn; i++) {
            n00b_list_push(*t->values,
                           metrics_buffer_copy(r->allocator,
                                               n00b_list_get(*values, i)));
        }
    }

    if (n_buckets_for_hist > 0) {
        t->hist_buckets = n00b_alloc_array_with_opts(
            _Atomic uint64_t, (int64_t)n_buckets_for_hist + 1,
            &(n00b_alloc_opts_t){.allocator = r->allocator,
                                  .no_scan   = true});
        for (size_t i = 0; i <= n_buckets_for_hist; i++) {
            atomic_store(&t->hist_buckets[i], 0);
        }
        atomic_store(&t->hist_count, 0);
        atomic_store(&t->hist_sum_bits, 0);
    }
    atomic_store(&t->counter_u64, 0);
    atomic_store(&t->gauge_bits, 0);

    n00b_list_push(*tuples, t);
    n00b_data_unlock(lock);
    return t;
}

/* ===========================================================================
 * Counters
 * =========================================================================== */

n00b_result_t(n00b_quic_metric_counter_t *)
n00b_quic_metric_counter(n00b_quic_metric_registry_t *r,
                         const char                  *name,
                         const char                  *help) _kargs
{
    n00b_list_t(n00b_buffer_t *) *labels = nullptr;
}
{
    if (!r || !name || !help || !valid_prom_name(name)) {
        return n00b_result_err(n00b_quic_metric_counter_t *,
                               N00B_QUIC_ERR_INVALID_ARG);
    }
    n00b_data_write_lock(r->lock);
    n00b_quic_metric_entry_t *existing = registry_find_entry(r, name);
    if (existing) {
        if (!n00b_variant_is_type(*existing, n00b_quic_metric_counter_t *)) {
            n00b_data_unlock(r->lock);
            return n00b_result_err(n00b_quic_metric_counter_t *,
                                   N00B_QUIC_ERR_INVALID_ARG);
        }
        n00b_data_unlock(r->lock);
        return n00b_result_ok(n00b_quic_metric_counter_t *,
                              n00b_variant_get(*existing,
                                               n00b_quic_metric_counter_t *));
    }
    n00b_quic_metric_counter_t *c = n00b_alloc_with_opts(
        n00b_quic_metric_counter_t,
        &(n00b_alloc_opts_t){.allocator = r->allocator});
    c->registry    = r;
    c->name        = metrics_strdup(r->allocator, name);
    c->help        = metrics_strdup(r->allocator, help);
    c->label_names = metrics_label_list_copy(r->allocator, labels);
    c->tuples      = nullptr;
    c->lock        = n00b_data_lock_new(.allocator = r->allocator);

    n00b_quic_metric_entry_t *e = n00b_alloc_with_opts(
        n00b_quic_metric_entry_t,
        &(n00b_alloc_opts_t){.allocator = r->allocator});
    *e = n00b_variant_set(n00b_quic_metric_entry_t,
                          n00b_quic_metric_counter_t *, c);
    n00b_list_push(*r->entries, e);
    n00b_data_unlock(r->lock);
    return n00b_result_ok(n00b_quic_metric_counter_t *, c);
}

void
n00b_quic_metric_counter_inc(n00b_quic_metric_counter_t *c, uint64_t by) _kargs
{
    n00b_list_t(n00b_buffer_t *) *label_values = nullptr;
}
{
    if (!c) return;
    n00b_quic_metric_tuple_t *t =
        find_or_create_tuple(c->registry, &c->tuples, c->lock,
                             label_values, 0);
    atomic_fetch_add(&t->counter_u64, by);
}

/* ===========================================================================
 * Gauges
 * =========================================================================== */

n00b_result_t(n00b_quic_metric_gauge_t *)
n00b_quic_metric_gauge(n00b_quic_metric_registry_t *r,
                       const char                  *name,
                       const char                  *help) _kargs
{
    n00b_list_t(n00b_buffer_t *) *labels = nullptr;
}
{
    if (!r || !name || !help || !valid_prom_name(name)) {
        return n00b_result_err(n00b_quic_metric_gauge_t *,
                               N00B_QUIC_ERR_INVALID_ARG);
    }
    n00b_data_write_lock(r->lock);
    n00b_quic_metric_entry_t *existing = registry_find_entry(r, name);
    if (existing) {
        if (!n00b_variant_is_type(*existing, n00b_quic_metric_gauge_t *)) {
            n00b_data_unlock(r->lock);
            return n00b_result_err(n00b_quic_metric_gauge_t *,
                                   N00B_QUIC_ERR_INVALID_ARG);
        }
        n00b_data_unlock(r->lock);
        return n00b_result_ok(n00b_quic_metric_gauge_t *,
                              n00b_variant_get(*existing,
                                               n00b_quic_metric_gauge_t *));
    }
    n00b_quic_metric_gauge_t *g = n00b_alloc_with_opts(
        n00b_quic_metric_gauge_t,
        &(n00b_alloc_opts_t){.allocator = r->allocator});
    g->registry    = r;
    g->name        = metrics_strdup(r->allocator, name);
    g->help        = metrics_strdup(r->allocator, help);
    g->label_names = metrics_label_list_copy(r->allocator, labels);
    g->tuples      = nullptr;
    g->lock        = n00b_data_lock_new(.allocator = r->allocator);

    n00b_quic_metric_entry_t *e = n00b_alloc_with_opts(
        n00b_quic_metric_entry_t,
        &(n00b_alloc_opts_t){.allocator = r->allocator});
    *e = n00b_variant_set(n00b_quic_metric_entry_t,
                          n00b_quic_metric_gauge_t *, g);
    n00b_list_push(*r->entries, e);
    n00b_data_unlock(r->lock);
    return n00b_result_ok(n00b_quic_metric_gauge_t *, g);
}

void
n00b_quic_metric_gauge_set(n00b_quic_metric_gauge_t *g, double v) _kargs
{
    n00b_list_t(n00b_buffer_t *) *label_values = nullptr;
}
{
    if (!g) return;
    n00b_quic_metric_tuple_t *t =
        find_or_create_tuple(g->registry, &g->tuples, g->lock,
                             label_values, 0);
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    atomic_store(&t->gauge_bits, bits);
}

/* ===========================================================================
 * Histograms
 * =========================================================================== */

n00b_result_t(n00b_quic_metric_hist_t *)
n00b_quic_metric_hist(n00b_quic_metric_registry_t *r,
                      const char                  *name,
                      const char                  *help,
                      const double                *buckets,
                      size_t                       n_buckets) _kargs
{
    n00b_list_t(n00b_buffer_t *) *labels = nullptr;
}
{
    if (!r || !name || !help || !valid_prom_name(name)
        || !buckets || n_buckets == 0) {
        return n00b_result_err(n00b_quic_metric_hist_t *,
                               N00B_QUIC_ERR_INVALID_ARG);
    }
    /* Buckets must be ascending. */
    for (size_t i = 1; i < n_buckets; i++) {
        if (buckets[i] <= buckets[i - 1]) {
            return n00b_result_err(n00b_quic_metric_hist_t *,
                                   N00B_QUIC_ERR_INVALID_ARG);
        }
    }
    n00b_data_write_lock(r->lock);
    n00b_quic_metric_entry_t *existing = registry_find_entry(r, name);
    if (existing) {
        if (!n00b_variant_is_type(*existing, n00b_quic_metric_hist_t *)) {
            n00b_data_unlock(r->lock);
            return n00b_result_err(n00b_quic_metric_hist_t *,
                                   N00B_QUIC_ERR_INVALID_ARG);
        }
        n00b_data_unlock(r->lock);
        return n00b_result_ok(n00b_quic_metric_hist_t *,
                              n00b_variant_get(*existing,
                                               n00b_quic_metric_hist_t *));
    }
    n00b_quic_metric_hist_t *h = n00b_alloc_with_opts(
        n00b_quic_metric_hist_t,
        &(n00b_alloc_opts_t){.allocator = r->allocator});
    h->registry    = r;
    h->name        = metrics_strdup(r->allocator, name);
    h->help        = metrics_strdup(r->allocator, help);
    h->label_names = metrics_label_list_copy(r->allocator, labels);
    h->tuples      = nullptr;
    h->lock        = n00b_data_lock_new(.allocator = r->allocator);
    h->n_buckets   = n_buckets;
    h->upper_bounds = n00b_alloc_array_with_opts(double, (int64_t)n_buckets,
        &(n00b_alloc_opts_t){.allocator = r->allocator, .no_scan = true});
    memcpy(h->upper_bounds, buckets, n_buckets * sizeof(double));

    n00b_quic_metric_entry_t *e = n00b_alloc_with_opts(
        n00b_quic_metric_entry_t,
        &(n00b_alloc_opts_t){.allocator = r->allocator});
    *e = n00b_variant_set(n00b_quic_metric_entry_t,
                          n00b_quic_metric_hist_t *, h);
    n00b_list_push(*r->entries, e);
    n00b_data_unlock(r->lock);
    return n00b_result_ok(n00b_quic_metric_hist_t *, h);
}

void
n00b_quic_metric_hist_observe(n00b_quic_metric_hist_t *h, double v) _kargs
{
    n00b_list_t(n00b_buffer_t *) *label_values = nullptr;
}
{
    if (!h) return;
    n00b_quic_metric_tuple_t *t =
        find_or_create_tuple(h->registry, &h->tuples, h->lock,
                             label_values, h->n_buckets);
    /* Cumulative counts: first bucket whose upper_bound >= v
     * gets incremented; so do all buckets above it (per Prom
     * histogram semantics). */
    bool counted = false;
    for (size_t i = 0; i < h->n_buckets; i++) {
        if (v <= h->upper_bounds[i]) {
            atomic_fetch_add(&t->hist_buckets[i], 1);
            counted = true;
        }
    }
    if (!counted) {
        /* No finite bucket matched; +Inf bucket only. */
    }
    atomic_fetch_add(&t->hist_buckets[h->n_buckets], 1); /* +Inf */
    atomic_fetch_add(&t->hist_count, 1);
    /* sum: load bits → add v → store bits, with CAS retry. */
    uint64_t old_bits, new_bits;
    double old_sum, new_sum;
    do {
        old_bits = atomic_load(&t->hist_sum_bits);
        memcpy(&old_sum, &old_bits, sizeof(old_sum));
        new_sum = old_sum + v;
        memcpy(&new_bits, &new_sum, sizeof(new_bits));
    } while (!atomic_compare_exchange_weak(&t->hist_sum_bits,
                                           &old_bits, new_bits));
}

/* ===========================================================================
 * Public encode entry point
 * =========================================================================== */

n00b_buffer_t *
n00b_quic_metrics_encode(n00b_quic_metric_registry_t *r)
{
    n00b_buffer_t *b = n00b_buffer_empty(.allocator = r ? r->allocator
                                                        : metrics_default_alloc());
    if (r) _n00b_quic_metrics_render(r, b);
    return b;
}

/* ===========================================================================
 * Listener
 * =========================================================================== */

n00b_result_t(n00b_quic_metric_listener_t *)
n00b_quic_metrics_listener_open(n00b_quic_metric_registry_t *r,
                                n00b_conduit_t              *c,
                                n00b_conduit_io_backend_t   *io) _kargs
{
    n00b_buffer_t *bind_host = nullptr;
    uint16_t       bind_port = 9100;
    n00b_buffer_t *path      = nullptr;
}
{
    if (!r || !c || !io) {
        return n00b_result_err(n00b_quic_metric_listener_t *,
                               N00B_QUIC_ERR_NULL_ARG);
    }
    n00b_string_t *host_str = bind_host
        ? n00b_string_from_cstr((const char *)bind_host->data)
        : r"::1";
    auto lr = n00b_conduit_listen_tcp(c, io, host_str, bind_port, 16);
    if (!n00b_result_is_ok(lr)) {
        return n00b_result_err(n00b_quic_metric_listener_t *,
                               N00B_QUIC_ERR_BIND_FAILED);
    }
    n00b_conduit_listener_t *cl = n00b_result_get(lr);

    auto at_opt = n00b_conduit_listener_accept_topic(cl);
    if (!n00b_option_is_set(at_opt)) {
        n00b_conduit_listener_close(cl);
        return n00b_result_err(n00b_quic_metric_listener_t *,
                               N00B_QUIC_ERR_BIND_FAILED);
    }
    n00b_conduit_topic_base_t *accept_topic = n00b_option_get(at_opt);

    n00b_quic_metric_listener_t *l = n00b_alloc_with_opts(
        n00b_quic_metric_listener_t,
        &(n00b_alloc_opts_t){.allocator = r->allocator});
    l->registry     = r;
    l->conduit      = c;
    l->io           = io;
    l->tcp_listener = cl;
    l->closed       = false;

    /* Default path is "/metrics".  Always store as a buffer in our
     * allocator so the caller's path doesn't need to outlive us. */
    const char *p_cstr = path ? (const char *)path->data : "/metrics";
    n00b_buffer_t *p_copy = n00b_buffer_empty(.allocator = r->allocator);
    size_t pl = strlen(p_cstr);
    n00b_buffer_resize(p_copy, pl + 1);
    memcpy(p_copy->data, p_cstr, pl + 1);
    n00b_buffer_resize(p_copy, pl);
    l->path = p_copy;

    /* Subscribe an accept inbox. */
    n00b_conduit_sock_accept_inbox_t *inbox = n00b_alloc_with_opts(
        n00b_conduit_sock_accept_inbox_t,
        &(n00b_alloc_opts_t){.allocator = r->allocator});
    n00b_conduit_inbox_init(n00b_conduit_sock_accept_payload_t,
                            inbox, c,
                            N00B_CONDUIT_BP_UNBOUNDED, 0);
    l->accept_inbox = inbox;
    l->sub_handle = n00b_conduit_sock_accept_subscribe(
        accept_topic, inbox, .operations = N00B_CONDUIT_OP_ALL);
    if (l->sub_handle == N00B_CONDUIT_INVALID_SUB_HANDLE) {
        n00b_conduit_listener_close(cl);
        return n00b_result_err(n00b_quic_metric_listener_t *,
                               N00B_QUIC_ERR_BIND_FAILED);
    }
    return n00b_result_ok(n00b_quic_metric_listener_t *, l);
}

size_t
n00b_quic_metrics_listener_run_once(n00b_quic_metric_listener_t *l)
{
    if (!l || l->closed) return 0;
    size_t serviced = 0;
    n00b_conduit_sock_accept_msg_t *msg;
    while ((msg = n00b_conduit_sock_accept_inbox_pop(l->accept_inbox))) {
        base_socket_t fd = msg->payload.client_fd;
        if (fd == BASE_INVALID_SOCKET) continue;
        _n00b_quic_metrics_handle_conn(l, fd);
        serviced++;
    }
    return serviced;
}

void
n00b_quic_metrics_listener_close(n00b_quic_metric_listener_t *l)
{
    if (!l || l->closed) return;
    l->closed = true;
    if (l->tcp_listener) {
        n00b_conduit_listener_close(l->tcp_listener);
        l->tcp_listener = nullptr;
    }
}

uint16_t
n00b_quic_metrics_listener_port(n00b_quic_metric_listener_t *l)
{
    if (!l || l->closed || !l->tcp_listener ||
        l->tcp_listener->fd == BASE_INVALID_SOCKET) {
        return 0;
    }
    struct sockaddr_storage ss;
    socklen_t               slen = sizeof(ss);
    if (getsockname(l->tcp_listener->fd, (struct sockaddr *)&ss, &slen) != 0) {
        return 0;
    }
    if (ss.ss_family == AF_INET) {
        return ntohs(((struct sockaddr_in *)&ss)->sin_port);
    }
    if (ss.ss_family == AF_INET6) {
        return ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
    }
    return 0;
}

/* ===========================================================================
 * Per-connection HTTP/1.1 handler
 * =========================================================================== */

#define METRICS_REQ_MAX 4096

// Writes all of `bytes`, resuming after signals and short writes. Each
// stretch without progress gets the socket's timeout. Returns false when the
// peer cannot take the rest.
static bool
metrics_write_all(base_socket_t fd, const char *bytes, size_t len)
{
    while (len > 0) {
        uint64_t deadline = n00b_ns_timestamp() + METRICS_IO_TIMEOUT_NS;
        ssize_t  w        = N00B_METRICS_WRITE(deadline, fd, bytes, len);
        if (w <= 0) {
            return false;
        }
        bytes += w;
        len -= (size_t)w;
    }
    return true;
}

void
_n00b_quic_metrics_handle_conn(n00b_quic_metric_listener_t *l,
                               base_socket_t                fd)
{
    /* Read up to METRICS_REQ_MAX bytes; bail on first \r\n\r\n. */
    char    buf[METRICS_REQ_MAX];
    size_t  total = 0;
    bool    have_eoh = false;

    /* The conduit listener handed us a non-blocking fd; switch it
     * to blocking and bound it with SO_{RCV,SND}TIMEO so this
     * synchronous handler doesn't spin and doesn't hang. */
    metrics_prepare_blocking_socket(fd);

    uint64_t deadline = n00b_ns_timestamp() + METRICS_IO_TIMEOUT_NS;
    while (total < sizeof(buf) - 1) {
        ssize_t n = N00B_METRICS_READ(deadline,
                                      fd,
                                      buf + total,
                                      sizeof(buf) - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) { have_eoh = true; break; }
    }

    /* Parse request line: "GET <path> HTTP/1.x\r\n" */
    int         status = 400;
    const char *status_text = "Bad Request";

    if (have_eoh && total >= 4 && memcmp(buf, "GET ", 4) == 0) {
        const char *path_start = buf + 4;
        const char *path_end = memchr(path_start, ' ', total - 4);
        if (path_end) {
            size_t plen = (size_t)(path_end - path_start);
            if (plen == l->path->byte_len
                && memcmp(path_start, l->path->data, plen) == 0) {
                status = 200;
                status_text = "OK";
            } else {
                status = 404;
                status_text = "Not Found";
            }
        }
    }

    /* Build response.  Resolve body + length first so the
     * Content-Length header agrees with the bytes we write. */
    n00b_buffer_t *body_buf = nullptr;
    const char    *body_str = NULL;
    size_t         body_len = 0;
    if (status == 200) {
        body_buf = n00b_quic_metrics_encode(l->registry);
        body_len = body_buf ? body_buf->byte_len : 0;
    } else {
        body_str = (status == 404) ? "Not Found\n" : "Bad Request\n";
        body_len = strlen(body_str);
    }
    char head[256];
    int hl = snprintf(head, sizeof(head),
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      status, status_text, body_len);
    if (hl > 0 && metrics_write_all(fd, head, (size_t)hl)) {
        if (body_buf && body_len > 0) {
            (void)metrics_write_all(fd, body_buf->data, body_len);
        }
        else if (body_str && body_len > 0) {
            (void)metrics_write_all(fd, body_str, body_len);
        }
    }
    N00B_METRICS_CLOSE_SOCKET(fd);
}
