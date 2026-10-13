/**
 * @file runtime.h
 * @brief Runtime state and initialization.
 *
 * Defines the n00b_runtime_t structure that holds global state (threads,
 * memory maps, allocators) and the n00b_init() entry point.
 */
#pragma once

#if !defined(N00B_THREADS_MAX)
#define N00B_THREADS_MAX 4096
#endif

#include <assert.h>

#include "n00b.h"
#include "adt/result.h"
#include "core/alloc_base.h"
#include "core/rt_access.h"
#include "core/thread.h"
#include "adt/array.h"
#include "adt/list.h"
#include "adt/option.h"
#include "adt/variant.h"
#include "adt/interval_tree.h"
#include "core/pool.h"
#include "core/spinlock.h"
#include "core/mutex.h"
#include "core/rwlock.h"
#include "conduit/conduit_types.h"

typedef struct n00b_runtime_t n00b_runtime_t;

// Forward declarations to avoid circular includes.
typedef struct n00b_conduit_service         n00b_conduit_service_t;
typedef struct n00b_http_connection_pool    n00b_http_connection_pool_t;
typedef struct n00b_acme_tls_state          n00b_acme_tls_state_t;
typedef struct n00b_static_identity_entry_t n00b_static_identity_entry_t;
typedef struct n00b_marshal_pending_static_patch_t n00b_marshal_pending_static_patch_t;
typedef struct n00b_ct_image_repair_registration_t n00b_ct_image_repair_registration_t;
typedef struct n00b_crt_pending_protect_t n00b_crt_pending_protect_t;

/**
 * @brief Variant type for mmap registry interval trees.
 *
 * Mmap records and lightweight sub-range records are stored in separate
 * trees, but share the same concrete node/data shape so back-pointers and
 * registry helpers can stay uniform.
 */
typedef n00b_variant_t(n00b_mmap_info_t *, n00b_alloc_range_t *) n00b_mmap_data_t;

struct n00b_mmap_ctx_t {
    n00b_interval_tree_t(n00b_mmap_data_t) * mmap_tree;
    n00b_interval_tree_t(n00b_mmap_data_t) * range_tree;
    n00b_static_identity_entry_t *static_identities;
    /* Mmap-registry lock (WP-001): the re-entrant, non-parking spinlock.
     * A tree mutation can nest a lookup (mmaps_insert_raw -> n00b_free ->
     * finalizer -> n00b_mmap_by_address), and this lock's owner+nesting
     * accounting lets the same thread re-acquire without self-deadlock and
     * without a nested unlock dropping the lock the outer mutation holds.
     * It must NOT park (it is held inside the can't-STW barrier), which is
     * why it is the spinlock class and not n00b_mutex_t. */
    n00b_spin_lock_t              lock;
    n00b_pool_t                   pool;
};

