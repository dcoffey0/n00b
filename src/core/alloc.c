#include <stdio.h>
#include "n00b.h"
#include "core/codegen_abi.h" // n00b_gc_struct_layout_t, scan_cb externs
#include "core/alloc.h"
#include "core/arena.h"
#include "core/alloc_mdata.h"
#include "adt/dict.h"
#include "adt/dict_untyped.h"
#include "core/mmaps.h"
#include "core/memory_info.h"
#include "core/rwlock.h"
#include "core/pool.h"
#include "core/runtime.h"
#include "core/type_info.h"
#include "core/data_lock.h"
#include "core/gc_baked.h"
#include "util/assert.h"
#include "core/thread.h"
#include "core/epoch.h"
#include "core/stw.h"
#include "core/syscall.h"
#include <stdlib.h>
#include "core/gc.h"

#ifndef N00B_METADATA_START_ENTRIES
#define N00B_METADATA_START_ENTRIES 1 << 12
#endif

extern uint64_t         n00b_gc_guard;

_Atomic(uint64_t)       n00b_max_inline_alloc_len = 0;
const n00b_alloc_opts_t _n00b_default_alloc_opts = {};
static void             n00b_run_and_remove_finalizers(void *ptr);

// Every external-metadata allocator stores its dict in an attached metadata
// arena. Any dict access must drain before STW may inspect or swap that arena.
static inline bool
n00b_allocator_metadata_needs_stw_gate(n00b_allocator_t *al)
{
    n00b_runtime_t *rt = n00b_get_runtime();

    return al && al->metadata_pool != nullptr && !al->__system
        && rt != nullptr && rt->critical_execution.inited
        && !n00b_atomic_load(&rt->stw_active);
}

static inline void
n00b_metadata_gate_unlock(bool unlock_gate)
{
    if (unlock_gate) {
        n00b_rw_unlock(&n00b_get_runtime()->critical_execution);
    }
}

void
n00b_alloc_hdr_unpin(bool pinned)
{
    n00b_metadata_gate_unlock(pinned);
}

#if defined(N00B_DEBUG)
void (*n00b_alloc_hdr_lookup_hook)(void *addr) = nullptr;

// A thread without a TCB has no read log, so whether it holds the gate cannot
// be checked; only a thread with one is held to it.
static bool
alloc_hdr_use_is_pinned(void)
{
    n00b_thread_t *self = n00b_thread_self();

    if (self == nullptr || self->record == nullptr) {
        return true;
    }
    return n00b_rw_held_by_self(&n00b_get_runtime()->critical_execution);
}
#endif

#include "core/oob_md_dict.h"

static inline n00b_oob_hdr_t *
n00b_oob_for_user_ptr_held(void *ptr, bool *unlock_gate)
{
    if (unlock_gate != nullptr) {
        *unlock_gate = false;
    }

    n00b_allocator_opt_t alloc_opt = n00b_mem_get_allocator(ptr);
    if (!n00b_option_is_set(alloc_opt)) {
        return nullptr;
    }

    n00b_allocator_t *allocator = n00b_option_get(alloc_opt);
    if (allocator == nullptr || allocator->metadata == nullptr) {
        return nullptr;
    }

    bool md_stw = n00b_allocator_metadata_needs_stw_gate(allocator);
    if (md_stw) {
        n00b_rw_read_lock(&n00b_get_runtime()->critical_execution);
        if (unlock_gate != nullptr) {
            *unlock_gate = true;
        }
    }

    return n00b_md_get(allocator->metadata, ptr);
}

static n00b_allocator_t *
n00b_new_metadata_pool(const char *creation_loc)
{
    // Hidden, not-mapped metadata pool. hidden + not mmap-registered keeps
    // it out of the GC scan and the mmap tree (so the GC can't trace into it and
    // pin every record's referenced allocation); the struct lives in the no_scan
    // system pool for the same reason. Metadata dict mutation is already
    // serialized by the owning allocator's STW gate, and the pool is excluded
    // from epoch lists so GC cannot invalidate retired metadata store nodes.
    n00b_runtime_t *rt   = n00b_get_runtime();
    n00b_pool_t    *pool = n00b_alloc_with_opts(
        n00b_pool_t,
        &(n00b_alloc_opts_t){.allocator = (n00b_allocator_t *)&rt->system_pool,
                                .no_scan   = true});
    return n00b_pool_init_at(pool,
                             .__system     = true,
                             .hidden       = true,
                             .name         = "md_pool",
                             .creation_loc = creation_loc,
                             // The metadata dict is !gc and concurrently read, so
                             // its store epoch-defers on migration (n00b_retire
                             // stamps it, keyed on !gc). Because this pool's stores
                             // land on the epoch retire list, it USES epochs:
                             // use_epochs MUST be true so allocator destroy
                             // (n00b_epoch_drain_allocator) drains this pool's
                             // retired nodes before its pages go away.
                             //
                             // NO inline headers: the epoch_hdr n00b_epoch_alloc
                             // prepends is the only header these stores need, and
                             // header-none puts it right at the allocation base so
                             // n00b_retire (user_ptr - sizeof(epoch_hdr)) and the
                             // free path land exactly on the base. Adding an inline
                             // header shifts the epoch_hdr off the base, and the
                             // free/free-list base math then lands at the wrong
                             // offset -> corrupt retire list -> SIGSEGV.
                             .use_epochs   = true,
                             // Marks vtable.is_metadata: metadata pools defer
                             // their teardown to the next stop-the-world (see
                             // n00b_allocator_destroy) because dict-store
                             // migration retires their stores lock-free from
                             // any thread — an inline drain always races.
                             .__is_md_pool = true);
}

#if defined(N00B_DEBUG)
// Debug-build work counter: finalizer registry entries a bare n00b_free
// examined looking for its object's finalizer.
_Atomic uint64_t n00b_finalizer_registry_probes = 0;
#endif

// rt->finalizers is mutated only under the critical_execution read gate, so a
// collection, which takes the write side before it suspends anyone, never sees
// it mid-update. Callers already running with the world stopped skip the gate.
static inline bool
n00b_finalizer_gate_lock(n00b_runtime_t *rt)
{
    if (!rt->critical_execution.inited || n00b_atomic_load(&rt->stw_active)) {
        return false;
    }
    n00b_rw_read_lock(&rt->critical_execution);
    return true;
}

void
n00b_finalizer_index_init(n00b_runtime_t *rt)
{
    rt->finalizer_pool = n00b_new_metadata_pool(N00B_LOC_STRING());
    rt->finalizers     = n00b_alloc_with_opts(
        _n00b_dict_internal_t,
        &(n00b_alloc_opts_t){.allocator = rt->finalizer_pool});
    _n00b_dict_internal_init(rt->finalizers,
                             N00B_MD_KSZ,
                             N00B_MD_VSZ,
                             typehash(void *),
                             typehash(n00b_finalizer_info_t *),
                             .allocator     = rt->finalizer_pool,
                             .hash          = n00b_hash_word,
                             .skip_obj_hash = true,
                             .scan_kind     = N00B_GC_SCAN_KIND_NONE);
    n00b_atomic_store(&rt->finalizer_count, 0);
}

// Removes and returns the index entry for `key`. Caller holds the gate.
static n00b_finalizer_info_t *
n00b_finalizer_index_take(n00b_runtime_t *rt, void *key)
{
    bool                   found = false;
    n00b_finalizer_info_t *info  = nullptr;
    void                  *slot  = _n00b_dict_internal_get(rt->finalizers,
                                                     N00B_MD_KSZ,
                                                     N00B_MD_VSZ,
                                                     &key,
                                                     &info,
                                                     &found);
    if (slot == nullptr || !found) {
        return nullptr;
    }
    info = *(n00b_finalizer_info_t **)slot;
    (void)_n00b_dict_internal_remove(rt->finalizers, N00B_MD_KSZ, N00B_MD_VSZ, &key);
    atomic_fetch_sub_explicit(&rt->finalizer_count, 1, memory_order_acq_rel);
    return info;
}

// Attaches fn(user) to `obj` in the index, replacing any finalizer it had.
static void
n00b_finalizer_index_put(n00b_runtime_t *rt, void *obj, n00b_finalizer_t fn, void *user)
{
    n00b_allocator_t      *sp   = (n00b_allocator_t *)&rt->system_pool;
    n00b_finalizer_info_t *info = n00b_alloc_with_opts(n00b_finalizer_info_t,
                                                       &(n00b_alloc_opts_t){.allocator = sp});

    bool                   gate = n00b_finalizer_gate_lock(rt);
    n00b_finalizer_info_t *old  = n00b_finalizer_index_take(rt, obj);

    *info = (n00b_finalizer_info_t){
        .funcptr  = fn,
        .key      = obj,
        .user_ptr = user,
    };
    (void)_n00b_dict_internal_put(rt->finalizers, N00B_MD_KSZ, N00B_MD_VSZ, &obj, &info);
    n00b_atomic_add(&rt->finalizer_count, 1);
    if (gate) {
        n00b_rw_unlock(&rt->critical_execution);
    }

    if (old != nullptr) {
        n00b_free(old, .allocator = sp);
    }
}

static uint32_t
n00b_metadata_start_capacity(uint64_t records)
{
    uint64_t result = records * 2u;
    uint64_t max_u32 = (uint64_t)(uint32_t)-1;

    if (result < N00B_METADATA_START_ENTRIES) {
        result = N00B_METADATA_START_ENTRIES;
    }
    if (result > max_u32) {
        result = max_u32;
    }

    return (uint32_t)result;
}


// The scoped allocator override is now a per-thread field reached via
// n00b_thread_self() (D-005), not a thread_local.  Before the runtime /
// calling thread is registered, self() is nullptr; the getter then
// reports "no override" and the setters/restores become no-ops, matching
// the startup-incomplete-window handling in src/core/data_lock.c.
n00b_allocator_t *
n00b_current_allocator(void)
{
    n00b_thread_t *self = n00b_thread_self();
    if (self != nullptr) {
        return self->current_allocator;
    }
    // No resolvable thread context.  This happens when code runs on a thread's
    // signal ALTSTACK (e.g. the crash handler): the altstack belongs to no
    // registered thread, so n00b_thread_self() returns null.  The GC's
    // collectable default arena is UNSAFE here -- an allocation that triggers a
    // collection runs n00b_collect on the altstack, where its own
    // n00b_thread_self() is also null and it derefs a null self.  Fall back to
    // the non-collecting, always-valid, non-moving system pool so allocation in
    // this context can never trip a GC collect.  (Before the runtime exists,
    // n00b_thread_self() returns the bootstrap thread, not null, so this path is
    // only reached post-init, where n00b_get_runtime() is valid.)
    n00b_runtime_t *rt = n00b_default_runtime_or_null();
    return rt == nullptr ? nullptr : (n00b_allocator_t *)&rt->system_pool;
}

