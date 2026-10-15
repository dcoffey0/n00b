#define N00B_MEM_INTERNAL_API

#include "n00b.h"
#include "core/codegen_abi_inject.h" // n00b_static_identity_t layout + N00B_STATIC_IDENTITY_* constants
#include "core/arena.h"
#include "core/runtime.h"
#include "core/mmaps.h"
#include "core/alloc_mdata.h"
#include "core/alloc.h"
#include "core/atomic.h"
#include "core/futex.h"
#include "core/align.h"
#include "core/pool.h"
#include "core/thread.h"
#include "core/runtime.h"
#include "core/stw.h" // n00b_world_is_stopped (mmap_lock STW short-circuit)
#include "adt/interval_tree.h"
#include "adt/variant.h"
#include "conduit/print.h"

// Raw munmap/VirtualFree failures from n00b_safe_munmap (declared in mmaps.h).
// Nonzero => pages not returned to the OS (silent leak); watched while chasing
// the GC page-reclaim leak.
_Atomic(uint64_t) n00b_munmap_fail_count = 0;
_Atomic(uint64_t) n00b_mmap_skip_register_count = 0;
_Atomic(uint64_t) n00b_mmap_skip_register_bytes = 0;
_Atomic(uint64_t) n00b_safe_munmap_registry_count = 0;
_Atomic(uint64_t) n00b_safe_munmap_registry_bytes = 0;
_Atomic(uint64_t) n00b_safe_munmap_raw_count = 0;
_Atomic(uint64_t) n00b_safe_munmap_raw_bytes = 0;

#ifdef _WIN32
bool
n00b_win_free_range(void *addr, size_t size)
{
    uintptr_t start = (uintptr_t)addr;
    uintptr_t end   = start + (uintptr_t)size;

    if (addr == nullptr || size == 0 || end < start) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(addr, &mbi, sizeof(mbi))) {
        return false;
    }

    void *allocation_base = mbi.AllocationBase;
    bool  whole           = allocation_base == addr;

    if (whole) {
        uintptr_t p = start;
        while (p < end) {
            if (!VirtualQuery((void *)p, &mbi, sizeof(mbi))
                || mbi.AllocationBase != allocation_base) {
                whole = false;
                break;
            }
            uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            if (next <= p) {
                whole = false;
                break;
            }
            p = next;
        }
        if (whole && VirtualQuery((void *)end, &mbi, sizeof(mbi))
            && mbi.AllocationBase == allocation_base) {
            whole = false;
        }
    }

    return whole ? VirtualFree(addr, 0, MEM_RELEASE)
                 : VirtualFree(addr, size, MEM_DECOMMIT);
}
#endif

// TODO: fix this
// #include "conduit/print.h"
#include <stdio.h>
#define n00b_fprintf fprintf

#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <errno.h>

#if !defined(N00B_MMAPS_START_PAGES)
#define N00B_MMAPS_START_PAGES 16
#endif

// Convenience alias for the concrete tree/node types.
#define mmap_tree_t n00b_interval_tree_t(n00b_mmap_data_t)
#define mmap_node_t n00b_interval_node_t(n00b_mmap_data_t)

// ============================================================================
// Locking (WP-001)
//
// The mmap registry is guarded by ctx->lock, the re-entrant non-parking
// spinlock (n00b_spin_lock_t).  Two properties matter here:
//
//   * Re-entrant.  A tree mutation can nest a lookup (mmaps_insert_raw ->
//     n00b_free -> finalizer -> n00b_mmap_by_address).  The spinlock tracks
//     owner+nesting, so the same thread re-acquiring just bumps the nesting
//     count, and only the OUTERMOST unlock releases — fixing the old tid_lock
//     bug where a nested unlock dropped the lock the outer mutation still held
//     (letting another thread mutate concurrently → cyclic tree → infinite
//     loop in n00b_mmap_search_point).
//
//   * Non-parking.  It is held inside the critical_execution gate; a parked
//     holder would stall a stop-the-world initiator forever (see
//     core/spinlock.h).
//
// A registry op is CRITICAL EXECUTION (WP-001): mutating the mmap interval tree
// while a stop-the-world initiator suspended us mid-op would corrupt the tree
// for the collector's lock-free walk.  So the outermost acquire also holds
// rt->critical_execution — the single STW gate — for the duration of the op.
// A stop-the-world initiator must ACQUIRE that gate before it suspends anyone,
// so it can never freeze a thread mid-registry-mutation.
//
// The collector short-circuits on rt->stw_active: once the world is stopped it
// is the SOLE runner (every mutator suspended), the tree is immutable, and the
// per-word mark must not pay a lock.  It gates on stw_active (set only AFTER the
// gate is held and everyone is suspended), NOT on "is a stop in progress": a
// mutator that reached this code is, by definition, holding the gate, so no
// stop can be in progress underneath it.
//
// Both "read" and "write" registry locks take the spinlock EXCLUSIVELY: the
// interval-tree search/lookup paths mutate a SHARED, non-reentrant per-tree
// descent stack (see adt/interval_tree.h), so two concurrent readers would
// clobber it — the original tid_lock serialized every registry op, and we keep
// that.
//
// Early init (before startup_complete) is single-threaded — no other thread,
// no GC — so the lock and gate are skipped entirely there.
// ============================================================================

[[n00b::nogc]] static inline void
mmap_lock(n00b_mmap_ctx_t *ctx)
{
    if (n00b_atomic_load(&n00b_get_runtime()->stw_active)) {
        return; // sole-runner collector: tree immutable, no lock needed.
    }
    if (!n00b_atomic_load(&n00b_get_runtime()->startup_complete)) {
        return; // single-threaded init.
    }
    /* MUTATOR.  Take the STW lock as a READER on the OUTERMOST acquire only, so
     * the collector (the WRITE lock holder) cannot freeze us mid-tree-mutation;
     * concurrent mutators run as fellow readers and are serialized among
     * themselves by the spinlock below.  A nested acquire is already covered by
     * the outer reader hold (the read lock is reentrant, but we gate on the
     * spinlock's own owner check so the matching unlock balances). */
    if (!n00b_lock_already_owner((n00b_lock_base_t *)&ctx->lock)) {
        n00b_rw_read_lock(&n00b_get_runtime()->critical_execution);
    }
    n00b_spinlock_lock(&ctx->lock);
}

[[n00b::nogc]] static inline void
mmap_unlock(n00b_mmap_ctx_t *ctx)
{
    if (n00b_atomic_load(&n00b_get_runtime()->stw_active)) {
        return;
    }
    if (!n00b_atomic_load(&n00b_get_runtime()->startup_complete)) {
        return;
    }
    /* Only the OUTERMOST unlock (spinlock fully released) releases the gate; a
     * nested unlock just unwinds the spinlock's nesting count. */
    if (n00b_spinlock_unlock(&ctx->lock)) {
        n00b_rw_unlock(&n00b_get_runtime()->critical_execution);
    }
}

#define mmap_write_lock(ctx)   mmap_lock(ctx)
#define mmap_read_lock(ctx)    mmap_lock(ctx)
#define mmap_write_unlock(ctx) mmap_unlock(ctx)
#define mmap_read_unlock(ctx)  mmap_unlock(ctx)

[[n00b::nogc]] static n00b_option_t(n00b_mmap_info_t *)
n00b_mmap_lookup_unlocked(n00b_mmap_ctx_t *ctx, void *addr);

