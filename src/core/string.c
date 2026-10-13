#define N00B_NO_STRING_SITE_PROXY
#include "core/string.h"
#include "core/alloc.h"
#include "core/pool.h"
#include "core/runtime.h"
#include "internal/text/unicode/raw.h"
#include "text/strings/string_ops.h"
#include <string.h>

/* Thread-local scratch pool for transparent intermediate-string
 * allocation.  When a string-builder is called without an explicit
 * @c .allocator kwarg, no @ref n00b_with_allocator scope is active,
 * and no outer string-builder already set up a scratch, the builder
 * stands up a per-thread scratch pool, lets all inner allocations
 * go into it (via @ref n00b_with_allocator), and at the outermost
 * exit deep-copies the result string out into the runtime's default
 * allocator and tears down the scratch — confining every transient
 * format / concat / etc. allocation that would otherwise pollute
 * the GC default arena.
 *
 * Nested builder calls share the outer scratch (created==false in
 * the inner scope token) so a single event handler that does many
 * cformat / from_cstr calls only stands up one scratch.
 *
 * The scratch lives in the per-thread n00b_thread_t (folding out a
 * former thread_local, D-005/D-012 cont.): string_scratch_storage is
 * the control struct (allocated once from system_pool — non-GC-scanned,
 * pinned — and reused for the thread's life) and string_scratch_pool is
 * the active marker.  Both are reached via n00b_thread_self() so a raw
 * (off-libc, WP-001) worker thread needs ZERO TLS — touching a
 * thread_local on such a thread crashes via the dyld TLV path
 * (_tlv_get_addr → pthread_self), which is the bug this folds out.
 * pool_destroy on scope exit munmaps the page table but leaves the
 * control struct zeroed-by-pool_init-ready for the next outermost
 * entry. */

n00b_string_scope_t
n00b_string_scope_enter(n00b_allocator_t **resolved)
{
    /* Explicit allocator wins. */
    if (*resolved != nullptr) {
        return (n00b_string_scope_t){.created = false};
    }
    /* Active @ref n00b_with_allocator scope wins next. */
    n00b_allocator_t *ovr = n00b_current_allocator();
    if (ovr != nullptr) {
        *resolved = ovr;
        return (n00b_string_scope_t){.created = false};
    }
    /* The scratch now lives in the per-thread n00b_thread_t, reached
     * via n00b_thread_self() (no TLS).  A thread that is not yet
     * registered (pre-registration window: self->record == nullptr,
     * the idiomatic check — cf. raw_gateway ingest.c) or before the
     * runtime is up has no per-thread home, so it skips the scratch
     * and falls back to the runtime default via @ref
     * n00b_ensure_allocator (matching the former pre-init behavior;
     * also correct for the `static` n00b_string_t descriptors emitted
     * by the ncc static-image transform — built into the binary, not
     * dynamically allocated). */
    n00b_thread_t  *self = n00b_thread_self();
    n00b_runtime_t *rt   = n00b_get_runtime();
    if (rt == nullptr || self == nullptr || self->record == nullptr) {
        return (n00b_string_scope_t){.created = false};
    }
    /* Nested builder call: share the outer scratch. */
    if (self->string_scratch_arena != nullptr) {
        *resolved = (n00b_allocator_t *)self->string_scratch_arena;
        return (n00b_string_scope_t){.created = false};
    }
    /* Outermost: stand up a fresh scratch.  The control struct is
     * allocated once per thread from system_pool (non-GC-scanned,
     * pinned, no_scan) and reused for the thread's life; pool_init is
     * hidden so the GC's metadata-pool walk doesn't pick it up (the
     * whole point is to keep this off the GC's plate). */
    if (self->string_scratch_storage == nullptr) {
        self->string_scratch_storage = n00b_alloc_with_opts(
            n00b_pool_t,
            &(n00b_alloc_opts_t){.allocator = (n00b_allocator_t *)&rt->system_pool,
                                 .no_scan   = true});
    }
    // The string scratch pool is created and destroyed on every outermost
    // string-builder scope (every n00b_cformat).  pool_destroy's lock-chain
    // scrub dominates that teardown — and under concurrent load, when another
    // thread holds an exclusive (write) lock or mutex, the scrub's fast-path
    // can't early-out (it scans the exclusive-lock chain) and it walks every
    // thread's chain per page on each scope exit.
    //
    // INVARIANT this opt-out relies on: nothing allocated into this scratch may
    // embed an n00b_mutex_t / n00b_rwlock_t (N00B_COMMON_LOCK_BASE).  Today it
    // only holds n00b_string_t structs + raw char buffers — no lock-bearing
    // object (locks live in Regex-style objects, not here), so the scrub has
    // nothing to unlink.  If a future formatting path ever builds a lock-bearing
    // object (e.g. a locked n00b_list_t) into this scratch, revisit this.
    // Covered by test/unit/test_string_scratch_raw_worker.c.
    n00b_pool_init(self->string_scratch_storage,
                   .hidden                 = true,
                   .scrub_locks_on_destroy = false,
                   .use_epochs             = false,
                   .name                   = "n00b_string_scratch");
    self->string_scratch_arena = self->string_scratch_storage;
    *resolved                  = (n00b_allocator_t *)self->string_scratch_arena;
    return (n00b_string_scope_t){.created = true};
}

