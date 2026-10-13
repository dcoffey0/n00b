#define N00B_MEM_INTERNAL_API
#define N00B_USE_INTERNAL_API

#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h> /* getenv/strtol/abort: big-free quarantine (+ N00B_DEBUG) */

#include "n00b.h"
#include "core/alloc_mdata.h"
#include "core/alloc.h"
#include "core/memory_info.h"
#include "core/mmaps.h"
#include "core/atomic.h"

/* Forward decl to avoid pulling in lock_common.h (and its
 * transitive thread-id dependency) at this layer. */
extern void n00b_lock_chains_scrub_range(uint64_t lo, uint64_t hi);
#include "adt/llstack.h"
#include "adt/list.h"
#include "adt/dict.h" /* per-site pool audit dict (N00B_POOL_ALLOC_AUDIT) */
#include "core/align.h"
#include "core/epoch.h"
#include "core/pool.h"
#include "core/alloc_interpose.h"
/* Declared after pool.h so n00b_pool_t is a complete file-scope type. */
extern void n00b_lock_chains_scrub_pool(n00b_pool_t *pool);
#include "core/runtime.h"
#include "util/math.h"

#define N00B_POOL_GLOBAL_REGISTRY_MAX 65536

// Lock-free registry: slots are claimed/released with per-slot CAS on
// `pool`, so registration can never block — the GC collector registers its
// work pool DURING stop-the-world, where waiting on any lock a suspended
// thread might hold is a deadlock. `name` is published after the claiming
// CAS; a reader must tolerate a null/stale name on a just-claimed slot.
// n00b_pool_unlisted_owner reads only `pool`, one slot per lookup.
typedef struct {
    _Atomic(n00b_pool_t *) pool;
    _Atomic(const char *)  name;
} n00b_pool_registry_entry_t;

static n00b_pool_registry_entry_t n00b_pool_registry[N00B_POOL_GLOBAL_REGISTRY_MAX];
static _Atomic uint64_t           n00b_pool_registry_init_count;
static _Atomic uint64_t           n00b_pool_registry_destroy_count;
static _Atomic uint64_t           n00b_pool_registry_overflow_count;
static _Atomic uint64_t           n00b_pool_destroy_unmap_count;
static _Atomic uint64_t           n00b_pool_destroy_unmap_bytes;
static _Atomic uint64_t           n00b_pool_destroy_unmap_fail_count;
static _Atomic uint64_t           n00b_pool_destroy_unmap_fail_bytes;
static _Atomic uint64_t           n00b_pool_big_unmap_fail_count;
static _Atomic uint64_t           n00b_pool_big_unmap_fail_bytes;

/* ---------------------------------------------------------------------------
 * Big-free quarantine (diagnostic; env N00B_POOL_BIG_QUARANTINE=<pages>).
 *
 * Every big (page-granular) pool allocation is its own mmap, so a
 * use-after-free of one dereferences unmapped (or, worse, since-remapped)
 * address space, and a double-free unmaps whatever NOW lives at the reused
 * address. Both present as delayed, unattributable crashes far from the bug.
 * When enabled, delete_one_page_entry parks a freed big page here instead of
 * unmapping it: the page is mprotect(PROT_NONE)'d — any later touch faults
 * immediately, with the page still attributable — and recorded with the
 * freeing call stack. The oldest parked page is truly munmapped only when
 * the ring wraps. A free landing inside a still-parked page is a definite
 * double-free: both call stacks are reported and the process aborts. The
 * crash handler calls n00b_pool_quarantine_find() (async-signal-safe: plain
 * atomic loads, no locks) to attribute a faulting address to its freeing
 * site. Off by default; parked pages cost address space, not RSS.
 * ------------------------------------------------------------------------- */

#define N00B_POOL_QUAR_RING_MAX 1024

typedef struct {
    _Atomic(uintptr_t) start; /* 0 = slot empty */
    _Atomic(uint32_t)  busy;  /* per-slot parker claim (see park below) */
    uint64_t           size;
    uint64_t           seq;
    const char        *pool_name;
    void              *frees[N00B_POOL_QUARANTINE_FRAMES];
} n00b_pool_quar_slot_t;

[[n00b::nogc]] static n00b_pool_quar_slot_t
    n00b_pool_quar_ring[N00B_POOL_QUAR_RING_MAX];
static _Atomic uint64_t n00b_pool_quar_cursor;
static _Atomic int32_t  n00b_pool_quar_capacity = -1; /* -1 = env unread */

/* Guard mode (env N00B_POOL_PAGE_PER_ALLOC): route EVERY allocation through
 * the single-entry mmap path so each alloc owns a full page. Combined with
 * the big-free quarantine this extends use-after-free / double-free
 * detection to slab-class allocations — a wrongly freed small object parks
 * its whole page and the next touch (or second free) faults attributably —
 * at the cost of a page plus an mmap syscall per allocation. Value "1" or
 * "all" applies to every pool; any other value is a substring filter on the
 * pool's debug_name (e.g. "system" to target only the system pool). */
static _Atomic int32_t n00b_pool_guard_mode = -1; /* -1 unread, 0 off,
                                                     1 all, 2 filtered */
static const char *n00b_pool_guard_filter;

static bool
pool_guard_page_per_alloc(n00b_pool_t *pool)
{
    int32_t mode = atomic_load(&n00b_pool_guard_mode);
    if (mode < 0) {
        /* Raw getenv: same allocator-layer bootstrap exception as
         * pool_quar_capacity above. */
        const char *env = getenv("N00B_POOL_PAGE_PER_ALLOC");
        int32_t     v   = 0;
        if (env != nullptr && env[0] != '\0') {
            if ((env[0] == '1' && env[1] == '\0')
                || strcmp(env, "all") == 0) {
                v = 1;
            }
            else {
                n00b_pool_guard_filter = env;
                v                      = 2;
            }
        }
        int32_t expected = -1;
        atomic_compare_exchange_strong(&n00b_pool_guard_mode, &expected, v);
        mode = atomic_load(&n00b_pool_guard_mode);
    }
    switch (mode) {
    case 1:
        return true;
    case 2: {
        const char *name = ((n00b_allocator_t *)pool)->debug_name;
        return name != nullptr
               && n00b_pool_guard_filter != nullptr
               && strstr(name, n00b_pool_guard_filter) != nullptr;
    }
    default:
        return false;
    }
}

static uint32_t
pool_quar_capacity(void)
{
    int32_t cap = atomic_load(&n00b_pool_quar_capacity);
    if (cap >= 0) {
        return (uint32_t)cap;
    }
    /* Raw getenv, not n00b_getenv: n00b_getenv allocates its result through
     * the runtime's default allocator, and this file IS the allocator layer —
     * routing a pool-internal gate through it is a bootstrap circularity.
     * Same documented exception as pool_mmap_audit_enabled below. */
    const char *env = getenv("N00B_POOL_BIG_QUARANTINE");
    int32_t     v   = 0;
    if (env != nullptr && env[0] != '\0') {
        v = (int32_t)strtol(env, nullptr, 10);
        if (v < 0) {
            v = 0;
        }
        if (v > N00B_POOL_QUAR_RING_MAX) {
            v = N00B_POOL_QUAR_RING_MAX;
        }
    }
    int32_t expected = -1;
    atomic_compare_exchange_strong(&n00b_pool_quar_capacity, &expected, v);
    return (uint32_t)atomic_load(&n00b_pool_quar_capacity);
}

/* Capture the FREEING call stack. Windows provides a native bounded capture;
 * elsewhere use a best-effort frame-pointer walk. This runs in ordinary
 * context on the free path where a fault is not acceptable, so every manual
 * dereference stays inside the current thread stack. A chain that leaves
 * those bounds simply truncates the capture. */
#define POOL_QUAR_FRAME_WINDOW (8u << 20)

static void
pool_quar_capture_frames(void **out)
{
    for (int i = 0; i < N00B_POOL_QUARANTINE_FRAMES; i++) {
        out[i] = nullptr;
    }
#if defined(_WIN32)
    (void)RtlCaptureStackBackTrace(1,
                                   N00B_POOL_QUARANTINE_FRAMES,
                                   out,
                                   nullptr);
#else
    void **anchor = (void **)__builtin_frame_address(0);
    void **fp     = anchor;
    for (int i = 0; i < N00B_POOL_QUARANTINE_FRAMES; i++) {
        if (fp == nullptr || (((uintptr_t)fp) & 0xf) != 0
            || fp < anchor
            || (uintptr_t)fp - (uintptr_t)anchor > POOL_QUAR_FRAME_WINDOW) {
            break;
        }
        void **next = (void **)fp[0];
        void  *ret  = fp[1];
        if (ret == nullptr) {
            break;
        }
        out[i] = ret;
        if (next <= fp
            || (uintptr_t)next - (uintptr_t)fp > POOL_QUAR_FRAME_WINDOW) {
            break;
        }
        fp = next;
    }
#endif
}

/* Park a freed big page. Returns true when parked (caller must NOT munmap).
 * Aborts on a detected double-free. */
static bool
pool_quarantine_park(n00b_pool_t *pool, void *addr, size_t mapped)
{
    uint32_t cap = pool_quar_capacity();
    if (cap == 0) {
        return false;
    }

    uintptr_t lo = (uintptr_t)addr;

    /* No explicit double-free scan is needed: a second free of a parked page
     * faults inside delete_one_page_entry's own entry-header reads (the page
     * is PROT_NONE), and the crash handler's quarantine lookup attributes it
     * — the faulting stack IS the double-freer, the recorded stack the first
     * free. */

    /* Release the PHYSICAL pages before revoking access: a parked page must
     * cost only address space. PROT_NONE alone leaves the dirty pages
     * resident/compressed until the ring-wrap munmap; MADV_FREE_REUSABLE
     * (macOS: immediate phys_footprint credit; MADV_FREE elsewhere) hands
     * them back now. Must precede the mprotect — the advice needs an
     * accessible mapping. */
#if defined(_WIN32)
    if (!VirtualFree(addr, mapped, MEM_DECOMMIT)) {
        DWORD old_protect;
        (void)VirtualProtect(addr,
                             mapped,
                             PAGE_NOACCESS,
                             &old_protect);
    }
#else
#if defined(__APPLE__)
    (void)madvise(addr, mapped, MADV_FREE_REUSABLE);
#else
    (void)madvise(addr, mapped, MADV_FREE);
#endif

    /* Fault-on-touch before any slot work: the page is already unlinked and
     * private to this freeing thread, so no lock is needed around the
     * syscall. Failure is non-fatal: worst case the page stays readable
     * until the ring evicts it (same as no quarantine).
     *
     * This applies to every POSIX target, macOS included.  MADV_FREE_REUSABLE
     * hands the physical pages back but leaves the mapping READABLE, so
     * without this the page reads as zeroes instead of faulting: the
     * fault-on-touch contract in this file's header comment, the double-free
     * detection below, and the crash handler's quarantine attribution
     * (crash.c) were all silently inert on macOS.  test_pool_alloc's
     * quarantine_uaf_faults asserts the fault and failed here. */
    (void)mprotect(addr, mapped, PROT_NONE);
#endif

    /* Slot claim is PER-SLOT, not a global lock: the cursor hands every
     * parker a distinct sequence, so two frees contend on the same slot only
     * when `cap` parks are in flight at once (ring wrap onto an in-progress
     * slot). The busy bit makes that case correct without ever serializing
     * the normal path. */
    uint64_t               seq  = atomic_fetch_add(&n00b_pool_quar_cursor, 1);
    n00b_pool_quar_slot_t *slot = &n00b_pool_quar_ring[seq % cap];

    while (atomic_exchange(&slot->busy, 1) != 0) {}

    uintptr_t old_start = atomic_load(&slot->start);
    uint64_t  old_size  = slot->size;

    /* Publish order matters for the lock-free crash-time reader: empty the
     * slot first, fill the fields, then publish the new start LAST so a
     * concurrent find() never pairs the new range with stale attribution. */
    atomic_store(&slot->start, (uintptr_t)0);
    slot->size      = (uint64_t)mapped;
    slot->seq       = seq;
    slot->pool_name = ((n00b_allocator_t *)pool)->debug_name;
    pool_quar_capture_frames(slot->frees);
    atomic_store(&slot->start, lo);

    atomic_store(&slot->busy, 0);

    /* Evicted page (if any) is released outside the slot claim. */
    if (old_start != 0) {
        n00b_safe_munmap((void *)old_start, (size_t)old_size);
    }
    return true;
}

