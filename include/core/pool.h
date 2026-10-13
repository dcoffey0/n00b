/**
 * @file pool.h
 * @brief Fixed-size slab pool allocator.
 *
 * Provides fast allocation of small, fixed-size objects using
 * power-of-two size classes with lock-free free lists.
 */
#pragma once

#include "n00b.h"
#include "core/alloc_base.h"
#include "adt/llstack.h"
#include "adt/option.h" /* n00b_option_t for n00b_pool_quarantine_find */
#include "core/align.h"

#define N00B_POST_ROUND_SHIFT 6
#define N00B_NUM_FREE_LISTS   8
#define N00B_SYSTEM_POOL_AUDIT_TOP_N 16

typedef struct n00b_pool_page_t {
    struct n00b_pool_page_t *prev;
    struct n00b_pool_page_t *next;
    /* Page-aligned size of the underlying mmap, captured by
     * new_page_entry so pool_free's big-alloc path can munmap the
     * page when its single entry is freed. Without this, big-alloc
     * pool_free was only unlinking the page from page_table — the
     * memory stayed mapped until pool_destroy. Observed leak: any
     * pool client that n00b_free's a >N00B_NUM_FREE_LISTS-class
     * allocation gave back its slot in the free list but the page
     * remained mapped. */
    size_t                   mapped_size;
    // Interposer range-table slot for a .libc_backing pool's page, 0 if none.
    uint32_t                 interpose_slot;
} n00b_pool_page_t;

typedef struct n00b_pool_t n00b_pool_t;

// The header before every pool allocation. owner tags the pool's slot in
// the pool registry, which is how n00b_free finds a pool whose pages stay
// out of the mmap tree (pool.c).
typedef struct {
    unsigned int list_index;
    uint32_t     owner;
    n00b_pool_t *pool;
} n00b_pool_entry_t;

// Last-unref hook for .pool_refcount pools. A single-token typedef so it can be
// used as a _kargs kwarg type (the kwargs generator does not parse inline
// function-pointer declarators).
typedef void (*n00b_pool_unref_cb_t)(void *ctx);

static_assert(sizeof(n00b_pool_entry_t) <= N00B_ALIGN);

