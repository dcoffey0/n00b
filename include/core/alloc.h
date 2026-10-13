/**
 * @file alloc.h
 * @brief Memory allocation interface.
 *
 * Provides the allocator vtable structure, raw allocation/free entry
 * points, allocation info lookup, allocator setup, and convenience
 * macros (n00b_alloc, n00b_alloc_array, n00b_alloc_flex).
 */
#pragma once

#include "n00b.h"
#include "core/alloc_base.h"
#include "core/alloc_mdata.h"
#include "core/mmaps.h"
#include "core/macros.h"
#include "core/align.h"
#include "core/atomic.h"
#include "core/rt_access.h"
#include "core/vargs.h"

struct n00b_allocator_t {
    n00b_calloc_fn            zero_alloc;
    n00b_free_fn              free;
    n00b_allocator_pre_destroy_fn pre_destroy;
    n00b_allocator_destroy_fn destroy;
    const char               *debug_name;
    uint8_t                   add_inline_header : 1;
    uint8_t                   __system          : 1; // no STW check
    uint8_t                   hidden            : 1; // GC-invisible; see below.
    uint8_t                   is_metadata       : 1; // OOB-metadata md_pool arena.
    uint8_t                   use_epochs        : 1;
    uint8_t                   libc_backing      : 1; // see n00b_pool_init.
    n00b_allocator_t         *metadata_pool;
    _n00b_dict_internal_t    *metadata;
    // Allocator-specific OOB flex-tail size; MUST mirror n00b_base_allocator_t
    // (these two structs share a layout prefix and are cast to each other).
    uint32_t                  oob_extra_size;
    // "file:line" of the create-site. MUST mirror n00b_base_allocator_t (these
    // two structs share a layout prefix and are cast to each other).
    const char               *creation_loc;
    // MUST mirror n00b_base_allocator_t.oob_finalizers.
    _Atomic bool              oob_finalizers;
    void                     *opaque[];
};

typedef uint64_t (*n00b_obj_size_helper)(void *);

/** @brief Magic guard word placed before every managed allocation. */
extern uint64_t n00b_gc_guard;

// High-water mark of the largest inline-header allocation this process has
// made, in bytes. Bounds the conservative interior-pointer back-scan: no live
// allocation is larger than this, so a candidate with no guard within it is
// not a pointer into a live object. See _find_sentinal (n00b#321).
extern _Atomic(uint64_t) n00b_max_inline_alloc_len;

/**
 * @brief How much work the conservative backward guard scan has done.
 *
 * `n00b_sentinel_scan_calls` counts interior-pointer resolutions that ran the
 * backward walk; `n00b_sentinel_scan_words` counts the words those walks
 * actually stepped over. The ratio is the average distance one resolution
 * pays, which is the quantity n00b#275 and n00b#395 are about -- and the one
 * that used to be knowable only from a spindump of a wedged process. Counted
 * once per call, not per word, so reading them costs the scan nothing.
 */
extern _Atomic(uint64_t) n00b_sentinel_scan_calls;
extern _Atomic(uint64_t) n00b_sentinel_scan_words;

/**
 * @brief Check whether the GC should scan a mapped region.
 *
 * Returns false for allocator-internal memory that must not be
 * traced during garbage collection.  Hidden allocators' pages are
 * the primary case: they are never registered in the mmap tree
 * (so the GC normally cannot find them), but this function provides
 * a defence-in-depth check for any path that has an mmap record.
 *
 * This is the canonical GC-visibility gate for the allocator
 * abstraction.  Allocator authors only need to set `hidden = true`
 * in their allocator setup; the rest is automatic.
 *
 * @param map Mmap info to check (must not be nullptr).
 * @return    true if the region should be scanned by the GC.
 */
[[n00b::nogc]] static inline bool
n00b_mmap_is_gc_scannable(n00b_mmap_info_t *map)
{
    if (map->allocator && map->allocator->hidden) {
        return false;
    }
    return true;
}