n00b_string_t *
n00b_string_scope_exit(n00b_string_scope_t *scope, n00b_string_t *result)
{
    if (!scope->created) {
        return result;
    }
    /* Copy the result OUT of the scratch into the runtime default
     * allocator before the scratch dies.  Use the lowest-level
     * @ref n00b_alloc_with_opts primitives so this copy does NOT
     * re-enter the string-builder API and recurse back through
     * @ref n00b_string_scope_enter (which would create another
     * scratch, copy again, and infinite-loop).  Layout matches
     * @c struct n00b_string_t verbatim — char *data, size_t
     * u8_bytes, size_t codepoints, void *styling.  The .styling
     * pointer is shallow-copied: it lives outside the scratch
     * already (styles are managed by the styling subsystem). */
    n00b_string_t *durable = result;
    if (result != nullptr) {
        /* IMPORTANT: durable must NOT be no_scan. n00b_string_t has
         * pointer fields (.data, .styling) that the copying GC needs
         * to update when their backing buffers are forwarded into
         * to-space. The earlier @c .no_scan=true flag here caused
         * post-collection dangling pointers, surfacing as SIGSEGV in
         * @c wax_text_has_value reading freed bytes via the stale
         * @c .data pointer. The char-buffer beneath is still no_scan
         * — that's correct, it holds raw bytes, no pointers. */
        durable
            = n00b_alloc_with_opts(n00b_string_t, &(n00b_alloc_opts_t){.allocator = nullptr});
        durable->u8_bytes   = result->u8_bytes;
        durable->codepoints = result->codepoints;
        durable->styling    = result->styling;
        if (result->data != nullptr && result->u8_bytes > 0) {
            char *bytes = n00b_alloc_array_with_opts(
                char,
                result->u8_bytes + 1,
                &(n00b_alloc_opts_t){.allocator = nullptr, .no_scan = true});
            memcpy(bytes, result->data, result->u8_bytes);
            bytes[result->u8_bytes] = '\0';
            durable->data           = bytes;
        }
        else {
            /* Empty-string case: result->data was either a static
             * literal (e.g. "" from @ref n00b_string_empty) or a
             * scratch-pool allocation. Static is safe to point at
             * directly; scratch would be freed below. Resolve to a
             * known-safe empty string via a fresh 1-byte allocation. */
            char *bytes = n00b_alloc_array_with_opts(
                char,
                1,
                &(n00b_alloc_opts_t){.allocator = nullptr, .no_scan = true});
            bytes[0]      = '\0';
            durable->data = bytes;
        }
    }
    /* Tear down the scratch via the same per-thread home that
     * n00b_string_scope_enter created it in.  scope->created is true
     * only when enter resolved a registered thread, so self is that
     * same thread here; guard defensively anyway. */
    n00b_thread_t *self = n00b_thread_self();
    n00b_pool_t   *pool = (self != nullptr) ? self->string_scratch_arena : nullptr;
    if (self != nullptr) {
        self->string_scratch_arena = nullptr;
    }
    if (pool != nullptr) {
        n00b_allocator_destroy((n00b_allocator_t *)pool);
    }
    return durable;
}