n00b_option_t(n00b_pool_quarantine_hit_t)
    n00b_pool_quarantine_find(uintptr_t addr)
{
    int32_t cap = atomic_load(&n00b_pool_quar_capacity);
    if (cap <= 0) {
        return n00b_option_none(n00b_pool_quarantine_hit_t);
    }
    for (int32_t i = 0; i < cap && i < N00B_POOL_QUAR_RING_MAX; i++) {
        n00b_pool_quar_slot_t *s     = &n00b_pool_quar_ring[i];
        uintptr_t              start = atomic_load(&s->start);
        if (start == 0 || addr < start || addr >= start + s->size) {
            continue;
        }
        n00b_pool_quarantine_hit_t hit = {
            .start     = start,
            .size      = s->size,
            .seq       = s->seq,
            .pool_name = s->pool_name,
        };
        for (int f = 0; f < N00B_POOL_QUARANTINE_FRAMES; f++) {
            hit.frees[f] = s->frees[f];
        }
        /* Seqlock-style re-check: park() zeroes start before recycling a
         * slot and republishes it last, so an unchanged start means the
         * fields copied above all belong to this range. A mismatch means
         * the ring wrapped mid-copy — skip rather than misattribute. */
        if (atomic_load(&s->start) != start) {
            continue;
        }
        return n00b_option_set(n00b_pool_quarantine_hit_t, hit);
    }
    return n00b_option_none(n00b_pool_quarantine_hit_t);
}

#define N00B_POOL_PAGE_DIAG_REGISTRY_BITS 18
#define N00B_POOL_PAGE_DIAG_REGISTRY_MAX  (1u << N00B_POOL_PAGE_DIAG_REGISTRY_BITS)
// Linear probing degrades sharply past ~3/4 load, so registrations beyond it
// are counted as overflow instead of stored.
#define N00B_POOL_PAGE_DIAG_REGISTRY_CAP  (N00B_POOL_PAGE_DIAG_REGISTRY_MAX / 4 * 3)

/* Bounded CAS spinlock for the process-wide side tables below (the released
 * page cache and the page-diagnostic registry). After N00B_POOL_SPIN_TRIES
 * failed attempts it reports failure and the caller skips that table's work,
 * which both tables can afford, so an allocator path never blocks on them. */
#define N00B_POOL_SPIN_TRIES 1024

[[n00b::nogc]] static inline bool
pool_spin_trylock(_Atomic uint32_t *lock)
{
    for (uint32_t i = 0; i < N00B_POOL_SPIN_TRIES; i++) {
        uint32_t expected = 0;
        if (atomic_compare_exchange_weak(lock, &expected, 1)) {
            return true;
        }
    }
    return false;
}

[[n00b::nogc]] static inline void
pool_spin_unlock(_Atomic uint32_t *lock)
{
    atomic_store(lock, 0);
}

/* ---- released big-page cache -------------------------------------------
 *
 * A big pool allocation maps its own page and delete_one_page_entry unmaps it
 * again, so a workload that cycles big allocations re-faults every page it
 * touches. Measured on the regex suite (98 cases, arm64): 3.65M minor faults
 * and 22.8s system against 168K faults and 1.8s on macOS for the same work.
 * At ~6us a fault under nested paging that is most of the system time.
 *
 * Keeping the mapping and handing it back out turns each of those faults into
 * a write to memory that is already resident. The pages stay charged to RSS
 * (and on Windows to commit) while cached, so the cache is capped; past the
 * cap a release unmaps. N00B_POOL_PAGE_CACHE_MB overrides the cap, and 0
 * disables the cache entirely. The default is 0 on Windows, where a parked
 * page is commit charge, and N00B_POOL_PAGE_CACHE_DEFAULT_MB elsewhere.
 *
 * A pool created with .page_cache = false never parks its pages: its big
 * frees go straight to munmap. Scratch pools that bound their own footprint
 * (the marshaler's) use this, because a buffer that grows geometrically
 * retires a new size on every grow, and no later request matches it.
 * n00b_pool_page_cache_drain unmaps whatever the cache holds.
 *
 * Parked pages also age out with the collector. Each collection advances the
 * cache's generation and, once it restarts the world, unmaps every page parked
 * two or more generations back. A page no request matched across a whole
 * collection interval is likely a size nothing will ask for again, so it
 * leaves within about two collections of its release.
 *
 * Reuse is by EXACT size: a page goes back to the bucket for its own size and
 * only satisfies a request for that size, so no caller ever receives a
 * mapping shorter than it asked for. Buckets are keyed by the size's bit
 * width, which is dense because the sizes in play are page-rounded and few.
 *
 * Recycled memory is NOT zero-filled the way a fresh mmap is, and pool_alloc
 * documents a zero-fill contract (see the memset in the free-list pop below),
 * so a cache hit memsets before returning. That is the same zeroing the
 * kernel would have done on the fault, minus the trap.
 *
 * Interaction with the big-free quarantine: the quarantine is a diagnostic
 * that deliberately parks pages PROT_NONE to catch use-after-free, so it wins
 * when enabled and this cache never sees those pages. */
#define N00B_POOL_PAGE_CACHE_CLASSES   40
#define N00B_POOL_PAGE_CACHE_PER_CLASS 4
#if defined(_WIN32)
#define N00B_POOL_PAGE_CACHE_DEFAULT_MB 0
#else
#define N00B_POOL_PAGE_CACHE_DEFAULT_MB 768
#endif

/* Rounds a drain retries a busy cache lock, sleeping between them, before it
 * gives up. */
#define N00B_POOL_PAGE_CACHE_DRAIN_ROUNDS   64
#define N00B_POOL_PAGE_CACHE_DRAIN_SLEEP_NS 10000
#define N00B_POOL_PAGE_CACHE_SLOTS                                             \
    (N00B_POOL_PAGE_CACHE_CLASSES * N00B_POOL_PAGE_CACHE_PER_CLASS)

typedef struct {
    void    *addr;
    size_t   size;
    uint64_t gen; // n00b_pool_page_cache_gen when parked
} n00b_pool_cached_page_t;

static n00b_pool_cached_page_t
    n00b_pool_page_cache[N00B_POOL_PAGE_CACHE_CLASSES]
                        [N00B_POOL_PAGE_CACHE_PER_CLASS];
static _Atomic uint32_t n00b_pool_page_cache_lock;
/* Written only under n00b_pool_page_cache_lock; read lock-free by stats. */
static _Atomic uint64_t n00b_pool_page_cache_bytes;
static _Atomic uint64_t n00b_pool_page_cache_hits;
static _Atomic uint64_t n00b_pool_page_cache_misses;
static _Atomic uint64_t n00b_pool_page_cache_parked;
static _Atomic uint64_t n00b_pool_page_cache_evicted;
/* gen advances once per collection; trimmed_gen is the generation the last
 * trim ran at. */
static _Atomic uint64_t n00b_pool_page_cache_gen;
static _Atomic uint64_t n00b_pool_page_cache_trimmed_gen;

[[n00b::nogc]] static inline uint64_t
pool_page_cache_cap(void)
{
    static _Atomic uint64_t cap  = 0;
    static _Atomic bool     seen = false;

    if (atomic_load(&seen)) {
        return atomic_load(&cap);
    }

    const char *env = getenv("N00B_POOL_PAGE_CACHE_MB");
    uint64_t    mb  = N00B_POOL_PAGE_CACHE_DEFAULT_MB;

    if (env && env[0]) {
        char    *end = nullptr;
        long long v  = strtoll(env, &end, 10);
        if (end != env && v >= 0) {
            mb = (uint64_t)v;
        }
    }
    atomic_store(&cap, mb * 1024ull * 1024ull);
    atomic_store(&seen, true);
    return mb * 1024ull * 1024ull;
}

[[n00b::nogc]] static inline uint64_t
pool_page_cache_class(size_t size)
{
    uint64_t ix = 0;
    while ((1ull << ix) < (uint64_t)size && ix < N00B_POOL_PAGE_CACHE_CLASSES - 1) {
        ix++;
    }
    return ix;
}

/* Park a released page. Returns true when the cache took ownership, in which
 * case the mapping stays alive and the caller must NOT unmap it. */
[[n00b::nogc]] static bool
pool_page_cache_put(void *addr, size_t size)
{
    uint64_t cap = pool_page_cache_cap();

    if (cap == 0 || addr == nullptr || size == 0 || (uint64_t)size > cap) {
        return false;
    }
    /* Refuse without the lock when the cache is already too full, then check
     * again under it so concurrent puts cannot overshoot the cap together. */
    if (atomic_load(&n00b_pool_page_cache_bytes) + (uint64_t)size > cap) {
        return false;
    }
    if (!pool_spin_trylock(&n00b_pool_page_cache_lock)) {
        return false;
    }
    if (atomic_load(&n00b_pool_page_cache_bytes) + (uint64_t)size > cap) {
        pool_spin_unlock(&n00b_pool_page_cache_lock);
        return false;
    }

    uint64_t ix = pool_page_cache_class(size);

    for (int i = 0; i < N00B_POOL_PAGE_CACHE_PER_CLASS; i++) {
        if (n00b_pool_page_cache[ix][i].addr == nullptr) {
            n00b_pool_page_cache[ix][i].addr = addr;
            n00b_pool_page_cache[ix][i].size = size;
            n00b_pool_page_cache[ix][i].gen
                = atomic_load(&n00b_pool_page_cache_gen);
            atomic_fetch_add(&n00b_pool_page_cache_bytes, (uint64_t)size);
            pool_spin_unlock(&n00b_pool_page_cache_lock);
            atomic_fetch_add(&n00b_pool_page_cache_parked, 1);
            return true;
        }
    }
    pool_spin_unlock(&n00b_pool_page_cache_lock);
    return false;
}