/**
 * @brief Get the current thread's scoped allocator override.
 * @return Current override, or nullptr when this thread uses the runtime default.
 *
 * The returned allocator is only a fallback for APIs whose allocator
 * argument is nullptr.  Explicit `.allocator` kwargs always take
 * precedence over this per-thread override.
 *
 * The override now lives in `n00b_thread_t::current_allocator` reached
 * via `n00b_thread_self()` (D-005), not a `thread_local`.  Before the
 * runtime / calling thread is registered it returns nullptr (the
 * runtime default is used).
 */
extern n00b_allocator_t *n00b_current_allocator(void);

// MEASUREMENT (opt-in via -DN00B_GC_ATTRIB): GC-default-arena byte attribution
// (consumer = inside rocs ingest, vs other). Observational; no allocation
// redirection. When the flag is off the whole API compiles to empty inlines so
// ingest call sites stay zero-cost without #if guards of their own.
#if defined(N00B_GC_ATTRIB)
extern bool     n00b_gc_attrib_enter_ingest(void);
extern void     n00b_gc_attrib_exit_ingest(bool prev);
extern uint64_t n00b_gc_attrib_ingest_bytes(void);
extern uint64_t n00b_gc_attrib_other_bytes(void);
#else
static inline bool
n00b_gc_attrib_enter_ingest(void)
{
    return false;
}
static inline void
n00b_gc_attrib_exit_ingest(bool prev)
{
    (void)prev;
}
static inline uint64_t
n00b_gc_attrib_ingest_bytes(void)
{
    return 0;
}
static inline uint64_t
n00b_gc_attrib_other_bytes(void)
{
    return 0;
}
#endif

/**
 * @brief Install a current allocator override for this thread.
 * @param allocator Allocator to use for implicit allocations, or nullptr.
 * @return The previous thread-local allocator override.
 *
 * Use this low-level setter only when the previous value is restored on
 * every exit path.  The scoped helpers below are preferred for normal use.
 */
extern n00b_allocator_t *n00b_set_current_allocator(n00b_allocator_t *allocator);

/**
 * @brief Alias for @ref n00b_set_current_allocator.
 * @param allocator Allocator to install for this thread.
 * @return The previous thread-local allocator override.
 */
static inline n00b_allocator_t *
n00b_push_current_allocator(n00b_allocator_t *allocator)
{
    return n00b_set_current_allocator(allocator);
}

/**
 * @brief Restore a prior thread-local allocator override.
 * @param previous Value returned by @ref n00b_set_current_allocator.
 */
extern void n00b_restore_current_allocator(n00b_allocator_t *previous);

/**
 * @brief Guard object for scoped current-allocator overrides.
 *
 * Prefer `n00b_with_allocator(allocator) { ... }` or a local variable
 * annotated with `[[gnu::cleanup(n00b_allocator_scope_exit)]]`.
 */
typedef struct {
    n00b_allocator_t *previous;
    n00b_allocator_t *installed;
    bool              active;
    bool              run;
} n00b_allocator_scope_t;

/**
 * @brief Enter a scoped current-allocator override.
 * @param allocator Allocator to use for implicit allocations in this thread.
 * @return Guard that restores the previous override when exited.
 *
 * @pre @p allocator must outlive every allocation made from it.
 * @post Implicit allocations in this thread use @p allocator until restored.
 */
extern n00b_allocator_scope_t n00b_allocator_scope_enter(n00b_allocator_t *allocator);

/**
 * @brief Exit a scoped current-allocator override.
 * @param scope Guard returned by @ref n00b_allocator_scope_enter.
 *
 * This function is cleanup-safe and idempotent for inactive scopes.  In
 * N00B_DEBUG builds it asserts that the allocator the scope installed is
 * still current, so an override set inside the scope and never restored
 * fails here instead of being overwritten.
 */
extern void n00b_allocator_scope_exit(n00b_allocator_scope_t *scope);

#define _N00B_WITH_ALLOCATOR(_scope_name, _allocator)                                          \
    for ([[gnu::cleanup(n00b_allocator_scope_exit)]]                                           \
         n00b_allocator_scope_t _scope_name                                                    \
         = n00b_allocator_scope_enter((_allocator));                                           \
         _scope_name.run;                                                                      \
         _scope_name.run = false)