#if defined(N00B_GC_ATTRIB)
// MEASUREMENT (opt-in via -DN00B_GC_ATTRIB): cumulative bytes allocated into
// the GC default arena, split by whether the allocating thread is inside rocs
// ingest. Observational only; the enter/exit/bytes API is a zero-cost set of
// empty inlines (see alloc.h) when the flag is off.
_Atomic uint64_t n00b_gc_attrib_ingest_bytes_v = 0;
_Atomic uint64_t n00b_gc_attrib_other_bytes_v  = 0;

bool
n00b_gc_attrib_enter_ingest(void)
{
    n00b_thread_t *self = n00b_thread_self();
    if (self == nullptr) {
        return false;
    }
    bool prev            = self->in_rocs_ingest;
    self->in_rocs_ingest = true;
    return prev;
}

void
n00b_gc_attrib_exit_ingest(bool prev)
{
    n00b_thread_t *self = n00b_thread_self();
    if (self != nullptr) {
        self->in_rocs_ingest = prev;
    }
}

uint64_t
n00b_gc_attrib_ingest_bytes(void)
{
    return atomic_load_explicit(&n00b_gc_attrib_ingest_bytes_v, memory_order_relaxed);
}

uint64_t
n00b_gc_attrib_other_bytes(void)
{
    return atomic_load_explicit(&n00b_gc_attrib_other_bytes_v, memory_order_relaxed);
}
#endif // N00B_GC_ATTRIB

n00b_allocator_t *
n00b_set_current_allocator(n00b_allocator_t *allocator)
{
    n00b_thread_t *self = n00b_thread_self();
    if (self == nullptr) {
        return nullptr;
    }

    n00b_allocator_t *previous = self->current_allocator;
    self->current_allocator    = allocator;
    return previous;
}

void
n00b_restore_current_allocator(n00b_allocator_t *previous)
{
    n00b_thread_t *self = n00b_thread_self();
    if (self == nullptr) {
        return;
    }
    self->current_allocator = previous;
}

n00b_allocator_t *
n00b_thread_scratch_pool(void)
{
    n00b_thread_t  *self = n00b_thread_self();
    n00b_runtime_t *rt   = n00b_get_runtime();
    // Foreign / not-yet-registered threads (e.g. the gateway's ES-sensor and
    // pipeline threads, where n00b_thread_self() has no per-thread record) have
    // nowhere to hang a per-thread pool.  Fall back to the shared system pool:
    // still non-GC, so n00b_free reclaims the transient instead of churning the
    // GC arena.  Registered threads get their own pool (no cross-thread
    // contention).
    if (self == nullptr || self->record == nullptr) {
        return rt == nullptr ? nullptr : (n00b_allocator_t *)&rt->system_pool;
    }
    if (self->scratch_pool == nullptr) {
        // Control struct in the (non-GC) system pool, mirroring the per-thread
        // string scratch.  Holds only raw, explicitly-freed buffers (no
        // lock-bearing objects), so the lock-chain scrub on destroy is opted
        // out -- same invariant as string_scratch_storage.
        self->scratch_pool = n00b_alloc_with_opts(
            n00b_pool_t,
            &(n00b_alloc_opts_t){.allocator = (n00b_allocator_t *)&rt->system_pool,
                                 .no_scan   = true});
        n00b_pool_init(self->scratch_pool,
                       .hidden                 = true,
                       .scrub_locks_on_destroy = false,
                       .use_epochs             = false,
                       .name                   = "n00b_thread_scratch");
    }
    return (n00b_allocator_t *)self->scratch_pool;
}

n00b_allocator_scope_t
n00b_allocator_scope_enter(n00b_allocator_t *allocator)
{
    return (n00b_allocator_scope_t){
        .previous  = n00b_set_current_allocator(allocator),
        .installed = allocator,
        .active    = true,
        .run       = true,
    };
}

void
n00b_allocator_scope_exit(n00b_allocator_scope_t *scope)
{
    if (!scope || !scope->active) {
        return;
    }

#ifdef N00B_DEBUG
    n00b_thread_t *self = n00b_thread_self();
    n00b_assert(self == nullptr || self->current_allocator == scope->installed);
#endif

    n00b_restore_current_allocator(scope->previous);
    scope->active = false;
}

static inline void
n00b_alloc_add_inline_header(n00b_inline_hdr_t **hdrp,
                             size_t              alloc_len,
                             uint32_t            ptr_words,
                             bool                ptr_words_known,
                             uint64_t            type_hash,
                             bool                is_array,
                             bool                no_scan,
                             bool                mem_debug,
                             bool                mem_debug_taint,
                             n00b_gc_scan_kind_t scan_kind,
                             n00b_gc_scan_cb_t   scan_cb,
                             void               *scan_user)
{
    n00b_inline_hdr_t *hdr = *hdrp;
    assert(alloc_len >= sizeof(n00b_inline_hdr_t));

    // Record the largest inline-header allocation we have ever made. This is
    // the only site that writes a guard, so it is the only place the bound on
    // the backward guard scan can be kept honest (n00b#321). Monotonic, so a
    // racing update can only raise it -- never below a live allocation.
    uint64_t prev_max = n00b_atomic_load(&n00b_max_inline_alloc_len);
    while (alloc_len > prev_max) {
        if (atomic_compare_exchange_weak(&n00b_max_inline_alloc_len,
                                         &prev_max,
                                         (uint64_t)alloc_len)) {
            break;
        }
    }

    *hdr = (n00b_inline_hdr_t){
        .guard           = n00b_gc_guard,
        .tinfo           = type_hash,
        .alloc_len       = alloc_len,
        .ptr_words       = ptr_words,
        .ptr_words_known = ptr_words_known,
        .is_array        = is_array,
        .no_scan         = no_scan,
        .mem_debug       = mem_debug,
        .mem_debug_taint = mem_debug_taint,
        // The collector reads the scan shape from this header for every
        // inline-header allocation (n00b_add_alloc_to_worklist, and the
        // forwarded copy in n00b_forward_alloc). Until n00b-lang/n00b#365
        // these three were never written, so every such allocation scanned as
        // DEFAULT no matter what the caller asked for.
        .scan_kind       = scan_kind,
        .scan_cb         = scan_cb,
        .scan_user       = scan_user,
    };

    *hdrp = ++hdr;
}

