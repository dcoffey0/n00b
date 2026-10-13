/**
 * @file thread.h
 * @brief Thread management and per-thread state.
 *
 * Defines the n00b_thread_t structure, the TLS-free n00b_thread_self() recovery
 * entry point, and helpers for capturing stack bounds (needed by the
 * GC).
 *
 * The calling thread's n00b_thread_t lives in the permanent
 * n00b_thread_t allocated from the GC-visible, non-moving `user_pool`
 * and pointed at by `rt->threads[slot].thread`.  Being a
 * pool it is non-moving (so `n00b_thread_self()` and the raw slot pointer
 * stay valid), but unlike the old `system_pool` home it is GC-OWNED —
 * reclaimed once unreferenced, not bulk-freed at teardown.
 * `n00b_thread_self()` recovers the slot from the current stack
 * pointer in O(1): a range check against the main thread's kernel-stack
 * bounds (stored in its `n00b_thread_record_t` at init), then the
 * Phase-1 masking helper `n00b_callstack_id_word` for worker callstacks.
 */
#pragma once

// Capacity of n00b_thread_t.gc_captured_vregs, in words. Linux copies the
// signal frame's XSAVE area (AVX-512 state is about 2.5KB) or SVE registers.
#if defined(__linux__)
#define N00B_GC_VREG_WORDS 512
#else
#define N00B_GC_VREG_WORDS 64
#endif

#include "n00b.h"
#include "core/platform.h"
#include "core/rt_access.h"
#include "core/atomic.h"
#include "core/callstack.h"
#include "adt/option.h"
#include "adt/result.h"

/**
 * @brief Slot index reserved for the main (kernel-stack) thread.
 *
 * The main thread is the first to call `n00b_thread_init`, so it
 * acquires slot 0.  `n00b_thread_self()`'s O(1) range-check branch
 * reads the main-stack bounds from this slot's record.
 */
#define N00B_MAIN_THREAD_SLOT ((uint32_t)0)

/**
 * @brief Bootstrap thread struct used during the startup window.
 *
 * Serves `n00b_thread_self()` after the runtime exists but before the
 * main thread's slot bounds are published (init is single-threaded, so
 * a single non-moving struct is safe).  Defined in thread.c; declared
 * here because the `n00b_thread_self()` macro references it from every
 * translation unit.
 */
extern n00b_thread_t _n00b_bootstrap_thread;

typedef struct {
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t alloc_failures;
    uint64_t current_count;
    uint64_t current_bytes;
    uint64_t high_water_count;
    uint64_t high_water_bytes;
    uint64_t page_size_bytes;
} n00b_thread_tcb_stats_t;

n00b_thread_tcb_stats_t n00b_thread_tcb_stats(void);

typedef struct {
    int     fds[2]; // 0 is read end, 1 is write end
    uint8_t ready;
} n00b_memperm_pipe_t;

/**
 * @brief Per-thread state for condition variable waits.
 *
 * Defined here (rather than in condition.h) so that
 * `n00b_thread_record_t` can embed it by value without a
 * circular include dependency.
 */
struct n00b_condition_thread_state_t {
    _Atomic(n00b_condition_t *) current_cv;
    _Atomic uint32_t            wait_state;
    uint64_t                    wait_predicate;
    void                       *thread_param;
    char                       *wait_loc;
};

/**
 * @brief Per-thread shared record allocated from the system pool.
 *
 * Lives in `rt->threads[]` so that *other* threads can walk lock
 * chains on crash or thread-exit cleanup.  The owning thread
 * accesses this through `n00b_thread_t::record`.
 */
struct n00b_thread_record_t {
    _Atomic(n00b_thread_t *)          thread; ///< Back-pointer to the runtime-owned thread_t.
    uint32_t                          generation;       ///< Generation counter for slot reuse.
    _Atomic(n00b_lock_base_t *)       exclusive_locks;  ///< Head of exclusive-lock chain.
    _Atomic(n00b_thread_read_log_t *) read_locks;       ///< Head of read-lock chain.
    _Atomic(n00b_thread_read_log_t *) log_alloc_cache;  ///< Cached freed read-log entries.
    n00b_condition_thread_state_t     cv_info;          ///< Condition-variable wait state.
    n00b_lock_base_t                 *lock_wait_target; ///< Lock we are currently blocked on.
    char                             *lock_wait_loc;    ///< Source location of the wait.
    char                             *lock_wait_trace;  ///< Backtrace at wait (debug).
    /* Owner-thread-only recursion depth for the pool page-table gate
     * (pool.c pool_page_gate_enter). The gate's critical_execution read
     * acquisition can itself allocate (rwlock read-log record -> system_pool
     * growth -> new_page_entry), which re-enters the gate; the depth counter
     * bounds that recursion (nested entries skip the lock — the outer entry
     * either holds it or is in the middle of acquiring it). Never touched by
     * other threads; reset at slot claim. */
    uint32_t                          pool_gate_depth;
    n00b_string_t  *regex_last_detail; ///< Last regex compile error detail (per-thread).
    /* Native-stack bounds for n00b_thread_self() range checks.
     * The main thread uses an O(1) check; attached foreign threads use the
     * live-slot scan. Workers resolve via the masking helper. `stack_lo` is the
     * lowest in-stack address, `stack_hi` is one past the highest. Published
     * by the owning thread (stack_hi first, stack_lo last, so a non-null
     * stack_lo implies stack_hi is set); read by n00b_thread_self() on any
     * thread.  On Windows, stack_lo can move downward as the kernel commits
     * additional stack pages. */
    _Atomic(void *) stack_lo;
    _Atomic(void *) stack_hi;
#ifdef N00B_DEBUG
    // Owner-thread-only counts of this thread's rwlock read and write
    // acquisitions and of its write releases, each of which makes a futex
    // wake.
    uint64_t debug_rw_read_acquires;
    uint64_t debug_rw_write_acquires;
    uint64_t debug_rw_write_wakes;
#endif
};

#ifdef _WIN32
#define N00B_REFRESH_WINDOWS_STACK_BOUNDS(_record, _sp, _lo, _hi)                            \
    do {                                                                                     \
        if ((_lo) != nullptr && (_hi) != nullptr                                             \
            && (uintptr_t)(_sp) < (uintptr_t)(_lo)) {                                        \
            NT_TIB *_n00b_tib = (NT_TIB *)NtCurrentTeb();                                    \
            void   *_n00b_win_lo = _n00b_tib == nullptr ? nullptr : _n00b_tib->StackLimit;   \
            void   *_n00b_win_hi = _n00b_tib == nullptr ? nullptr : _n00b_tib->StackBase;    \
            if (_n00b_win_lo != nullptr && _n00b_win_hi == (_hi)                             \
                && (uintptr_t)(_sp) >= (uintptr_t)_n00b_win_lo                               \
                && (uintptr_t)(_sp) < (uintptr_t)_n00b_win_hi) {                             \
                n00b_atomic_store(&(_record)->stack_hi, _n00b_win_hi);                       \
                n00b_atomic_store(&(_record)->stack_lo, _n00b_win_lo);                       \
                (_lo) = _n00b_win_lo;                                                        \
                (_hi) = _n00b_win_hi;                                                        \
            }                                                                                \
        }                                                                                    \
    } while (0)
#else
#define N00B_REFRESH_WINDOWS_STACK_BOUNDS(_record, _sp, _lo, _hi) \
    do {                                                     \
    } while (0)
#endif

