/**
 * @file mmaps.h
 * @brief Memory-mapping registry and helpers.
 *
 * Wraps mmap/munmap with a global interval-tree registry that tracks
 * every mapped region, its kind, and its owning allocator.  Also
 * provides sub-range registration for individual allocations within
 * larger mappings.
 */
#pragma once

#include "n00b.h"
#include "core/alloc_base.h"
#include "core/rt_access.h"
#include "adt/option.h"
#include "adt/result.h"
#include "core/alloc_mdata.h"
#include "core/align.h"
#include "core/atomic.h"
#include "core/macros.h"

#ifdef _WIN32
#include "core/platform.h"
#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define MAP_PRIVATE 0x02
#define MAP_ANON    0x20
#define MAP_FAILED  ((void *)-1)
#else
#include <sys/mman.h>
#endif
#include <assert.h>

#define N00B_MPROT (PROT_READ | PROT_WRITE)
#define N00B_MFLAG (MAP_PRIVATE | MAP_ANON)

typedef enum n00b_mmap_rec_kind_t n00b_mmap_rec_kind_t;

typedef struct {
    uint64_t total_count;
    uint64_t total_bytes;
    uint64_t static_count;
    uint64_t static_bytes;
    uint64_t arena_count;
    uint64_t arena_bytes;
    uint64_t managed_segment_count;
    uint64_t managed_segment_bytes;
    uint64_t system_segment_count;
    uint64_t system_segment_bytes;
    // "Phantom" memory: allocators that are deliberately OUT of the mmap registry
    // (so the collector never walks its own bookkeeping) and would otherwise be
    // unattributed. Tracked separately so every mapped byte in the process is
    // accountable. all_arena_bytes = sum of ALL live arenas (the audit ring,
    // includes the hidden md_pool metadata arenas + scratch + collection spaces);
    // registry_pool_bytes = the mmap interval-tree's own backing pool (it cannot
    // live in the registry it implements).
    uint64_t all_arena_bytes;
    uint64_t registry_pool_bytes;
    uint64_t zero_page_count;
    uint64_t zero_page_bytes;
    uint64_t unmanaged_count;
    uint64_t unmanaged_bytes;
    uint64_t stack_count;
    uint64_t stack_bytes;
    uint64_t internal_count;
    uint64_t internal_bytes;
    uint64_t pool_count;
    uint64_t pool_bytes;
    uint64_t api_mmap_count;
    uint64_t api_mmap_bytes;
    uint64_t skip_register_count;
    uint64_t skip_register_bytes;
    uint64_t safe_munmap_registry_count;
    uint64_t safe_munmap_registry_bytes;
    uint64_t safe_munmap_raw_count;
    uint64_t safe_munmap_raw_bytes;
    uint64_t safe_munmap_fail_count;
    uint64_t unknown_count;
    uint64_t unknown_bytes;
    uint64_t largest_bytes;
    uint64_t largest_kind;
    const char *largest_file;
    const char *largest_source_file;
    uint32_t largest_source_line;
    uint64_t largest_static_bytes;
    uint64_t largest_static_kind;
    const char *largest_static_file;
    const char *largest_static_source_file;
    uint32_t largest_static_source_line;
} n00b_mmap_registry_stats_t;

/**
 * @brief Look up an mmap record by address (internal — prefer n00b_mmap_by_address).
 * @param ctx  Mmap context to search.
 * @param addr Address to look up.
 * @return     Optional mmap info.
 */
extern n00b_option_t(n00b_mmap_info_t *) n00b_mmap_lookup(n00b_mmap_ctx_t *ctx, void *addr);

/**
 * @brief The registry fields an address lookup needs, copied out under the
 *        registry lock. A collection frees the from-space records, so only a
 *        caller that controls a mapping's lifetime reads its record directly.
 */
typedef struct {
    uint64_t             start;
    uint64_t             end;
    n00b_allocator_t    *allocator;
    uint64_t             max_alloc_len;
    n00b_mmap_rec_kind_t kind;
    n00b_mmap_perms_t    perms;
} n00b_mmap_view_t;

/**
 * @brief Copy the registry record covering @p addr into @p out.
 * @return false when no mapping covers @p addr.
 */