void
n00b_string_init(n00b_string_t *self) _kargs
{
    const char         *src       = nullptr;
    int64_t             byte_len  = -1;
    n00b_allocator_t   *allocator = nullptr;
    int64_t            *cp_count  = nullptr;
    n00b_gc_scan_kind_t scan_kind = N00B_GC_SCAN_KIND_NONE;
    n00b_gc_scan_cb_t   scan_cb   = nullptr;
    void               *scan_user = nullptr;
    const char         *alloc_site = nullptr;
}
{
    (void)src;
    (void)byte_len;
    (void)allocator;
    (void)cp_count;
    (void)scan_kind;
    (void)scan_cb;
    (void)scan_user;
    (void)alloc_site;

    n00b_ensure_allocator(kargs->allocator);

    int64_t len = kargs->byte_len;

    if (len < 0 && kargs->src) {
        const char *p = kargs->src;
        while (*p) {
            p++;
        }
        len = (int64_t)(p - kargs->src);
    }
    else if (len < 0) {
        len = 0;
    }

    self->data = n00b_alloc_array_with_opts(char,
                                            (size_t)len + 1,
                                            &(n00b_alloc_opts_t){
                                                .allocator  = kargs->allocator,
                                                .scan_kind  = N00B_GC_SCAN_KIND_NONE,
                                                .alloc_site = kargs->alloc_site,
                                            });

    if (len > 0 && kargs->src) {
        memcpy(self->data, kargs->src, (size_t)len);
    }
    self->data[len]  = '\0';
    self->u8_bytes   = len;
    self->codepoints = n00b_unicode_utf8_count_codepoints_raw(kargs->src, (uint32_t)len);

    if (kargs->cp_count) {
        *kargs->cp_count = (int64_t)self->codepoints;
    }
}

n00b_string_t *
n00b_string_from_raw(const char *src, int64_t byte_len) _kargs
{
    n00b_allocator_t *allocator  = nullptr;
    int64_t          *cp_count   = nullptr;
    const char       *alloc_site = nullptr;
}
{
    (void)cp_count;
    (void)alloc_site;
    n00b_allocator_t *resolved_allocator = allocator;

    if (resolved_allocator == nullptr) {
        n00b_thread_t *self = n00b_thread_self();
        if (self != nullptr && self->record != nullptr &&
            self->string_scratch_arena != nullptr) {
            resolved_allocator = (n00b_allocator_t *)self->string_scratch_arena;
        }
    }

    n00b_ensure_allocator(resolved_allocator);

    n00b_string_t *result
        = n00b_alloc_with_opts(n00b_string_t,
                               &(n00b_alloc_opts_t){.allocator  = resolved_allocator,
                                                    .alloc_site = kargs->alloc_site},
                               n00b_kargs(string,
                                          .src        = src,
                                          .byte_len   = byte_len,
                                          .allocator  = resolved_allocator,
                                          .cp_count   = kargs->cp_count,
                                          .alloc_site = kargs->alloc_site));
    return result;
}

n00b_string_t *
_n00b_string_from_raw_at(const char *src,
                         int64_t     byte_len,
                         const char *alloc_location) _kargs
{
    n00b_allocator_t *allocator = nullptr;
    int64_t          *cp_count  = nullptr;
}
{
    (void)cp_count;
    n00b_string_t *result = n00b_string_from_raw(src,
                                                byte_len,
                                                .allocator  = kargs->allocator,
                                                .cp_count   = kargs->cp_count,
                                                .alloc_site = alloc_location);
    return result;
}

n00b_string_t *
n00b_ncc_rstr(const char *src)
{
    return n00b_string_from_cstr(src);
}

n00b_string_t *
n00b_string_from_cstr(const char *src) _kargs
{
    n00b_allocator_t *allocator  = nullptr;
    const char       *alloc_site = nullptr;
}
{
    (void)allocator;
    (void)alloc_site;
    if (!src) {
        return n00b_string_empty(.allocator = kargs->allocator);
    }

    const char *p = src;
    while (*p) {
        p++;
    }
    int64_t byte_len = (int64_t)(p - src);

    return n00b_string_from_raw(src,
                                byte_len,
                                .allocator  = kargs->allocator,
                                .alloc_site = kargs->alloc_site);
}

n00b_string_t *
_n00b_string_from_cstr_at(const char *src, const char *alloc_location) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    (void)allocator;
    n00b_string_t *result = n00b_string_from_cstr(src,
                                                  .allocator  = kargs->allocator,
                                                  .alloc_site = alloc_location);
    return result;
}

n00b_string_t *
n00b_string_empty() _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    (void)allocator;
    return n00b_string_from_raw("", 0, .allocator = kargs->allocator);
}

n00b_string_t *
_n00b_string_empty_at(const char *alloc_location) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    (void)allocator;
    return n00b_string_from_raw("",
                                0,
                                .allocator  = kargs->allocator,
                                .alloc_site = alloc_location);
}