/**
 * @brief Normalized, cross-platform thread scheduling tier
 *
 * A small ordered set of OS-agnostic tiers a caller asks for via
 * `n00b_thread_spawn(.priority = …)` / `.scheduler = …`.  The launcher maps
 * the requested tier to each OS's native scheduling primitive on the worker
 * itself:
 *
 *   - macOS: Mach `thread_policy_set(THREAD_PRECEDENCE_POLICY)` with a signed
 *     `importance` (idle → -2 … realtime → +2).  Precedence is the queryable
 *     Mach surface (`thread_policy_get` reads it back); it is best-effort by
 *     construction and never fails the spawn.
 *   - Linux: raw `sched_setscheduler(SCHED_*)` + `setpriority(nice)` on the
 *     calling thread (NOT `pthread_setschedparam`):
 *       idle      → SCHED_IDLE,  nice +19
 *       low       → SCHED_OTHER, nice +10
 *       normal    → SCHED_OTHER, nice  0
 *       high      → SCHED_OTHER, nice -10
 *       realtime  → SCHED_FIFO,  rt-priority 10
 *   - Win32: `SetThreadPriority` (idle → THREAD_PRIORITY_IDLE, low → BELOW,
 *     normal → NORMAL, high → ABOVE, realtime → TIME_CRITICAL).
 *
 * `.priority` and `.scheduler` are TWO NAMES for the SAME tier request (the
 * normalized model folds policy + priority into one ordered tier); passing
 * both is allowed and the launcher uses the higher of the two so a caller
 * cannot accidentally downgrade by setting one and leaving the other at the
 * default.  The exact per-OS mapping is NOT 1:1 and is documented above.
 *
 * FAIL-SOFT: a privileged tier the process cannot grant (e.g.
 * `N00B_THREAD_TIER_REALTIME` without CAP_SYS_NICE on Linux) does NOT fail
 * the spawn — the worker runs at whatever the OS granted and the REQUESTED
 * tier is still recorded on `n00b_thread_t::sched_tier` for introspection.
 */
typedef enum : uint8_t {
    N00B_THREAD_TIER_DEFAULT  = 0, ///< No tier requested (leave OS default).
    N00B_THREAD_TIER_IDLE     = 1, ///< Run only when nothing else wants the CPU.
    N00B_THREAD_TIER_LOW      = 2, ///< Below-normal, still time-shared.
    N00B_THREAD_TIER_NORMAL   = 3, ///< The default time-shared tier.
    N00B_THREAD_TIER_HIGH     = 4, ///< Above-normal, still time-shared.
    N00B_THREAD_TIER_REALTIME = 5, ///< Highest; privileged on most OSes (fails soft).
} n00b_thread_tier_t;

/**
 * @brief Raw, per-OS scheduling escape hatch for platform-aware callers.
 *
 * When a caller passes `n00b_thread_spawn(.sched_raw = &(…){…})`, the launcher
 * BYPASSES the @ref n00b_thread_tier_t mapping entirely and applies these
 * values directly to the per-OS primitive on the worker.  The fields
 * are n00b-owned `int64_t`s (NOT a libc `struct sched_param` — no libc type
 * leaks into the public surface); each OS interprets them:
 *
 *   - Linux: `policy` is the `SCHED_*` constant for raw `sched_setscheduler`;
 *     `priority` is the rt-priority for the SCHED_FIFO/RR policies, or the
 *     `nice` value (via raw `setpriority`) for SCHED_OTHER/BATCH/IDLE.
 *   - macOS: `policy` selects the Mach policy flavor.
 *   - Win32: `policy` is unused; `priority` is the `SetThreadPriority` level.
 *
 * Like the tier path, the raw apply FAILS SOFT: an ungrantable request leaves
 * the worker at the OS default and the spawn still succeeds.  The descriptor
 * itself need only live for the duration of the spawn call (it is copied onto
 * the bundle); the applied values are recorded on
 * `n00b_thread_t::sched_raw` for introspection.
 */
typedef struct {
    int64_t policy;   ///< Per-OS policy selector (see per-OS notes above).
    int64_t priority; ///< Per-OS priority / importance / nice value.
} n00b_thread_sched_raw_t;

/**
 * @brief CPU-id set for thread affinity
 *
 * A caller asks for a worker's CPU affinity via
 * `n00b_thread_spawn(.affinity = &(n00b_thread_cpuset_t){.mask = …})`.  The set
 * is a FIXED 64-bit bitmask: bit `n` set means "CPU `n` is in the set" (so this
 * representation addresses logical CPUs 0..63; see the >64-CPU note below).
 * The launcher applies the set to the worker itself,
 * with per-OS strength:
 *
 *   - Linux: HARD PIN via raw `sched_setaffinity(0, sizeof(unsigned long),
 *     &mask)` (tid 0 = self) — NOT `pthread_setaffinity_np`, and WITHOUT
 *     glibc's `cpu_set_t`/`CPU_SET` macros: the @ref mask word IS the raw
 *     kernel cpu-set (one `unsigned long` on LP64, low bit = CPU 0), so it is
 *     handed to the syscall directly with `cpusetsize = sizeof(unsigned long)`.
 *   - Win32: HARD PIN via `SetThreadAffinityMask(GetCurrentThread(), mask)` —
 *     the `DWORD_PTR` mask is the @ref mask word truncated to pointer width.
 *   - macOS: ADVISORY ONLY.  Darwin exposes no hard CPU pin to user space; the
 *     launcher applies Mach `thread_policy_set(THREAD_AFFINITY_POLICY)` with an
 *     `affinity_tag` DERIVED from the set (the 1-based index of the lowest set
 *     CPU, so workers requesting the same lowest CPU share an L2-affinity tag).
 *     This is a SCHEDULER HINT, not a pin — the kernel may run the worker on
 *     any CPU (THREAD_AFFINITY_POLICY is documented "experimental" and a hint).
 *     Tests MUST NOT assert a hard pin on macOS.
 *
 * An EMPTY set (`mask == 0`) requests no affinity and is treated as "leave the
 * OS default" on every platform (no syscall is made).  Like the scheduling
 * attributes, the apply is best-effort: a failed `sched_setaffinity` /
 * `SetThreadAffinityMask` (e.g. a mask naming no online CPU) is IGNORED and the
 * worker keeps running; the REQUESTED set is still recorded on
 * @ref n00b_thread_t::affinity for introspection.
 *
 * >64-CPU NOTE: the fixed 64-bit mask addresses logical CPUs 0..63.  Machines
 * with more than 64 CPUs are not addressable above CPU 63 through this type; a
 * growable cpu-set is a possible future widening (the raw Linux syscall already
 * takes a `cpusetsize`, so the kernel side scales) but is NOT implemented
 */
typedef struct {
    uint64_t mask; ///< Bit `n` set ⇒ CPU `n` is in the set; 0 = no affinity.
} n00b_thread_cpuset_t;

/**
 * @brief Per-thread crash-handler callback
 */
typedef void (*n00b_thread_crash_handler_t)(n00b_thread_t *thread, void *data);

struct n00b_pool_t; // Folded-in string scratch; pointer only.

struct n00b_thread_t {
    union [[n00b::raw_union]] {
        struct {
            int32_t id;
            int32_t generation;
        } parts;
        uint64_t unique_id;
    } id_info;