extern bool n00b_mmap_view_by_address(void *addr, n00b_mmap_view_t *out);

/**
 * @brief Copy @p info into @p out. Only for a caller that controls the
 *        mapping's lifetime, such as the collector under stop-the-world.
 */
extern void n00b_mmap_view_of(n00b_mmap_info_t *info, n00b_mmap_view_t *out);

#ifdef N00B_DEBUG
/**
 * @brief Debug-only hook run after a registry lookup has released the
 *        registry lock and before its caller reads the result.
 */
extern void (*n00b_mmap_debug_lookup_hook)(void *addr);
/**
 * @brief Debug-only: fill each freed mapping record with 0xdb and never
 *        reuse it, so a read of a freed record sees the pattern.
 */
extern bool n00b_mmap_debug_poison_freed_records;
#endif

extern n00b_mmap_registry_stats_t n00b_mmap_registry_stats(void);

/**
 * @brief Hard cap on how many kernel-probed foreign pages the registry caches.
 *        See n00b_mmap_register_probe_page (n00b#213).
 */
#ifndef N00B_MMAP_PROBE_CACHE_ENTRIES
#define N00B_MMAP_PROBE_CACHE_ENTRIES 1024
#endif

/**
 * @brief Register a single kernel-probed foreign page in a BOUNDED cache.
 *
 * Used by n00b_check_kernel_page_map to cache "something is mapped here" for an
 * address n00b does not own. Unlike every other registration, this one has no
 * unmap path to unregister it, so it is capped: a fixed ring of records is
 * reused in FIFO order and the total number of probe-cached records never
 * exceeds N00B_MMAP_PROBE_CACHE_ENTRIES (n00b#213). Evicted records are
 * recycled in place, never freed, so a pointer already handed to a caller
 * stays valid.
 *
 * @param startp Page start.
 * @param endp   Page end (exclusive).
 * @param kind   Record kind (unmanaged, or zero page).
 * @param perms  Permissions observed by the probe.
 */
extern n00b_option_t(n00b_mmap_info_t *)
n00b_mmap_register_probe_page(void                *startp,
                              void                *endp,
                              n00b_mmap_rec_kind_t kind,
                              n00b_mmap_perms_t    perms);

/** @brief Count of probe-cache evictions; nonzero once the cap is reached. */
extern _Atomic(uint64_t) n00b_mmap_probe_evictions;

// Total mapped bytes across ALL live arenas (debug audit ring) — including the
// hidden/no_map ones (GC md_pool metadata arenas, scratch arenas, collection
// spaces) that are out of the mmap registry. Returns 0 unless the arena audit is
// compiled in. See arena.c.
extern uint64_t n00b_arena_audit_total_bytes(void);

// Refresh the audit snapshot: stops the world, walks every live allocator
// (arenas AND pools — pools are arenas too for census), and caches both the
// total and a per-(debug_name) breakdown. Call from a periodic heartbeat (debug
// only); no-op unless the audit is compiled in.
extern void n00b_arena_audit_census(void);

// Same refresh, but WITHOUT stopping the world: the caller must already have all
// other threads frozen. Called from inside n00b_collect so the audit rides the
// collection's existing stop-the-world (no extra pause, no status-path STW
// livelock). No-op unless the audit is compiled in.
extern void n00b_arena_audit_census_nolock(void);

// Register/unregister an allocator (arena or pool) in the audit ring. Arenas are
// wired automatically in n00b_initialize_arena/delete; pools call these from
// n00b_pool_init_at / pool_destroy. No-ops unless the audit is compiled in.
extern void n00b_allocator_audit_register(n00b_allocator_t *a);
extern void n00b_allocator_audit_unregister(n00b_allocator_t *a);

// One arena-census bucket: all live arenas sharing a debug_name (e.g. "md_pool"
// = the GC's OOB metadata arenas, "to-space" = collection space, "arena" =
// scratch), with their live count and summed segment bytes.
typedef struct n00b_arena_census_bucket_t {
    const char *name;
    uint64_t    count;
    uint64_t    bytes;
} n00b_arena_census_bucket_t;