struct n00b_runtime_t {
    n00b_array_t(char *) argv;
    n00b_array_t(char *) envp;
    n00b_mmap_ctx_t             mmaps;
    _Atomic uint32_t            next_thread_slot;
    _Atomic uint32_t            live_threads;
    _Atomic bool                startup_complete;
    /* Set once thread records exist and before init spawns its first
     * thread. Data structures created from here on get their rwlock. */
    _Atomic bool                threads_may_exist;
    /* Set by `n00b_shutdown` before tearing anything down.  Spin-wait
     * helpers (`n00b_futex_wait_for_value`, `n00b_futex_wait_on_mask`)
     * check this and break out instead of looping forever when the
     * thread they were waiting on is being reaped during shutdown. */
    _Atomic bool                shutdown_started;
    _Atomic(n00b_allocator_t *) default_allocator;
    n00b_arena_t               *default_arena; // GC'd arena (when using default allocator)
    n00b_pool_t                 system_pool;   // System pool for root list & lock records.
    _Atomic int64_t             marshal_pending_static_patch_lock;
    n00b_marshal_pending_static_patch_t *marshal_pending_static_patches;
    size_t                      marshal_pending_static_patch_count;
    _Atomic int64_t             ct_image_repair_hook_lock;
    n00b_ct_image_repair_registration_t *ct_image_repair_hooks;
    _Atomic int64_t             crt_pending_protect_lock;
    n00b_crt_pending_protect_t *crt_pending_protects;
    /* GC-VISIBLE, non-moving pool for GC-reclaimable runtime structs —
     * currently the per-thread `n00b_thread_t` (WP-3a / D-034).  NOTE: named
     * `runtime_obj_pool` to avoid colliding with the upstream `user_pool`
     * below, which is a HIDDEN leak-tracking pool for application allocations
     * (the WP-close deconfliction — D-034/D-039; the two are NOT the same pool:
     * this one is GC-visible, that one is hidden).  Unlike `system_pool`
     * (hidden + bulk-freed at teardown) and `conduit_pool` (also hidden),
     * `runtime_obj_pool` is initialized with `hidden = false` and WITHOUT
     * `.__system`, so the GC sees its allocations as ordinary objects: they
     * live as long as reachable and are reclaimed once unreferenced.  Being a
     * pool, it is NON-MOVING — the GC never relocates an allocation made from
     * it, which keeps the raw `rt->threads[].thread` pointers,
     * `n00b_thread_self()`'s masked recovery, and the explicit thread-struct /
     * record / lock-chain scan in gc.c all valid. */
    n00b_pool_t                 runtime_obj_pool;
    n00b_list_t(n00b_gc_root_t) gc_roots; // User-registered GC roots.
    /* Finalizers attached to allocations with no OOB record (arenas in
     * release builds, inline-header pools, the system pool): a dict from the
     * user pointer to its n00b_finalizer_info_t *, backed by finalizer_pool.
     * Mutators touch it only under the critical_execution read gate, so a
     * collector, which holds the write side, always finds it quiescent and
     * re-keys moved objects in place. finalizer_count is its size, read
     * without the gate so a free with nothing registered skips the lookup. */
    _n00b_dict_internal_t   *finalizers;
    n00b_allocator_t        *finalizer_pool;
    _Atomic uint64_t         finalizer_count;
    /* Finalizers of objects a collection found unreachable. The collector
     * appends under STW and treats every entry's user_ptr as a root, so the
     * object and what it references survive until the finalizer has run;
     * n00b_gc_run_finalizers pops them after the world restarts. */
    n00b_finalizer_info_t   *pending_finalizers;
    uint32_t                 pending_finalizers_cap;
    _Atomic uint32_t         pending_finalizers_len;
    _Atomic bool             finalizer_drain_active;
    /* Every pool created with @c external_metadata=true registers
     * itself here so the GC mark phase can iterate its metadata dict
     * directly. Each metadata-bearing alloc with @c alive set is a
     * root for the mark pass; entries reached during mark get
     * stamped with @c gc_current_epoch on their OOB header. Entries
     * still @c alive after mark whose epoch is stale are leaks —
     * the sweep returns them to the pool.  System_pool and the
     * hidden no-metadata pools sit outside this list and continue
     * to use the global rt->finalizers fallback. */
    n00b_list_t(n00b_allocator_t *) metadata_pools;
    /* Monotonic counter incremented at the start of every GC. Used
     * by the metadata-pool sweep above to detect leaks (stale
     * epoch = handed-out but never reached). */
    _Atomic(uint64_t)          gc_current_epoch;
    /* Always-on stop-the-world pause accounting (written by the collector
     * around its STW window, read by status/diagnostics). The 0.25s pause
     * budget is enforced against gc_max_pause_ns under a full-build
     * workload; poll-based watchers cannot resolve sub-second pauses. */
    _Atomic(uint64_t)          gc_last_pause_ns;
    _Atomic(uint64_t)          gc_max_pause_ns;
    _Atomic(uint64_t)          gc_pause_total_ns;
    _Atomic(uint64_t)          gc_pause_count;
    /* When set, the next collection prints file_name + tinfo +
     * alloc_len for each metadata-pool leak it finds before
     * returning the slot to its pool. Toggled by
     * n00b_debug_find_leaks. */
    _Atomic(bool)              debug_leak_detect;
    /* When set, every NATURAL collection (arena-pressure auto-collect,
     * marshal collect, etc.) runs the diagnostic default-arena by-site
     * census in-line and publishes the report to stderr_topic AFTER the
     * world restarts.  Unlike debug_leak_detect this does NOT change
     * reclaim semantics — it only reads the post-mark gc_epoch stamps the
     * normal collect already sets — so it is safe to leave armed under
     * load.  Opt-in (off by default); only meaningful when the census is
     * compiled in (N00B_DEBUG or N00B_DEBUG_LIVE_CENSUS builds).  Enabled
     * by long-running diagnostics (e.g. crayon-gw) that
     * want default-heap occupancy on every natural GC without issuing a
     * proactive collect.  See n00b_collect()/n00b_collect_internal(). */
    _Atomic(bool)              census_on_collect;
    n00b_dict_untyped_t       *type_registry; // typehash -> n00b_type_info_t *
    n00b_pool_t                conduit_pool;  // Pool for conduit infra (registered as GC root).
    /* User-space pool for application allocations that want
     * leak-tracking. Initialised with external_metadata=true so
     * every alloc carries an OOB record (alive bit + gc_epoch +
     * file_name + tinfo) and participates in
     * n00b_debug_find_leaks. NOT for hot-path traffic — the
     * per-alloc dict+OOB bookkeeping makes this pool more
     * expensive than system_pool or conduit_pool. Use it for
     * client allocations whose lifecycle the application owns
     * and that need to be auditable for leaks. */
    n00b_pool_t                user_pool;
    n00b_dict_untyped_t       *sub_map;         // conduit subscription handle -> sub ptr
    n00b_conduit_t            *default_conduit; // Default conduit for IO service.
    n00b_conduit_service_t    *default_service; // Service thread pool (IO + signal).
    n00b_conduit_fd_owner_t   *stdin_owner;     // Managed fd 0.
    n00b_conduit_fd_owner_t   *stdout_owner;    // Managed fd 1.
    n00b_conduit_fd_owner_t   *stderr_owner;    // Managed fd 2.
    n00b_conduit_topic_base_t *stdout_topic;    // Typed stdout buffer topic.
    n00b_conduit_topic_base_t *stderr_topic;    // Typed stderr buffer topic.
    /* Thread slot table.  Sized at init time per the @c max_threads
     * kwarg (defaults to @c N00B_THREADS_MAX).  Allocated from
     * @c system_pool so that other threads can read it safely
     * (non-moving, hidden from GC).  @c max_threads is the number of
     * slots and the modulo used for slot acquisition. */
    uint32_t                   max_threads;
    n00b_thread_record_t      *threads;
    _Atomic uint64_t           mm_epoch; // Memory management thread epoch
    // For epoch-based reclaims (metadata pools mainly).
    _Atomic uint64_t          *epoch_reservations;
    /* Runtime retirement center. Threads own their hot-path retire_list while
     * alive; on exit they atomically transfer unreclaimed nodes here so another
     * mutator can reclaim them once all epoch reservations have advanced. */
    _Atomic(struct n00b_epoch_hdr_t *) epoch_dead_letters;
    /* Serializes epoch retire-list ownership transfers. Retire/reclaim, thread
     * exit dead-letter transfer, and allocator destruction all detach and walk
     * the same intrusive lists; this raw lock is taken inside
     * critical_execution when the runtime is live so STW cannot suspend a
     * mutator while it owns the retire center. */
    _Atomic uint32_t           epoch_retire_lock;
    /* Deferred teardown queue for use_epochs allocators. n00b_retire parks
     * nodes on the calling thread's retire list WITHOUT the retire-center
     * lock, and dict-store migration can retire another allocator's stores
     * from any thread — so an inline destroy always races a concurrent
     * retire: the drain detaches a list, the racer re-parks a fresh node for
     * the dying pool, and the next flush frees it through a dead allocator.
     * Instead, destroy ENQUEUES the allocator here (per-slot CAS) and the
     * actual teardown runs during stop-the-world, right after
     * n00b_epoch_flush_all_stw has emptied every retire list — no mutator
     * can race, and all parked nodes were freed while the pool was alive.
     * Queue-full falls back to an explicit STW barrier
     * (n00b_allocator_deferred_destroy_barrier). */
#define N00B_DEFERRED_DESTROY_MAX 256
    _Atomic(n00b_allocator_t *) deferred_destroys[N00B_DEFERRED_DESTROY_MAX];
    _Atomic uint64_t           deferred_destroy_barrier_count;
    /* Live-slot bitmap for n00b_thread_self()'s foreign-safe bounds scan.
     * One bit per thread slot ((max_threads+63)/64 words), allocated from
     * system_pool at init.  A bit is SET after a thread publishes its
     * stack bounds (n00b_thread_init) and CLEARED before those bounds are
     * torn down (n00b_thread_exit), so the scan visits only slots whose
     * [stack_lo, stack_hi) is currently valid.  This lets n00b_thread_self()
     * resolve a FOREIGN (non-callstack) thread by SP-against-bounds without
     * the masked id-word read that faults on foreign stacks (see the
     * foreign-thread note in include/core/thread.h), and bounds the scan to
     * live threads rather than the full max_threads table. */
    _Atomic uint64_t          *live_slot_bits;
    /* Open-addressed hash set of live n00b CALLSTACK region bases (each an
     * S-aligned 8 MiB region).  n00b_thread_self()'s worker fast path probes
     * this O(1) to decide whether the SP's masked base is a real callstack —
     * only then is the id-word read at base+S-8 guaranteed mapped and taken
     * (the D-014 masking).  A FOREIGN (libdispatch/XPC) thread's base is NOT
     * in the set, so it falls back to the foreign-safe live-slot bounds scan
     * instead of faulting on an unmapped region top.  Sized to a power of two
     * (mask = size-1) at init from system_pool; a base is inserted when its
     * callstack region is laid out (n00b_callstack apply-geometry) and is
     * never removed (callstack pages persist in the pool, so the entry stays
     * safe-to-read).  This keeps the runtime's hottest call O(1) — an O(live)
     * scan here cost ~460ns/call and throttled high-throughput workers. */
    _Atomic(uintptr_t)        *callstack_base_set;
    uint32_t                   callstack_base_set_mask;
    /* Callstack reclamation bookkeeping (WP-3a Phase 2, D-034).  Both
     * lists are zero-initialized by the runtime's zero-fill at init (a
     * null head + a 0 lock is the correct empty state), so no explicit
     * init call is needed.
     *
     * `callstack_pool` is the free-list a spawn draws an 8 MiB callstack
     * region from before falling back to a fresh `n00b_mmap`; a worker's
     * region returns here at OS-confirmed death instead of being unmapped
     * (glibc stack-cache / Go stack-pool precedent).  The list links
     * through `n00b_callstack_t::pool_next`; `callstack_pool_lock` guards
     * it and `callstack_pool_count` tracks the pooled-region count for the
     * keep-N trim cap (see `N00B_CALLSTACK_POOL_MAX` / DF-4).
     *
     * `reap_pending` is the queue of workers that have published exit but
     * await OS-death confirmation (macOS dead Mach port / Linux
     * CLONE_CHILD_CLEARTID futex).  A worker enqueues its own struct at the
     * launcher exit (before self-terminate); the reaper drains it from the
     * callstack-pool slow path and the conduit signal thread, reclaiming
     * only the workers whose death edge has fired.  The queue links through
     * `n00b_thread_t::reap_next`; `reap_lock` guards it.  This is NOT the
     * GC-owned struct lifetime (D-034): the struct stays in `runtime_obj_pool`;
     * only the callstack/TCB/slot are reclaimed here. */
    struct n00b_callstack_t   *callstack_pool;
    _Atomic uint32_t           callstack_pool_lock;
    uint32_t                   callstack_pool_count;
    struct n00b_thread_t      *reap_pending;
    _Atomic uint32_t           reap_lock;
    /* Serializes the slot-scanning FOREIGN reaper (_n00b_reap_foreign_sweep).
     * Foreign (libdispatch/XPC) threads never self-destroy, so that reaper
     * must clear their slot itself; the lock ensures only one sweep does the
     * per-slot clear-then-CAS at a time (no concurrent sweep can clear a
     * slot's bits after another sweep freed it and a new thread reacquired). */
    _Atomic uint32_t           foreign_reap_lock;
    n00b_base_allocator_t      slab_allocator;
    /* Pure-preemptive stop-the-world (WP-001).  `critical_execution` is the
     * single STW lock — a READER/WRITER lock.  "Stopping the world" does NOT
     * mean actually stopping it: a thread doing critical execution takes a READ
     * lock and runs concurrently with other readers.  Critical execution =
     * mmap/munmap, mmap interval-tree mutation, a thread's WHOLE init / WHOLE
     * destroy, and ALL access (read and write) to a MOVABLE (copying-GC) arena's
     * OOB metadata.  The collector takes the WRITE lock: acquiring it DRAINS all
     * readers, so by the time it runs nothing is mid-critical-section (the mmap
     * tree and every movable-arena metadata dict are quiescent); it then
     * preemptively suspends every other registered thread for the stack scan —
     * safe now, because no frozen thread can hold a metadata/mmap lock.  The
     * read side is reentrant (via the futex reader count) and, AS A SPECIAL CASE
     * FOR THIS LOCK ONLY, may be acquired with n00b_thread_self() unresolvable
     * (a thread holds it across its whole init/destroy); it is excluded from the
     * per-thread lock-accounting chain.  `stw_active` is set once the world is
     * fully stopped (write lock held + everyone suspended); while it is set every
     * n00b lock acquire/release short-circuits to a no-op (the collector is the
     * sole runner, so its own re-entrant reads of this lock during the scan must
     * not block on the write lock it holds). */
    n00b_rwlock_t              critical_execution;
    /*
     * Global epoch for epoch-based memory allocation pools.
     */
    uint64_t                   global_epoch;
    _Atomic bool               stw_active;
    /* Stop-the-world nesting depth, owned exclusively by the (single) STW
     * initiator.  The gate's own owner+nesting recursion cannot track this
     * once stw_active is set, because at that point every lock op — including a
     * nested critical_execution acquire by the initiator — short-circuits to a
     * no-op.  So nested stop/restart is tracked here: only the outermost stop
     * suspends and the outermost restart resumes.  Only the initiator touches
     * it (everyone else is suspended), but it is _Atomic for clean visibility
     * across the suspend/resume boundary. */
    _Atomic uint32_t           stw_nesting;
    const char                *theme_name;  // Active theme name (set during init).
    n00b_unicode_ctx_t        *unicode_ctx; // Phase 4.5 unicode subsystem state.
    n00b_regex_ctx_t          *regex_ctx;   // Regex port-side caches.
    /* Per-runtime HTTP connection pool — populated lazily on first
     * `n00b_http_request_sync` / `n00b_http_request` call via
     * `n00b_http_get_connection_pool(runtime)`.  Drained at runtime
     * shutdown.  See include/internal/net/http/http_pool.h for the
     * pool API. */
    _Atomic(n00b_http_connection_pool_t *) http_connection_pool;
    /* Per-runtime picotls base context + chain verifier for the h1
     * TLS transport (see src/net/quic/acme_tls.c).  Lazy-initialized
     * on first connect; the slot holds an opaque pointer because the
     * underlying picotls types aren't part of n00b's public surface. */
    _Atomic(n00b_acme_tls_state_t *)       acme_tls_state;
};