struct n00b_pool_t {
    n00b_base_allocator_t vtable;
    n00b_llstack_t        free_lists[N00B_NUM_FREE_LISTS];
    n00b_pool_page_t     *page_table;
    _Atomic uint32_t      lock;
    /* Diagnostics counters. Map = successful big_mmap (page table
     * grew), unmap = delete_one_page_entry's munmap. Used by callers
     * that need to verify alloc/free symmetry on the big-mmap fast
     * path. Always-on accounting so users don't need to recompile
     * libn00b to inspect them. */
    _Atomic uint64_t      big_map_count;
    _Atomic uint64_t      big_unmap_count;
    // Running sum of every live page's mapped_size, maintained atomically as
    // pages are added/removed. Lets n00b_pool_mapped_bytes be an O(1) lock-free
    // read -- it is called per record on the rocs seal hot path
    // (rocs_store_should_seal_hot), and from status paths that must never wait
    // on (or spin against) another thread's pool lock.
    _Atomic uint64_t      mapped_bytes_total;
    // High-water mark of mapped_bytes_total over the pool's life. Updated on
    // every page add; never lowered. The point of a scratch pool is that its
    // PEAK is bounded -- a final reading of mapped_bytes_total says nothing
    // about how much was held simultaneously, which is exactly the property
    // n00b-lang/n00b#432 got wrong (one retained block per marshaled object).
    _Atomic uint64_t      mapped_bytes_peak;
    bool                  scrub_locks_on_destroy;
    // When false, a freed big page is unmapped at once and never parked in the
    // process-wide released-page cache (see n00b_pool_init .page_cache).
    bool                  page_cache;
    // Per-pool ref-counting (opt-in via n00b_pool_init .pool_refcount). When
    // armed, pool_refs starts at 1 (the creator's ref); n00b_pool_ref/unref
    // adjust it and the last unref reclaims the whole pool. `on_last_unref`, if
    // set, runs instead of the default n00b_allocator_destroy so owners that
    // pair the pool with extra resources (e.g. rocs's ctl arena) can do the
    // full teardown. Refcounted pools are forced to OOB metadata (see
    // n00b_pool_init): inline headers are marshal-only.
    bool                  pool_refcounted;
    _Atomic(int32_t)      pool_refs;
    n00b_pool_unref_cb_t  on_last_unref;
    void                 *unref_ctx;
    // Debug-only per-allocation-site audit (compiled in only under
    // N00B_POOL_ALLOC_AUDIT). Opt in per-pool via n00b_pool_init(.alloc_audit
    // = true). `alloc_audit_dict` is a lazily-created
    // n00b_dict_t(uint64_t, void *) mapping (uintptr_t)site_string ->
    // n00b_pool_audit_counter_t *. It (and its counters) are allocated from a
    // dedicated HIDDEN storage pool (n00b_pool_audit_storage), NOT from any
    // audited pool, so the alloc/free hooks — which fire only for registered
    // audited pools — never recurse on their own bookkeeping allocations, and
    // never self-count; hidden-pool retention keeps them alive. The per-site
    // live counters carry the
    // GC-liveness signal: a leak shows up as a site whose live_bytes never
    // comes back down.
    bool                  alloc_audit;
    _Atomic(void *)       alloc_audit_dict;
    // The pool's slot in the pool registry, or UINT32_MAX when the registry
    // was full. Every allocation's header carries it (n00b_pool_entry_t).
    uint32_t              registry_slot;
    // Set when the pool's pages stay out of the mmap tree, so n00b_free
    // finds the pool through the allocation header.
    bool                  unlisted;
};

typedef struct n00b_pool_t n00b_pool_t;

// Counters-only snapshot (pure atomic loads; no registry walk, no lock).
typedef struct {
    uint64_t    total_init_count;
    uint64_t    total_destroy_count;
    uint64_t    registry_overflow_count;
    uint64_t    destroy_unmap_count;
    uint64_t    destroy_unmap_bytes;
    uint64_t    destroy_unmap_fail_count;
    uint64_t    destroy_unmap_fail_bytes;
    uint64_t    big_unmap_fail_count;
    uint64_t    big_unmap_fail_bytes;
    uint64_t    diagnostic_page_count;
    uint64_t    diagnostic_page_overflow_count;
    uint64_t    diagnostic_page_lock_skip_count;
    // Released big-page cache (pool.c). hits/misses count lookups when a big
    // page is mapped; parked counts pages the cache accepted on release, and
    // evicted counts pages it unmapped after they aged out across collections.
    // page_cache_bytes is what the cache holds mapped right now, and cap_bytes
    // is the limit on it (0 when the cache is disabled).
    uint64_t    page_cache_hits;
    uint64_t    page_cache_misses;
    uint64_t    page_cache_parked;
    uint64_t    page_cache_evicted;
    uint64_t    page_cache_bytes;
    uint64_t    page_cache_cap_bytes;
} n00b_pool_global_stats_t;

typedef struct {
    uint64_t    total_alloc_count;
    uint64_t    total_free_count;
    uint64_t    total_alloc_bytes;
    uint64_t    total_free_bytes;
    uint64_t    live_alloc_count;
    uint64_t    live_bytes;
    uint64_t    ptr_overflow_count;
    uint64_t    site_overflow_count;
    uint64_t    free_miss_count;
    uint64_t    lock_skip_count;
    uint64_t    top_count;
    const char *top_site[N00B_SYSTEM_POOL_AUDIT_TOP_N];
    uint64_t    top_alloc_count[N00B_SYSTEM_POOL_AUDIT_TOP_N];
    uint64_t    top_free_count[N00B_SYSTEM_POOL_AUDIT_TOP_N];
    uint64_t    top_alloc_bytes[N00B_SYSTEM_POOL_AUDIT_TOP_N];
    uint64_t    top_free_bytes[N00B_SYSTEM_POOL_AUDIT_TOP_N];
    uint64_t    top_live_count[N00B_SYSTEM_POOL_AUDIT_TOP_N];
    uint64_t    top_live_bytes[N00B_SYSTEM_POOL_AUDIT_TOP_N];
} n00b_system_pool_audit_stats_t;