// Fill out[] (capacity cap) from the last census snapshot, top buckets by bytes
// descending; returns the number written. Reads the cache (no STW, no lock).
extern uint32_t n00b_arena_audit_histogram(n00b_arena_census_bucket_t *out,
                                           uint32_t                    cap);

/**
 * @brief One source-location bucket of the mmap-registry histogram:
 *        how many live registered segments (and total bytes) were allocated
 *        from a given (source_file:source_line).
 */
typedef struct n00b_mmap_site_t {
    const char *source_file;
    uint32_t    source_line;
    uint64_t    count;
    uint64_t    bytes;
} n00b_mmap_site_t;

/**
 * @brief Histogram of currently-registered mmap segments grouped by the
 *        (source_file:source_line) that allocated them. Fills `out` (capacity
 *        `cap`) with the top buckets by segment count, descending, and returns
 *        the number of buckets written. Read-only; takes the registry read lock.
 */
extern uint32_t n00b_mmap_source_histogram(n00b_mmap_site_t *out, uint32_t cap);

/**
 * @brief Register an mmap'd region in the global registry.
 * @param startp Start address of the mapping.
 * @param endp   End address (exclusive).
 * @param kind   Kind of mapping (arena, pool, etc.).
 *
 * @kw runtime          Runtime whose mmap context to use.
 * @kw file             Debug file name for this mapping.
 * @kw allocator        Allocator owning the mapping.
 * @kw binary_offset    Offset within a mapped binary file.
 * @kw slide            ASLR slide for the mapping.
 * @kw order_id         Insertion order identifier.
 * @kw perms            Known mapping permissions, if authoritative.
 * @kw definitely_unique If true, skip duplicate checks on insert.
 */
extern n00b_option_t(n00b_mmap_info_t *)
_n00b_mmap_register(void *startp, void *endp, n00b_mmap_rec_kind_t kind) _kargs
{
    n00b_runtime_t   *runtime           = n00b_get_runtime();
    const char       *file              = nullptr;
    const char       *source_file       = nullptr;
    uint32_t          source_line       = 0;
    n00b_allocator_t *allocator         = nullptr;
    uint64_t          binary_offset     = 0;
    intptr_t          slide             = 0;
    uint64_t          order_id          = 0;
    n00b_mmap_perms_t perms             = n00b_mmap_perms_unknown;
    bool              definitely_unique = true;
};

#define n00b_mmap_register(startp, endp, kind, ...)                                      \
    _n00b_mmap_register((startp),                                                        \
                        (endp),                                                          \
                        (kind),                                                          \
                        .source_file = __FILE__,                                         \
                        .source_line = __LINE__ __VA_OPT__(, __VA_ARGS__))

/**
 * @brief Register a page that backs an @ref n00b_pool_t allocator.
 *
 * @param startp    Start address of the page.
 * @param endp      End address of the page (exclusive).
 * @param allocator Owning pool allocator.
 * @param file      Caller location for diagnostics.
 *
 * Narrow internal entry point intended only for @ref new_page_entry
 * in pool.c. Unlike @ref n00b_mmap_register this does NOT short-
 * circuit when @c allocator->hidden is set — that's the point.
 * @ref n00b_mem_get_allocator can then resolve a pointer in the
 * page back to the owning hidden pool, which is what makes
 * @ref n00b_free work for hidden+metadata pools (rt->user_pool in
 * particular).
 *
 * The GC-scan path is unaffected: @ref n00b_mmap_is_gc_scannable
 * still keys off @c allocator->hidden / @c metadata_pool, so a
 * hidden pool's pages remain opaque to conservative scanning;
 * registering them here only enables address-to-allocator lookup.
 *
 * Kind is fixed at @c n00b_mmap_pool.
 */
extern n00b_option_t(n00b_mmap_info_t *)
_n00b_mmap_register_pool_page(void *startp,
                               void *endp,
                               n00b_allocator_t *allocator,
                               const char *file,
                               const char *source_file,
                               uint32_t source_line);

#define n00b_mmap_register_pool_page(startp, endp, allocator, file)                     \
    _n00b_mmap_register_pool_page((startp),                                             \
                                  (endp),                                               \
                                  (allocator),                                          \
                                  (file),                                               \
                                  __FILE__,                                             \
                                  __LINE__)