/**
 * @brief Initialize the n00b runtime.
 * @param rt   Runtime structure to initialize.
 * @param argc Argument count from main().
 * @param argv Argument vector from main().
 *
 * @kw allocator    Allocator to use (nullptr = GC'd arena).
 * @kw envp         Environment pointer (nullptr = inherit).
 * @kw numeric_locale Numeric locale string ("" = default).
 * @kw fd_limit     File descriptor limit (0 = don't change, <0 = don't set).
 * @kw max_threads  Maximum thread count (default N00B_THREADS_MAX).
 *
 * @pre Must be called exactly once, from the main thread, before any
 *      other n00b API.
 * @post `n00b_get_runtime()` returns a valid pointer. The calling
 *       thread is registered with the STW subsystem.
 */
extern void
n00b_init(n00b_runtime_t *rt, int argc, char *argv[]) _kargs
{
    n00b_allocator_t *allocator      = nullptr; // nullptr = use a GC'd arena
    char            **envp           = nullptr;
    char             *numeric_locale = "";
    int               fd_limit       = 0; // Less than 0 = "don't set"
    unsigned int      max_threads    = N00B_THREADS_MAX;
};

/**
 * @brief Plain-C wrapper for n00b_init() with default kargs.
 *
 * Callable from code not compiled through ncc (e.g. startup shims
 * for AOT-compiled binaries).  Heap-allocates an n00b_runtime_t
 * internally.
 */
