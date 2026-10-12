/**
 * @file gc.h
 * @brief Garbage collector interface and supporting types.
 *
 * Implements a copying/compacting collector for @c n00b_arena_t heaps.
 * When an arena runs out of space and has @c collection_enabled set,
 * @c n00b_collect() copies live allocations to a new arena segment,
 * rewrites all pointers, and releases the old memory.
 *
 * The collector traces:
 *   - User-registered GC roots (@ref n00b_gc_register_root)
 *   - Thread stacks (all registered threads)
 *   - The @c argv / @c envp arrays in @c n00b_runtime_t
 *
 * @c n00b_collect() owns the stop-the-world lock internally.
 */
#pragma once

#include "n00b.h"
#include "core/alloc_mdata.h"
#include "adt/list.h"
#include "adt/dict_untyped.h"
#include "conduit/conduit_types.h"
#include "core/pool.h"
#include "core/arena.h"

n00b_conduit_topic_t(n00b_buffer_t *);

#define N00B_DEBUG_CENSUS_HEALTH_TOP_N 8u

typedef struct {
    bool     enabled;
    bool     active;
    uint64_t runs;
    uint64_t last_started_ns;
    uint64_t last_finished_ns;
    uint64_t last_duration_ns;
    uint64_t gc_total_pause_ns;
    uint64_t gc_census_ns;
    uint64_t gc_root_count;
    uint64_t gc_root_words;
    uint64_t gc_scan_range_count;
    uint64_t gc_scan_words;
    uint64_t gc_worklist_origin_count;
    uint64_t gc_worklist_origin_words;
    uint64_t pool_live_allocs;
    uint64_t pool_live_bytes;
    uint64_t pool_leak_allocs;
    uint64_t pool_leak_bytes;
    uint64_t metadata_pool_count;
    uint64_t metadata_pool_mapped_bytes;
    uint64_t metadata_pool_records;
    uint64_t metadata_pool_slots;
    uint64_t arena_record_count;
    uint64_t arena_total_bytes;
    uint64_t arena_forwarded_count;
    uint64_t leak_sample_count;
    uint64_t leak_total_count;
    uint64_t leak_total_bytes;
    uint64_t suspicious_alloc_count;
    uint64_t suspicious_worklist_count;
    uint64_t slow_worklist_count;
    uint64_t site_live_top_count;
    const char *site_live_top_site[N00B_DEBUG_CENSUS_HEALTH_TOP_N];
    uint64_t site_live_top_allocs[N00B_DEBUG_CENSUS_HEALTH_TOP_N];
    uint64_t pool_live_top_count;
    const char *pool_live_top_site[N00B_DEBUG_CENSUS_HEALTH_TOP_N];
    uint64_t pool_live_top_bytes[N00B_DEBUG_CENSUS_HEALTH_TOP_N];
    uint64_t pool_live_top_allocs[N00B_DEBUG_CENSUS_HEALTH_TOP_N];
    uint64_t pool_leak_top_count;
    const char *pool_leak_top_site[N00B_DEBUG_CENSUS_HEALTH_TOP_N];
    uint64_t pool_leak_top_bytes[N00B_DEBUG_CENSUS_HEALTH_TOP_N];
    uint64_t pool_leak_top_allocs[N00B_DEBUG_CENSUS_HEALTH_TOP_N];
} n00b_debug_census_stats_t;

// ============================================================================
// GC root type
// ============================================================================

/*
 * Maintainer note: `n00b_gc_root_t` was relocated to
 * `include/n00b.h` so ncc's `--ncc-auto-gc-roots` transform can
 * emit static tables of this type from arbitrary TUs that include
 * only `n00b.h`. The Doxygen lives with the definition there.
 */

// ============================================================================
// Public API
// ============================================================================

/**
 * @brief Run a copying collection on @p arena.
 *
 * All live allocations reachable from the root set are copied to a
 * fresh segment; stale memory is unmapped.
 *
 * @param arena  The arena to collect.
 * @kw out_of_memory  True only when this collection is triggered by @p arena
 *                    actually running out of room (the n00b_arena_alloc
 *                    pressure path).  Gates the to-space growth heuristic:
 *                    only a genuine out-of-memory collect may pre-grow the
 *                    to-space.  Defaults false, so manual / test / marshal
 *                    collects never grow a low-traffic arena.
 * @pre @p arena has `collection_enabled` set.
 * @post All live allocations in @p arena have been relocated; old
 *       segments are unmapped and pointers are rewritten.
 *
 * @note Stops the world internally (acquires the critical_execution write lock,
 *       drains readers, suspends the other threads, scans, then restarts).  The
 *       caller need NOT — and normally should not — stop the world first.  A
 *       caller that has already stopped the world nests correctly via
 *       `rt->stw_nesting` (the inner stop/restart become no-ops), so the few
 *       sites that bracket their own stop-the-world around a collect remain
 *       safe.
 */