/* Take a cached page of EXACTLY `size` bytes, or nullptr. */
[[n00b::nogc]] static void *
pool_page_cache_get(size_t size)
{
    if (pool_page_cache_cap() == 0 || size == 0) {
        return nullptr;
    }
    if (!pool_spin_trylock(&n00b_pool_page_cache_lock)) {
        return nullptr;
    }

    uint64_t ix = pool_page_cache_class(size);

    for (int i = 0; i < N00B_POOL_PAGE_CACHE_PER_CLASS; i++) {
        if (n00b_pool_page_cache[ix][i].addr != nullptr
            && n00b_pool_page_cache[ix][i].size == size) {
            void *addr                       = n00b_pool_page_cache[ix][i].addr;
            n00b_pool_page_cache[ix][i].addr = nullptr;
            n00b_pool_page_cache[ix][i].size = 0;
            atomic_fetch_sub(&n00b_pool_page_cache_bytes, (uint64_t)size);
            pool_spin_unlock(&n00b_pool_page_cache_lock);
            atomic_fetch_add(&n00b_pool_page_cache_hits, 1);
            return addr;
        }
    }
    pool_spin_unlock(&n00b_pool_page_cache_lock);
    atomic_fetch_add(&n00b_pool_page_cache_misses, 1);
    return nullptr;
}

/* Empty every slot parked before generation `before` into `out`, and return
 * how many it took. The caller holds n00b_pool_page_cache_lock. */
[[n00b::nogc]] static int
pool_page_cache_take_locked(uint64_t before, n00b_pool_cached_page_t *out)
{
    int      n     = 0;
    uint64_t bytes = 0;

    for (int c = 0; c < N00B_POOL_PAGE_CACHE_CLASSES; c++) {
        for (int i = 0; i < N00B_POOL_PAGE_CACHE_PER_CLASS; i++) {
            n00b_pool_cached_page_t *slot = &n00b_pool_page_cache[c][i];
            if (slot->addr != nullptr && slot->gen < before) {
                out[n++] = *slot;
                bytes += (uint64_t)slot->size;
                *slot = (n00b_pool_cached_page_t){};
            }
        }
    }
    atomic_fetch_sub(&n00b_pool_page_cache_bytes, bytes);
    return n;
}

/* Unmap pages taken out of the cache, counting failures the way
 * delete_one_page_entry does. Returns the bytes actually unmapped. */
[[n00b::nogc]] static uint64_t
pool_page_cache_unmap(n00b_pool_cached_page_t *pages, int n)
{
    uint64_t bytes = 0;

    for (int i = 0; i < n; i++) {
        uint64_t fail_before = atomic_load(&n00b_munmap_fail_count);
        n00b_safe_munmap(pages[i].addr, pages[i].size);
        if (atomic_load(&n00b_munmap_fail_count) != fail_before) {
            atomic_fetch_add(&n00b_pool_big_unmap_fail_count, 1);
            atomic_fetch_add(&n00b_pool_big_unmap_fail_bytes,
                             (uint64_t)pages[i].size);
        }
        else {
            bytes += (uint64_t)pages[i].size;
        }
    }
    return bytes;
}

uint64_t
n00b_pool_page_cache_drain(void)
{
    n00b_pool_cached_page_t taken[N00B_POOL_PAGE_CACHE_SLOTS];
    bool                    locked = false;

    /* Retry a busy lock for a while, but not forever: a stop-the-world can
     * suspend a holder mid-put, and a signal handler can interrupt one on its
     * own thread. */
    for (int round = 0; round < N00B_POOL_PAGE_CACHE_DRAIN_ROUNDS; round++) {
        if (pool_spin_trylock(&n00b_pool_page_cache_lock)) {
            locked = true;
            break;
        }
        base_nanosleep_ns(N00B_POOL_PAGE_CACHE_DRAIN_SLEEP_NS);
    }
    if (!locked) {
        return 0;
    }

    int n = pool_page_cache_take_locked(UINT64_MAX, taken);
    pool_spin_unlock(&n00b_pool_page_cache_lock);
    return pool_page_cache_unmap(taken, n);
}

void
n00b_pool_page_cache_note_collection(void)
{
    atomic_fetch_add(&n00b_pool_page_cache_gen, 1);
}

void
n00b_pool_page_cache_trim(void)
{
    uint64_t gen = atomic_load(&n00b_pool_page_cache_gen);

    if (gen == atomic_load(&n00b_pool_page_cache_trimmed_gen)
        || pool_page_cache_cap() == 0) {
        return;
    }
    if (!pool_spin_trylock(&n00b_pool_page_cache_lock)) {
        return; // the next collection retries
    }

    n00b_pool_cached_page_t taken[N00B_POOL_PAGE_CACHE_SLOTS];

    atomic_store(&n00b_pool_page_cache_trimmed_gen, gen);
    int n = pool_page_cache_take_locked(gen - 1, taken);
    pool_spin_unlock(&n00b_pool_page_cache_lock);

    atomic_fetch_add(&n00b_pool_page_cache_evicted, (uint64_t)n);
    (void)pool_page_cache_unmap(taken, n);
}

typedef struct {
    uintptr_t   start;
    uintptr_t   end;
    const char *name;
    const char *creation_loc;
    bool        registered;
    uint8_t     state; // 0 empty, 1 occupied
} n00b_pool_page_diag_entry_t;

static n00b_pool_page_diag_entry_t n00b_pool_page_diag_registry[N00B_POOL_PAGE_DIAG_REGISTRY_MAX];
static _Atomic uint32_t            n00b_pool_page_diag_lock;
static _Atomic uint64_t            n00b_pool_page_diag_count;
static _Atomic uint64_t            n00b_pool_page_diag_overflow_count;
static _Atomic uint64_t            n00b_pool_page_diag_lock_skip_count;

// ------------------------------------------------------------------
// Per-allocation-site pool audit (debug-only; N00B_POOL_ALLOC_AUDIT).
//
// Replaces the old fixed-size site/ptr open-addressed hash tables. Each pool
// opened with n00b_pool_init(.alloc_audit=true) is appended to
// n00b_audited_pools[] and carries a lazily-created GC dict
// (pool->alloc_audit_dict) mapping (uintptr_t)site-string ->
// n00b_pool_audit_counter_t *. The alloc hook stashes the site pointer in a
// trailing aligned word that alloc.c reserves for audited-pool allocations,
// then bumps the site counter; the free hook recovers the site from that
// trailing word in O(1) (no ptr table) via the allocation's inline-header
// alloc_len, and decrements. The dicts + counters live in a dedicated HIDDEN
// storage pool (n00b_pool_audit_storage) that is itself never audited, so the
// hooks never recurse or self-count, and hidden-pool bulk retention keeps every
// counter alive without root registration. Per-site live_bytes that never
// comes back down IS the GC-liveness leak signal. See include/core/pool.h.
// ------------------------------------------------------------------

#ifdef N00B_POOL_ALLOC_AUDIT

typedef struct {
    const char      *site;
    _Atomic uint64_t alloc_count;
    _Atomic uint64_t alloc_bytes;
    _Atomic uint64_t free_count;
    _Atomic uint64_t free_bytes;
    _Atomic int64_t  live_count;
    _Atomic int64_t  live_bytes;
} n00b_pool_audit_counter_t;

typedef n00b_dict_t(uint64_t, void *) n00b_pool_audit_dict_t;

#define N00B_POOL_AUDIT_MAX_POOLS 16
static n00b_pool_t     *n00b_audited_pools[N00B_POOL_AUDIT_MAX_POOLS];
static _Atomic uint32_t n00b_audited_pool_count;

// Dedicated backing store for the audit dicts + counter records. Hidden (so its
// contents are bulk-retained and never traced through) and NOT itself in the
// audited set (so the hooks no-op on its allocations -> no recursion). Lazily
// created via a CAS on the state word.
// [[n00b::nomap]]: suppress ncc's auto-generated gcmap descriptor for this
// file-static. It is an allocator (its free_lists hold _Atomic heads, which the
// generated root walk cannot legally dereference), and it must never be a GC
// root anyway — its contents are retained because it is a hidden pool, not by
// root scanning.
[[n00b::nomap]] static n00b_pool_t      n00b_pool_audit_storage;
static _Atomic uint32_t n00b_pool_audit_storage_state; // 0 none,1 building,2 ready

// Cross-pool diagnostic: a free whose recovered site was never counted at alloc
// (e.g. allocated before the runtime was ready enough to arm the dict).
static _Atomic uint64_t n00b_pool_audit_free_miss_count;

#endif // N00B_POOL_ALLOC_AUDIT

// The pool lock protects the page_table doubly-linked list, a multi-word
// structure that must never be observed half-spliced. Two layers make that
// hold across stop-the-world:
//
// 1. Every pool_lock critical section is bracketed by a read acquisition of
//    rt->critical_execution (pool_page_gate below — same pattern as mmap
//    interval-tree mutation in alloc.c). The STW initiator write-acquires
//    that gate BEFORE suspending any thread, so no thread is ever suspended
//    mid-splice: in-flight sections drain first.
// 2. Once the world is stopped (stw_active is set only AFTER), the collector
//    is the sole runner and pool_lock/pool_unlock no-op (same short-circuit
//    as rwlock.c): the collector must never wait on a suspended holder, and
//    must not clear a suspended holder's lock word either (the holder
//    finishes its critical section after resume — with layer 1 in place a
//    suspended holder can't exist, but the bypass keeps the collector
//    correct even if a new un-gated pool_lock site slips in).
static inline bool
pool_lock_stw_bypass(void)
{
    n00b_runtime_t *rt = n00b_default_runtime_or_null();

    return rt != nullptr && n00b_atomic_load(&rt->stw_active);
}

typedef struct {
    bool depth_bumped;
    bool lock_held;
} pool_page_gate_t;

// The gate's own critical_execution read acquisition can allocate: a
// thread's first acquisition allocates its rwlock read-log record from
// system_pool (rwlock.c acquire_read_record), and if that needs a fresh
// page it re-enters new_page_entry -> this gate. The per-thread depth
// counter bounds that recursion: a nested entry skips the lock (the outer
// entry either already holds it, or is mid-acquisition — the one splice
// that runs ungated is the outer acquisition's own bootstrap page, an
// accepted, vanishingly-narrow window). Threads with no TCB yet skip the
// gate too: per rwlock.c WP-001, a mid-init thread already holds
// critical_execution record-less across its whole init.
static inline pool_page_gate_t
pool_page_gate_enter(void)
{
    pool_page_gate_t gate = {};
    n00b_runtime_t  *rt   = n00b_default_runtime_or_null();

    if (rt == nullptr || !rt->critical_execution.inited
        || n00b_atomic_load(&rt->stw_active)) {
        return gate;
    }

    n00b_thread_t *self = n00b_thread_self();
    if (self == nullptr || self->record == nullptr) {
        return gate;
    }

    gate.depth_bumped = true;
    if (self->record->pool_gate_depth++ > 0) {
        return gate;
    }

    n00b_rw_read_lock(&rt->critical_execution);
    gate.lock_held = true;
    return gate;
}