extern void n00b_init_simple(int argc, char *argv[]);

/**
 * @brief Plain-C wrapper for the core-only runtime init phase.
 *
 * @details Intended for ncc-generated entries that must initialize the
 * allocator/GC/thread runtime, apply migrated static-initializer roots, and
 * only then initialize registries that consume those roots.
 */
extern void n00b_init_core_simple(int argc, char *argv[]);

/**
 * @brief Finish runtime registries that depend on migrated static init state.
 *
 * @pre n00b_init_core_simple() or n00b_init() core setup has completed.
 * @post Built-in style/theme/tokenizer/renderer registries are initialized.
 */
extern void n00b_init_late(void);

/**
 * @brief Plain-C wrapper for n00b_shutdown() with default kargs.
 *
 * Callable from code not compiled through ncc (e.g. the AOT startup
 * shim), which cannot expand the _kargs form.  Shuts down the default
 * runtime.
 */
extern void n00b_shutdown_simple(void);

/**
 * @brief Shut down the runtime, stopping all service threads.
 * @pre  Must be called from the main thread before returning from main().
 * @post All conduit IO threads have exited.
 *
 * @kw runtime  Runtime to shut down (default: the active default runtime
 *              via n00b_get_runtime()).  Pass an explicit handle when the
 *              caller still holds the live runtime and must not rely on
 *              the global (e.g. a stack-local runtime in the CLI pattern).
 */
