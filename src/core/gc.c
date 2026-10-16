/*
 * @file gc.c
 * @brief Copying/compacting garbage collector for n00b arenas.
 *
 * Ported from the original gc.nc.  The algorithm is unchanged: when
 * an arena is full, we create a "to-space", trace roots
 * (user-registered roots, runtime argv/envp, thread stacks), copy
 * live allocations, rewrite pointers, and swap segments.
 */

#define N00B_MEM_INTERNAL_API
#define N00B_USE_INTERNAL_API

#include <string.h>
#include <assert.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#if !defined(_WIN32)
#include <sys/mman.h>
#endif

// Debug aid: when defined, n00b_reclaim_pinned_pages PROT_NONE-poisons unpinned
// from-space runs instead of returning them to the kernel.  Poisoned pages can't
// be reused, so any stale dereference faults at the exact address — turning the
// async-seal use-after-reclaim race into a deterministic, reuse-free crash.
// Leaks address space; debug-only.  Define to enable (off by default).
// #define N00B_GC_POISON_RECLAIM 1

// Debug aid: when defined, the per-allocation-site live census (default-arena
// by-site occupancy, pool census, GC pass timing, leak sampling) is compiled
// in and can be armed via n00b_debug_census_on_collect_set(true) to fire on
// every NATURAL collection.  A full N00B_DEBUG build implies it.  This flag
// exists so the census can be enabled in an otherwise-non-debug build (e.g. a
// production-shaped crayon-gw) the same way N00B_GC_POISON_RECLAIM is: define
// here, or pass -DN00B_DEBUG_LIVE_CENSUS on the command line.  Off by default;
// when neither this flag nor N00B_DEBUG is set the census code (and its cost)
// compiles out entirely and the public API degrades to no-op stubs.
// #define N00B_DEBUG_LIVE_CENSUS 1

// Internal umbrella: the census facility is present whenever either a full
// debug build (N00B_DEBUG) or the standalone census flag is set.  All
// census-only code in this file is guarded by N00B_CENSUS_ENABLED, NOT by
// N00B_DEBUG directly, so the standalone flag turns it on by itself.
#if defined(N00B_DEBUG) || defined(N00B_DEBUG_LIVE_CENSUS)
#define N00B_CENSUS_ENABLED 1
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include "n00b.h"
#include "util/assert.h"
#include "conduit/write.h"
#include "core/syscall.h" // n00b_raw_write — STW-safe direct-fd census emit
#include "core/gc.h"
#include "core/gc_map.h"
// Full GC stack-map / policy struct + enum defs (this TU defines the stack API).
#include "core/codegen_abi_inject.h"
#include "core/gc_stack.h"
#include "core/stw.h"
#include "core/alloc_mdata.h"
#include "core/alloc.h"
#include "core/memory_info.h"
#include "core/arena.h"
#include "core/atomic.h"
#include "core/buffer.h"
#include "core/thread.h"
#include "core/lock_common.h"
#include "core/align.h"
#include "core/mmaps.h"
#include "core/runtime.h"
#include "core/pool.h"
#include "core/rt_access.h"
#include "core/time.h"
#include "adt/option.h"
#include "adt/dict_untyped.h"
#include "adt/dict.h"
#include "core/oob_md_dict.h"

#if defined(N00B_CENSUS_ENABLED)
/* Diagnostic: per-allocation-site live census. File-static; only gc.c
 * recompiles. Active only during a debug_leak_detect collect. Typed dicts
 * are keyed by the OOB site pointer bits as uint64_t (ncc typeid does not
 * normalize `const char *`) and live in one discardable NON-GC census arena.
 * The collector records raw counts while STW is active; formatting and
 * conduit publishing happen only after n00b_collect() restarts the world. */
typedef n00b_dict_t(uint64_t, int64_t) n00b_site_census_dict_t;

#define N00B_DEBUG_CENSUS_TOP_N           40
#define N00B_DEBUG_CENSUS_LEAK_SAMPLE_MAX 128

typedef struct {
    const char *pool_name;
    const char *site_name;
    void       *user_ptr;
    uint64_t    tinfo;
    uint64_t    alloc_len;
} n00b_debug_leak_sample_t;

typedef struct {
    uint64_t key;
    uint64_t primary;
    uint64_t count;
} n00b_debug_census_row_t;

typedef struct {
    uint64_t    alloc_kind;
    uint64_t    alloc_len;
    uint64_t    ptr_words;
    uint64_t    ptr_words_known;
    uint64_t    scan_kind;
    uint64_t    no_scan;
    uint64_t    tinfo;
    const char *site;
} n00b_debug_alloc_origin_t;

typedef struct {
    n00b_arena_t     *arena;
    n00b_allocator_t *allocator;

    n00b_site_census_dict_t *site_live_count;

    n00b_site_census_dict_t *pool_live_bytes;
    n00b_site_census_dict_t *pool_live_count;
    n00b_site_census_dict_t *pool_leak_bytes;
    n00b_site_census_dict_t *pool_leak_count;
    uint64_t                 pool_live_allocs;
    uint64_t                 pool_live_bytes_total;
    uint64_t                 pool_leak_allocs;
    uint64_t                 pool_leak_bytes_total;
    uint64_t                 metadata_pool_count;
    uint64_t                 metadata_pool_mapped_bytes;
    uint64_t                 metadata_pool_records;
    uint64_t                 metadata_pool_slots;

    // Per-origin-site GC default-arena census.  arena_site_bytes/count are
    // TOTAL (every record in from_space at collect time); arena_site_live_*
    // are the subset reached by this collect (gc_epoch == current_epoch).
    // reclaimed = total - live, per site, computed at emit.  This is the full
    // "what is allocated into the GC heap, by site" audit.
    n00b_site_census_dict_t *arena_site_bytes;
    n00b_site_census_dict_t *arena_site_count;
    n00b_site_census_dict_t *arena_site_live_bytes;
    n00b_site_census_dict_t *arena_site_live_count;
    const char              *arena_name;
    uint64_t                 arena_record_count;
    uint64_t                 arena_total_bytes;
    uint64_t                 arena_live_record_count;
    uint64_t                 arena_live_bytes_total;
    uint64_t                 arena_forwarded_count;
    bool                     arena_seen;

    n00b_debug_leak_sample_t *leak_samples;
    uint64_t                  leak_sample_count;
    uint64_t                  leak_sample_capacity;
    uint64_t                  leak_total_count;
    uint64_t                  leak_total_bytes;

    uint64_t suspicious_alloc_count;
    uint64_t suspicious_worklist_count;
    uint64_t slow_worklist_count;

    uint64_t    gc_total_pause_ns;
    uint64_t    gc_stop_ns;
    uint64_t    gc_collect_ns;
    uint64_t    gc_restart_ns;
    uint64_t    gc_internal_ns;
    uint64_t    gc_setup_ns;
    uint64_t    gc_roots_ns;
    uint64_t    gc_runtime_scan_ns;
    uint64_t    gc_worklist_roots_ns;
    uint64_t    gc_thread_scan_ns;
    uint64_t    gc_worklist_threads_ns;
    uint64_t    gc_metadata_scan_ns;
    uint64_t    gc_metadata_worklist_ns;
    uint64_t    gc_census_ns;
    uint64_t    gc_pool_sweep_ns;
    uint64_t    gc_foreign_reap_ns;
    uint64_t    gc_finalizers_ns;
    uint64_t    gc_cleanup_ns;
    uint64_t    gc_root_count;
    uint64_t    gc_root_words;
    uint64_t    gc_root_max_words;
    uint64_t    gc_root_max_addr;
    uint64_t    gc_root_max_index;
    uint64_t    gc_root_slowest_ns;
    uint64_t    gc_root_slowest_words;
    uint64_t    gc_root_slowest_addr;
    uint64_t    gc_root_slowest_index;
    uint64_t    gc_scan_range_count;
    uint64_t    gc_scan_words;
    uint64_t    gc_scan_max_words;
    uint64_t    gc_scan_max_addr;
    uint64_t    gc_scan_max_alloc_kind;
    uint64_t    gc_scan_max_alloc_len;
    uint64_t    gc_scan_max_ptr_words;
    uint64_t    gc_scan_max_ptr_words_known;
    uint64_t    gc_scan_max_scan_kind;
    uint64_t    gc_scan_max_no_scan;
    uint64_t    gc_scan_max_tinfo;
    const char *gc_scan_max_site;
    uint64_t    gc_worklist_origin_count;
    uint64_t    gc_worklist_origin_words;
    uint64_t    gc_worklist_origin_max_words;
    uint64_t    gc_worklist_origin_max_addr;
    uint64_t    gc_worklist_origin_max_alloc_kind;
    uint64_t    gc_worklist_origin_max_alloc_len;
    uint64_t    gc_worklist_origin_max_ptr_words;
    uint64_t    gc_worklist_origin_max_ptr_words_known;
    uint64_t    gc_worklist_origin_max_scan_kind;
    uint64_t    gc_worklist_origin_max_no_scan;
    uint64_t    gc_worklist_origin_max_tinfo;
    const char *gc_worklist_origin_max_site;
    bool        gc_out_of_memory;
} n00b_debug_census_t;

static n00b_site_census_dict_t *g_site_census         = nullptr;
static n00b_debug_census_t     *g_debug_census        = nullptr;
static _Atomic(bool)            g_debug_census_active = false;

static _Atomic uint64_t   g_debug_census_runs;
static _Atomic uint64_t   g_debug_census_last_started_ns;
static _Atomic uint64_t   g_debug_census_last_finished_ns;
static _Atomic uint64_t   g_debug_census_last_duration_ns;
static _Atomic uint64_t   g_debug_census_gc_total_pause_ns;
static _Atomic uint64_t   g_debug_census_gc_census_ns;
static _Atomic uint64_t   g_debug_census_gc_root_count;
static _Atomic uint64_t   g_debug_census_gc_root_words;
static _Atomic uint64_t   g_debug_census_gc_scan_range_count;
static _Atomic uint64_t   g_debug_census_gc_scan_words;
static _Atomic uint64_t   g_debug_census_gc_worklist_origin_count;
static _Atomic uint64_t   g_debug_census_gc_worklist_origin_words;
static _Atomic uint64_t   g_debug_census_pool_live_allocs;
static _Atomic uint64_t   g_debug_census_pool_live_bytes;
static _Atomic uint64_t   g_debug_census_pool_leak_allocs;
static _Atomic uint64_t   g_debug_census_pool_leak_bytes;
static _Atomic uint64_t   g_debug_census_metadata_pool_count;
static _Atomic uint64_t   g_debug_census_metadata_pool_mapped_bytes;
static _Atomic uint64_t   g_debug_census_metadata_pool_records;
static _Atomic uint64_t   g_debug_census_metadata_pool_slots;
static _Atomic uint64_t   g_debug_census_arena_record_count;
static _Atomic uint64_t   g_debug_census_arena_total_bytes;
static _Atomic uint64_t   g_debug_census_arena_forwarded_count;
static _Atomic uint64_t   g_debug_census_leak_sample_count;
static _Atomic uint64_t   g_debug_census_leak_total_count;
static _Atomic uint64_t   g_debug_census_leak_total_bytes;
static _Atomic uint64_t   g_debug_census_suspicious_alloc_count;
static _Atomic uint64_t   g_debug_census_suspicious_worklist_count;
static _Atomic uint64_t   g_debug_census_slow_worklist_count;
static _Atomic uint64_t   g_debug_census_site_live_top_count;
static _Atomic(uintptr_t) g_debug_census_site_live_top_site[N00B_DEBUG_CENSUS_HEALTH_TOP_N];
static _Atomic uint64_t   g_debug_census_site_live_top_allocs[N00B_DEBUG_CENSUS_HEALTH_TOP_N];
static _Atomic uint64_t   g_debug_census_pool_live_top_count;
static _Atomic(uintptr_t) g_debug_census_pool_live_top_site[N00B_DEBUG_CENSUS_HEALTH_TOP_N];
static _Atomic uint64_t   g_debug_census_pool_live_top_bytes[N00B_DEBUG_CENSUS_HEALTH_TOP_N];
static _Atomic uint64_t   g_debug_census_pool_live_top_allocs[N00B_DEBUG_CENSUS_HEALTH_TOP_N];
static _Atomic uint64_t   g_debug_census_pool_leak_top_count;
static _Atomic(uintptr_t) g_debug_census_pool_leak_top_site[N00B_DEBUG_CENSUS_HEALTH_TOP_N];
static _Atomic uint64_t   g_debug_census_pool_leak_top_bytes[N00B_DEBUG_CENSUS_HEALTH_TOP_N];
static _Atomic uint64_t   g_debug_census_pool_leak_top_allocs[N00B_DEBUG_CENSUS_HEALTH_TOP_N];
#else
#define n00b_debug_census_finish_phase(slot, phase_start_ns) ((void)0)
#define n00b_debug_census_record_pass_timing(pause_start_ns,                                   \
                                             stop_done_ns,                                     \
                                             restart_start_ns,                                 \
                                             pause_done_ns)                                    \
    ((void)0)
#define n00b_debug_census_record_scan_range(start, nwords)              ((void)0)
#define n00b_debug_census_record_worklist_origin(origin, start, nwords) ((void)0)
#define n00b_debug_census_record_leak(allocator, oob)                   ((void)0)
#define n00b_debug_census_record_suspicious_alloc()                     ((void)0)
#define n00b_debug_census_record_suspicious_worklist()                  ((void)0)
#define n00b_debug_census_record_slow_worklist()                        ((void)0)
#endif

// ============================================================================
// Forward declarations
// ============================================================================

static void n00b_collect_setup(n00b_collect_t *, n00b_arena_t *, bool);
static void n00b_scan_memory_range(n00b_collect_t *, void *, size_t);
static void n00b_process_worklist(n00b_collect_t *);
static bool
n00b_visit_possible_pointer(n00b_collect_t *ctx, uint64_t **base, size_t i, bool base_checked);
static void n00b_collection_cleanup(n00b_collect_t *);
static void n00b_process_finalizers(n00b_collect_t *);
static void n00b_scan_pending_finalizers(n00b_collect_t *);
static void n00b_gc_settle_tlabs(n00b_arena_t *arena, bool retire);
static void n00b_scan_metadata_pools(n00b_collect_t *);
static void n00b_sweep_metadata_pool_leaks(n00b_collect_t *);
#if defined(N00B_CENSUS_ENABLED)
static void n00b_debug_pool_census(uint64_t live_epoch);
static void n00b_debug_arena_census(n00b_collect_t *ctx);
static void n00b_debug_census_record_leak(n00b_allocator_t *allocator, n00b_oob_hdr_t *oob);
static void n00b_debug_census_record_suspicious_alloc(void);
static void n00b_debug_census_record_suspicious_worklist(void);
static void n00b_debug_census_record_slow_worklist(void);
static void n00b_debug_census_publish(n00b_debug_census_t *census,
                                      n00b_conduit_topic_t(n00b_buffer_t *) * topic,
                                      bool to_fd);
#endif
static void n00b_scan_thread_stacks(n00b_collect_t *);
static void n00b_scan_thread_lock_chains(n00b_collect_t *ctx, n00b_thread_record_t *rec);
static void n00b_scan_runtime(n00b_collect_t *);
static void n00b_scan_roots(n00b_collect_t *);
static void n00b_add_alloc_to_worklist(n00b_alloc_info_t ainfo, n00b_collect_t *ctx);
static void n00b_add_range_strided_to_worklist(void           *start,
                                               uint64_t        nwords,
                                               uint64_t        stride,
                                               uint64_t        offset,
                                               n00b_collect_t *ctx);
static void n00b_add_range_to_worklist(void *start, uint64_t nwords, n00b_collect_t *ctx);
static void n00b_add_described_scan_range_to_worklist(n00b_collect_t     *ctx,
                                                      void               *start,
                                                      uint64_t            nwords,
                                                      n00b_gc_scan_kind_t scan_kind,
                                                      n00b_gc_scan_cb_t   scan_cb,
                                                      void               *scan_user,
                                                      n00b_alloc_info_t   origin);
// Mostly-copying pin support (ambiguous-root pinning).
static void        n00b_pin_bitmaps_alloc(n00b_collect_t *ctx);
static void        n00b_pin_candidate(n00b_collect_t *ctx, void *candidate);
static void        n00b_pin_prepass(n00b_collect_t *ctx);
static void
n00b_pin_object_pages(n00b_collect_t *ctx, n00b_segment_t *hint, n00b_alloc_info_t ainfo);
static bool
n00b_alloc_is_pinned(n00b_collect_t *ctx, n00b_segment_t *hint, n00b_alloc_info_t ainfo);
static void        n00b_scan_pinned_in_place(n00b_collect_t *ctx, n00b_alloc_info_t ainfo);
static void        n00b_reclaim_pinned_pages(n00b_collect_t *ctx, n00b_segment_t *from_chain);
static void        gc_audit_segment_chain(n00b_arena_t *arena, const char *when);

// GC pin accounting (non-static so a debugger can read them). Per-collect: how
// many from-space pages were RETAINED (pinned, linked back into the live arena)
// vs FREED (unpinned, munmapped). If pinned dominates, the from-space is being
// abandoned-in-place instead of returned.
_Atomic uint64_t n00b_gc_last_pinned_pages   = 0;
_Atomic uint64_t n00b_gc_last_freed_pages    = 0;
_Atomic uint64_t n00b_gc_last_retained_runs  = 0;
_Atomic uint64_t n00b_gc_last_nobitmap_segs  = 0;
_Atomic uint64_t n00b_gc_last_segment_lookups      = 0;
_Atomic uint64_t n00b_gc_last_segment_lookup_steps = 0;
_Atomic uint64_t n00b_gc_total_pinned_pages  = 0;
_Atomic uint64_t n00b_gc_total_freed_pages   = 0;
_Atomic uint64_t n00b_gc_collect_count       = 0;
_Atomic uint64_t n00b_gc_last_primary_used_bytes   = 0;
_Atomic uint64_t n00b_gc_last_primary_capacity     = 0;
_Atomic uint64_t n00b_gc_last_primary_shrink_bytes = 0;
_Atomic uint64_t n00b_gc_total_primary_shrink_bytes = 0;
// Collector-only, under-STW reclaim of dead foreign-thread records (thread.c).
extern void        n00b_reap_dead_foreign_threads(void);
// Diagnostic: foreign-self aliasing evidence pass (thread.c).
extern void        n00b_diag_foreign_self_check(void);
static bool n00b_add_alloc_range_to_worklist(n00b_collect_t *ctx, n00b_alloc_range_t *range);

#define N00B_GC_SHRINK_HEADROOM_FACTOR 2u

// Always-on: the STW pause accounting uses it; the census (debug) machinery
// further down uses it too.
static uint64_t
n00b_gc_timestamp_ns(void)
{
    int64_t now = n00b_ns_timestamp();
    return now < 0 ? 0 : (uint64_t)now;
}

// Record one stop-the-world pause into the runtime's always-on counters
// (runtime.h gc_*_pause fields). The 0.25s pause budget is enforced against
// gc_max_pause_ns under a full-build workload.
[[n00b::nogc]] static void
n00b_gc_record_pause(uint64_t start_ns, uint64_t end_ns)
{
    if (end_ns <= start_ns) {
        return;
    }
    n00b_runtime_t *rt = n00b_default_runtime_or_null();
    if (rt == nullptr) {
        return;
    }
    uint64_t pause = end_ns - start_ns;
    n00b_atomic_store(&rt->gc_last_pause_ns, pause);
    atomic_fetch_add(&rt->gc_pause_total_ns, pause);
    atomic_fetch_add(&rt->gc_pause_count, 1);
    uint64_t prev = n00b_atomic_load(&rt->gc_max_pause_ns);
    while (pause > prev
           && !n00b_cas(&rt->gc_max_pause_ns, &prev, pause))
        ;
}

#if defined(N00B_CENSUS_ENABLED)
static uint64_t
n00b_gc_elapsed_ns(uint64_t start_ns, uint64_t end_ns)
{
    if (start_ns == 0 || end_ns < start_ns) {
        return 0;
    }
    return end_ns - start_ns;
}

static void
n00b_debug_census_finish_phase(uint64_t *slot, [[maybe_unused]] uint64_t *phase_start_ns)
{
    if (slot == nullptr || phase_start_ns == nullptr || *phase_start_ns == 0) {
        return;
    }
    uint64_t now_ns = n00b_gc_timestamp_ns();
    *slot += n00b_gc_elapsed_ns(*phase_start_ns, now_ns);
    *phase_start_ns = now_ns;
}

static void
n00b_debug_census_record_pass_timing([[maybe_unused]] uint64_t pause_start_ns,
                                     [[maybe_unused]] uint64_t stop_done_ns,
                                     [[maybe_unused]] uint64_t restart_start_ns,
                                     [[maybe_unused]] uint64_t pause_done_ns)
{
    n00b_debug_census_t *census = g_debug_census;
    if (census == nullptr) {
        return;
    }

    census->gc_total_pause_ns = n00b_gc_elapsed_ns(pause_start_ns, pause_done_ns);
    census->gc_stop_ns        = n00b_gc_elapsed_ns(pause_start_ns, stop_done_ns);
    census->gc_collect_ns     = n00b_gc_elapsed_ns(stop_done_ns, restart_start_ns);
    census->gc_restart_ns     = n00b_gc_elapsed_ns(restart_start_ns, pause_done_ns);
}
#endif

static n00b_gc_scan_kind_t
n00b_effective_scan_kind(n00b_gc_scan_kind_t scan_kind, bool no_scan)
{
    if (scan_kind != N00B_GC_SCAN_KIND_DEFAULT) {
        return scan_kind;
    }

#if defined(N00B_DISABLE_NOSCAN)
    return N00B_GC_SCAN_KIND_ALL;
#else
    return no_scan ? N00B_GC_SCAN_KIND_NONE : N00B_GC_SCAN_KIND_ALL;
#endif
}

#if defined(N00B_CENSUS_ENABLED)
static n00b_debug_alloc_origin_t
n00b_debug_census_alloc_origin(n00b_alloc_info_t ainfo)
{
    n00b_debug_alloc_origin_t result = {
        .alloc_kind = (uint64_t)ainfo.kind,
    };

    switch (ainfo.kind) {
    case n00b_alloc_oob:
        result.alloc_len       = ainfo.hdr.oob->alloc_len;
        result.ptr_words       = ainfo.hdr.oob->ptr_words;
        result.ptr_words_known = ainfo.hdr.oob->ptr_words_known ? 1u : 0u;
        result.scan_kind       = ainfo.hdr.oob->scan_kind;
        result.no_scan         = ainfo.hdr.oob->no_scan ? 1u : 0u;
        result.tinfo           = ainfo.hdr.oob->tinfo;
        result.site            = ainfo.hdr.oob->file_name;
        break;
    case n00b_alloc_inline:
        result.alloc_len       = ainfo.hdr.in_line->alloc_len;
        result.ptr_words       = ainfo.hdr.in_line->ptr_words;
        result.ptr_words_known = ainfo.hdr.in_line->ptr_words_known ? 1u : 0u;
        result.scan_kind       = ainfo.hdr.in_line->scan_kind;
        result.no_scan         = ainfo.hdr.in_line->no_scan ? 1u : 0u;
        result.tinfo           = ainfo.hdr.in_line->tinfo;
        break;
    case n00b_alloc_static_range:
        result.alloc_len       = ainfo.hdr.range->len;
        result.ptr_words       = ainfo.hdr.range->len / sizeof(void *);
        result.ptr_words_known = 0;
        result.scan_kind       = ainfo.hdr.range->scan_kind;
        result.no_scan         = ainfo.hdr.range->scan_kind == N00B_GC_SCAN_KIND_NONE ? 1u : 0u;
        result.tinfo           = ainfo.hdr.range->tinfo;
        result.site            = ainfo.hdr.range->file;
        break;
    default:
        break;
    }

    return result;
}

static void
n00b_debug_census_record_scan_range(void *start, size_t nwords)
{
    n00b_debug_census_t *census = g_debug_census;
    if (census == nullptr) {
        return;
    }

    census->gc_scan_range_count++;
    census->gc_scan_words += (uint64_t)nwords;
    if ((uint64_t)nwords > census->gc_scan_max_words) {
        census->gc_scan_max_words = (uint64_t)nwords;
        census->gc_scan_max_addr  = (uint64_t)(uintptr_t)start;

        n00b_debug_alloc_origin_t origin
            = n00b_debug_census_alloc_origin(n00b_find_alloc_info(start));
        census->gc_scan_max_alloc_kind      = origin.alloc_kind;
        census->gc_scan_max_alloc_len       = origin.alloc_len;
        census->gc_scan_max_ptr_words       = origin.ptr_words;
        census->gc_scan_max_ptr_words_known = origin.ptr_words_known;
        census->gc_scan_max_scan_kind       = origin.scan_kind;
        census->gc_scan_max_no_scan         = origin.no_scan;
        census->gc_scan_max_tinfo           = origin.tinfo;
        census->gc_scan_max_site            = origin.site;
    }
}

static void
n00b_debug_census_record_worklist_origin(n00b_alloc_info_t origin, void *start, uint64_t nwords)
{
    n00b_debug_census_t *census = g_debug_census;
    if (census == nullptr) {
        return;
    }

    census->gc_worklist_origin_count++;
    census->gc_worklist_origin_words += nwords;

    if (nwords > census->gc_worklist_origin_max_words) {
        n00b_debug_alloc_origin_t alloc_origin = n00b_debug_census_alloc_origin(origin);

        census->gc_worklist_origin_max_words           = nwords;
        census->gc_worklist_origin_max_addr            = (uint64_t)(uintptr_t)start;
        census->gc_worklist_origin_max_alloc_kind      = alloc_origin.alloc_kind;
        census->gc_worklist_origin_max_alloc_len       = alloc_origin.alloc_len;
        census->gc_worklist_origin_max_ptr_words       = alloc_origin.ptr_words;
        census->gc_worklist_origin_max_ptr_words_known = alloc_origin.ptr_words_known;
        census->gc_worklist_origin_max_scan_kind       = alloc_origin.scan_kind;
        census->gc_worklist_origin_max_no_scan         = alloc_origin.no_scan;
        census->gc_worklist_origin_max_tinfo           = alloc_origin.tinfo;
        census->gc_worklist_origin_max_site            = alloc_origin.site;
    }
}

// ============================================================================
// Debug census report formatting
// ============================================================================

static void
n00b_census_buf_append(n00b_buffer_t *buf, const char *bytes, uint64_t len)
{
    if (buf == nullptr || bytes == nullptr || len == 0) {
        return;
    }

    uint64_t old_len = buf->byte_len;
    n00b_buffer_resize(buf, old_len + len);

    for (uint64_t i = 0; i < len; i++) {
        buf->data[old_len + i] = bytes[i];
    }
}

#define n00b_census_lit(buf, lit)                                                              \
    n00b_census_buf_append((buf), (lit), (uint64_t)(sizeof(lit) - 1))

static void
n00b_census_buf_append_cstr(n00b_buffer_t *buf, const char *s)
{
    if (s == nullptr) {
        n00b_census_lit(buf, "?");
        return;
    }

    const char *p = s;
    while (*p != '\0') {
        p++;
    }

    n00b_census_buf_append(buf, s, (uint64_t)(p - s));
}