/**
 * @brief Async-signal-handler-safe lookup for SIGBUS / SIGSEGV
 *        handlers.  Pulls registry fields for a candidate faulting
 *        address without allocating or calling any n00b string /
 *        print machinery.
 */
extern void *
n00b_mmap_handler_lookup(uintptr_t addr,
                         uint64_t *out_start,
                         uint64_t *out_end,
                         uint32_t *out_kind,
                         const char **out_file,
                         const char **out_source_file,
                         uint32_t *out_source_line);


/**
 * @brief Unregister an mmap'd region.
 * @param start Start address of the mapping to remove.
 *
 * @kw runtime Runtime whose mmap context to use.
 */
extern void
n00b_mmap_unregister(void *start) _kargs
{
    n00b_runtime_t *runtime = n00b_get_runtime();
};

/**
 * @brief Allocate memory via mmap.  Use the n00b_mmap() macro.
 * @param sz  Number of bytes to map.
 * @param loc Source location string (auto-filled by macro).
 *
 * @kw allocator Allocator to associate with the mapping.
 * @kw kind      Mmap record kind (default n00b_mmap_api_mmap).
 * @kw name      Debug name for the mapping.
 *
 * @pre @p sz > 0.
 * @post On success, the mapped region is registered in the global mmap registry.
 */
// clang-format off
extern n00b_result_t(void *)
_n00b_mmap(size_t sz, char *loc) _kargs
{
    n00b_allocator_t    *allocator     = nullptr;
    n00b_mmap_rec_kind_t kind          = n00b_mmap_api_mmap;
    char                *name          = nullptr;
    /* When true, allocate the page but don't enter it in the mmap
     * tree. The caller is then responsible for registering (and
     * unregistering) the page itself — currently only @c pool.c
     * does so, via @ref n00b_mmap_register_pool_page paired with
     * @ref n00b_mmap_unregister at free time. The skip avoids the
     * implicit @ref n00b_mmap_register that would otherwise filter
     * by @c allocator->hidden, leaving the caller in full control
     * of which pages are visible to @ref n00b_mem_get_allocator. */
    bool                 skip_register = false;
};
// clang-format on

#define n00b_mmap(sz, ...) _n00b_mmap(sz, N00B_LOC_STRING() __VA_OPT__(, __VA_ARGS__))

/**
 * @brief Unmap a previously mmap'd region.
 * @param addr Start address to unmap.
 *
 * @kw runtime Runtime whose mmap context to use.
 *
 * @pre @p addr was returned by a prior n00b_mmap() call.
 * @post The mapping is removed from the global registry and memory is released.
 */
// clang-format off
extern n00b_result_t(int)
n00b_munmap(void *addr) _kargs
{
    n00b_runtime_t *runtime = n00b_get_runtime();
};
// clang-format on

/**
 * @brief Record that an allocation of @p len bytes was placed in @p map.
 *
 * Keeps a per-mapping, monotonically rising high-water mark of the largest
 * allocation the mapping has ever held.  Because an allocation always lies
 * WHOLLY inside one registered mapping (arena.c sizes a fresh segment to the
 * request that did not fit, and never writes an object across segment_end),
 * this value can never be smaller than a live allocation inside the mapping --
 * which is exactly the property the conservative backward guard scan needs to
 * stop early without under-scanning a live object (n00b#395, n00b#321).
 *
 * Callers must record BEFORE the allocation's guard word becomes visible, so
 * that any scan able to find the guard also sees a bound that covers it.
 *
 * @param map Mapping the allocation was placed in (nullptr is a no-op).
 * @param len Byte length reserved for the allocation, header included.
 */
[[n00b::nogc]] static inline void
n00b_mmap_note_alloc_len(n00b_mmap_info_t *map, uint64_t len)
{
    if (map == nullptr) {
        return;
    }

    uint64_t prev = atomic_load_explicit(&map->max_alloc_len,
                                         memory_order_relaxed);
    while (len > prev) {
        if (atomic_compare_exchange_weak(&map->max_alloc_len, &prev, len)) {
            return;
        }
    }
}

/**
 * @brief Check whether a mapping is an arena segment.
 * @param map Mmap info to check.
 * @return    true if managed or system segment.
 */