/**
 * @brief Run a block with a thread-local allocator fallback.
 *
 * Example:
 *
 * ```c
 * n00b_with_allocator((n00b_allocator_t *)scratch) {
 *     n00b_string_t *tmp = n00b_cformat("frame [|#|]", frame_name);
 * }
 * ```
 *
 * Only allocations that would otherwise use the runtime default are
 * redirected.  Explicit `.allocator` kwargs still win.  Objects allocated
 * in the scope must not escape unless the scoped allocator outlives them;
 * restore the prior allocator before resetting or destroying scratch
 * arenas/pools.
 */
#define n00b_with_allocator(_allocator)                                                        \
    _N00B_WITH_ALLOCATOR(N00B_CONCAT(_bl_allocator_scope_, __COUNTER__), _allocator)

#define n00b_ensure_allocator(allocator_var)                                                   \
    if (!(allocator_var)) {                                                                    \
        (allocator_var) = n00b_current_allocator();                                            \
        if (!(allocator_var)) {                                                                \
            (allocator_var) = n00b_atomic_load(&n00b_get_runtime()->default_allocator);        \
        }                                                                                      \
        assert(allocator_var);                                                                 \
    }

/*
 * allocator      Allocator to use (nullptr = runtime default).
 * no_scan        If true, GC will not scan this allocation for pointers.
 *                Legacy switch; superseded by scan_kind when scan_kind
 *                != N00B_GC_SCAN_KIND_DEFAULT.
 * mem_debug      Enable memory debugging for this allocation.
 * debug_taint    Taint freed memory with a debug pattern.
 * finalizer      Finalizer callback to run when the object is collected
 *                    or freed. Registered at allocation time, avoiding the
 *                    header lookup that n00b_add_finalizer() requires.
 * finalizer_data Opaque pointer passed to @p finalizer when invoked.
 * scan_kind      Per-allocation GC scan shape (see core/gc_map.h).
 *                DEFAULT (0) falls back to the no_scan switch above.
 * scan_cb        Callback invoked by the GC when scan_kind == CALLBACK.
 *                Requires an allocator with OOB metadata; non-OOB allocators
 *                fall back to DEFAULT scanning.
 * scan_user      Opaque pointer passed to scan_cb.
 */

typedef struct {
    n00b_allocator_t   *allocator;
    bool                no_scan;
    bool                mem_debug;
    bool                debug_taint;
    n00b_finalizer_t    finalizer;
    void               *finalizer_data;
    n00b_gc_scan_kind_t scan_kind;
    n00b_gc_scan_cb_t   scan_cb;
    void               *scan_user;
    // Optional per-allocation audit-site override ("file:line"). When non-null,
    // the per-site pool audit and the OOB record's file_name attribute this
    // allocation to it instead of the
    // innermost N00B_LOC_STRING() capture, so a caller (or a _*_at wrapper that
    // threads its alloc_location down) can trace the allocation back to the real
    // instantiation site rather than the primitive in string.c/buffer.c. Null
    // preserves the default (innermost) attribution.
    const char         *alloc_site;
} n00b_alloc_opts_t;

extern const n00b_alloc_opts_t _n00b_default_alloc_opts;

/**
 * @brief Low-level allocation.  Prefer the n00b_alloc() macro family.
 * @param n         Number of elements.
 * @param sz        Size of each element in bytes.
 * @param type_hash typehash(T) for the allocated type (0 = unknown).
 * @param location  Source location string (auto-filled by macro).
 * *
 * @pre Runtime must be initialized (or an explicit allocator must be provided).
 * @post Returned pointer is zero-filled and aligned to `N00B_ALIGN`.
 */
extern void *_n00b_alloc_raw(size_t             n,
                             size_t             sz,
                             uint64_t           type_hash,
                             const char        *location,
                             n00b_alloc_opts_t *opts,
                             +) _kargs : opaque;