/* Async-signal-handler-callable lookup: pulls fields out of the
 * existing mmap registry for a candidate faulting address.  No
 * @ref n00b_eprintf, no n00b_string_t allocation — only the tree's
 * read lock, which is just an atomic spin.
 *
 * In a healthy process the lock contention risk is real; the only
 * intended caller is a SIGBUS handler in a process that is already
 * crashing, so a stuck spinlock just means we get reaped by the
 * kernel a few milliseconds later. */
[[n00b::nogc]] void *
n00b_mmap_handler_lookup(uintptr_t addr,
                         uint64_t *out_start,
                         uint64_t *out_end,
                         uint32_t *out_kind,
                         const char **out_file,
                         const char **out_source_file,
                         uint32_t *out_source_line)
{
    n00b_mmap_ctx_t  *ctx  = n00b_global_mem_map(n00b_get_runtime());
    n00b_mmap_info_t *info = nullptr;

    mmap_read_lock(ctx);
    auto opt = n00b_mmap_lookup_unlocked(ctx, (void *)addr);
    if (n00b_option_is_set(opt)) {
        info = n00b_option_get(opt);
        if (out_start) *out_start = info->start;
        if (out_end)   *out_end   = info->end;
        if (out_kind)  *out_kind  = (uint32_t)info->kind;
        if (out_file)  *out_file  = info->file;
        if (out_source_file) *out_source_file = info->source_file;
        if (out_source_line) *out_source_line = info->source_line;
    }
    mmap_read_unlock(ctx);
    return info;
}

static void
mmap_registry_stats_record_largest(n00b_mmap_registry_stats_t *stats,
                                   n00b_mmap_info_t           *info,
                                   uint64_t                    bytes)
{
    if (bytes > stats->largest_bytes) {
        stats->largest_bytes       = bytes;
        stats->largest_kind        = (uint64_t)info->kind;
        stats->largest_file        = info->file;
        stats->largest_source_file = info->source_file;
        stats->largest_source_line = info->source_line;
    }
    if (info->kind == n00b_mmap_static && bytes > stats->largest_static_bytes) {
        stats->largest_static_bytes       = bytes;
        stats->largest_static_kind        = (uint64_t)info->kind;
        stats->largest_static_file        = info->file;
        stats->largest_static_source_file = info->source_file;
        stats->largest_static_source_line = info->source_line;
    }
}

static void
mmap_registry_stats_add(n00b_mmap_registry_stats_t *stats,
                        n00b_mmap_info_t           *info)
{
    if (stats == nullptr || info == nullptr || info->end <= info->start) {
        return;
    }
    uint64_t bytes = info->end - info->start;
    stats->total_count++;
    stats->total_bytes += bytes;
    mmap_registry_stats_record_largest(stats, info, bytes);
    switch (info->kind) {
    case n00b_mmap_static:
        stats->static_count++;
        stats->static_bytes += bytes;
        break;
    case n00b_mmap_arena:
        stats->arena_count++;
        stats->arena_bytes += bytes;
        break;
    case n00b_mmap_managed_segment:
        stats->managed_segment_count++;
        stats->managed_segment_bytes += bytes;
        break;
    case n00b_mmap_sys_segment:
        stats->system_segment_count++;
        stats->system_segment_bytes += bytes;
        break;
    case n00b_mmap_zero_page:
        stats->zero_page_count++;
        stats->zero_page_bytes += bytes;
        break;
    case n00b_mmap_unmanaged:
        stats->unmanaged_count++;
        stats->unmanaged_bytes += bytes;
        break;
    case n00b_mmap_stack:
        stats->stack_count++;
        stats->stack_bytes += bytes;
        break;
    case n00b_mmap_internal:
        stats->internal_count++;
        stats->internal_bytes += bytes;
        break;
    case n00b_mmap_pool:
        stats->pool_count++;
        stats->pool_bytes += bytes;
        break;
    case n00b_mmap_api_mmap:
        stats->api_mmap_count++;
        stats->api_mmap_bytes += bytes;
        break;
    default:
        stats->unknown_count++;
        stats->unknown_bytes += bytes;
        break;
    }
}

static void
mmap_registry_stats_walk(mmap_node_t                 *node,
                         n00b_mmap_registry_stats_t *stats)
{
    if (node == nullptr || stats == nullptr) {
        return;
    }
    mmap_registry_stats_walk(node->left, stats);
    if (n00b_variant_is_type(node->data, n00b_mmap_info_t *)) {
        mmap_registry_stats_add(stats,
                                n00b_variant_get(node->data,
                                                 n00b_mmap_info_t *));
    }
    mmap_registry_stats_walk(node->right, stats);
}

n00b_mmap_registry_stats_t
n00b_mmap_registry_stats(void)
{
    n00b_mmap_registry_stats_t stats = {};
    n00b_runtime_t *rt = n00b_get_runtime();
    if (rt == nullptr) {
        return stats;
    }
    n00b_mmap_ctx_t *ctx = n00b_global_mem_map(rt);
    mmap_read_lock(ctx);
    mmap_registry_stats_walk(ctx->mmap_tree->root, &stats);
    mmap_read_unlock(ctx);
    // Phantom memory — out-of-registry allocators, tracked separately so every
    // mapped byte is accountable.
    stats.all_arena_bytes     = n00b_arena_audit_total_bytes();
    stats.registry_pool_bytes = n00b_pool_mapped_bytes(&ctx->pool);
    stats.skip_register_count = atomic_load(&n00b_mmap_skip_register_count);
    stats.skip_register_bytes = atomic_load(&n00b_mmap_skip_register_bytes);
    stats.safe_munmap_registry_count =
        atomic_load(&n00b_safe_munmap_registry_count);
    stats.safe_munmap_registry_bytes =
        atomic_load(&n00b_safe_munmap_registry_bytes);
    stats.safe_munmap_raw_count = atomic_load(&n00b_safe_munmap_raw_count);
    stats.safe_munmap_raw_bytes = atomic_load(&n00b_safe_munmap_raw_bytes);
    stats.safe_munmap_fail_count = atomic_load(&n00b_munmap_fail_count);
    return stats;
}

// Histogram of registered mmap segments by (source_file:source_line).  Distinct
// mmap call sites are few (tens), so a fixed accumulator captures them all; if it
// ever overflowed, further sites are dropped (count stops growing) rather than
// misattributed.
#define N00B_MMAP_HIST_SLOTS 256

typedef struct {
    n00b_mmap_site_t slots[N00B_MMAP_HIST_SLOTS];
    uint32_t         n;
} mmap_hist_acc_t;