static inline void
pool_page_gate_exit(pool_page_gate_t gate)
{
    if (gate.lock_held) {
        n00b_rw_unlock(&n00b_get_runtime()->critical_execution);
    }
    if (gate.depth_bumped) {
        n00b_thread_self()->record->pool_gate_depth--;
    }
}

static inline void
pool_lock(n00b_pool_t *pool)
{
    if (pool_lock_stw_bypass()) {
        return;
    }
    while (n00b_atomic_or(&pool->lock, 1) != 0)
        ;
}

static inline void
pool_unlock(n00b_pool_t *pool)
{
    if (pool_lock_stw_bypass()) {
        return;
    }
    n00b_atomic_store(&pool->lock, 0);
}

// Returns the claimed slot, or UINT32_MAX when the registry is full.
static uint32_t
pool_registry_register(n00b_pool_t *pool, const char *name)
{
    atomic_fetch_add(&n00b_pool_registry_init_count, 1);

    for (uint64_t i = 0; i < N00B_POOL_GLOBAL_REGISTRY_MAX; i++) {
        if (n00b_atomic_load(&n00b_pool_registry[i].pool) != nullptr) {
            continue;
        }
        n00b_pool_t *expected = nullptr;
        if (n00b_cas(&n00b_pool_registry[i].pool, &expected, pool)) {
            n00b_atomic_store(&n00b_pool_registry[i].name, name);
            return (uint32_t)i;
        }
    }

    atomic_fetch_add(&n00b_pool_registry_overflow_count, 1);
    return UINT32_MAX;
}

static void
pool_registry_unregister(n00b_pool_t *pool)
{
    uint32_t i = pool->registry_slot;
    if (i >= N00B_POOL_GLOBAL_REGISTRY_MAX
        || n00b_atomic_load(&n00b_pool_registry[i].pool) != pool) {
        return;
    }

    // Only the pool's owner unregisters it, so a plain release of the slot
    // suffices: clear name first so a slot with pool set never carries the
    // wrong name, then release the slot for re-claim.
    pool->registry_slot = UINT32_MAX;
    n00b_atomic_store(&n00b_pool_registry[i].name, (const char *)nullptr);
    n00b_atomic_store(&n00b_pool_registry[i].pool, (n00b_pool_t *)nullptr);
    atomic_fetch_add(&n00b_pool_registry_destroy_count, 1);
}

// A pool's pages are registered in the global mmap tree (so
// n00b_mem_get_allocator / n00b_find_alloc_info can resolve an in-page
// pointer back to the pool — which is what makes n00b_free reclaim a
// pool allocation to its free list, and what lets the GC navigate it)
// when the pool is NOT __system AND either carries OOB metadata or is
// GC-visible (non-hidden):
//   - __system pools are bootstrap-critical (registering ctx->pool's
//     pages recurses through _n00b_alloc_raw) and are excluded.
//   - hidden, no-metadata, header-less pools are libn00b-internal and stay
//     out of the tree. n00b_free finds them through the allocation header
//     (n00b_pool_unlisted_owner).
//   - everything else — external-metadata pools (incl. hidden ones like
//     user_pool) and plain non-hidden pools (e.g. a caller's claim pool)
//     — is registered so n00b_free works.
//   - hidden pools that carry INLINE headers are also registered. Such a
//     pool stays out of GC root scanning (n00b_mmap_is_gc_scannable is
//     false without a metadata pool) but still needs n00b_find_alloc_info
//     to resolve its allocations via the inline header — e.g. so the
//     self-contained rocs hot shard can be walked by n00b_marshal at seal
//     without paying a per-allocation OOB metadata dict. Inline headers on
//     a hidden pool are useless unless registered (find_alloc_info must
//     locate the page first), so gating on add_inline_header is exact.
static inline bool
pool_pages_registered(n00b_allocator_t *alloc)
{
    return !alloc->__system
        && (alloc->metadata_pool != nullptr || !alloc->hidden || alloc->add_inline_header);
}

// An allocation header's owner word: a tag in the high half and the pool's
// registry slot in the low half, or 0 for a pool the registry could not take.
#define N00B_POOL_ENTRY_TAG 0xB00Cu
static_assert(N00B_POOL_GLOBAL_REGISTRY_MAX <= (1u << 16));

[[n00b::nogc]] static inline uint32_t
pool_entry_owner(n00b_pool_t *pool)
{
    uint32_t slot = pool->registry_slot;
    return slot < N00B_POOL_GLOBAL_REGISTRY_MAX ? (N00B_POOL_ENTRY_TAG << 16) | slot
                                                 : 0;
}

n00b_allocator_t *
n00b_pool_unlisted_owner(void *ptr)
{
    if (ptr == nullptr || ((uintptr_t)ptr & (N00B_ALIGN - 1)) != 0) {
        return nullptr;
    }
    n00b_pool_entry_t *entry = (n00b_pool_entry_t *)((char *)ptr - N00B_ALIGN);
    uint32_t           owner = entry->owner;
    n00b_pool_t       *pool  = entry->pool;
    if ((owner >> 16) != N00B_POOL_ENTRY_TAG || pool == nullptr) {
        return nullptr;
    }
    // The slot holding this pool is what makes the header's pool pointer
    // trustworthy, and it means the pool is live.
    if (n00b_atomic_load(&n00b_pool_registry[owner & 0xFFFFu].pool) != pool
        || !pool->unlisted) {
        return nullptr;
    }
    return (n00b_allocator_t *)pool;
}

#ifdef N00B_POOL_ALLOC_AUDIT

// Backing store for the audit dicts + counters (see the block comment above).
// Hidden + never audited, so the hooks no-op on its own allocations. Lazily
// created once via a CAS on the state word.
static n00b_pool_t *
pool_audit_storage(void)
{
    if (atomic_load(&n00b_pool_audit_storage_state) == 2) {
        return &n00b_pool_audit_storage;
    }
    uint32_t expected = 0;
    if (atomic_compare_exchange_strong(&n00b_pool_audit_storage_state,
                                       &expected,
                                       1)) {
        n00b_pool_init(&n00b_pool_audit_storage,
                       .hidden         = true,
                       .inline_headers = true,
                       .name           = "pool_audit_storage");
        atomic_store(&n00b_pool_audit_storage_state, 2);
        return &n00b_pool_audit_storage;
    }
    while (atomic_load(&n00b_pool_audit_storage_state) != 2) {
        ;
    }
    return &n00b_pool_audit_storage;
}

// The audited-pool record for an allocator (nullptr if not audited). A linear
// scan of <= N00B_POOL_AUDIT_MAX_POOLS pointers; both the alloc hot path and the
// hooks consult it so the trailing-word layout decision always agrees.
[[n00b::nogc]] static n00b_pool_t *
pool_audit_lookup(n00b_allocator_t *allocator)
{
    uint32_t n = atomic_load(&n00b_audited_pool_count);
    if (n > N00B_POOL_AUDIT_MAX_POOLS) {
        n = N00B_POOL_AUDIT_MAX_POOLS;
    }
    for (uint32_t i = 0; i < n; i++) {
        if ((n00b_allocator_t *)n00b_audited_pools[i] == allocator) {
            return n00b_audited_pools[i];
        }
    }
    return nullptr;
}

// The pool's site->counter dict, created on first use. Built in the hidden
// storage pool so it (and its future counters) are bulk-retained and invisible
// to the audit hooks. Returns nullptr only if the runtime is not ready enough
// to allocate yet, in which case the caller just skips counting (the trailing
// site-word is still stamped).
static n00b_pool_audit_dict_t *
pool_audit_dict(n00b_pool_t *pool)
{
    void *d = atomic_load(&pool->alloc_audit_dict);
    if (d != nullptr) {
        return (n00b_pool_audit_dict_t *)d;
    }
    if (n00b_default_runtime_or_null() == nullptr) {
        return nullptr;
    }

    n00b_pool_t            *store = pool_audit_storage();
    n00b_pool_audit_dict_t *fresh = n00b_alloc_with_opts(
        n00b_pool_audit_dict_t,
        &(n00b_alloc_opts_t){.allocator = (n00b_allocator_t *)store});
    n00b_dict_init(fresh,
                   .allocator     = (n00b_allocator_t *)store,
                   .skip_obj_hash = true,
                   .locked        = true);

    void *expected = nullptr;
    if (atomic_compare_exchange_strong(&pool->alloc_audit_dict,
                                       &expected,
                                       (void *)fresh)) {
        return fresh;
    }
    // Lost the publish race: the winner's dict is authoritative. `fresh` stays
    // (harmlessly) retained in the hidden storage pool.
    return (n00b_pool_audit_dict_t *)expected;
}

// get-or-create the counter for `site`. add-if-absent resolves the concurrent
// first-touch race without relying on the (unused elsewhere) dict CAS: the
// loser re-gets the winner's record. Counters live in the hidden storage pool.
static n00b_pool_audit_counter_t *
pool_audit_counter_for(n00b_pool_audit_dict_t *dict, const char *site)
{
    uint64_t key = (uint64_t)(uintptr_t)site;
    bool     found;
    void    *v = n00b_dict_get(dict, key, &found);
    if (found) {
        return (n00b_pool_audit_counter_t *)v;
    }

    n00b_pool_audit_counter_t *c = n00b_alloc_with_opts(
        n00b_pool_audit_counter_t,
        &(n00b_alloc_opts_t){
            .allocator = (n00b_allocator_t *)pool_audit_storage()});
    c->site   = site;
    void *cv  = c;
    if (n00b_dict_add(dict, key, cv)) {
        return c;
    }
    v = n00b_dict_get(dict, key, &found);
    return found ? (n00b_pool_audit_counter_t *)v : c;
}

#endif // N00B_POOL_ALLOC_AUDIT

bool
n00b_pool_alloc_audit_enabled(n00b_allocator_t *allocator)
{
#ifdef N00B_POOL_ALLOC_AUDIT
    return pool_audit_lookup(allocator) != nullptr;
#else
    (void)allocator;
    return false;
#endif
}

[[n00b::nogc]]
void
n00b_system_pool_audit_alloc(n00b_allocator_t *allocator,
                             void             *ptr,
                             uint64_t          bytes,
                             const char       *site)
{
#ifdef N00B_POOL_ALLOC_AUDIT
    if (ptr == nullptr || bytes < N00B_ALIGN) {
        return;
    }
    n00b_pool_t *pool = pool_audit_lookup(allocator);
    if (pool == nullptr) {
        return;
    }
    if (site == nullptr) {
        site = "";
    }
    // Stash the site in the trailing word alloc.c reserved for audited
    // allocations (`bytes` includes that trailing N00B_ALIGN slot). This runs
    // unconditionally — even before the dict is armed — so the free hook always
    // recovers a valid site rather than uninitialized memory.
    *(const char **)((char *)ptr + bytes - N00B_ALIGN) = site;

    n00b_pool_audit_dict_t *dict = pool_audit_dict(pool);
    if (dict == nullptr) {
        return;
    }
    n00b_pool_audit_counter_t *c = pool_audit_counter_for(dict, site);
    if (c == nullptr) {
        return;
    }
    atomic_fetch_add(&c->alloc_count, 1);
    atomic_fetch_add(&c->alloc_bytes, bytes);
    atomic_fetch_add(&c->live_count, 1);
    atomic_fetch_add(&c->live_bytes, (int64_t)bytes);
#else
    (void)allocator;
    (void)ptr;
    (void)bytes;
    (void)site;
#endif
}