static void
n00b_census_buf_append_u64(n00b_buffer_t *buf, uint64_t v)
{
    char     tmp[20];
    uint64_t n = 0;

    if (v == 0) {
        n00b_census_lit(buf, "0");
        return;
    }

    while (v != 0) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }

    while (n != 0) {
        n--;
        n00b_census_buf_append(buf, &tmp[n], 1);
    }
}

static void
n00b_census_buf_append_hex(n00b_buffer_t *buf, uint64_t v)
{
    static const char hexdigits[] = "0123456789abcdef";
    char              tmp[16];
    uint64_t          n = 0;

    n00b_census_lit(buf, "0x");

    if (v == 0) {
        n00b_census_lit(buf, "0");
        return;
    }

    while (v != 0) {
        tmp[n++] = hexdigits[v & 0xf];
        v >>= 4;
    }

    while (n != 0) {
        n--;
        n00b_census_buf_append(buf, &tmp[n], 1);
    }
}

static void
n00b_debug_census_sort_rows(n00b_debug_census_row_t *rows, uint64_t n)
{
    for (uint64_t a = 0; a < n; a++) {
        uint64_t best = a;

        for (uint64_t b = a + 1; b < n; b++) {
            if (rows[b].primary > rows[best].primary) {
                best = b;
            }
        }

        if (best != a) {
            n00b_debug_census_row_t tmp = rows[a];
            rows[a]                     = rows[best];
            rows[best]                  = tmp;
        }
    }
}

static uint64_t
n00b_debug_census_rows_from_dicts(n00b_debug_census_t      *census,
                                  n00b_site_census_dict_t  *primary,
                                  n00b_site_census_dict_t  *counts,
                                  n00b_debug_census_row_t **out_rows)
{
    *out_rows = nullptr;

    if (census == nullptr || primary == nullptr) {
        return 0;
    }

    uint64_t n = (uint64_t)n00b_dict_internal_len((_n00b_dict_internal_t *)primary);

    if (n == 0) {
        return 0;
    }

    n00b_debug_census_row_t *rows
        = n00b_alloc_array_with_opts(n00b_debug_census_row_t,
                                     n,
                                     &(n00b_alloc_opts_t){.allocator = census->allocator});

    uint64_t i = 0;
    n00b_dict_foreach(primary, ck, cv, {
        if (i < n) {
            bool    found = false;
            int64_t count = counts == nullptr ? cv : n00b_dict_get(counts, ck, &found);

            rows[i].key     = ck;
            rows[i].primary = (uint64_t)cv;
            rows[i].count   = (uint64_t)(counts == nullptr || found ? count : 0);
            i++;
        }
    });

    n00b_debug_census_sort_rows(rows, i);
    *out_rows = rows;
    return i;
}

static void
n00b_debug_census_emit_site_rows(n00b_buffer_t           *out,
                                 n00b_debug_census_t     *census,
                                 n00b_site_census_dict_t *primary,
                                 n00b_site_census_dict_t *counts,
                                 const char              *prefix,
                                 bool                     primary_is_bytes)
{
    n00b_debug_census_row_t *rows = nullptr;
    uint64_t nrows = n00b_debug_census_rows_from_dicts(census, primary, counts, &rows);

    uint64_t n = nrows < N00B_DEBUG_CENSUS_TOP_N ? nrows : N00B_DEBUG_CENSUS_TOP_N;

    for (uint64_t i = 0; i < n; i++) {
        n00b_census_buf_append_cstr(out, prefix);
        n00b_census_buf_append_u64(out, rows[i].primary);
        if (primary_is_bytes) {
            n00b_census_lit(out, " bytes  ");
            n00b_census_buf_append_u64(out, rows[i].count);
            n00b_census_lit(out, " allocs  ");
        }
        else {
            n00b_census_lit(out, " allocs  ");
        }
        n00b_census_buf_append_cstr(out, (const char *)(uintptr_t)rows[i].key);
        n00b_census_lit(out, "\n");
    }
}

// Full (UNCAPPED) per-origin-site GC default-arena audit: every site that has a
// live or reclaimable allocation in the arena this collect, sorted by TOTAL
// bytes, with the live / reclaimed split.  This is the "what is allocated into
// the GC heap, by site, and how much of it is churn" report the operator asked
// for -- no top-N truncation.
static void
n00b_debug_census_emit_arena_full(n00b_buffer_t *out, n00b_debug_census_t *census)
{
    n00b_debug_census_row_t *rows  = nullptr;
    uint64_t                 nrows = n00b_debug_census_rows_from_dicts(
        census,
        census->arena_site_bytes, // primary = TOTAL bytes (returned sorted desc)
        census->arena_site_count, // count   = TOTAL allocs
        &rows);

    n00b_census_lit(out, "n00b arena-census FULL by-site (");
    n00b_census_buf_append_u64(out, nrows);
    n00b_census_lit(out,
                    " sites) total_bytes | live_bytes | reclaimed_bytes | "
                    "total_allocs | live_allocs | site:\n");

    for (uint64_t i = 0; i < nrows; i++) {
        uint64_t total_bytes = rows[i].primary;
        uint64_t total_count = rows[i].count;

        bool     f  = false;
        int64_t  lb = census->arena_site_live_bytes != nullptr
                        ? n00b_dict_get(census->arena_site_live_bytes, rows[i].key, &f)
                        : 0;
        uint64_t live_bytes
            = (census->arena_site_live_bytes != nullptr && f) ? (uint64_t)lb : 0;
        int64_t  lc = census->arena_site_live_count != nullptr
                        ? n00b_dict_get(census->arena_site_live_count, rows[i].key, &f)
                        : 0;
        uint64_t live_count
            = (census->arena_site_live_count != nullptr && f) ? (uint64_t)lc : 0;
        uint64_t reclaimed = total_bytes > live_bytes ? total_bytes - live_bytes : 0;

        n00b_census_lit(out, "  ");
        n00b_census_buf_append_u64(out, total_bytes);
        n00b_census_lit(out, " | ");
        n00b_census_buf_append_u64(out, live_bytes);
        n00b_census_lit(out, " | ");
        n00b_census_buf_append_u64(out, reclaimed);
        n00b_census_lit(out, " | ");
        n00b_census_buf_append_u64(out, total_count);
        n00b_census_lit(out, " | ");
        n00b_census_buf_append_u64(out, live_count);
        n00b_census_lit(out, " | ");
        n00b_census_buf_append_cstr(out, (const char *)(uintptr_t)rows[i].key);
        n00b_census_lit(out, "\n");
    }
}

static void
n00b_debug_census_store_stats(n00b_debug_census_t *census,
                              uint64_t             started_ns,
                              uint64_t             finished_ns)
{
    if (census == nullptr) {
        return;
    }

    n00b_atomic_store(&g_debug_census_last_started_ns, started_ns);
    n00b_atomic_store(&g_debug_census_last_finished_ns, finished_ns);
    n00b_atomic_store(&g_debug_census_last_duration_ns,
                      finished_ns >= started_ns ? finished_ns - started_ns : 0);
    n00b_atomic_store(&g_debug_census_gc_total_pause_ns, census->gc_total_pause_ns);
    n00b_atomic_store(&g_debug_census_gc_census_ns, census->gc_census_ns);
    n00b_atomic_store(&g_debug_census_gc_root_count, census->gc_root_count);
    n00b_atomic_store(&g_debug_census_gc_root_words, census->gc_root_words);
    n00b_atomic_store(&g_debug_census_gc_scan_range_count, census->gc_scan_range_count);
    n00b_atomic_store(&g_debug_census_gc_scan_words, census->gc_scan_words);
    n00b_atomic_store(&g_debug_census_gc_worklist_origin_count,
                      census->gc_worklist_origin_count);
    n00b_atomic_store(&g_debug_census_gc_worklist_origin_words,
                      census->gc_worklist_origin_words);
    n00b_atomic_store(&g_debug_census_pool_live_allocs, census->pool_live_allocs);
    n00b_atomic_store(&g_debug_census_pool_live_bytes, census->pool_live_bytes_total);
    n00b_atomic_store(&g_debug_census_pool_leak_allocs, census->pool_leak_allocs);
    n00b_atomic_store(&g_debug_census_pool_leak_bytes, census->pool_leak_bytes_total);
    n00b_atomic_store(&g_debug_census_metadata_pool_count, census->metadata_pool_count);
    n00b_atomic_store(&g_debug_census_metadata_pool_mapped_bytes,
                      census->metadata_pool_mapped_bytes);
    n00b_atomic_store(&g_debug_census_metadata_pool_records, census->metadata_pool_records);
    n00b_atomic_store(&g_debug_census_metadata_pool_slots, census->metadata_pool_slots);
    n00b_atomic_store(&g_debug_census_arena_record_count, census->arena_record_count);
    n00b_atomic_store(&g_debug_census_arena_total_bytes, census->arena_total_bytes);
    n00b_atomic_store(&g_debug_census_arena_forwarded_count, census->arena_forwarded_count);
    n00b_atomic_store(&g_debug_census_leak_sample_count, census->leak_sample_count);
    n00b_atomic_store(&g_debug_census_leak_total_count, census->leak_total_count);
    n00b_atomic_store(&g_debug_census_leak_total_bytes, census->leak_total_bytes);
    n00b_atomic_store(&g_debug_census_suspicious_alloc_count, census->suspicious_alloc_count);
    n00b_atomic_store(&g_debug_census_suspicious_worklist_count,
                      census->suspicious_worklist_count);
    n00b_atomic_store(&g_debug_census_slow_worklist_count, census->slow_worklist_count);

    n00b_debug_census_row_t *rows = nullptr;
    uint64_t                 nrows
        = n00b_debug_census_rows_from_dicts(census, census->site_live_count, nullptr, &rows);
    uint64_t top
        = nrows < N00B_DEBUG_CENSUS_HEALTH_TOP_N ? nrows : N00B_DEBUG_CENSUS_HEALTH_TOP_N;
    n00b_atomic_store(&g_debug_census_site_live_top_count, top);
    for (uint64_t i = 0; i < N00B_DEBUG_CENSUS_HEALTH_TOP_N; i++) {
        n00b_atomic_store(&g_debug_census_site_live_top_site[i],
                          i < top ? (uintptr_t)rows[i].key : 0);
        n00b_atomic_store(&g_debug_census_site_live_top_allocs[i],
                          i < top ? rows[i].primary : 0);
    }

    rows  = nullptr;
    nrows = n00b_debug_census_rows_from_dicts(census,
                                              census->pool_live_bytes,
                                              census->pool_live_count,
                                              &rows);
    top   = nrows < N00B_DEBUG_CENSUS_HEALTH_TOP_N ? nrows : N00B_DEBUG_CENSUS_HEALTH_TOP_N;
    n00b_atomic_store(&g_debug_census_pool_live_top_count, top);
    for (uint64_t i = 0; i < N00B_DEBUG_CENSUS_HEALTH_TOP_N; i++) {
        n00b_atomic_store(&g_debug_census_pool_live_top_site[i],
                          i < top ? (uintptr_t)rows[i].key : 0);
        n00b_atomic_store(&g_debug_census_pool_live_top_bytes[i],
                          i < top ? rows[i].primary : 0);
        n00b_atomic_store(&g_debug_census_pool_live_top_allocs[i], i < top ? rows[i].count : 0);
    }

    rows  = nullptr;
    nrows = n00b_debug_census_rows_from_dicts(census,
                                              census->pool_leak_bytes,
                                              census->pool_leak_count,
                                              &rows);
    top   = nrows < N00B_DEBUG_CENSUS_HEALTH_TOP_N ? nrows : N00B_DEBUG_CENSUS_HEALTH_TOP_N;
    n00b_atomic_store(&g_debug_census_pool_leak_top_count, top);
    for (uint64_t i = 0; i < N00B_DEBUG_CENSUS_HEALTH_TOP_N; i++) {
        n00b_atomic_store(&g_debug_census_pool_leak_top_site[i],
                          i < top ? (uintptr_t)rows[i].key : 0);
        n00b_atomic_store(&g_debug_census_pool_leak_top_bytes[i],
                          i < top ? rows[i].primary : 0);
        n00b_atomic_store(&g_debug_census_pool_leak_top_allocs[i], i < top ? rows[i].count : 0);
    }
    n00b_atomic_store(&g_debug_census_runs, n00b_atomic_load(&g_debug_census_runs) + 1);
}

static void
n00b_debug_census_publish(n00b_debug_census_t *census,
                          n00b_conduit_topic_t(n00b_buffer_t *) * topic,
                          bool to_fd)
{
    if (census == nullptr || topic == nullptr) {
        return;
    }

    n00b_conduit_topic_base_t *topic_base = (n00b_conduit_topic_base_t *)topic;
    n00b_allocator_t          *out_alloc = topic_base->conduit && topic_base->conduit->allocator
                                             ? topic_base->conduit->allocator
                                             : (n00b_allocator_t *)&n00b_get_runtime()->conduit_pool;
    n00b_buffer_t             *out       = n00b_buffer_empty(.allocator = out_alloc);

    n00b_census_lit(out, "n00b census: collection complete\n");
    n00b_census_lit(out, "n00b gc-timing: total_pause_ns=");
    n00b_census_buf_append_u64(out, census->gc_total_pause_ns);
    n00b_census_lit(out, " stop_ns=");
    n00b_census_buf_append_u64(out, census->gc_stop_ns);
    n00b_census_lit(out, " collect_ns=");
    n00b_census_buf_append_u64(out, census->gc_collect_ns);
    n00b_census_lit(out, " restart_ns=");
    n00b_census_buf_append_u64(out, census->gc_restart_ns);
    n00b_census_lit(out, " oom=");
    n00b_census_buf_append_u64(out, census->gc_out_of_memory ? 1u : 0u);
    n00b_census_lit(out, "\n");
    n00b_census_lit(out, "n00b gc-timing phases: internal_ns=");
    n00b_census_buf_append_u64(out, census->gc_internal_ns);
    n00b_census_lit(out, " setup_ns=");
    n00b_census_buf_append_u64(out, census->gc_setup_ns);
    n00b_census_lit(out, " roots_ns=");
    n00b_census_buf_append_u64(out, census->gc_roots_ns);
    n00b_census_lit(out, " runtime_ns=");
    n00b_census_buf_append_u64(out, census->gc_runtime_scan_ns);
    n00b_census_lit(out, " worklist_roots_ns=");
    n00b_census_buf_append_u64(out, census->gc_worklist_roots_ns);
    n00b_census_lit(out, " thread_scan_ns=");
    n00b_census_buf_append_u64(out, census->gc_thread_scan_ns);
    n00b_census_lit(out, " worklist_threads_ns=");
    n00b_census_buf_append_u64(out, census->gc_worklist_threads_ns);
    n00b_census_lit(out, " metadata_scan_ns=");
    n00b_census_buf_append_u64(out, census->gc_metadata_scan_ns);
    n00b_census_lit(out, " metadata_worklist_ns=");
    n00b_census_buf_append_u64(out, census->gc_metadata_worklist_ns);
    n00b_census_lit(out, " census_ns=");
    n00b_census_buf_append_u64(out, census->gc_census_ns);
    n00b_census_lit(out, " sweep_ns=");
    n00b_census_buf_append_u64(out, census->gc_pool_sweep_ns);
    n00b_census_lit(out, " foreign_reap_ns=");
    n00b_census_buf_append_u64(out, census->gc_foreign_reap_ns);
    n00b_census_lit(out, " finalizers_ns=");
    n00b_census_buf_append_u64(out, census->gc_finalizers_ns);
    n00b_census_lit(out, " cleanup_ns=");
    n00b_census_buf_append_u64(out, census->gc_cleanup_ns);
    n00b_census_lit(out, "\n");
    n00b_census_lit(out, "n00b gc-roots: count=");
    n00b_census_buf_append_u64(out, census->gc_root_count);
    n00b_census_lit(out, " words=");
    n00b_census_buf_append_u64(out, census->gc_root_words);
    n00b_census_lit(out, " max_words=");
    n00b_census_buf_append_u64(out, census->gc_root_max_words);
    n00b_census_lit(out, " max_addr=");
    n00b_census_buf_append_hex(out, census->gc_root_max_addr);
    n00b_census_lit(out, " max_index=");
    n00b_census_buf_append_u64(out, census->gc_root_max_index);
    n00b_census_lit(out, " slowest_ns=");
    n00b_census_buf_append_u64(out, census->gc_root_slowest_ns);
    n00b_census_lit(out, " slowest_words=");
    n00b_census_buf_append_u64(out, census->gc_root_slowest_words);
    n00b_census_lit(out, " slowest_addr=");
    n00b_census_buf_append_hex(out, census->gc_root_slowest_addr);
    n00b_census_lit(out, " slowest_index=");
    n00b_census_buf_append_u64(out, census->gc_root_slowest_index);
    n00b_census_lit(out, "\n");
    n00b_census_lit(out, "n00b gc-scan: ranges=");
    n00b_census_buf_append_u64(out, census->gc_scan_range_count);
    n00b_census_lit(out, " words=");
    n00b_census_buf_append_u64(out, census->gc_scan_words);
    n00b_census_lit(out, " max_words=");
    n00b_census_buf_append_u64(out, census->gc_scan_max_words);
    n00b_census_lit(out, " max_addr=");
    n00b_census_buf_append_hex(out, census->gc_scan_max_addr);
    n00b_census_lit(out, " max_alloc_kind=");
    n00b_census_buf_append_u64(out, census->gc_scan_max_alloc_kind);
    n00b_census_lit(out, " max_alloc_len=");
    n00b_census_buf_append_u64(out, census->gc_scan_max_alloc_len);
    n00b_census_lit(out, " max_ptr_words=");
    n00b_census_buf_append_u64(out, census->gc_scan_max_ptr_words);
    n00b_census_lit(out, " max_ptr_words_known=");
    n00b_census_buf_append_u64(out, census->gc_scan_max_ptr_words_known);
    n00b_census_lit(out, " max_scan_kind=");
    n00b_census_buf_append_u64(out, census->gc_scan_max_scan_kind);
    n00b_census_lit(out, " max_no_scan=");
    n00b_census_buf_append_u64(out, census->gc_scan_max_no_scan);
    n00b_census_lit(out, " max_tinfo=");
    n00b_census_buf_append_hex(out, census->gc_scan_max_tinfo);
    n00b_census_lit(out, " max_site=");
    n00b_census_buf_append_cstr(out, census->gc_scan_max_site);
    n00b_census_lit(out, "\n");
    n00b_census_lit(out, "n00b gc-worklist-origin: ranges=");
    n00b_census_buf_append_u64(out, census->gc_worklist_origin_count);
    n00b_census_lit(out, " words=");
    n00b_census_buf_append_u64(out, census->gc_worklist_origin_words);
    n00b_census_lit(out, " max_words=");
    n00b_census_buf_append_u64(out, census->gc_worklist_origin_max_words);
    n00b_census_lit(out, " max_addr=");
    n00b_census_buf_append_hex(out, census->gc_worklist_origin_max_addr);
    n00b_census_lit(out, " max_alloc_kind=");
    n00b_census_buf_append_u64(out, census->gc_worklist_origin_max_alloc_kind);
    n00b_census_lit(out, " max_alloc_len=");
    n00b_census_buf_append_u64(out, census->gc_worklist_origin_max_alloc_len);
    n00b_census_lit(out, " max_ptr_words=");
    n00b_census_buf_append_u64(out, census->gc_worklist_origin_max_ptr_words);
    n00b_census_lit(out, " max_ptr_words_known=");
    n00b_census_buf_append_u64(out, census->gc_worklist_origin_max_ptr_words_known);
    n00b_census_lit(out, " max_scan_kind=");
    n00b_census_buf_append_u64(out, census->gc_worklist_origin_max_scan_kind);
    n00b_census_lit(out, " max_no_scan=");
    n00b_census_buf_append_u64(out, census->gc_worklist_origin_max_no_scan);
    n00b_census_lit(out, " max_tinfo=");
    n00b_census_buf_append_hex(out, census->gc_worklist_origin_max_tinfo);
    n00b_census_lit(out, " max_site=");
    n00b_census_buf_append_cstr(out, census->gc_worklist_origin_max_site);
    n00b_census_lit(out, "\n");

    if (census->site_live_count != nullptr) {
        n00b_debug_census_emit_site_rows(out,
                                         census,
                                         census->site_live_count,
                                         nullptr,
                                         "n00b census LIVE: ",
                                         false);
    }

    n00b_census_lit(out, "n00b pool-census: LIVE ");
    n00b_census_buf_append_u64(out, census->pool_live_allocs);
    n00b_census_lit(out, " allocs / ");
    n00b_census_buf_append_u64(out, census->pool_live_bytes_total);
    n00b_census_lit(out, " bytes ; LEAKED ");
    n00b_census_buf_append_u64(out, census->pool_leak_allocs);
    n00b_census_lit(out, " allocs / ");
    n00b_census_buf_append_u64(out, census->pool_leak_bytes_total);
    n00b_census_lit(out, " bytes ; ");
    n00b_census_buf_append_u64(out, census->metadata_pool_count);
    n00b_census_lit(out, " pools\n");
    n00b_census_lit(out, "n00b pool-census metadata: ");
    n00b_census_buf_append_u64(out, census->metadata_pool_mapped_bytes);
    n00b_census_lit(out, " mapped bytes ; ");
    n00b_census_buf_append_u64(out, census->metadata_pool_records);
    n00b_census_lit(out, " records ; ");
    n00b_census_buf_append_u64(out, census->metadata_pool_slots);
    n00b_census_lit(out, " dict slots\n");

    n00b_debug_census_emit_site_rows(out,
                                     census,
                                     census->pool_live_bytes,
                                     census->pool_live_count,
                                     "n00b pool-census LIVE: ",
                                     true);
    n00b_debug_census_emit_site_rows(out,
                                     census,
                                     census->pool_leak_bytes,
                                     census->pool_leak_count,
                                     "n00b pool-census LEAKED: ",
                                     true);

    if (census->arena_seen) {
        n00b_census_lit(out, "n00b arena-census [");
        n00b_census_buf_append_cstr(out, census->arena_name);
        n00b_census_lit(out, "]: TOTAL ");
        n00b_census_buf_append_u64(out, census->arena_record_count);
        n00b_census_lit(out, " records / ");
        n00b_census_buf_append_u64(out, census->arena_total_bytes);
        n00b_census_lit(out, " bytes ; LIVE ");
        n00b_census_buf_append_u64(out, census->arena_live_record_count);
        n00b_census_lit(out, " records / ");
        n00b_census_buf_append_u64(out, census->arena_live_bytes_total);
        n00b_census_lit(out, " bytes ; RECLAIMED ");
        n00b_census_buf_append_u64(out,
                                   census->arena_record_count
                                       - census->arena_live_record_count);
        n00b_census_lit(out, " records / ");
        n00b_census_buf_append_u64(out,
                                   census->arena_total_bytes - census->arena_live_bytes_total);
        n00b_census_lit(out, " bytes\n");

        n00b_debug_census_emit_arena_full(out, census);
    }

    if (census->leak_total_count != 0) {
        n00b_census_lit(out, "n00b leak-samples: ");
        n00b_census_buf_append_u64(out, census->leak_sample_count);
        n00b_census_lit(out, " of ");
        n00b_census_buf_append_u64(out, census->leak_total_count);
        n00b_census_lit(out, " leaks / ");
        n00b_census_buf_append_u64(out, census->leak_total_bytes);
        n00b_census_lit(out, " bytes\n");

        for (uint64_t i = 0; i < census->leak_sample_count; i++) {
            n00b_debug_leak_sample_t *s = &census->leak_samples[i];

            n00b_census_lit(out, "n00b leak: pool=");
            n00b_census_buf_append_cstr(out, s->pool_name);
            n00b_census_lit(out, " alloc_len=");
            n00b_census_buf_append_u64(out, s->alloc_len);
            n00b_census_lit(out, " tinfo=");
            n00b_census_buf_append_u64(out, s->tinfo);
            n00b_census_lit(out, " at ");
            n00b_census_buf_append_cstr(out, s->site_name);
            n00b_census_lit(out, " ptr=");
            n00b_census_buf_append_hex(out, (uint64_t)(uintptr_t)s->user_ptr);
            n00b_census_lit(out, "\n");
        }
    }

    if (census->suspicious_alloc_count != 0 || census->suspicious_worklist_count != 0
        || census->slow_worklist_count != 0) {
        n00b_census_lit(out, "n00b gc-diagnostics: suspicious_alloc=");
        n00b_census_buf_append_u64(out, census->suspicious_alloc_count);
        n00b_census_lit(out, " suspicious_worklist=");
        n00b_census_buf_append_u64(out, census->suspicious_worklist_count);
        n00b_census_lit(out, " slow_worklist=");
        n00b_census_buf_append_u64(out, census->slow_worklist_count);
        n00b_census_lit(out, "\n");
    }

    if (to_fd) {
        // STW-safe path: when the census runs inside n00b_collect (natural
        // collection), the world is stopped, so publishing through the conduit
        // would block forever in the CV notify (no consumer can ack). Write the
        // fully-rendered report straight to stderr (fd 2) instead.
        n00b_raw_write_all_brief(2, out->data, (unsigned long)out->byte_len);
    }
    else {
        n00b_write(n00b_buffer_t *, topic, out, .sync = false);
    }
}

static void
n00b_debug_census_record_leak(n00b_allocator_t *allocator, n00b_oob_hdr_t *oob)
{
    n00b_debug_census_t *census = g_debug_census;

    if (census == nullptr || oob == nullptr) {
        return;
    }

    census->leak_total_count++;
    census->leak_total_bytes += oob->alloc_len;

    if (census->leak_sample_count >= census->leak_sample_capacity) {
        return;
    }

    n00b_debug_leak_sample_t *sample = &census->leak_samples[census->leak_sample_count++];
    sample->pool_name = allocator && allocator->debug_name ? allocator->debug_name : "?";
    sample->site_name = oob->file_name ? oob->file_name : "?";
    sample->user_ptr  = oob->user_ptr;
    sample->tinfo     = oob->tinfo;
    sample->alloc_len = oob->alloc_len;
}

static void
n00b_debug_census_record_suspicious_alloc(void)
{
    n00b_debug_census_t *census = g_debug_census;

    if (census != nullptr) {
        census->suspicious_alloc_count++;
    }
}

static void
n00b_debug_census_record_suspicious_worklist(void)
{
    n00b_debug_census_t *census = g_debug_census;

    if (census != nullptr) {
        census->suspicious_worklist_count++;
    }
}

static void
n00b_debug_census_record_slow_worklist(void)
{
    n00b_debug_census_t *census = g_debug_census;

    if (census != nullptr) {
        census->slow_worklist_count++;
    }
}
#endif

// ============================================================================
// Exact stack-map frame publication
// ============================================================================

n00b_gc_stack_policy_t
n00b_gc_stack_get_policy(void)
{
    return (n00b_gc_stack_policy_t)n00b_thread_self()->gc_stack_policy;
}

n00b_gc_stack_policy_t
n00b_gc_stack_set_policy(n00b_gc_stack_policy_t policy)
{
    assert(policy <= N00B_GC_STACK_EXACT_ONLY);

    n00b_thread_t         *thread = n00b_thread_self();
    n00b_gc_stack_policy_t old    = (n00b_gc_stack_policy_t)thread->gc_stack_policy;

    thread->gc_stack_policy = (uint32_t)policy;
    return old;
}