void *
_n00b_alloc_raw(size_t             n,
                size_t             sz,
                uint64_t           type_hash,
                const char        *location,
                n00b_alloc_opts_t *opts,
                +) _kargs: opaque
{
    n00b_inline_hdr_t *hdr      = nullptr;
    n00b_oob_hdr_t    *map_item = nullptr;
    n00b_alloc_opts_t  local_opts;

    if (!opts) {
        local_opts = _n00b_default_alloc_opts;
        opts       = &local_opts;
    }

    n00b_ensure_allocator(opts->allocator);

    // No cooperative STW check-in on the alloc hot path (WP-001): the collector
    // preempts mutators (it does not wait for them to self-park at an
    // allocation), so an allocating thread no longer needs to poll for a stop.

    /* D-049: upgrade a DEFAULT-scanned typed allocation to a precise
     * CALLBACK scan when a link-time GC-map descriptor is registered for
     * its type. Only when the caller specified no scan policy of its own
     * (DEFAULT + no scan_cb) and the allocator has somewhere to keep the
     * callback: an OOB record or an inline header (the collector reads the
     * shape from either; see the CALLBACK fallback below). Until
     * n00b-lang/n00b#309 this required OOB metadata, so every typed object
     * in an inline-header pool (rt->user_pool) stayed on the conservative
     * DEFAULT scan even in a binary that carried the dictionary, and any
     * non-pointer word of it that aliased a from-space address was rewritten
     * by the collector (#368). The descriptor's element count is derived from
     * the allocation length by n00b_gc_scan_cb_type_layout, so one shared
     * per-type descriptor serves both n (=1) and arrays. */
    if (opts->scan_kind == N00B_GC_SCAN_KIND_DEFAULT
        && opts->scan_cb == nullptr
        && type_hash != 0
        && (opts->allocator->metadata_pool != nullptr
            || opts->allocator->add_inline_header)) {
        const n00b_gc_struct_layout_t *layout = n00b_gc_type_map_lookup(type_hash);
        if (layout != nullptr) {
            opts->scan_kind = N00B_GC_SCAN_KIND_CALLBACK;
            opts->scan_cb   = n00b_gc_scan_cb_type_layout;
            opts->scan_user = (void *)layout;
        }
    }

    /* Map scan_kind == NONE onto the legacy no_scan switch so the GC's
     * worklist add-path (which checks no_scan) skips scanning this
     * allocation. */
    if (opts->scan_kind == N00B_GC_SCAN_KIND_NONE) {
        opts->no_scan = true;
    }
    /* CALLBACK needs somewhere to keep scan_cb / scan_user that the collector
     * will find again: the OOB record, or the inline header (which the
     * forwarding copy carries along). An allocator with neither cannot honour
     * the request; fall back to the conservative DEFAULT scan instead of
     * aborting the process. (Inline headers qualified all along -- the
     * collector has always read the shape from them -- but the allocator only
     * started writing it there with n00b-lang/n00b#365.) */
    if (opts->scan_kind == N00B_GC_SCAN_KIND_CALLBACK
        && opts->allocator->metadata_pool == nullptr
        && !opts->allocator->add_inline_header) {
        opts->scan_kind = N00B_GC_SCAN_KIND_DEFAULT;
        opts->scan_cb   = nullptr;
        opts->scan_user = nullptr;
    }

    uint64_t request  = n * sz;
    uint64_t user_words = request / sizeof(void *);
    n00b_require(user_words <= (uint64_t)(uint32_t)-1,
                 "allocation logical pointer words exceed metadata capacity");
    uint32_t ptr_words = (uint32_t)user_words;
    bool     is_array = n > 1;

    if (opts->allocator->add_inline_header) {
        request += N00B_ALLOC_HDR_SZ;
    }

#if defined(N00B_GC_ATTRIB)
    // MEASUREMENT (opt-in via -DN00B_GC_ATTRIB): attribute allocations into the
    // GC default arena to "consumer" (inside rocs ingest) vs "other". This runs
    // n00b_get_runtime()/n00b_thread_self() + an atomic on the hot path, so it
    // perceptibly slows ingest -- never enable it in a shipping build.
    {
        n00b_runtime_t *attrib_rt = n00b_get_runtime();
        if (attrib_rt != nullptr
            && opts->allocator == (n00b_allocator_t *)attrib_rt->default_arena) {
            n00b_thread_t *attrib_self = n00b_thread_self();
            if (attrib_self != nullptr && attrib_self->in_rocs_ingest) {
                atomic_fetch_add_explicit(&n00b_gc_attrib_ingest_bytes_v,
                                          request,
                                          memory_order_relaxed);
            }
            else {
                atomic_fetch_add_explicit(&n00b_gc_attrib_other_bytes_v,
                                          request,
                                          memory_order_relaxed);
            }
        }
    }
#endif

    void *r;

    if (!request) {
        request = opts->allocator->add_inline_header ? sizeof(n00b_inline_hdr_t) : 1;
    }

    request = n00b_align(request);

#ifdef N00B_POOL_ALLOC_AUDIT
    // Debug-only: for pools under per-site audit, reserve one trailing aligned
    // word to stash the allocation-site string. `request` (hence the inline
    // header's alloc_len) grows by exactly N00B_ALIGN, so the free hook can
    // recover the site at base + alloc_len - N00B_ALIGN in O(1). Gated on the
    // SAME predicate the audit hook uses, so the two always agree on layout.
    bool n00b_alloc_audited = n00b_pool_alloc_audit_enabled(opts->allocator);
    if (n00b_alloc_audited) {
        request += N00B_ALIGN;
    }
#endif

    // Currently never pass parameters to the allocator. For future use.
    r = (*opts->allocator->zero_alloc)(opts->allocator, request, nullptr);
#ifdef N00B_POOL_ALLOC_AUDIT
    // n00b_alloc_audited also controls the trailing site-word layout above.
    // A caller-supplied alloc_site overrides the innermost N00B_LOC_STRING()
    // capture so per-site audit traces back to the real instantiation.
    if (n00b_alloc_audited) {
        n00b_system_pool_audit_alloc(opts->allocator,
                                     r,
                                     request,
                                     opts->alloc_site ? opts->alloc_site
                                                      : location);
    }
#endif

    if (opts->allocator->add_inline_header) {
        hdr = r;

        n00b_alloc_add_inline_header((n00b_inline_hdr_t **)&r,
                                     request,
                                     ptr_words,
                                     true,
                                     type_hash,
                                     n > 1,
                                     opts->no_scan,
                                     opts->mem_debug,
                                     opts->debug_taint,
                                     opts->scan_kind,
                                     opts->scan_cb,
                                     opts->scan_user);
    }

    if (opts->allocator->metadata_pool != nullptr) {
        bool md_stw = n00b_allocator_metadata_needs_stw_gate(opts->allocator);
        if (md_stw) {
            n00b_rw_read_lock(&n00b_get_runtime()->critical_execution);
        }

        n00b_alloc_opts_t md_opts = {.allocator = opts->allocator->metadata_pool};
        // Allocators that reserve per-alloc OOB flex-tail bytes (e.g.
        // .alloc_refcount) grow the record by oob_extra_size. The metadata pool
        // is hidden/use_gc=false and its records are scanned structurally (by
        // the mark walk reading OOB fields), not via tinfo, so the flex form's
        // type_hash=0 is safe here. The zero_alloc leaves the flex tail zeroed;
        // seeding by value below writes only sizeof(n00b_oob_hdr_t), never the tail.
        uint32_t oob_extra = opts->allocator->oob_extra_size;
        if (oob_extra != 0) {
            map_item = n00b_alloc_flex_with_opts(n00b_oob_hdr_t,
                                                 uint8_t,
                                                 oob_extra,
                                                 &md_opts);
        }
        else {
            map_item = n00b_alloc_with_opts(n00b_oob_hdr_t, &md_opts);
        }

        /* Seed the OOB liveness state. `alive` flags this slot as
         * handed out so the GC mark/sweep treats it as a root and a
         * non-leak. The epoch is stamped to the runtime's current
         * value so a collection running between this alloc and the
         * caller's first use cannot misclassify it as stale. */
        uint64_t epoch_now = 0;
        {
            n00b_runtime_t *rt = n00b_get_runtime();
            if (rt) {
                epoch_now = n00b_atomic_load(&rt->gc_current_epoch);
            }
        }

        *map_item = (n00b_oob_hdr_t){
            .user_ptr        = r,
            .tinfo           = type_hash,
            .alloc_len       = request,
            .ptr_words       = ptr_words,
            .ptr_words_known = true,
            .is_array        = n > 1,
            .no_scan         = opts->no_scan,
            .mem_debug       = opts->mem_debug,
            .mem_debug_taint = opts->debug_taint,
            .scan_kind       = opts->scan_kind,
            .scan_cb         = opts->scan_cb,
            .scan_user       = opts->scan_user,
            .hcur            = hdr,
            .file_name       = opts->alloc_site ? opts->alloc_site : location,
            .gc_epoch        = epoch_now,
            .alive           = 1,
            .finalizer       = opts->finalizer,
            .finalizer_user  = opts->finalizer_data,
        };

        n00b_md_put(opts->allocator->metadata, r, map_item);
        if (opts->finalizer) {
            n00b_atomic_store(&opts->allocator->oob_finalizers, true);
        }
        assert(n00b_md_get(opts->allocator->metadata, r) == map_item);
        if (md_stw) {
            n00b_rw_unlock(&n00b_get_runtime()->critical_execution);
        }
    }

    // The object is now registered (inline header written and/or OOB record
    // inserted), so clear this thread's in-flight allocation reservation: the
    // collector can discover the object normally from here on, so its page no
    // longer needs the pin-pre-pass safety net (see n00b_arena_alloc + the pin
    // pre-pass).  Cheap relaxed store; the matching publish was in n00b_arena_alloc.
    n00b_thread_t *self_for_finalizers = n00b_thread_self();
    {
        n00b_thread_t *_self = self_for_finalizers;
        if (_self != nullptr
            && n00b_atomic_load(&_self->gc_inflight_len) != 0) {
            // Mirror image of the publish in n00b_arena_alloc: `start` is the
            // gate, so RETRACT it first (release), then drop the length.  The
            // reverse order would leave a window where a collector reads a
            // still-valid `start` against a zeroed `len` and drops the pin
            // (n00b#431).
            atomic_store_explicit(&_self->gc_inflight_start, nullptr,
                                  memory_order_release);
            atomic_store_explicit(&_self->gc_inflight_len, 0,
                                  memory_order_relaxed);
        }
    }

    // If the allocator has no headers and no metadata but is visible to
    // the GC (non-hidden), register the allocation in the range tree so
    // the collector's fallback path can discover and scan it.
    if (!opts->allocator->hidden && !opts->allocator->add_inline_header
        && opts->allocator->metadata_pool == nullptr
        && n00b_default_runtime_is_set()) {
        n00b_mmap_register_range(r,
                                 (char *)r + request,
                                 n00b_mmap_pool,
                                 .allocator = opts->allocator,
                                 .scan_kind = opts->scan_kind,
                                 .scan_cb   = opts->scan_cb,
                                 .scan_user = opts->scan_user);
    }

    assert(!(((uint64_t)r) & (N00B_ALIGN - 1)));

    // Dispatch the vtable constructor if the type is registered.
    // Guard on startup_complete to avoid early-init lookups.
    //
    // Constructor dispatch depends on type_info flags:
    //   !ctor_takes_kargs && !ctor_takes_vargs: ctor(self)
    //   ctor_takes_kargs  && !ctor_takes_vargs: ctor(self, kargs_ptr)
    //   ctor_takes_vargs:                       ctor(self, vargs, kargs_ptr)
    //
    // For kargs/vargs constructors, the data comes from vargs packed
    // by n00b_new_kargs / n00b_new_both.  If no vargs were provided,
    // kargs/vargs constructors are skipped (use n00b_new_kargs to
    // trigger construction, not bare n00b_alloc).
    if (!is_array && type_hash && n00b_default_runtime_is_set()
        && n00b_get_runtime()->startup_complete) {
        auto tinfo_opt = n00b_type_lookup(type_hash);

        if (n00b_option_is_set(tinfo_opt)) {
            n00b_type_info_t *tinfo = n00b_option_get(tinfo_opt);
            n00b_vtable_entry ctor  = tinfo->core_vtable[N00B_BI_CONSTRUCTOR];
            if (ctor) {
                bool have_vargs = vargs && vargs->nargs > 0;

                if (tinfo->ctor_takes_vargs && have_vargs) {
                    // ctor(self, vargs, kargs).
                    // n00b_new_both packs: real_varg0, ..., kargs_ptr
                    // kargs is always the last varg.
                    void *ctor_kargs = vargs->args[vargs->nargs - 1];
                    vargs->nargs--;
                    ((void (*)(void *, n00b_vargs_t *, void *))ctor)(
                        r, vargs, ctor_kargs);
                }
                else if (tinfo->ctor_takes_kargs && !tinfo->ctor_takes_vargs) {
                    // ctor(self, kargs).
                    // kargs comes from the opaque _kargs parameter.
                    if (kargs) {
                        ((void (*)(void *, void *))ctor)(r, kargs);
                    }
                }
                else if (!tinfo->ctor_takes_kargs && !tinfo->ctor_takes_vargs) {
                    // ctor(self) — always dispatched (no data needed).
                    ((void (*)(void *))ctor)(r);
                }
            }
        }
    }

    // An OOB record was armed with the finalizer above, under the metadata
    // gate. Anything else goes in the runtime index.
    if (opts->finalizer && opts->allocator->metadata_pool == nullptr) {
        n00b_runtime_t *rt = n00b_get_runtime();
        if (rt && rt->finalizers) {
            n00b_finalizer_index_put(rt, r, opts->finalizer, opts->finalizer_data);
        }
    }

    // This allocation's own out-of-memory collect queued finalizers; the
    // object is fully registered now, so run them before returning.
    if (self_for_finalizers != nullptr && self_for_finalizers->gc_run_finalizers) {
        self_for_finalizers->gc_run_finalizers = false;
        n00b_gc_run_finalizers();
    }

    return r;
}