extern void
n00b_system_pool_audit_alloc(n00b_allocator_t *allocator,
                             void             *ptr,
                             uint64_t          bytes,
                             const char       *site);

extern void
n00b_system_pool_audit_free(n00b_allocator_t *allocator, void *ptr);

// Convenience wrapper = n00b_pool_audit_stats(&rt->system_pool). NOTE: returns
// a zeroed struct unless system_pool was opened with .alloc_audit=true; the
// runtime does NOT audit system_pool by default (only user_pool + conduit_pool),
// so this is normally zero. Kept for API compatibility.
extern n00b_system_pool_audit_stats_t
n00b_system_pool_audit_stats(void);

// Debug-only (N00B_POOL_ALLOC_AUDIT): per-pool variant. Returns the audit
// snapshot (top sites by live bytes) for any pool opened with
// .alloc_audit=true. For pools not under audit — or in builds without the
// audit compiled in — returns a zeroed struct.
extern n00b_system_pool_audit_stats_t
n00b_pool_audit_stats(n00b_pool_t *pool);

// Zero-arg wrappers for the runtime's audited pools, so non-ncc consumers (the
// gateway census in main.m, which cannot name n00b_pool_t) can pull each pool's
// snapshot. Return a zeroed struct when the audit is not compiled in / armed.
extern n00b_system_pool_audit_stats_t
n00b_user_pool_audit_stats(void);

extern n00b_system_pool_audit_stats_t
n00b_conduit_pool_audit_stats(void);

// Reports whether per-site pool auditing is available in this build. Callers
// should publish this state beside audit snapshots because an unavailable audit
// also returns a zeroed snapshot.
extern bool
n00b_pool_audit_compiled(void);

// Report whether each runtime pool is collecting per-site audit data.
extern bool
n00b_user_pool_audit_enabled(void);

extern bool
n00b_conduit_pool_audit_enabled(void);

// The default runtime does not enable auditing for system_pool.
extern bool
n00b_system_pool_audit_enabled(void);

// Debug-only (N00B_POOL_ALLOC_AUDIT): true iff `allocator` is a pool opened
// with .alloc_audit=true. The allocator hot path (alloc.c) consults this to
// decide whether to reserve the trailing site-word; the audit hooks consult
// the same registry so the two always agree on layout.
extern bool
n00b_pool_alloc_audit_enabled(n00b_allocator_t *allocator);