static void
mmap_source_histogram_walk(mmap_node_t *node, mmap_hist_acc_t *h)
{
    if (node == nullptr) {
        return;
    }
    mmap_source_histogram_walk(node->left, h);
    if (n00b_variant_is_type(node->data, n00b_mmap_info_t *)) {
        n00b_mmap_info_t *info = n00b_variant_get(node->data, n00b_mmap_info_t *);
        if (info != nullptr && info->end > info->start) {
            uint64_t    bytes = info->end - info->start;
            // Attribute the segment to the create-site of its owning allocator
            // (arena/pool segments carry no source loc of their own — only the
            // arena.c / pool.c registration line, which is the same for all of
            // them). creation_loc is the "file:line" of the n00b_new_arena /
            // n00b_pool_init call; debug_name is the next-best label for a named
            // allocator with no recorded create-site. Fall back to the mmap's own
            // source loc for everything else (code maps, big mmaps, etc.).
            n00b_allocator_t *a = n00b_atomic_load(&info->allocator);
            const char       *sf;
            uint32_t          sl;
            if (a != nullptr && a->creation_loc != nullptr) {
                sf = a->creation_loc;
                sl = 0;
            }
            else if (a != nullptr && a->debug_name != nullptr) {
                sf = a->debug_name;
                sl = 0;
            }
            else {
                sf = info->source_file ? info->source_file : info->file;
                sl = info->source_line;
            }
            uint32_t    i;
            for (i = 0; i < h->n; i++) {
                if (h->slots[i].source_file == sf
                    && h->slots[i].source_line == sl) {
                    break;
                }
            }
            if (i == h->n && h->n < N00B_MMAP_HIST_SLOTS) {
                h->slots[i].source_file = sf;
                h->slots[i].source_line = sl;
                h->slots[i].count       = 0;
                h->slots[i].bytes       = 0;
                h->n++;
            }
            if (i < h->n) {
                h->slots[i].count++;
                h->slots[i].bytes += bytes;
            }
        }
    }
    mmap_source_histogram_walk(node->right, h);
}

uint32_t
n00b_mmap_source_histogram(n00b_mmap_site_t *out, uint32_t cap)
{
    if (out == nullptr || cap == 0) {
        return 0;
    }
    n00b_runtime_t *rt = n00b_get_runtime();
    if (rt == nullptr) {
        return 0;
    }
    n00b_mmap_ctx_t *ctx = n00b_global_mem_map(rt);
    // Stack-local accumulator (~8 KB): no allocation while holding the registry
    // lock (which would re-enter the mmap path), and no allocator dependency.
    mmap_hist_acc_t hist = {};
    mmap_read_lock(ctx);
    mmap_source_histogram_walk(ctx->mmap_tree->root, &hist);
    mmap_read_unlock(ctx);

    // Selection-sort the top `cap` buckets by count, descending, into `out`.
    uint32_t out_n = cap < hist.n ? cap : hist.n;
    for (uint32_t k = 0; k < out_n; k++) {
        uint32_t best = k;
        for (uint32_t j = k + 1; j < hist.n; j++) {
            if (hist.slots[j].count > hist.slots[best].count) {
                best = j;
            }
        }
        n00b_mmap_site_t tmp = hist.slots[k];
        hist.slots[k]        = hist.slots[best];
        hist.slots[best]     = tmp;
        out[k]               = hist.slots[k];
    }
    return out_n;
}

struct n00b_static_identity_entry_t {
    const n00b_static_identity_t *identity;
    n00b_alloc_range_t           *range;
    n00b_static_identity_entry_t *next;
};

static bool
static_identity_valid(const n00b_static_identity_t *identity)
{
    if (identity == nullptr) {
        return false;
    }

    if (identity->version != N00B_STATIC_IDENTITY_VERSION) {
        return false;
    }

    if (identity->kind == N00B_STATIC_IDENTITY_NONE) {
        return false;
    }

    if (identity->namespace_id == nullptr || identity->namespace_id[0] == '\0') {
        return false;
    }

    if (identity->object_key == nullptr || identity->object_key[0] == '\0') {
        return false;
    }

    return true;
}

static bool
static_identity_equal(const n00b_static_identity_t *a,
                      const n00b_static_identity_t *b)
{
    return a->version == b->version
        && a->kind == b->kind
        && strcmp(a->namespace_id, b->namespace_id) == 0
        && strcmp(a->object_key, b->object_key) == 0;
}

static n00b_static_identity_status_t
static_identity_validate_range(n00b_alloc_range_t                 *range,
                               const n00b_static_identity_query_t *query)
{
    if (query == nullptr || query->checks == N00B_STATIC_IDENTITY_CHECK_NONE) {
        return N00B_STATIC_IDENTITY_OK;
    }

    if ((query->checks & N00B_STATIC_IDENTITY_CHECK_LEN) != 0
        && range->len != query->len) {
        return N00B_STATIC_IDENTITY_ERR_LENGTH;
    }

    if ((query->checks & N00B_STATIC_IDENTITY_CHECK_TINFO) != 0
        && range->tinfo != query->tinfo) {
        return N00B_STATIC_IDENTITY_ERR_TYPE;
    }

    if ((query->checks & N00B_STATIC_IDENTITY_CHECK_SCAN_KIND) != 0
        && range->scan_kind != query->scan_kind) {
        return N00B_STATIC_IDENTITY_ERR_SCAN;
    }

    if ((query->checks & N00B_STATIC_IDENTITY_CHECK_FLAGS) != 0
        && (range->flags & query->flags_mask) != query->flags_value) {
        return N00B_STATIC_IDENTITY_ERR_MUTABILITY;
    }

    if ((query->checks & N00B_STATIC_IDENTITY_CHECK_BYTES) != 0) {
        uint64_t end = query->check_offset + (uint64_t)query->check_len;

        if (query->check_bytes == nullptr) {
            return N00B_STATIC_IDENTITY_ERR_CHECK_BYTES;
        }
        if (end < query->check_offset || end > range->len) {
            return N00B_STATIC_IDENTITY_ERR_LENGTH;
        }
        if (memcmp((const unsigned char *)range->start + query->check_offset,
                   query->check_bytes,
                   query->check_len) != 0) {
            return N00B_STATIC_IDENTITY_ERR_CHECK_BYTES;
        }
    }

    return N00B_STATIC_IDENTITY_OK;
}

static n00b_static_identity_status_t
static_identity_register_unlocked(n00b_mmap_ctx_t                *ctx,
                                  const n00b_static_identity_t   *identity,
                                  n00b_alloc_range_t             *range)
{
    if (identity == nullptr || range == nullptr) {
        return N00B_STATIC_IDENTITY_ERR_NULL;
    }
    if (!static_identity_valid(identity)) {
        return N00B_STATIC_IDENTITY_ERR_INVALID;
    }

    uint32_t live_matches = 0;
    n00b_static_identity_entry_t *entry;
    for (entry = ctx->static_identities;
         entry != nullptr;
         entry = entry->next) {
        if (entry->range != nullptr && static_identity_equal(entry->identity, identity)) {
            if (entry->range == range) {
                return live_matches == 0
                    ? N00B_STATIC_IDENTITY_OK
                    : N00B_STATIC_IDENTITY_ERR_DUPLICATE;
            }
            live_matches++;
        }
    }

    n00b_allocator_t *alloc = (n00b_allocator_t *)&ctx->pool;
    entry = n00b_alloc_with_opts(
        n00b_static_identity_entry_t,
        &(n00b_alloc_opts_t){.allocator = alloc});

    *entry = (n00b_static_identity_entry_t){
        .identity = identity,
        .range    = range,
        .next     = ctx->static_identities,
    };
    ctx->static_identities = entry;
    range->identity        = identity;

    return live_matches == 0
        ? N00B_STATIC_IDENTITY_OK
        : N00B_STATIC_IDENTITY_ERR_DUPLICATE;
}

static void
static_identity_unregister_range_unlocked(n00b_mmap_ctx_t *ctx,
                                          n00b_alloc_range_t *range)
{
    // A range only ever enters the list alongside range->identity being set,
    // under this same write lock (static_identity_register_unlocked), so a null
    // identity proves the list holds no entry for it and the walk below cannot
    // match.  Every sub-range of an ordinary allocator page is in that class,
    // and each detach was paying a full list traversal to discover it.
    if (range == nullptr || range->identity == nullptr) {
        return;
    }

    n00b_static_identity_entry_t *entry;
    for (entry = ctx->static_identities;
         entry != nullptr;
         entry = entry->next) {
        if (entry->range == range) {
            entry->range = nullptr;
        }
    }
}