    void                              *stack_base;
    void                              *stack_top;
    n00b_mmap_info_t                  *stack_map;
    /**
     * @brief Worker callstack guard-band low bound (nullptr for the
     *         main thread, which has no n00b guard band). Cached here
     *         so the crash handler classifies a stack-overflow fault
     *         by a lock-free pointer-range compare. _Atomic because
     *         the handler reads it cross-thread (matching
     *         stack_lo/stack_hi); the owning thread stores it once
     *         before its altstack install.
     */
    _Atomic(void *)                    guard_lo;
    // @brief Worker callstack guard-band high bound (exclusive); see @ref guard_lo.
    _Atomic(void *)                    guard_hi;
    n00b_memperm_pipe_t                memperm_pipe;
    n00b_futex_t                       self_lock;
    n00b_futex_t                       cv_wake;      ///< Per-thread futex for CV notification.
    struct n00b_gc_stack_frame_t      *gc_stack_top; ///< Top compiler-published exact GC frame.
    uint32_t                           gc_stack_policy; ///< n00b_gc_stack_policy_t value.
    /**
     * @brief true ⇒ this worker's C stack is EXCLUDED from the GC conservative
     *         scan; see @ref n00b_thread_spawn.
     */
    bool                               gc_isolated;
    /**
     * @brief set by the STW initiator when it PREEMPTIVELY suspended
     *         this RUNNING thread (vs. it self-parking at a
     *         checkin). When set, the GC scans @ref gc_captured_regs
     *         as conservative roots and uses @ref stack_top (= the
     *         captured SP) for the interrupted top frame.  Cleared by
     *         `_n00b_restart_the_world` before `thread_resume`.
     *         _Atomic for cross-thread visibility; only the (single)
     *         STW initiator writes it, under the STW lock.
     */
    _Atomic(bool)                      gc_preempt_suspended;
    /**
     * @brief  GP registers (arm64 x0-x28, fp, lr) captured from the
     *         OS thread state at preemptive suspend, scanned CONSERVATIVELY
     *         (the interrupted frame's live pointers may be
     *         register-resident only). SP→@ref stack_top (stack bound, not
     *         a heap pointer); PC excluded (code). Written by the STW
     *         initiator under the STW lock, read by the GC in the same
     *         stop. Valid only while @ref gc_preempt_suspended.
     */
    uint64_t                           gc_captured_regs[31];
    /**
     * @brief  SIMD/FP register state captured with @ref gc_captured_regs,
     *         as raw words: arm64 v0-v31 (plus SVE z0-z31 on Linux), and on
     *         x86-64 xmm0-15 plus, on Linux, the rest of the XSAVE area (ymm,
     *         zmm). A vectorized copy of a pointer array holds heap pointers
     *         here and nowhere else, so these are pinned like the GP
     *         registers. The first @ref gc_captured_vreg_words words are
     *         valid while @ref gc_preempt_suspended.
     */
    uint64_t                           gc_captured_vregs[N00B_GC_VREG_WORDS];
    uint32_t                           gc_captured_vreg_words;
    /**
     * @brief  In-flight allocation reservation, published BEFORE a thread
     *         commits an arena bump and cleared after the object's GC metadata
     *         (inline header / OOB record) is registered.  A thread can be
     *         preemptively STW-suspended between reserving storage and
     *         registering it; during that window the region is live (the thread
     *         will return and use it) but invisible to the collector (no
     *         header/record yet).  The pin pre-pass pins this range for every
     *         suspended thread so its page is retained, not reclaimed.  Stored
     *         as [start, start+len); len==0 means none.  Relaxed atomics: the
     *         owning thread writes; the collector reads it only while the thread
     *         is suspended (the suspend provides the ordering barrier).
     */
    _Atomic(void *)                    gc_inflight_start;
    _Atomic(uint64_t)                  gc_inflight_len;
    /// Set by an out-of-memory collect this thread ran that queued
    /// finalizers; the allocation that triggered it runs them before returning.
    bool                               gc_run_finalizers;
    /// Thread-local allocation buffer in the runtime's default arena:
    /// [tlab_cur, tlab_end) was reserved with one shared bump and is handed
    /// out with plain stores. Only this thread writes these, except that the
    /// collector retires the buffer (both null) with the world stopped.
    _Atomic(char *)                    tlab_cur;
    _Atomic(char *)                    tlab_end;
    /// Largest allocation already recorded against the buffer's mapping
    /// (n00b#395), so smaller ones skip n00b_arena_note_alloc_extent.
    uint64_t                           tlab_noted_len;
    /// Objects this thread has handed out from buffers. Only this thread
    /// writes it.
    uint64_t                           tlab_allocs;
    /// The tlab_allocs value already added to the arena's alloc_count. Only
    /// the collector (world stopped) and this thread's teardown (under the
    /// critical_execution gate) write it, so neither races the other.
    uint64_t                           tlab_allocs_seen;
    n00b_thread_record_t              *record; ///< Pointer into rt->threads[slot].
    // @brief Scoped allocator override (was __n00b_current_allocator).
    n00b_allocator_t                  *current_allocator;
    // @brief Last dynamic-lib error (was dynamic_lib.c t_last_error).
    n00b_string_t                     *dl_last_error;
    ///< Nulls cache scratch slot (was nulls.c `last`; NullsId-shaped).
    uint32_t                           regex_nulls_last;
    /**
     * @brief Per-thread string-builder scratch pool control struct (was
     *         string.c thread_local __n00b_string_scratch_storage).
     *         system_pool-allocated (non-GC-scanned, pinned), allocated
     *         once and reused for the thread's life; nullptr until first
     *         use.
     */
    struct n00b_pool_t                *string_scratch_storage;
    /**
     * @brief Active scratch marker (was string.c thread_local
     *         __n00b_string_scratch_pool): == string_scratch_storage while
     *         a string scope is open, nullptr otherwise.
     */
    struct n00b_pool_t                *string_scratch_arena;
    /**
     * @brief Persistent per-thread transient-scratch POOL for short-lived,
     *         explicitly-freed working buffers (e.g. the case-fold output
     *         buffer, encoder grow buffers).  A pool (not a bump arena) so
     *         n00b_free returns the slot immediately -- keeping these
     *         transients off the GC default arena without per-call pool
     *         create/destroy or shared-lock contention.  Lazily created and
     *         reused for the thread's life via n00b_thread_scratch_pool();
     *         nullptr until first use.
     */
    struct n00b_pool_t                *scratch_pool;
    /**
     * @brief Per-thread reusable conduit write-completion inbox for the
     *         synchronous n00b_fd_owner_write path.  A blocking write needs a
     *         private reply channel; allocating one per write churned a fresh
     *         condition variable each call, and because that CV lives in the
     *         (non-GC) conduit pool, _n00b_condition_init registered a GC root
     *         for it that was never unregistered.  Streaming N rows therefore
     *         leaked N roots and made each write O(roots) in
     *         _n00b_gc_register_root's linear dedup scan -- O(N^2) overall.
     *         One inbox per (pooled) thread, reused for the thread's life,
     *         bounds that to a single root per thread.  Completions are matched
     *         by (fd, request_id) so reuse across owners/conduits is safe.
     *         void* to avoid a conduit-header dependency here; the conduit layer
     *         owns the concrete type.  nullptr until first use.
     */
    void                              *write_done_inbox;
    // MEASUREMENT (opt-in, -DN00B_GC_ATTRIB): set while this thread is inside
    // rocs ingest, so the
    // GC-default-arena byte counter attributes implicit allocations to
    // "consumer" vs "other". Observational only (no allocation redirection).
    bool                               in_rocs_ingest;
    uint64_t                           aba_ctr;
    /**
     * @brief Worker's OS callstack (nullptr for the main thread); reclaimed by the
     *         REAPER at OS-death (NOT the joiner).
     */
    struct n00b_callstack_t           *callstack;
    /**
     * @brief crash-handler alternate signal stack — a pool callstack
     *         region (so self() resolves on it via SA_ONSTACK, surviving a stack
     *         overflow), drawn by the spawner and returned to the pool by the REAPER at
     *         OS-death alongside @ref callstack. _Atomic because the crash handler
     *         reads it cross-thread when scanning slots (via
     *         rt->threads[i].thread->altstack). Lives on the PER-WORKER struct (not the
     *         shared slot record) so a slot reused before this worker is reaped cannot
     *         make the reaper return a live worker's altstack. nullptr until
     *         n00b_crash_install_altstack() runs.
     */
    _Atomic(struct n00b_callstack_t *) altstack;
    n00b_futex_t       join_futex; ///< 0 while running, 1 once the worker has exited.
    n00b_futex_t       reap_futex; ///< 0 until OS-death reaper has reclaimed worker-local state.
    // @brief Worker's fn() return value, published before the join wake.
    _Atomic(void *)    join_result;
    /** @brief Worker's 64-bit exit code (default 0); see n00b_thread_exit /
     * n00b_thread_exit_code.
     */
    _Atomic(uint64_t)  exit_code;
    /* @brief Spawn attributes.  Set from the n00b_thread_spawn `_kargs`
     * carried spawner->launcher in the bundle, then copied onto this struct
     * by the launcher on the worker (where n00b_thread_self() resolves).  All three live
     * in the GC-visible, non-moving user_pool WITH the struct  */
    n00b_string_t     *name; ///< Caller-supplied OS thread name (nullptr = unnamed).
    // @brief Run once on worker exit, before the join wake (nullptr = none).
    n00b_finalizer_t   finalizer;
    void              *finalizer_data; ///< Opaque argument passed to @ref finalizer.
    /* @brief Scheduling attributes.  The REQUESTED tier and
     * the optional raw per-OS escape are recorded here by the launcher (on the
     * worker, after applying them to the OS primitive) so the request survives
     * for introspection even when the OS could not grant it (fail-soft). */
    n00b_thread_tier_t sched_tier; ///< Requested tier (N00B_THREAD_TIER_DEFAULT = none).
    bool sched_raw_set; ///< true when the raw escape was supplied (overrides @ref sched_tier).
    // @brief Raw per-OS policy/priority (valid iff @ref sched_raw_set).
    n00b_thread_sched_raw_t     sched_raw;
    /* @brief Affinity attribute.  The REQUESTED CPU-id set is
     * recorded here by the launcher (on the worker, after applying it to the OS
     * primitive) so the request survives for introspection even when the OS
     * could not honor it (a mask == 0 means none was requested).  Hard pin on
     * Linux/Win32, advisory on macOS — see @ref n00b_thread_cpuset_t. */
    n00b_thread_cpuset_t        affinity; ///< Requested CPU affinity set (mask == 0 = none).
    n00b_thread_crash_handler_t crash_handler; ///< Registered crash callback (nullptr = none)
    void                 *crash_handler_data;  ///< Opaque argument passed to @ref crash_handler
    void                 *tcb;       ///< Linux raw-clone TCB (nullptr on other platforms).
    struct n00b_thread_t *reap_next; ///< Pending-reap queue link (rt->reap_pending).
    _Atomic(struct n00b_epoch_hdr_t *) retire_list;
    /// Nesting depth of n00b_epoch_acquire/yield on this thread. Epoch-protected
    /// operations nest (a dict op allocates -> OOB metadata registration ->
    /// n00b_md_put/get is itself a dict op). Only the outermost acquire records a
    /// reservation and only the outermost yield clears it, so a nested yield can
    /// no longer drop the outer op's protection. Single-thread field (only the
    /// owning thread mutates it), so it needs no atomics.
    uint32_t             epoch_depth;
    /**
     * @brief macOS: the worker's Mach thread port for the OS-death check.
     *
     * Captured by the worker with `mach_thread_self()` before it is published.
     * The death edge is this port becoming unreachable after the detached
     * pthread returns, detected by `thread_info(mach_port, …)` returning a
     * non-`KERN_SUCCESS` error. The reaper deallocates the port name after
     * confirming death. 0 on non-macOS and for the main thread. A fork()
     * child re-captures it for the forking thread, since port names do not
     * survive fork.
     */
    uint32_t                 os_thread_port;
    /**
     * @brief Linux: pointer to the CLONE_CHILD_CLEARTID child-tid word (death edge).
     *
     * clone()'s `ctid` argument is fixed at thread-create time, but the
     * permanent `n00b_thread_t` is allocated later by the worker itself
     * (`n00b_thread_init`), so the word the kernel clears cannot live inside
     * this struct.  It lives in the per-spawn bundle (stable, non-moving
     * system_pool, never relocated) and clone() is pointed at it; the launcher
     * records the word's address here so the reaper can read it via `self`.
     * At true thread exit the kernel writes 0 to that word and futex-wakes it
     * (the raw `CLONE_CHILD_CLEARTID` contract — no libc helper).  The spawner
     * seeds the word nonzero, so a 0 there is the unambiguous death edge.
     * nullptr on non-Linux and for the main thread.. */
    _Atomic(uint32_t)       *child_tid_word;
    /**
     * @brief Windows: owning thread handle used as the worker death edge.
     *
     * The spawner keeps the handle returned by CreateThread and the worker
     * copies it onto its permanent struct before publishing ready. The reaper
     * waits until the handle is signaled before recycling the n00b callstack,
     * then closes the handle. nullptr on non-Windows and for non-raw workers.
     */
    void                 *os_thread_handle;
    /**
     * @brief The thread's OS id, as @ref n00b_os_thread_id returns it.
     *
     * Captured on the thread itself during init, for workers, main and
     * attached foreign threads alike; 0 until then. In a fork() child a
     * pthread_atfork handler re-reads it for the forking thread. Linux uses
     * it as the `tgkill` target for STW suspension and Windows to
     * `OpenThread` + `SuspendThread`. On Linux @ref n00b_thread_os_id also
     * reads it as the lock-owner key. */
    uint32_t                 os_tid;
};