[[n00b::nogc]]
void
n00b_system_pool_audit_free(n00b_allocator_t *allocator, void *ptr)
{
#ifdef N00B_POOL_ALLOC_AUDIT
    if (ptr == nullptr) {
        return;
    }
    n00b_pool_t *pool = pool_audit_lookup(allocator);
    if (pool == nullptr) {
        return;
    }
    // pool_free is called with the pool's base allocation, which IS the inline
    // header (see pool_free / _n00b_alloc_raw). Recover alloc_len from it, then
    // read the site out of the trailing slot the alloc hook stamped.
    n00b_inline_hdr_t *hdr = (n00b_inline_hdr_t *)ptr;
    if (hdr->guard != n00b_gc_guard) {
        return; // not an inline-header allocation we stamped.
    }
    uint64_t bytes = hdr->alloc_len;
    if (bytes < N00B_ALIGN) {
        return;
    }
    const char *site = *(const char **)((char *)ptr + bytes - N00B_ALIGN);

    n00b_pool_audit_dict_t *dict = pool_audit_dict(pool);
    if (dict == nullptr) {
        return;
    }
    uint64_t key = (uint64_t)(uintptr_t)site;
    bool     found;
    void    *v = n00b_dict_get(dict, key, &found);
    if (!found) {
        atomic_fetch_add(&n00b_pool_audit_free_miss_count, 1);
        return;
    }
    n00b_pool_audit_counter_t *c = (n00b_pool_audit_counter_t *)v;
    atomic_fetch_add(&c->free_count, 1);
    atomic_fetch_add(&c->free_bytes, bytes);
    atomic_fetch_sub(&c->live_count, 1);
    atomic_fetch_sub(&c->live_bytes, (int64_t)bytes);
#else
    (void)allocator;
    (void)ptr;
#endif
}

#ifdef N00B_POOL_ALLOC_AUDIT
[[n00b::nogc]] static void
pool_audit_record_top(n00b_system_pool_audit_stats_t *stats,
                      const char                     *site,
                      uint64_t                        alloc_count,
                      uint64_t                        free_count,
                      uint64_t                        alloc_bytes,
                      uint64_t                        free_bytes,
                      uint64_t                        live_count,
                      uint64_t                        live_bytes)
{
    if (live_bytes == 0) {
        return;
    }

    uint64_t pos = stats->top_count;
    if (pos < N00B_SYSTEM_POOL_AUDIT_TOP_N) {
        stats->top_count++;
    }
    else {
        pos = N00B_SYSTEM_POOL_AUDIT_TOP_N - 1;
        if (live_bytes <= stats->top_live_bytes[pos]) {
            return;
        }
    }

    while (pos > 0 && live_bytes > stats->top_live_bytes[pos - 1]) {
        stats->top_site[pos]        = stats->top_site[pos - 1];
        stats->top_alloc_count[pos] = stats->top_alloc_count[pos - 1];
        stats->top_free_count[pos]  = stats->top_free_count[pos - 1];
        stats->top_alloc_bytes[pos] = stats->top_alloc_bytes[pos - 1];
        stats->top_free_bytes[pos]  = stats->top_free_bytes[pos - 1];
        stats->top_live_count[pos]  = stats->top_live_count[pos - 1];
        stats->top_live_bytes[pos]  = stats->top_live_bytes[pos - 1];
        pos--;
    }

    stats->top_site[pos]        = site != nullptr ? site : "";
    stats->top_alloc_count[pos] = alloc_count;
    stats->top_free_count[pos]  = free_count;
    stats->top_alloc_bytes[pos] = alloc_bytes;
    stats->top_free_bytes[pos]  = free_bytes;
    stats->top_live_count[pos]  = live_count;
    stats->top_live_bytes[pos]  = live_bytes;
}
#endif // N00B_POOL_ALLOC_AUDIT

n00b_system_pool_audit_stats_t
n00b_pool_audit_stats(n00b_pool_t *pool)
{
    n00b_system_pool_audit_stats_t stats = {0};
#ifdef N00B_POOL_ALLOC_AUDIT
    if (pool == nullptr) {
        return stats;
    }
    n00b_pool_audit_dict_t *dict = (n00b_pool_audit_dict_t *)
        atomic_load(&pool->alloc_audit_dict);
    if (dict == nullptr) {
        return stats;
    }
    // NOTE: free_miss is a single process-wide counter (across all audited
    // pools), not per-pool; every pool's stats report the same aggregate. It is
    // a minor diagnostic (frees whose recovered site was never counted), so the
    // shared counter is acceptable; per-pool attribution is a possible follow-up.
    stats.free_miss_count = atomic_load(&n00b_pool_audit_free_miss_count);

    n00b_dict_foreach(dict, k, v, {
        (void)k;
        n00b_pool_audit_counter_t *c  = (n00b_pool_audit_counter_t *)v;
        uint64_t                   ac = atomic_load(&c->alloc_count);
        uint64_t                   fc = atomic_load(&c->free_count);
        uint64_t                   ab = atomic_load(&c->alloc_bytes);
        uint64_t                   fb = atomic_load(&c->free_bytes);
        int64_t                    lc = atomic_load(&c->live_count);
        int64_t                    lb = atomic_load(&c->live_bytes);
        uint64_t                   ulc = lc > 0 ? (uint64_t)lc : 0;
        uint64_t                   ulb = lb > 0 ? (uint64_t)lb : 0;
        stats.total_alloc_count += ac;
        stats.total_free_count += fc;
        stats.total_alloc_bytes += ab;
        stats.total_free_bytes += fb;
        stats.live_alloc_count += ulc;
        stats.live_bytes += ulb;
        pool_audit_record_top(&stats, c->site, ac, fc, ab, fb, ulc, ulb);
    });
#else
    (void)pool;
#endif
    return stats;
}

n00b_system_pool_audit_stats_t
n00b_system_pool_audit_stats(void)
{
#ifdef N00B_POOL_ALLOC_AUDIT
    n00b_runtime_t *rt = n00b_default_runtime_or_null();
    if (rt != nullptr) {
        return n00b_pool_audit_stats(&rt->system_pool);
    }
#endif
    return (n00b_system_pool_audit_stats_t){0};
}

n00b_system_pool_audit_stats_t
n00b_user_pool_audit_stats(void)
{
#ifdef N00B_POOL_ALLOC_AUDIT
    n00b_runtime_t *rt = n00b_default_runtime_or_null();
    if (rt != nullptr) {
        return n00b_pool_audit_stats(&rt->user_pool);
    }
#endif
    return (n00b_system_pool_audit_stats_t){0};
}

// A zeroed snapshot is ambiguous unless callers can distinguish an unavailable
// audit from an audited pool with no live allocations.
bool
n00b_pool_audit_compiled(void)
{
#ifdef N00B_POOL_ALLOC_AUDIT
    return true;
#else
    return false;
#endif
}

bool
n00b_user_pool_audit_enabled(void)
{
#ifdef N00B_POOL_ALLOC_AUDIT
    n00b_runtime_t *rt = n00b_default_runtime_or_null();
    return rt != nullptr && rt->user_pool.alloc_audit;
#else
    return false;
#endif
}

bool
n00b_conduit_pool_audit_enabled(void)
{
#ifdef N00B_POOL_ALLOC_AUDIT
    n00b_runtime_t *rt = n00b_default_runtime_or_null();
    return rt != nullptr && rt->conduit_pool.alloc_audit;
#else
    return false;
#endif
}

// Runtime initialization leaves auditing disabled for system_pool.
bool
n00b_system_pool_audit_enabled(void)
{
#ifdef N00B_POOL_ALLOC_AUDIT
    n00b_runtime_t *rt = n00b_default_runtime_or_null();
    return rt != nullptr && rt->system_pool.alloc_audit;
#else
    return false;
#endif
}

n00b_system_pool_audit_stats_t
n00b_conduit_pool_audit_stats(void)
{
#ifdef N00B_POOL_ALLOC_AUDIT
    n00b_runtime_t *rt = n00b_default_runtime_or_null();
    if (rt != nullptr) {
        return n00b_pool_audit_stats(&rt->conduit_pool);
    }
#endif
    return (n00b_system_pool_audit_stats_t){0};
}

[[n00b::nogc]] static inline bool
pool_page_diag_lock(void)
{
    if (pool_spin_trylock(&n00b_pool_page_diag_lock)) {
        return true;
    }
    atomic_fetch_add(&n00b_pool_page_diag_lock_skip_count, 1);
    return false;
}

[[n00b::nogc]] static inline void
pool_page_diag_unlock(void)
{
    pool_spin_unlock(&n00b_pool_page_diag_lock);
}

// Fibonacci hashing: pages are usually mapped back to back, and a plain
// (start >> 12) would place them in consecutive slots, forming one probe run
// that every insert and delete in the region has to walk.
[[n00b::nogc]] static inline uint64_t
pool_page_diag_hash(uintptr_t start)
{
    return ((uint64_t)(start >> 12) * 0x9E3779B97F4A7C15ull)
        >> (64 - N00B_POOL_PAGE_DIAG_REGISTRY_BITS);
}

[[n00b::nogc]] static inline uint64_t
pool_page_diag_next(uint64_t ix)
{
    return (ix + 1) & (N00B_POOL_PAGE_DIAG_REGISTRY_MAX - 1);
}

[[n00b::nogc]]
static void
pool_page_diag_register(n00b_pool_t *pool, n00b_pool_page_t *page)
{
    if (pool == nullptr || page == nullptr || page->mapped_size == 0) {
        return;
    }

    n00b_allocator_t *alloc = (n00b_allocator_t *)pool;
    uintptr_t         start = (uintptr_t)page;
    uintptr_t         end   = start + (uintptr_t)page->mapped_size;
    if (end <= start) {
        return;
    }

    n00b_pool_page_diag_entry_t new_entry = {
        .start        = start,
        .end          = end,
        .name         = alloc->debug_name,
        .creation_loc = alloc->creation_loc,
        .registered   = pool_pages_registered(alloc),
        .state        = 1,
    };

    if (!pool_page_diag_lock()) {
        return;
    }
    // The cap keeps at least a quarter of the slots empty, so this terminates.
    for (uint64_t ix = pool_page_diag_hash(start);; ix = pool_page_diag_next(ix)) {
        n00b_pool_page_diag_entry_t *slot = &n00b_pool_page_diag_registry[ix];
        if (slot->state == 1 && slot->start == start) {
            *slot = new_entry;
            break;
        }
        if (slot->state == 0) {
            if (atomic_load(&n00b_pool_page_diag_count)
                >= N00B_POOL_PAGE_DIAG_REGISTRY_CAP) {
                atomic_fetch_add(&n00b_pool_page_diag_overflow_count, 1);
                break;
            }
            *slot = new_entry;
            atomic_fetch_add(&n00b_pool_page_diag_count, 1);
            break;
        }
    }
    pool_page_diag_unlock();
}