n00b_static_identity_status_t
n00b_static_identity_register(const n00b_static_identity_t *identity,
                              n00b_alloc_range_t *range)
{
    n00b_runtime_t  *rt  = n00b_get_runtime();
    n00b_mmap_ctx_t *ctx = n00b_global_mem_map(rt);
    n00b_static_identity_status_t result;

    mmap_write_lock(ctx);
    result = static_identity_register_unlocked(ctx, identity, range);
    mmap_write_unlock(ctx);

    return result;
}

n00b_static_identity_status_t
n00b_static_identity_lookup(const n00b_static_identity_t *identity,
                            const n00b_static_identity_query_t *query,
                            n00b_alloc_range_t **out_range)
{
    if (out_range != nullptr) {
        *out_range = nullptr;
    }
    if (identity == nullptr) {
        return N00B_STATIC_IDENTITY_ERR_NULL;
    }
    if (!static_identity_valid(identity)) {
        return N00B_STATIC_IDENTITY_ERR_INVALID;
    }

    n00b_runtime_t       *rt  = n00b_get_runtime();
    n00b_mmap_ctx_t      *ctx = n00b_global_mem_map(rt);
    n00b_alloc_range_t   *match = nullptr;
    uint32_t              live_matches = 0;

    mmap_read_lock(ctx);
    n00b_static_identity_entry_t *entry;
    for (entry = ctx->static_identities;
         entry != nullptr;
         entry = entry->next) {
        if (entry->range != nullptr && static_identity_equal(entry->identity, identity)) {
            match = entry->range;
            live_matches++;
        }
    }

    if (live_matches == 0) {
        mmap_read_unlock(ctx);
        return N00B_STATIC_IDENTITY_ERR_MISSING;
    }
    if (live_matches > 1) {
        mmap_read_unlock(ctx);
        return N00B_STATIC_IDENTITY_ERR_DUPLICATE;
    }

    n00b_static_identity_status_t result = static_identity_validate_range(match, query);
    if (result == N00B_STATIC_IDENTITY_OK && out_range != nullptr) {
        *out_range = match;
    }
    mmap_read_unlock(ctx);

    return result;
}

const char *
n00b_static_identity_status_name(n00b_static_identity_status_t status)
{
    switch (status) {
    case N00B_STATIC_IDENTITY_OK:
        return "ok";
    case N00B_STATIC_IDENTITY_ERR_NULL:
        return "null";
    case N00B_STATIC_IDENTITY_ERR_INVALID:
        return "invalid";
    case N00B_STATIC_IDENTITY_ERR_MISSING:
        return "missing";
    case N00B_STATIC_IDENTITY_ERR_DUPLICATE:
        return "duplicate";
    case N00B_STATIC_IDENTITY_ERR_MUTABILITY:
        return "mutability";
    case N00B_STATIC_IDENTITY_ERR_TYPE:
        return "type";
    case N00B_STATIC_IDENTITY_ERR_SCAN:
        return "scan";
    case N00B_STATIC_IDENTITY_ERR_LENGTH:
        return "length";
    case N00B_STATIC_IDENTITY_ERR_CHECK_BYTES:
        return "check-bytes";
    default:
        return "unknown";
    }
}

// ============================================================================
// Internal helpers
// ============================================================================

static n00b_mmap_info_t *
mmaps_insert_raw(n00b_mmap_ctx_t     *ctx,
                 void                *startp,
                 uint64_t             blen,
                 n00b_mmap_rec_kind_t kind,
                 uint64_t             binary_offset,
                 n00b_mmap_perms_t    perms)
{
    n00b_allocator_t *alloc = (n00b_allocator_t *)&ctx->pool;
    n00b_mmap_info_t *info  = n00b_alloc_with_opts(n00b_mmap_info_t, &(n00b_alloc_opts_t){.allocator = alloc});
    uint64_t          start = (uint64_t)startp;
    uint64_t          end   = start + blen;

    *info = (n00b_mmap_info_t){
        .start         = start,
        .end           = end,
        .kind          = kind,
        .binary_offset = binary_offset,
        .perms         = perms,
    };

    n00b_mmap_data_t data = n00b_variant_set(n00b_mmap_data_t, n00b_mmap_info_t *, info);
    auto insert_r = n00b_interval_insert(ctx->mmap_tree, start, end, data);
    assert(n00b_result_is_ok(insert_r));
    info->tree_node = n00b_result_get(insert_r);

    return info;
}

static bool
n00b_mmaps_detach_base(n00b_mmap_ctx_t *ctx, n00b_mmap_info_t *info)
{
    /* The hidden filter that used to live here was dead code under
     * the original register path (hidden allocators' pages never
     * entered the tree, so this detach was never reached for them).
     * @ref n00b_mmap_register_pool_page does put hidden+metadata
     * pool pages in the tree on purpose so n00b_free can resolve
     * them, and those pages MUST be removed here when the pool
     * unmaps them — otherwise the tree grows monotonically as the
     * pool churns. The check is therefore unconditional now. */
    (void)n00b_interval_delete(ctx->mmap_tree, (mmap_node_t *)info->tree_node);
    return true;
}

static inline void
n00b_mmap_free_registry_obj(n00b_mmap_ctx_t *ctx, void *ptr)
{
    n00b_allocator_t *alloc = (n00b_allocator_t *)&ctx->pool;

    if (ptr != nullptr && alloc->free != nullptr) {
        (*alloc->free)(alloc, ptr);
    }
}

#ifdef N00B_DEBUG
bool n00b_mmap_debug_poison_freed_records = false;
#endif

static inline void
n00b_mmap_free_info_record(n00b_mmap_ctx_t *ctx, n00b_mmap_info_t *info)
{
#ifdef N00B_DEBUG
    if (n00b_mmap_debug_poison_freed_records && info != nullptr) {
        memset(info, 0xdb, sizeof(*info));
        return;
    }
#endif
    n00b_mmap_free_registry_obj(ctx, info);
}

static inline void
n00b_mmaps_remove(n00b_mmap_ctx_t *ctx, n00b_mmap_info_t *info)
{
    bool detached;

    mmap_write_lock(ctx);
    detached = n00b_mmaps_detach_base(ctx, info);
    mmap_write_unlock(ctx);

    if (detached) {
        n00b_mmap_free_info_record(ctx, info);
    }
}

// ============================================================================
// Lookup
// ============================================================================

[[n00b::nogc]] static inline bool
n00b_mmap_tree_needs_lock(void)
{
    return !n00b_atomic_load(&n00b_get_runtime()->stw_active);
}

[[n00b::nogc]] static mmap_node_t *
n00b_mmap_search_smallest_map(mmap_node_t *node,
                              uint64_t     start,
                              uint64_t     end,
                              uint64_t    *result_len)
{
    mmap_node_t *result = nullptr;

    if (node == nullptr || node->maximum <= start || node->minimum >= end) {
        return nullptr;
    }

    if (node->low < end && start < node->high
        && n00b_variant_is_type(node->data, n00b_mmap_info_t *)) {
        uint64_t len = node->high - node->low;

        if (len < *result_len) {
            result      = node;
            *result_len = len;
        }
    }

    mmap_node_t *left = n00b_mmap_search_smallest_map(node->left,
                                                       start,
                                                       end,
                                                       result_len);
    if (left != nullptr) {
        result = left;
    }
    mmap_node_t *right = n00b_mmap_search_smallest_map(node->right,
                                                        start,
                                                        end,
                                                        result_len);
    if (right != nullptr) {
        result = right;
    }

    return result;
}