/**
 * @brief Free a managed allocation.
 * @param ptr Pointer returned by a prior n00b_alloc call.
 * @pre @p ptr was returned by an n00b_alloc family macro, or is nullptr.
 *
 * @kw allocator Optional owning allocator. When null (default), ownership is
 *   discovered via the global mmap interval-tree (n00b_mem_get_allocator) and
 *   object finalizers run — the general path. When the caller already knows the
 *   producing allocator, pass it to SHORT-CIRCUIT the interval-tree search
 *   entirely (the hot path: freeing scratch/arena/pool temporaries in bulk).
 *   Like n00b_free_from_allocator, the hinted path does NOT run finalizers,
 *   does NOT re-derive the allocator, and does NOT no-op baked/comptime-image
 *   pointers (the bare path does), so only pass @c .allocator for owner-known
 *   raw heap storage with no finalizer that is NEVER a read-only static/baked
 *   pointer — passing a baked pointer would corrupt a static page (asserted in
 *   debug builds). Works for both discoverable (pool/arena) and undiscoverable
 *   (hidden-pool) storage — it subsumes n00b_free_with_allocator_hint.
 */
extern void n00b_free(void *ptr) _kargs {
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Return storage through a known allocator.
 * @param allocator Allocator that produced @p ptr.
 * @param ptr       Pointer returned by @p allocator.
 *
 * This is lower-level than @ref n00b_free: it does not discover ownership and
 * it does not run object finalizers. It exists only for allocator-owned
 * internal storage whose owner is intentionally not visible through
 * @ref n00b_mem_get_allocator, such as hidden/system pools whose pages are
 * absent from the global mmap lookup tree.
 */
extern void n00b_free_from_allocator(n00b_allocator_t *allocator, void *ptr);

/**
 * @brief Free storage, using @p allocator only when ownership is undiscoverable.
 * @param allocator Allocator that produced @p ptr when @p ptr is hidden from
 *                  the global mmap lookup tree.
 * @param ptr       Pointer returned by an n00b allocation API, or nullptr.
 *
 * Discoverable allocations take the normal @ref n00b_free path, including
 * finalizers. Undiscoverable hidden-pool storage is returned directly through
 * @p allocator. This is for allocator-owned conduit/runtime storage where the
 * caller already knows the producing allocator.
 */
extern void n00b_free_with_allocator_hint(n00b_allocator_t *allocator, void *ptr);

/**
 * @brief Tear down an allocator, releasing all its resources.
 * @param allocator Allocator to destroy.
 * @pre No outstanding allocations should be in use from @p allocator.
 * @post @p allocator is invalid and must not be used.
 */
extern void n00b_allocator_destroy(n00b_allocator_t *allocator);

/**
 * @brief STW-only: tear down every allocator parked in the deferred-destroy
 *        queue. Must run AFTER n00b_epoch_flush_all_stw in the same stop —
 *        the flush frees parked retire nodes THROUGH their allocators, so
 *        the pools must still be alive when it runs.
 */
extern void n00b_allocator_run_deferred_destroys(n00b_runtime_t *rt);

/**
 * @brief Mutator-callable full barrier: stop the world, flush every epoch
 *        retire list, tear down all parked allocators, restart. Used on
 *        deferral-queue overflow and by tests/shutdown paths that need
 *        deferred destruction to be observable NOW.
 */
extern void n00b_allocator_deferred_destroy_barrier(void);

/**
 * @brief Rebuild an allocator's OOB metadata into a fresh metadata arena.
 *
 * Allocators with out-of-band metadata keep the active metadata dict and OOB
 * records in @c allocator->metadata_pool.  That backing store is arena-based,
 * so individual OOB records are not reclaimed with @ref n00b_free.  This
 * routine copies currently-alive metadata records into a new attached metadata
 * arena, swaps the allocator link, and destroys the old metadata arena whole.
 *
 * If the world is not already stopped, this function takes the runtime's
 * critical-execution write gate around the rebuild/swap so no mutator can be
 * inside the allocator's metadata dict. It does not suspend threads itself.
 */
extern void n00b_allocator_compact_metadata(n00b_allocator_t *allocator);

/**
 * @brief Look up allocation metadata for an address.
 * @param addr   Address to look up.
 * @param result Output structure to fill.
 *
 * @kw allocator       Allocator to search (nullptr = search all).
 * @kw scan_for_header If true, scan backward for an inline header.
 * @kw pin             Set to true when the lookup returns an OOB header that
 *                     a collection or metadata compaction could reclaim. The
 *                     lookup then keeps stop-the-world held off until the
 *                     caller passes the flag to @ref n00b_alloc_hdr_unpin.
 *
 * Such a header is valid only while stop-the-world is held off, so a caller
 * that reads or writes through it must pass @c .pin, or already hold the STW
 * gate. A collection waits for the pin, so keep the span short. In
 * N00B_DEBUG builds a lookup that returns such a header unpinned aborts.
 */
[[n00b::nogc]] extern void
_n00b_find_alloc_info(void *addr, n00b_alloc_info_t *result) _kargs
{
    n00b_allocator_t *allocator       = nullptr;
    bool              scan_for_header = false;
    bool             *pin             = nullptr;
};

// Get this in the caller's frame.
#define n00b_find_alloc_info(addr, ...)                                                        \
    ({                                                                                         \
        n00b_alloc_info_t _info;                                                               \
        _n00b_find_alloc_info((addr), &_info __VA_OPT__(, __VA_ARGS__));                       \
        _info;                                                                                 \
    })

/** @brief Release the pin a lookup took for its @c .pin flag. */
extern void n00b_alloc_hdr_unpin(bool pinned);

#if defined(N00B_DEBUG)
/**
 * @brief Test-only hook, called when a lookup returns an OOB header that needs
 *        a pin, after the lookup releases its own hold on the STW gate.
 *
 * Null outside tests. A test parks the caller here to race a collection
 * against the caller's use of the header.
 */
extern void (*n00b_alloc_hdr_lookup_hook)(void *addr);
#endif

// Fast-path resolver: resolve `addr` via a KNOWN external-metadata allocator's
// OOB index, skipping the global mmap interval-tree search.  Returns
// kind=n00b_alloc_oob on hit, kind=n00b_alloc_none on miss (fall back to
// n00b_find_alloc_info).  A hit sets *pin as n00b_find_alloc_info's .pin does.
// See the definition for constraints.
extern n00b_alloc_info_t
n00b_try_alloc_info_in_allocator(void *addr, n00b_allocator_t *al, bool *pin);

/**
 * @brief Configure an allocator's vtable and options.
 * @param allocator Allocator to set up.
 * @param alloc     Zero-fill allocation function.
 *
 * @kw free              Free function (nullptr = no-op).
 * @kw pre_destroy       Optional allocator-specific quiesce/finalize hook.
 * @kw destroy           Allocator destroy function (nullptr = no-op).
 * @kw name              Debug name for the allocator.
 * @kw inline_headers    Prepend inline headers to allocations.
 * @kw external_metadata Keep OOB metadata in a separate pool.
 * @kw hidden            Make GC-invisible. Pages allocated through this
 *                       allocator are not registered in the mmap tree,
 *                       so the GC can never discover or scan them.
 *                       Allocator cleanup must use n00b_safe_munmap()
 *                       (which falls back to raw munmap for unregistered
 *                       pages).  See n00b_mmap_is_gc_scannable().
 * @kw __nomap           Skip mmap registration (internal only).
 * @kw __system          Skip STW checks (internal only).
 * @kw __is_md_pool      Mark as a metadata pool (internal only).
 */
extern void
n00b_allocator_setup(n00b_allocator_t *allocator, n00b_calloc_fn alloc) _kargs
{
    n00b_free_fn              free              = nullptr;
    n00b_allocator_pre_destroy_fn pre_destroy   = nullptr;
    n00b_allocator_destroy_fn destroy           = nullptr;
    char                     *name              = nullptr;
    bool                      inline_headers    = true;
    bool                      external_metadata = true;
    bool                      hidden            = false;
    // DO NOT USE for custom allocators. Skips mmaps.
    bool                      __nomap           = false;
    // DO NOT USE for custom allocators. Skips STW check.
    bool                      __system          = false;
    // Off for default allocators, which are assumed to be either
    // GC'd or private. Pools have the opposite default.
    bool                      use_epochs        = false;
    bool                      __is_md_pool      = false;
    // "file:line" of the create-site (via N00B_LOC_STRING()); stored in the
    // vtable for the mmap histogram. Defaults to nullptr for ad-hoc allocators.
    const char               *creation_loc      = nullptr;
};

/**
 * @brief Try to retrieve the inline allocation header for a pointer.
 * @param p Pointer to a managed allocation.
 * @return  Optional inline header (none if @p p is not managed).
 */
static inline n00b_option_t(n00b_inline_hdr_t *) n00b_inline_alloc_header(void *p)
{
    if (!p) {
        return n00b_option_none(n00b_inline_hdr_t *);
    }

    uintptr_t uptr = (uintptr_t)p;

    if (uptr < N00B_ALLOC_HDR_SZ) {
        return n00b_option_none(n00b_inline_hdr_t *);
    }

    if ((uptr & (N00B_ALIGN - 1)) != 0) {
        return n00b_option_none(n00b_inline_hdr_t *);
    }

    n00b_inline_hdr_t *hdr = (n00b_inline_hdr_t *)(uptr - N00B_ALLOC_HDR_SZ);

    if (hdr->guard == n00b_gc_guard) {
        return n00b_option_set(n00b_inline_hdr_t *, hdr);
    }

    return n00b_option_none(n00b_inline_hdr_t *);
}

// Helper: N00B_ALLOC_OPTS(allocator) → &(n00b_alloc_opts_t){.allocator = X}
//         N00B_ALLOC_OPTS()          → nullptr
// Use in macros that optionally accept an allocator pointer.
#define _N00B_ALLOC_OPTS_1(_alloc_ptr)                                                         \
    &(n00b_alloc_opts_t)                                                                       \
    {                                                                                          \
        .allocator = (_alloc_ptr)                                                              \
    }
#define N00B_ALLOC_OPTS(...) N00B_FIRST(__VA_OPT__(_N00B_ALLOC_OPTS_1(__VA_ARGS__), ) nullptr)

// The variadic tail of the allocation macros is an OPAQUE kargs blob handed to
// the allocated type's constructor (see _n00b_alloc_raw). It is not a place to
// set n00b_alloc_opts_t fields: ncc's opaque-kargs call sites pass the first
// keyword's VALUE through as the kargs pointer and drop the rest, so
// `n00b_alloc(T, .allocator = pool)` allocates from the ambient allocator and
// hands `pool` to T's constructor as its kargs struct. A tail that starts with
// a designator is therefore always a mistake, and is rejected at compile time.
// Options go in the opts argument instead:
// n00b_alloc_with_opts(T, &(n00b_alloc_opts_t){.allocator = pool}).
//
// Only the bare macros below can check this. In the _with_opts forms the
// preprocessor splits a multi-field `&(n00b_alloc_opts_t){...}` across `opts`
// and the tail (braces do not protect commas), so their tail legitimately
// starts with a designator.
#define _N00B_GUARDED_NULL_OPTS(...)                                                           \
    (n00b_alloc_opts_t *)(0 * sizeof(struct {                                                  \
             static_assert((#__VA_ARGS__)[0] != '.',                                           \
                           "allocation options are not keyword arguments; pass "               \
                           "them in the opts argument of the _with_opts form");                \
             int guard;                                                                        \
         }))

#define N00B_NO_OPTS(...)                                                                      \
    N00B_FIRST(__VA_OPT__(_N00B_GUARDED_NULL_OPTS(__VA_ARGS__), ) nullptr)

#define n00b_alloc_with_opts(T, opts, ...)                                                     \
    _n00b_alloc_raw(1,                                                                         \
                    sizeof(T),                                                                 \
                    typehash(T *),                                                             \
                    N00B_LOC_STRING(),                                                         \
                    opts __VA_OPT__(, __VA_ARGS__))

#define n00b_alloc_array_with_opts(T, N, opts, ...)                                            \
    _n00b_alloc_raw((N),                                                                       \
                    sizeof(T),                                                                 \
                    typehash(T *),                                                             \
                    N00B_LOC_STRING(),                                                         \
                    opts __VA_OPT__(, __VA_ARGS__))

/* Flex allocations are not exact T1 objects: the tail can carry additional
 * words with a different shape. Keep them out of the typehash->layout upgrade
 * until there is a descriptor that models the flexible tail. */
#define n00b_alloc_flex_with_opts(T1, T2, N2, opts, ...)                                       \
    _n00b_alloc_raw(1,                                                                         \
                    (sizeof(T1) + sizeof(T2) * (N2)),                                          \
                    0,                                                                         \
                    N00B_LOC_STRING(),                                                         \
                    opts __VA_OPT__(, __VA_ARGS__))

#define _n00b_kargs_name(base_name) N00B_CONCAT(N00B_CONCAT(n00b_, base_name), _init)
#define n00b_kargs(base_name, ...)                                                             \
    kw_func(_n00b_kargs_name(base_name) __VA_OPT__(, __VA_ARGS__))

// Raw allocation of `n` elements of `sz` bytes, with the element type hash
// supplied at RUNTIME. For generic containers that only know their element type
// dynamically (the typed backing of a dict/heap built around a runtime element
// size): the hash lets the GC scan the backing precisely and marshal it. Still
// a raw byte block; prefer the compile-time T macros (n00b_alloc_array) when the
// element type is statically known. Only for generic-container implementation
// headers. There is no type-erased counterpart in the public API: the sole
// conservative allocation in libn00b is the libc-malloc interposer's private
// helper (src/core/alloc_interpose.c).
#define n00b_alloc_size_typed_with_opts(n, sz, type_hash, opts, ...)                           \
    _n00b_alloc_raw((n), (sz), (type_hash), N00B_LOC_STRING(), opts __VA_OPT__(, __VA_ARGS__))

#define n00b_alloc_size_typed(n, sz, type_hash, ...)                                           \
    n00b_alloc_size_typed_with_opts((n),                                                       \
                                    (sz),                                                      \
                                    (type_hash),                                               \
                                    N00B_NO_OPTS(__VA_ARGS__)                                  \
                                        __VA_OPT__(, __VA_ARGS__))

#define n00b_alloc(T, ...)                                                                     \
    n00b_alloc_with_opts(T, N00B_NO_OPTS(__VA_ARGS__) __VA_OPT__(, __VA_ARGS__))

#define n00b_new_kargs(T, base_name, ...)                                                      \
    n00b_alloc(T, n00b_kargs(base_name __VA_OPT__(, __VA_ARGS__)))

#define n00b_new_vargs(T, __vargs) n00b_alloc(T, n00b_vargs __vargs)

// Caller should put vargs in parentheses, to clearly delineate, and will make the expansion a
// function-like macro.
#define n00b_new_both(T, base_name, __vargs, ...)                                              \
    n00b_alloc(T, n00b_vargs __vargs, n00b_kargs(base_name __VA_OPT__(, __VA_ARGS__)))

#define n00b_alloc_array(T, N, ...)                                                            \
    n00b_alloc_array_with_opts(T,                                                              \
                               N,                                                              \
                               N00B_NO_OPTS(__VA_ARGS__) __VA_OPT__(, __VA_ARGS__))

#define n00b_alloc_flex(T1, T2, N2, ...)                                                       \
    n00b_alloc_flex_with_opts(T1,                                                              \
                              T2,                                                              \
                              N2,                                                              \
                              N00B_NO_OPTS(__VA_ARGS__) __VA_OPT__(, __VA_ARGS__))

// The type-erased n00b_alloc_size / n00b_alloc_size_with_opts macros were
// removed from the public API: every n00b allocation should carry a type so the
// GC scans it precisely and it is marshalable. Use n00b_alloc / n00b_alloc_array
// / n00b_alloc_size_typed for typed allocation. The sole conservative,
// type-erased allocation left in libn00b is a private static helper in the
// libc-malloc interposer (src/core/alloc_interpose.c), where no element type is
// knowable.

/**
 * @brief Get the inline header for a managed object.
 * @param p Pointer to a managed object.
 * @return  Optional inline header.
 */
extern n00b_option_t(n00b_inline_hdr_t *) n00b_object_header(void *p);