extern void
n00b_shutdown() _kargs
{
    n00b_runtime_t *runtime = nullptr;
};

/**
 * @brief Shut down the runtime and terminate the process.
 * @param code Process exit status.
 *
 * Prefer this over calling `exit()` directly from n00b-aware programs,
 * so conduit IO and other runtime services get a chance to drain.
 */
extern void n00b_exit(int code);

/**
 * @brief Get the runtime's default allocator.
 * @return Pointer to the default allocator.
 */
static inline n00b_allocator_t *
n00b_default_allocator(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();

    assert(rt);
    assert(rt->default_allocator);

    return rt->default_allocator;
}

/**
 * @brief Get the runtime's slab (pool) allocator.
 * @return Pointer to the slab allocator.
 */
static inline n00b_allocator_t *
n00b_slab_allocator(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();

    assert(rt);
    return (n00b_allocator_t *)&rt->slab_allocator;
}

/**
 * @brief Get the runtime's system pool allocator.
 *
 * The system pool is non-arena, non-GC-scanned, never moved, and never
 * freed. Use it for objects whose addresses must stay valid outside the
 * GC's view — e.g. environment slots (`core/env.c`) or heap literals
 * baked into JIT-generated code as raw immediates (`slay/codegen.c`).
 *
 * @return Pointer to the system pool allocator.
 */
static inline n00b_allocator_t *
n00b_system_allocator(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();

    assert(rt);
    return (n00b_allocator_t *)&rt->system_pool;
}