// Point searches of the registry, counted while a test sets
// n00b_mmap_count_point_searches, so tests can bound how often a path
// consults it without every search paying for a shared counter.
_Atomic(bool)     n00b_mmap_count_point_searches = false;
_Atomic(uint64_t) n00b_mmap_point_searches       = 0;

[[n00b::nogc]] static mmap_node_t *
n00b_mmap_search_point(mmap_tree_t *tree, uint64_t start, uint64_t end)
{
    if (atomic_load_explicit(&n00b_mmap_count_point_searches, memory_order_relaxed)) {
        atomic_fetch_add_explicit(&n00b_mmap_point_searches, 1, memory_order_relaxed);
    }
    bool         locked     = n00b_mmap_tree_needs_lock();
    mmap_node_t *result     = nullptr;
    uint64_t     result_len = (uint64_t)-1;

    if (locked) {
        n00b_data_read_lock(tree->lock);
    }
    if (tree->root != nullptr) {
        result = n00b_mmap_search_smallest_map(tree->root,
                                               start,
                                               end,
                                               &result_len);
    }
    if (locked) {
        n00b_data_unlock(tree->lock);
    }

    return result;
}

[[n00b::nogc]] static n00b_option_t(n00b_mmap_info_t *)
n00b_mmap_lookup_unlocked(n00b_mmap_ctx_t *ctx, void *addr)
{
    uint64_t start = (uint64_t)addr;
    uint64_t end   = start + 1;

    if (end < start) {
        return n00b_option_none(n00b_mmap_info_t *);
    }

    mmap_node_t *node = n00b_mmap_search_point(ctx->mmap_tree, start, end);

    if (node != nullptr) {
        assert(n00b_variant_is_type(node->data, n00b_mmap_info_t *));
        return n00b_option_set(n00b_mmap_info_t *,
                               n00b_variant_get(node->data, n00b_mmap_info_t *));
    }
    return n00b_option_none(n00b_mmap_info_t *);
}

[[n00b::nogc]] static void
n00b_mmap_search_smallest_range(mmap_node_t          *node,
                                uint64_t              start,
                                uint64_t              end,
                                n00b_alloc_range_t  **result,
                                uint64_t             *result_len)
{
    if (node == nullptr || node->maximum <= start || node->minimum >= end) {
        return;
    }

    if (node->low < end && start < node->high) {
        if (n00b_variant_is_type(node->data, n00b_alloc_range_t *)) {
            uint64_t len = node->high - node->low;

            if (len < *result_len) {
                *result     = n00b_variant_get(node->data, n00b_alloc_range_t *);
                *result_len = len;
            }
        }
    }

    n00b_mmap_search_smallest_range(node->left, start, end, result, result_len);
    n00b_mmap_search_smallest_range(node->right, start, end, result, result_len);
}

// clang-format off
[[n00b::nogc]]
n00b_option_t(n00b_alloc_range_t *)
n00b_mmap_range_by_address(void *addr) _kargs
{
    n00b_runtime_t *runtime = n00b_get_runtime();
}
// clang-format on
{
    uint64_t start = (uint64_t)addr;
    uint64_t end   = start + 1;

    if (end < start) {
        return n00b_option_none(n00b_alloc_range_t *);
    }

    n00b_mmap_ctx_t   *ctx        = n00b_global_mem_map(runtime);
    mmap_tree_t        *tree       = ctx->range_tree;
    n00b_alloc_range_t *result     = nullptr;
    uint64_t            result_len = (uint64_t)-1;
    bool                locked     = n00b_mmap_tree_needs_lock();

    mmap_read_lock(ctx);
    if (locked) {
        n00b_data_read_lock(tree->lock);
    }
    if (tree->root != nullptr) {
        n00b_mmap_search_smallest_range(tree->root, start, end, &result, &result_len);
    }
    if (locked) {
        n00b_data_unlock(tree->lock);
    }
    mmap_read_unlock(ctx);

    if (result != nullptr) {
        return n00b_option_set(n00b_alloc_range_t *, result);
    }
    return n00b_option_none(n00b_alloc_range_t *);
}

#ifdef N00B_DEBUG
void (*n00b_mmap_debug_lookup_hook)(void *addr) = nullptr;
#endif

[[n00b::nogc]] static inline void
mmap_lookup_done(void *addr)
{
#ifdef N00B_DEBUG
    if (n00b_mmap_debug_lookup_hook != nullptr) {
        n00b_mmap_debug_lookup_hook(addr);
    }
#else
    (void)addr;
#endif
}

[[n00b::nogc]]
n00b_option_t(n00b_mmap_info_t *)
n00b_mmap_lookup(n00b_mmap_ctx_t *ctx, void *addr)
{
    n00b_option_t(n00b_mmap_info_t *) result;

    mmap_read_lock(ctx);
    result = n00b_mmap_lookup_unlocked(ctx, addr);
    mmap_read_unlock(ctx);
    mmap_lookup_done(addr);

    return result;
}

[[n00b::nogc]] static inline void
mmap_view_copy(n00b_mmap_info_t *info, n00b_mmap_view_t *out)
{
    out->start         = info->start;
    out->end           = info->end;
    out->allocator     = atomic_load_explicit(&info->allocator, memory_order_relaxed);
    out->max_alloc_len = atomic_load_explicit(&info->max_alloc_len, memory_order_relaxed);
    out->kind          = info->kind;
    out->perms         = info->perms;
}

[[n00b::nogc]] void
n00b_mmap_view_of(n00b_mmap_info_t *info, n00b_mmap_view_t *out)
{
    mmap_view_copy(info, out);
}

[[n00b::nogc]] bool
n00b_mmap_view_by_address(void *addr, n00b_mmap_view_t *out)
{
    n00b_mmap_ctx_t *ctx   = n00b_global_mem_map(n00b_get_runtime());
    bool             found = false;

    mmap_read_lock(ctx);
    auto opt = n00b_mmap_lookup_unlocked(ctx, addr);
    if (n00b_option_is_set(opt)) {
        mmap_view_copy(n00b_option_get(opt), out);
        found = true;
    }
    mmap_read_unlock(ctx);
    mmap_lookup_done(addr);

    return found;
}

// ============================================================================
// Initialization
// ============================================================================

// In memory_info.c
extern void n00b_load_static_ranges();

void
n00b_mmaps_initialize(n00b_mmap_ctx_t *ctx)
{
    *ctx = (n00b_mmap_ctx_t){0};
    n00b_spinlock_init(&ctx->lock);

    n00b_pool_init(&ctx->pool, .__system = true, .hidden = true, .name = "mmaps");

    n00b_allocator_t  *alloc = (n00b_allocator_t *)&ctx->pool;
    n00b_alloc_opts_t  aopts = { .allocator = alloc };

    ctx->mmap_tree = n00b_alloc_with_opts(mmap_tree_t, &aopts);
    n00b_interval_tree_init(ctx->mmap_tree, .allocator = alloc);
    ctx->range_tree = n00b_alloc_with_opts(mmap_tree_t, &aopts);
    n00b_interval_tree_init(ctx->range_tree, .allocator = alloc);

    n00b_load_static_ranges();
}