void
n00b_allocator_setup(n00b_allocator_t *allocator, n00b_calloc_fn alloc) _kargs
{
    n00b_free_fn              free              = nullptr;
    n00b_allocator_pre_destroy_fn pre_destroy   = nullptr;
    n00b_allocator_destroy_fn destroy           = nullptr;
    const char               *name              = nullptr;
    bool                      inline_headers    = true;
    bool                      external_metadata = true;
    // RISKY for custom allocators. Hides from GC.
    bool                      hidden            = false;
    // DO NOT USE for custom allocators. Skips mmaps.
    bool                      __nomap           = false;
    // DO NOT USE for custom allocators. Skips STW check.
    bool                      __system          = false;
    bool                      use_epochs        = false;
    bool                      __is_md_pool      = false;
    // "file:line" of the create-site (via N00B_LOC_STRING()); stored in the
    // vtable for the mmap histogram. Defaults to nullptr for ad-hoc allocators.
    const char               *creation_loc      = nullptr;
}
{
    (void)__nomap;
    n00b_allocator_t *md_pool = nullptr;

    if (external_metadata) {
        md_pool = n00b_new_metadata_pool(creation_loc);
    }

    _n00b_dict_internal_t *md = nullptr;

    if (external_metadata) {
        md = n00b_alloc_with_opts(_n00b_dict_internal_t, &(n00b_alloc_opts_t){.allocator = md_pool});
    }

    *allocator = (n00b_allocator_t){
        .zero_alloc        = alloc,
        .free              = free,
        .pre_destroy       = pre_destroy,
        .destroy           = destroy,
        .debug_name        = name,
        .add_inline_header = inline_headers,
        .__system          = __system,
        .hidden            = hidden,
        .use_epochs        = use_epochs,
        .is_metadata       = __is_md_pool,
        .metadata_pool     = md_pool,
        .metadata          = md,
        .creation_loc      = creation_loc,
    };

    if (external_metadata) {
        // Typed dict, key = void* (alloc addr), value = n00b_oob_hdr_t*. The
        // backing pool is hidden (use_gc=false), so the key/value arrays are not
        // GC-scanned — scan_kind NONE, and the element typehashes are immaterial
        // to scanning/marshal here.
        _n00b_dict_internal_init(allocator->metadata,
                                 N00B_MD_KSZ,
                                 N00B_MD_VSZ,
                                 typehash(void *),
                                 typehash(n00b_oob_hdr_t *),
                                 .start_capacity = N00B_METADATA_START_ENTRIES,
                                 .allocator      = allocator->metadata_pool,
                                 .hash           = n00b_hash_word,
                                 .skip_obj_hash  = true,
                                 .scan_kind      = N00B_GC_SCAN_KIND_NONE);

        /* NOTE: external_metadata allocators are deliberately NOT
         * auto-registered in rt->metadata_pools. That list means
         * "treat every alive record as a GC root" (n00b_scan_metadata_pools)
         * — appropriate only for the never-collected "array" pools, of
         * which there are currently none. Registering every
         * external_metadata allocator here pinned GC ARENAS' allocations
         * as roots, defeating arena collection (a moving arena kept ~60%
         * of dead allocs alive; with the metadata-pool root pass skipped
         * it collected to zero). Nothing is registered by default; an
         * array pool that needs root semantics must opt in explicitly. */
    }
}

void
n00b_allocator_compact_metadata(n00b_allocator_t *allocator)
{
    if (allocator == nullptr || allocator->metadata_pool == nullptr
        || allocator->metadata == nullptr) {
        return;
    }

    n00b_runtime_t *rt          = n00b_get_runtime();
    bool            unlock_gate = false;

    if (rt != nullptr && rt->critical_execution.inited
        && !n00b_atomic_load(&rt->stw_active)) {
        n00b_rw_write_lock(&rt->critical_execution);
        unlock_gate = true;
    }

    _n00b_dict_internal_t               *old_md   = allocator->metadata;
    n00b_allocator_t                    *old_pool = allocator->metadata_pool;
    __n00b_internal_type_erased_store_t *store
        = (__n00b_internal_type_erased_store_t *)n00b_atomic_load(&old_md->store);

    if (store == nullptr) {
        if (unlock_gate) {
            n00b_rw_unlock(&rt->critical_execution);
        }
        return;
    }

    // Typed erased store: a bucket is reserved iff hv != 0; key/value live in the
    // parallel keys[]/values[] arrays (not in the bucket).
    uint64_t records = 0;
    for (uint32_t i = 0; i <= store->last_slot; i++) {
        n00b_dict_bucket_t *bucket = &store->buckets[i];
        if (bucket->hv == (n00b_uint128_t)0) {
            continue;
        }
        if (n00b_atomic_load(&bucket->flags) & N00B_HT_FLAG_DELETED) {
            continue;
        }
        n00b_oob_hdr_t *oob = (n00b_oob_hdr_t *)store->values[i];
        if (oob != nullptr && oob->alive) {
            records++;
        }
    }

    n00b_allocator_t      *new_pool = n00b_new_metadata_pool(allocator->creation_loc);
    _n00b_dict_internal_t *new_md
        = n00b_alloc_with_opts(_n00b_dict_internal_t,
                               &(n00b_alloc_opts_t){.allocator = new_pool});

    _n00b_dict_internal_init(new_md,
                             N00B_MD_KSZ,
                             N00B_MD_VSZ,
                             typehash(void *),
                             typehash(n00b_oob_hdr_t *),
                             .start_capacity = n00b_metadata_start_capacity(records),
                             .allocator      = new_pool,
                             .hash           = n00b_hash_word,
                             .skip_obj_hash  = true,
                             .scan_kind      = N00B_GC_SCAN_KIND_NONE);

    for (uint32_t i = 0; i <= store->last_slot; i++) {
        n00b_dict_bucket_t *bucket = &store->buckets[i];
        if (bucket->hv == (n00b_uint128_t)0) {
            continue;
        }
        if (n00b_atomic_load(&bucket->flags) & N00B_HT_FLAG_DELETED) {
            continue;
        }
        n00b_oob_hdr_t *old_oob = (n00b_oob_hdr_t *)store->values[i];
        if (old_oob == nullptr || !old_oob->alive) {
            continue;
        }

        // Preserve the allocator-specific OOB flex tail (e.g. the
        // .alloc_refcount counter): allocate the new record with the same
        // oob_extra_size and copy the tail by value. `*new_oob = *old_oob`
        // copies only sizeof(n00b_oob_hdr_t) and would otherwise drop it.
        uint32_t        oob_extra = allocator->oob_extra_size;
        n00b_oob_hdr_t *new_oob;
        if (oob_extra != 0) {
            new_oob = n00b_alloc_flex_with_opts(n00b_oob_hdr_t,
                                                uint8_t,
                                                oob_extra,
                                                &(n00b_alloc_opts_t){.allocator = new_pool});
            *new_oob = *old_oob;
            memcpy(new_oob->alloc_extra, old_oob->alloc_extra, oob_extra);
        }
        else {
            new_oob  = n00b_alloc_with_opts(n00b_oob_hdr_t,
                                           &(n00b_alloc_opts_t){.allocator = new_pool});
            *new_oob = *old_oob;
        }
        n00b_md_put(new_md, store->keys[i], new_oob);
    }

    allocator->metadata_pool = new_pool;
    allocator->metadata      = new_md;

    n00b_allocator_destroy(old_pool);

    if (unlock_gate) {
        n00b_rw_unlock(&rt->critical_execution);
    }
}

static void
n00b_free_storage_from_allocator(n00b_allocator_t *allocator, void *ptr)
{
    if (allocator == nullptr || ptr == nullptr || allocator->free == nullptr) {
        return;
    }

    // Remove the range-tree entry for headerless non-hidden allocations
    // (the mirror of the registration in _n00b_alloc_raw).
    if (!allocator->hidden && !allocator->add_inline_header
        && allocator->metadata_pool == nullptr) {
        n00b_runtime_t  *rt   = n00b_get_runtime();
        n00b_mmap_ctx_t *mctx = n00b_global_mem_map(rt);
        n00b_mmap_delete_ranges(mctx, (uint64_t)ptr, (uint64_t)ptr + 1);
    }

    /* Tear down the OOB metadata for this allocation before we
     * hand the memory back to the allocator. Three things have to
     * happen, in order, atomically with respect to the GC mark/
     * sweep:
     *
     *   1. clear @c alive — the source of truth for the GC's
     *      "this slot is handed out" view; the metadata-pool sweep
     *      depends on it for leak classification.
     *   2. clear @c finalizer — defangs the order-dependent free
     *      chain that the leak detector's per-bucket sweep can
     *      trigger when both halves of an owned pair (e.g. the wax
     *      payload msg + buffer) are flagged as leaks in the same
     *      pass. Without this, processing the msg's slot would
     *      call its finalizer (= n00b_free(buffer)) on an already-
     *      reclaimed buffer.
     *   3. remove the dict entry. The OOB record lives in the
     *      allocator's attached metadata arena; it is reclaimed when
     *      that metadata arena is compacted/replaced wholesale.
     */
    if (allocator->metadata_pool != nullptr) {
        // Metadata teardown runs under the STW read lock so any destructive
        // metadata-arena rebuild first drains in-flight dict mutation.
        bool md_stw = n00b_allocator_metadata_needs_stw_gate(allocator);
        if (md_stw) {
            n00b_rw_read_lock(&n00b_get_runtime()->critical_execution);
        }
        n00b_oob_hdr_t *oob = n00b_md_get(allocator->metadata, ptr);
        if (oob) {
            oob->alive          = 0;
            oob->finalizer      = nullptr;
            oob->finalizer_user = nullptr;
            (void)n00b_md_remove(allocator->metadata, ptr);
        }
        if (md_stw) {
            n00b_rw_unlock(&n00b_get_runtime()->critical_execution);
        }
    }

    if (allocator->add_inline_header) {
        ptr = (char *)ptr - N00B_ALLOC_HDR_SZ;
    }

    (*allocator->free)(allocator, ptr);
}