// Stack-map frames pushed, counted while a test sets n00b_gc_count_stack_frames,
// so a test can bound the frames a path pushes without every push paying for a
// shared counter.
_Atomic(bool)     n00b_gc_count_stack_frames  = false;
_Atomic(uint64_t) n00b_gc_stack_frames_pushed = 0;

// ncc-emitted prologues call this with (void *) arguments (the descriptors are
// emitted type-name-free as anonymous structs), and the header declares it the
// same way, so the parameters are void * here and recovered to their concrete
// types up front.
void
n00b_gc_stack_push(void *frame_, const void *map_, void **roots)
{
    if (atomic_load_explicit(&n00b_gc_count_stack_frames, memory_order_relaxed)) {
        atomic_fetch_add_explicit(&n00b_gc_stack_frames_pushed, 1, memory_order_relaxed);
    }

    n00b_gc_stack_frame_t           *frame = frame_;
    const n00b_gc_stack_map_t *const map   = map_;
    assert(frame);
    assert(map);
    assert(!map->num_roots || roots);

    n00b_thread_t *thread = n00b_thread_self();

    // During a worker thread's pre-registration init window (after it
    // starts but before n00b_thread_init publishes its stack bounds, so
    // n00b_thread_self() cannot yet resolve it — D-004/D-014/D-019), the
    // thread is not yet a GC participant and has no per-thread frame chain
    // to maintain.  The codegen still emits a frame push in those early
    // prologues; with no owning n00b_thread_t there is nowhere to thread
    // it, so initialize the frame's links to empty and skip publishing.
    // The frame's roots remain live C-stack locals (conservatively
    // scannable) until the thread registers; this restores the
    // always-resolvable invariant the former thread_local self provided.
    if (thread == nullptr) {
        *frame = (n00b_gc_stack_frame_t){
            .prev  = nullptr,
            .map   = map,
            .roots = roots,
        };
        return;
    }

    *frame = (n00b_gc_stack_frame_t){
        .prev  = thread->gc_stack_top,
        .map   = map,
        .roots = roots,
    };
    thread->gc_stack_top = frame;
}

void
n00b_gc_stack_pop(void *frame_)
{
    n00b_gc_stack_frame_t *frame = frame_;
    assert(frame);

    n00b_thread_t *thread = n00b_thread_self();

    // Pre-registration worker window (see n00b_gc_stack_push): the matching
    // push did not publish this frame into any thread's chain, so there is
    // nothing to unlink — just clear the frame.
    if (thread == nullptr) {
        frame->prev  = nullptr;
        frame->map   = nullptr;
        frame->roots = nullptr;
        return;
    }

    if (thread->gc_stack_top == frame) {
        thread->gc_stack_top = frame->prev;
        frame->prev          = nullptr;
        frame->map           = nullptr;
        frame->roots         = nullptr;
        return;
    }

    // gc_stack_top != frame.  The sound way this happens is a FOREIGN thread
    // that attached (n00b_thread_init) MID-FRAME: every framed function
    // already on its C stack ran its prologue push while n00b_thread_self()
    // was still null — a no-op that never chained the frame (see
    // n00b_gc_stack_push) — but runs its epilogue pop now that the thread is
    // attached and self() resolves.  E.g. the Crayon gateway's libdispatch
    // entry points are gc-framed and call raw_gateway_ensure_thread_attached
    // (-> n00b_thread_init) from inside, so their own frame is pushed
    // pre-attach and popped post-attach.  Such a frame was never on this
    // thread's chain, so leave gc_stack_top alone and just clear the frame.
    // We do NOT walk the chain to "verify" it: the common-path equality check
    // above enforces LIFO for chained frames, and walking here would risk
    // dereferencing an unrelated chain on this rare attach-boundary path.
    frame->prev  = nullptr;
    frame->map   = nullptr;
    frame->roots = nullptr;
}

n00b_jmp_buf_t *
n00b_gc_stack_prepare_jmp(n00b_jmp_buf_t *ctx)
{
    assert(ctx);

    n00b_thread_t *thread = n00b_thread_self();
    assert(thread);

    ctx->n00b_thread       = thread;
    ctx->n00b_gc_stack_top = thread->gc_stack_top;
    return ctx;
}

void
n00b_gc_stack_restore(n00b_gc_stack_frame_t *top)
{
    // Pre-registration worker window (see n00b_gc_stack_push): no owning
    // n00b_thread_t to restore into.  A pre-registration thread has no
    // frame chain, so there is nothing to restore.
    n00b_thread_t *thread = n00b_thread_self();
    if (thread == nullptr) {
        return;
    }
    thread->gc_stack_top = top;
}

[[noreturn]] void
n00b_longjmp(n00b_jmp_buf_t *ctx, int value)
{
    assert(ctx);
    assert(ctx->n00b_thread == n00b_thread_self());

    n00b_gc_stack_restore(ctx->n00b_gc_stack_top);
    longjmp(ctx->n00b_jmp_env, value ? value : 1);
}

// ============================================================================
// Helpers
// ============================================================================

[[n00b::nogc]] static inline uint32_t
arena_overhead(n00b_arena_t *arena)
{
    return arena->vtable.add_inline_header ? N00B_ALLOC_HDR_SZ : 0;
}

[[n00b::nogc]] static inline n00b_inline_hdr_t *
alloc_info_raw_hdr(n00b_alloc_info_t info)
{
    assert(n00b_alloc_info_is_heap(info));
    return (info.kind == n00b_alloc_oob) ? (n00b_inline_hdr_t *)info.hdr.oob : info.hdr.in_line;
}

// ============================================================================
// Create destination arena (to-space)
// ============================================================================

static n00b_arena_t *
n00b_create_destination_arena(n00b_arena_t *src, bool out_of_memory)
{
    uint64_t sz = n00b_arena_size(src);

    // If we were really short on memory last time, go up a power of two.
    // This growth heuristic is ONLY valid when THIS collection was triggered
    // by the arena actually running out of room (out_of_memory): then a
    // multi-segment spill or a sparse-but-full arena is a genuine pressure
    // signal and pre-growing the to-space avoids an immediate re-collect.  A
    // manual / test / marshal collect is NOT memory pressure (the arena had
    // room; the caller just wanted to compact), so growing on it is wrong: a
    // low-traffic arena collected on a tight cadence (e.g. the default arena
    // polled every ~1ms while the churn lands in a *different* pool) has
    // alloc_count < N00B_TOO_FEW_ALLOCS every cycle and would double its
    // capacity each time — 32M → 64M → … → multi-GB unbounded — which then
    // makes the conservative backward sentinel scan over that segment stall
    // the world.  Gate the doubling on out_of_memory so non-pressure collects
    // keep the to-space the same size as the from-space.
    //
    // src->grow is the occupancy signal: it is set in n00b_collection_cleanup
    // when the previous collect left the live set above 25% of capacity, i.e.
    // less than 4x headroom.  Without it a single-segment arena that is full of
    // *live* data (next_segment == NULL, alloc_count >= N00B_TOO_FEW_ALLOCS)
    // never grows, so the to-space comes back the same size, refills on the
    // next alloc, and every allocation triggers a full-heap collect — an O(heap)
    // per-alloc CPU pin.  Growing on a dense collect restores the amortized
    // O(1)/byte semispace invariant.
    if (out_of_memory
        && (src->grow || src->current_segment->next_segment
            || src->alloc_count < N00B_TOO_FEW_ALLOCS)) {
        sz *= 2;
    }

    // To-space gets its OWN metadata arena exactly when from-space has one
    // (dev/census builds derive external_metadata = !no_map). n00b_new_arena
    // then attaches a fresh md_pool + forwarding dict. At GC end the collector
    // moves that metadata arena onto the recycled live arena and drops
    // from-space's old metadata arena wholesale -- the same move it does for
    // the data segments.
    bool src_has_md = src->vtable.metadata_pool != nullptr;
    // clang-format off
    n00b_arena_t *result = n00b_new_arena(
        .size           = sz,
        .use_gc         = true,
        .no_map         = !src_has_md,
        .hidden         = true,
        .inline_headers = src->vtable.add_inline_header,
        .name           = "to-space");
    // clang-format on

    assert(result->current_segment->size > 0 && result->current_segment->size >= sz);

    return result;
}

static uint64_t
n00b_gc_pow2_capacity_at_least(uint64_t need)
{
    if (need <= N00B_DEFAULT_SCRATCH_ARENA_SIZE) {
        return N00B_DEFAULT_SCRATCH_ARENA_SIZE;
    }

    uint64_t cap = n00b_align_closest_pow2_ceil(need);
    if (cap == 0 || cap < need) {
        return n00b_page_align(need);
    }
    return n00b_page_align(cap);
}

static void
n00b_gc_shrink_primary_segment(n00b_arena_t *arena)
{
    n00b_segment_t *segment = n00b_atomic_load(&arena->current_segment);
    if (segment == nullptr || segment->retained
        || segment->size <= N00B_DEFAULT_SCRATCH_ARENA_SIZE) {
        return;
    }

    char *next = n00b_atomic_load(&arena->next_alloc);
    if (next < segment->data || next > segment->data + segment->size) {
        return;
    }

    uint64_t used = (uint64_t)(next - segment->data);
    uint64_t old_size = segment->size;
    n00b_atomic_store(&n00b_gc_last_primary_used_bytes, used);
    n00b_atomic_store(&n00b_gc_last_primary_capacity, old_size);
    n00b_atomic_store(&n00b_gc_last_primary_shrink_bytes, 0);

    uint64_t need = used;
    if (need > UINT64_MAX / N00B_GC_SHRINK_HEADROOM_FACTOR) {
        need = UINT64_MAX;
    }
    else {
        need *= N00B_GC_SHRINK_HEADROOM_FACTOR;
    }

    uint64_t target = n00b_gc_pow2_capacity_at_least(need);
    if (target >= segment->size) {
        return;
    }

    char    *tail     = segment->data + target;
    uint64_t tail_len = old_size - target;

    /* Today the only caller reaches this with the to-space segment, which is
     * still hidden and therefore has no registry record, so the fixup below
     * does not fire. It is here because this is the ONLY place a live segment
     * shrinks, and shrinking a registered one without shortening its record
     * would now be a crash rather than a stale entry: n00b#275 made arena
     * segments record their permissions so the mark loop's guard scan can
     * trust them WITHOUT asking the kernel, so a record that outlived its
     * mapping would have the scan read unmapped memory on that authority --
     * the n00b#290 failure mode, reached from the other side.
     *
     * Carrying the recorded allocation high-water across keeps the tightened
     * scan bound (n00b#395) from resetting to "nothing recorded" on a segment
     * that is full of live objects. */
    uint64_t carried        = 0;
    bool     was_registered = (segment->mmap_rec != nullptr);

    if (was_registered) {
        carried = atomic_load_explicit(&segment->mmap_rec->max_alloc_len,
                                       memory_order_relaxed);
        n00b_mmap_unregister(segment->data);
        segment->mmap_rec = nullptr;
    }

    n00b_safe_munmap(tail, tail_len);

    segment->size      = target;
    segment->last_addr = segment->data + target;
    arena->segment_end = segment->last_addr;

    if (was_registered) {
        segment->mmap_rec = n00b_register_arena_segment(segment->data,
                                                        segment->last_addr,
                                                        arena);
        n00b_mmap_note_alloc_len(segment->mmap_rec, carried);
    }

    n00b_atomic_store(&n00b_gc_last_primary_shrink_bytes, tail_len);
    n00b_atomic_add(&n00b_gc_total_primary_shrink_bytes, tail_len);
}

// ============================================================================
// Forward allocation helpers
// ============================================================================

static __attribute__((noinline)) n00b_inline_hdr_t *
n00b_forward_mdata(n00b_collect_t *ctx, n00b_oob_hdr_t *old_map, n00b_inline_hdr_t *new_alloc)
{
    n00b_oob_hdr_t *map_item;

    char   *old_user_ptr = old_map->user_ptr;
    int64_t copy_len     = old_map->alloc_len - arena_overhead(ctx->to_space);
    char   *new_user_ptr = ((char *)new_alloc) + arena_overhead(ctx->to_space);

    assert(new_user_ptr + old_map->alloc_len < ctx->to_space->segment_end);

    // Allocate the new OOB record from TO-space's own metadata arena, so the
    // entire from-space metadata arena can be dropped wholesale at GC end.
    map_item = n00b_alloc_with_opts(
        n00b_oob_hdr_t,
        &(n00b_alloc_opts_t){.allocator = ctx->to_space->vtable.metadata_pool});

    memcpy(map_item, old_map, sizeof(n00b_oob_hdr_t));

    map_item->user_ptr  = new_user_ptr;
    map_item->hcur      = new_alloc;
    map_item->file_name = old_map->file_name;

    if (ctx->to_space->vtable.add_inline_header) {
        memcpy(new_alloc, old_map, sizeof(n00b_inline_hdr_t));
        new_alloc->guard = n00b_gc_guard;
    }

    // Add to the new metadata dict (stored on to-space during collection).
    n00b_md_put(ctx->to_space->vtable.metadata, new_user_ptr, map_item);

    memcpy(new_user_ptr, old_user_ptr, copy_len);

    old_map->hcur->moved = true;
    old_map->moved       = true;

    return (n00b_inline_hdr_t *)map_item;
}

// Must have full size headers in this case.
static __attribute__((noinline)) n00b_inline_hdr_t *
n00b_forward_inline(n00b_collect_t    *ctx,
                    n00b_inline_hdr_t *old_alloc,
                    n00b_inline_hdr_t *new_alloc)
{
    assert(!ctx->to_space->vtable.metadata);
    memcpy(new_alloc, old_alloc, old_alloc->alloc_len);

    old_alloc->moved = true;

    return new_alloc;
}

// This needs to act like we actually called n00b_core_alloc().
// But we have solo use of the allocator with no thread contention,
// so we bypass most of it.
//
// Note that we prefer working with the metadata record.  However, we
// will also update the inline alloc record if it's used.
//
// The inline alloc record must be used if the arena collects, yet
// doesn't keep metadata records. Otherwise, we assume that the first
// bytes are the inline header, unless:
//
//  1. The arena 'overhead' is less than sizeof(n00b_inline_hdr_t); or
//  2. The no_inline_headers flag is on (currently unused otherwise).
//
// The default arena keeps inline headers on, so that we can marshal
// out of it.

static inline n00b_inline_hdr_t *
n00b_forward_alloc(n00b_collect_t *ctx, n00b_inline_hdr_t *old)
{
    char *top;
    n00b_inline_hdr_t *new;

    top = n00b_atomic_load(&ctx->to_space->next_alloc);
    new = (n00b_inline_hdr_t *)top;
    top = top + old->alloc_len;

    /* This is a RAW bump: no CAS (the world is stopped, the collector is the
     * sole runner) -- but it still has to respect the end of the segment, and
     * until now it did not check.
     *
     * "The survivors always fit" holds by construction, because the to-space is
     * sized from the from-space's whole capacity.  But it held SILENTLY: an
     * object that did not fit was memcpy'd past the end of the mapped segment,
     * i.e. an out-of-bounds write from inside the collector with the world
     * stopped, which lands wherever the next mapping happens to be.
     *
     * Make the invariant explicit.  A failure here is a bug in the to-space
     * sizing, and it needs to say so at the point of the mistake rather than
     * corrupt whatever is next in the address space and fault somewhere else
     * entirely (n00b#431).  Growing the to-space instead of aborting is the
     * right end state, but it requires the multi-segment to-space path to be
     * sound first -- see the issue. */
    n00b_require(top <= ctx->to_space->segment_end,
                 "GC to-space overrun: a survivor does not fit the to-space, "
                 "which is sized from the from-space's capacity (n00b#431)");

    /* n00b#395: the to-space is hidden (and so unregistered) for the duration
     * of the collect, so record the extent here and seed it onto the segment's
     * registry record when the to-space is registered as the live segment. */
    if (old->alloc_len > ctx->to_space_max_alloc_len) {
        ctx->to_space_max_alloc_len = old->alloc_len;
    }

    ctx->to_space->alloc_count++;

    n00b_atomic_store(&ctx->to_space->next_alloc, top);

    n00b_inline_hdr_t  *result;
    void               *scan_start;
    bool                no_scan;
    uint32_t            nwords;
    n00b_gc_scan_kind_t scan_kind;
    n00b_gc_scan_cb_t   scan_cb   = nullptr;
    void               *scan_user = nullptr;
    n00b_alloc_info_t   origin    = {0};

    if (ctx->from_space->vtable.metadata_pool) {
        n00b_option_t(n00b_oob_hdr_t *) old_oob_opt = n00b_to_mem_metadata_record(old);
        n00b_require(n00b_option_is_set(old_oob_opt),
                     "metadata_pool branch implies OOB allocation");
        n00b_oob_hdr_t *old_oob = n00b_option_get(old_oob_opt);
        result                  = n00b_forward_mdata(ctx, old_oob, new);
        scan_start              = ((n00b_oob_hdr_t *)result)->user_ptr;
        no_scan                 = old_oob->no_scan;
        scan_kind               = (n00b_gc_scan_kind_t)old_oob->scan_kind;
        scan_cb                 = old_oob->scan_cb;
        scan_user               = old_oob->scan_user;
        origin                  = (n00b_alloc_info_t){
                             .kind    = n00b_alloc_oob,
                             .hdr.oob = old_oob,
        };
        if (old_oob->ptr_words_known) {
            nwords = old_oob->ptr_words;
        }
        else {
            nwords = (old_oob->alloc_len - arena_overhead(ctx->from_space)) / sizeof(void *);
        }
    }
    else {
        result     = n00b_forward_inline(ctx, old, new);
        scan_start = (char *)new + arena_overhead(ctx->to_space);
        no_scan    = new->no_scan;
        scan_kind  = (n00b_gc_scan_kind_t) new->scan_kind;
        scan_cb    = new->scan_cb;
        scan_user  = new->scan_user;
        origin     = (n00b_alloc_info_t){
                .kind        = n00b_alloc_inline,
                .hdr.in_line = new,
        };
        if (new->ptr_words_known) {
            nwords = new->ptr_words;
        }
        else {
            nwords = (old->alloc_len - arena_overhead(ctx->from_space)) / sizeof(void *);
        }
    }

    n00b_add_described_scan_range_to_worklist(ctx,
                                              scan_start,
                                              nwords,
                                              n00b_effective_scan_kind(scan_kind, no_scan),
                                              scan_cb,
                                              scan_user,
                                              origin);

    return result;
}

// ============================================================================
// Pointer translation
// ============================================================================

// Compute the user-data base address from a raw header, using the
// arena configuration (has_oob) rather than bit-testing.
[[n00b::nogc]] static inline char *
n00b_user_data_base(n00b_inline_hdr_t *hdr, bool has_oob)
{
    if (has_oob) {
        return ((n00b_oob_hdr_t *)hdr)->user_ptr;
    }
    return ((char *)hdr) + sizeof(n00b_inline_hdr_t);
}

[[n00b::nogc]] static void *
n00b_translate_pointer(n00b_collect_t    *ctx,
                       n00b_inline_hdr_t *old_alloc,
                       uint64_t         **arr,
                       n00b_inline_hdr_t *fw_loc,
                       uint32_t           ix)
{
    uint64_t *old_ptr = n00b_atomic_load((_Atomic(uint64_t *) *)(arr + ix));
    bool      has_oob = ctx->from_space->vtable.metadata_pool != nullptr;

    if (!old_ptr) {
        return nullptr;
    }

    assert(fw_loc);
    assert(fw_loc != old_alloc);

    char *old_base = n00b_user_data_base(old_alloc, has_oob);
    char *new_base = n00b_user_data_base(fw_loc, has_oob);

    ptrdiff_t offset   = (char *)old_ptr - old_base;
    ptrdiff_t user_len = old_alloc->alloc_len - arena_overhead(ctx->from_space);

    if (offset < 0) {
        return nullptr;
    }
    if (offset > user_len) {
        return nullptr;
    }

    return (void *)(new_base + offset);
}

// ============================================================================
// Memo operations
// ============================================================================

#define N00B_GC_PTR_TABLE_MULT UINT64_C(0x9E3779B97F4A7C15)

void
n00b_gc_ptr_table_init(n00b_gc_ptr_table_t *t, n00b_allocator_t *allocator, uint32_t log2_capacity)
{
    uint64_t cap = UINT64_C(1) << log2_capacity;

    t->keys      = n00b_alloc_array_with_opts(void *, cap, &(n00b_alloc_opts_t){.allocator = allocator});
    t->values    = n00b_alloc_array_with_opts(void *, cap, &(n00b_alloc_opts_t){.allocator = allocator});
    t->allocator = allocator;
    t->count     = 0;
    t->shift     = 64 - log2_capacity;
}

// The value recorded for `key`, setting `found` to whether there is one.
[[n00b::nogc]] void *
n00b_gc_ptr_table_get(n00b_gc_ptr_table_t *t, void *key, bool *found)
{
    uint64_t mask = (UINT64_C(1) << (64 - t->shift)) - 1;
    uint64_t i    = ((uint64_t)(uintptr_t)key * N00B_GC_PTR_TABLE_MULT) >> t->shift;

    while (t->keys[i] != nullptr) {
        if (t->keys[i] == key) {
            *found = true;
            return t->values[i];
        }
        i = (i + 1) & mask;
    }
    *found = false;
    return nullptr;
}

static void
n00b_gc_ptr_table_grow(n00b_gc_ptr_table_t *t)
{
    n00b_gc_ptr_table_t old = *t;
    uint64_t            cap = UINT64_C(1) << (64 - old.shift);

    n00b_gc_ptr_table_init(t, old.allocator, 64 - old.shift + 1);
    for (uint64_t i = 0; i < cap; i++) {
        if (old.keys[i] != nullptr) {
            n00b_gc_ptr_table_put(t, old.keys[i], old.values[i]);
        }
    }
    n00b_free_from_allocator(old.allocator, old.keys);
    n00b_free_from_allocator(old.allocator, old.values);
}

// Records `value` for `key`, replacing any earlier value. The table stays at
// most half full.
[[n00b::nogc]] void
n00b_gc_ptr_table_put(n00b_gc_ptr_table_t *t, void *key, void *value)
{
    if ((t->count + 1) * 2 > (UINT64_C(1) << (64 - t->shift))) {
        n00b_gc_ptr_table_grow(t);
    }

    uint64_t mask = (UINT64_C(1) << (64 - t->shift)) - 1;
    uint64_t i    = ((uint64_t)(uintptr_t)key * N00B_GC_PTR_TABLE_MULT) >> t->shift;

    while (t->keys[i] != nullptr && t->keys[i] != key) {
        i = (i + 1) & mask;
    }
    if (t->keys[i] == nullptr) {
        t->keys[i] = key;
        t->count++;
    }
    t->values[i] = value;
}

[[n00b::nogc]] static inline bool
n00b_is_first_visit(n00b_collect_t *ctx, n00b_inline_hdr_t *h, n00b_inline_hdr_t **fw)
{
    bool result;

    *fw = n00b_gc_ptr_table_get(&ctx->visited, h, &result);

    if (!result) {
        assert(!h->moved);
    }

    return !result;
}

[[n00b::nogc]] static inline void
n00b_register_visit(n00b_collect_t *ctx, n00b_inline_hdr_t *h, n00b_inline_hdr_t *fw)
{
    n00b_gc_ptr_table_put(&ctx->visited, h, fw);
}

// ============================================================================
// Worklist operations
// ============================================================================

static void
n00b_add_range_to_worklist(void *start, uint64_t nwords, n00b_collect_t *ctx)
{
    n00b_gc_wl_item_t *entry;
    entry = n00b_alloc_with_opts(
        n00b_gc_wl_item_t,
        &(n00b_alloc_opts_t){.allocator = (n00b_allocator_t *)&ctx->work_pool});
    entry->start     = start;
    entry->num_words = nwords;
    entry->stride    = 0; // 0 == legacy scan-every-word
    entry->offset    = 0;
    /* DIAGNOSTIC (leak/hang hunt): flag an absurdly large scan range.
     * Census records the counter only while leak-debug capture is active;
     * report formatting is deferred until after STW. */
    if (nwords > (1ULL << 24)) {
        n00b_debug_census_record_suspicious_worklist();
    }
    n00b_list_push(ctx->worklist, entry);
}

static void
n00b_add_scan_range_to_worklist(n00b_collect_t     *ctx,
                                void               *start,
                                uint64_t            nwords,
                                n00b_gc_scan_kind_t scan_kind,
                                n00b_gc_scan_cb_t   scan_cb,
                                void               *scan_user)
{
    if (!nwords || scan_kind == N00B_GC_SCAN_KIND_NONE) {
        return;
    }

    if (scan_kind == N00B_GC_SCAN_KIND_EVERY_OTHER) {
        n00b_add_range_strided_to_worklist(start, nwords, 2, 0, ctx);
        return;
    }

    if (scan_kind == N00B_GC_SCAN_KIND_CALLBACK && scan_cb) {
        n00b_allocator_t *wpool    = (n00b_allocator_t *)&ctx->work_pool;
        uint64_t          bm_words = n00b_gc_map_word_count(nwords);
        uint64_t         *bitmap   = n00b_alloc_array_with_opts(uint64_t,
                                                      bm_words,
                                                      &(n00b_alloc_opts_t){.allocator = wpool});

        for (uint64_t bi = 0; bi < bm_words; bi++) {
            bitmap[bi] = 0;
        }

        n00b_gc_map_t m = {.user_ptr = start, .num_words = nwords, .bitmap = bitmap};
        scan_cb(&m, scan_user);

        for (uint64_t bi = 0; bi < nwords; bi++) {
            if (n00b_gc_map_is_set(&m, bi)) {
                n00b_add_range_strided_to_worklist((char *)start + bi * sizeof(void *),
                                                   1,
                                                   0,
                                                   0,
                                                   ctx);
            }
        }
        return;
    }

    n00b_add_range_to_worklist(start, nwords, ctx);
}

static void
n00b_add_described_scan_range_to_worklist(n00b_collect_t     *ctx,
                                          void               *start,
                                          uint64_t            nwords,
                                          n00b_gc_scan_kind_t scan_kind,
                                          n00b_gc_scan_cb_t   scan_cb,
                                          void               *scan_user,
                                          n00b_alloc_info_t   origin)
{
    if (!nwords || scan_kind == N00B_GC_SCAN_KIND_NONE) {
        return;
    }

    if (scan_kind != N00B_GC_SCAN_KIND_CALLBACK || scan_cb == nullptr) {
        n00b_debug_census_record_worklist_origin(origin, start, nwords);
    }

    n00b_add_scan_range_to_worklist(ctx, start, nwords, scan_kind, scan_cb, scan_user);
}

static bool
n00b_add_alloc_range_to_worklist(n00b_collect_t *ctx, n00b_alloc_range_t *range)
{
    bool found = false;

    (void)n00b_gc_ptr_table_get(&ctx->visited, range, &found);
    if (found) {
        return false;
    }

    n00b_gc_ptr_table_put(&ctx->visited, range, range);
    n00b_add_described_scan_range_to_worklist(ctx,
                                              range->start,
                                              range->len / sizeof(void *),
                                              range->scan_kind,
                                              range->scan_cb,
                                              range->scan_user,
                                              (n00b_alloc_info_t){
                                                  .kind      = n00b_alloc_static_range,
                                                  .hdr.range = range,
                                              });
    return true;
}