// Worker-slot reservation sentinel.  n00b_thread_spawn pre-acquires a thread
// slot and parks this placeholder in rt->threads[slot].thread until the
// worker's n00b_thread_init swaps in its real, registered n00b_thread_t.
// During that window the slot is NON-NULL but is NOT a dereferenceable thread:
// the worker is blocked on the critical-execution gate (it cannot run n00b
// code, hold GC roots, or be suspended), so every slot walker -- STW
// suspend/restart, the GC stack scan and pin prepass, the foreign-self
// diagnostics, the pre-STW quarantine -- MUST treat it exactly like an empty
// slot and never dereference it.  A bare `!t` null check does NOT catch it, so
// walkers fault on the first field access.  The value is all-ones (~0), not a
// small tag: it can never collide with a low-bit-tagged pointer and is
// unmistakably a non-address, so `is_vacant` below is the ONLY correct gate.
#define N00B_THREAD_SLOT_PLACEHOLDER ((n00b_thread_t *)~(uintptr_t)0)

// True when a thread-slot load holds no real registered thread: either empty
// (never acquired / cleared) or the pre-acquired spawn placeholder above.  Use
// this instead of a bare `!t` at every rt->threads[].thread walk site.
static inline bool
n00b_thread_slot_is_vacant(const volatile n00b_thread_t *t)
{
    return t == nullptr || t == N00B_THREAD_SLOT_PLACEHOLDER;
}

/**
 * @brief Get a pointer to the calling thread's runtime-owned n00b_thread_t.
 *
 * Evaluates to the calling thread's `n00b_thread_t *`, or nullptr when
 * called before the runtime / main thread is registered.  O(1),
 * lock-free, TLS-free.
 *
 * This is a function-like **macro** (statement expression), not a
 * function, by design: ncc emits a GC stack-map push in every
 * framed function's prologue, and that push calls `n00b_thread_self()`.
 * A framed `n00b_thread_self()` would therefore recurse through its own prologue.
 * As a macro it has no prologue to instrument, and its entire body is
 * instrumentation-free — it expands to pure atomic loads (compiler
 * intrinsics via the `n00b_atomic_load` macro), `__builtin_frame_address`
 * for the SP, and masking arithmetic, calling no framed function.  In
 * particular it reaches the runtime through the `n00b_default_runtime`
 * global with the `n00b_option_*` macros rather than any accessor.
 *
 * Call sites are unchanged: `n00b_thread_self()` reads identically and
 * `&n00b_thread_self()->field` remains a valid address-of-a-field.
 *
 * Recovery, instrumentation-free on every branch:
 *   - Startup window (no runtime / no thread table): the bootstrap struct.
 *   - Main thread: an O(1) range check of the SP against the kernel-stack
 *     bounds published in N00B_MAIN_THREAD_SLOT's record at init.
 *   - Worker / non-main / FOREIGN: a foreign-safe bounds scan.  Every
 *     registered thread (n00b worker, main, or an attached foreign thread)
 *     publishes its real `stack_lo`/`stack_hi` into its slot record at init
 *     and sets its bit in `rt->live_slot_bits`.  This branch walks the live
 *     bits and returns the thread whose `[stack_lo, stack_hi)` brackets the
 *     SP.  It reads ONLY runtime memory (`rt->live_slot_bits`, `rt->threads[]`)
 *     — never the caller's own stack region — so it cannot fault, and it
 *     resolves a not-yet-attached foreign thread to nullptr (it brackets no
 *     live slot), which the GC-stack-push pre-registration window tolerates.
 *     `live_slot_bits` is set AFTER bounds are published and cleared BEFORE
 *     they are torn down, so a set bit always pairs with valid bounds; that
 *     gates the walk to live threads (O(live), not O(max_threads)).  Inline,
 *     lock-free — atomic loads + ctz + masking, no framed call.
 *
 *     This replaced the earlier O(1) masked id-word read at `base + S - 8`
 *     which FAULTED for foreign (non-callstack) threads whose
 *     masked region top is unmapped — e.g. the Crayon gateway's libdispatch
 *     reply threads (fill_es_subscription_bits / enqueue_payload), which are
 *     foreign to n00b yet reach n00b_thread_self() via the gc-stack-push
 *     prologue.  The bounds scan never dereferences the caller's region top,
 *     so it is sound for any thread.
 *
 * @note Each translation unit that uses this macro must have the full
 *       `n00b_runtime_t` definition (`core/runtime.h`) in scope, since
 *       the body dereferences `rt->threads[]` / `rt->max_threads`.  thread.h
 *       does NOT include core/runtime.h: runtime.h pulls in thread.h
 *       (n00b_thread_record_t is embedded by value in n00b_runtime_t's
 *       thread table), so including it here is a hard circular dependency.
 *       Callers therefore include core/runtime.h themselves (every current
 *       n00b_thread_self() caller already does, via the N00b internal-API umbrella).
 */