void
n00b_free_from_allocator(n00b_allocator_t *allocator, void *ptr)
{
    if (ptr == nullptr) {
        return;
    }

#if defined(N00B_FREE_DISCOVERABILITY_AUDIT)
    /* Contract check only — a FULL global mmap interval-tree search per
     * free. Opt-in audit builds only: always-on, this single require made
     * every hinted free in the system O(mmap tree) — including the GC's
     * per-worklist-item frees, epoch node reclaim, and pool teardown — and
     * dominated collect pauses (0.1s collects became minutes). The hinted
     * path's contract is explicitly "no interval-tree search". */
    n00b_require(!n00b_option_is_set(n00b_mem_get_allocator(ptr)),
                 "n00b_free_from_allocator requires undiscoverable storage");
#endif

    n00b_free_storage_from_allocator(allocator, ptr);
}

// Folded into n00b_free(ptr, .allocator=...). Kept as a thin wrapper for
// existing callers. NOTE: unlike the old implementation (which ran finalizers
// for discoverable storage), the hinted path now frees straight through the
// allocator with NO finalizer run and NO interval-tree search — pass only
// owner-known raw storage with no finalizer.
void
n00b_free_with_allocator_hint(n00b_allocator_t *allocator, void *ptr)
{
    n00b_free(ptr, .allocator = allocator);
}

void
n00b_free(void *ptr) _kargs {
    n00b_allocator_t *allocator = nullptr;
}
{
    // Caller asserts the owning allocator: free straight through it. The old
    // hint branch already returned before the finalizer scan and the global
    // n00b_mem_get_allocator search; this branch now sits AHEAD of the
    // baked-region check too, so a hinted free takes ZERO global mmap_lock
    // lookups — the hot path for bulk-freeing scratch/arena/pool temporaries
    // that otherwise dominates ingest under load. Contract: owner-known heap
    // storage from `allocator`, with no finalizer, and NEVER a read-only
    // baked/comptime-image pointer — freeing one of those through an allocator
    // would delete its mmap range and corrupt a static page (see DECISIONS
    // WP-012 Part D.2). The bare-free path below still no-ops baked pointers;
    // the debug assert re-imposes that safety net on the hinted path without
    // paying the lookup in release builds.
    if (allocator != nullptr) {
        if (ptr != nullptr) {
            n00b_assert(!n00b_gc_addr_in_baked_region(ptr));
            n00b_free_storage_from_allocator(allocator, ptr);
        }
        return;
    }

    if (n00b_gc_addr_in_baked_region(ptr)) {
        return;
    }

    /* No cooperative STW handshake here (WP-001).  The old concern was a
     * foreign thread walking into pool_free / delete_one_page_entry — mutating
     * the mmap tree or munmap'ing a page — while the GC mark phase read the tree
     * under a cooperative stop.  That is now closed structurally: munmap and
     * every mmap interval-tree mutation are CRITICAL EXECUTION held under
     * rt->critical_execution, and a stop-the-world initiator must ACQUIRE that
     * gate before it suspends any thread.  So the collector can never be mid-walk
     * while a free mutates the tree underneath it, with no per-free poll. */
    n00b_run_and_remove_finalizers(ptr);

    // A bare n00b_free(ptr) with no allocator hint discovers the owning
    // allocator via the global mmap interval-tree. (Resolving nullptr -> the
    // current allocator here was tried and reverted: some callers free objects
    // not owned by the active current allocator, so it freed to the wrong
    // allocator and crashed. Hot paths avoid this search by passing an explicit
    // .allocator instead.)
    n00b_allocator_opt_t alloc_opt = n00b_mem_get_allocator(ptr);

    if (!n00b_option_is_set(alloc_opt) && ptr != nullptr) {
        // Hidden pools keep their pages out of the tree, but each allocation
        // names its pool in its header.
        n00b_allocator_t *pool = n00b_pool_unlisted_owner(ptr);
        if (pool != nullptr) {
            n00b_free_storage_from_allocator(pool, ptr);
            return;
        }
        // An arena that stays out of the tree frees nothing singly, and its
        // allocations carry the inline header.
        if (n00b_option_is_set(n00b_inline_alloc_header(ptr))) {
            return;
        }
#ifdef N00B_DEBUG
        // Nothing owns it, so there is nothing to free it to, and a silent
        // return would hide a leak or a bad pointer. The report is raw
        // because formatting one frees, which would come back here.
        static const char msg[] = "n00b_free: no allocator owns this pointer\n";
        n00b_raw_write_all_brief(2, msg, sizeof msg - 1);
        abort();
#endif
        return;
    }
    if (!n00b_option_is_set(alloc_opt)) {
        return;
    }

    n00b_free_storage_from_allocator(n00b_option_get(alloc_opt), ptr);
}

// Single-lookup gated resolve for the flex-tail APIs. Resolves ptr's owning
// allocator ONCE, bails unless it reserves at least `min_extra` flex-tail bytes,
// then (taking the metadata STW gate when required, reporting it via
// *unlock_gate) returns the OOB record. Folds what would otherwise be two
// n00b_mem_get_allocator interval-tree searches (one to read oob_extra_size, one
// inside n00b_oob_for_user_ptr_held) into one — oob_extra_size is immutable after
// n00b_pool_init, so the single read is authoritative.
static inline n00b_oob_hdr_t *
n00b_oob_with_extra_held(void *ptr, uint32_t min_extra, bool *unlock_gate)
{
    *unlock_gate = false;

    n00b_allocator_opt_t ao = n00b_mem_get_allocator(ptr);
    if (!n00b_option_is_set(ao)) {
        return nullptr;
    }
    n00b_allocator_t *al = n00b_option_get(ao);
    if (al->oob_extra_size < min_extra || al->metadata == nullptr) {
        return nullptr;
    }

    if (n00b_allocator_metadata_needs_stw_gate(al)) {
        n00b_rw_read_lock(&n00b_get_runtime()->critical_execution);
        *unlock_gate = true;
    }

    return n00b_md_get(al->metadata, ptr);
}

void *
n00b_alloc_extra(void *ptr)
{
    if (ptr == nullptr) {
        return nullptr;
    }

    bool            unlock_gate = false;
    n00b_oob_hdr_t *oob         = n00b_oob_with_extra_held(ptr, 1, &unlock_gate);
    void           *result      = (oob != nullptr) ? (void *)oob->alloc_extra
                                                   : nullptr;
    n00b_metadata_gate_unlock(unlock_gate);
    // NOTE: `result` points into the OOB record, which a metadata-arena
    // compaction would relocate. Callers must not hold it across a point where
    // the world can stop / the metadata arena can be rebuilt; re-resolve as
    // needed. n00b_alloc_ref/unref do their atomic under the gate for exactly
    // this reason.
    return result;
}

// Per-alloc refcount lives in the first 4 bytes of the OOB flex tail and uses a
// BIASED encoding: stored = (live refs - 1). A fresh allocation's flex tail is
// zero-filled by zero_alloc, which is therefore exactly "1 reference" with no
// alloc-time initialisation needed (keeps the alloc fast path free of refcount
// knowledge). ref() bumps it; unref() at stored==0 is the last reference and
// frees. The usual refcount contract holds: only call n00b_alloc_ref while you
// already hold a reference, and balance every ref with one unref — so the
// last-ref free can never race a concurrent ref (there is no live holder left
// to issue one), exactly like an Arc with no Weak upgrade.
void
n00b_alloc_ref(void *ptr)
{
    if (ptr == nullptr) {
        return;
    }

    bool            unlock_gate = false;
    n00b_oob_hdr_t *oob         = n00b_oob_with_extra_held(ptr,
                                                           sizeof(uint32_t),
                                                           &unlock_gate);
    if (oob != nullptr) {
        _Atomic(uint32_t) *rc = (_Atomic(uint32_t) *)oob->alloc_extra;
        atomic_fetch_add_explicit(rc, 1, memory_order_relaxed);
    }
    n00b_metadata_gate_unlock(unlock_gate);
}

void
n00b_alloc_unref(void *ptr)
{
    if (ptr == nullptr) {
        return;
    }

    bool            unlock_gate = false;
    n00b_oob_hdr_t *oob         = n00b_oob_with_extra_held(ptr,
                                                           sizeof(uint32_t),
                                                           &unlock_gate);
    bool            do_free     = false;
    if (oob != nullptr) {
        _Atomic(uint32_t) *rc  = (_Atomic(uint32_t) *)oob->alloc_extra;
        uint32_t           cur = atomic_load_explicit(rc, memory_order_acquire);
        while (true) {
            if (cur == 0) {
                // stored 0 == last live ref; by contract no concurrent ref is
                // possible, so claiming the free here is safe.
                do_free = true;
                break;
            }
            if (atomic_compare_exchange_weak_explicit(rc,
                                                      &cur,
                                                      cur - 1,
                                                      memory_order_acq_rel,
                                                      memory_order_acquire)) {
                break;
            }
        }
    }
    n00b_metadata_gate_unlock(unlock_gate);

    // Free outside the metadata gate: n00b_free re-takes it for OOB teardown.
    if (do_free) {
        n00b_free(ptr);
    }
}