/**
 * @brief Initialize a pool allocator.
 * @param pool Pool structure to initialize.
 * @return     Allocator interface pointer for the pool.
 *
 * @kw __system          System pool — skip STW checks (internal only).
 * @kw inline_headers    Prepend inline headers to allocations.
 * @kw external_metadata Keep OOB metadata in a separate arena.
 * @kw hidden            Hide from GC.
 * @kw scrub_locks_on_destroy
 *                      Scrub per-thread lock accounting chains before unmap.
 * @kw name              Debug name for the pool.
 * @kw pool_refcount     Enable per-pool reference counting. Orthogonal to the
 *                       metadata strategy — the count is a slot in the pool
 *                       header, so the caller's inline_headers/external_metadata
 *                       choice is left intact. pool_refs starts at 1;
 *                       n00b_pool_ref/unref adjust it; the last unref reclaims
 *                       the whole pool (via n00b_pool_set_unref_cb's hook if set,
 *                       else n00b_allocator_destroy).
 * @kw alloc_refcount    Enable per-allocation reference counting. Forces OOB
 *                       metadata and reserves a uint32_t counter in each OOB
 *                       record's flex tail. n00b_alloc_ref/unref adjust it; the
 *                       last unref returns that allocation to the pool.
 * @kw alloc_audit       Debug-only per-allocation-site audit
 *                       (N00B_POOL_ALLOC_AUDIT); no-op otherwise. Default false.
 * @kw page_cache        Park this pool's freed big pages in the process-wide
 *                       released-page cache for reuse by any pool. Default
 *                       true. Pass false for a pool whose big frees must
 *                       return memory to the kernel immediately, such as a
 *                       scratch pool that bounds its own footprint.
 * @kw libc_backing      Let libc allocations made while this pool is the
 *                       thread's current allocator land here. Default false,
 *                       which sends them to rt->user_pool instead. Set it only
 *                       on a pool that outlives every pointer libc may keep,
 *                       since libc caches state across calls. Its pages are
 *                       recorded so a free that arrives after shutdown is
 *                       still recognized as n00b memory.
 *
 * @pre @p pool points to zeroed or uninitialized memory.
 * @post The returned allocator is ready for use.
 */
extern n00b_allocator_t *
n00b_pool_init_at(n00b_pool_t *pool) _kargs
{
    bool        __system               = false;
    bool        inline_headers         = false;
    bool        external_metadata      = false;
    bool        hidden                 = false;
    bool        scrub_locks_on_destroy = true;
    const char *name                   = "pool";
    // "file:line" of the create-site, injected by the n00b_pool_init macro.
    const char *creation_loc           = nullptr;
    // Ref-counting (both force external_metadata; inline headers stay
    // marshal-only). pool_refcount: per-pool count, reclaim whole pool at last
    // unref. alloc_refcount: per-allocation count in the OOB flex tail, return
    // the alloc to the pool at its last unref. The last-unref hook is set
    // separately via n00b_pool_set_unref_cb (the kwargs generator cannot
    // express a function-pointer parameter type).
    bool        pool_refcount          = false;
    bool        alloc_refcount         = false;
    // Epoch reclamation is ONLY for !gc dictionary backing stores read
    // concurrently without a pinref/STW reader gate (the OOB metadata md_pool;
    // set true explicitly there). Every other pool must leave this OFF — see the
    // definition in pool.c for the full rationale.
    bool        use_epochs             = false;
    // Debug-only (N00B_POOL_ALLOC_AUDIT): opt into per-allocation-site auditing.
    // No-op in builds without the audit compiled in.
    bool        alloc_audit            = false;
    // Internal (n00b_new_metadata_pool only): marks vtable.is_metadata, which
    // routes this pool's destroy through the deferred STW teardown queue.
    bool        __is_md_pool           = false;
    bool        page_cache             = true;
    bool        libc_backing           = false;
};

// Create-site proxy, mirroring n00b_new_arena. Callers keep writing
// n00b_pool_init(&pool, .name = "x"); this injects the "file:line" string that
// n00b_allocator_setup stores in the vtable and the mmap histogram attributes
// segments by. (n00b_pool_init_at is the real _kargs entry point.)
#define n00b_pool_init(p, ...)                                                                 \
    n00b_pool_init_at((p), .creation_loc = N00B_LOC_STRING() __VA_OPT__(, __VA_ARGS__))

/**
 * @brief Install a last-unref hook on a `.pool_refcount` pool. When set, the
 *        last n00b_pool_unref runs `cb(ctx)` instead of n00b_allocator_destroy,
 *        so owners that pair the pool with extra resources can do the full
 *        teardown. Must be called after n00b_pool_init, before the pool is
 *        shared. No-op on non-refcounted pools.
 */
extern void n00b_pool_set_unref_cb(n00b_pool_t *pool, n00b_pool_unref_cb_t cb, void *ctx);