/**
 * @brief Whether collections pin every reached from-space object in place
 *        instead of copying (see n00b_collect_t.pin_all).
 *
 * Default: true when no link-time GC type map is linked into the process
 * (every DEFAULT scan is conservative, so forwarding would rewrite data words
 * that merely alias a from-space address, n00b#368), false otherwise.
 * Override with N00B_GC_PIN_ALL=0|1. Evaluated once, at the first collection.
 */
extern bool n00b_gc_pin_all_policy(void);

extern void
n00b_collect(n00b_arena_t *arena) _kargs
{
    bool out_of_memory = false;
};

/**
 * @brief Run the finalizers of objects earlier collections found unreachable.
 *
 * A collection does not run finalizers with the world stopped. It keeps each
 * unreachable finalizable object (and what its finalizer data references)
 * alive and queues the finalizer; the queue runs here, on the calling thread.
 * n00b_collect calls this itself once the world is running again, and an
 * allocation whose own out-of-memory collect queued finalizers runs them
 * before it returns. Returns at once while the world is stopped or another
 * thread is already draining the queue.
 */
extern void n00b_gc_run_finalizers(void);

/**
 * @brief Stop-the-world GC pass with leak-detection diagnostics
 *        enabled.
 *
 * Runs the normal collection cycle with @c rt->debug_leak_detect
 * temporarily set. Census data is captured while the world is stopped;
 * report bytes are built and published to the runtime stderr conduit only
 * after the collection has restarted the world.
 *
 * Useful as an on-demand debug knob (e.g. wired into a daemon's
 * periodic health tick) to pinpoint the origin of pool
 * allocations that never get freed. This is a compile-time debug
 * facility: unless libn00b is built with @c N00B_DEBUG or
 * @c N00B_DEBUG_LIVE_CENSUS, the function is present for link
 * compatibility but does nothing.
 */
extern void n00b_debug_find_leaks(void);

/**
 * @brief Run leak-detection census and publish the report to @p topic.
 *
 * @p topic must be a @c n00b_buffer_t * conduit topic. Collection and raw
 * census capture happen under STW; formatting and conduit publishing happen
 * after @c n00b_collect() has restarted the world. This is a no-op unless
 * libn00b is built with @c N00B_DEBUG or @c N00B_DEBUG_LIVE_CENSUS.
 */
extern void
n00b_debug_find_leaks_to_conduit(n00b_conduit_topic_t(n00b_buffer_t *) *topic);

/**
 * @brief Return the last completed debug-census summary.
 *
 * This is a cheap snapshot of counters captured by the most recent
 * @ref n00b_debug_find_leaks_to_conduit call. It never starts a census scan.
 * When libn00b is built with neither @c N00B_DEBUG nor
 * @c N00B_DEBUG_LIVE_CENSUS, @c enabled is false and all numeric counters
 * are zero.
 */
extern n00b_debug_census_stats_t n00b_debug_census_stats(void);

/**
 * @brief Arm/disarm the default-arena census on NATURAL collections.
 *
 * When armed, every collection that the runtime drives for its own reasons
 * (arena-pressure auto-collect, marshal, etc.) additionally walks the
 * default arena after the mark and publishes a per-origin-site occupancy
 * report (TOTAL vs LIVE vs RECLAIMED bytes/records) to the runtime stderr
 * conduit once the world restarts. It does NOT issue a collection itself
 * and does NOT change reclaim behaviour (unlike @ref n00b_debug_find_leaks,
 * which forces a leak-detect collect). Use it to observe what is landing in
 * the default heap under load without perturbing GC cadence.
 *
 * This is a compile-time debug facility: unless libn00b is built with
 * @c N00B_DEBUG or @c N00B_DEBUG_LIVE_CENSUS it is a no-op (and
 * @ref n00b_debug_census_on_collect_enabled always returns false).
 */
extern void n00b_debug_census_on_collect_set(bool enabled);

/**
 * @brief Report whether the natural-collection census is currently armed.
 */
extern bool n00b_debug_census_on_collect_enabled(void);

/**
 * @brief Register a memory range as a GC root.
 *
 * The collector will scan @p num_words pointer-sized words starting
 * at @p addr during every collection.  Use @ref n00b_gc_register_root
 * for the convenient macro interface.
 *
 * @param addr      Start address of the root region.
 * @param num_words Number of pointer-sized words to scan.
 * @pre Runtime must be initialized.
 * @pre @p addr must remain valid for the lifetime of the registration.
 */