#define n00b_thread_self()                                                                      \
    ({                                                                                          \
        n00b_thread_t *_bl_result;                                                              \
        /* ONE load, then use that value. n00b#337: this used to test                           \
         * n00b_default_runtime_is_set() and then separately call                               \
         * n00b_get_runtime(), so a thread preempted between them read the                      \
         * pointer AFTER n00b_shutdown cleared it and dereferenced null --                      \
         * fault_addr=0x2, i.e. null plus the offset of ->threads. Every                         \
         * deref below is off this local, so the value cannot change                            \
         * underneath them mid-macro. */                                                        \
        n00b_runtime_t *_bl_rt = n00b_default_runtime_or_null();                                \
        if (_bl_rt == nullptr) {                                                                \
            _bl_result = &_n00b_bootstrap_thread;                                               \
        }                                                                                       \
        else {                                                                                  \
            if (_bl_rt->threads == nullptr) {                                                   \
                _bl_result = &_n00b_bootstrap_thread;                                           \
            }                                                                                   \
            else {                                                                              \
                /* SP captured without adding a frame: never                                    \
                 * dereferenced, used only for range checks / masking. */                       \
                void                 *_bl_sp   = __builtin_frame_address(0);                    \
                n00b_thread_record_t *_bl_main = &_bl_rt->threads[N00B_MAIN_THREAD_SLOT];       \
                /* stack_lo is published last (release gate), so a non-null                     \
                 * load implies stack_hi is visible too. */                                     \
                void                 *_bl_lo   = n00b_atomic_load(&_bl_main->stack_lo);         \
                void                 *_bl_hi   = n00b_atomic_load(&_bl_main->stack_hi);         \
                if (_bl_lo == nullptr && !n00b_atomic_load(&_bl_rt->startup_complete)) {        \
                    /* Main-slot bounds unset during runtime bootstrap. */                      \
                    _bl_result = &_n00b_bootstrap_thread;                                       \
                }                                                                               \
                else {                                                                          \
                    _bl_result = nullptr;                                                        \
                    /* Windows records TEB->StackLimit at init, but that is the current          \
                     * committed low address, not the reserved stack low address.  A later        \
                     * large frame can legitimately commit more pages below it.  When the         \
                     * current OS stack is still the main stack, refresh the low bound before     \
                     * deciding this is a foreign/null self. */                                  \
                    N00B_REFRESH_WINDOWS_STACK_BOUNDS(_bl_main, _bl_sp, _bl_lo, _bl_hi);         \
                }                                                                               \
                if (_bl_result == nullptr                                                       \
                    && _bl_lo != nullptr && _bl_sp >= _bl_lo && _bl_sp < _bl_hi) {               \
                    /* Main-thread O(1) range check. */                                         \
                    _bl_result = n00b_atomic_load(&_bl_main->thread);                           \
                }                                                                               \
                else if (_bl_result == nullptr) {                                               \
                    /* WORKER / FOREIGN path.                                                   \
                     * FAST PATH (n00b worker, O(1)): if the SP's S-aligned                     \
                     * masked base is a registered callstack region (an O(1)                    \
                     * probe of rt->callstack_base_set), the id word at                         \
                     * base+S-8 is guaranteed mapped, so read it bracket the SP.                \
                     * This is the runtime's hottest call (every gc-framed                      \
                     * prologue) and MUST stay O(1): a prior O(live) bounds                     \
                     * scan here cost ~460 ns/call and throttled the gateway                    \
                     * ingest worker below the event rate (unbounded queue ->                   \
                     * RSS blow-up).                                                            \
                     * SLOW PATH (FOREIGN libdispatch/XPC thread — NOT on a                   \
                     * callstack region, so base+S-8 may be unmapped and must                   \
                     * NEVER be read): foreign-safe bounds scan over live                       \
                     * slots, gated by live_slot_bits.  Foreign entry is rare                   \
                     * (event boundaries), so O(live) is fine.  A not-yet-                      \
                     * attached foreign thread brackets no slot -> nullptr,                     \
                     * which gc_stack_push tolerates until attach.  */                          \
                    _bl_result          = nullptr;                                              \
                    uintptr_t _bl_base  = (uintptr_t)_bl_sp & N00B_CALLSTACK_REGION_MASK;       \
                    bool      _bl_is_cs = false;                                                \
                    if (_bl_rt->callstack_base_set != nullptr) {                                \
                        uintptr_t _bl_h = (_bl_base / N00B_CALLSTACK_REGION_SIZE)               \
                                        & _bl_rt->callstack_base_set_mask;                      \
                        for (;;) {                                                              \
                            uintptr_t _bl_e                                                     \
                                = n00b_atomic_load(&_bl_rt->callstack_base_set[_bl_h]);         \
                            if (_bl_e == _bl_base) {                                            \
                                _bl_is_cs = true;                                               \
                                break;                                                          \
                            }                                                                   \
                            if (_bl_e == 0) {                                                   \
                                break;                                                          \
                            }                                                                   \
                            _bl_h = (_bl_h + 1) & _bl_rt->callstack_base_set_mask;              \
                        }                                                                       \
                    }                                                                           \
                    if (_bl_is_cs) {                                                            \
                        uint64_t *_bl_idw = (uint64_t *)(_bl_base + N00B_CALLSTACK_REGION_SIZE  \
                                                         - N00B_CALLSTACK_ID_WORD_SIZE);        \
                        uint64_t  _bl_id  = *_bl_idw;                                           \
                        if (_bl_id < (uint64_t)_bl_rt->max_threads) {                           \
                            n00b_thread_record_t *_bl_r   = &_bl_rt->threads[_bl_id];           \
                            void                 *_bl_rlo = n00b_atomic_load(&_bl_r->stack_lo); \
                            void                 *_bl_rhi = n00b_atomic_load(&_bl_r->stack_hi); \
                            if (_bl_rlo != nullptr && _bl_sp >= _bl_rlo && _bl_sp < _bl_rhi) {  \
                                _bl_result = n00b_atomic_load(&_bl_r->thread);                  \
                            }                                                                   \
                        }                                                                       \
                    }                                                                           \
                    else {                                                                      \
                        uint32_t _bl_nwords = (_bl_rt->max_threads + 63u) >> 6;                 \
                        for (uint32_t _bl_w = 0; _bl_w < _bl_nwords && _bl_result == nullptr;   \
                             _bl_w++) {                                                         \
                            uint64_t _bl_word                                                   \
                                = n00b_atomic_load(&_bl_rt->live_slot_bits[_bl_w]);             \
                            while (_bl_word != 0) {                                             \
                                uint32_t _bl_bit = (uint32_t)__builtin_ctzll(_bl_word);         \
                                _bl_word &= _bl_word - 1;                                       \
                                uint32_t              _bl_idx = (_bl_w << 6) + _bl_bit;         \
                                n00b_thread_record_t *_bl_r   = &_bl_rt->threads[_bl_idx];      \
                                void *_bl_rlo = n00b_atomic_load(&_bl_r->stack_lo);             \
                                if (_bl_rlo == nullptr) {                                       \
                                    continue;                                                   \
                                }                                                               \
                                void *_bl_rhi = n00b_atomic_load(&_bl_r->stack_hi);             \
                                N00B_REFRESH_WINDOWS_STACK_BOUNDS(                             \
                                    _bl_r, _bl_sp, _bl_rlo, _bl_rhi);                            \
                                if (_bl_sp >= _bl_rlo && _bl_sp < _bl_rhi) {                    \
                                    _bl_result = n00b_atomic_load(&_bl_r->thread);              \
                                    break;                                                      \
                                }                                                               \
                            }                                                                   \
                        }                                                                       \
                    }                                                                           \
                }                                                                               \
            }                                                                                   \
        }                                                                                       \
        _bl_result;                                                                             \
    })

/**
 * @brief Record the current stack top for GC scanning.
 * @param thread Thread whose stack_top to update.
 * @pre @p thread is the calling thread's n00b_thread_t.
 * @post `thread->stack_top` points to the approximate top of the stack frame.
 */