static inline bool
n00b_mmap_is_arena_segment(n00b_mmap_info_t *map)
{
    switch (map->kind) {
    case n00b_mmap_managed_segment:
    case n00b_mmap_sys_segment:
        return true;
    default:
        return false;
    }
}

/**
 * @brief Check whether a mapping is an arena root (not a segment).
 * @param map Mmap info to check.
 * @return    true if arena root.
 */
static inline bool
n00b_mmap_is_arena(n00b_mmap_info_t *map)
{
    switch (map->kind) {
    case n00b_mmap_arena:
        return true;
    default:
        return false;
    }
}

static inline n00b_mmap_rec_kind_t
n00b_mmap_get_kind(n00b_mmap_info_t *map)
{
    return map->kind;
}

/**
 * @brief Look up mmap info by address using the global registry.
 * @param addr Address to look up.
 * @return     Optional mmap info.
 *
 * @kw runtime Runtime whose mmap context to use.
 */
extern n00b_option_t(n00b_mmap_info_t *)
n00b_mmap_by_address(void *addr) _kargs
{
    n00b_runtime_t *runtime = n00b_get_runtime();
};

/**
 * @brief Look up a registered allocation/static-object sub-range by address.
 * @param addr Address to look up.
 * @return     Optional range descriptor covering @p addr.
 *
 * @kw runtime Runtime whose mmap context to use.
 */
extern n00b_option_t(n00b_alloc_range_t *)
n00b_mmap_range_by_address(void *addr) _kargs
{
    n00b_runtime_t *runtime = n00b_get_runtime();
};

typedef n00b_option_t(n00b_allocator_t *) n00b_allocator_opt_t;

/**
 * @brief Find the allocator owning an address.
 * @param addr Address to look up.
 * @return     Optional allocator pointer.
 *
 * @kw runtime Runtime whose mmap context to use.
 */
extern n00b_allocator_opt_t
n00b_mem_get_allocator(void *addr) _kargs
{
    n00b_runtime_t *runtime = n00b_get_runtime();
};

/**
 * @brief Check whether a mapping is GC-managed.
 * @param map Mmap info to check.
 * @return    true if managed segment, system segment, or pool.
 */
static inline bool
n00b_mmap_is_managed(n00b_mmap_info_t *map)
{
    if (!map) {
        return false;
    }
    switch (map->kind) {
    case n00b_mmap_managed_segment:
    case n00b_mmap_sys_segment:
    case n00b_mmap_pool:
        return true;
    default:
        return false;
    }
}

// Count of raw munmap/VirtualFree failures from n00b_safe_munmap (e.g. a
// partial-range unmap in the GC page reclaim that the kernel rejected). A
// nonzero value means pages were not returned to the OS — a silent leak.
extern _Atomic(uint64_t) n00b_munmap_fail_count;
extern _Atomic(uint64_t) n00b_mmap_skip_register_count;
extern _Atomic(uint64_t) n00b_mmap_skip_register_bytes;
extern _Atomic(uint64_t) n00b_safe_munmap_registry_count;
extern _Atomic(uint64_t) n00b_safe_munmap_registry_bytes;
extern _Atomic(uint64_t) n00b_safe_munmap_raw_count;
extern _Atomic(uint64_t) n00b_safe_munmap_raw_bytes;

#ifdef _WIN32
extern bool n00b_win_free_range(void *addr, size_t size);
#endif

/**
 * @brief Unmap a region, handling both registered and hidden pages.
 *
 * Tries n00b_munmap() first (removes the mmap record and unmaps).
 * If the region is not registered (e.g. hidden allocator pages),
 * falls back to raw munmap with the provided size.
 *
 * This is the canonical cleanup path for allocator implementations
 * that may operate in hidden mode.
 *
 * @param addr Address to unmap.
 * @param size Size of the mapping (used as fallback for hidden pages).
 */