extern void _n00b_gc_register_root(void *addr, size_t num_words);

/**
 * @brief Bulk-register an array of GC roots in one call.
 *
 * Init-time-only with respect to concurrent collection: callers
 * must invoke during process init or before any `n00b_collect()`
 * runs. Not thread-safe with concurrent collection.
 *
 * Runtime-resident code goes through the single-entry dedup path
 * immediately. Compiler-generated static roots are not expected to
 * call this during dynamic-loader construction; ncc emits
 * `n00b_gc_root_section_entry_t` records into the `n00b_gcroots`
 * linker section, and n00b registers those during `n00b_init()`.
 *
 * @param roots Pointer to an array of n00b_gc_root_t entries.
 *              The array must outlive the process (typically a
 *              `static` table emitted by ncc's `--ncc-auto-gc-roots`
 *              transform).
 * @param count Number of entries in @p roots. Zero is a clean
 *              no-op; @p roots may be `nullptr` when @p count is
 *              zero.
 */
extern void n00b_gc_register_roots(const n00b_gc_root_t *roots,
                                   size_t                count);

/**
 * @brief Internal: register compiler-emitted static GC root sections.
 *
 * Invoked from `n00b_init()` after `runtime->gc_roots` exists.
 * Enumerates `n00b_gc_root_section_entry_t` records emitted by ncc
 * into the `n00b_gcroots` linker section and replays each table
 * through `n00b_gc_register_roots()`.
 */
extern void _n00b_gc_register_static_roots(void);

/**
 * @brief Unregister a previously registered GC root.
 *
 * Removes the first root whose address matches @p addr.
 *
 * @param addr  The address originally passed to register.
 */
extern void _n00b_gc_unregister_root(void *addr);

/**
 * @brief Register a variable as a GC root.
 *
 * Takes the address of @p var and computes the number of
 * pointer-sized words from its size.
 *
 * @code
 *     int *my_ptr;
 *     n00b_gc_register_root(my_ptr);
 * @endcode
 */
#define n00b_gc_register_root(var)                                                             \
    _n00b_gc_register_root(&(var),                                                             \
                           (sizeof(var) + sizeof(void *) - 1) / sizeof(void *))

/**
 * @brief Unregister a variable previously registered as a GC root.
 */
#define n00b_gc_unregister_root(var) _n00b_gc_unregister_root(&(var))

// ============================================================================
// Constants
// ============================================================================

#define N00B_DEFAULT_GC_ARENA_SIZE (1 << 25) // 32 MiB for to-space initial
#define N00B_GC_WL_START_SIZE      256
#define N00B_TOO_FEW_ALLOCS        128

// ============================================================================
// Internal types (used by gc.c; exposed for arena.c helpers)
// ============================================================================

/**
 * @brief Work-list entry: a memory range that still needs scanning.
 *
 * `stride == 0` requests the legacy "every word in [0, num_words)" scan.
 * `stride > 0` requests a strided visit: words at indices
 * `offset, offset+stride, offset+2*stride, ...` while in
 * `[0, num_words)`.  Used by EVERY_OTHER (stride=2) and by GC clients
 * that want a struct-array pattern without a per-allocation callback.
 */
typedef struct {
    void     *start;
    uint64_t  num_words;
    uint64_t  stride;
    uint64_t  offset;
} n00b_gc_wl_item_t;

/**
 * @brief Per-collection memo of the conservative backward guard scan.
 *
 * Maps each 256-byte line a backward guard scan has crossed to the nearest
 * guard below it, so a later scan stops at the first such line it reaches and
 * each line is scanned once per memo. Valid only while no guard word in the
 * memoized mappings is written or erased, which holds for registered arena
 * segments and pool pages while the world is stopped.
 */
typedef struct {
    n00b_dict_untyped_t pages;     // page base -> uint64_t[page / 256] lines
    n00b_allocator_t   *allocator; // line arrays; freed with it
    uintptr_t           last_page; // most recent `pages` lookup
    uint64_t           *last_lines;
} n00b_guard_memo_t;

/**
 * @brief n00b_find_alloc_info for an address the caller already knows the
 * registry record of.
 *
 * `mmap` stands in for the global registry search when it covers `addr`, and
 * is ignored otherwise. A non-null `memo` memoizes the backward guard scan
 * that `scan_for_header` runs.
 */
extern n00b_alloc_info_t n00b_find_alloc_info_in(void              *addr,
                                                 n00b_mmap_info_t  *mmap,
                                                 bool               scan_for_header,
                                                 n00b_guard_memo_t *memo);

/**
 * @brief Per-collection state, stack-allocated by the collector entry point.
 */