void
n00b_add_finalizer(void *obj, n00b_finalizer_t fn, void *user_data)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    assert(rt);

    // Allocations with an OOB record keep the finalizer there, so the free
    // path reads it back with the record lookup it already does. The inline
    // header stays out of it: it doubles as the marshal payload.
    n00b_allocator_opt_t alloc_opt = n00b_mem_get_allocator(obj);
    if (n00b_option_is_set(alloc_opt)) {
        n00b_allocator_t *al = n00b_option_get(alloc_opt);
        if (al != nullptr && al->metadata != nullptr) {
            bool unlock_gate = n00b_allocator_metadata_needs_stw_gate(al);
            if (unlock_gate) {
                n00b_rw_read_lock(&rt->critical_execution);
            }
            n00b_oob_hdr_t *oob = n00b_md_get(al->metadata, obj);
            if (oob != nullptr) {
                oob->finalizer      = fn;
                oob->finalizer_user = user_data;
                n00b_atomic_store(&al->oob_finalizers, true);
            }
            n00b_metadata_gate_unlock(unlock_gate);
            if (oob != nullptr) {
                return;
            }
        }
    }

    n00b_finalizer_index_put(rt, obj, fn, user_data);
}

static void
n00b_run_and_remove_finalizers(void *ptr)
{
    if (!n00b_default_runtime_is_set()) {
        return;
    }

    n00b_runtime_t *rt = n00b_get_runtime();
    if (!rt) {
        goto type_cleanup;
    }

    // Fast path: read the finalizer slot from the allocation's
    // out-of-band record. Clear the slot before invoking so a
    // finalizer that frees other memory cannot trigger a recursive
    // run against the same allocation. OOB-bearing allocations
    // never have an index entry (n00b_add_finalizer chooses one
    // or the other), so this path is complete.
    {
        bool            unlock_gate = false;
        n00b_oob_hdr_t *oob         = n00b_oob_for_user_ptr_held(ptr, &unlock_gate);
        if (oob != nullptr) {
            n00b_finalizer_t fn   = oob->finalizer;
            void            *user = oob->finalizer_user;
            oob->finalizer      = nullptr;
            oob->finalizer_user = nullptr;
            n00b_metadata_gate_unlock(unlock_gate);
            if (fn) {
                fn(user);
            }
            goto type_cleanup;
        }
        n00b_metadata_gate_unlock(unlock_gate);
    }

    if (rt->finalizers != nullptr && n00b_atomic_load(&rt->finalizer_count) != 0) {
        bool                   gate = n00b_finalizer_gate_lock(rt);
        n00b_finalizer_info_t *info = n00b_finalizer_index_take(rt, ptr);
        if (gate) {
            n00b_rw_unlock(&rt->critical_execution);
        }
#if defined(N00B_DEBUG)
        atomic_fetch_add_explicit(&n00b_finalizer_registry_probes, 1, memory_order_relaxed);
#endif
        if (info != nullptr) {
            n00b_finalizer_t fn   = info->funcptr;
            void            *user = info->user_ptr;
            n00b_free(info, .allocator = (n00b_allocator_t *)&rt->system_pool);
            fn(user);
        }
    }

type_cleanup:;
    // Lock cleanup and vtable destructor via the type registry.
    auto tinfo_opt = n00b_type_info_for(ptr);
    if (!n00b_option_is_set(tinfo_opt)) {
        return;
    }
    n00b_type_info_t *tinfo = n00b_option_get(tinfo_opt);

    // Free the lock if the type has a registered lock_offset.  Guard
    // the deref: with external_metadata pools, tinfo is set even for
    // pool allocs whose user_ptr offset doesn't actually point to a
    // heap-allocated lock pointer (the type may have been allocated
    // raw into pool memory without n00b_*_init populating .lock).
    // Confirm `*lock_ptr` resolves to a tracked allocation header
    // before freeing.
    if (n00b_option_is_set(tinfo->lock_offset)) {
        uint32_t        offset   = n00b_option_get(tinfo->lock_offset);
        n00b_rwlock_t **lock_ptr = (n00b_rwlock_t **)((char *)ptr + offset);
        n00b_rwlock_t  *lock_val = *lock_ptr;
        if (lock_val) {
            bool pin  = false;
            bool heap = n00b_alloc_info_is_heap(
                n00b_find_alloc_info(lock_val, .pin = &pin));
            n00b_alloc_hdr_unpin(pin);
            if (heap) {
                n00b_free(lock_val);
            }
            *lock_ptr = nullptr;
        }
    }

    // Run the vtable destructor.
    n00b_vtable_entry dtor = tinfo->core_vtable[N00B_BI_FINALIZER];
    if (dtor) {
        ((void (*)(void *))dtor)(ptr);
    }
}

// The pre-deferral destroy body: runs inline for non-epoch allocators, and
// during stop-the-world (via n00b_allocator_run_deferred_destroys or a
// collector-context destroy) for use_epochs allocators.
static void
allocator_destroy_now(n00b_allocator_t *allocator)
{
    // Order matters. al->metadata (the OOB header dict) is backed by
    // al->metadata_pool, so destroying the metadata pool frees the dict's
    // storage. The main pool's pages stay in the global mmap tree (pointing at
    // this allocator) until (*allocator->destroy) unregisters them. If we freed
    // the metadata pool FIRST (the old order), there was a window where a
    // conservative GC stack scan could resolve a main-pool address back to this
    // allocator via n00b_mmap_by_address and then dereference the already-freed
    // (and often reused -> ASCII) dict in _n00b_find_alloc_info /
    // n00b_dict_untyped_get -> SIGSEGV. The metadata lookup is NOT STW-gated
    // during collection, so STW did not protect it; under hot-allocator churn
    // this crashed the gateway.
    //
    // Allocator-specific quiescence/finalization must run before the backing
    // memory is unmapped, while metadata is still intact. Pools use this to
    // drain epoch retirements; other allocator kinds can leave it null or
    // provide their own policy.
    if (allocator->pre_destroy != nullptr) {
        allocator->pre_destroy(allocator);
    }

    n00b_allocator_t *metadata_pool = allocator->metadata_pool;
    allocator->metadata_pool        = nullptr;
    // Captured BEFORE the vtable destroy: arenas unmap their own control
    // struct in (*destroy), so `allocator` must not be dereferenced after.
    bool              self_is_md    = allocator->is_metadata;

    if (allocator->destroy != nullptr) {
        (*allocator->destroy)(allocator);
    }

    if (metadata_pool != nullptr) {
        // Route through n00b_allocator_destroy: metadata pools defer their
        // teardown to the next stop-the-world (dict-store migration retires
        // their stores lock-free from any thread, so an inline drain always
        // races a concurrent retire). Whoever runs the real teardown also
        // frees the control struct — see the self_is_md block below.
        n00b_allocator_destroy(metadata_pool);
    }

    // Metadata-pool control structs are allocated from the runtime
    // system_pool in n00b_new_metadata_pool() (pool_destroy does NOT unmap
    // them, unlike arena structs). Return the struct with the teardown so
    // repeated transient external-metadata allocators do not ratchet
    // system_pool upward — and so a DEFERRED teardown never leaves a freed
    // struct parked in the queue.
    if (self_is_md) {
        n00b_runtime_t *rt = n00b_default_runtime_or_null();
        if (rt != nullptr) {
            ((n00b_allocator_t *)&rt->system_pool)
                ->free((n00b_allocator_t *)&rt->system_pool, allocator);
        }
    }
}

// Park a use_epochs allocator for teardown at the next stop-the-world.
// Returns false when the queue is full (caller falls back to the barrier).
static bool
allocator_defer_destroy(n00b_runtime_t *rt, n00b_allocator_t *allocator)
{
    for (uint32_t i = 0; i < N00B_DEFERRED_DESTROY_MAX; i++) {
        if (n00b_atomic_load(&rt->deferred_destroys[i]) != nullptr) {
            continue;
        }
        n00b_allocator_t *expected = nullptr;
        if (n00b_cas(&rt->deferred_destroys[i], &expected, allocator)) {
            return true;
        }
    }
    return false;
}

// STW-only: tear down every parked allocator. Must run AFTER
// n00b_epoch_flush_all_stw in the same stop — the flush frees parked retire
// nodes THROUGH their allocators, so the pools must still be alive when it
// runs; once the lists are empty, teardown cannot orphan a node.
void
n00b_allocator_run_deferred_destroys(n00b_runtime_t *rt)
{
    if (rt == nullptr) {
        return;
    }

    for (uint32_t i = 0; i < N00B_DEFERRED_DESTROY_MAX; i++) {
        n00b_allocator_t *allocator =
            n00b_atomic_load(&rt->deferred_destroys[i]);
        if (allocator == nullptr) {
            continue;
        }
        n00b_atomic_store(&rt->deferred_destroys[i],
                          (n00b_allocator_t *)nullptr);
        allocator_destroy_now(allocator);
    }
}

// Mutator-callable full barrier: stop the world, flush every retire list,
// tear down all parked allocators, restart. Used when the deferral queue is
// full and by tests/shutdown paths that need destruction to be observable
// NOW.
void
n00b_allocator_deferred_destroy_barrier(void)
{
    n00b_runtime_t *rt = n00b_default_runtime_or_null();
    if (rt == nullptr) {
        return;
    }

    atomic_fetch_add(&rt->deferred_destroy_barrier_count, 1);
    n00b_stop_the_world();
    n00b_epoch_flush_all_stw(rt);
    n00b_allocator_run_deferred_destroys(rt);
    n00b_restart_the_world();
}

void
n00b_allocator_destroy(n00b_allocator_t *allocator)
{
    if (allocator == nullptr) {
        return;
    }

    // Metadata pools can ALWAYS be racing a lock-free n00b_retire
    // (dict-store migration retires their stores from whatever thread
    // touches the dict), so an inline drain-then-destroy leaves a window
    // where a fresh node is parked for a now-dead pool — the node is later
    // freed through the dead allocator and corrupts recycled memory. Defer
    // the teardown to the next stop-the-world, where the epoch flush empties
    // every retire list while the pool is still alive. Only metadata pools
    // defer: their control structs are always heap-owned (system_pool),
    // whereas ordinary n00b_pool_t structs are frequently stack-locals whose
    // frame dies with the destroy call. During STW itself (collector
    // context) there are no racers, so destroy inline.
    if (allocator->is_metadata) {
        n00b_runtime_t *rt = n00b_default_runtime_or_null();
        if (rt != nullptr && !n00b_atomic_load(&rt->stw_active)) {
            if (allocator_defer_destroy(rt, allocator)) {
                return;
            }
            n00b_allocator_deferred_destroy_barrier();
            // Queue was full; the barrier just emptied it (and flushed all
            // retire lists), so this allocator can now park.
            if (allocator_defer_destroy(rt, allocator)) {
                return;
            }
        }
    }

    allocator_destroy_now(allocator);
}