// ============================================================================
// Sub-range tracking
// ============================================================================

// Detach registry entries while holding registry/tree locks, then free
// metadata after unlocking. n00b_free() can re-enter mmap lookup.
static n00b_stack_t(void *)
n00b_mmap_detach_ranges(n00b_mmap_ctx_t *ctx, uint64_t start, uint64_t end)
{
    n00b_allocator_t    *alloc = (n00b_allocator_t *)&ctx->pool;
    n00b_stack_t(void *) hits  = n00b_stack_new_private(void *, .allocator = alloc);
    n00b_stack_t(void *) dead  = n00b_stack_new_private(void *, .allocator = alloc);

    (void)n00b_interval_search(ctx->range_tree, start, end, &hits);

    while (n00b_stack_len(hits) > 0) {
        mmap_node_t *node = n00b_option_get(n00b_stack_pop(void *, hits));

        // Only delete sub-range entries, leave mmap records alone.
        if (n00b_variant_is_type(node->data, n00b_alloc_range_t *)) {
            n00b_alloc_range_t *range = n00b_variant_get(node->data, n00b_alloc_range_t *);
            static_identity_unregister_range_unlocked(ctx, range);
            (void)n00b_interval_delete(ctx->range_tree, node);
            n00b_stack_push(dead, range);
        }
    }

    n00b_stack_free(hits);
    return dead;
}

static void
n00b_mmap_free_detached_ranges(n00b_mmap_ctx_t *ctx, n00b_stack_t(void *) *ranges)
{
    while (n00b_stack_len(*ranges) > 0) {
        void *range = n00b_option_get(n00b_stack_pop(void *, *ranges));
        n00b_mmap_free_registry_obj(ctx, range);
    }
}

void
n00b_mmap_delete_ranges(n00b_mmap_ctx_t *ctx, uint64_t start, uint64_t end)
{
    n00b_stack_t(void *) dead;

    mmap_write_lock(ctx);
    dead = n00b_mmap_detach_ranges(ctx, start, end);
    mmap_write_unlock(ctx);

    n00b_mmap_free_detached_ranges(ctx, &dead);
    n00b_stack_free(dead);
}

n00b_alloc_range_t *
n00b_mmap_register_range(void *startp, void *endp, n00b_mmap_rec_kind_t kind) _kargs
{
    n00b_allocator_t      *allocator = nullptr;
    const char            *file      = nullptr;
    n00b_gc_scan_kind_t    scan_kind = N00B_GC_SCAN_KIND_DEFAULT;
    n00b_gc_scan_cb_t      scan_cb   = nullptr;
    void                  *scan_user = nullptr;
    n00b_alloc_type_info_t tinfo     = 0;
    uint64_t               object_id = 0;
    const n00b_static_identity_t *identity = nullptr;
    uint32_t               flags     = 0;
}
{
    n00b_runtime_t  *rt  = n00b_get_runtime();
    n00b_mmap_ctx_t *ctx = n00b_global_mem_map(rt);

    n00b_allocator_t   *alloc = (n00b_allocator_t *)&ctx->pool;
    n00b_alloc_range_t *range = n00b_alloc_with_opts(n00b_alloc_range_t,
                                                      &(n00b_alloc_opts_t){.allocator = alloc});
    uint64_t start = (uint64_t)startp;
    uint64_t end   = (uint64_t)endp;

    assert(end > start);

    *range = (n00b_alloc_range_t){
        .start     = startp,
        .tinfo     = tinfo,
        .scan_cb   = scan_cb,
        .scan_user = scan_user,
        .allocator = allocator,
        .file      = file,
        .identity  = identity,
        .object_id = object_id,
        .len       = end - start,
        .kind      = kind,
        .scan_kind = scan_kind,
        .flags     = flags,
    };

    n00b_mmap_data_t data = n00b_variant_set(n00b_mmap_data_t, n00b_alloc_range_t *, range);

    mmap_write_lock(ctx);
    auto range_r = n00b_interval_insert(ctx->range_tree, start, end, data);
    assert(n00b_result_is_ok(range_r));
    range->tree_node = n00b_result_get(range_r);
    if (identity != nullptr) {
        (void)static_identity_register_unlocked(ctx, identity, range);
    }
    mmap_write_unlock(ctx);

    return range;
}

// clang-format off
n00b_alloc_range_t *
_n00b_static_object_register(void *startp,
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
}
// clang-format on
{
    assert(startp);
    assert(len > 0);

    char *endp = (char *)startp + len;
    assert(endp > (char *)startp);

    char *cursor = (char *)startp;
    while (cursor < endp) {
        auto map_opt = n00b_mmap_by_address(cursor);
        if (!n00b_option_is_set(map_opt)) {
            return nullptr;
        }

        n00b_mmap_info_t *map = n00b_option_get(map_opt);
        if (map->kind != n00b_mmap_static || map->end <= (uint64_t)cursor) {
            return nullptr;
        }

        cursor = (char *)map->end;
    }

    n00b_alloc_range_t *range =
        n00b_mmap_register_range(startp,
                                 endp,
                                 n00b_mmap_static,
                                 .file      = loc,
                                 .tinfo     = tinfo,
                                 .scan_kind = scan_kind,
                                 .scan_cb   = scan_cb,
                                 .scan_user = scan_user,
                                 .object_id = object_id,
                                 .identity  = identity,
                                 .flags     = flags);
    return range;
}

// ============================================================================
// Registration / unregistration
// ============================================================================

// clang-format off
n00b_option_t(n00b_mmap_info_t *)
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
}
// clang-format on
{
#if !defined(N00B_DEBUG)
    (void)source_file;
    (void)source_line;
#endif
    if (allocator && allocator->hidden) {
        return n00b_option_none(n00b_mmap_info_t *);
    }

    n00b_mmap_info_t *result;
    n00b_mmap_ctx_t  *ctx   = n00b_global_mem_map(runtime);
    uint64_t          start = (uint64_t)startp;
    uint64_t          end   = (uint64_t)endp;
    uint64_t          blen  = end - start;

    assert(ctx);
    assert(end > start);

    mmap_write_lock(ctx);

    if (!definitely_unique) {
        auto existing = n00b_mmap_lookup_unlocked(ctx, startp);
        if (n00b_option_is_set(existing)) {
            result = n00b_option_get(existing);
            assert(allocator == result->allocator || !allocator);
            if (result->perms == n00b_mmap_perms_unknown
                && perms != n00b_mmap_perms_unknown) {
                result->perms = perms;
            }
#if defined(N00B_DEBUG)
            if (result->source_file == nullptr && source_file != nullptr) {
                result->source_file = source_file;
                result->source_line = source_line;
            }
#endif
            mmap_write_unlock(ctx);
            return n00b_option_set(n00b_mmap_info_t *, result);
        }
    }

    result            = mmaps_insert_raw(ctx, startp, blen, kind, binary_offset, perms);
    result->allocator = allocator;
    result->file      = file;
    result->slide     = slide;
    result->order_id  = order_id;
#if defined(N00B_DEBUG)
    result->source_file = source_file;
    result->source_line = source_line;
#endif

    mmap_write_unlock(ctx);

    return n00b_option_set(n00b_mmap_info_t *, result);
}