static inline void
n00b_capture_stack_top(n00b_thread_t *thread)
{
    void *ptr;
#if defined(__GNUC__)
    // This dodges a silly warning.
    static volatile uint64_t x = ~0ULL;
    thread->stack_top          = (void *)(((uint64_t)&ptr) & x);
#else
    thread->stack_top = &ptr;
#endif
}

/**
 * @brief Is @p t the main thread?
 *
 * The main thread is the first to register and owns the reserved slot
 * @ref N00B_MAIN_THREAD_SLOT; `n00b_thread_self()`'s O(1) range
 * check already relies on that invariant.  This is the canonical "is
 * main" test.  It replaces the former `os_thread_port == 0` heuristic,
 * which stopped identifying main once the main thread was given a real OS
 * control handle so it can be preemptively suspended for STW like any
 * other thread.
 *
 * @param t Thread to test (nullptr → false).
 * @return true iff @p t is the main thread.
 */
static inline bool
n00b_thread_is_main(n00b_thread_t *t)
{
    return t != nullptr && t->id_info.parts.id == (int32_t)N00B_MAIN_THREAD_SLOT;
}

/**
 * @brief Get the unique (slot + generation) 64-bit thread ID.
 * @return The calling thread's `id_info.unique_id`, or 0 if unregistered.
 *
 * Out-of-line (defined in thread.c) rather than inline: it expands the
 * `n00b_thread_self()` macro, which dereferences the full
 * `n00b_runtime_t`, so it must be compiled where that type is complete —
 * not at every header that includes thread.h.  Unlike `n00b_thread_self()` itself
 * this helper is NOT on the GC stack-map push path, so being a framed
 * function is safe.
 */
extern int64_t n00b_thread_unique_id(void);

/**
 * @brief The calling thread's OS id, the value lock owners are keyed on.
 *
 * Always equal to @ref n00b_os_thread_id. On Linux that is a @c SYS_gettid
 * syscall, so for n00b workers and main this reads the id cached in
 * @c os_tid. Foreign threads always ask the kernel, because after a stack
 * reuse @c n00b_thread_self() can resolve one to a dead thread's record (see
 * @ref n00b_thread_attach_foreign). Elsewhere the kernel read is a register
 * or TEB load and is used directly.
 *
 * @p _t is the caller's @c n00b_thread_self(), possibly nullptr, and is
 * evaluated more than once. A macro so that it adds no GC-framed call to the
 * lock paths.
 */
#if defined(__linux__)
#define n00b_thread_os_id(_t)                                                  \
    (((_t) != nullptr && (_t)->os_tid != 0                                     \
      && ((_t)->callstack != nullptr                                           \
          || (uint32_t)(_t)->id_info.parts.id == N00B_MAIN_THREAD_SLOT))       \
         ? (int64_t)(_t)->os_tid                                               \
         : n00b_os_thread_id())
#else
#define n00b_thread_os_id(_t) ((void)(_t), n00b_os_thread_id())
#endif

/**
 * @brief @ref n00b_thread_os_id for a caller that has not resolved
 * @c n00b_thread_self(). Resolves it only on Linux, where that is cheaper
 * than the syscall.
 */
#if defined(__linux__)
#define n00b_self_os_id()                                                      \
    ({                                                                         \
        n00b_thread_t *_soi_t = n00b_thread_self();                            \
        n00b_thread_os_id(_soi_t);                                             \
    })
#else
#define n00b_self_os_id() n00b_os_thread_id()
#endif

/**
 * @brief Get the current thread's slot index.
 * @return The calling thread's slot id, or 0 if unregistered.
 *
 * Out-of-line for the same reason as n00b_thread_unique_id().
 */
extern int32_t n00b_thread_id(void);

/**
 * @brief Get the OS-level thread id of the calling thread.
 * @return A stable, OS-assigned thread identifier for the running thread.
 *
 * This does NOT resolve through n00b_thread_self() / the TCB: it asks the OS
 * directly (Linux gettid, macOS pthread_threadid_np, Windows GetCurrentThreadId).
 * That matters because a thread holds the critical_execution lock during its
 * WHOLE init and WHOLE destroy, windows in which n00b_thread_self() is not
 * resolvable.  The lock owner / reentrancy key is therefore keyed on this id,
 * not on the runtime slot id (which only indexes rt->threads[]).
 */
extern int64_t n00b_os_thread_id(void);

/**
 * @brief Get the current thread's generation counter.
 * @return The calling thread's generation, or 0 if unregistered.
 *
 * Out-of-line for the same reason as n00b_thread_unique_id().
 */
extern int32_t n00b_thread_generation(void);