static inline void
n00b_safe_munmap(void *addr, size_t size)
{
    bool use_registry = false;
    auto map_opt      = n00b_mmap_by_address(addr);

    if (n00b_option_is_set(map_opt)) {
        n00b_mmap_info_t *map = n00b_option_get(map_opt);
        uint64_t          lo  = (uint64_t)addr;
        uint64_t          hi  = lo + (uint64_t)size;

        use_registry = map->start == lo && map->end == hi && hi >= lo;
    }

    if (use_registry) {
        auto r = n00b_munmap(addr);
        if (n00b_result_is_ok(r)) {
            n00b_atomic_add(&n00b_safe_munmap_registry_count, 1);
            n00b_atomic_add(&n00b_safe_munmap_registry_bytes, (uint64_t)size);
            return;
        }
    }
#ifdef _WIN32
    n00b_atomic_add(&n00b_safe_munmap_raw_count, 1);
    n00b_atomic_add(&n00b_safe_munmap_raw_bytes, (uint64_t)size);
    if (!n00b_win_free_range(addr, size)) {
        n00b_atomic_add(&n00b_munmap_fail_count, 1);
    }
#else
    // Partial-range unmaps (GC page reclaim) take this raw path — the registry
    // only matches whole segments.  A failure here means pages were NOT returned
    // to the kernel (a silent leak), so surface it via a counter instead of
    // dropping munmap's return on the floor.
    n00b_atomic_add(&n00b_safe_munmap_raw_count, 1);
    n00b_atomic_add(&n00b_safe_munmap_raw_bytes, (uint64_t)size);
    if (munmap(addr, size) != 0) {
        n00b_atomic_add(&n00b_munmap_fail_count, 1);
    }
#endif
}

/**
 * @brief Register a sub-range (individual allocation) within an existing mmap.
 * @param start Start address of the sub-range.
 * @param end   End address (exclusive) of the sub-range.
 * @param kind  Kind of sub-range.
 *
 * @kw allocator Allocator owning the sub-range.
 * @kw file      Debug file name.
 */
extern n00b_alloc_range_t *
n00b_mmap_register_range(void *start, void *end, n00b_mmap_rec_kind_t kind) _kargs
{
    n00b_allocator_t    *allocator = nullptr;
    const char          *file      = nullptr;
    n00b_gc_scan_kind_t  scan_kind = N00B_GC_SCAN_KIND_DEFAULT;
    n00b_gc_scan_cb_t    scan_cb   = nullptr;
    void                *scan_user = nullptr;
    n00b_alloc_type_info_t tinfo    = 0;
    uint64_t             object_id = 0;
    const n00b_static_identity_t *identity = nullptr;
    uint32_t             flags     = 0;
};

/**
 * @brief Register a static object descriptor in the global mmap/range tree.
 * @param start Object start address.
 * @param len   Object size in bytes.
 * @param tinfo Runtime type hash for the object, or 0 when unknown.
 * @param loc   Source location string.
 *
 * @kw scan_kind GC scan shape for the object's words.
 * @kw scan_cb   Optional callback used when scan_kind == CALLBACK.
 * @kw scan_user Opaque callback data.
 * @kw object_id Stable generated-object identity, if available.
 * @kw identity  Portable source-semantic identity, if available.
 * @kw flags     Reserved descriptor flags for generated-code contracts.
 */
extern n00b_alloc_range_t *
_n00b_static_object_register(void *start,
                             size_t len,
                             n00b_alloc_type_info_t tinfo,
                             const char *loc) _kargs
{
    n00b_gc_scan_kind_t scan_kind = N00B_GC_SCAN_KIND_DEFAULT;
    n00b_gc_scan_cb_t   scan_cb   = nullptr;
    void               *scan_user = nullptr;
    uint64_t            object_id = 0;
    const n00b_static_identity_t *identity = nullptr;
    uint32_t            flags     = 0;
};

#define n00b_static_object_register(start, len, tinfo, ...)                                   \
    _n00b_static_object_register((start),                                                     \
                                 (len),                                                       \
                                 (tinfo),                                                     \
                                 N00B_LOC_STRING() __VA_OPT__(, __VA_ARGS__))

/**
 * @brief Delete all sub-ranges overlapping [start, end).
 * @param ctx   Mmap context.
 * @param start Start of the range to clear.
 * @param end   End of the range to clear (exclusive).
 */
extern void n00b_mmap_delete_ranges(n00b_mmap_ctx_t *ctx, uint64_t start, uint64_t end);

#define n00b_global_mem_map(rt) (&(rt->mmaps))