// Backward-shift deletion: after emptying a slot, pull later members of the
// same probe run into the hole so no lookup ever stops short of its key.
[[n00b::nogc]]
static void
pool_page_diag_unregister(n00b_pool_page_t *page)
{
    if (page == nullptr) {
        return;
    }

    uintptr_t start = (uintptr_t)page;
    if (!pool_page_diag_lock()) {
        return;
    }
    uint64_t hole = pool_page_diag_hash(start);
    for (;; hole = pool_page_diag_next(hole)) {
        n00b_pool_page_diag_entry_t *slot = &n00b_pool_page_diag_registry[hole];
        if (slot->state == 0) {
            pool_page_diag_unlock();
            return;
        }
        if (slot->start == start) {
            break;
        }
    }

    for (uint64_t ix = pool_page_diag_next(hole);; ix = pool_page_diag_next(ix)) {
        n00b_pool_page_diag_entry_t *slot = &n00b_pool_page_diag_registry[ix];
        if (slot->state == 0) {
            break;
        }
        // Distance from each slot's home: the entry may move into the hole
        // only if the hole lies on its probe path.
        uint64_t home = pool_page_diag_hash(slot->start);
        uint64_t mask = N00B_POOL_PAGE_DIAG_REGISTRY_MAX - 1;
        if (((ix - home) & mask) >= ((ix - hole) & mask)) {
            n00b_pool_page_diag_registry[hole] = *slot;
            hole                               = ix;
        }
    }
    n00b_pool_page_diag_registry[hole] = (n00b_pool_page_diag_entry_t){0};
    atomic_fetch_sub(&n00b_pool_page_diag_count, 1);
    pool_page_diag_unlock();
}

[[n00b::nogc]]
static bool
pool_page_diag_lookup(uintptr_t addr,
                      uint64_t *out_start,
                      uint64_t *out_end,
                      const char **out_name,
                      const char **out_creation_loc,
                      bool *out_registered)
{
    if (addr == 0) {
        return false;
    }

    bool found = false;
    if (!pool_page_diag_lock()) {
        return false;
    }
    for (uint64_t ix = pool_page_diag_hash(addr);; ix = pool_page_diag_next(ix)) {
        n00b_pool_page_diag_entry_t *slot = &n00b_pool_page_diag_registry[ix];
        if (slot->state == 0) {
            break;
        }
        if (slot->start != addr) {
            continue;
        }
        if (out_start != nullptr) {
            *out_start = (uint64_t)slot->start;
        }
        if (out_end != nullptr) {
            *out_end = (uint64_t)slot->end;
        }
        if (out_name != nullptr) {
            *out_name = slot->name != nullptr ? slot->name : "";
        }
        if (out_creation_loc != nullptr) {
            *out_creation_loc = slot->creation_loc;
        }
        if (out_registered != nullptr) {
            *out_registered = slot->registered;
        }
        found = true;
        break;
    }
    pool_page_diag_unlock();
    return found;
}

#ifdef N00B_DEBUG
static bool
pool_mmap_audit_enabled(void)
{
    static _Atomic int enabled;
    int cached = atomic_load(&enabled);
    if (cached != 0) {
        return cached == 2;
    }
    bool on = getenv("N00B_POOL_MMAP_AUDIT") != nullptr;
    atomic_store(&enabled, on ? 2 : 1);
    return on;
}

static void
pool_mmap_audit(n00b_pool_t *pool, const char *op, void *addr, size_t mapped)
{
    n00b_allocator_t *alloc = (n00b_allocator_t *)pool;
    if (!alloc->hidden || !pool_mmap_audit_enabled()) {
        return;
    }

    fprintf(stderr,
            "n00b pool-mmap-audit op=%s pool=%s hidden=1 system=%u registered=%u "
            "addr=%p size=%zu live_mapped=%llu pages=%llu big_maps=%llu big_unmaps=%llu\n",
            op,
            alloc->debug_name != nullptr ? alloc->debug_name : "",
            (unsigned)alloc->__system,
            (unsigned)pool_pages_registered(alloc),
            addr,
            mapped,
            (unsigned long long)n00b_pool_mapped_bytes(pool),
            (unsigned long long)n00b_pool_page_count(pool),
            (unsigned long long)n00b_pool_big_map_count(pool),
            (unsigned long long)n00b_pool_big_unmap_count(pool));
}
#else
static inline void
pool_mmap_audit(n00b_pool_t *pool, const char *op, void *addr, size_t mapped)
{
    (void)pool;
    (void)op;
    (void)addr;
    (void)mapped;
}
#endif

/* Bytes of slab payload backing one chunk of same-class entries.
 *
 * This used to be exactly one OS page, which ties slab batching to a number
 * the allocator does not choose. At a 16K base page that is fine; at 4K it
 * falls apart at the top of the class range. With 4064 usable bytes the 2K
 * class fits ONE entry per chunk (so every 2K allocation maps its own page),
 * and the 4K and 8K classes do not fit at all, so pool_alloc routes them to
 * the single-entry mmap path below. Three of the eight classes therefore get
 * no batching on a 4K-page host and full batching on a 16K-page one, which is
 * most of why the same workload costs far more in the allocator on Linux than
 * on macOS.
 *
 * Sizing the chunk independently fixes that: 64K holds 8 of the largest class
 * and 1024 of the smallest, on every host. The cost is per-pool slack, capped
 * at one chunk per class actually used. Kept at least one page so a host with
 * a larger base page still gets a whole number of pages. */
#define N00B_POOL_SLAB_CHUNK (64ull * 1024ull)

[[n00b::nogc]] static inline uint64_t
pool_slab_payload(void)
{
    uint64_t chunk = N00B_POOL_SLAB_CHUNK;

    if (chunk < (uint64_t)n00b_page_size) {
        chunk = (uint64_t)n00b_page_size;
    }
    return chunk - n00b_align(sizeof(n00b_pool_page_t));
}

static inline void *
new_page_entry(n00b_pool_t *pool, uint64_t *sz_ptr)
{
    uint64_t sz     = *sz_ptr;
    uint64_t hdr_sz = n00b_align(sizeof(n00b_pool_page_t));
    sz += hdr_sz;

    n00b_allocator_t *alloc      = (n00b_allocator_t *)pool;
    char             *name       = (char *)alloc->debug_name;
    uint64_t          aligned_sz = n00b_page_align(sz);
    /* skip_register=true puts the page back under pool control —
     * the matching @ref n00b_mmap_register_pool_page below decides
     * whether (and how) this page enters the global mmap tree.
     * The pool side wants this control so the unregister at free
     * time strictly precedes munmap (avoiding a window where a
     * concurrent GC mark scan can dereference a tree entry whose
     * backing page is no longer mapped). */
    n00b_pool_page_t *cur = pool_page_cache_get(aligned_sz);

    if (cur != nullptr) {
        /* Recycled: still mapped and resident, so nothing faults here. A
         * fresh mmap would arrive zeroed and pool_alloc's callers rely on
         * that, so do the zeroing here instead -- same work the fault handler
         * would have done, without the trap. */
        memset((void *)cur, 0, aligned_sz);
    }
    else {
        auto mmap_r = n00b_mmap(aligned_sz,
                                .allocator     = alloc,
                                .name          = name,
                                .kind          = n00b_mmap_pool,
                                .skip_register = true);
        assert(n00b_result_is_ok(mmap_r));
        cur = n00b_result_get(mmap_r);
    }

    /* Register the page with the allocator so @ref n00b_mem_get_allocator
     * can resolve in-page pointers back to this pool, which is what
     * makes n00b_free → pool_free work (the address-to-allocator
     * lookup goes through the mmap tree).
     *
     * Only register hidden pools that have @c external_metadata.
     * Two distinct exclusions:
     *
     *  - @c __system pools (the mmap context's own backing @c ctx->pool
     *    and @c rt->system_pool) are bootstrap-critical: registering
     *    @c ctx->pool's pages here recurses infinitely
     *    (register_pool_page → mmaps_insert_raw → _n00b_alloc_raw →
     *    pool_alloc on ctx->pool → new_page_entry → back here).
     *
     *  - Non-metadata hidden pools (@c conduit_pool, per-thread
     *    @c ctx->pool, etc.) are libn00b-internal and exclusively use
     *    the @c pool_free fast path; their pages do not need
     *    address-to-allocator resolution.  Putting them in the global
     *    mmap tree adds GC-scan tree traversal load and a race window
     *    (concurrent @c pool_free → unregister → munmap vs GC mark
     *    iterator) for no benefit. */
    if (pool_pages_registered(alloc)) {
        (void)n00b_mmap_register_pool_page((void *)cur, (char *)cur + aligned_sz, alloc, name);
    }

    *sz_ptr          = aligned_sz - hdr_sz;
    cur->mapped_size = aligned_sz;
    void *res        = (void *)cur + hdr_sz;

    pool_page_gate_t gate = pool_page_gate_enter();
    pool_lock(pool);
    cur->next = pool->page_table;
    if (cur->next) {
        cur->next->prev = cur;
    }
    pool->page_table = cur;
    if (alloc->libc_backing) {
        cur->interpose_slot =
            n00b_alloc_interpose_note_pages((void *)cur, (char *)cur + aligned_sz);
    }
    uint64_t live_mapped
        = atomic_fetch_add(&pool->mapped_bytes_total, (uint64_t)cur->mapped_size)
        + (uint64_t)cur->mapped_size;
    uint64_t peak = n00b_atomic_load(&pool->mapped_bytes_peak);
    while (peak < live_mapped
           && !n00b_cas(&pool->mapped_bytes_peak, &peak, live_mapped))
        ;
    pool_unlock(pool);
    pool_page_gate_exit(gate);

    pool_mmap_audit(pool, "map", (void *)cur, (size_t)cur->mapped_size);
    pool_page_diag_register(pool, cur);

    return res;
}

// Here, sz includes space for the n00b_pool_entry_t at the front.
static inline n00b_pool_entry_t *
big_mmap(n00b_pool_t *pool, uint64_t sz)
{
    n00b_pool_entry_t *entry = new_page_entry(pool, &sz);
    entry->list_index        = N00B_NUM_FREE_LISTS;

    /* Diagnostics: account for big-mmap allocations symmetrically
     * with delete_one_page_entry below so callers can verify the
     * fast path is balanced. */
    atomic_fetch_add(&pool->big_map_count, 1);

    return entry;
}