static void
n00b_add_range_strided_to_worklist(void           *start,
                                   uint64_t        nwords,
                                   uint64_t        stride,
                                   uint64_t        offset,
                                   n00b_collect_t *ctx)
{
    n00b_gc_wl_item_t *entry;
    entry = n00b_alloc_with_opts(
        n00b_gc_wl_item_t,
        &(n00b_alloc_opts_t){.allocator = (n00b_allocator_t *)&ctx->work_pool});
    entry->start     = start;
    entry->num_words = nwords;
    entry->stride    = stride;
    entry->offset    = offset;
    n00b_list_push(ctx->worklist, entry);
}

static void
n00b_add_alloc_to_worklist(n00b_alloc_info_t ainfo, n00b_collect_t *ctx)
{
    void               *start;
    uint32_t            n;
    n00b_gc_scan_kind_t kind;
    bool                no_scan;
    n00b_gc_scan_cb_t   scan_cb;
    void               *scan_user;

    assert(n00b_alloc_info_is_heap(ainfo));

    if (ainfo.kind == n00b_alloc_oob) {
        n00b_oob_hdr_t *oob = ainfo.hdr.oob;
        /* Skip mid-allocation / freed-but-dict-stale OOBs. */
        if (oob->alive == 0) {
            return;
        }
#if !defined(N00B_DISABLE_PTR_WORDS)
        if (oob->ptr_words_known) {
            n = oob->ptr_words;
        }
        else
#endif
        {
            /* OOB records live on metadata-bearing pools (per the
             * @c external_metadata=true path in @c n00b_allocator_setup);
             * those pools do not add an inline header to user
             * allocations, so @c alloc_len here is the bare user-request
             * byte count.  Subtracting @c arena_overhead would underflow
             * for any allocation smaller than @c sizeof(n00b_inline_hdr_t)
             * and yield @c n ~ 0x1FFFFFFC words, walking the scan into
             * dyld shared-cache __DATA and SIGBUSing under macOS COW
             * pressure.  Mirror the bare-divide used in
             * @ref n00b_scan_one_alive_alloc_oob (gc.c, pool-walk-as-roots
             * path) which already noted the same constraint in its
             * comment. */
            n = (uint32_t)((uint64_t)oob->alloc_len / sizeof(void *));
        }
        start     = oob->user_ptr;
        kind      = (n00b_gc_scan_kind_t)oob->scan_kind;
        no_scan   = oob->no_scan;
        scan_cb   = oob->scan_cb;
        scan_user = oob->scan_user;
    }
    else {
        n00b_inline_hdr_t *hdr = ainfo.hdr.in_line;
#if !defined(N00B_DISABLE_PTR_WORDS)
        if (hdr->ptr_words_known) {
            n = hdr->ptr_words;
        }
        else
#endif
        {
            n = (hdr->alloc_len - arena_overhead(ctx->from_space)) / sizeof(void *);
        }
        start     = (char *)hdr + arena_overhead(ctx->from_space);
        kind      = (n00b_gc_scan_kind_t)hdr->scan_kind;
        no_scan   = hdr->no_scan;
        scan_cb   = hdr->scan_cb;
        scan_user = hdr->scan_user;
    }

    /* DIAGNOSTIC (leak/hang hunt): a word count past any real alloc means a
     * corrupt/underflowed alloc_len. The census captures a counter; detailed
     * formatting is not allowed while STW is active. */
    if (n > (1u << 24)) {
        n00b_debug_census_record_suspicious_alloc();
    }

    n00b_add_described_scan_range_to_worklist(ctx,
                                              start,
                                              n,
                                              n00b_effective_scan_kind(kind, no_scan),
                                              scan_cb,
                                              scan_user,
                                              ainfo);
}

[[n00b::nogc]] static void
n00b_process_worklist(n00b_collect_t *ctx)
{
    n00b_gc_wl_item_t *item;
    uint64_t           _iters = 0; // DIAGNOSTIC: detect a non-draining worklist

    while (n00b_list_len(ctx->worklist) > 0) {
        /* DIAGNOSTIC (leak/hang hunt): if the loop runs absurdly long, report
         * the current worklist length periodically.  A length that stays high
         * / grows => dedup (n00b_is_first_visit) is re-adding the same allocs
         * (a cycle that never converges); a length near 1 with us pinned here
         * => a single huge scan range (see the SUSPICIOUS-range log). */
        if ((++_iters & 0xfffff) == 0) { // every ~1M items
            n00b_debug_census_record_slow_worklist();
        }
        item = n00b_option_get(n00b_list_pop(n00b_gc_wl_item_t *, ctx->worklist));
        if (item->stride == 0) {
            n00b_scan_memory_range(ctx, item->start, item->num_words);
        }
        else {
            /* Strided scan: visit slots at indices offset, offset+stride,
             * offset+2*stride, ... while in [0, num_words).
             *
             * Pass base_checked=true. The base_checked=false path in
             * n00b_visit_possible_pointer resolves the base through
             * n00b_mmap_by_address and HARD-BAILS on a registry miss; a
             * freshly-forwarded to-space copy is not always in that registry
             * yet (it is readable, just not indexed), so relying on the base
             * check silently dropped every slot of a strided (EVERY_OTHER /
             * CALLBACK) scan over a forwarded alloc — the to-space copy the
             * mutator then reads kept its stale, un-forwarded pointers.
             *
             * As in n00b_scan_memory_range, the range is a KNOWN readable
             * region by construction (gc-managed alloc / precise static object
             * / explicitly-bounded stack), so there is no per-page readability
             * gate: we never probe or conservatively scan an unknown region. */
            uint64_t **base = (uint64_t **)item->start;

            for (uint64_t i = item->offset; i < item->num_words; i += item->stride) {
                n00b_visit_possible_pointer(ctx, base, i, true);
            }
        }
        n00b_free_from_allocator((n00b_allocator_t *)&ctx->work_pool, item);
    }
}

// ============================================================================
// Pointer visitor
// ============================================================================

// This examines a memory cell, rewriting it, if it contains a
// from-space pointer.
//
// If the memory cell contains a pointer into any managed heap space
// at all, we queue a scan for that pointer's allocation, UNLESS
// one of the following things is true:
//
// 1. We have already queued a scan (n00b_is_first_visit will be false)
// 2. `no_scan` is set on the alloc.
//
// For in-heap first visits, n00b_forward_alloc() will always copy,
// and only check the alloc's `no_scan` field.

// ============================================================================
// Per-collect conservative-scan tree (transient).
//
// At collect start we build a small interval tree of just the gc-scannable
// arena/pool segments and query it per scanned word, instead of the global mmap
// interval tree.  A miss — the overwhelming majority of scanned words — returns
// without touching the global tree at all, which both skips its deep search AND
// avoids the per-word lazy 'unmanaged' registration that bloated it.  The tree
// is allocated from the collect's work_pool, so it is reclaimed when work_pool
// is destroyed at cleanup (no manual free).  It is queried with a raw, lockless
// augmented-interval point search: the world is stopped during the scan, so the
// builder is the sole writer and the tree is immutable while we read it. Each
// hit carries its allocator so tracing does not rescan every retained segment
// for each reached pointer.
// ============================================================================

typedef struct {
    n00b_allocator_t *allocator;
    n00b_segment_t   *segment; // The arena segment; null for a pool page.
    n00b_mmap_info_t *map;     // The range's registry record; a pool page's is
                               // looked up on the first hit.
} n00b_scan_owner_t;

static void
n00b_scan_tree_add_allocator(n00b_allocator_t *al, void *arg)
{
    n00b_interval_tree_t(n00b_scan_owner_t) *tree = arg;

    // Match n00b_mmap_is_gc_scannable exactly: skip only the OPAQUE hidden
    // allocators (hidden with no OOB metadata — system_pool, the collector's own
    // work pools).  Hidden pools that DO carry metadata are still scanned into
    // (their dict gives precise boundaries), so they must stay in the tree or we
    // would miss transitive roots.
    if (al->hidden && al->metadata_pool == nullptr) {
        return;
    }

    if (al->free != nullptr) {
        // Pool: walk its mapped pages. The page struct base IS the mmap base.
        n00b_pool_t      *pool = (n00b_pool_t *)al;
        n00b_pool_page_t *pg   = pool->page_table;
        while (pg != nullptr) {
            uint64_t lo = (uint64_t)pg;
            uint64_t hi = lo + (uint64_t)pg->mapped_size;
            if (hi > lo) {
                (void)n00b_interval_insert(
                    tree,
                    lo,
                    hi,
                    ((n00b_scan_owner_t){.allocator = al}));
            }
            pg = pg->next;
        }
    }
    else {
        // Arena: walk its segment chain.
        n00b_arena_t   *a   = (n00b_arena_t *)al;
        n00b_segment_t *seg = n00b_atomic_load(&a->current_segment);
        while (seg != nullptr) {
            uint64_t lo = (uint64_t)seg->data;
            uint64_t hi = lo + seg->size;
            if (hi > lo) {
                (void)n00b_interval_insert(
                    tree,
                    lo,
                    hi,
                    ((n00b_scan_owner_t){.allocator = al,
                                         .segment   = seg,
                                         .map       = seg->mmap_rec}));
            }
            seg = seg->next_segment;
        }
    }
}

// ============================================================================
// Cached static-object range tree.
//
// The registered static-object ranges (`__DATA,n00b_stobj`: r-strings and other
// ncc static objects) are immutable after init: n00b_static_objects_register_all
// registers them exactly once at startup, and nothing registers a static range
// lazily at runtime (the r"..." expansion emits a link-time section entry, it
// makes no runtime registration call).  So we build an interval tree of them
// ONCE — lazily, under the first collect's stop-the-world — and reuse it every
// collect.  Each node carries the n00b_alloc_range_t* so a conservative-scan hit
// queues the range for in-place scanning with NO global mmap lookup.
//
// Source of truth is the mmap ctx range_tree (rt->mmaps.range_tree), which
// already holds every registered static range with its range record.
//
// [[n00b::nogc]]: the cached tree is allocated from the system_pool (hidden,
// non-GC, persistent — the same pool used for the root list and lock records),
// holds raw addresses, and is never a GC root / never traced.
[[n00b::nogc]] static _Atomic(void *) n00b_static_scan_tree = nullptr;

// Recursively copy every alloc_range entry from a range_tree subtree into the
// cached static scan tree.  Called once, under STW, so range_tree is stable and
// a lockless read is safe.
static void
n00b_static_scan_tree_copy(n00b_interval_node_t(n00b_mmap_data_t)      *node,
                           n00b_interval_tree_t(n00b_alloc_range_t *) *dst)
{
    if (node == nullptr) {
        return;
    }
    if (n00b_variant_is_type(node->data, n00b_alloc_range_t *)
        && node->high > node->low) {
        n00b_alloc_range_t *range = n00b_variant_get(node->data,
                                                     n00b_alloc_range_t *);
        (void)n00b_interval_insert(dst, node->low, node->high, range);
    }
    n00b_static_scan_tree_copy(node->left, dst);
    n00b_static_scan_tree_copy(node->right, dst);
}

// Build the cached static tree on first use.  Runs under the collect's STW
// (single collector at a time), so the check-then-build needs no extra locking;
// the atomic store publishes the finished tree.
static void
n00b_build_static_scan_tree_once(void)
{
    if (n00b_atomic_load(&n00b_static_scan_tree) != nullptr) {
        return;
    }
    n00b_runtime_t   *rt   = n00b_get_runtime();
    n00b_allocator_t *pool = (n00b_allocator_t *)&rt->system_pool;

    n00b_interval_tree_t(n00b_alloc_range_t *) *tree = n00b_alloc_with_opts(
        n00b_interval_tree_t(n00b_alloc_range_t *),
        &(n00b_alloc_opts_t){.allocator = pool});
    n00b_interval_tree_init(tree, .allocator = pool);

    if (rt->mmaps.range_tree != nullptr) {
        n00b_static_scan_tree_copy(rt->mmaps.range_tree->root, tree);
    }

    n00b_atomic_store(&n00b_static_scan_tree, (void *)tree);
}

// Lockless augmented-interval point lookup of the cached static tree.  Returns
// the static range that contains `addr`, or nullptr.  Safe during the scan: the
// tree is immutable after its one-time build.
[[n00b::nogc]] static inline n00b_alloc_range_t *
n00b_static_scan_range(uint64_t addr)
{
    n00b_interval_tree_t(n00b_alloc_range_t *) *tree
        = n00b_atomic_load(&n00b_static_scan_tree);
    if (tree == nullptr) {
        return nullptr;
    }
    n00b_interval_node_t(n00b_alloc_range_t *) *node = tree->root;
    while (node != nullptr) {
        if (addr >= node->low && addr < node->high) {
            return node->data;
        }
        if (node->left != nullptr && node->left->maximum > addr) {
            node = node->left;
        }
        else {
            node = node->right;
        }
    }
    return nullptr;
}

static void
n00b_build_scan_tree(n00b_collect_t *ctx)
{
    n00b_build_static_scan_tree_once();

    n00b_allocator_t                       *wp = (n00b_allocator_t *)&ctx->work_pool;
    n00b_interval_tree_t(n00b_scan_owner_t) *tree = n00b_alloc_with_opts(
        n00b_interval_tree_t(n00b_scan_owner_t),
        &(n00b_alloc_opts_t){.allocator = wp});

    n00b_interval_tree_init(tree, .allocator = wp);
    n00b_arena_audit_foreach(n00b_scan_tree_add_allocator, tree);
    ctx->scan_tree = tree;

    // Union bounds of both candidate trees for the per-word fast-reject gate
    // (see n00b_collect_t.scan_floor). Leftmost node's low is the tree
    // minimum; the augmented root's `maximum` is the tree maximum.
    uint64_t floor   = UINT64_MAX;
    uint64_t ceiling = 0;

    if (tree->root != nullptr) {
        n00b_interval_node_t(n00b_scan_owner_t) *n = tree->root;
        while (n->left != nullptr) {
            n = n->left;
        }
        floor   = n->low;
        ceiling = tree->root->maximum;
    }

    n00b_interval_tree_t(n00b_alloc_range_t *) *stree
        = n00b_atomic_load(&n00b_static_scan_tree);
    if (stree != nullptr && stree->root != nullptr) {
        n00b_interval_node_t(n00b_alloc_range_t *) *sn = stree->root;
        while (sn->left != nullptr) {
            sn = sn->left;
        }
        if (sn->low < floor) {
            floor = sn->low;
        }
        if (stree->root->maximum > ceiling) {
            ceiling = stree->root->maximum;
        }
    }

    ctx->scan_floor   = floor;
    ctx->scan_ceiling = ceiling;
}

// The allocators the collector writes while it marks: the to-space it copies
// into, that space's metadata pool, and the collector's own work pool. Guard
// words in their memory move under a guard memo, so walks there take none.
[[n00b::nogc]] static inline bool
n00b_gc_written_during_mark(n00b_collect_t *ctx, n00b_allocator_t *al)
{
    return al == (n00b_allocator_t *)ctx->to_space
        || al == ctx->to_space->vtable.metadata_pool
        || al == (n00b_allocator_t *)&ctx->work_pool;
}

[[n00b::nogc]] static inline n00b_scan_owner_t *
n00b_scan_tree_owner(n00b_collect_t *ctx, uint64_t addr)
{
    n00b_interval_tree_t(n00b_scan_owner_t) *tree = ctx->scan_tree;
    if (tree == nullptr) {
        return nullptr;
    }
    n00b_interval_node_t(n00b_scan_owner_t) *node = tree->root;
    while (node != nullptr) {
        if (addr >= node->low && addr < node->high) {
            return &node->data;
        }
        if (node->left != nullptr && node->left->maximum > addr) {
            node = node->left;
        }
        else {
            node = node->right;
        }
    }
    return nullptr;
}

[[n00b::nogc]] static inline bool
n00b_visit_possible_pointer(n00b_collect_t *ctx, uint64_t **base, size_t i, bool base_checked)
{
    // Returns 'true' if we find a pointer, so that custom marking functions
    // can more easily be data-dependent.

    if (!base_checked) {
        auto mmap_opt = n00b_mmap_by_address(base);

        if (!n00b_option_is_set(mmap_opt)) {
            // The memory is hidden from us, so we should not try to
            // access it.
            return false;
        }
    }

    n00b_inline_hdr_t *fw_hdr;
    n00b_inline_hdr_t *old_hdr;
    uint64_t          *word = base[i];

    // Fast-reject gate: two compares against the union bounds of both
    // candidate trees dispose of the overwhelmingly common non-pointer word
    // (small ints, flags, text bytes, nulls) before any tree descent. Words
    // inside the range still take the authoritative tree paths below.
    if ((uint64_t)word < ctx->scan_floor || (uint64_t)word >= ctx->scan_ceiling) {
        return false;
    }

    // Conservative-scan resolution, with ZERO per-word global mmap lookups.
    // Two authoritative private trees answer every candidate word:
    //
    //   1. ctx->scan_tree — the per-collect transient tree of gc-scannable
    //      arena/pool segments ("visible heaps").  A hit means `word` points
    //      into a heap we can forward; fall through to header resolution.
    //
    //   2. the cached static-object tree — registered `__DATA,n00b_stobj`
    //      ranges (root holders).  A heap miss but static hit means queue the
    //      range for in-place scanning.
    //
    // A miss in BOTH is a definitive non-pointer / dyld / stack / control
    // address: return false.  Once the private tree is built, a miss IS the
    // answer — we never fall back to n00b_mmap_by_address / range_by_address.
    n00b_scan_owner_t *owner = n00b_scan_tree_owner(ctx, (uint64_t)word);
    if (owner == nullptr) {
        n00b_alloc_range_t *range = n00b_static_scan_range((uint64_t)word);
        if (range != nullptr) {
            n00b_add_alloc_range_to_worklist(ctx, range);
        }
        return false;
    }

    // Hit: `word` lands inside a gc-scannable arena/pool segment.  Resolve its
    // header (statics never reach here — they are answered by the static tree
    // above, so no static-range guard is needed).  The hit carries its range's
    // registry record, found on a pool page's first hit, so the lookup skips
    // the global mmap tree.  Heap ranges hold still while the world is stopped,
    // so their backward walks can share the memo; the to-space is the one being
    // filled.
    if (owner->map == nullptr && owner->segment == nullptr) {
        auto map_opt = n00b_mmap_by_address(word);
        if (n00b_option_is_set(map_opt)) {
            owner->map = n00b_option_get(map_opt);
        }
    }

    n00b_mmap_info_t  *map  = owner->map;
    n00b_guard_memo_t *memo = n00b_gc_written_during_mark(ctx, owner->allocator)
                                ? nullptr
                                : &ctx->guard_memo;

    auto ainfo = n00b_find_alloc_info_in(word, map, false, nullptr);

    if (!n00b_alloc_info_is_heap(ainfo)) {
        ainfo = n00b_find_alloc_info_in(word, map, true, memo);

        if (!n00b_alloc_info_is_heap(ainfo)) {
            // Interior pointer into an accepted segment's free space — no live
            // object header at/over it, so nothing to forward.
            return false;
        }
    }

    old_hdr = alloc_info_raw_hdr(ainfo);

    /* Stamp the GC epoch onto the OOB record so the post-mark
     * sweep can tell "reached this round" from "stale, leak". Done
     * every visit, not just first — cheap and idempotent. */
    if (ainfo.kind == n00b_alloc_oob) {
        ainfo.hdr.oob->gc_epoch = ctx->current_epoch;
#if defined(N00B_CENSUS_ENABLED)
        /* Diagnostic site census (debug_leak_detect collects only): count
         * live allocations per origin site. Keyed by the file_name pointer
         * bits; reported + torn down in n00b_collect_internal. */
        if (g_site_census != nullptr && ainfo.hdr.oob->file_name != nullptr) {
            uint64_t ck = (uint64_t)(uintptr_t)ainfo.hdr.oob->file_name;
            bool     cf = false;
            int64_t  cc = n00b_dict_get(g_site_census, ck, &cf);
            int64_t  nv = cc + 1;
            n00b_dict_put(g_site_census, ck, nv);
        }
#endif
    }

    bool in_from_space = owner->allocator == (n00b_allocator_t *)ctx->from_space;

    if (n00b_is_first_visit(ctx, old_hdr, &fw_hdr)) {
        if (in_from_space) {
            // n00b#309 / #368: under pin_all every reached object is treated
            // like an ambiguous-root target. Nothing is copied and no scanned
            // word is rewritten, so a data word that merely aliases a
            // from-space address (`producer_id << 32 | magic`, `head << 32 |
            // cap`, a lock's packed fields) survives the collection intact.
            if (ctx->pin_all || n00b_alloc_is_pinned(ctx, owner->segment, ainfo)) {
                // Ambiguous-root pinned: keep in place.  The sentinel
                // fw_hdr == old_hdr marks "pinned" for this and every
                // subsequent visit.  Pin ALL of this object's pages first so a
                // page-spanning kept object never has a tail page reclaimed,
                // then scan its pointers in place (so referents still evacuate
                // and those in-place slots get rewritten) but do NOT copy it
                // and do NOT rewrite the referring slot.
                fw_hdr = old_hdr;
                n00b_pin_object_pages(ctx, owner->segment, ainfo);
                n00b_scan_pinned_in_place(ctx, ainfo);
            }
            else {
                fw_hdr = n00b_forward_alloc(ctx, old_hdr);
                assert(fw_hdr);
            }
        }
        else {
            fw_hdr = nullptr;
            n00b_add_alloc_to_worklist(ainfo, ctx);
        }
        // This has to happen after we create the fw_hdr object if
        // it's in the heap we're collecting.
        n00b_register_visit(ctx, old_hdr, fw_hdr);
    }
    else {
        if (in_from_space) {
            assert(fw_hdr);
        }
    }

    // Pinned objects (fw_hdr == old_hdr sentinel) stay in place: leave the
    // referring slot pointing at the in-place object.  Only genuinely
    // forwarded objects translate.
    if (in_from_space && fw_hdr != old_hdr) {
        uint64_t *v = n00b_translate_pointer(ctx, old_hdr, base, fw_hdr, i);

        if (v) {
            base[i] = v;
            return true;
        }
    }

    return false;
}

// ============================================================================
// Memory range scanning
// ============================================================================

[[n00b::nogc]] static void
n00b_scan_memory_range(n00b_collect_t *ctx, void *start, size_t nwords)
{
    n00b_debug_census_record_scan_range(start, nwords);

    size_t     i    = nwords;
    uint64_t **base = (uint64_t **)start;

    // Every range on the worklist is enqueued because it is a KNOWN, readable
    // region: a gc-managed arena/pool allocation, a precise static object, or a
    // thread stack whose bounds are given to us explicitly. So each slot is
    // readable by construction — there is no reason to probe. We must NEVER
    // conservatively scan an unknown mapped region: candidate resolution
    // (ctx->scan_tree + the static-object map, miss => ignore) lives entirely
    // in n00b_visit_possible_pointer, with zero per-word/page global mmap
    // lookups (n00b_mmap_by_address) or syscall perms probes
    // (n00b_check_memory_perms).
    while (i--) {
        n00b_visit_possible_pointer(ctx, base, i, true);
    }
}

// ============================================================================
// Thread lock-chain scanning
// ============================================================================

static inline size_t
n00b_words_for_scan(size_t bytes)
{
    return (bytes + sizeof(void *) - 1) / sizeof(void *);
}

static void
n00b_scan_thread_lock_chains(n00b_collect_t *ctx, n00b_thread_record_t *rec)
{
    /* Locks can live in hidden, non-moving pools.  Thread-record scanning
     * updates the chain head, but hidden lock storage is not discoverable
     * through the mmap registry, so scan only the active chains here instead
     * of registering every initialized lock as a durable root. */
    n00b_lock_base_t *lock = n00b_atomic_load(&rec->exclusive_locks);

    while (lock != nullptr) {
        n00b_scan_memory_range(ctx, lock, n00b_words_for_scan(sizeof(n00b_lock_base_t)));
        n00b_process_worklist(ctx);
        lock = n00b_atomic_load(&lock->next_thread_lock);
    }

    n00b_thread_read_log_t *rlog = n00b_atomic_load(&rec->read_locks);

    while (rlog != nullptr) {
        n00b_scan_memory_range(ctx, rlog, n00b_words_for_scan(sizeof(n00b_thread_read_log_t)));
        n00b_process_worklist(ctx);

        n00b_lock_base_t *read_lock = (n00b_lock_base_t *)rlog->obj;
        if (read_lock != nullptr) {
            n00b_scan_memory_range(ctx,
                                   read_lock,
                                   n00b_words_for_scan(sizeof(n00b_lock_base_t)));
            n00b_process_worklist(ctx);
        }

        rlog = rlog->next_entry;
    }
}

// ============================================================================
// Thread stack scanning
// ============================================================================

// Scan ONLY the n00b_thread_t fields that reference relocatable GC-heap objects.
// The struct must NOT be scanned wholesale: it is full of values the collector
// must never follow as pointers --
//   * gc_captured_regs[31]: raw register file of a suspended thread (arbitrary
//     ints); register-reachable objects are pinned by the pin pre-pass, not
//     forwarded from here.  Following these chases non-pointers into arena
//     segments (the runaway backward guard scan).
//   * arena / pool / mmap references: current_allocator, string_scratch_storage,
//     string_scratch_arena, scratch_pool, stack_map, gc_inflight_start -- these
//     point at non-GC allocators / the reserved in-flight region, never at
//     relocatable heap objects.
//   * stack pointers, OS handles, code pointers, and scalars.
// Only these eight fields hold GC-heap objects the collector must keep alive
// (and relocate the slot for): everything else is skipped by construction.
static inline void
n00b_scan_thread_heap_fields(n00b_collect_t *ctx, n00b_thread_t *t)
{
    n00b_scan_memory_range(ctx, (void *)&t->record, 1);
    n00b_scan_memory_range(ctx, (void *)&t->dl_last_error, 1);
    n00b_scan_memory_range(ctx, (void *)&t->name, 1);
    n00b_scan_memory_range(ctx, (void *)&t->callstack, 1);
    n00b_scan_memory_range(ctx, (void *)&t->altstack, 1);
    n00b_scan_memory_range(ctx, (void *)&t->join_result, 1);
    n00b_scan_memory_range(ctx, (void *)&t->finalizer_data, 1);
    n00b_scan_memory_range(ctx, (void *)&t->reap_next, 1);
}

// Resolve the conservative C-stack scan range for a suspended thread's captured
// SP. A thread can be running on its MAIN stack, or — mid signal/crash handler —
// on its alternate SIGNAL stack (`t->altstack`, a registered n00b_callstack_t).
// Pick whichever registered region the captured SP actually lies in and scan
// [sp, region_end). This is what keeps the scanner from asserting base>top (and
// aborting, masking the real fault) when a thread is caught on its altstack.
// Returns false when the SP is in neither known region (e.g. a not-yet-fully-
// on-stack thread, or teardown residue) — the caller then skips the range scan.
// Bytes below SP that the ABI lets leaf code use without moving SP: the x86-64
// SysV and Apple arm64 red zones. A thread suspended in such a leaf can hold its
// only reference to an object there.
#if (defined(__x86_64__) && !defined(_WIN32)) || (defined(__APPLE__) && defined(__aarch64__))
#define N00B_STACK_RED_ZONE 128
#else
#define N00B_STACK_RED_ZONE 0
#endif