/**
 * @brief Spawn a new thread with full n00b lifecycle.
 *
 * Reserves a thread slot, allocates an n00b callstack, and creates a native
 * worker wrapped in the n00b launcher (GC registration, STW participation,
 * lock cleanup on exit). macOS uses a detached pthread on the n00b callstack;
 * Linux uses raw clone and Windows switches a native thread onto that stack.
 *
 * Threads are detached from the spawner by default. A spawned worker
 * needs NO join to avoid a leak.  Its OS resources (the 8 MiB
 * callstack and, on Linux, the minimal TCB) are reclaimed by the runtime reaper
 * at OS-CONFIRMED death (the worker enqueues itself as it exits; the
 * reaper returns the callstack to the callstack pool and frees the
 * TCB only once the OS confirms it is off that stack — macOS dead
 * Mach port / Linux CLONE_CHILD_CLEARTID), and the returned
 * n00b_thread_t struct is GC-owned (it lives in the runtime's
 * GC-visible, non-moving user_pool) and is collected once it becomes
 * unreferenced.  "Detached" is therefore the implicit model — there
 * is no `.detached`/`.attached` attribute: a thread is reaped
 * and collected regardless of whether anyone joins it.  "Joinable"
 * simply means the caller KEPT the returned handle: while that handle
 * is held in a GC-scanned location it keeps the struct alive, the
 * caller may n00b_thread_join() it for the result (see below), and
 * the caller may safely read the handle's fields after the worker has
 * died.  Dropping the last reference to a dead worker's handle lets
 * the GC collect the struct.
 *
 * The positional `(fn, arg)` call shape is the common case and stays
 * source-compatible.  Optional spawn attributes are supplied as keyword
 * arguments:
 *
 * @param fn   Thread entry point.
 * @param arg  Argument passed to @p fn.
 *
 * @kw name           OS thread name applied where supported and stored on the worker's
 *                    n00b_thread_t::name.  nullptr (default) leaves the
 *                    worker unnamed.  Linux applies it via raw
 *                    prctl(PR_SET_NAME); Win32 via SetThreadDescription;
 *                    macOS stores it on the struct only.
 * @kw finalizer      Cleanup callback run EXACTLY ONCE on the worker as it
 *                    exits, BEFORE the join handshake wakes the joiner
 *                    (so a joiner never observes "done" before cleanup).
 *                    nullptr (default) means no finalizer.  This is NOT a GC
 *                    struct finalizer: the n00b_thread_t is GC-owned, so the
 *                    finalizer runs in the launcher exit path on the worker,
 *                    not via the GC.
 * @kw finalizer_data Opaque argument passed to @p finalizer (default nullptr).
 * @kw custom_stack   Pointer to a caller-owned backing region (base + size)
 *                    for the worker's callstack.  When non-null, n00b
 *                    lays its `S`-aligned power-of-2 + ID-word geometry over
 *                    the caller's pages instead of allocating a fresh
 *                    callstack, so the worker still resolves `n00b_thread_self()` in O(1)
 *                    (`n00b_callstack_alloc_over`).  The region MUST be at
 *                    least `2 * N00B_CALLSTACK_REGION_SIZE` bytes (so an
 *                    `S`-aligned sub-region can be carved); an undersized /
 *                    un-alignable region fails the spawn with
 *                    `N00B_ERR_CALLSTACK_REGION_UNUSABLE`.  OWNERSHIP STAYS
 *                    WITH THE CALLER: n00b never frees or unmaps the region
 *                    (the reaper, which reclaims the callstack at OS-confirmed
 *                    death, only drops the mmap-tree registration for a
 *                    caller-owned region so the GC tree stays balanced — it
 *                    does not unmap the caller's pages); the caller must keep
 *                    the region alive until the worker has finished and free
 *                    it itself afterward.  The descriptor itself need only
 *                    live for the duration of the spawn call (it is copied).
 *                    The default (nullptr) allocates an n00b callstack as
 *                    usual.  This is also the mechanism that closes the Win32
 *                    `n00b_thread_self()` gap: a Win32 worker run on the carved
 *                    n00b region resolves identity.  NOTE: `n00b_thread_spawn`
 *                    takes no `.allocator` kwarg, so the small `n00b_callstack_t`
 *                    bookkeeping struct is taken from the default allocator
 *                    regardless of the caller's arena context (the backing
 *                    pages themselves are the caller's own).
 * @kw priority       Requested scheduling tier (@ref n00b_thread_tier_t,
 *                    default N00B_THREAD_TIER_DEFAULT = leave OS default).
 *                    Applied to the worker on the worker itself via the per-OS
 *                    raw primitive (macOS Mach thread_policy_set; Linux raw
 *                    sched_setscheduler/setpriority; Win32 SetThreadPriority).
 *                    `.priority` and `.scheduler` are two names for the SAME
 *                    tier request (the normalized model folds policy+priority
 *                    into one ordered tier); when both are set the launcher
 *                    uses the higher.  An ungrantable privileged tier (e.g.
 *                    realtime without privilege) FAILS SOFT — the spawn still
 *                    succeeds and the requested tier is recorded on
 *                    n00b_thread_t::sched_tier.  See @ref n00b_thread_tier_t
 *                    for the full per-OS mapping table.
 * @kw scheduler      Alias for @kw priority (same tier request; see above).
 * @kw sched_raw      Optional pointer to a raw per-OS scheduling descriptor
 *                    (@ref n00b_thread_sched_raw_t).  When non-null it BYPASSES
 *                    the tier mapping and the launcher applies its {policy,
 *                    priority} directly to the OS primitive on the worker.
 *                    Also fails soft.  The descriptor is copied; it need only
 *                    live for the duration of the spawn call.  Default nullptr
 *                    (use the tier path).
 * @kw affinity       Optional pointer to a CPU-id set (@ref n00b_thread_cpuset_t)
 *                    applied to the worker on the worker itself.  HARD-PINS the
 *                    worker on Linux (raw sched_setaffinity) and Win32
 *                    (SetThreadAffinityMask); on macOS the apply is ADVISORY
 *                    ONLY (Mach thread_policy_set THREAD_AFFINITY_POLICY — a
 *                    scheduler hint, not a pin; tests must not assert a pin
 *                    there).  Fails soft: an un-honorable set leaves the worker
 *                    unpinned and the spawn still succeeds, with the REQUESTED
 *                    set recorded on n00b_thread_t::affinity.  An empty set
 *                    (mask == 0) or nullptr (default) requests no affinity.  The
 *                    descriptor is copied; it need only live for the duration of
 *                    the spawn call.  NUMA co-location is DEFERRED and expressed
 *                    meanwhile via this set — see @ref n00b_thread_cpuset_t.
 * @kw isolation      When true (default false), the worker runs ISOLATED from
 *                    the GC's conservative stack scan: the collector does NOT
 *                    treat the worker's raw C stack as a root source (it is
 *                    excluded from the conservative range scan in
 *                    n00b_scan_thread_stacks; the flag is recorded on
 *                    n00b_thread_t::gc_isolated and applied on the worker by
 *                    the launcher).  The worker's thread struct, its
 *                    n00b_thread_record_t, and its lock chains are STILL
 *                    scanned — isolation never corrupts the GC's view of the
 *                    worker's locks; only the worker's own C-stack words are
 *                    no longer scanned conservatively.
 *
 *                    SELF-REGISTRATION CONTRACT (caller responsibility): an
 *                    isolated worker MUST itself keep alive any GC-heap object
 *                    it holds, because the collector will no longer find that
 *                    object by scanning the worker's C stack.  It does so by
 *                    EITHER (a) being compiled with exact GC stack maps so its
 *                    live roots are published via the compiler's GC stack-frame
 *                    push (gc_stack_policy != N00B_GC_STACK_CONSERVATIVE — note
 *                    an exact-scanned worker is already not conservatively
 *                    scanned, so `.isolation` matters specifically for a
 *                    conservatively-scanned worker that wants OUT), OR (b)
 *                    registering the memory as a persistent GC root via
 *                    n00b_gc_register_root() / _n00b_gc_register_root() (see
 *                    include/core/gc.h), OR (c) keeping every such object also
 *                    reachable from an already-scanned root (the thread struct,
 *                    a registered global, or a non-isolated thread's stack).
 *                    A heap object that is reachable ONLY from an isolated
 *                    worker's C stack and is none of the above WILL be
 *                    collected as garbage → use-after-free.
 *
 * @kw crash_handler  Optional per-thread crash callback
 *                    (@ref n00b_thread_crash_handler_t, default nullptr = none)
 *                    STORED on n00b_thread_t::crash_handler.
 * @kw crash_handler_data Opaque argument passed to @kw crash_handler (default
 * nullptr).  Stored on n00b_thread_t::crash_handler_data.
 *
 * @return     The spawned thread, or an error code (ENXIO, ENOMEM, the raw
 *             OS thread-creation failure code, or a negative
 *             `N00B_ERR_CALLSTACK_*` for an unusable `custom_stack` region).
 *
 * @pre  Runtime must be initialized.
 * @post The new thread participates in GC stop-the-world.
 */
extern n00b_result_t(n00b_thread_t *) n00b_thread_spawn(void *(*fn)(void *), void *arg) _kargs
{
    n00b_string_t              *name               = nullptr;
    n00b_finalizer_t            finalizer          = nullptr;
    void                       *finalizer_data     = nullptr;
    n00b_callstack_region_t    *custom_stack       = nullptr;
    n00b_thread_tier_t          priority           = N00B_THREAD_TIER_DEFAULT;
    n00b_thread_tier_t          scheduler          = N00B_THREAD_TIER_DEFAULT;
    n00b_thread_sched_raw_t    *sched_raw          = nullptr;
    n00b_thread_cpuset_t       *affinity           = nullptr;
    bool                        isolation          = false;
    n00b_thread_crash_handler_t crash_handler      = nullptr;
    void                       *crash_handler_data = nullptr;
};

/**
 * @brief Wait for a spawned thread to finish and read its result (RESULT-ONLY).
 * @param thread  Thread to join (from n00b_thread_spawn). nullptr returns nullptr.
 * @return        Thread return value (the worker's `void *` fn-return).
 *
 * RESULT-ONLY.  This call WAITS for the worker to publish
 * "done", then returns the worker's `void *` fn-return.  It FREES NOTHING —
 * not the n00b_thread_t struct, not the worker's callstack, not its TCB.  It
 * is NEVER required to avoid a leak and is NEVER the cleanup path: the runtime
 * reaper reclaims the callstack/TCB at OS-CONFIRMED death (the worker is still
 * on its stack at the moment a joiner observes "done", so reclamation cannot
 * happen here), and the GC reclaims the struct once it becomes unreferenced
 * (the struct lives in the GC-visible user_pool).  Joining is therefore purely
 * a synchronization-plus-result operation; a worker that is never joined is
 * still reaped and collected (the default-detached model — see
 * @ref n00b_thread_spawn).
 *
 * The `void *` return is the worker's fn() return value and is INDEPENDENT
 * of the 64-bit exit code: a worker can both return a `void *`
 * and stash a separate exit code via n00b_thread_exit().  Read the exit
 * code with n00b_thread_exit_code() after this call returns; it remains
 * readable for as long as the handle is held (the worker settles the code
 * before the wake, and join frees nothing).
 */
extern void *n00b_thread_join(n00b_thread_t *thread);

/**
 * @brief Stash a 64-bit exit code for the calling worker (STASH-ONLY).
 * @param code  The 64-bit exit code to publish (default semantics: 0).
 *
 * Stores @p code into the calling worker's n00b_thread_t::exit_code and
 * RETURNS — it does NOT terminate the worker mid-function.  The
 * worker's fn() continues to run normally; the launcher publishes the
 * stashed code (the last value passed here, or 0 if never called) when the
 * worker exits, BEFORE the join handshake wakes the joiner.  Because the
 * exit code is a separate channel from the fn()-return, calling this does
 * NOT change the `void *` value n00b_thread_join() returns.
 *
 * If called more than once, the last value wins (each call overwrites the
 * stashed code).  Calling it from the main thread or a thread that is not
 * a spawned worker resolving via n00b_thread_self() is a no-op.
 *
 * @pre  The runtime is initialized and the caller resolves via
 *       n00b_thread_self() (a spawned worker on its n00b callstack).
 * @post n00b_thread_exit_code() on this thread, observed by a joiner after
 *       a successful join, yields @p code.
 */