static inline void
delete_one_page_entry(n00b_pool_t *pool, n00b_pool_page_t *entry)
{
    pool_page_gate_t gate = pool_page_gate_enter();
    pool_lock(pool);

    if (entry->prev) {
        entry->prev->next = entry->next;
    }
    else {
        assert(pool->page_table == entry);
        pool->page_table = entry->next;
    }

    if (entry->next) {
        entry->next->prev = entry->prev;
    }
    n00b_alloc_interpose_forget_pages(entry->interpose_slot, (void *)entry);

    /* Capture mapped_size while we still hold the lock; the munmap
     * itself is fine to do unlocked once the page is unlinked. */
    size_t   mapped = entry->mapped_size;
    uint64_t old    = n00b_atomic_load(&pool->mapped_bytes_total);
    while (!n00b_cas(&pool->mapped_bytes_total,
                     &old,
                     old >= (uint64_t)mapped ? old - (uint64_t)mapped : 0))
        ;

    pool_unlock(pool);
    pool_page_gate_exit(gate);

    /* Symmetric counterpart to new_page_entry's optional
     * @ref n00b_mmap_register_pool_page: pull the tree entry
     * BEFORE munmap so a concurrent GC mark pass can't follow a
     * stale interval-tree node into a no-longer-mapped page and
     * SIGBUS. The unregister is a no-op when the page was never
     * registered, so the check here mirrors the register-side gate
     * verbatim (skip @c __system bootstrap pools and skip pools
     * without @c external_metadata). */
    n00b_allocator_t *alloc = (n00b_allocator_t *)pool;
    if (pool_pages_registered(alloc)) {
        n00b_mmap_unregister((void *)entry);
    }

    pool_mmap_audit(pool, "unmap", (void *)entry, mapped);
    pool_page_diag_unregister(entry);

    /* Big-alloc free path: actually release the page back to the OS.
     * Without this the page stayed mapped until pool_destroy — any
     * pool client n00b_free-ing a >N00B_NUM_FREE_LISTS-class
     * allocation observed the slot count drop but RSS keep climbing.
     * Under the diagnostic quarantine the release is deferred: the page
     * is parked PROT_NONE so a use-after-free faults attributably (see
     * pool_quarantine_park above). */
    if (mapped != 0) {
        if (pool_quarantine_park(pool, (void *)entry, mapped)) {
            atomic_fetch_add(&pool->big_unmap_count, 1);
        }
        else if (pool->page_cache
                 && pool_page_cache_put((void *)entry, mapped)) {
            /* Cache owns the mapping now; do not unmap it. */
            atomic_fetch_add(&pool->big_unmap_count, 1);
        }
        else {
            uint64_t fail_before = atomic_load(&n00b_munmap_fail_count);
            n00b_safe_munmap((void *)entry, mapped);
            if (atomic_load(&n00b_munmap_fail_count) != fail_before) {
                atomic_fetch_add(&n00b_pool_big_unmap_fail_count, 1);
                atomic_fetch_add(&n00b_pool_big_unmap_fail_bytes,
                                 (uint64_t)mapped);
            }
            atomic_fetch_add(&pool->big_unmap_count, 1);
        }
    }
}

static inline void *
add_page_to_list(n00b_pool_t *pool, uint64_t sz, n00b_llstack_t *stack)
{
    // Return one, and push the others to the free list.
    // Yes, we could pre-link the extras, but that's a PITA.
    uint64_t alloc_sz = pool_slab_payload();
    void    *res      = new_page_entry(pool, &alloc_sz);

    assert(!(((uint64_t)sz) & 15));

    char    *p = ((char *)res) + n00b_align(sz);
    uint32_t n = (alloc_sz / n00b_align(sz));

    for (uint32_t i = 1; i < n; i++) {
        // Yes, we end up having to cast.
        n00b_llstack_node_t *node = (n00b_llstack_node_t *)p;
        p += n00b_align(sz);
        n00b_llstack_push_node(stack, node);
    }

    return res;
}

static void
pool_destroy(n00b_pool_t *pool)
{
    // Drop from the audit ring BEFORE freeing pages, so a concurrent census
    // never dereferences a half-freed pool (mirrors the arena delete hook).
    n00b_allocator_audit_unregister((n00b_allocator_t *)pool);
    pool_registry_unregister(pool);

    n00b_pool_page_t *entry = pool->page_table;
    n00b_pool_page_t *next;

    /* Scrub the lock chains FIRST, walking the page table, so that
     * the scrubber can chase chain links without ever reading from
     * a freed page.  Pool pages can hold n00b_mutex_t /
     * n00b_rwlock_t (e.g. as fields of an in-pool Regex), and
     * `n00b_lock_acquire_accounting` threads those onto each
     * thread's exclusive-lock chain.  The owning regex's teardown
     * destroys the pool without releasing them, so the chain would
     * otherwise be left with dangling pointers into freed memory. */
    if (pool->scrub_locks_on_destroy) {
        /* Walk the thread table ONCE for the whole pool (the scrubber
         * tests each held-lock chain entry against the pool's page
         * table).  The old per-page call paid pages * 4096 thread-record
         * walks per destroy, which froze the process during GC cleanup. */
        n00b_lock_chains_scrub_pool(pool);
    }

    /* If this pool's pages were registered in the global mmap tree,
     * pull each one out BEFORE munmap so a later n00b_mem_get_allocator
     * / GC mark can't follow a stale tree node into a freed page. */
    bool registered = pool_pages_registered((n00b_allocator_t *)pool);

    while (entry) {
        next          = entry->next;
        /* Big-alloc pages can be larger than n00b_page_size — use the
         * captured mapped_size so we unmap the right region length. */
        size_t mapped = entry->mapped_size != 0 ? entry->mapped_size : n00b_page_size;
        if (registered) {
            n00b_mmap_unregister((void *)entry);
        }
        pool_page_diag_unregister(entry);
        if (entry->interpose_slot != 0) {
            pool_page_gate_t gate = pool_page_gate_enter();
            n00b_alloc_interpose_forget_pages(entry->interpose_slot, (void *)entry);
            pool_page_gate_exit(gate);
        }
        uint64_t fail_before = atomic_load(&n00b_munmap_fail_count);
        n00b_safe_munmap(entry, mapped);
        atomic_fetch_add(&n00b_pool_destroy_unmap_count, 1);
        atomic_fetch_add(&n00b_pool_destroy_unmap_bytes, (uint64_t)mapped);
        if (atomic_load(&n00b_munmap_fail_count) != fail_before) {
            atomic_fetch_add(&n00b_pool_destroy_unmap_fail_count, 1);
            atomic_fetch_add(&n00b_pool_destroy_unmap_fail_bytes, (uint64_t)mapped);
        }
        entry = next;
    }
}

static void
pool_free(n00b_pool_t *pool, void *ptr)
{
    n00b_system_pool_audit_free((n00b_allocator_t *)pool, ptr);

    n00b_pool_entry_t *entry = (n00b_pool_entry_t *)((char *)ptr - N00B_ALIGN);
    assert(entry->list_index <= N00B_NUM_FREE_LISTS);

    if (entry->list_index == N00B_NUM_FREE_LISTS) {
        /* `entry` sits at offset n00b_align(sizeof(n00b_pool_page_t))
         * past the page header (see `new_page_entry`).  Decrementing
         * a `n00b_pool_page_t *` by 1 only walks back
         * `sizeof(n00b_pool_page_t)` bytes, which is wrong when that
         * is smaller than N00B_ALIGN. */
        n00b_pool_page_t *page
            = (n00b_pool_page_t *)(((char *)entry) - n00b_align(sizeof(n00b_pool_page_t)));
        delete_one_page_entry(pool, page);
        return;
    }

    unsigned int ix = entry->list_index;
    /* Entry size at bucket index ix is (1u << ix) << POST_ROUND_SHIFT
     * bytes — the same formula pool_alloc uses to derive ix from the
     * rounded size.  Earlier the memset used `ix << POST_ROUND_SHIFT`,
     * which under-zeros the tail of every bucket (and zeros 0 bytes
     * at ix=0); freshly-popped entries then return non-zero bytes,
     * silently violating the documented zero-fill contract. */
    memset(entry, 0, (1u << ix) << N00B_POST_ROUND_SHIFT);
    n00b_llstack_node_t *node_ptr = (n00b_llstack_node_t *)entry;

    n00b_llstack_push_node(&pool->free_lists[ix], node_ptr);
}

static void *
pool_alloc(n00b_pool_t *pool, uint64_t request, void *ignore)
{
    request += n00b_align(sizeof(n00b_pool_entry_t));

    uint64_t     sz = n00b_align_closest_pow2_ceil(request);
    unsigned int ix = n00b_int_log2(sz >> N00B_POST_ROUND_SHIFT);

    n00b_pool_entry_t *entry;

    uint64_t slab_payload = pool_slab_payload();

    /* On 4 KiB-page Linux, the 4 KiB/8 KiB nominal slab classes cannot fit
     * after the pool page header. Route those requests through the single-entry
     * mmap path instead of creating undersized slab slots. The guard-mode
     * check (page per alloc; see pool_guard_page_per_alloc) also lands here
     * so slab-class objects become individually parkable by the quarantine. */
    if (ix >= N00B_NUM_FREE_LISTS || sz > slab_payload
        || pool_guard_page_per_alloc(pool)) {
        entry = big_mmap(pool, request);
        ix    = N00B_NUM_FREE_LISTS;
    }
    else {
        n00b_llstack_t      *stack = &pool->free_lists[ix];
        n00b_llstack_node_t *nptr  = n00b_llstack_pop_node(stack);

        if (nptr) {
            entry = (n00b_pool_entry_t *)nptr;
        }
        else {
            entry = add_page_to_list(pool, sz, stack);
        }
    }

    entry->list_index = ix;
    entry->owner      = pool_entry_owner(pool);
    entry->pool       = pool;
    void *p           = (void *)(((char *)entry) + N00B_ALIGN);

    assert(!(((uint64_t)p) & (N00B_ALIGN - 1)));
    return p;
}

size_t
n00b_pool_usable_size(void *ptr)
{
    /* Recover the usable byte count for a raw pool allocation (the
     * pointer returned by the pool's zero_alloc).  Used by the libc-malloc
     * interposition layer for realloc()/malloc_usable_size().  The size
     * class is stored in the n00b_pool_entry_t header at -N00B_ALIGN; the
     * formulas here mirror pool_alloc / pool_free exactly. */
    if (ptr == nullptr) {
        return 0;
    }

    n00b_pool_entry_t *entry = (n00b_pool_entry_t *)((char *)ptr - N00B_ALIGN);
    unsigned int       ix    = entry->list_index;

    if (ix < N00B_NUM_FREE_LISTS) {
        /* Small slab: the bucket is (1<<ix)<<POST_ROUND_SHIFT bytes; the
         * user gets all but the N00B_ALIGN-byte entry header. */
        size_t bucket = ((size_t)1u << ix) << N00B_POST_ROUND_SHIFT;
        return bucket - N00B_ALIGN;
    }

    /* Big mmap: the page header sits n00b_align(sizeof(page)) before the
     * entry, and the user pointer is N00B_ALIGN past the entry. */
    n00b_pool_page_t *page
        = (n00b_pool_page_t *)((char *)entry - n00b_align(sizeof(n00b_pool_page_t)));
    size_t hdr = n00b_align(sizeof(n00b_pool_page_t)) + N00B_ALIGN;

    if (page->mapped_size <= hdr) {
        return 0;
    }
    return page->mapped_size - hdr;
}

uint64_t
n00b_pool_mapped_bytes(n00b_pool_t *pool)
{
    if (pool == nullptr) {
        return 0;
    }
    // O(1) lock-free atomic read of the running total maintained by
    // new_page_entry / delete_one_page_entry. Deliberately does NOT take the
    // pool lock: status/diagnostic paths call this on arbitrary pools and
    // must never spin against (or wait on) another thread's pool lock.
    return n00b_atomic_load(&pool->mapped_bytes_total);
}