static inline bool
n00b_thread_stack_scan_bounds(volatile n00b_thread_t *t,
                              uint64_t              **top_out,
                              uint64_t              **base_out)
{
    uint64_t sp  = (uint64_t)t->stack_top;
    uint64_t low = sp;
    if (n00b_atomic_load(&t->gc_preempt_suspended)) {
        low = sp - N00B_STACK_RED_ZONE;
    }

    // Main stack: SP at/below its high end. On Windows the registered start
    // can be an old StackLimit, so validate an SP below it against the same
    // stack allocation before scanning newly committed pages.
    n00b_mmap_info_t *m = t->stack_map;
    if (m != nullptr && sp < m->end) {
        uint64_t stack_low = m->start;
#ifdef _WIN32
        if (sp < stack_low) {
            MEMORY_BASIC_INFORMATION current, registered;
            if (VirtualQuery((void *)sp, &current, sizeof(current))
                    == sizeof(current)
                && VirtualQuery((void *)(m->end - 1), &registered, sizeof(registered))
                       == sizeof(registered)
                && current.AllocationBase == registered.AllocationBase
                && current.State == MEM_COMMIT
                && !(current.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                stack_low = sp;
            }
        }
#endif
        *top_out  = (uint64_t *)(low < stack_low ? stack_low : low);
        *base_out = (uint64_t *)m->end;
        return true;
    }

    // Alternate signal stack: the thread is executing a signal/crash handler, so
    // SP is above the main stack, inside the altstack's usable region. Scan that
    // region instead.
    n00b_callstack_t *as = n00b_atomic_load(&t->altstack);
    if (as != nullptr
        && sp >= (uint64_t)as->stack_low
        && sp < (uint64_t)as->stack_high) {
        *top_out  = (uint64_t *)(low < (uint64_t)as->stack_low ? (uint64_t)as->stack_low : low);
        *base_out = (uint64_t *)as->stack_high;
        return true;
    }

    return false;
}

static __attribute__((noinline)) void
n00b_scan_thread_stacks(n00b_collect_t *ctx)
{
    volatile n00b_thread_t *t;
    n00b_runtime_t         *rt = n00b_get_runtime();

    for (volatile uint32_t i = 0; i < rt->max_threads; i++) {
        t = n00b_atomic_load(&rt->threads[i].thread);

        if (n00b_thread_slot_is_vacant(t)) {
            continue;
        }

        n00b_gc_stack_policy_t stack_policy        = (n00b_gc_stack_policy_t)t->gc_stack_policy;
        bool                   exact_stack_scanned = false;

        if (stack_policy != N00B_GC_STACK_CONSERVATIVE) {
            n00b_gc_stack_frame_t *frame = (n00b_gc_stack_frame_t *)t->gc_stack_top;

            if (frame) {
                exact_stack_scanned = true;
            }

            while (frame) {
                const n00b_gc_stack_map_t *map = frame->map;

                assert(map);
                assert(map->num_slots == 0 || map->slots);
                assert(map->num_roots == 0 || frame->roots);

                for (uint32_t si = 0; si < map->num_slots; si++) {
                    const n00b_gc_stack_slot_t *slot = &map->slots[si];

                    assert(slot->root_index < map->num_roots);

                    if (!slot->num_words) {
                        continue;
                    }

                    void *addr = frame->roots[slot->root_index];
                    if (addr) {
                        n00b_scan_memory_range(ctx, addr, slot->num_words);
                    }
                }

                frame = frame->prev;
            }
        }

        if (stack_policy == N00B_GC_STACK_EXACT_ONLY
            || (stack_policy == N00B_GC_STACK_EXACT_WITH_FALLBACK && exact_stack_scanned)) {
            goto scan_thread_state;
        }

        // ISOLATION (WP-002 Phase 5, D-025 Q1).  An isolated worker is
        // EXCLUDED from the conservative C-stack range scan below: it has
        // declared (by setting `.isolation` on n00b_thread_spawn) that the
        // GC must NOT treat its raw C stack as a root source — the worker
        // self-registers (via n00b_gc_register_root / an explicit GC stack
        // map) any heap memory it wants kept alive.  This is a NARROW change
        // to the scan-set INCLUSION TEST only; it does not alter the D-007
        // exact-vs-conservative scan model (above) or the shadow-stack
        // push/pop.  The thread struct, its n00b_thread_record_t, and its
        // lock chains are STILL scanned (the `scan_thread_state` block below)
        // so the GC's view of the worker's locks is never corrupted — only
        // the conservative range scan over the worker's own C stack is
        // skipped.  SAFETY: excluding the C-stack scan while a worker holds
        // the only reference to a heap object on that stack loses that root
        // → use-after-free under collection; honoring the self-registration
        // contract is the isolated worker's responsibility.
        if (t->gc_isolated) {
            goto scan_thread_state;
        }

        // Since n00b_scan_memory_range cares about aligned words, we will
        // convert the stack bounds to pointers to the type uint64_t; that
        // way we can subtract the pointers.
        //
        // Remember that the stack grows down (on machines we are
        // targeting), so the "top" will be the smaller address, and
        // the one we want to start with in the scan.

        // The world is stopped here: every other thread is frozen (preemptively
        // suspended with its registers captured).  A teardown can never be in
        // flight concurrently — n00b_thread_destroy runs its WHOLE teardown
        // under critical_execution, which the STW initiator had to acquire
        // before it stopped the world.  A null stack_map is the residue of a
        // teardown that has already nulled its map (and unregistered its stack)
        // but not yet cleared its slot; its raw C stack is gone, so do NOT
        // conservatively scan it — a null stack_map is the signal.  Its
        // struct/record/lock chains are still scanned below (the locks were
        // already released at teardown, so the chains are empty/safe).
        if (t->stack_map == nullptr) {
            goto scan_thread_state;
        }

        // Resolve the scan range against whichever stack the captured SP is on
        // (main or alternate signal stack). A thread caught mid signal/crash
        // handler is on its altstack, above the main stack — resolving the
        // region instead of assuming the main stack is what keeps this from
        // aborting (base>top) and masking the real fault.
        uint64_t *top;
        uint64_t *base;
        if (!n00b_thread_stack_scan_bounds(t, &top, &base)) {
            // SP in neither known region — skip the conservative C-stack scan;
            // the struct / record / lock chains are still scanned below.
            goto scan_thread_state;
        }

        // The stack is always word aligned.
        top  = (uint64_t *)n00b_align_ceil((uint64_t)top, 0x08);
        base = (uint64_t *)n00b_align_floor((uint64_t)base, 0x08);

        if (base <= top) {
            // Degenerate/empty range (e.g. SP at the very top of its region).
            goto scan_thread_state;
        }

        uint64_t num_words = base - top;

        n00b_scan_memory_range(ctx, top, num_words);

scan_thread_state:
        // Scan only the thread struct's GC-heap pointer fields (NOT the whole
        // struct -- see n00b_scan_thread_heap_fields: the captured register file
        // and the arena/pool/stack references must never be followed).
        n00b_scan_thread_heap_fields(ctx, (n00b_thread_t *)t);
        // Scan the thread RECORD too — it lives in `rt->threads[i]` and
        // holds pointers into the GC heap that nothing else scans:
        // `exclusive_locks` / `read_locks` (heads of per-thread lock
        // accounting chains, where each in-heap `n00b_mutex_t` /
        // `n00b_rwlock_t` lives inside a relocatable allocation — e.g.
        // an embedded field of a GC-allocated `Regex`), `lock_wait_target`,
        // and `regex_last_detail` (per-thread error string).
        //
        // Without this scan, after a collection that relocates a heap
        // lock, the chain head still points at the old (freed) address,
        // and the next `n00b_lock_acquire_accounting` walks one step
        // into freed memory.  Reproducible by running `regex_count_all`
        // in a tight loop over a multi-hundred-KB haystack — the SIMD
        // match path generates enough GC pressure to force a collection
        // mid-lock, after ~1000 iterations.
        n00b_scan_memory_range(ctx,
                               (void *)&rt->threads[i],
                               n00b_words_for_scan(sizeof(n00b_thread_record_t)));
        n00b_scan_thread_lock_chains(ctx, &rt->threads[i]);
    }

    // Keep the reap-pending chain alive (WP-001).  A dead worker queued for
    // reaping has had its rt->threads[] slot cleared, so the loop above never
    // reaches it: the ONLY reference to its struct — and to the n00b_callstack_t
    // descriptors it still owns via ->callstack / ->altstack, which the reaper
    // returns to the pool — is this raw chain.  Both the struct and the
    // descriptors live in the GC-visible runtime_obj_pool, so without marking
    // them here a collection reclaims them out from under the reaper, which then
    // reads freed memory at reap time (observed at shutdown:
    // n00b_callstack_pool_return faulting on a freed descriptor).  The world is
    // stopped (we hold critical_execution; every other thread is suspended), so
    // the chain is stable.  Mark each entry's struct (which keeps it AND, via the
    // worklist trace, its ->callstack/->altstack descriptors); we do NOT scan a
    // dead worker's C stack — it is gone.
    n00b_thread_t *reap_t = rt->reap_pending;
    while (reap_t != nullptr) {
        n00b_thread_t *reap_next = reap_t->reap_next;
        // Mark the struct itself (conservatively, via the pointer slot), then
        // scan its contents so the worklist trace reaches the ->callstack /
        // ->altstack descriptors it still owns.
        n00b_scan_memory_range(ctx, (void *)&reap_t, 1);
        // Same deliberate field set as the live-thread scan (keeps the
        // ->callstack/->altstack descriptors the reaper still owns); never scan
        // the whole struct.
        n00b_scan_thread_heap_fields(ctx, reap_t);
        reap_t = reap_next;
    }

    // Keep pooled callstack descriptors alive. Once a dead worker is reaped,
    // its callstack and altstack descriptors move from reap_pending to
    // rt->callstack_pool. The regions remain mapped for reuse, and spawn pulls
    // descriptors from this free-list, so the free-list itself is a runtime
    // root. Without this, GC can reclaim/recycle a descriptor while
    // rt->callstack_pool still points at it; a later spawn reuses a stale
    // descriptor and the next reaper faults in n00b_callstack_pool_return.
    n00b_callstack_t *pooled = rt->callstack_pool;
    while (pooled != nullptr) {
        n00b_callstack_t *next = pooled->pool_next;
        n00b_scan_memory_range(ctx, (void *)&pooled, 1);
        pooled = next;
    }
}

// ============================================================================
// Runtime scanning (argv / envp)
// ============================================================================

static void
n00b_scan_runtime(n00b_collect_t *ctx)
{
    n00b_runtime_t *rt = n00b_get_runtime();

    // Scan argv array (array of char *)
    if (rt->argv.data && rt->argv.len) {
        n00b_scan_memory_range(ctx,
                               rt->argv.data,
                               rt->argv.len * sizeof(char *) / sizeof(void *));
        n00b_process_worklist(ctx);
    }

    // Scan envp array (array of char *)
    if (rt->envp.data && rt->envp.len) {
        n00b_scan_memory_range(ctx,
                               rt->envp.data,
                               rt->envp.len * sizeof(char *) / sizeof(void *));
        n00b_process_worklist(ctx);
    }

    n00b_scan_pending_finalizers(ctx);
}

// ============================================================================
// User-registered root scanning
// ============================================================================

static void
n00b_scan_roots(n00b_collect_t *ctx)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    size_t          n  = rt->gc_roots.len;

#if defined(N00B_CENSUS_ENABLED)
    n00b_debug_census_t *census = g_debug_census;
    if (census != nullptr) {
        census->gc_root_count = (uint64_t)n;
    }
#endif
    for (size_t i = 0; i < n; i++) {
        n00b_gc_root_t *root     = &rt->gc_roots.data[i];
        uint64_t        start_ns = 0;

#if defined(N00B_CENSUS_ENABLED)
        if (census != nullptr) {
            uint64_t words = (uint64_t)root->num_words;
            census->gc_root_words += words;
            if (words > census->gc_root_max_words) {
                census->gc_root_max_words = words;
                census->gc_root_max_addr  = (uint64_t)(uintptr_t)root->addr;
                census->gc_root_max_index = (uint64_t)i;
            }
            start_ns = n00b_gc_timestamp_ns();
        }
#else
        (void)start_ns;
#endif

        n00b_scan_memory_range(ctx, root->addr, root->num_words);
        n00b_process_worklist(ctx);

#if defined(N00B_CENSUS_ENABLED)
        if (census != nullptr) {
            uint64_t elapsed_ns = n00b_gc_elapsed_ns(start_ns, n00b_gc_timestamp_ns());
            if (elapsed_ns > census->gc_root_slowest_ns) {
                census->gc_root_slowest_ns    = elapsed_ns;
                census->gc_root_slowest_words = (uint64_t)root->num_words;
                census->gc_root_slowest_addr  = (uint64_t)(uintptr_t)root->addr;
                census->gc_root_slowest_index = (uint64_t)i;
            }
        }
#endif
    }
}

// ============================================================================
// Root registration API
// ============================================================================
//
// ncc emits TU-scope root tables into a linker section. n00b registers those
// tables during `n00b_init`, after `runtime->gc_roots` exists. No runtime
// allocation is needed before the n00b world exists.

static size_t
n00b_gc_register_root_section_entries(const n00b_gc_root_section_entry_t *start,
                                      const n00b_gc_root_section_entry_t *stop)
{
    if (start == nullptr || stop == nullptr || stop < start) {
        return 0;
    }

    size_t                              count = 0;
    const n00b_gc_root_section_entry_t *entry = start;
    for (; entry < stop; entry++) {
        if (entry->roots == nullptr || entry->count == 0) {
            continue;
        }
        n00b_gc_register_roots(entry->roots, entry->count);
        count++;
    }

    return count;
}

#if defined(__APPLE__)
static size_t
n00b_gc_register_macho_root_section(const struct mach_header *hdr, intptr_t slide)
{
    if (hdr == nullptr || hdr->magic != MH_MAGIC_64) {
        return 0;
    }

    const struct mach_header_64 *header   = (const struct mach_header_64 *)hdr;
    const uint8_t               *cursor   = (const uint8_t *)&header[1];
    const uint8_t *const         cmds_end = cursor + header->sizeofcmds;
    size_t                       count    = 0;

    for (uint32_t i = 0; i < header->ncmds; i++) {
        if ((size_t)(cmds_end - cursor) < sizeof(struct load_command)) {
            return count;
        }

        const struct load_command *lc = (const struct load_command *)cursor;
        if (lc->cmdsize < sizeof(struct load_command)
            || lc->cmdsize > (uint32_t)(cmds_end - cursor)) {
            return count;
        }

        if (lc->cmd == LC_SEGMENT_64 && lc->cmdsize >= sizeof(struct segment_command_64)) {
            const struct segment_command_64 *seg = (const struct segment_command_64 *)cursor;
            size_t sections_bytes = (size_t)seg->nsects * sizeof(struct section_64);

            if (sections_bytes <= (size_t)(lc->cmdsize - sizeof(struct segment_command_64))) {
                const struct section_64 *section = (const struct section_64 *)(seg + 1);

                for (uint32_t j = 0; j < seg->nsects; j++) {
                    if (strncmp(section[j].segname, "__DATA", 16) != 0
                        || strncmp(section[j].sectname, "n00b_gcroots", 16) != 0) {
                        continue;
                    }

                    uintptr_t start_addr = (uintptr_t)section[j].addr + (uintptr_t)slide;
                    size_t    n_entries
                        = (size_t)section[j].size / sizeof(n00b_gc_root_section_entry_t);
                    const n00b_gc_root_section_entry_t *start
                        = (const n00b_gc_root_section_entry_t *)start_addr;

                    count += n00b_gc_register_root_section_entries(start, start + n_entries);
                }
            }
        }

        cursor += lc->cmdsize;
    }

    return count;
}
#elif defined(_WIN32)
#if defined(_MSC_VER) && !defined(__clang__)
#pragma section(".n00br$a", read)
#pragma section(".n00br$z", read)
#define N00B_GC_ROOT_SECTION_PRE(section_name) __declspec(allocate(section_name))
#define N00B_GC_ROOT_SECTION_POST(section_name)
#else
#define N00B_GC_ROOT_SECTION_PRE(section_name)
#define N00B_GC_ROOT_SECTION_POST(section_name) [[gnu::section(section_name), gnu::used]]
#endif

N00B_GC_ROOT_SECTION_PRE(".n00br$a")
static const n00b_gc_root_section_entry_t
    __n00b_gc_root_section_start N00B_GC_ROOT_SECTION_POST(".n00br$a")
    = {0};

N00B_GC_ROOT_SECTION_PRE(".n00br$z")
static const n00b_gc_root_section_entry_t
    __n00b_gc_root_section_end N00B_GC_ROOT_SECTION_POST(".n00br$z")
    = {0};
#else
extern const n00b_gc_root_section_entry_t __start_n00b_gcroots[] __attribute__((weak));
extern const n00b_gc_root_section_entry_t __stop_n00b_gcroots[] __attribute__((weak));
#endif

#ifdef N00B_DEBUG
void (*n00b_gc_register_root_hook)(void *) = nullptr;
#endif

void
_n00b_gc_register_root(void *addr, size_t num_words)
{
    if (addr == nullptr || num_words == 0) {
        return;
    }

    n00b_runtime_t *rt    = n00b_get_runtime();
    auto            roots = &rt->gc_roots;

    n00b_data_write_lock(roots->lock);
#ifdef N00B_DEBUG
    if (n00b_gc_register_root_hook != nullptr) {
        n00b_gc_register_root_hook(addr);
    }
#endif

    for (size_t i = 0; i < roots->len; i++) {
        n00b_gc_root_t existing = roots->data[i];

        if (existing.addr == addr) {
            if (existing.num_words < num_words) {
                roots->data[i].num_words = num_words;
            }
            n00b_data_unlock(roots->lock);
            return;
        }
    }

    _n00b_list_ensure_cap(roots, roots->len + 1);
    roots->data[roots->len++] = (n00b_gc_root_t){
        .addr      = addr,
        .num_words = num_words,
    };

    n00b_data_unlock(roots->lock);
}

void
n00b_gc_register_roots(const n00b_gc_root_t *roots, size_t count)
{
    if (roots == nullptr || count == 0) {
        return;
    }

    for (size_t i = 0; i < count; i++) {
        _n00b_gc_register_root(roots[i].addr, roots[i].num_words);
    }
}

void
_n00b_gc_register_static_roots(void)
{
#if defined(__APPLE__)
    uint32_t image_count = _dyld_image_count();

    for (uint32_t i = 0; i < image_count; i++) {
        (void)n00b_gc_register_macho_root_section(_dyld_get_image_header(i),
                                                  _dyld_get_image_vmaddr_slide(i));
    }
#elif defined(_WIN32)
    (void)n00b_gc_register_root_section_entries(&__n00b_gc_root_section_start + 1,
                                                &__n00b_gc_root_section_end);
#else
    if (__start_n00b_gcroots != nullptr && __stop_n00b_gcroots != nullptr) {
        (void)n00b_gc_register_root_section_entries(__start_n00b_gcroots, __stop_n00b_gcroots);
    }
#endif
}

void
_n00b_gc_unregister_root(void *addr)
{
    if (addr == nullptr) {
        return;
    }

    n00b_runtime_t *rt    = n00b_get_runtime();
    auto            roots = &rt->gc_roots;

    n00b_data_write_lock(roots->lock);

    for (size_t i = 0; i < roots->len; i++) {
        if (roots->data[i].addr == addr) {
            roots->len--;
            if (i < roots->len) {
                memmove(roots->data + i,
                        roots->data + i + 1,
                        (roots->len - i) * sizeof(*roots->data));
            }
            n00b_data_unlock(roots->lock);
            return;
        }
    }

    n00b_data_unlock(roots->lock);
}

// ============================================================================
// Mostly-copying pin support (ambiguous-root pinning)
//
// A preemptively-suspended thread's captured general-purpose registers are
// AMBIGUOUS roots: a value captured at an arbitrary PC may be an interior
// pointer or a non-pointer, and — unlike a stack slot, whose memory is shared
// and rewritten in place — a register is restored from a copy that the GC
// discards at resume.  Forwarding such a root therefore leaves the resumed
// thread holding a pointer into freed from-space.  The fix is to PIN the object
// the register implicates: keep it in place (do not move it), so the original
// register value stays valid.  Pinning is PAGE-granular (the gateway RSS
// constraint): only the pages an implicated object occupies are retained; the
// rest of the segment is returned to the kernel at collect end.
//
// This pre-pass runs BEFORE any forwarding and only sets bits; the forward
// phase and the page-reclaim pass consume the bitmap.
// ============================================================================

// Allocate a zeroed page-pin bitmap (one bit per n00b_page_size of `data`) for
// every from-space segment.  Descriptors live in system_pool (non-moving), so
// the bitmap pointer parked on the descriptor is stable across the collect.
static void
n00b_pin_bitmaps_alloc(n00b_collect_t *ctx)
{
    n00b_allocator_t *scratch = (n00b_allocator_t *)&ctx->work_pool;
    n00b_segment_t   *seg = ctx->from_space->current_segment;

    while (seg) {
        uint64_t npages = (seg->size + n00b_page_size - 1) / n00b_page_size;
        uint64_t nbytes = (npages + 7) / 8;
        seg->pin_bitmap = n00b_alloc_array_with_opts(uint8_t,
                                                     nbytes,
                                                     &(n00b_alloc_opts_t){.allocator = scratch});
        seg->pin_max_alloc_len = 0;
        seg                    = seg->next_segment;
    }
}

// In-arena footprint [*start, *start + *len) of an allocation: for an inline
// alloc the raw header IS the in-arena start; for an OOB alloc the in-arena
// guard/header is `hcur` and `alloc_len` is the full footprint.
static inline void
n00b_alloc_footprint(n00b_alloc_info_t ainfo, char **start, uint64_t *len)
{
    if (ainfo.kind == n00b_alloc_oob) {
        *start = (char *)ainfo.hdr.oob->hcur;
        *len   = ainfo.hdr.oob->alloc_len;
    }
    else {
        *start = (char *)ainfo.hdr.in_line;
        *len   = ainfo.hdr.in_line->alloc_len;
    }
}

// The from-space segment whose data region contains `addr`, or null. Answered
// from the per-collect scan tree: the from-space chain gains a segment for
// every retained page run, so walking it per lookup grows with the heap's
// fragmentation.
static inline n00b_segment_t *
n00b_from_segment_for(n00b_collect_t *ctx, char *addr)
{
    n00b_interval_tree_t(n00b_scan_owner_t) *tree = ctx->scan_tree;
    uint64_t                                 a    = (uint64_t)addr;

    ctx->segment_lookups++;
    if (tree == nullptr) {
        return nullptr;
    }
    n00b_interval_node_t(n00b_scan_owner_t) *node = tree->root;
    while (node != nullptr) {
        ctx->segment_lookup_steps++;
        if (a >= node->low && a < node->high) {
            if (node->data.allocator != (n00b_allocator_t *)ctx->from_space) {
                return nullptr;
            }
            return node->data.segment;
        }
        if (node->left != nullptr && node->left->maximum > a) {
            node = node->left;
        }
        else {
            node = node->right;
        }
    }
    return nullptr;
}

// The from-space segment holding `addr`: `hint` when it does, else a lookup.
static inline n00b_segment_t *
n00b_from_segment_hinted(n00b_collect_t *ctx, n00b_segment_t *hint, char *addr)
{
    if (hint != nullptr && addr >= hint->data && addr < hint->data + hint->size) {
        return hint;
    }
    return n00b_from_segment_for(ctx, addr);
}

// Mark EVERY page of `ainfo`'s in-arena footprint pinned.  Used both by the
// register pre-pass and, during forwarding, to retain the full footprint of any
// object kept in place — so a page-spanning kept object never has a tail page
// returned to the kernel.
static void
n00b_pin_object_pages(n00b_collect_t *ctx, n00b_segment_t *hint, n00b_alloc_info_t ainfo)
{
    char    *fs;
    uint64_t fl;
    n00b_alloc_footprint(ainfo, &fs, &fl);
    if (!fs || fl == 0) {
        return;
    }
    n00b_segment_t *seg = n00b_from_segment_hinted(ctx, hint, fs);
    if (!seg || !seg->pin_bitmap) {
        return;
    }
    char *fe = fs + fl;
    if (fe > seg->data + seg->size) {
        fe = seg->data + seg->size; // objects never span segments; clamp
    }
    uint64_t first = (uint64_t)(fs - seg->data) / n00b_page_size;
    uint64_t last  = (uint64_t)(fe - 1 - seg->data) / n00b_page_size;
    for (uint64_t pg = first; pg <= last; pg++) {
        seg->pin_bitmap[pg >> 3] |= (uint8_t)(1u << (pg & 7));
    }
    // The retained runs this segment leaves behind are bounded by the largest
    // thing pinned in it (n00b#395); see n00b_reclaim_pinned_pages.
    if (fl > seg->pin_max_alloc_len) {
        seg->pin_max_alloc_len = fl;
    }
}

// Mark every page of the raw range [start, start+len) pinned, if it falls in a
// from-space segment.  Used to retain a suspended thread's in-flight allocation
// reservation (n00b_thread_t.gc_inflight_*): the storage is reserved but not yet
// registered, so the trace can't discover it — pin its page(s) so reclaim keeps
// the region until the thread resumes and registers it.
static void
n00b_pin_raw_range(n00b_collect_t *ctx, char *start, uint64_t len)
{
    if (!start || len == 0) {
        return;
    }
    n00b_segment_t *seg = n00b_from_segment_for(ctx, start);
    if (!seg || !seg->pin_bitmap) {
        return; // not in the arena being collected (e.g. a different segment)
    }
    char *end = start + len;
    if (end > seg->data + seg->size) {
        end = seg->data + seg->size;
    }
    uint64_t first = (uint64_t)(start - seg->data) / n00b_page_size;
    uint64_t last  = (uint64_t)(end - 1 - seg->data) / n00b_page_size;
    for (uint64_t pg = first; pg <= last; pg++) {
        seg->pin_bitmap[pg >> 3] |= (uint8_t)(1u << (pg & 7));
    }
    // The reservation becomes an object of exactly this length once the
    // suspended thread resumes, and that thread may have been stopped before
    // it could record the extent itself (n00b_arena_note_alloc_extent).
    if (len > seg->pin_max_alloc_len) {
        seg->pin_max_alloc_len = len;
    }
}

// If `candidate` resolves to a live allocation in the arena we are collecting,
// pin every page that allocation's in-arena footprint spans.  No forward, no
// rewrite.  Mirrors the resolution filter used by n00b_visit_possible_pointer.
static void
n00b_pin_candidate(n00b_collect_t *ctx, void *candidate)
{
    if ((uint64_t)candidate < ctx->scan_floor || (uint64_t)candidate >= ctx->scan_ceiling) {
        return;
    }
    // Only addresses in a segment of the arena we are collecting can pin.
    n00b_segment_t *seg = n00b_from_segment_for(ctx, candidate);
    if (seg == nullptr) {
        return;
    }

    n00b_alloc_info_t ainfo = n00b_find_alloc_info(candidate, .scan_for_header = true);
    if (!n00b_alloc_info_is_heap(ainfo)) {
        return;
    }

    n00b_pin_object_pages(ctx, seg, ainfo);
}