/**
 * @brief Add a reference to a `.pool_refcount` pool. No-op on non-refcounted
 *        pools. Safe across threads.
 */
extern void n00b_pool_ref(n00b_pool_t *pool);

/**
 * @brief Drop a reference to a `.pool_refcount` pool. On the last unref the pool
 *        is reclaimed: `on_last_unref(unref_ctx)` if set, else
 *        `n00b_allocator_destroy(pool)`. No-op on non-refcounted pools.
 */
extern void n00b_pool_unref(n00b_pool_t *pool);

/**
 * @brief The allocator-specific OOB flex-tail bytes for a pool allocation, or
 *        nullptr if the owning allocator has no OOB record / no extra bytes.
 *        Size is the allocator's `oob_extra_size`.
 *
 * @note The returned pointer is INTO the allocation's OOB record. A
 *       metadata-arena compaction (n00b_allocator_compact_metadata) or a
 *       stop-the-world event can relocate that record and invalidate the
 *       pointer. Callers must not retain it across any point where the world can
 *       stop or the metadata arena can be rebuilt; re-resolve as needed.
 *       n00b_alloc_ref/unref perform their atomic update under the metadata gate
 *       for exactly this reason.
 */
extern void *n00b_alloc_extra(void *ptr);

/**
 * @brief Add / drop a reference to a single `.alloc_refcount` allocation. The
 *        last unref returns that allocation to its pool. No-op if the owning
 *        allocator is not alloc-refcounted.
 */
extern void n00b_alloc_ref(void *ptr);
extern void n00b_alloc_unref(void *ptr);

/**
 * @brief Total bytes the pool has currently mapped from the kernel.
 *
 * Lock-free O(1) atomic read of the running total maintained as pages
 * are added/removed. This counts every mmap region the pool owns
 * — both the small-slab pages backing the size-class freelists and
 * the per-allocation big-mmap pages handed out for requests larger
 * than the freelist classes — regardless of how many of the slots in
 * those pages are currently in use.
 *
 * Intended for diagnostics: pair with phys_footprint sampling to
 * attribute resident memory to specific pools. Safe to call from any
 * thread at any time, including while the caller holds no idea whether
 * the pool's owner is suspended (it never touches the pool lock).
 */
extern uint64_t n00b_pool_mapped_bytes(n00b_pool_t *pool);

/**
 * @brief The pool @p ptr came from when that pool keeps its pages out of
 *        the mmap tree (the system pool and other hidden pools), or nullptr.
 *
 * @p ptr must be the address an allocation returned. n00b_free uses this
 * when the mmap tree knows no owner. A pool made while the pool registry
 * was full (n00b_pool_global_stats().registry_overflow_count) is not found.
 */
extern n00b_allocator_t *n00b_pool_unlisted_owner(void *ptr);

/**
 * @brief Highest value @ref n00b_pool_mapped_bytes has ever reported for this
 *        pool.
 *
 * Monotonic for the life of the pool (reset only by n00b_pool_init). Use it to
 * assert that a scratch pool's peak footprint stays within a bound: unlike the
 * live total, it survives the frees that happen before the caller can look.
 */
extern uint64_t n00b_pool_mapped_bytes_peak(n00b_pool_t *pool);

/**
 * @brief Number of mmap regions currently owned by the pool.
 *
 * Counts page-table entries, not live allocations. Small size-class slabs and
 * large one-allocation mappings both contribute one entry each.
 */
extern uint64_t n00b_pool_page_count(n00b_pool_t *pool);

/**
 * @brief Cumulative count of big-mmap pages this pool has released
 *        back to the kernel (i.e. n00b_safe_munmap calls in
 *        @ref delete_one_page_entry).
 *
 * Pair with the corresponding alloc counter to verify that the
 * big-mmap fast path is symmetric — every allocation eventually
 * paired with a release.
 */
extern uint64_t n00b_pool_big_unmap_count(n00b_pool_t *pool);