/* ------------------------------------------------------------------------
 * Bounded cache for kernel-probed foreign pages (n00b#213)
 * ------------------------------------------------------------------------
 *
 * n00b_check_kernel_page_map caches its answer by registering the probed page
 * in the global registry. Every OTHER registration in the process has a
 * matching unregister on the path that unmaps the memory; this one has none,
 * because nothing in n00b owns the mapping. So a process that keeps asking
 * about foreign addresses -- libc-malloc'd buffers, mapped files, a
 * non-n00b thread's stack -- accumulates one permanent single-page record per
 * distinct page, forever. That is the unbounded registry growth n00b#213 is
 * about, and it compounds: every registry search in the process gets deeper as
 * the tree fills up, and the conservative scan's search is already the
 * heaviest leaf in a wedged collector.
 *
 * The cache is kept, and bounded. A fixed ring of records is reused in FIFO
 * order: reaching the cap recycles the oldest record (detach from the tree,
 * rewrite it in place, re-insert) instead of allocating another one. Growth
 * from this path is therefore O(N00B_MMAP_PROBE_CACHE_ENTRIES) records for the
 * life of the process, whatever the probe volume.
 *
 * Records are RECYCLED rather than freed on eviction, deliberately. A caller
 * that has already been handed a record is not holding a lock on it, so
 * freeing an evicted record would be a use-after-free on any racing reader.
 * Rewriting it in place leaves the pointer valid; the worst a racing reader
 * can see is an answer about a different foreign page, which is a stale
 * answer to an advisory question -- these records exist only to say "something
 * is mapped here", carry no allocator, and are never walked by the GC.
 */
static n00b_mmap_info_t *probe_cache[N00B_MMAP_PROBE_CACHE_ENTRIES];
static uint64_t          probe_cache_cursor = 0;
_Atomic(uint64_t)        n00b_mmap_probe_evictions = 0;

n00b_option_t(n00b_mmap_info_t *)
n00b_mmap_register_probe_page(void                *startp,
                              void                *endp,
                              n00b_mmap_rec_kind_t kind,
                              n00b_mmap_perms_t    perms)
{
    n00b_runtime_t   *runtime = n00b_get_runtime();
    n00b_mmap_ctx_t  *ctx     = n00b_global_mem_map(runtime);
    uint64_t          start   = (uint64_t)startp;
    uint64_t          end     = (uint64_t)endp;
    n00b_mmap_info_t *result;

    assert(ctx);
    assert(end > start);

    mmap_write_lock(ctx);

    /* Another thread may have registered this page (or a mapping covering it)
     * while we were probing; don't add a duplicate. */
    auto existing = n00b_mmap_lookup_unlocked(ctx, startp);

    if (n00b_option_is_set(existing)) {
        result = n00b_option_get(existing);
        if (result->perms == n00b_mmap_perms_unknown
            && perms != n00b_mmap_perms_unknown) {
            result->perms = perms;
        }
        mmap_write_unlock(ctx);
        return n00b_option_set(n00b_mmap_info_t *, result);
    }

    uint64_t slot = probe_cache_cursor % N00B_MMAP_PROBE_CACHE_ENTRIES;
    result        = probe_cache[slot];

    if (result == nullptr) {
        result = mmaps_insert_raw(ctx, startp, end - start, kind, 0, perms);
    }
    else {
        (void)n00b_interval_delete(ctx->mmap_tree,
                                   (mmap_node_t *)result->tree_node);

        result->start         = start;
        result->end           = end;
        result->kind          = kind;
        result->perms         = perms;
        result->binary_offset = 0;
        result->slide         = 0;
        result->order_id      = 0;
        result->file          = nullptr;
        result->source_file   = nullptr;
        result->source_line   = 0;
        n00b_atomic_store(&result->allocator, nullptr);
        atomic_store(&result->max_alloc_len, 0);

        n00b_mmap_data_t data
            = n00b_variant_set(n00b_mmap_data_t, n00b_mmap_info_t *, result);
        auto insert_r = n00b_interval_insert(ctx->mmap_tree, start, end, data);
        assert(n00b_result_is_ok(insert_r));
        result->tree_node = n00b_result_get(insert_r);

        atomic_fetch_add(&n00b_mmap_probe_evictions, 1);
    }

    probe_cache[slot]  = result;
    probe_cache_cursor = probe_cache_cursor + 1;

    mmap_write_unlock(ctx);

    return n00b_option_set(n00b_mmap_info_t *, result);
}

/* Narrow internal entry point for n00b_pool_t. n00b_mmap_register
 * deliberately skips hidden allocators (so the GC can't discover
 * their pages); that's correct for callers that want hidden
 * semantics end-to-end, but it leaves n00b_free unable to resolve a
 * hidden-pool pointer back to its allocator. Pool clients that need
 * n00b_free to route to pool_free (i.e. hidden+metadata pools like
 * @c rt->user_pool) call this from new_page_entry to put the page
 * in the mmap tree unconditionally, while keeping every other
 * hidden allocator opaque to the tree.
 *
 * Kind is fixed at @c n00b_mmap_pool. The GC-scan path's hidden
 * filter in @ref n00b_mmap_is_gc_scannable still skips these pages
 * (the allocator pointer carries the hidden flag), so registering
 * here doesn't expose them to GC scanning — it only enables
 * @ref n00b_mem_get_allocator to find the owning allocator. */
n00b_option_t(n00b_mmap_info_t *)
_n00b_mmap_register_pool_page(void *startp,
                               void *endp,
                               n00b_allocator_t *allocator,
                               const char *file,
                               const char *source_file,
                               uint32_t source_line)
{
#if !defined(N00B_DEBUG)
    (void)source_file;
    (void)source_line;
#endif
    n00b_runtime_t   *runtime = n00b_get_runtime();
    n00b_mmap_info_t *result;
    n00b_mmap_ctx_t  *ctx   = n00b_global_mem_map(runtime);
    uint64_t          start = (uint64_t)startp;
    uint64_t          end   = (uint64_t)endp;
    uint64_t          blen  = end - start;

    assert(ctx);
    assert(end > start);

    mmap_write_lock(ctx);
    /* A pool page is an anonymous PROT_READ|PROT_WRITE mapping for as long as
     * it is registered: new_page_entry registers right after n00b_mmap, and
     * delete_one_page_entry / pool_destroy unregister BEFORE any munmap or
     * quarantine mprotect(PROT_NONE). Recording the perms here lets
     * n00b_check_memory_perms answer from the registry. With perms_unknown
     * every probe into a pool page fell through to the pipe write/poll
     * syscalls (plus signal() on macOS), once per candidate pointer in the
     * mark loop, which is n00b#275's 84%-in-syscalls profile on a process
     * whose objects live in user_pool. */
    result = mmaps_insert_raw(ctx,
                               startp,
                               blen,
                               n00b_mmap_pool,
                               0,
                               n00b_mmap_perms_rw);
    result->allocator = allocator;
    result->file      = file;
#if defined(N00B_DEBUG)
    result->source_file = source_file;
    result->source_line = source_line;
#endif
    mmap_write_unlock(ctx);

    return n00b_option_set(n00b_mmap_info_t *, result);
}