// Pin pre-pass: BEFORE any forwarding, pin the objects implicated by every
// preemptively-suspended thread's captured registers.  Only threads with
// `gc_preempt_suspended` set have valid captured registers (the comment on
// n00b_thread_t.gc_captured_regs); that flag also excludes the collector's own
// thread, whose registers are live in hardware, not captured.  The conservative
// C-stack is intentionally NOT pinned here: stack memory is shared with the
// suspended thread and is rewritten in place by the forward phase, so those
// roots stay coherent without pinning.
static __attribute__((noinline)) void
n00b_pin_prepass(n00b_collect_t *ctx)
{
    n00b_runtime_t *rt = n00b_get_runtime();

    for (uint32_t i = 0; i < rt->max_threads; i++) {
        volatile n00b_thread_t *t = n00b_atomic_load(&rt->threads[i].thread);
        if (n00b_thread_slot_is_vacant(t)) {
            continue;
        }
        if (!n00b_atomic_load(&t->gc_preempt_suspended)) {
            continue;
        }
        for (uint32_t r = 0; r < 31; r++) {
            n00b_pin_candidate(ctx, (void *)t->gc_captured_regs[r]);
        }
        for (uint32_t v = 0; v < t->gc_captured_vreg_words; v++) {
            n00b_pin_candidate(ctx, (void *)t->gc_captured_vregs[v]);
        }
        // Pin this thread's in-flight allocation reservation: storage was bumped
        // but the object's GC metadata is not yet registered, so the trace can't
        // discover it.  Without pinning, its page would be reclaimed out from
        // under the suspended thread (the async-seal use-after-reclaim).
        // `start` is the publication gate (see n00b_arena_alloc): acquire it
        // first, so a non-null value guarantees the matching `len` is visible.
        char *infl_start = (char *)atomic_load_explicit(&t->gc_inflight_start,
                                                        memory_order_acquire);
        uint64_t infl_len = n00b_atomic_load(&t->gc_inflight_len);
        if (infl_start != nullptr && infl_len != 0) {
            n00b_pin_raw_range(ctx, infl_start, infl_len);
            // `start` is one past the end of the allocation before it, so the
            // thread's copy of `start` resolves to that allocation.  Keep it in
            // place, or forwarding it would move `start` onto whatever the
            // collector copies after it.
            n00b_pin_candidate(ctx, infl_start);
        }
    }
}

// True if ANY page of `ainfo`'s in-arena footprint is pinned.  Checking the
// whole footprint (not just the start page) is what makes a page-spanning object
// that overlaps a pinned page get kept in place — and the caller then pins its
// remaining pages so reclaim retains the entire object.
static bool
n00b_alloc_is_pinned(n00b_collect_t *ctx, n00b_segment_t *hint, n00b_alloc_info_t ainfo)
{
    char    *fs;
    uint64_t fl;
    n00b_alloc_footprint(ainfo, &fs, &fl);
    if (!fs || fl == 0) {
        return false;
    }
    n00b_segment_t *seg = n00b_from_segment_hinted(ctx, hint, fs);
    if (!seg || !seg->pin_bitmap) {
        return false;
    }
    char *fe = fs + fl;
    if (fe > seg->data + seg->size) {
        fe = seg->data + seg->size;
    }
    uint64_t first = (uint64_t)(fs - seg->data) / n00b_page_size;
    uint64_t last  = (uint64_t)(fe - 1 - seg->data) / n00b_page_size;
    for (uint64_t pg = first; pg <= last; pg++) {
        if ((seg->pin_bitmap[pg >> 3] >> (pg & 7)) & 1) {
            return true;
        }
    }
    return false;
}

// Pinned (un-moved) object: queue an IN-PLACE scan of its pointer slots so its
// referents are still evacuated and those slots rewritten in place, but do NOT
// copy it.  For OOB arenas, also migrate the object's metadata record into the
// rebuilt dict (same user_ptr — the object does not move) so it stays resolvable
// after the collect; without this the next collect can't find the record and
// would reclaim the live object.
static void
n00b_scan_pinned_in_place(n00b_collect_t *ctx, n00b_alloc_info_t ainfo)
{
    void               *scan_start;
    bool                no_scan;
    uint32_t            nwords;
    n00b_gc_scan_kind_t scan_kind;
    n00b_gc_scan_cb_t   scan_cb;
    void               *scan_user;
    n00b_alloc_info_t   origin;

    if (ainfo.kind == n00b_alloc_oob) {
        n00b_oob_hdr_t *oob = ainfo.hdr.oob;
        scan_start          = oob->user_ptr;
        no_scan             = oob->no_scan;
        scan_kind           = (n00b_gc_scan_kind_t)oob->scan_kind;
        scan_cb             = oob->scan_cb;
        scan_user           = oob->scan_user;
        origin              = (n00b_alloc_info_t){.kind = n00b_alloc_oob, .hdr.oob = oob};
        if (oob->ptr_words_known) {
            nwords = oob->ptr_words;
        }
        else {
            nwords = (oob->alloc_len - arena_overhead(ctx->from_space)) / sizeof(void *);
        }
        if (ctx->from_space->vtable.metadata_pool && ctx->to_space->vtable.metadata) {
            n00b_oob_hdr_t *keep = n00b_alloc_with_opts(
                n00b_oob_hdr_t,
                &(n00b_alloc_opts_t){.allocator = ctx->to_space->vtable.metadata_pool});
            memcpy(keep, oob, sizeof(n00b_oob_hdr_t));
            // user_ptr + hcur unchanged: the object stays in place.
            n00b_md_put(ctx->to_space->vtable.metadata, keep->user_ptr, keep);
        }
    }
    else {
        n00b_inline_hdr_t *ih = ainfo.hdr.in_line;
        scan_start            = (char *)ih + arena_overhead(ctx->from_space);
        no_scan               = ih->no_scan;
        scan_kind             = (n00b_gc_scan_kind_t)ih->scan_kind;
        scan_cb               = ih->scan_cb;
        scan_user             = ih->scan_user;
        origin = (n00b_alloc_info_t){.kind = n00b_alloc_inline, .hdr.in_line = ih};
        if (ih->ptr_words_known) {
            nwords = ih->ptr_words;
        }
        else {
            nwords = (ih->alloc_len - arena_overhead(ctx->from_space)) / sizeof(void *);
        }
    }

    n00b_add_described_scan_range_to_worklist(ctx,
                                              scan_start,
                                              nwords,
                                              n00b_effective_scan_kind(scan_kind, no_scan),
                                              scan_cb,
                                              scan_user,
                                              origin);
}

// ============================================================================
// Metadata-bearing pool walk + leak sweep.
//
// Every pool initialised with external_metadata=true registers in
// rt->metadata_pools at construction. n00b_scan_metadata_pools walks
// each pool's metadata dict, finds buckets whose OOB record carries
// alive=1, stamps the current epoch on the record, and adds the
// alloc to the worklist so its outbound pointers get traced — the
// pool walk thereby substitutes for the older "register pool struct
// as a GC root" workaround.
//
// n00b_sweep_metadata_pool_leaks runs after the worklist has
// drained.  Any alive alloc whose gc_epoch is still stale was not
// reachable from real roots and is by definition a leak: the sweep
// frees it back to the pool. When rt->debug_leak_detect is set,
// each leak is printed with its tinfo + alloc_len + file_name so
// callers using n00b_debug_find_leaks() get a precise origin.
// ============================================================================

static void
n00b_scan_one_alive_alloc_oob(n00b_collect_t *ctx, n00b_oob_hdr_t *oob)
{
    /* Stamp the epoch — the alloc is reachable in this collection
     * by virtue of being an alive pool slot, regardless of whether
     * any real root pointed at it. The post-mark sweep treats
     * "alive && epoch stale" as leak; this prevents the pool walk
     * itself from manufacturing false-positive leaks. */
    oob->gc_epoch = ctx->current_epoch;

    n00b_gc_scan_kind_t kind = (n00b_gc_scan_kind_t)oob->scan_kind;
    kind                     = n00b_effective_scan_kind(kind, oob->no_scan);

    /* For non-scannable allocs we still want the epoch stamp above. */
    if (kind == N00B_GC_SCAN_KIND_NONE) {
        return;
    }

    void    *start;
    uint32_t n;
    start = oob->user_ptr;
#if !defined(N00B_DISABLE_PTR_WORDS)
    n = oob->ptr_words;
    if (!n)
#endif
    {
        /* Pool allocs are not in any arena, so the arena_overhead
         * subtraction n00b_add_alloc_to_worklist applies doesn't
         * apply here: alloc_len is the bare user request. */
        n = oob->alloc_len / sizeof(void *);
    }

    n00b_add_described_scan_range_to_worklist(ctx,
                                              start,
                                              n,
                                              kind,
                                              oob->scan_cb,
                                              oob->scan_user,
                                              (n00b_alloc_info_t){
                                                  .kind    = n00b_alloc_oob,
                                                  .hdr.oob = oob,
                                              });
}

static void
n00b_scan_metadata_pools(n00b_collect_t *ctx)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    if (!rt || rt->metadata_pools.data == nullptr) {
        return;
    }

    size_t npools = n00b_list_len(rt->metadata_pools);
    for (size_t pi = 0; pi < npools; pi++) {
        n00b_allocator_t *allocator = n00b_list_get(rt->metadata_pools, pi);
        if (allocator == nullptr || allocator->metadata == nullptr) {
            continue;
        }

        /* Walk the dict store's bucket array directly. We can't use
         * the public get/put API to iterate, but the bucket layout
         * is stable: occupied = key != nullptr && !(flags & DELETED). */
        __n00b_internal_type_erased_store_t *store
            = (__n00b_internal_type_erased_store_t *)n00b_atomic_load(
                &allocator->metadata->store);
        if (store == nullptr) {
            continue;
        }
        uint32_t slots = store->last_slot + 1;
        for (uint32_t bi = 0; bi < slots; bi++) {
            n00b_dict_bucket_t *b = &store->buckets[bi];
            if (b->hv == (n00b_uint128_t)0) {
                continue;
            }
            if (n00b_atomic_load(&b->flags) & N00B_HT_FLAG_DELETED) {
                continue;
            }
            n00b_oob_hdr_t *oob = (n00b_oob_hdr_t *)store->values[bi];
            if (oob == nullptr || !oob->alive) {
                continue;
            }
            n00b_scan_one_alive_alloc_oob(ctx, oob);
        }
    }
}

static void
n00b_sweep_metadata_pool_leaks(n00b_collect_t *ctx)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    if (!rt || rt->metadata_pools.data == nullptr) {
        return;
    }

    bool debug = n00b_atomic_load(&rt->debug_leak_detect);

    size_t npools = n00b_list_len(rt->metadata_pools);
    for (size_t pi = 0; pi < npools; pi++) {
        n00b_allocator_t *allocator = n00b_list_get(rt->metadata_pools, pi);
        if (allocator == nullptr || allocator->metadata == nullptr) {
            continue;
        }

        __n00b_internal_type_erased_store_t *store
            = (__n00b_internal_type_erased_store_t *)n00b_atomic_load(
                &allocator->metadata->store);
        if (store == nullptr) {
            continue;
        }
        uint32_t slots = store->last_slot + 1;

        for (uint32_t bi = 0; bi < slots; bi++) {
            n00b_dict_bucket_t *b = &store->buckets[bi];
            if (b->hv == (n00b_uint128_t)0) {
                continue;
            }
            if (n00b_atomic_load(&b->flags) & N00B_HT_FLAG_DELETED) {
                continue;
            }
            n00b_oob_hdr_t *oob = (n00b_oob_hdr_t *)store->values[bi];
            if (oob == nullptr || !oob->alive) {
                continue;
            }
            if (oob->gc_epoch == ctx->current_epoch) {
                /* Reached by this collection — alive and traced,
                 * not a leak. */
                continue;
            }

            /* Stale-epoch alive alloc — leak. Two policies:
             *
             *   debug=true   → record the callsite for the post-STW census
             *                  report. Do NOT reclaim:
             *                  the false-positive case (e.g. live
             *                  state reachable only through a
             *                  non-scannable container) would
             *                  otherwise turn into use-after-free.
             *                  Bumping gc_epoch keeps the same
             *                  alloc from being reported every
             *                  collection.
             *   debug=false  → silent reclaim path, the original
             *                  "auto-return-to-pool" design.
             */
            if (debug) {
                n00b_debug_census_record_leak(allocator, oob);
                oob->gc_epoch = ctx->current_epoch;
                continue;
            }

            /* Mark dead, then return to pool. n00b_free runs
             * finalizers + the allocator's free routine — same
             * teardown the caller would have done had they
             * remembered to. */
            void *user_ptr = oob->user_ptr;
            oob->alive     = 0;
            if (user_ptr) {
                n00b_free(user_ptr);
            }
        }
    }
}

// ============================================================================
// Finalizers
//
// A finalizer's data word is a reference held by its object: while the object
// is reached, the data is traced (and updated if its target moves). An object
// the mark did not reach survives this collection: its finalizer is detached
// onto rt->pending_finalizers, the data is traced from there, and
// n00b_gc_run_finalizers runs it once the world is running again. The next
// collection that finds the object unreachable reclaims it.
// ============================================================================

static inline void
n00b_trace_finalizer_data(n00b_collect_t *ctx, void **slot)
{
    n00b_visit_possible_pointer(ctx, (uint64_t **)slot, 0, true);
}

static void
n00b_pending_finalizer_push(n00b_runtime_t *rt, n00b_finalizer_t fn, void *user)
{
    n00b_allocator_t *sp  = (n00b_allocator_t *)&rt->system_pool;
    uint32_t          len = n00b_atomic_load(&rt->pending_finalizers_len);

    if (len == rt->pending_finalizers_cap) {
        uint32_t               cap   = len ? len * 2 : 16;
        n00b_finalizer_info_t *grown = n00b_alloc_array_with_opts(
            n00b_finalizer_info_t,
            cap,
            &(n00b_alloc_opts_t){.allocator = sp});
        n00b_finalizer_info_t *old = rt->pending_finalizers;
        if (len) {
            memcpy(grown, old, len * sizeof(n00b_finalizer_info_t));
        }
        rt->pending_finalizers     = grown;
        rt->pending_finalizers_cap = cap;
        if (old != nullptr) {
            n00b_free(old, .allocator = sp);
        }
    }

    rt->pending_finalizers[len] = (n00b_finalizer_info_t){
        .funcptr  = fn,
        .user_ptr = user,
    };
    n00b_atomic_store(&rt->pending_finalizers_len, len + 1);
}

// Pending finalizers from earlier collections are roots until they run.
static void
n00b_scan_pending_finalizers(n00b_collect_t *ctx)
{
    n00b_runtime_t *rt  = n00b_get_runtime();
    uint32_t        len = n00b_atomic_load(&rt->pending_finalizers_len);

    for (uint32_t i = 0; i < len; i++) {
        n00b_trace_finalizer_data(ctx, &rt->pending_finalizers[i].user_ptr);
    }
    n00b_process_worklist(ctx);
}

// One pass over the from-space OOB records that carry a finalizer. A reached
// record has its data traced from its to-space copy. With `queue_dead`, an
// unreached one is detached and resurrected. Returns the number reached.
static uint64_t
n00b_finalizers_oob_pass(n00b_collect_t *ctx, bool queue_dead)
{
    n00b_arena_t   *from = ctx->from_space;
    n00b_runtime_t *rt   = n00b_get_runtime();
    uint64_t        reached = 0;

    __n00b_internal_type_erased_store_t *store
        = (__n00b_internal_type_erased_store_t *)n00b_atomic_load(&from->vtable.metadata->store);
    if (store == nullptr) {
        return 0;
    }
    uint32_t slots = store->last_slot + 1;

    for (uint32_t bi = 0; bi < slots; bi++) {
        n00b_dict_bucket_t *b = &store->buckets[bi];
        if (b->hv == (n00b_uint128_t)0
            || (n00b_atomic_load(&b->flags) & N00B_HT_FLAG_DELETED)) {
            continue;
        }
        n00b_oob_hdr_t *oob = (n00b_oob_hdr_t *)store->values[bi];
        if (oob == nullptr || !oob->alive || oob->finalizer == nullptr) {
            continue;
        }

        bool               found = false;
        n00b_inline_hdr_t *fw    = n00b_gc_ptr_table_get(&ctx->visited, oob, &found);

        if (found) {
            n00b_oob_hdr_t *live = (fw == (n00b_inline_hdr_t *)oob)
                                     ? n00b_md_get(ctx->to_space->vtable.metadata,
                                                   oob->user_ptr)
                                     : (n00b_oob_hdr_t *)fw;
            if (live != nullptr && live->finalizer != nullptr) {
                n00b_trace_finalizer_data(ctx, &live->finalizer_user);
                reached++;
            }
            continue;
        }

        if (queue_dead) {
            n00b_finalizer_t fn   = oob->finalizer;
            void            *user = oob->finalizer_user;
            oob->finalizer        = nullptr;
            oob->finalizer_user   = nullptr;
            n00b_pending_finalizer_push(rt, fn, user);
            uint32_t at = n00b_atomic_load(&rt->pending_finalizers_len) - 1;
            n00b_trace_finalizer_data(ctx, &rt->pending_finalizers[at].user_ptr);
        }
    }

    n00b_process_worklist(ctx);
    return reached;
}

typedef struct {
    void                  *key;
    n00b_finalizer_info_t *info;
} n00b_finalizer_rekey_t;

// The index counterpart of n00b_finalizers_oob_pass. Entries whose object lies
// outside the from-space are live as far as this collection knows, so their
// data is traced too. With `queue_dead`, unreached from-space entries are
// removed and resurrected, and entries whose object moved are re-keyed.
static uint64_t
n00b_finalizers_index_pass(n00b_collect_t *ctx, bool queue_dead)
{
    n00b_runtime_t *rt      = n00b_get_runtime();
    uint64_t        reached = 0;

    if (rt->finalizers == nullptr || n00b_atomic_load(&rt->finalizer_count) == 0) {
        return 0;
    }

    __n00b_internal_type_erased_store_t *store
        = (__n00b_internal_type_erased_store_t *)n00b_atomic_load(&rt->finalizers->store);
    if (store == nullptr) {
        return 0;
    }
    uint32_t slots = store->last_slot + 1;

    n00b_allocator_t       *wa     = (n00b_allocator_t *)&ctx->work_pool;
    n00b_finalizer_rekey_t *rekeys = nullptr;
    uint32_t                nrekey = 0;
    if (queue_dead) {
        rekeys = n00b_alloc_array_with_opts(n00b_finalizer_rekey_t,
                                            n00b_atomic_load(&rt->finalizer_count),
                                            &(n00b_alloc_opts_t){.allocator = wa});
    }

    for (uint32_t bi = 0; bi < slots; bi++) {
        n00b_dict_bucket_t *b = &store->buckets[bi];
        if (b->hv == (n00b_uint128_t)0
            || (n00b_atomic_load(&b->flags) & N00B_HT_FLAG_DELETED)) {
            continue;
        }
        n00b_finalizer_info_t *info = (n00b_finalizer_info_t *)store->values[bi];
        if (info == nullptr) {
            continue;
        }

        if (n00b_from_segment_for(ctx, info->key) == nullptr) {
            n00b_trace_finalizer_data(ctx, &info->user_ptr);
            continue;
        }

        n00b_alloc_info_t ainfo = n00b_find_alloc_info(info->key);
        if (!n00b_alloc_info_is_heap(ainfo)) {
            continue;
        }
        n00b_inline_hdr_t *hdr   = alloc_info_raw_hdr(ainfo);
        bool               found = false;
        n00b_inline_hdr_t *fw    = n00b_gc_ptr_table_get(&ctx->visited, hdr, &found);

        if (found) {
            n00b_trace_finalizer_data(ctx, &info->user_ptr);
            reached++;
            if (queue_dead && fw != nullptr && fw != hdr) {
                rekeys[nrekey++] = (n00b_finalizer_rekey_t){.key = info->key, .info = info};
            }
            continue;
        }

        if (queue_dead) {
            n00b_pending_finalizer_push(rt, info->funcptr, info->user_ptr);
            uint32_t at = n00b_atomic_load(&rt->pending_finalizers_len) - 1;
            n00b_trace_finalizer_data(ctx, &rt->pending_finalizers[at].user_ptr);
            info->funcptr = nullptr;
            rekeys[nrekey++] = (n00b_finalizer_rekey_t){.key = info->key, .info = info};
        }
    }

    n00b_process_worklist(ctx);

    // The dict is not mutated while its store is being walked.
    for (uint32_t i = 0; i < nrekey; i++) {
        n00b_finalizer_info_t *info = rekeys[i].info;
        void                  *key  = rekeys[i].key;

        (void)_n00b_dict_internal_remove(rt->finalizers, N00B_MD_KSZ, N00B_MD_VSZ, &key);

        if (info->funcptr == nullptr) {
            atomic_fetch_sub_explicit(&rt->finalizer_count, 1, memory_order_acq_rel);
            n00b_free(info, .allocator = (n00b_allocator_t *)&rt->system_pool);
            continue;
        }

        n00b_alloc_info_t  ainfo = n00b_find_alloc_info(key);
        n00b_inline_hdr_t *hdr   = alloc_info_raw_hdr(ainfo);
        bool               found = false;
        n00b_inline_hdr_t *fw    = n00b_gc_ptr_table_get(&ctx->visited, hdr, &found);
        void              *moved = (char *)fw + ((char *)key - (char *)hdr);

        info->key = moved;
        (void)_n00b_dict_internal_put(rt->finalizers, N00B_MD_KSZ, N00B_MD_VSZ, &moved, &info);
    }

    return reached;
}

static void
n00b_process_finalizers(n00b_collect_t *ctx)
{
    bool     oob  = ctx->from_space->vtable.metadata_pool != nullptr
              && n00b_atomic_load(&ctx->from_space->vtable.oob_finalizers);
    uint64_t prev = UINT64_MAX;
    uint64_t now  = 0;

    // Tracing one finalizer's data can reach another finalizable object, so
    // repeat until the reached set stops growing before calling anything dead.
    while (now != prev) {
        prev = now;
        now  = n00b_finalizers_index_pass(ctx, false);
        if (oob) {
            now += n00b_finalizers_oob_pass(ctx, false);
        }
    }

    (void)n00b_finalizers_index_pass(ctx, true);
    uint64_t oob_live = oob ? n00b_finalizers_oob_pass(ctx, true) : 0;

    // A resurrected object can reach a finalizable one the dead pass had not
    // looked at yet; trace that one's data as well.
    prev = UINT64_MAX;
    now  = 0;
    while (now != prev) {
        prev = now;
        now  = n00b_finalizers_index_pass(ctx, false);
        if (oob) {
            oob_live = n00b_finalizers_oob_pass(ctx, false);
            now += oob_live;
        }
    }

    if (oob && oob_live == 0) {
        n00b_atomic_store(&ctx->from_space->vtable.oob_finalizers, false);
    }
}

void
n00b_gc_run_finalizers(void)
{
    if (!n00b_default_runtime_is_set()) {
        return;
    }
    n00b_runtime_t *rt = n00b_get_runtime();
    if (rt == nullptr || n00b_atomic_load(&rt->pending_finalizers_len) == 0
        || n00b_atomic_load(&rt->stw_active)) {
        return;
    }

    bool expected = false;
    if (!n00b_atomic_cas(&rt->finalizer_drain_active, &expected, true)) {
        return;
    }

    while (true) {
        n00b_finalizer_t fn   = nullptr;
        void            *user = nullptr;

        // The gate keeps a collection from growing or rewriting the queue
        // while an entry is taken off it.
        n00b_rw_read_lock(&rt->critical_execution);
        uint32_t len = n00b_atomic_load(&rt->pending_finalizers_len);
        if (len != 0) {
            n00b_finalizer_info_t *e = &rt->pending_finalizers[len - 1];
            fn                       = e->funcptr;
            user                     = e->user_ptr;
            *e                       = (n00b_finalizer_info_t){};
            n00b_atomic_store(&rt->pending_finalizers_len, len - 1);
        }
        n00b_rw_unlock(&rt->critical_execution);

        if (fn == nullptr) {
            break;
        }
        fn(user);
    }

    n00b_atomic_store(&rt->finalizer_drain_active, false);
}

// ============================================================================
// Collection setup
// ============================================================================

// n00b#309 / #368. Copying is only sound when every scanned word that
// resolves into from-space really is a pointer. Without a link-time GC type
// map every DEFAULT scan is conservative, so a consumer linked without the
// gcmap wrapper (wax) has ambiguous words throughout its heap and the forward
// step rewrites any of them that alias a live from-space address. In that
// configuration pin everything reached: page-granular mark-sweep, dead-only
// pages returned, live pages retained in place. With a type map present the
// copying collector stays on (typed objects scan precisely; #368 tracks the
// remaining conservative words). N00B_GC_PIN_ALL=0|1 overrides either way.
// Decided once, at the first collection, so a process never mixes modes.
bool
n00b_gc_pin_all_policy(void)
{
    static _Atomic int decided = -1; // -1 undecided, 0 copy, 1 pin all
    int                v       = n00b_atomic_load(&decided);
    if (v >= 0) {
        return v == 1;
    }
    const char *env = getenv("N00B_GC_PIN_ALL");
    if (env != nullptr && (env[0] == '0' || env[0] == '1') && env[1] == '\0') {
        v = env[0] - '0';
    }
    else {
        v = n00b_gc_type_map_available() ? 0 : 1;
    }
    int expected = -1;
    if (!n00b_atomic_cas(&decided, &expected, v)) {
        v = expected;
    }
    return v == 1;
}