#define find_sentinal(p, m, memo) _find_sentinal(((uint64_t)p), (m), (memo))

// Backstop for the conservative scan: a candidate that resolves into a managed
// segment but is NOT a real object pointer (a non-pointer int slot, or a stack/
// register value that merely looks like an address) has no allocation guard
// preceding it.  Without a cap, the backward guard search walks from the
// candidate to the segment start -- on a large/sparsely-used arena segment that
// is hundreds of MB of word-by-word scanning, which livelocks the collector.
// Real interior pointers always have their guard within one allocation, so a
// candidate with no guard within ONE ALLOCATION back is, by definition, not a
// pointer into a live object: bail.  (Conservative stack/register roots are
// irreducible -- precise heap GC maps cannot cover them -- so this cap is
// required regardless of how precise heap scanning is.)
//
// n00b#321: "one allocation" is the correct bound, and this constant used to
// stand in for it. That is only equivalent while no allocation exceeds it --
// past 8 MB the scan gave up SHORT of a live object's guard and the caller
// could not tell that from "no guard exists". n00b_visit_possible_pointer
// reads that as free space and drops the root, so a GC stack root holding an
// interior pointer more than 8 MB into its allocation was silently neither
// marked nor forwarded: the object moves (or is reclaimed) while a live root
// still points into it, and the next dereference faults on a stale address.
//
// n00b#321 made the bound the largest inline-header allocation the process has
// ever made, so it tracked "one allocation" by construction, with this
// constant as a floor.
//
// n00b#395: that value is PROCESS-GLOBAL and MONOTONIC. It never falls, so one
// large allocation made once raised the cost of every later probe for the
// remaining life of the process -- after that allocation was freed, and in
// mappings that never held anything but small objects. Two processes with
// identical live sets could differ by orders of magnitude in scan cost based
// only on what one of them allocated once, hours earlier.
//
// The bound is therefore taken PER MAPPING whenever the mapping has one
// (n00b_mmap_info_t.max_alloc_len, maintained by n00b_mmap_note_alloc_len);
// there the number is exact, so the floor below would only re-inflate it. The
// global value plus this floor remain the fallback for mappings that record
// nothing, which is exactly today's behaviour.
//
// The per-mapping value is safe for the same reason the scan's floor is
// ALREADY clamped to mmap->start: an allocation lies wholly inside one
// registered mapping (arena.c sizes a fresh segment to the request that did
// not fit, and never writes an object across segment_end), so a mapping's own
// high-water can never be smaller than a live allocation inside it -- which is
// the property whose absence caused n00b#321.
#define N00B_SENTINEL_SCAN_MIN_WORDS (1u << 20) // 8 MB

// Cost of the backward guard scan, in-process. n00b#275 and n00b#395 were both
// diagnosed from a spindump on someone's laptop, which is a poor way to know
// whether a collector is scan-bound. One atomic add per CALL (not per word) is
// free relative to the walk it measures, and it turns "the mark loop is
// syscall/scan bound" into a number any consumer can read out of a wedged
// process: words / calls is the average distance each interior-pointer
// resolution actually walks.
_Atomic(uint64_t) n00b_sentinel_scan_calls = 0;
_Atomic(uint64_t) n00b_sentinel_scan_words = 0;

[[n00b::nogc]] static inline void
sentinal_scan_account(uint64_t *from, uint64_t *to)
{
    atomic_fetch_add_explicit(&n00b_sentinel_scan_calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&n00b_sentinel_scan_words,
                              from >= to ? (uint64_t)(from - to) : 0,
                              memory_order_relaxed);
}

// The guard memo records, for each N00B_GUARD_MEMO_LINE-aligned address B a
// backward walk has crossed, what lies below B: the nearest guard word, or that
// no guard lies in [S, B) for the lowest address S the walk examined. Both are
// facts about memory, so they hold whatever mapping a later walk resolves
// against. A later walk stops at the first line boundary whose entry answers
// it, and jumps past a guard-free range that ends above its floor, so each
// line is walked once per memo. An entry is the distance down to the guard or
// to S in words, shifted left one bit, with the low bit set for "no guard";
// zero means unknown, and a distance that does not fit is not recorded. Lines
// are kept in one array per page, found through the memo's dict.
#define N00B_GUARD_MEMO_LINE 256

[[n00b::nogc]] static uint32_t *
guard_memo_lines(n00b_guard_memo_t *memo, uintptr_t page)
{
    if (memo->last_lines != nullptr && memo->last_page == page) {
        return memo->last_lines;
    }

    bool  found = false;
    void *lines = n00b_gc_ptr_table_get(&memo->pages, (void *)page, &found);

    if (!found) {
        lines = n00b_alloc_array_with_opts(
            uint32_t,
            n00b_page_size / N00B_GUARD_MEMO_LINE,
            &(n00b_alloc_opts_t){.allocator = memo->pages.allocator});
        n00b_gc_ptr_table_put(&memo->pages, (void *)page, lines);
    }
    memo->last_page  = page;
    memo->last_lines = lines;
    return lines;
}

[[n00b::nogc]] static char *
find_sentinal_memo(uint64_t          *p0,
                   uint64_t          *floor,
                   uint64_t          *start,
                   n00b_guard_memo_t *memo)
{
    uintptr_t pgmask    = (uintptr_t)n00b_page_size - 1;
    uintptr_t linemask  = N00B_GUARD_MEMO_LINE - 1;
    uintptr_t cur_page  = ~(uintptr_t)0;
    uint32_t *cur_lines = nullptr;
    uint64_t *p         = p0;
    uint64_t *guard     = nullptr;
    uint64_t *clear     = nullptr; // [clear, p0] holds no guard, when set
    uint64_t  examined  = 0;

    while (p >= floor) {
        examined++;
        if (*p == n00b_gc_guard) {
            guard = p;
            break;
        }
        if (((uintptr_t)p & linemask) == 0 && p > start) {
            uintptr_t page = (uintptr_t)p & ~pgmask;
            if (page != cur_page) {
                cur_page  = page;
                cur_lines = guard_memo_lines(memo, page);
            }
            uint32_t known = cur_lines[((uintptr_t)p & pgmask) / N00B_GUARD_MEMO_LINE];
            if (known != 0) {
                uint64_t *target = p - (known >> 1);
                if (!(known & 1)) {
                    guard = target;
                    break;
                }
                if (target <= floor) {
                    clear = target;
                    break;
                }
                p = target - 1;
                continue;
            }
        }
        p--;
    }

    if (guard == nullptr && clear == nullptr) {
        clear = floor;
    }

    uint64_t *below = guard ? guard : clear;

    // Record the answer at each line the walk crossed or took from the memo,
    // following the walk's jumps. A guard-free entry the walk jumped through
    // gets this answer too, so a run of walks with falling floors follows one
    // entry each rather than every earlier floor. Each line counts as one word
    // examined.
    for (uintptr_t b = (uintptr_t)p0 & ~linemask;
         b > (uintptr_t)below && b > (uintptr_t)start;) {
        uintptr_t page = b & ~pgmask;
        if (page != cur_page) {
            cur_page  = page;
            cur_lines = guard_memo_lines(memo, page);
        }
        examined++;

        uint32_t *line  = &cur_lines[(b & pgmask) / N00B_GUARD_MEMO_LINE];
        uint32_t  known = *line;
        uintptr_t next  = b - N00B_GUARD_MEMO_LINE;

        if (known != 0) {
            if (!(known & 1)) {
                break;
            }
            next = (uintptr_t)((uint64_t *)b - (known >> 1) - 1) & ~linemask;
        }

        uint64_t distance = (uint64_t)((uint64_t *)b - below);
        if (distance <= UINT32_MAX >> 1) {
            *line = (uint32_t)(distance << 1) | (guard ? 0u : 1u);
        }
        b = next;
    }

    atomic_fetch_add_explicit(&n00b_sentinel_scan_calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&n00b_sentinel_scan_words, examined, memory_order_relaxed);

    if (guard == nullptr || guard < floor) {
        return nullptr;
    }
    return (char *)guard;
}

[[n00b::nogc]] static inline char *
_find_sentinal(uint64_t p_num, const n00b_mmap_view_t *map, n00b_guard_memo_t *memo)
{
    uint64_t *start = (uint64_t *)map->start;
    uint64_t *p     = (uint64_t *)n00b_align_floor(p_num, sizeof(void *));
    uint64_t *floor = start;
    uint64_t *p0    = p;

    // The caller backs `p` up by one header before scanning, so a candidate
    // at the very base of a mapping can land BELOW its start.  `p - start` is
    // then negative, and the unsigned compare below would read it as an
    // enormous positive distance -- putting the floor below the mapping and
    // walking off the front of it.  There is no guard to find down there in
    // any case.
    if (p < start) {
        sentinal_scan_account(p0, p0);
        return nullptr;
    }

    // Round the byte high-water mark UP to whole words: a guard sits at the
    // allocation's first word, so scanning a partial word short of it would
    // reintroduce exactly the off-by-a-little miss this bound exists to stop.
    uint64_t max_len = map->max_alloc_len;

    uint64_t cap_words;

    if (max_len != 0) {
        // This mapping accounts for every allocation placed in it, so "one
        // allocation" is exactly this number -- the 8 MB floor below would
        // only re-inflate a bound we now know precisely.
        cap_words = (max_len + sizeof(uint64_t) - 1) / sizeof(uint64_t);
    }
    else {
        max_len   = n00b_atomic_load(&n00b_max_inline_alloc_len);
        cap_words = (max_len + sizeof(uint64_t) - 1) / sizeof(uint64_t);

        if (cap_words < N00B_SENTINEL_SCAN_MIN_WORDS) {
            cap_words = N00B_SENTINEL_SCAN_MIN_WORDS;
        }
    }

    if ((uint64_t)(p - start) > cap_words) {
        floor = p - cap_words;
    }

    // Page-safe backward scan.  A false-positive candidate can sit in an arena
    // segment's reserved-but-UNCOMMITTED tail (or just before a guard page);
    // reading those words SIGBUSes.  A real object's guard is always in
    // committed memory at or above its own pages, so stop at the first
    // unreadable page rather than fault.
    //
    // The scan never leaves [map->start, p], so when the mapping's OWN record
    // already states its permissions, those permissions answer for every page
    // the walk can reach and no per-page check is needed at all. That check
    // used to be a global interval-tree search per page plus -- whenever the
    // record's perms were unknown -- a pipe write() + poll() probe per page.
    // n00b#275 measured that pair as 84% of _n00b_find_alloc_info's samples on
    // a gateway that was stopped-the-world for 90% of wall clock, with
    // n00b_mmap_search_smallest_map the single heaviest leaf in the process.
    // Mappings whose perms are genuinely unknown (a static segment's
    // unbacked tail, n00b#290) still pay the per-page check.
    if (map->perms == n00b_mmap_perms_no_access) {
        sentinal_scan_account(p0, p0);
        return nullptr;
    }

    bool page_checks_needed = (map->perms == n00b_mmap_perms_unknown);

    if (memo != nullptr && !page_checks_needed) {
        return find_sentinal_memo(p0, floor, start, memo);
    }

    uintptr_t pgmask   = (uintptr_t)n00b_page_size - 1;
    uintptr_t cur_page = ~(uintptr_t)0;
    while (p >= floor) {
        if (page_checks_needed) {
            uintptr_t pg = (uintptr_t)p & ~pgmask;
            if (pg != cur_page) {
                cur_page = pg;
                if (n00b_check_memory_perms((void *)p)
                    == n00b_mmap_perms_no_access) {
                    break; // uncommitted / guard page: not inside a live object
                }
            }
        }
        if (*p == n00b_gc_guard) {
            sentinal_scan_account(p0, p);
            return (char *)p;
        }
        p--;
    }

    sentinal_scan_account(p0, p);
    return nullptr;
}