// clang-format off
n00b_result_t(void *)
_n00b_mmap(size_t sz, char *loc) _kargs
{
    n00b_allocator_t    *allocator     = nullptr;
    n00b_mmap_rec_kind_t kind          = n00b_mmap_api_mmap;
    char                *name          = nullptr;
    bool                 skip_register = false;
}
// clang-format on
{
    char *source_loc = loc;
    if (name) {
        loc = name;
    }

    if (allocator) {
        sz = n00b_page_align(sz);
    }
    else {
        assert(kind == n00b_mmap_api_mmap || kind == n00b_mmap_arena);

        if (kind == n00b_mmap_arena) {
            sz = n00b_page_size;
        }
        else {
            sz = n00b_page_align(sz);
        }
    }

    auto mmap_r = n00b_check_mmap(nullptr, sz, N00B_MPROT, N00B_MFLAG, -1, 0);

    if (n00b_result_is_err(mmap_r)) {
        return n00b_result_err(void *, n00b_result_get_err(mmap_r));
    }

    void *result = n00b_result_get(mmap_r);

    /* Skip the implicit register entirely when the caller asks —
     * pool.c does this so it can drive the register/unregister
     * pair itself (so the unregister happens BEFORE munmap and
     * concurrent GC mark passes never see a stale tree entry
     * pointing into a no-longer-mapped page). */
    if (skip_register) {
        atomic_fetch_add(&n00b_mmap_skip_register_count, 1);
        atomic_fetch_add(&n00b_mmap_skip_register_bytes, (uint64_t)sz);
    }
    else {
        (void)_n00b_mmap_register(result,
                                  ((char *)result) + sz,
                                  kind,
                                  .file        = loc,
                                  .source_file = source_loc,
                                  .source_line = 0,
                                  .allocator   = allocator,
                                  .perms       = n00b_mmap_perms_rw);
    }

    return n00b_result_ok(void *, result);
}

// clang-format off
n00b_result_t(int)
n00b_munmap(void *addr) _kargs
{
    n00b_runtime_t *runtime = n00b_get_runtime();
}
// clang-format on
{
    n00b_mmap_ctx_t     *ctx       = n00b_global_mem_map(runtime);
    n00b_mmap_info_t    *map       = nullptr;
    n00b_stack_t(void *) dead      = {};
    void                *start     = nullptr;
    size_t               len       = 0;
    bool                 detached = false;

    mmap_write_lock(ctx);
    auto map_opt = n00b_mmap_lookup_unlocked(ctx, addr);

    if (!n00b_option_is_set(map_opt)) {
        mmap_write_unlock(ctx);
        return n00b_result_err(int, EINVAL);
    }

    map      = n00b_option_get(map_opt);
    start    = (void *)map->start;
    len      = map->end - map->start;
    dead     = n00b_mmap_detach_ranges(ctx, map->start, map->end);
    detached = n00b_mmaps_detach_base(ctx, map);
    mmap_write_unlock(ctx);

    n00b_mmap_free_detached_ranges(ctx, &dead);
    if (detached) {
        n00b_mmap_free_info_record(ctx, map);
    }

#ifdef _WIN32
    if (!n00b_win_free_range(start, len)) {
        n00b_atomic_add(&n00b_munmap_fail_count, 1);
    }
#else
    munmap(start, len);
#endif
    return n00b_result_ok(int, 0);
}

// clang-format off
void
n00b_mmap_unregister(void *start) _kargs
{
    n00b_runtime_t *runtime = n00b_get_runtime();
}
// clang-format on
{
    n00b_mmap_ctx_t     *ctx       = n00b_global_mem_map(runtime);
    n00b_mmap_info_t    *map       = nullptr;
    n00b_stack_t(void *) dead      = {};
    bool                 detached = false;

    mmap_write_lock(ctx);
    auto map_opt = n00b_mmap_lookup_unlocked(ctx, start);

    if (!n00b_option_is_set(map_opt)) {
        mmap_write_unlock(ctx);
        return;
    }

    map      = n00b_option_get(map_opt);
    dead     = n00b_mmap_detach_ranges(ctx, map->start, map->end);
    detached = n00b_mmaps_detach_base(ctx, map);
    mmap_write_unlock(ctx);

    n00b_mmap_free_detached_ranges(ctx, &dead);
    if (detached) {
        n00b_mmap_free_info_record(ctx, map);
    }
}

// clang-format off
[[n00b::nogc]]
n00b_option_t(n00b_mmap_info_t *)
n00b_mmap_by_address(void *addr) _kargs
{
    n00b_runtime_t *runtime = n00b_get_runtime();
}
// clang-format on
{
    return n00b_mmap_lookup(n00b_global_mem_map(runtime), addr);
}

[[n00b::nogc]]
n00b_allocator_opt_t
n00b_mem_get_allocator(void *addr) _kargs
{
    n00b_runtime_t *runtime = n00b_get_runtime();
}
{
    n00b_mmap_ctx_t  *ctx       = n00b_global_mem_map(runtime);
    n00b_allocator_t *allocator = nullptr;

    mmap_read_lock(ctx);
    auto mmap_opt = n00b_mmap_lookup_unlocked(ctx, addr);
    if (n00b_option_is_set(mmap_opt)) {
        allocator = n00b_atomic_load(&n00b_option_get(mmap_opt)->allocator);
    }
    mmap_read_unlock(ctx);
    mmap_lookup_done(addr);

    if (allocator == nullptr) {
        return n00b_option_none(n00b_allocator_t *);
    }
    return n00b_option_set(n00b_allocator_t *, allocator);
}

// ============================================================================
// Debug printing
// ============================================================================

void
n00b_print_mmap_tree(void)
{
    n00b_runtime_t  *rt  = n00b_get_runtime();
    n00b_mmap_ctx_t *ctx = n00b_global_mem_map(rt);

    n00b_allocator_t *alloc = (n00b_allocator_t *)&ctx->pool;
    n00b_stack_t(void *) results = n00b_stack_new(void *, .allocator = alloc);

    (void)n00b_interval_search_ordered(ctx->mmap_tree, 0, (uint64_t)-1, &results);

    for (size_t i = 0; i < n00b_stack_len(results); i++) {
        mmap_node_t      *node = results.data[i];
        n00b_mmap_data_t  data = node->data;

        // Skip sub-range entries in the debug dump.
        if (!n00b_variant_is_type(data, n00b_mmap_info_t *)) {
            continue;
        }

        n00b_mmap_info_t *info = n00b_variant_get(data, n00b_mmap_info_t *);
        char             *name;

        switch (info->kind) {
        case n00b_mmap_static:
            name = "Static  ";
            break;
        case n00b_mmap_arena:
            name = "Arena   ";
            break;
        case n00b_mmap_managed_segment:
            name = "Managed ";
            break;
        case n00b_mmap_sys_segment:
            name = "System  ";
            break;
        case n00b_mmap_zero_page:
            name = "No-map  ";
            break;
        case n00b_mmap_unmanaged:
            name = "mmap()  ";
            break;
        case n00b_mmap_stack:
            name = "\e[31;1;4mStack   \e[0m";
            break;
        case n00b_mmap_internal:
            name = "Internal";
            break;
        case n00b_mmap_pool:
            name = "Pool    ";
            break;
        case n00b_mmap_api_mmap:
            name = "Direct  ";
            break;
        default:
            name = "Unknown ";
            break;
        }

        n00b_fprintf(stderr,
                     "%s %p-%p %s source=%s:%u\n",
                     name,
                     (void *)info->start,
                     (void *)info->end,
                     info->file ? info->file : "(no file)",
                     info->source_file ? info->source_file : "(no source)",
                     info->source_line);
    }
}