uint64_t
n00b_pool_mapped_bytes_peak(n00b_pool_t *pool)
{
    if (pool == nullptr) {
        return 0;
    }
    return n00b_atomic_load(&pool->mapped_bytes_peak);
}

uint64_t
n00b_pool_page_count(n00b_pool_t *pool)
{
    if (pool == nullptr) {
        return 0;
    }
    uint64_t          total = 0;
    n00b_pool_page_t *p;
    pool_page_gate_t  gate = pool_page_gate_enter();
    pool_lock(pool);
    for (p = pool->page_table; p != nullptr; p = p->next) {
        total++;
    }
    pool_unlock(pool);
    pool_page_gate_exit(gate);
    return total;
}

// Counters-only snapshot: pure atomic loads, no registry walk, no pool
// dereference, no lock. Live-pool aggregation was removed with the
// registry-walking stats (it dereferenced registered pools with no
// liveness guarantee and spun on per-pool locks).
[[n00b::nogc]] n00b_pool_global_stats_t
n00b_pool_global_stats(void)
{
    n00b_pool_global_stats_t stats = {
        .total_init_count        = atomic_load(&n00b_pool_registry_init_count),
        .total_destroy_count     = atomic_load(&n00b_pool_registry_destroy_count),
        .registry_overflow_count = atomic_load(&n00b_pool_registry_overflow_count),
        .destroy_unmap_count     = atomic_load(&n00b_pool_destroy_unmap_count),
        .destroy_unmap_bytes     = atomic_load(&n00b_pool_destroy_unmap_bytes),
        .destroy_unmap_fail_count =
            atomic_load(&n00b_pool_destroy_unmap_fail_count),
        .destroy_unmap_fail_bytes =
            atomic_load(&n00b_pool_destroy_unmap_fail_bytes),
        .big_unmap_fail_count = atomic_load(&n00b_pool_big_unmap_fail_count),
        .big_unmap_fail_bytes = atomic_load(&n00b_pool_big_unmap_fail_bytes),
        .diagnostic_page_count = atomic_load(&n00b_pool_page_diag_count),
        .diagnostic_page_overflow_count =
            atomic_load(&n00b_pool_page_diag_overflow_count),
        .diagnostic_page_lock_skip_count =
            atomic_load(&n00b_pool_page_diag_lock_skip_count),
        .page_cache_hits      = atomic_load(&n00b_pool_page_cache_hits),
        .page_cache_misses    = atomic_load(&n00b_pool_page_cache_misses),
        .page_cache_parked    = atomic_load(&n00b_pool_page_cache_parked),
        .page_cache_evicted   = atomic_load(&n00b_pool_page_cache_evicted),
        .page_cache_bytes     = atomic_load(&n00b_pool_page_cache_bytes),
        .page_cache_cap_bytes = pool_page_cache_cap(),
    };

    return stats;
}

bool
n00b_pool_diagnostic_lookup_page(uintptr_t addr,
                                 uint64_t *out_start,
                                 uint64_t *out_end,
                                 const char **out_name,
                                 const char **out_creation_loc,
                                 bool *out_registered)
{
    if (addr == 0) {
        return false;
    }

    return pool_page_diag_lookup(addr,
                                 out_start,
                                 out_end,
                                 out_name,
                                 out_creation_loc,
                                 out_registered);
}

uint64_t
n00b_pool_big_map_count(n00b_pool_t *pool)
{
    return pool == nullptr ? 0 : atomic_load(&pool->big_map_count);
}

uint64_t
n00b_pool_big_unmap_count(n00b_pool_t *pool)
{
    return pool == nullptr ? 0 : atomic_load(&pool->big_unmap_count);
}

[[n00b::nogc]] static void
pool_pre_destroy(n00b_allocator_t *allocator)
{
    n00b_runtime_t *rt = n00b_default_runtime_or_null();

    if (rt != nullptr && n00b_atomic_load(&rt->stw_active)) {
        /* During STW the initiating thread is the only runnable thread.
         * Waiting for epoch quiescence here deadlocks if suspended threads
         * still have older reservations.  Non-STW pool teardown still drains
         * retire lists normally. */
        return;
    }

    /* The drain mutates epoch retire lists, and the collector flushes ALL
     * allocators' epochs inside its STW window (gc.c
     * n00b_epoch_flush_all_stw). A thread suspended mid-drain would resume
     * into nodes the flush already freed. Hold the critical-execution gate
     * across the drain so STW cannot begin until it completes (the same
     * guarantee the page-table splices rely on). */
    pool_page_gate_t gate = pool_page_gate_enter();
    n00b_epoch_drain_allocator(allocator);
    pool_page_gate_exit(gate);
}

n00b_allocator_t *
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
    // the alloc to the pool at its last unref.
    bool        pool_refcount          = false;
    bool        alloc_refcount         = false;
    // Epoch reclamation is ONLY for backing stores of !gc dictionaries that are
    // read concurrently and cannot use a pinref/STW reader gate — in practice
    // the OOB metadata dict's md_pool (n00b_new_metadata_pool sets this true
    // explicitly). Every other pool must NOT defer frees through the epoch retire
    // lists: doing so needlessly parks freed allocations on per-thread retire
    // lists, and tearing such a pool down then has to reclaim those nodes across
    // threads (only safe under STW) — the source of the pool-destroy retire-list
    // crashes. Default OFF; opt in only for a genuine cross-thread !gc dict store.
    bool        use_epochs             = false;
    // Debug-only (N00B_POOL_ALLOC_AUDIT): opt into per-allocation-site auditing.
    // No-op in builds without the audit compiled in.
    bool        alloc_audit            = false;
    // Internal (n00b_new_metadata_pool only): marks vtable.is_metadata, which
    // routes this pool's destroy through the deferred STW teardown queue.
    bool        __is_md_pool           = false;
    // Park freed big pages in the released-page cache (see pool_page_cache_put).
    bool        page_cache             = true;
    bool        libc_backing           = false;
}
{
    // Only per-ALLOC refcounting needs OOB: its counter lives in the OOB flex
    // tail (the inline header is the marshal payload and must not change). So
    // .alloc_refcount forces OOB metadata on and inline headers off.
    //
    // Per-POOL refcounting does NOT touch metadata at all — its counter is a
    // slot in the pool header (pool_refs). It is therefore orthogonal to the
    // metadata strategy and leaves the caller's inline_headers/external_metadata
    // choice intact. This matters for the rocs hot shard, which is .pool_refcount
    // AND inline-headers-only (external_metadata=false) so marshal can resolve
    // every shard alloc via its inline header without paying OOB's per-alloc
    // dict put/get + STW-gate read-lock on the ingest hot path.
    if (alloc_refcount) {
        external_metadata = true;
        inline_headers    = false;
    }

    n00b_allocator_setup((n00b_allocator_t *)pool,
                         (n00b_calloc_fn)pool_alloc,
                         .free              = (n00b_free_fn)pool_free,
                         .pre_destroy       = pool_pre_destroy,
                         .destroy           = (n00b_allocator_destroy_fn)pool_destroy,
                         .name              = (char *)name,
                         .inline_headers    = inline_headers,
                         .external_metadata = external_metadata,
                         .hidden            = hidden,
                         .use_epochs        = use_epochs,
                         .__system          = __system,
                         .__is_md_pool      = __is_md_pool,
                         .creation_loc      = creation_loc);

    // Allocator-specific OOB flex-tail size. .alloc_refcount reserves a
    // uint32_t counter at the end of each OOB record (n00b_oob_hdr_t.alloc_extra).
    // Must be set after n00b_allocator_setup, which overwrites the whole struct.
    pool->vtable.oob_extra_size = alloc_refcount ? (uint32_t)sizeof(uint32_t) : 0;
    pool->vtable.libc_backing   = libc_backing;

    pool->lock                   = 0;
    pool->page_table             = nullptr;
    atomic_store(&pool->mapped_bytes_total, 0);
    atomic_store(&pool->mapped_bytes_peak, 0);
    pool->scrub_locks_on_destroy = scrub_locks_on_destroy;
    pool->page_cache             = page_cache;
    atomic_store(&pool->big_map_count, 0);
    atomic_store(&pool->big_unmap_count, 0);

    // Per-pool refcount. Starts at 1 (the creator's ref) when armed; the last
    // n00b_pool_unref reclaims the pool via on_last_unref or n00b_allocator_destroy.
    // The optional last-unref hook is installed later via n00b_pool_set_unref_cb.
    pool->pool_refcounted = pool_refcount;
    atomic_store(&pool->pool_refs, pool_refcount ? 1 : 0);
    pool->on_last_unref = nullptr;
    pool->unref_ctx     = nullptr;

    // Per-allocation-site audit (debug-only). The dict is armed lazily on first
    // audited allocation; here we only record the opt-in + registry membership.
    pool->alloc_audit = false;
    atomic_store(&pool->alloc_audit_dict, nullptr);
#ifdef N00B_POOL_ALLOC_AUDIT
    if (alloc_audit) {
        pool->alloc_audit = true;
        uint32_t ix       = atomic_fetch_add(&n00b_audited_pool_count, 1);
        if (ix < N00B_POOL_AUDIT_MAX_POOLS) {
            n00b_audited_pools[ix] = pool;
        }
    }
#else
    (void)alloc_audit;
#endif

    for (int i = 0; i < N00B_NUM_FREE_LISTS; i++) {
        n00b_llstack_init(&pool->free_lists[i]);
    }

    pool->unlisted      = !pool_pages_registered((n00b_allocator_t *)pool);
    pool->registry_slot = pool_registry_register(pool, name);
    // Pools are arenas too for the memory census: track every live pool in the
    // shared allocator audit ring (debug-only; no-op otherwise).
    n00b_allocator_audit_register((n00b_allocator_t *)pool);

    return (n00b_allocator_t *)pool;
}

void
n00b_pool_set_unref_cb(n00b_pool_t *pool, n00b_pool_unref_cb_t cb, void *ctx)
{
    if (pool == nullptr || !pool->pool_refcounted) {
        return;
    }
    pool->on_last_unref = cb;
    pool->unref_ctx     = ctx;
}

void
n00b_pool_ref(n00b_pool_t *pool)
{
    if (pool == nullptr || !pool->pool_refcounted) {
        return;
    }
    atomic_fetch_add_explicit(&pool->pool_refs, 1, memory_order_relaxed);
}

void
n00b_pool_unref(n00b_pool_t *pool)
{
    if (pool == nullptr || !pool->pool_refcounted) {
        return;
    }
    // acq_rel so the thread that observes the 1->0 transition has a
    // happens-before edge to every prior ref-holder's last use of the pool.
    int32_t prev = atomic_fetch_sub_explicit(&pool->pool_refs, 1, memory_order_acq_rel);
    if (prev != 1) {
        return;
    }

    if (pool->on_last_unref != nullptr) {
        pool->on_last_unref(pool->unref_ctx);
    }
    else {
        n00b_allocator_destroy((n00b_allocator_t *)pool);
    }
}