extern void n00b_thread_exit(uint64_t code);

/**
 * @brief Read a thread's published 64-bit exit code.
 * @param thread  The thread whose exit code to read.
 * @return        The exit code @p thread stashed via n00b_thread_exit(),
 *                or 0 if it never called n00b_thread_exit().
 *
 * READ-VALIDITY CONTRACT: the returned value is meaningful only AFTER a
 * successful n00b_thread_join(thread).  The worker publishes its exit code
 * before the join_futex publish-then-wake, so a joiner that completed the
 * handshake reads the settled value; reading before the join is not
 * guaranteed to observe the final code.  The exit code is distinct from the
 * `void *` value returned by n00b_thread_join.
 *
 * Out-of-line (defined in thread.c) for parity with the other thread
 * accessors; it does not expand n00b_thread_self() (it reads @p thread
 * directly), so being a framed function is safe.
 */
extern uint64_t n00b_thread_exit_code(n00b_thread_t *thread);

/**
 * @brief Check whether @p ptr is inside the calling thread's stack.
 *
 * This is stricter than checking the global mmap registry: some
 * platforms expose the main stack as multiple VM regions.  The
 * authoritative span is the registered n00b_mmap_stack region (the
 * OS-native main-stack bounds, or a worker's n00b callstack).
 */
extern bool n00b_current_thread_stack_contains(void *ptr);

/**
 * @brief Get this thread's persistent transient-scratch pool, creating it on
 *        first use.
 *
 * For short-lived working buffers that the caller frees itself (n00b_free
 * returns the slot to the pool).  Keeps such transients off the GC default
 * arena without per-call pool create/destroy or shared-lock contention.  The
 * pool is non-GC (hidden), reused for the thread's life, and destroyed at
 * thread death.  Returns nullptr before the calling thread is registered.
 */
extern n00b_allocator_t *n00b_thread_scratch_pool(void);

#if defined __N00B_THREAD_INTERNAL
/**
 * @brief Initialize the calling thread's n00b_thread_t (internal).
 *
 * @kw runtime       Runtime to register with.
 * @kw acquired_slot Pre-acquired thread slot index (0 = auto-assign).
 * @kw callstack     The worker's OS callstack (nullptr for the main thread).
 *                   When set, the thread's stack bounds are taken directly
 *                   from the callstack region (the worker self-describes its
 *                   bounds; no OS stack query is needed) and recorded on the
 *                   thread so the n00b_thread_self() worker-masking back-check resolves.
 * @kw os_thread_port macOS only: the worker's retained Mach control port,
 *                   captured before runtime initialization. Set on the thread BEFORE it is
 *                   published as a STW participant so the pure-preemptive STW can
 *                   suspend it from the instant it appears.  0
 *                   (default) for the main thread and on Linux/Windows, which
 *                   use os_tid (read from the running thread during init).
 *
 * @pre Runtime must be initialized.
 * @post The calling thread is registered in the runtime's thread table
 *       and participates in STW pauses.
 */
void
n00b_thread_init() _kargs
{
    n00b_runtime_t          *runtime            = n00b_get_runtime();
    uint32_t                 acquired_slot      = 0;
    struct n00b_callstack_t *callstack          = nullptr;
    uint32_t                 os_thread_port     = 0;
    // FOREIGN threads (no n00b callstack) MUST pass their real stack bounds
    // here — the runtime does not discover them.  Omitted => the C stack is not
    // GC-scanned (self-register roots).  A foreign thread MUST n00b_thread_destroy.
    void                    *foreign_stack_low  = nullptr;
    void                    *foreign_stack_high = nullptr;
};

/**
 * @brief Tear down the calling thread's n00b_thread_t.
 *
 * Releases any locks still held, clears the thread record, and
 * decrements `live_threads`.
 *
 * @pre  The calling thread was previously initialized via n00b_thread_init().
 * @post The thread slot is available for reuse.
 */
extern void n00b_thread_destroy(void);

/**
 * @brief Record the stack base address for a thread.
 * @param thread  Thread to update.
 * @param runtime Runtime owning the thread.
 */
extern void n00b_capture_stack_base(n00b_thread_t  *thread,
                                    n00b_runtime_t *runtime,
                                    void           *foreign_stack_low,
                                    void           *foreign_stack_high);

/**
 * @brief Reaper backstop: reclaim OS-confirmed-dead workers (internal).
 *
 * Sweeps the runtime reap-pending queue, returning the
 * callstack of every worker whose OS death edge has fired (macOS dead Mach port
 * / Linux CLONE_CHILD_CLEARTID futex) to the callstack pool, freeing its TCB,
 * and (macOS) deallocating its thread port; workers not yet confirmed dead stay
 * queued.  Bounded (one queue walk).  Called as a PROMPT backstop from the
 * conduit signal thread's poll loop so unheld detached workers are reaped
 * without waiting for the next spawn (the other reaper driver is the
 * callstack-pool slow path inside n00b_thread_spawn).  No dedicated reaper
 * thread.  A no-op when the runtime is not yet initialized.
 */
extern void n00b_thread_reap_pending(void);

/**
 * @brief Reclaim dead unmanaged thread records (collector-only, internal).
 *
 * Foreign (libdispatch/XPC) threads attach via n00b_thread_init but may never
 * call n00b_thread_destroy — libdispatch recycles them silently — so their
 * slot, n00b_thread_t, stack-bounds advertisement, mmap-tree node and Mach
 * port name leak.  Daemons that enter dispatch_main() can also retire the
 * original main thread while the process continues on dispatch workers.  This
 * scans the slot table for unmanaged records whose OS thread is confirmed dead
 * and tears each down exactly like n00b_thread_destroy.  MUST be called by the
 * collector with the world stopped (from n00b_collect_internal, after
 * mark+sweep): the teardown mutates CV-waiter/lock chains and the mmap
 * interval tree, unsafe to touch concurrently.  macOS only; a no-op elsewhere.
 */
extern void n00b_reap_dead_foreign_threads(void);

/**
 * @brief Quarantine an OS-dead unmanaged slot during the STW stop pass.
 *
 * The full unmanaged-thread reaper runs after the world is stopped, but the stop
 * pass itself must first walk the thread table and suspend every live slot.  A
 * dead libdispatch/XPC or retired dispatch_main() slot cannot be suspended, and
 * retrying that slot forever wedges STW before the full reaper can run.  This
 * helper detects that case and atomically removes the dead slot from stack
 * matching/scanning while leaving the record installed for the later under-STW
 * reaper cleanup.
 *
 * @return true iff @p t was an OS-dead unmanaged thread and was quarantined.
 */
extern bool n00b_thread_quarantine_dead_foreign_for_stw(n00b_thread_record_t *rec,
                                                        n00b_thread_t        *t);

/**
 * @brief Sound foreign-thread attach (reclaim-on-attach), macOS only.
 *
 * Call at every foreign (libdispatch/XPC) entry into the n00b runtime, before
 * any other n00b work.  Ensures the calling thread resolves n00b_thread_self()
 * to a record IT owns: if it instead resolved to a dead thread's stale record
 * (its SP fell inside that dead thread's still-advertised stack bounds after a
 * libdispatch stack reuse), this detaches from the stale record and attaches a
 * fresh slot.  Idempotent (cheap no-op once the thread owns its record).
 * Returns the thread's own n00b_thread_t.  See the definition for the full
 * rationale (this session's stale-bounds identity crash).
 *
 * @kw foreign_stack_low  Low bound of the caller's foreign stack, if known.
 * @kw foreign_stack_high High bound of the caller's foreign stack, if known.
 */
extern n00b_thread_t *
n00b_thread_attach_foreign() _kargs
{
    void *foreign_stack_low  = nullptr;
    void *foreign_stack_high = nullptr;
};

#endif