// Returns the most authoritative header, unless prefer_inline is set,
// in which case it'll return the inline header.
//
// If you need to know if it's an out-of-heap pointer so you can cast it,
// then provide is_metadata_header.
//
// Returns nullptr if not found. This obviously should all change to
// return a variant, once I revisit variants.

// `hint`, when it covers `addr`, is a registry record whose lifetime the
// caller controls (the collector's, under stop-the-world); any other lookup
// copies the record out under the registry lock.
[[n00b::nogc]] static void
find_alloc_info(void                *addr,
                n00b_alloc_info_t   *result,
                bool                 scan_for_header,
                n00b_guard_memo_t   *guard_memo,
                n00b_mmap_info_t    *hint,
                bool                *pin)
{
    n00b_mmap_view_t view;
    char            *p = (char *)addr;

    if (hint != nullptr && (uint64_t)addr >= hint->start && (uint64_t)addr < hint->end) {
        n00b_mmap_view_of(hint, &view);
    }
    else if (!n00b_mmap_view_by_address(addr, &view)) {
        *result = (n00b_alloc_info_t){.kind = n00b_alloc_none};
        return;
    }

    n00b_mmap_view_t *mmap = &view;
    n00b_allocator_t *al   = mmap->allocator;
    switch (mmap->kind) {
    case n00b_mmap_static: {
        auto range_opt = n00b_mmap_range_by_address(addr);
        if (!n00b_option_is_set(range_opt)) {
            break;
        }
        *result = (n00b_alloc_info_t){
            .kind      = n00b_alloc_static_range,
            .hdr.range = n00b_option_get(range_opt),
        };
        return;
    }
    case n00b_mmap_pool:
    case n00b_mmap_managed_segment:
    case n00b_mmap_sys_segment:

        if (al->add_inline_header) {
            p -= N00B_ALLOC_HDR_SZ;
        }

        if (scan_for_header && al->add_inline_header) {
            char *scan_ptr = find_sentinal(p, mmap, guard_memo);

            if (!scan_ptr) {
                break;
            }

            // The nearest header behind `addr` owns it only if `addr` lies
            // inside that allocation or exactly one past its end, which C
            // allows a live pointer to hold and which must move with the
            // object.  Anything further is free space.
            uint64_t span = ((n00b_inline_hdr_t *)scan_ptr)->alloc_len;
            if ((uint64_t)((char *)addr - scan_ptr) > span) {
                break;
            }

            p    = scan_ptr;
            addr = scan_ptr + N00B_ALLOC_HDR_SZ;
        }

        // An allocator with OOB metadata and NO inline header cannot normalize
        // an interior pointer: the block above is the only place `addr` is
        // walked back to an allocation base, and it needs a sentinel to find.
        // The metadata dict below is keyed by the exact base address, so an
        // interior address misses and used to return n00b_alloc_err -- the same
        // answer as "this is free space", which is what conservative scanning
        // produces constantly. Report the two separately so a caller can tell a
        // routine miss from "there may be a live object here I cannot resolve."
        // See n00b_alloc_info_kind_t and n00b#327.
        bool interior_unresolvable = scan_for_header && !al->add_inline_header
                                  && al->metadata != nullptr;

        if (al->metadata) {
            // Metadata reads take the STW read lock too: this same dict can be
            // rebuilt wholesale by the owning allocator's metadata compactor.
            bool md_stw = n00b_allocator_metadata_needs_stw_gate(al);
            if (md_stw) {
                n00b_rw_read_lock(&n00b_get_runtime()->critical_execution);
            }
            n00b_oob_hdr_t *oob = n00b_md_get(al->metadata, addr);
            // A caller that asked for a pin keeps the gate across its use of
            // the header and releases it with n00b_alloc_hdr_unpin.
            if (md_stw && oob != nullptr && pin != nullptr) {
                *pin = true;
            }
            else if (md_stw) {
                n00b_rw_unlock(&n00b_get_runtime()->critical_execution);
            }
            if (!oob) {
                *result = (n00b_alloc_info_t){
                    .kind = interior_unresolvable
                              ? n00b_alloc_interior_unresolvable
                              : n00b_alloc_err,
                };
                return;
            }
#if defined(N00B_DEBUG)
            if (md_stw) {
                n00b_assert(alloc_hdr_use_is_pinned());
                if (n00b_alloc_hdr_lookup_hook != nullptr) {
                    n00b_alloc_hdr_lookup_hook(addr);
                }
            }
#endif
            *result = (n00b_alloc_info_t){
                .kind    = n00b_alloc_oob,
                .hdr.oob = oob,
            };
            return;
        }

        n00b_inline_hdr_t *hdr = (n00b_inline_hdr_t *)p;

        // A dead object on a retained page can run past its mapping into
        // pages the collector reclaimed.
        if (((uint64_t)p) < mmap->start || hdr->guard != n00b_gc_guard
            || (uint64_t)p + hdr->alloc_len > mmap->end) {
            *result = (n00b_alloc_info_t){.kind = n00b_alloc_err};
            return;
        }
        *result = (n00b_alloc_info_t){
            .kind        = n00b_alloc_inline,
            .hdr.in_line = hdr,
        };
        return;

    default:
        break;
    }
    *result = (n00b_alloc_info_t){.kind = n00b_alloc_none};
    return;
}

[[n00b::nogc]] void
_n00b_find_alloc_info(void *addr, n00b_alloc_info_t *result) _kargs
{
    n00b_allocator_t *allocator       = nullptr;
    bool              scan_for_header = false;
    bool             *pin             = nullptr;
}
{
    if (pin != nullptr) {
        *pin = false;
    }
    find_alloc_info(addr, result, scan_for_header, nullptr, nullptr, pin);
}

[[n00b::nogc]] n00b_alloc_info_t
n00b_find_alloc_info_in(void              *addr,
                        n00b_mmap_info_t  *mmap,
                        bool               scan_for_header,
                        n00b_guard_memo_t *guard_memo)
{
    n00b_alloc_info_t result;

    find_alloc_info(addr, &result, scan_for_header, guard_memo, mmap, nullptr);
    return result;
}

// Fast-path allocation resolution when the owning allocator is already known
// (e.g. the marshaler walking a graph that lives predominantly in one pool):
// skip the global mmap interval-tree search entirely and resolve straight from
// the allocator's own OOB metadata index — the same lookup _n00b_find_alloc_info
// does in its external-metadata branch, including the STW metadata gate.
// Returns kind=n00b_alloc_oob on a hit (addr is an alloc start in `al`), or
// kind=n00b_alloc_none on a miss so the caller can fall back to the global path.
// Only valid for external-metadata allocators without inline headers (the inline
// case needs the page's mmap->start to bound the header scan); returns none
// otherwise. `pin` works as it does for n00b_find_alloc_info.
n00b_alloc_info_t
n00b_try_alloc_info_in_allocator(void *addr, n00b_allocator_t *al, bool *pin)
{
    *pin = false;
    if (al == nullptr || al->metadata == nullptr || al->add_inline_header) {
        return (n00b_alloc_info_t){.kind = n00b_alloc_none};
    }

    bool md_stw = n00b_allocator_metadata_needs_stw_gate(al);
    if (md_stw) {
        n00b_rw_read_lock(&n00b_get_runtime()->critical_execution);
    }
    n00b_oob_hdr_t *oob = n00b_md_get(al->metadata, addr);
    if (md_stw && oob != nullptr) {
        *pin = true;
    }
    else if (md_stw) {
        n00b_rw_unlock(&n00b_get_runtime()->critical_execution);
    }

    if (oob == nullptr) {
        return (n00b_alloc_info_t){.kind = n00b_alloc_none};
    }
    return (n00b_alloc_info_t){.kind = n00b_alloc_oob, .hdr.oob = oob};
}

n00b_option_t(n00b_inline_hdr_t *) n00b_object_header(void *p)
{
    n00b_alloc_info_t info = n00b_find_alloc_info(p);

    if (!n00b_alloc_info_is_heap(info)) {
        return n00b_option_none(n00b_inline_hdr_t *);
    }

    if (!n00b_alloc_info_is_oob(info)) {
        return n00b_alloc_info_inline(info);
    }

    n00b_oob_hdr_t *oob = info.hdr.oob;
    if (!oob->hcur) {
        return n00b_option_none(n00b_inline_hdr_t *);
    }
    return n00b_option_set(n00b_inline_hdr_t *, oob->hcur);
}