/**
 * @brief Cumulative count of big-mmap pages this pool has mapped
 *        from the kernel (i.e. successful @ref big_mmap calls).
 */
extern uint64_t n00b_pool_big_map_count(n00b_pool_t *pool);

extern n00b_pool_global_stats_t n00b_pool_global_stats(void);

/**
 * @brief Unmap every page held by the released big-page cache.
 *
 * The cache keeps freed big pool pages mapped (and resident) for exact-size
 * reuse, up to the cap reported in n00b_pool_global_stats().page_cache_cap_bytes.
 * Call this to hand that memory back to the kernel, for example after a burst
 * of large allocations that will not recur. Safe from any thread.
 *
 * If the cache lock stays busy for about a millisecond, which happens when
 * its holder is suspended by a stop-the-world or interrupted by the calling
 * thread's own signal handler, the drain gives up and unmaps nothing.
 *
 * @return Bytes unmapped.
 */
extern uint64_t n00b_pool_page_cache_drain(void);

/**
 * @brief Advance the released big-page cache's generation.
 *
 * Called by n00b_collect once per collection, with the world stopped.
 */
extern void n00b_pool_page_cache_note_collection(void);

/**
 * @brief Unmap cached pages parked two or more collections ago.
 *
 * Called by n00b_collect after it restarts the world. Returns at once when no
 * collection has run since the last trim, and skips the trim when the cache
 * lock is busy, leaving it to the next collection.
 */
extern void n00b_pool_page_cache_trim(void);

/**
 * @brief Diagnostic-only lookup of a live pool page by address.
 *
 * Unlike the global mmap registry, this also sees hidden/no-metadata pools whose
 * pages intentionally skip mmap registration. The implementation takes the pool
 * registry lock and the owning pool lock, so it is safe for low-frequency status
 * diagnostics but not for hot paths.
 */
extern bool n00b_pool_diagnostic_lookup_page(uintptr_t addr,
                                             uint64_t *out_start,
                                             uint64_t *out_end,
                                             const char **out_name,
                                             const char **out_creation_loc,
                                             bool *out_registered);

/**
 * @brief Usable byte count for a raw pool allocation.
 *
 * Given a pointer returned by a pool's zero_alloc, recover the number of
 * usable bytes (size class minus the entry header, or big-mmap region
 * minus its headers).  Used by the libc-malloc interposition layer
 * (`core/alloc_interpose.h`) to implement realloc()/malloc_usable_size()
 * without per-allocation side metadata.
 *
 * @pre @p ptr was returned by a pool (callers resolve ownership via
 *      @ref n00b_mem_get_allocator first).
 */
extern size_t n00b_pool_usable_size(void *ptr);

/**
 * @brief Big-free quarantine: freeing-call-stack depth recorded per parked
 *        page (see pool.c "Big-free quarantine").
 */
#define N00B_POOL_QUARANTINE_FRAMES 6

/**
 * @brief Attribution record for a faulting address inside a quarantined
 *        (freed, PROT_NONE-parked) big pool allocation.
 */
typedef struct n00b_pool_quarantine_hit_t {
    uint64_t    start;
    uint64_t    size;
    uint64_t    seq;
    const char *pool_name;
    void       *frees[N00B_POOL_QUARANTINE_FRAMES];
} n00b_pool_quarantine_hit_t;

/**
 * @brief Look up a faulting address in the big-free quarantine ring.
 *
 * Async-signal-safe (plain atomic loads, no locks, no allocation; the option
 * is a pure value type) — intended for the fatal-signal crash handler, which
 * uses it to attribute a use-after-free fault to the call stack that freed
 * the page. Returns none when the quarantine is disabled (env
 * N00B_POOL_BIG_QUARANTINE unset/0) or the address is not inside a parked
 * page.
 */
extern n00b_option_t(n00b_pool_quarantine_hit_t)
    n00b_pool_quarantine_find(uintptr_t addr);