static void
n00b_collect_setup(n00b_collect_t *ctx, n00b_arena_t *from_space, bool out_of_memory)
{
    ctx->from_space = from_space;
    // The to-space is sized from the arena's allocation count.
    n00b_gc_settle_tlabs(from_space, false);
    ctx->to_space   = n00b_create_destination_arena(from_space, out_of_memory);
    ctx->pin_all    = n00b_gc_pin_all_policy();
    // The context is an uninitialized stack struct; every field is assigned
    // here by hand.
    ctx->to_space_max_alloc_len = 0;
    ctx->segment_lookups        = 0;
    ctx->segment_lookup_steps   = 0;

    /* Bump the runtime's GC epoch counter and snapshot it onto the
     * collection context. The mark phase stamps this value onto
     * every metadata-bearing alloc it reaches (via the OOB record's
     * gc_epoch field). After mark, alloc records still flagged
     * `alive` whose epoch is stale = handed out + not reached =
     * leaks; the metadata-pool sweep returns them to their pool. */
    {
        n00b_runtime_t *rt = n00b_get_runtime();
        if (rt) {
            ctx->current_epoch = n00b_atomic_add(&rt->gc_current_epoch, 1) + 1;
        }
        else {
            ctx->current_epoch = 0;
        }
    }

    // clang-format off
    // The work pool is single-threaded: only the collecting thread touches it,
    // and only while the world is stopped. Epoch reclamation buys nothing here
    // and is actively harmful — the memo dict's migrations would epoch-retire
    // old stores onto the thread's retire list, but the pool is destroyed under
    // STW (so pool_pre_destroy skips the epoch drain to avoid a quiescence
    // deadlock), leaving those nodes dangling once the pool is unmapped. With
    // epochs off, migrations free old stores immediately back to the pool.
    n00b_pool_init(&ctx->work_pool,
                   .__system   = true,
                   .hidden     = true,
                   .use_epochs = false,
                   .name       = "gc_worklist");

    n00b_allocator_t *wa = (n00b_allocator_t *)&ctx->work_pool;

    ctx->worklist = n00b_list_new_cap(n00b_gc_wl_item_t *,
                                      N00B_GC_WL_START_SIZE, .allocator = wa);

    // clang-format on
    n00b_gc_ptr_table_init(&ctx->visited, wa, 8);
    n00b_gc_ptr_table_init(&ctx->guard_memo.pages, wa, 6);
    ctx->guard_memo.last_page  = 0;
    ctx->guard_memo.last_lines = nullptr;

    // If from-space uses OOB metadata, back the forwarding dict with TO-space's
    // OWN metadata arena (attached by n00b_new_arena above). Every moved
    // allocation gets its new OOB record allocated into this arena, so at GC end
    // the whole from-space metadata arena (old dict + now-dead records) is torn
    // down wholesale and to-space's arena is simply moved onto the recycled live
    // arena -- no survivor-by-survivor rebuild.
    //
    // The dict is pre-sized to this collection's live set (alloc_count * 2) so
    // it never migrates mid-collect: no epoch-retired store nodes accumulate in
    // to-space's pool, so when that pool is adopted onto the live arena it is
    // clean. (n00b_new_arena already created a default dict in this same pool;
    // it is unused and torn down with the pool.)
    if (from_space->vtable.metadata_pool) {
        n00b_allocator_t *md_pool = ctx->to_space->vtable.metadata_pool;

        _n00b_dict_internal_t *new_md
            = n00b_alloc_with_opts(_n00b_dict_internal_t,
                                   &(n00b_alloc_opts_t){.allocator = md_pool});
        _n00b_dict_internal_init(new_md,
                                 N00B_MD_KSZ,
                                 N00B_MD_VSZ,
                                 typehash(void *),
                                 typehash(n00b_oob_hdr_t *),
                                 .start_capacity = from_space->alloc_count * 2,
                                 .allocator      = md_pool,
                                 .hash           = n00b_hash_word,
                                 .skip_obj_hash  = true,
                                 .copy_values    = true,
                                 .scan_kind      = N00B_GC_SCAN_KIND_NONE);
        ctx->to_space->vtable.metadata = new_md;
    }

    assert(ctx->to_space->segment_end
           == ctx->to_space->current_segment->data + ctx->to_space->current_segment->size);
    assert(ctx->to_space && ctx->to_space != ctx->from_space);

    // Per-from-space-segment page-pin bitmaps for the ambiguous-root pin
    // pre-pass.  Allocated here (from_space segment chain is final) and freed in
    // n00b_collection_cleanup.
    n00b_pin_bitmaps_alloc(ctx);
}

// ============================================================================
// Page-granular reclaim of the from-space (mostly-copying GC)
// ============================================================================

// Walk each from-space segment's page-pin bitmap: return every UNPINNED page run
// to the kernel (raw munmap), and RETAIN every pinned run in place — re-register
// it as a managed segment and wrap it in a fresh `retained` descriptor chained
// into the live arena AFTER its bump segment, so the run is scanned and
// address-resolvable but never allocated from.  Lock-chain scrubbing is done per
// unpinned run only (pinned runs keep their still-valid embedded locks).  The
// from descriptor + its bitmap are freed here, so to_space owns no data segments
// at destroy time.  A retained run that a later collect no longer pins gets its
// pages returned + descriptor freed then — pins are not permanent.
static void
n00b_reclaim_pinned_pages(n00b_collect_t *ctx, n00b_segment_t *from_chain)
{
    extern void       n00b_lock_chains_scrub_range(uint64_t lo, uint64_t hi);
    n00b_allocator_t *sp         = (n00b_allocator_t *)&n00b_get_runtime()->system_pool;
    n00b_allocator_t *scratch    = (n00b_allocator_t *)&ctx->work_pool;
    n00b_arena_t     *live       = ctx->from_space; // keeps identity post-swap
    bool              unregister = !live->vtable.hidden;
    uint64_t          pg         = (uint64_t)n00b_page_size;
    n00b_segment_t   *seg        = from_chain;

    // Pin accounting for this collect: how much of the from-space we FREED
    // (unpinned runs munmapped) vs RETAINED (pinned runs linked back into the
    // live arena as `keep` descriptors). High retained == the from-space is
    // effectively abandoned-in-place rather than freed.
    uint64_t pinned_pages = 0, freed_pages = 0, retained_runs = 0, nobitmap_segs = 0;

    while (seg) {
        n00b_segment_t *next   = seg->next_segment;
        char           *data   = seg->data;
        uint64_t        size   = seg->size;
        uint64_t        npages = (size + pg - 1) / pg;
        uint8_t        *bm     = seg->pin_bitmap;

        // Drop the whole-segment registry record; pinned runs re-register below.
        if (unregister) {
            n00b_mmap_unregister(data);
        }

        if (!bm) {
            // Defensive: a from segment with no bitmap — free it wholesale.
            n00b_lock_chains_scrub_range((uintptr_t)data, (uintptr_t)data + size);
            n00b_safe_munmap(data, size);
            sp->free(sp, seg);
            freed_pages += npages;
            nobitmap_segs++;
            seg = next;
            continue;
        }

        uint64_t p = 0;
        while (p < npages) {
            bool     pinned    = (bm[p >> 3] >> (p & 7)) & 1;
            uint64_t run_start = p;
            while (p < npages) {
                bool b = (bm[p >> 3] >> (p & 7)) & 1;
                if (b != pinned) {
                    break;
                }
                p++;
            }
            char    *run_addr = data + run_start * pg;
            uint64_t run_len  = (p - run_start) * pg;
            // Clamp the final run to the mapped size (size need not be a whole
            // number of pages).
            if ((uint64_t)(run_addr - data) + run_len > size) {
                run_len = size - (uint64_t)(run_addr - data);
            }
            if (run_len == 0) {
                continue;
            }

            if (!pinned) {
                freed_pages += run_len / pg;
                n00b_lock_chains_scrub_range((uintptr_t)run_addr,
                                             (uintptr_t)run_addr + run_len);
#if defined(N00B_GC_POISON_RECLAIM)
                // Debug: POISON instead of unmap — keep the page mapped but
                // PROT_NONE so it can't be reused and any stale access faults at
                // the exact dereference.  Leaks pages; only for the crash repro.
                mprotect(run_addr, run_len, PROT_NONE);
#else
                n00b_safe_munmap(run_addr, run_len);
#endif
            }
            else {
                pinned_pages += run_len / pg;
                retained_runs++;
                n00b_mmap_info_t *run_rec = nullptr;
                if (unregister) {
                    run_rec = n00b_register_arena_segment(run_addr,
                                                          run_addr + run_len,
                                                          live);
                }
                n00b_segment_t *keep
                    = n00b_alloc_with_opts(n00b_segment_t,
                                           &(n00b_alloc_opts_t){.allocator = sp});
                /* A retained run gets a FRESH registry record, and the objects
                 * inside it were recorded against the from-space record that
                 * was just dropped.  Seed it with the largest footprint pinned
                 * in this segment: nothing lives in a retained run except what
                 * was pinned there (unpinned pages are unmapped above), and
                 * every pin recorded its footprint, so this can never be
                 * smaller than a live allocation in the run -- while staying
                 * far below the global all-time high-water mark a since-dead
                 * large object left behind (n00b#395).  Under the pin-all
                 * policy (a binary with no GC type map, n00b#309) EVERY
                 * survivor lives in a run like this, so without it the
                 * per-mapping bound would never apply to that heap at all.
                 * Nothing is ever bump-allocated into a retained run, so the
                 * value cannot go stale. */
                keep->mmap_rec                      = run_rec;
                keep->pin_max_alloc_len             = 0;
                n00b_mmap_note_alloc_len(run_rec, seg->pin_max_alloc_len);
                keep->size                          = run_len;
                keep->data                          = run_addr;
                keep->last_addr                     = run_addr + run_len;
                keep->retained                      = true;
                keep->pin_bitmap                    = nullptr;
                keep->next_segment                  = live->current_segment->next_segment;
                live->current_segment->next_segment = keep;
            }
        }

        scratch->free(scratch, bm);
        seg->pin_bitmap = nullptr;
        sp->free(sp, seg);
        seg = next;
    }

    n00b_atomic_store(&n00b_gc_last_pinned_pages, pinned_pages);
    n00b_atomic_store(&n00b_gc_last_freed_pages, freed_pages);
    n00b_atomic_store(&n00b_gc_last_retained_runs, retained_runs);
    n00b_atomic_store(&n00b_gc_last_nobitmap_segs, nobitmap_segs);
    n00b_atomic_store(&n00b_gc_last_segment_lookups, ctx->segment_lookups);
    n00b_atomic_store(&n00b_gc_last_segment_lookup_steps, ctx->segment_lookup_steps);
    n00b_atomic_add(&n00b_gc_total_pinned_pages, pinned_pages);
    n00b_atomic_add(&n00b_gc_total_freed_pages, freed_pages);
    n00b_atomic_add(&n00b_gc_collect_count, 1);
}

// ============================================================================
// Collection cleanup — swap segments and destroy temporaries
// ============================================================================

// Every thread's allocation buffer for the default arena: add the objects it
// handed out since the last settle to the arena's count and, when `retire`,
// drop the buffer, whose space is in the from-space this collection reclaims.
// Exited threads settled their own count in n00b_thread_destroy.
static void
n00b_gc_settle_tlabs(n00b_arena_t *arena, bool retire)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    if (arena != rt->default_arena) {
        return;
    }
    for (uint32_t i = 0; i < rt->max_threads; i++) {
        n00b_thread_t *t = (n00b_thread_t *)n00b_atomic_load(&rt->threads[i].thread);
        if (n00b_thread_slot_is_vacant(t)) {
            continue;
        }
        uint64_t unseen = t->tlab_allocs - t->tlab_allocs_seen;
        if (unseen != 0) {
            n00b_atomic_add(&arena->alloc_count, (uint32_t)unseen);
            t->tlab_allocs_seen = t->tlab_allocs;
        }
        if (retire) {
            atomic_store_explicit(&t->tlab_end, nullptr, memory_order_relaxed);
            atomic_store_explicit(&t->tlab_cur, nullptr, memory_order_relaxed);
            t->tlab_noted_len = 0;
        }
    }
}

#if defined(N00B_DEBUG)
// Every allocation from an arena with OOB metadata inserts one record, so the
// records inserted since alloc_count was last zeroed must match it. A thread
// stopped between counting an allocation and inserting its record skews the
// two by one, in either direction across a collection, so each live thread
// allows one.
static void
n00b_gc_check_alloc_count(n00b_arena_t *arena)
{
    _n00b_dict_internal_t *md = arena->vtable.metadata;
    if (md == nullptr || arena->md_inserts_unknown) {
        return;
    }

    n00b_runtime_t *rt      = n00b_get_runtime();
    int64_t         threads = 0;
    for (uint32_t i = 0; i < rt->max_threads; i++) {
        n00b_thread_t *t = (n00b_thread_t *)n00b_atomic_load(&rt->threads[i].thread);
        if (!n00b_thread_slot_is_vacant(t)) {
            threads++;
        }
    }

    uint32_t inserted = n00b_atomic_load(&md->insertion_epoch) - arena->md_inserts_at_zero;
    uint32_t counted  = n00b_atomic_load(&arena->alloc_count);
    int64_t  skew     = (int64_t)counted - (int64_t)inserted;

    if (skew > threads || skew < -threads) {
        fprintf(stderr,
                "n00b: arena %s counted %u allocations since its last collection "
                "but inserted %u metadata records (%lld threads)\n",
                arena->vtable.debug_name ? arena->vtable.debug_name : "?",
                counted,
                inserted,
                (long long)threads);
        n00b_require(false, "alloc_count disagrees with the arena's metadata records");
    }
}
#endif

static void
n00b_collection_cleanup(n00b_collect_t *ctx)
{
    // Swap the segment linked list so the from-space arena keeps its
    // identity but now holds the live (to-space) data.

    n00b_segment_t *new_segment  = ctx->to_space->current_segment;
    void           *old_segments = (void *)ctx->from_space->current_segment;

    ctx->from_space->current_segment = new_segment;
    ctx->from_space->next_alloc      = ctx->to_space->next_alloc;
    ctx->from_space->segment_end     = ctx->to_space->segment_end;
    n00b_gc_settle_tlabs(ctx->from_space, true);

    n00b_gc_shrink_primary_segment(ctx->from_space);

    // Post-collect occupancy gate for the growth heuristic in
    // n00b_create_destination_arena.  The arena now holds the compacted live
    // set, so measure how full it is: capacity across all (just-swapped-in)
    // segments minus the free tail of the current segment.  If the live set
    // occupies more than 50% of capacity we have less than 2x headroom, so the
    // next out-of-memory collect must double the to-space to avoid refilling
    // immediately and thrashing into a full-heap collect on every allocation.
    // Recomputed every collect, so it never latches stale.  (Was 25%/4x; at
    // that threshold a small live set sitting in a transient-churn-bloated
    // arena still armed the doubling, ratcheting capacity far above live.)
    uint64_t cap = n00b_arena_size(ctx->from_space);
    uint64_t free_bytes
        = (uint64_t)(ctx->from_space->segment_end - (char *)ctx->from_space->next_alloc);
    uint64_t live         = cap > free_bytes ? cap - free_bytes : 0;
    // live > cap/2 is the overflow-safe form of (live * 2 > cap).
    ctx->from_space->grow = (cap != 0) && (live > cap / 2);

    // Metadata handoff. The collector already created a fresh OOB metadata
    // entry in to-space's attached metadata arena for every object it copied,
    // so to-space's metadata dict IS the exact live set — nothing to rebuild.
    // Adopt to-space's metadata dict AND its metadata arena wholesale onto the
    // recycled from-space arena, then drop from-space's old metadata arena in
    // one shot. (The previous code re-inserted every survivor record into a
    // brand-new pool via n00b_allocator_compact_metadata — pure per-record
    // churn every collection, and it retired metadata-dict stores mid-collect,
    // which is what dangled the epoch retire list.)
    if (ctx->from_space->vtable.metadata_pool) {
        n00b_allocator_t *dead_md_pool = ctx->from_space->vtable.metadata_pool;

        ctx->from_space->vtable.metadata      = ctx->to_space->vtable.metadata;
        ctx->from_space->vtable.metadata_pool = ctx->to_space->vtable.metadata_pool;
        ctx->from_space->md_inserts_at_zero
            = n00b_atomic_load(&ctx->from_space->vtable.metadata->insertion_epoch);
        ctx->from_space->md_inserts_unknown = false;
        // to-space no longer owns them, so its destroy below leaves them alone.
        ctx->to_space->vtable.metadata      = nullptr;
        ctx->to_space->vtable.metadata_pool = nullptr;

        // Drop the from-space metadata arena wholesale. First drain its retired
        // epoch store nodes out of every thread's retire list ourselves:
        // pool_pre_destroy skips that drain under STW (its quiescence wait would
        // deadlock against the suspended threads), so without this the pool's
        // nodes would dangle on other threads' retire lists once its pages are
        // unmapped. Under STW there is no contention, so the drain is safe.
        n00b_epoch_drain_allocator_stw(dead_md_pool);
        n00b_allocator_destroy(dead_md_pool);
    }

    ctx->to_space->vtable.hidden = false;

    new_segment->mmap_rec
        = n00b_register_arena_segment(new_segment->data,
                                      ctx->from_space->segment_end,
                                      ctx->from_space,
                                      .file = ctx->from_space->vtable.debug_name);
    n00b_mmap_note_alloc_len(new_segment->mmap_rec,
                             ctx->to_space_max_alloc_len);

    // Page-granular reclaim of the from-space: return unpinned page runs to the
    // kernel and retain pinned runs in place (chained into the live arena as
    // non-allocatable segments).  Lock-chain scrubbing happens per unpinned run
    // INSIDE the reclaim — pinned runs stay mapped and keep their valid locks.
    // This frees the from descriptors + bitmaps, so to_space owns no data
    // segments at destroy time (current_segment nulled below).
    n00b_reclaim_pinned_pages(ctx, (n00b_segment_t *)old_segments);
    ctx->to_space->current_segment = nullptr;

    n00b_allocator_destroy((n00b_allocator_t *)&ctx->work_pool);
    n00b_allocator_destroy((n00b_allocator_t *)ctx->to_space);

    // Refresh the arena/pool audit snapshot while the world is STILL stopped for
    // this collection (we are between n00b_stop_the_world and restart). Walking
    // the audit ring + segment chains is only safe with all other threads frozen,
    // and the collection already froze them — so the audit costs no extra STW
    // (this replaces the status-path STW census that livelocked). It runs after
    // the to_space + work_pool destroys above, so the snapshot reflects the live
    // post-collection arena set, including the out-of-registry "phantom" arenas
    // (md_pool metadata, scratch, collection spaces) that otherwise read as
    // unattributed. No-op unless the audit is compiled in.
    n00b_arena_audit_census_nolock();

    gc_audit_segment_chain(ctx->from_space, "after reclaim");

    n00b_atomic_fence();
}

// ============================================================================
// Entry point
// ============================================================================

// Opt-in segment-chain audit (env N00B_GC_AUDIT_SEGMENT_CHAIN).
//
// Every crash signature on n00b#431 is a chain walk that dereferenced a
// descriptor whose system_pool slot had been recycled, or a reclaim that took
// `data`/`size` from one -- so the chain is the thing worth checking, and it
// is only checkable here, with the world stopped.
//
// Env-gated rather than compiled out, so it can be turned on in a shipped
// binary on a box that reproduces (the audit is O(segments) plus one registry
// lookup each, which is real cost on a pin-all heap carrying 100k+ retained
// runs -- far too much to leave on).
//
// Aborts with the offending descriptor named, which turns an access violation
// inside n00b_arena_size into a diagnosis.
static bool
gc_audit_segment_chain_enabled(void)
{
    static _Atomic int enabled;
    int                cached = n00b_atomic_load(&enabled);

    if (cached != 0) {
        return cached == 2;
    }

    bool on = getenv("N00B_GC_AUDIT_SEGMENT_CHAIN") != nullptr;
    n00b_atomic_store(&enabled, on ? 2 : 1);
    return on;
}

static void
gc_audit_segment_chain(n00b_arena_t *arena, const char *when)
{
    if (!gc_audit_segment_chain_enabled()) {
        return;
    }

    n00b_segment_t *seg = arena->current_segment;
    uint64_t        ix  = 0;

    while (seg != nullptr) {
        const char *bad = nullptr;

        if (seg->data == nullptr || seg->size == 0) {
            bad = "null data or zero size";
        }
        else if (((uintptr_t)seg->data & ((uintptr_t)n00b_page_size - 1)) != 0) {
            bad = "data is not page aligned";
        }
        else if (seg->last_addr < seg->data
                 || seg->last_addr > seg->data + seg->size) {
            bad = "last_addr outside the segment";
        }
        else if (!arena->vtable.hidden) {
            auto opt = n00b_mmap_by_address(seg->data);
            if (!n00b_option_is_set(opt)) {
                bad = "data is not a registered mapping";
            }
            else {
                n00b_mmap_info_t *map = n00b_option_get(opt);
                if (map->allocator != (n00b_allocator_t *)arena) {
                    bad = "mapping belongs to a different allocator";
                }
                else if ((uint64_t)(uintptr_t)seg->data < map->start
                         || (uint64_t)(uintptr_t)(seg->data + seg->size)
                                > map->end) {
                    bad = "segment extends past its mapping";
                }
            }
        }

        if (bad != nullptr) {
            fprintf(stderr,
                    "n00b: FATAL: GC segment-chain audit failed %s on arena "
                    "'%s': segment #%llu at %p is %s (data=%p size=%llu "
                    "last_addr=%p retained=%d next=%p). This is the n00b#431 "
                    "shape: a descriptor on the live chain that the collector "
                    "no longer owns.\n",
                    when,
                    arena->vtable.debug_name ? arena->vtable.debug_name
                                             : "(unnamed)",
                    (unsigned long long)ix,
                    (void *)seg,
                    bad,
                    (void *)seg->data,
                    (unsigned long long)seg->size,
                    (void *)seg->last_addr,
                    (int)seg->retained,
                    (void *)seg->next_segment);
            fflush(stderr);
            abort();
        }

        seg = seg->next_segment;
        if (++ix > (1u << 24)) {
            fprintf(stderr,
                    "n00b: FATAL: GC segment-chain audit found a cycle (or an "
                    "absurdly long chain) %s on arena '%s'.\n",
                    when,
                    arena->vtable.debug_name ? arena->vtable.debug_name
                                             : "(unnamed)");
            fflush(stderr);
            abort();
        }
    }
}

// We do not want the compiler to inline this, otherwise it will quite
// likely blend the stack frame in a way we don't like w/
// n00b_collect().
static __attribute__((noinline)) void
n00b_collect_internal(n00b_arena_t *arena, bool out_of_memory)
{
    n00b_collect_t  ctx;
    n00b_segment_t *segment = arena->current_segment;

    gc_audit_segment_chain(arena, "at collect entry");
#if defined(N00B_CENSUS_ENABLED)
    n00b_debug_census_t *timing_census = g_debug_census;
    uint64_t internal_start_ns         = timing_census == nullptr ? 0 : n00b_gc_timestamp_ns();
    uint64_t phase_start_ns            = internal_start_ns;
#else
    [[maybe_unused]] uint64_t phase_start_ns = 0;
#endif

    /* n00b_diag_foreign_self_check(); */ /* dormant evidence pass (thread.c) */

    segment->last_addr = n00b_atomic_load(&arena->next_alloc);

    n00b_collect_setup(&ctx, arena, out_of_memory);
#if defined(N00B_DEBUG)
    n00b_gc_check_alloc_count(arena);
#endif
    arena->alloc_count = 0;
#if defined(N00B_CENSUS_ENABLED)
    if (timing_census != nullptr) {
        timing_census->gc_out_of_memory = out_of_memory;
    }

    /* Diagnostic site census: only during a debug_leak_detect collect.
     * Populated as live OOB allocs are visited. Reporting is deferred until
     * after n00b_collect() restarts the world. */
    g_site_census = nullptr;
    {
        n00b_runtime_t      *crt    = n00b_get_runtime();
        n00b_debug_census_t *census = g_debug_census;

        if (crt && census != nullptr && n00b_atomic_load(&crt->debug_leak_detect)) {
            census->site_live_count
                = n00b_dict_new_private(uint64_t, int64_t, .allocator = census->allocator);
            g_site_census = census->site_live_count;
        }
    }
#else
    (void)out_of_memory;
#endif
    n00b_debug_census_finish_phase(timing_census == nullptr ? nullptr
                                                            : &timing_census->gc_setup_ns,
                                   &phase_start_ns);

    // Build the per-collect conservative-scan tree (gc-scannable arena/pool
    // segments) before any scan runs; the conservative scan queries it per word
    // instead of the global mmap tree. Allocated from work_pool (freed at
    // cleanup). The world is stopped, so the audit ring + segment chains are
    // stable.
    n00b_build_scan_tree(&ctx);

    // Ambiguous-root pin pre-pass: mark from-space pages implicated by suspended
    // threads' captured registers BEFORE any forwarding can move them.
    n00b_pin_prepass(&ctx);

    n00b_scan_roots(&ctx);
    n00b_debug_census_finish_phase(timing_census == nullptr ? nullptr
                                                            : &timing_census->gc_roots_ns,
                                   &phase_start_ns);

    n00b_scan_runtime(&ctx);
    n00b_debug_census_finish_phase(
        timing_census == nullptr ? nullptr : &timing_census->gc_runtime_scan_ns,
        &phase_start_ns);

    n00b_process_worklist(&ctx);
    n00b_debug_census_finish_phase(
        timing_census == nullptr ? nullptr : &timing_census->gc_worklist_roots_ns,
        &phase_start_ns);

    n00b_scan_thread_stacks(&ctx);
    n00b_debug_census_finish_phase(timing_census == nullptr ? nullptr
                                                            : &timing_census->gc_thread_scan_ns,
                                   &phase_start_ns);

    n00b_process_worklist(&ctx);
    n00b_debug_census_finish_phase(
        timing_census == nullptr ? nullptr : &timing_census->gc_worklist_threads_ns,
        &phase_start_ns);

    /* Pool walk as roots: every metadata-bearing pool gives up
     * each of its alive allocs as a root for this mark pass.  This
     * replaces "register the pool's owning struct as a root"
     * callers and is more precise — only the actually-live slots
     * get scanned. The visit path stamps gc_epoch on each alloc
     * reached.
     *
     * In leak-detect mode the runtime caller skips this step: the
     * point of leak detection is precisely to test reachability
     * from the **real** roots only, so that allocations whose only
     * inbound pointer is "they live in a pool" still get classified
     * as leaks. */
    {
        n00b_runtime_t *rt = n00b_get_runtime();
        if (!rt || !n00b_atomic_load(&rt->debug_leak_detect)) {
            n00b_scan_metadata_pools(&ctx);
            n00b_debug_census_finish_phase(
                timing_census == nullptr ? nullptr : &timing_census->gc_metadata_scan_ns,
                &phase_start_ns);
            n00b_process_worklist(&ctx);
            n00b_debug_census_finish_phase(
                timing_census == nullptr ? nullptr : &timing_census->gc_metadata_worklist_ns,
                &phase_start_ns);
        }
        else {
            n00b_debug_census_finish_phase(
                timing_census == nullptr ? nullptr : &timing_census->gc_metadata_scan_ns,
                &phase_start_ns);
        }
    }

    assert(!n00b_list_len(ctx.worklist));

    // Finalizable objects the mark did not reach are resurrected here, so the
    // census and the pool sweep below see them as live.
    n00b_process_finalizers(&ctx);
    n00b_debug_census_finish_phase(timing_census == nullptr ? nullptr
                                                            : &timing_census->gc_finalizers_ns,
                                   &phase_start_ns);
    assert(!n00b_list_len(ctx.worklist));

    /* Pool census: must run HERE — after the mark (so gc_epoch
     * distinguishes reachable=current from leaked=stale) but BEFORE the
     * sweep below, which stamps leaked allocs with the current epoch and
     * would erase that distinction.  Reports per-site LIVE-vs-LEAKED so a
     * retained-reference leak (reachable but should-be-dropped) is visible
     * as an outsized LIVE site.  Leak-detect collects only. */
#if defined(N00B_CENSUS_ENABLED)
    {
        n00b_runtime_t *crt = n00b_get_runtime();
        if (crt && n00b_atomic_load(&crt->debug_leak_detect)) {
            n00b_debug_pool_census(ctx.current_epoch);
            // The to-space OOB dict is the live (forwarded) set after
            // mark; census it by origin site + validate OOB migration.
            n00b_debug_arena_census(&ctx);
        }
        else if (g_debug_census != nullptr) {
            // Natural-collection census (census_on_collect, no leak-detect):
            // only the default-arena by-site walk.  It reads the post-mark
            // gc_epoch stamps the normal mark already set (LIVE == current
            // epoch, RECLAIMED == stale), so it is correct on a plain collect
            // and does not depend on record-don't-reclaim sweep mode.  The
            // pool census is intentionally skipped here: it is only meaningful
            // under debug_leak_detect's record-don't-reclaim sweep.
            n00b_debug_arena_census(&ctx);
        }
    }
#endif
    n00b_debug_census_finish_phase(timing_census == nullptr ? nullptr
                                                            : &timing_census->gc_census_ns,
                                   &phase_start_ns);

    /* Sweep stale-epoch alive allocs back to their pools — that
     * set is the leak diagnostic the runtime exposes. */
    n00b_sweep_metadata_pool_leaks(&ctx);
    n00b_debug_census_finish_phase(timing_census == nullptr ? nullptr
                                                            : &timing_census->gc_pool_sweep_ns,
                                   &phase_start_ns);

    /* Dead foreign slots are quarantined during the STW stop pass so the stop
     * loop never retries a dead Mach port forever.  Now stw_active is set, so
     * n00b locks/data locks short-circuit while the collector performs the full
     * CV/lock/port cleanup for those records. */
    n00b_reap_dead_foreign_threads();
    n00b_debug_census_finish_phase(
        timing_census == nullptr ? nullptr : &timing_census->gc_foreign_reap_ns,
        &phase_start_ns);

#if defined(N00B_CENSUS_ENABLED)
    g_site_census = nullptr;
#endif

    n00b_collection_cleanup(&ctx);
    n00b_debug_census_finish_phase(timing_census == nullptr ? nullptr
                                                            : &timing_census->gc_cleanup_ns,
                                   &phase_start_ns);
#if defined(N00B_CENSUS_ENABLED)
    if (timing_census != nullptr) {
        timing_census->gc_internal_ns = n00b_gc_elapsed_ns(internal_start_ns, phase_start_ns);
    }
#endif
}