typedef struct {
    n00b_arena_t                     *from_space;
    n00b_arena_t                     *to_space;
    n00b_pool_t                       work_pool;
    n00b_list_t(n00b_gc_wl_item_t *)  worklist;
    n00b_dict_untyped_t               memos;
    /* The runtime's gc_current_epoch value snapshotted at
     * collection start. The mark phase stamps this onto every
     * metadata-bearing alloc it reaches via the OOB record's
     * gc_epoch field; the post-mark sweep compares it back to
     * detect leaks (alloc still alive with a stale epoch). */
    uint64_t                          current_epoch;
    /* Per-collect transient interval tree of the gc-scannable arena/pool
     * segments (built in n00b_build_scan_tree, allocated from work_pool so it is
     * freed when work_pool is destroyed at cleanup). The conservative scan
     * queries this small GC-only tree per candidate word instead of the global
     * mmap interval tree — misses never touch the global tree, skipping both its
     * deep search and the per-word lazy 'unmanaged' registration that bloats it.
     * Opaque here; gc.c casts it to the private owner-payload tree type. */
    void                             *scan_tree;
    /* Fast-reject gate for the conservative scan: the union bounds of the
     * scan tree AND the cached static-object tree, computed once per collect
     * in n00b_build_scan_tree. The overwhelming majority of scanned words are
     * not pointers (ints, flags, text bytes); two compares reject them before
     * ANY tree descent. A word inside [scan_floor, scan_ceiling) still goes
     * through the trees for the real answer — the gate is purely a superset
     * pre-filter, so a widened range is safe, a narrowed one is not. */
    uint64_t                          scan_floor;
    uint64_t                          scan_ceiling;
    /* n00b#309 / #368: when set, every from-space object reached by the trace
     * is pinned in place (page-granular mark-sweep) instead of being copied,
     * and no scanned word is ever rewritten. This is the only sound mode when
     * heap words are ambiguous, i.e. when no link-time GC type map is present
     * (a consumer linked without the gcmap wrapper) and every DEFAULT scan is
     * conservative. Set from n00b_gc_pin_all_policy() in n00b_collect_setup. */
    bool                              pin_all;
    /* n00b#395: largest object forwarded into the to-space this collect.
     * The to-space arena is HIDDEN (unregistered) while the collect runs, so
     * n00b_forward_alloc has no registry record to record extents against.
     * The maximum is accumulated here instead -- single-writer, under
     * stop-the-world -- and seeded onto the segment's record at the moment the
     * to-space is registered as the live arena's segment. Without it, the
     * mapping holding the entire live heap would report "nothing recorded"
     * after every collect and the guard scan would fall back to the global
     * all-time high-water mark. */
    uint64_t                          to_space_max_alloc_len;
    /* From-space segment lookups made by the pin and finalizer paths this
     * collect, and the scan-tree nodes they visited. Published at cleanup as
     * n00b_gc_last_segment_lookups / n00b_gc_last_segment_lookup_steps. */
    uint64_t                          segment_lookups;
    uint64_t                          segment_lookup_steps;
    /* Interior-pointer resolutions outside the to-space share this memo, so
     * the mark scans each heap line once per collection. */
    n00b_guard_memo_t                 guard_memo;
} n00b_collect_t;

// ============================================================================
// Arena metric helpers (also used by arena.c)
// ============================================================================

/**
 * @brief Total bytes used across all segments of @p arena.
 * @param arena  Target arena.
 * @return       Byte count of live allocation space.
 */
static inline uint64_t
n00b_arena_used(n00b_arena_t *arena)
{
    uint64_t        sz      = 0;
    n00b_segment_t *segment = arena->current_segment;

    // The current (top) segment doesn't have last_addr set yet.
    segment->last_addr = atomic_load(&arena->next_alloc);

    while (segment) {
        sz     += (uint64_t)(segment->last_addr - segment->data);
        segment = segment->next_segment;
    }

    return sz;
}

/**
 * @brief Total capacity (usable bytes) across all segments of @p arena.
 * @param arena  Target arena.
 * @return       Byte count of total segment capacity.
 */
static inline uint64_t
n00b_arena_size(n00b_arena_t *arena)
{
    uint64_t        sz      = 0;
    n00b_segment_t *segment = arena->current_segment;

    while (segment) {
        sz     += segment->size;
        segment = segment->next_segment;
    }

    return sz;
}

/**
 * @brief Number of segments on @p arena's chain (diagnostics).
 */
static inline uint64_t
n00b_arena_segment_count(n00b_arena_t *arena)
{
    uint64_t        n       = 0;
    n00b_segment_t *segment = arena->current_segment;

    while (segment) {
        n++;
        segment = segment->next_segment;
    }

    return n;
}