void
n00b_collect(n00b_arena_t *arena) _kargs
{
    // Set when this collection is triggered by the arena actually running out
    // of room (the n00b_arena_alloc pressure path).  It gates the to-space
    // growth heuristic in n00b_create_destination_arena: only a genuine
    // out-of-memory collect may pre-grow the to-space.  A manual / test /
    // marshal collect leaves it false so it never grows a low-traffic arena.
    bool out_of_memory = false;
}
{
    n00b_jmp_buf_t                     register_spill = {};
    [[maybe_unused]] volatile uint64_t top            = 0;
    volatile n00b_thread_t            *self           = n00b_thread_self();

    self->stack_top = (void *)&top;

#if defined(N00B_CENSUS_ENABLED)
    /* Natural-collection census: if armed (and no find_leaks session is
     * already running this collect), set up a transient census so the
     * default-arena by-site walk runs in-line during this collect.  We do
     * NOT toggle rt->debug_leak_detect: reclaim semantics stay exactly as a
     * normal collect; n00b_debug_arena_census() only reads the post-mark
     * gc_epoch stamps the normal mark already sets.  Published below, after
     * the world restarts.  Re-uses g_debug_census_active so it can never
     * collide with an explicit n00b_debug_find_leaks() session. */
    n00b_debug_census_t *natural_census            = nullptr;
    n00b_allocator_t    *natural_census_alloc      = nullptr;
    uint64_t             natural_census_started_ns = 0;
    {
        n00b_runtime_t *crt = n00b_get_runtime();
        if (crt != nullptr && n00b_atomic_load(&crt->census_on_collect)) {
            bool expected = false;
            if (n00b_atomic_cas(&g_debug_census_active, &expected, true)) {
                n00b_arena_t *census_arena = n00b_new_arena(.size   = (1 << 22),
                                                            .use_gc = false,
                                                            .no_map = true,
                                                            .name   = "debug_census");
                natural_census_alloc       = (n00b_allocator_t *)census_arena;
                natural_census             = n00b_alloc_with_opts(
                    n00b_debug_census_t,
                    &(n00b_alloc_opts_t){.allocator = natural_census_alloc});
                *natural_census = (n00b_debug_census_t){
                    .arena                = census_arena,
                    .allocator            = natural_census_alloc,
                    .leak_sample_capacity = N00B_DEBUG_CENSUS_LEAK_SAMPLE_MAX,
                };
                natural_census->leak_samples
                    = n00b_alloc_array_with_opts(
                        n00b_debug_leak_sample_t,
                        N00B_DEBUG_CENSUS_LEAK_SAMPLE_MAX,
                        &(n00b_alloc_opts_t){.allocator = natural_census_alloc});
                natural_census_started_ns = n00b_gc_timestamp_ns();
                g_debug_census            = natural_census;
            }
        }
    }
#endif

    // The collection MUST run with the world stopped.  n00b_scan_thread_stacks
    // conservatively walks every other thread's C stack and reads its
    // stack_map/stack_top; if a thread is concurrently in n00b_thread_destroy it
    // nulls its stack_map and unregisters/unmaps its stack out from under the
    // scan (observed: SIGSEGV in n00b_visit_possible_pointer mid-range, the page
    // unmapped between the stack_map null-check and the range read).  Stopping
    // the world here both freezes every other thread and — because STW first
    // acquires `critical_execution` — guarantees no thread is mid-destroy
    // (destroy holds that same gate across its WHOLE teardown).  STW is
    // reentrant via the gate + stw_nesting, so callers that already stopped the
    // world (arena auto-collect, n00b_debug_find_leaks, marshal) simply nest.
#if defined(N00B_CENSUS_ENABLED)
    uint64_t pause_start_ns = g_debug_census == nullptr ? 0 : n00b_gc_timestamp_ns();
#else
    [[maybe_unused]] uint64_t pause_start_ns = 0;
#endif
    n00b_stop_the_world();
    // Counted from here, after every other thread is suspended, so the pause
    // counters exclude the wait for the STW gate; a collect nested inside a
    // caller's own STW credits only its own share.
    uint64_t stw_pause_start_ns = n00b_gc_timestamp_ns();
    // World is stopped: reclaim every parked epoch retire-list node before the
    // collection tears down/compacts the metadata pool, so a still-listed node
    // can't have its pool pages freed out from under the list (see
    // n00b_epoch_flush_all_stw).
    n00b_epoch_flush_all_stw(n00b_get_runtime());
    // With every retire list now empty, tear down allocators whose destroy
    // was deferred (metadata pools — their inline destroy races lock-free
    // n00b_retire pushes; see n00b_allocator_destroy).
    n00b_allocator_run_deferred_destroys(n00b_get_runtime());
#if defined(N00B_CENSUS_ENABLED)
    uint64_t stop_done_ns = g_debug_census == nullptr ? 0 : n00b_gc_timestamp_ns();
#else
    [[maybe_unused]] uint64_t stop_done_ns = 0;
#endif
    if (!n00b_setjmp(&register_spill)) {
        n00b_collect_internal(arena, out_of_memory);
        n00b_longjmp(&register_spill, 1);
    }
    n00b_pool_page_cache_note_collection();
#if defined(N00B_CENSUS_ENABLED)
    uint64_t restart_start_ns = g_debug_census == nullptr ? 0 : n00b_gc_timestamp_ns();
#else
    [[maybe_unused]] uint64_t restart_start_ns = 0;
#endif
    n00b_restart_the_world();
    n00b_gc_record_pause(stw_pause_start_ns, n00b_gc_timestamp_ns());
#if defined(N00B_CENSUS_ENABLED)
    uint64_t pause_done_ns = g_debug_census == nullptr ? 0 : n00b_gc_timestamp_ns();
#else
    [[maybe_unused]] uint64_t pause_done_ns = 0;
#endif
    n00b_debug_census_record_pass_timing(pause_start_ns,
                                         stop_done_ns,
                                         restart_start_ns,
                                         pause_done_ns);

    // Unmap released big pages no request reused across the last collection
    // interval. When a caller holds its own stop-the-world around this
    // collect, the world is still stopped here; the cache lock is only ever
    // tried, so a suspended holder makes the trim skip, never wait.
    n00b_pool_page_cache_trim();

#if defined(N00B_CENSUS_ENABLED)
    /* Publish the natural-collection census now that the world is running
     * again — formatting + conduit IO must NOT happen under STW. */
    if (natural_census != nullptr) {
        g_debug_census       = nullptr;
        g_site_census        = nullptr;
        uint64_t finished_ns = n00b_gc_timestamp_ns();
        n00b_debug_census_store_stats(natural_census, natural_census_started_ns, finished_ns);
        n00b_runtime_t *crt = n00b_get_runtime();
        if (crt != nullptr && crt->stderr_topic != nullptr) {
            n00b_debug_census_publish(
                natural_census,
                (n00b_conduit_topic_t(n00b_buffer_t *) *)crt->stderr_topic,
                true); // in-collect (STW): write direct to fd, never the conduit
        }
        n00b_allocator_destroy(natural_census_alloc);
        n00b_atomic_store(&g_debug_census_active, false);
    }
#endif

    // A no-op while a caller's own stop-the-world is still in force.
    n00b_gc_run_finalizers();
}

#if defined(N00B_CENSUS_ENABLED)
/* Diagnostic POOL census: enumerate every ALIVE allocation physically
 * resident in each metadata-bearing pool, bucketed by allocation site
 * (file_name). This includes rt->user_pool explicitly; it is not in
 * rt->metadata_pools because that list has GC-root/sweep semantics, but
 * user_pool is the primary debug pool and must be visible here. Runs at
 * the END of a debug_leak_detect collect, AFTER the mark, so each alive
 * alloc can be classified by its gc_epoch:
 *   - epoch == live_epoch  -> LIVE: reachable from real roots (retained).
 *   - epoch != live_epoch  -> LEAKED: alive in the pool but unreachable.
 * The LIVE breakdown is the one that answers "what am I holding a reference
 * to that I should have dropped" — a site with far more live bytes than its
 * role can justify is the retained-reference leak.  (The leak-detect sweep
 * runs in print-don't-reclaim mode, so both classes are still resident here
 * and the epoch is the discriminator.) */
static void
n00b_debug_pool_census_one(n00b_debug_census_t *census,
                           n00b_allocator_t    *allocator,
                           uint64_t             live_epoch)
{
    if (census == nullptr || allocator == nullptr || allocator->metadata == nullptr) {
        return;
    }

    __n00b_internal_type_erased_store_t *store
        = (__n00b_internal_type_erased_store_t *)n00b_atomic_load(&allocator->metadata->store);
    if (store == nullptr) {
        return;
    }

    uint32_t slots = store->last_slot + 1;
    for (uint32_t bi = 0; bi < slots; bi++) {
        n00b_dict_bucket_t *b = &store->buckets[bi];
        if (b->hv == (n00b_uint128_t)0) {
            continue;
        }
        if (n00b_atomic_load(&b->flags) & N00B_HT_FLAG_DELETED) {
            continue;
        }

        n00b_oob_hdr_t *oob = (n00b_oob_hdr_t *)store->values[bi];
        if (oob == nullptr || !oob->alive) {
            continue;
        }

        uint64_t ck = (uint64_t)(uintptr_t)(oob->file_name ? oob->file_name : "?");
        bool     f;
        if (oob->gc_epoch == live_epoch) {
            int64_t c  = n00b_dict_get(census->pool_live_count, ck, &f);
            int64_t nc = (f ? c : 0) + 1;
            n00b_dict_put(census->pool_live_count, ck, nc);

            int64_t bs = n00b_dict_get(census->pool_live_bytes, ck, &f);
            int64_t nb = (f ? bs : 0) + (int64_t)oob->alloc_len;
            n00b_dict_put(census->pool_live_bytes, ck, nb);

            census->pool_live_allocs++;
            census->pool_live_bytes_total += oob->alloc_len;
        }
        else {
            int64_t c  = n00b_dict_get(census->pool_leak_count, ck, &f);
            int64_t nc = (f ? c : 0) + 1;
            n00b_dict_put(census->pool_leak_count, ck, nc);

            int64_t bs = n00b_dict_get(census->pool_leak_bytes, ck, &f);
            int64_t nb = (f ? bs : 0) + (int64_t)oob->alloc_len;
            n00b_dict_put(census->pool_leak_bytes, ck, nb);

            census->pool_leak_allocs++;
            census->pool_leak_bytes_total += oob->alloc_len;
        }
    }
}

static uint64_t
n00b_debug_arena_mapped_bytes(n00b_arena_t *arena)
{
    if (arena == nullptr) {
        return 0;
    }

    uint64_t        total   = 0;
    n00b_segment_t *segment = n00b_atomic_load(&arena->current_segment);
    while (segment != nullptr) {
        total += segment->size;
        segment = segment->next_segment;
    }
    return total;
}

static void
n00b_debug_pool_census_note_metadata(n00b_debug_census_t *census, n00b_allocator_t *allocator)
{
    if (census == nullptr || allocator == nullptr) {
        return;
    }

    if (allocator->metadata_pool != nullptr) {
        census->metadata_pool_mapped_bytes
            += n00b_debug_arena_mapped_bytes((n00b_arena_t *)allocator->metadata_pool);
    }

    if (allocator->metadata == nullptr) {
        return;
    }
    __n00b_internal_type_erased_store_t *store
        = (__n00b_internal_type_erased_store_t *)n00b_atomic_load(&allocator->metadata->store);
    if (store == nullptr) {
        return;
    }

    census->metadata_pool_slots += (uint64_t)store->last_slot + 1u;
    for (uint32_t bi = 0; bi <= store->last_slot; bi++) {
        n00b_dict_bucket_t *b = &store->buckets[bi];
        if (b->hv == (n00b_uint128_t)0) {
            continue;
        }
        if (n00b_atomic_load(&b->flags) & N00B_HT_FLAG_DELETED) {
            continue;
        }
        census->metadata_pool_records++;
    }
}

static void
n00b_debug_pool_census(uint64_t live_epoch)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    if (!rt) {
        return;
    }

    n00b_debug_census_t *census = g_debug_census;
    if (census == nullptr) {
        return;
    }

    n00b_allocator_t *ca = census->allocator;

    census->pool_live_bytes = n00b_dict_new_private(uint64_t, int64_t, .allocator = ca);
    census->pool_live_count = n00b_dict_new_private(uint64_t, int64_t, .allocator = ca);
    census->pool_leak_bytes = n00b_dict_new_private(uint64_t, int64_t, .allocator = ca);
    census->pool_leak_count = n00b_dict_new_private(uint64_t, int64_t, .allocator = ca);

    size_t npools = rt->metadata_pools.data == nullptr ? 0 : n00b_list_len(rt->metadata_pools);
    bool   user_pool_seen = false;

    for (size_t pi = 0; pi < npools; pi++) {
        n00b_allocator_t *allocator = n00b_list_get(rt->metadata_pools, pi);
        if (allocator == (n00b_allocator_t *)&rt->user_pool) {
            user_pool_seen = true;
        }
        if (allocator != nullptr && allocator->metadata != nullptr) {
            census->metadata_pool_count++;
            n00b_debug_pool_census_note_metadata(census, allocator);
            n00b_debug_pool_census_one(census, allocator, live_epoch);
        }
    }

    n00b_allocator_t *user_pool = (n00b_allocator_t *)&rt->user_pool;
    if (!user_pool_seen && user_pool->metadata != nullptr) {
        census->metadata_pool_count++;
        n00b_debug_pool_census_note_metadata(census, user_pool);
        n00b_debug_pool_census_one(census, user_pool, live_epoch);
    }
}

// Census the to-space OOB metadata right after mark. For a GC arena the
// to-space dict holds exactly the live (forwarded) set — one record per
// surviving allocation — so this tallies the *retained* set by origin
// site (an over-retained arena shows which call sites kept allocations
// alive) and validates OOB migration: the record count MUST equal the
// forwarder's alloc_count. Leak-detect collects only; the to-space dict
// is still intact here (the segment swap / teardown runs later).
static void
n00b_debug_arena_census(n00b_collect_t *ctx)
{
    // Walk the FROM-space (every allocation live in the GC arena at collect
    // time), NOT the to-space (survivors only).  We run after mark and before
    // sweep, so each record's gc_epoch already separates reached (== current
    // epoch, LIVE) from stale (RECLAIMED garbage).  That gives total / live /
    // reclaimed per origin site in a single pass.
    _n00b_dict_internal_t *md = ctx->from_space->vtable.metadata;
    if (md == nullptr) {
        return; // inline-only arena: no OOB dict to walk.
    }

    __n00b_internal_type_erased_store_t *store
        = (__n00b_internal_type_erased_store_t *)n00b_atomic_load(&md->store);
    if (store == nullptr) {
        return;
    }

    n00b_debug_census_t *census = g_debug_census;
    if (census == nullptr) {
        return;
    }

    n00b_allocator_t *ca          = census->allocator;
    census->arena_site_count      = n00b_dict_new_private(uint64_t, int64_t, .allocator = ca);
    census->arena_site_bytes      = n00b_dict_new_private(uint64_t, int64_t, .allocator = ca);
    census->arena_site_live_count = n00b_dict_new_private(uint64_t, int64_t, .allocator = ca);
    census->arena_site_live_bytes = n00b_dict_new_private(uint64_t, int64_t, .allocator = ca);

    uint64_t rec_count = 0, total_bytes = 0;
    uint64_t live_count = 0, live_bytes = 0;
    uint32_t slots = store->last_slot + 1;

    for (uint32_t bi = 0; bi < slots; bi++) {
        n00b_dict_bucket_t *b = &store->buckets[bi];

        if (b->hv == (n00b_uint128_t)0) {
            continue;
        }
        if (n00b_atomic_load(&b->flags) & N00B_HT_FLAG_DELETED) {
            continue;
        }

        n00b_oob_hdr_t *oob = (n00b_oob_hdr_t *)store->values[bi];
        if (oob == nullptr) {
            continue;
        }

        bool is_live = (oob->gc_epoch == ctx->current_epoch);

        rec_count++;
        total_bytes += oob->alloc_len;
        if (is_live) {
            live_count++;
            live_bytes += oob->alloc_len;
        }

        uint64_t ck = (uint64_t)(uintptr_t)(oob->file_name ? oob->file_name : "?");
        bool     f;
        int64_t  c  = n00b_dict_get(census->arena_site_count, ck, &f);
        int64_t  nc = (f ? c : 0) + 1;
        n00b_dict_put(census->arena_site_count, ck, nc);
        int64_t bs = n00b_dict_get(census->arena_site_bytes, ck, &f);
        int64_t nb = (f ? bs : 0) + (int64_t)oob->alloc_len;
        n00b_dict_put(census->arena_site_bytes, ck, nb);

        if (is_live) {
            int64_t lc  = n00b_dict_get(census->arena_site_live_count, ck, &f);
            int64_t nlc = (f ? lc : 0) + 1;
            n00b_dict_put(census->arena_site_live_count, ck, nlc);
            int64_t lb  = n00b_dict_get(census->arena_site_live_bytes, ck, &f);
            int64_t nlb = (f ? lb : 0) + (int64_t)oob->alloc_len;
            n00b_dict_put(census->arena_site_live_bytes, ck, nlb);
        }
    }

    uint64_t fwd = ctx->to_space->alloc_count;
    census->arena_name
        = ctx->from_space->vtable.debug_name ? ctx->from_space->vtable.debug_name : "?";
    census->arena_record_count      = rec_count;
    census->arena_total_bytes       = total_bytes;
    census->arena_live_record_count = live_count;
    census->arena_live_bytes_total  = live_bytes;
    census->arena_forwarded_count   = fwd;
    census->arena_seen              = true;
}

void
n00b_debug_find_leaks_to_conduit(n00b_conduit_topic_t(n00b_buffer_t *) * topic)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    if (!rt) {
        return;
    }

    n00b_arena_t *arena = rt->default_arena;
    if (!arena) {
        return;
    }

    bool expected = false;
    if (!n00b_atomic_cas(&g_debug_census_active, &expected, true)) {
        return;
    }

    n00b_arena_t        *census_arena = n00b_new_arena(.size   = (1 << 22),
                                                .use_gc = false,
                                                .no_map = true,
                                                .name   = "debug_census");
    n00b_allocator_t    *ca           = (n00b_allocator_t *)census_arena;
    n00b_debug_census_t *census
        = n00b_alloc_with_opts(n00b_debug_census_t, &(n00b_alloc_opts_t){.allocator = ca});

    *census = (n00b_debug_census_t){
        .arena                = census_arena,
        .allocator            = ca,
        .leak_sample_capacity = N00B_DEBUG_CENSUS_LEAK_SAMPLE_MAX,
    };
    census->leak_samples = n00b_alloc_array_with_opts(n00b_debug_leak_sample_t,
                                                      N00B_DEBUG_CENSUS_LEAK_SAMPLE_MAX,
                                                      &(n00b_alloc_opts_t){.allocator = ca});

    /* Toggle the runtime flag that turns the standard sweep into
     * "record, don't reclaim" mode for the duration of one collection.
     * n00b_collect() drives the single STW handshake; the census session
     * is only published after collect returns and the world is running. */
    uint64_t started_ns = n00b_gc_timestamp_ns();
    g_debug_census      = census;
    n00b_atomic_store(&rt->debug_leak_detect, true);
    n00b_collect(arena);
    n00b_atomic_store(&rt->debug_leak_detect, false);
    g_debug_census       = nullptr;
    g_site_census        = nullptr;
    uint64_t finished_ns = n00b_gc_timestamp_ns();

    n00b_debug_census_store_stats(census, started_ns, finished_ns);
    n00b_debug_census_publish(census, topic, false); // find_leaks: post-collect, conduit OK
    n00b_allocator_destroy(ca);
    n00b_atomic_store(&g_debug_census_active, false);
}

[[n00b::nogc]] n00b_debug_census_stats_t
n00b_debug_census_stats(void)
{
    n00b_debug_census_stats_t stats = {
        .enabled                  = true,
        .active                   = n00b_atomic_load(&g_debug_census_active),
        .runs                     = n00b_atomic_load(&g_debug_census_runs),
        .last_started_ns          = n00b_atomic_load(&g_debug_census_last_started_ns),
        .last_finished_ns         = n00b_atomic_load(&g_debug_census_last_finished_ns),
        .last_duration_ns         = n00b_atomic_load(&g_debug_census_last_duration_ns),
        .gc_total_pause_ns        = n00b_atomic_load(&g_debug_census_gc_total_pause_ns),
        .gc_census_ns             = n00b_atomic_load(&g_debug_census_gc_census_ns),
        .gc_root_count            = n00b_atomic_load(&g_debug_census_gc_root_count),
        .gc_root_words            = n00b_atomic_load(&g_debug_census_gc_root_words),
        .gc_scan_range_count      = n00b_atomic_load(&g_debug_census_gc_scan_range_count),
        .gc_scan_words            = n00b_atomic_load(&g_debug_census_gc_scan_words),
        .gc_worklist_origin_count = n00b_atomic_load(&g_debug_census_gc_worklist_origin_count),
        .gc_worklist_origin_words = n00b_atomic_load(&g_debug_census_gc_worklist_origin_words),
        .pool_live_allocs         = n00b_atomic_load(&g_debug_census_pool_live_allocs),
        .pool_live_bytes          = n00b_atomic_load(&g_debug_census_pool_live_bytes),
        .pool_leak_allocs         = n00b_atomic_load(&g_debug_census_pool_leak_allocs),
        .pool_leak_bytes          = n00b_atomic_load(&g_debug_census_pool_leak_bytes),
        .metadata_pool_count      = n00b_atomic_load(&g_debug_census_metadata_pool_count),
        .metadata_pool_mapped_bytes
        = n00b_atomic_load(&g_debug_census_metadata_pool_mapped_bytes),
        .metadata_pool_records  = n00b_atomic_load(&g_debug_census_metadata_pool_records),
        .metadata_pool_slots    = n00b_atomic_load(&g_debug_census_metadata_pool_slots),
        .arena_record_count     = n00b_atomic_load(&g_debug_census_arena_record_count),
        .arena_total_bytes      = n00b_atomic_load(&g_debug_census_arena_total_bytes),
        .arena_forwarded_count  = n00b_atomic_load(&g_debug_census_arena_forwarded_count),
        .leak_sample_count      = n00b_atomic_load(&g_debug_census_leak_sample_count),
        .leak_total_count       = n00b_atomic_load(&g_debug_census_leak_total_count),
        .leak_total_bytes       = n00b_atomic_load(&g_debug_census_leak_total_bytes),
        .suspicious_alloc_count = n00b_atomic_load(&g_debug_census_suspicious_alloc_count),
        .suspicious_worklist_count
        = n00b_atomic_load(&g_debug_census_suspicious_worklist_count),
        .slow_worklist_count = n00b_atomic_load(&g_debug_census_slow_worklist_count),
    };
    stats.site_live_top_count = n00b_atomic_load(&g_debug_census_site_live_top_count);
    stats.pool_live_top_count = n00b_atomic_load(&g_debug_census_pool_live_top_count);
    stats.pool_leak_top_count = n00b_atomic_load(&g_debug_census_pool_leak_top_count);
    for (uint64_t i = 0; i < N00B_DEBUG_CENSUS_HEALTH_TOP_N; i++) {
        stats.site_live_top_site[i]
            = (const char *)(uintptr_t)n00b_atomic_load(&g_debug_census_site_live_top_site[i]);
        stats.site_live_top_allocs[i]
            = n00b_atomic_load(&g_debug_census_site_live_top_allocs[i]);
        stats.pool_live_top_site[i]
            = (const char *)(uintptr_t)n00b_atomic_load(&g_debug_census_pool_live_top_site[i]);
        stats.pool_live_top_bytes[i] = n00b_atomic_load(&g_debug_census_pool_live_top_bytes[i]);
        stats.pool_live_top_allocs[i]
            = n00b_atomic_load(&g_debug_census_pool_live_top_allocs[i]);
        stats.pool_leak_top_site[i]
            = (const char *)(uintptr_t)n00b_atomic_load(&g_debug_census_pool_leak_top_site[i]);
        stats.pool_leak_top_bytes[i] = n00b_atomic_load(&g_debug_census_pool_leak_top_bytes[i]);
        stats.pool_leak_top_allocs[i]
            = n00b_atomic_load(&g_debug_census_pool_leak_top_allocs[i]);
    }
    return stats;
}

void
n00b_debug_find_leaks(void)
{
    n00b_runtime_t *rt                           = n00b_get_runtime();
    n00b_conduit_topic_t(n00b_buffer_t *) *topic = nullptr;

    if (rt != nullptr && rt->stderr_topic != nullptr) {
        topic = (n00b_conduit_topic_t(n00b_buffer_t *) *)rt->stderr_topic;
    }

    n00b_debug_find_leaks_to_conduit(topic);
}

void
n00b_debug_census_on_collect_set(bool enabled)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    if (rt == nullptr) {
        return;
    }
    n00b_atomic_store(&rt->census_on_collect, enabled);
}

bool
n00b_debug_census_on_collect_enabled(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    if (rt == nullptr) {
        return false;
    }
    return n00b_atomic_load(&rt->census_on_collect);
}
#else
void
n00b_debug_find_leaks_to_conduit(n00b_conduit_topic_t(n00b_buffer_t *) * topic)
{
    (void)topic;
}

void
n00b_debug_find_leaks(void)
{
}

n00b_debug_census_stats_t
n00b_debug_census_stats(void)
{
    return (n00b_debug_census_stats_t){};
}

void
n00b_debug_census_on_collect_set(bool enabled)
{
    (void)enabled;
}

bool
n00b_debug_census_on_collect_enabled(void)
{
    return false;
}
#endif
