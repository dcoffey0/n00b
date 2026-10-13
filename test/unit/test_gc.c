#include <stdio.h>
#include <assert.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/arena.h"
#include "core/gc.h"
#include "core/stw.h"
#include "core/atomic.h"
#include "core/thread.h"

// ============================================================================
// Test helper type
// ============================================================================

typedef struct {
    uint64_t value;
    void    *next;
} test_obj_t;

// Convenience macro: cast arena pointer to allocator for n00b_alloc_with_opts().
#define ARENA_OPTS(a) &(n00b_alloc_opts_t){.allocator = (n00b_allocator_t *)(a)}
#define ARENA_OPTS_NOSCAN(a) &(n00b_alloc_opts_t){.allocator = (n00b_allocator_t *)(a), .no_scan = true}

// ============================================================================
// 1. Arena metrics
// ============================================================================

static void
test_arena_metrics(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);

    uint64_t initial_used = n00b_arena_used(arena);
    uint64_t capacity     = n00b_arena_size(arena);

    assert(capacity > 0);
    // Initial usage should be near zero (just segment header alignment).
    assert(initial_used < 256);

    for (int i = 0; i < 10; i++) {
        test_obj_t *obj = n00b_alloc_with_opts(test_obj_t, ARENA_OPTS(arena));
        obj->value      = i;
    }

    uint64_t after_used = n00b_arena_used(arena);

    assert(after_used > initial_used);
    assert(after_used <= capacity);

    printf("  [PASS] arena metrics\n");
}

// ============================================================================
// 2. Basic collection (pressure-based)
// ============================================================================

static void
test_basic_collection(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);

    uint32_t prev_count = 0;
    bool     collected  = false;

    for (int i = 0; i < 10000; i++) {
        test_obj_t *obj = n00b_alloc_with_opts(test_obj_t, ARENA_OPTS(arena));
        obj->value      = i;

        uint32_t cur = n00b_atomic_load(&arena->alloc_count);

        if (cur < prev_count) {
            // alloc_count was reset by n00b_collect_internal — GC happened.
            collected = true;
            break;
        }
        prev_count = cur;
    }

    assert(collected);
    printf("  [PASS] basic collection\n");
}

// ============================================================================
// 3. Object survival
// ============================================================================

#define SURVIVAL_COUNT 50

static void
test_object_survival(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);

    test_obj_t *objs[SURVIVAL_COUNT];

    for (int i = 0; i < SURVIVAL_COUNT; i++) {
        objs[i]        = n00b_alloc_with_opts(test_obj_t, ARENA_OPTS(arena));
        objs[i]->value = 0xDEAD0000ULL + (uint64_t)i;
        objs[i]->next  = nullptr;
    }

    // Trigger GC — all 50 objects are reachable from the stack-local array.
    n00b_stop_the_world();
    n00b_collect(arena);
    n00b_restart_the_world();

    // Verify every object's magic value survived the copy.
    for (int i = 0; i < SURVIVAL_COUNT; i++) {
        assert(objs[i]->value == 0xDEAD0000ULL + (uint64_t)i);
    }

    printf("  [PASS] object survival\n");
}

// ============================================================================
// 4. Pointer chain rewriting
// ============================================================================

static void
test_pointer_chain(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);

    test_obj_t *a = n00b_alloc_with_opts(test_obj_t, ARENA_OPTS(arena));
    test_obj_t *b = n00b_alloc_with_opts(test_obj_t, ARENA_OPTS(arena));
    test_obj_t *c = n00b_alloc_with_opts(test_obj_t, ARENA_OPTS(arena));

    a->value = 0xAAAA;
    b->value = 0xBBBB;
    c->value = 0xCCCC;
    a->next  = b;
    b->next  = c;
    c->next  = nullptr;

    // Drop direct references; chain is only reachable via a->next->next.
    b = nullptr;
    c = nullptr;

    n00b_stop_the_world();
    n00b_collect(arena);
    n00b_restart_the_world();

    // Walk the chain — proves internal heap pointers were rewritten.
    assert(a->value == 0xAAAA);

    test_obj_t *b2 = (test_obj_t *)a->next;
    assert(b2 != nullptr);
    assert(b2->value == 0xBBBB);

    test_obj_t *c2 = (test_obj_t *)b2->next;
    assert(c2 != nullptr);
    assert(c2->value == 0xCCCC);
    assert(c2->next == nullptr);

    printf("  [PASS] pointer chain\n");
}

// ============================================================================
// 5. Unreachable objects collected
// ============================================================================

// noinline so the 399 discarded pointers live only in this frame,
// which is dead when the caller triggers collection.
static __attribute__((noinline)) test_obj_t *
allocate_waste(n00b_arena_t *arena, int count)
{
    test_obj_t *last = nullptr;

    for (int i = 0; i < count; i++) {
        last        = n00b_alloc_with_opts(test_obj_t, ARENA_OPTS(arena));
        last->value = (uint64_t)i;
        last->next  = nullptr;
    }

    return last;
}

static void
test_unreachable_collected(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);

    // Allocate 400 objects; only the very last pointer is returned.
    test_obj_t *survivor = allocate_waste(arena, 400);

    uint64_t before = n00b_arena_used(arena);

    n00b_stop_the_world();
    n00b_collect(arena);
    n00b_restart_the_world();

    uint64_t after = n00b_arena_used(arena);

    // Conservative stack scanning may keep a handful alive, but the
    // vast majority should be reclaimed. Under the pin-all policy
    // (N00B_GC_PIN_ALL=1, or no gcmap dictionary linked) nothing is copied and
    // reclaim is page-granular, so a 4 KB arena of dead objects sharing pages
    // with the survivor is legitimately retained; only check the copying case.
    if (!n00b_gc_pin_all_policy()) {
        assert(after < before);
        assert(after < before / 2);
    }

    assert(survivor->value == 399);

    printf("  [PASS] unreachable collected\n");
}

// ============================================================================
// 6. Multiple collections
// ============================================================================

#define MULTI_COUNT 20

static void
test_multiple_collections(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);

    for (int round = 0; round < 3; round++) {
        test_obj_t *objs[MULTI_COUNT];

        for (int i = 0; i < MULTI_COUNT; i++) {
            objs[i]        = n00b_alloc_with_opts(test_obj_t, ARENA_OPTS(arena));
            objs[i]->value = (uint64_t)(round * 1000 + i);
        }

        n00b_stop_the_world();
        n00b_collect(arena);
        n00b_restart_the_world();

        for (int i = 0; i < MULTI_COUNT; i++) {
            assert(objs[i]->value == (uint64_t)(round * 1000 + i));
        }
    }

    printf("  [PASS] multiple collections\n");
}

// ============================================================================
// 7. Manual collect
// ============================================================================

static void
test_manual_collect(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);

    test_obj_t *obj = n00b_alloc_with_opts(test_obj_t, ARENA_OPTS(arena));
    obj->value      = 0x12345678DEADBEEFULL;
    obj->next       = nullptr;

    n00b_stop_the_world();
    n00b_collect(arena);
    n00b_restart_the_world();

    assert(obj->value == 0x12345678DEADBEEFULL);
    assert(obj->next == nullptr);

    printf("  [PASS] manual collect\n");
}

// ============================================================================
// 7b. Stop-the-world pause counters
// ============================================================================

static void
test_pause_counters_record_collection(void)
{
    n00b_runtime_t *rt           = n00b_get_runtime();
    uint64_t        count_before = n00b_atomic_load(&rt->gc_pause_count);
    uint64_t        total_before = n00b_atomic_load(&rt->gc_pause_total_ns);

    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);
    test_obj_t   *obj   = n00b_alloc_with_opts(test_obj_t, ARENA_OPTS(arena));
    obj->value          = 1;
    obj->next           = nullptr;

    n00b_collect(arena);

    uint64_t last = n00b_atomic_load(&rt->gc_last_pause_ns);
    assert(n00b_atomic_load(&rt->gc_pause_count) > count_before);
    assert(last > 0);
    assert(n00b_atomic_load(&rt->gc_pause_total_ns) >= total_before + last);
    assert(n00b_atomic_load(&rt->gc_max_pause_ns) >= last);

    printf("  [PASS] pause counters record collection\n");
}

// ============================================================================
// 8. Conservative header-address false positives
// ============================================================================

static void
test_header_address_false_positive(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);

    test_obj_t *target = n00b_alloc_with_opts(test_obj_t, ARENA_OPTS(arena));
    test_obj_t *holder = n00b_alloc_with_opts(test_obj_t, ARENA_OPTS(arena));

    n00b_option_t(n00b_inline_hdr_t *) hdr_opt = n00b_inline_alloc_header(target);
    assert(n00b_option_is_set(hdr_opt));

    target->value = 0xFEEDFACEULL;
    target->next  = nullptr;
    holder->value = (uint64_t)(uintptr_t)n00b_option_get(hdr_opt);
    holder->next  = target;

    n00b_stop_the_world();
    n00b_collect(arena);
    n00b_restart_the_world();

    assert(target->value == 0xFEEDFACEULL);
    assert(holder->next == target);

    printf("  [PASS] conservative header-address false positive\n");
}

// True when `addr` resolves to the allocation whose inline header is `hdr`.
static bool
resolves_to(void *addr, n00b_inline_hdr_t *hdr)
{
    bool              pin  = false;
    n00b_alloc_info_t info = n00b_find_alloc_info(addr,
                                                  .scan_for_header = true,
                                                  .pin             = &pin);
    bool              same = false;

    switch (info.kind) {
    case n00b_alloc_inline:
        same = info.hdr.in_line == hdr;
        break;
    case n00b_alloc_oob:
        same = info.hdr.oob->hcur == hdr;
        break;
    default:
        break;
    }
    n00b_alloc_hdr_unpin(pin);
    return same;
}

// Masked so no scan can take these for pointers and rewrite them.
#define HIDE(p) ((uint64_t)(uintptr_t)(p) ^ 0xFFFF000000000000ull)

typedef struct {
    n00b_arena_t    *arena;
    bool             reserve; // publish a reservation starting at the end
    _Atomic uint32_t ready;
    _Atomic uint32_t collected;
    uint64_t         obj_was;
    uint64_t         end_was;
    uint64_t         obj_now;
    uint64_t         end_now;
    uint64_t         value;
} past_end_probe_t;

// Allocates the object so that it ends on a page boundary, and stores it, and
// the address just past it, into the caller's slots.  A separate frame, so no
// register the caller keeps across its wait holds either address; a register
// root would pin the object.
static __attribute__((noinline)) void
past_end_setup(n00b_arena_t          *arena,
               test_obj_t *volatile  *obj_slot,
               char *volatile        *end_slot)
{
    test_obj_t        *sizer     = n00b_alloc_with_opts(test_obj_t, ARENA_OPTS(arena));
    uint64_t           obj_total = n00b_option_get(n00b_inline_alloc_header(sizer))->alloc_len;
    uintptr_t          cur       = (uintptr_t)n00b_atomic_load(&arena->next_alloc);
    uint64_t           filler    = (n00b_page_size - (cur + obj_total) % n00b_page_size)
                                   % n00b_page_size;
    if (filler < N00B_ALLOC_HDR_SZ) {
        filler += n00b_page_size;
    }
    (void)n00b_alloc_array_with_opts(char,
                                     filler - N00B_ALLOC_HDR_SZ,
                                     ARENA_OPTS(arena));

    test_obj_t *obj = n00b_alloc_with_opts(test_obj_t, ARENA_OPTS(arena));
    obj->value      = 0x0B1EC7ULL;
    obj->next       = nullptr;

    n00b_inline_hdr_t *hdr = n00b_option_get(n00b_inline_alloc_header(obj));
    char              *end = (char *)hdr + hdr->alloc_len;
    n00b_require((uintptr_t)end % n00b_page_size == 0,
                 "the object does not end on a page boundary");
    n00b_require(end == n00b_atomic_load(&arena->next_alloc),
                 "the object is not the arena's last allocation");

    *obj_slot = obj;
    *end_slot = end;
}

// Holds an object and the address just past it on this thread's stack while
// main collects.  With `reserve` set, that address is also the start of an
// in-flight reservation, the way a thread parked between its bump CAS and
// writing the new object's header holds it.
static void *
past_end_worker(void *arg)
{
    past_end_probe_t    *probe    = arg;
    test_obj_t *volatile obj_word = nullptr;
    char *volatile       end_word = nullptr;

    past_end_setup(probe->arena, &obj_word, &end_word);
    probe->obj_was = HIDE(obj_word);
    probe->end_was = HIDE(end_word);
    if (probe->reserve) {
        n00b_thread_t *self = n00b_thread_self();
        atomic_store_explicit(&self->gc_inflight_len, 64, memory_order_relaxed);
        atomic_store_explicit(&self->gc_inflight_start,
                              (void *)end_word,
                              memory_order_release);
    }

    // Drop any heap address the setup left in a scratch register.
#if defined(__aarch64__)
    __asm__ volatile("mov x0, xzr\n mov x1, xzr\n mov x2, xzr\n mov x3, xzr\n"
                     "mov x4, xzr\n mov x5, xzr\n mov x6, xzr\n mov x7, xzr\n"
                     "mov x8, xzr\n mov x9, xzr\n mov x10, xzr\n mov x11, xzr\n"
                     "mov x12, xzr\n mov x13, xzr\n mov x14, xzr\n mov x15, xzr\n"
                     "mov x16, xzr\n mov x17, xzr\n"
                     ::: "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8",
                         "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16",
                         "x17", "memory");
#elif defined(__x86_64__)
    __asm__ volatile("xor %%eax, %%eax\n xor %%ecx, %%ecx\n xor %%edx, %%edx\n"
                     "xor %%esi, %%esi\n xor %%edi, %%edi\n xor %%r8d, %%r8d\n"
                     "xor %%r9d, %%r9d\n xor %%r10d, %%r10d\n xor %%r11d, %%r11d\n"
                     ::: "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10",
                         "r11", "memory");
#endif

    n00b_atomic_store(&probe->ready, 1);
    while (!n00b_atomic_load(&probe->collected)) {
        __asm__ volatile("" ::: "memory");
    }

    if (probe->reserve) {
        n00b_thread_t *self = n00b_thread_self();
        atomic_store_explicit(&self->gc_inflight_start, nullptr,
                              memory_order_release);
        atomic_store_explicit(&self->gc_inflight_len, 0, memory_order_relaxed);
    }
    probe->obj_now = HIDE(obj_word);
    probe->end_now = HIDE(end_word);
    probe->value   = obj_word->value;
    return nullptr;
}

// Runs the worker over a fresh arena and collects that arena while it waits.
static past_end_probe_t
run_past_end_worker(bool reserve)
{
    past_end_probe_t probe = {
        .arena   = n00b_new_arena(.size = 64 * 1024, .use_gc = true),
        .reserve = reserve,
    };

    auto r = n00b_thread_spawn(past_end_worker, &probe);
    n00b_require(n00b_result_is_ok(r), "worker spawn failed");
    n00b_thread_t *worker = n00b_result_get(r);

    while (!n00b_atomic_load(&probe.ready)) {
        __asm__ volatile("" ::: "memory");
    }
    n00b_collect(probe.arena);
    n00b_atomic_store(&probe.collected, 1);
    n00b_thread_join(worker);

    return probe;
}

// A word exactly one past an allocation's end belongs to that allocation, as
// C allows, and moves with it.  Anything further is free space.
static void
test_word_one_past_allocation_end(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);

    test_obj_t *obj = n00b_alloc_with_opts(test_obj_t, ARENA_OPTS(arena));
    obj->value      = 0x0B1EC7ULL;
    obj->next       = nullptr;

    n00b_option_t(n00b_inline_hdr_t *) hdr_opt = n00b_inline_alloc_header(obj);
    n00b_require(n00b_option_is_set(hdr_opt), "no inline header");
    n00b_inline_hdr_t *hdr = n00b_option_get(hdr_opt);
    char              *end = (char *)hdr + hdr->alloc_len;
    n00b_require(end == n00b_atomic_load(&arena->next_alloc),
                 "the object is not the arena's last allocation");

    n00b_require(resolves_to(end, hdr),
                 "the address one past an allocation did not resolve to it");
    n00b_require(!n00b_alloc_info_is_heap(
                     n00b_find_alloc_info(end + 64, .scan_for_header = true)),
                 "free space past an allocation resolved to it");
    n00b_require(resolves_to((char *)obj + 8, hdr),
                 "an interior address did not resolve to its allocation");

    test_obj_t *empty = n00b_alloc_array_with_opts(test_obj_t, 0, ARENA_OPTS(arena));
    n00b_inline_hdr_t *empty_hdr = n00b_option_get(n00b_inline_alloc_header(empty));
    n00b_require(resolves_to(empty, empty_hdr),
                 "a zero-length allocation's pointer did not resolve to it");

    // The collector forwards words on a suspended thread's stack, so the
    // words under test live on a worker's.
    past_end_probe_t probe = run_past_end_worker(false);

    n00b_require(probe.value == 0x0B1EC7ULL, "the object did not survive");
    if (n00b_gc_pin_all_policy()) {
        printf("  [PASS] word one past an allocation's end (pin-all: "
               "nothing moves)\n");
        return;
    }
    n00b_require(probe.obj_now != probe.obj_was,
                 "the collection did not move the object, so nothing was "
                 "tested");
    n00b_require(probe.end_now - probe.obj_now == probe.end_was - probe.obj_was,
                 "a word one past the allocation's end did not move with it");

    printf("  [PASS] word one past an allocation's end\n");
}

// A reservation that starts a page begins one past the end of the allocation
// on the page before.  The collector keeps that allocation in place, or it
// would forward the reserving thread's `start` onto whatever it copies next,
// and the thread would write its object over that one.
static void
test_reservation_after_allocation(void)
{
    past_end_probe_t probe = run_past_end_worker(true);

    n00b_require(probe.value == 0x0B1EC7ULL, "the object did not survive");
    n00b_require(probe.obj_now == probe.obj_was,
                 "the allocation before a reservation moved");
    n00b_require(probe.end_now == probe.end_was,
                 "the reservation's start was forwarded");

    printf("  [PASS] allocation before a reservation stays in place\n");
}

typedef struct {
    char            *keep;
    _Atomic uint32_t ready;
    _Atomic uint32_t collected;
} retain_probe_t;

// Holds a reservation on `keep`, which pins the pages `keep`'s allocation
// covers, while main collects.
static void *
retain_worker(void *arg)
{
    retain_probe_t *probe = arg;
    n00b_thread_t  *self  = n00b_thread_self();

    atomic_store_explicit(&self->gc_inflight_len, 8, memory_order_relaxed);
    atomic_store_explicit(&self->gc_inflight_start,
                          (void *)probe->keep,
                          memory_order_release);
    n00b_atomic_store(&probe->ready, 1);
    while (!n00b_atomic_load(&probe->collected)) {
        __asm__ volatile("" ::: "memory");
    }
    atomic_store_explicit(&self->gc_inflight_start, nullptr,
                          memory_order_release);
    atomic_store_explicit(&self->gc_inflight_len, 0, memory_order_relaxed);
    return nullptr;
}

// Lays out `keep`, ending 256 bytes before a page boundary, then a dead
// 512-byte object that crosses it, then a dead tail object.  Returns `keep`
// and hides the dead object's header, so no root on this thread reaches it.
static __attribute__((noinline)) char *
retain_setup(n00b_arena_t *arena, uint64_t *dead_hdr_hidden, uint64_t *dead_len)
{
    uintptr_t cur      = (uintptr_t)n00b_atomic_load(&arena->next_alloc);
    uintptr_t boundary = (cur + 2 * n00b_page_size) & ~((uintptr_t)n00b_page_size - 1);
    char     *keep     = n00b_alloc_array_with_opts(char,
                                                boundary - 256 - cur - N00B_ALLOC_HDR_SZ,
                                                ARENA_OPTS(arena));
    char     *dead     = n00b_alloc_array_with_opts(char, 512, ARENA_OPTS(arena));
    (void)n00b_alloc_with_opts(test_obj_t, ARENA_OPTS(arena));

    n00b_inline_hdr_t *hdr = n00b_option_get(n00b_inline_alloc_header(dead));
    n00b_require((uintptr_t)hdr == boundary - 256,
                 "the dead object does not start 256 bytes before the page "
                 "boundary");
    n00b_require(resolves_to(dead + 16, hdr),
                 "a pointer into the object did not resolve to it");

    *dead_hdr_hidden = HIDE(hdr);
    *dead_len        = hdr->alloc_len;
    return keep;
}

// Overwrites the stack below the caller, so no stale slot the collector
// scans holds the dead object.
static __attribute__((noinline)) void
scrub_stack(void)
{
    volatile char buf[16384];
    for (size_t i = 0; i < sizeof(buf); i++) {
        buf[i] = 0;
    }
}

// A dead object whose header sits on a page the collector retains, and whose
// length runs into a page it reclaims, must not resolve: copying or scanning
// it would read past its mapping into unmapped memory.
static void
test_dead_object_past_retained_page(void)
{
    // Inline headers only, so the lookup reads the header rather than an OOB
    // record, as it does in a release build.
    n00b_arena_t *arena = n00b_new_arena(.size   = 64 * 1024,
                                         .use_gc = true,
                                         .no_map = true);
    n00b_require(((n00b_allocator_t *)arena)->metadata_pool == nullptr,
                 "the arena keeps OOB metadata");

    uint64_t       dead_hidden;
    uint64_t       dead_len;
    retain_probe_t probe = {
        .keep = retain_setup(arena, &dead_hidden, &dead_len),
    };
    scrub_stack();

    auto r = n00b_thread_spawn(retain_worker, &probe);
    n00b_require(n00b_result_is_ok(r), "worker spawn failed");
    n00b_thread_t *worker = n00b_result_get(r);

    while (!n00b_atomic_load(&probe.ready)) {
        __asm__ volatile("" ::: "memory");
    }
    n00b_collect(arena);
    n00b_atomic_store(&probe.collected, 1);
    n00b_thread_join(worker);

    char *dead_hdr = (char *)(uintptr_t)(dead_hidden ^ 0xFFFF000000000000ull);
    auto  rec_opt  = n00b_mmap_by_address(dead_hdr);
    n00b_require(n00b_option_is_set(rec_opt),
                 "the page holding the dead object's header was not retained");
    n00b_require(n00b_option_get(rec_opt)->end < (uint64_t)dead_hdr + dead_len,
                 "the page the dead object runs into was retained, so nothing "
                 "was tested");
    n00b_require(((n00b_inline_hdr_t *)dead_hdr)->guard == n00b_gc_guard,
                 "the dead object's header is gone, so nothing was tested");

    n00b_require(!n00b_alloc_info_is_heap(
                     n00b_find_alloc_info(dead_hdr + N00B_ALLOC_HDR_SZ + 16,
                                          .scan_for_header = true)),
                 "a stale pointer resolved to a dead object that runs past "
                 "its mapping");

    printf("  [PASS] dead object past a retained page does not resolve\n");
}

// ============================================================================
// 9. No-scan objects survive
// ============================================================================

#define NOSCAN_COUNT 10

static void
test_noscan_survival(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);

    test_obj_t *objs[NOSCAN_COUNT];

    for (int i = 0; i < NOSCAN_COUNT; i++) {
        objs[i]        = n00b_alloc_with_opts(test_obj_t, ARENA_OPTS_NOSCAN(arena));
        objs[i]->value = 0xCAFE0000ULL + (uint64_t)i;
        objs[i]->next  = nullptr;
    }

    n00b_stop_the_world();
    n00b_collect(arena);
    n00b_restart_the_world();

    for (int i = 0; i < NOSCAN_COUNT; i++) {
        assert(objs[i]->value == 0xCAFE0000ULL + (uint64_t)i);
    }

    printf("  [PASS] noscan survival\n");
}

// ============================================================================
// 10. Allocation after collection
// ============================================================================

static void
test_alloc_after_collection(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);

    test_obj_t *pre = n00b_alloc_with_opts(test_obj_t, ARENA_OPTS(arena));
    pre->value      = 0x1111;

    n00b_stop_the_world();
    n00b_collect(arena);
    n00b_restart_the_world();

    // Allocate 10 new objects on the post-collection arena.
    test_obj_t *post[10];

    for (int i = 0; i < 10; i++) {
        post[i] = n00b_alloc_with_opts(test_obj_t, ARENA_OPTS(arena));
        assert(post[i] != nullptr);
        post[i]->value = 0x2000ULL + (uint64_t)i;
    }

    for (int i = 0; i < 10; i++) {
        assert(post[i]->value == 0x2000ULL + (uint64_t)i);
    }

    // Pre-collection object must also survive.
    assert(pre->value == 0x1111);

    printf("  [PASS] alloc after collection\n");
}

// ============================================================================
// 11. Large linked list (GC fires mid-construction)
// ============================================================================

#define LIST_LENGTH 600

static void
test_large_linked_list(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);

    test_obj_t *head = nullptr;
    test_obj_t *prev = nullptr;

    for (int i = 0; i < LIST_LENGTH; i++) {
        test_obj_t *node = n00b_alloc_with_opts(test_obj_t, ARENA_OPTS(arena));
        node->value      = (uint64_t)i;
        node->next       = nullptr;

        if (prev) {
            prev->next = node;
        }
        else {
            head = node;
        }
        prev = node;
    }

    // With ~500 allocs filling a 32K segment, at least one collection
    // must have occurred during the 600-node build.  Walk the entire
    // list and verify every value.
    test_obj_t *cur = head;

    for (int i = 0; i < LIST_LENGTH; i++) {
        assert(cur != nullptr);
        assert(cur->value == (uint64_t)i);
        cur = (test_obj_t *)cur->next;
    }
    assert(cur == nullptr);

    printf("  [PASS] large linked list\n");
}

// ============================================================================
// 12. Memo table resize during collection
// ============================================================================

#define MEMO_RESIZE_COUNT 4096

static void
test_memo_resize_during_collection(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 1024 * 1024, .use_gc = true);

    test_obj_t *head = nullptr;

    for (int i = 0; i < MEMO_RESIZE_COUNT; i++) {
        test_obj_t *node = n00b_alloc_with_opts(test_obj_t, ARENA_OPTS(arena));
        node->value      = 0x600D0000ULL + (uint64_t)i;
        node->next       = head;
        head             = node;
    }

    n00b_stop_the_world();
    n00b_collect(arena);
    n00b_restart_the_world();

    uint64_t    expected = 0x600D0000ULL + (uint64_t)(MEMO_RESIZE_COUNT - 1);
    test_obj_t *cur      = head;

    for (int i = 0; i < MEMO_RESIZE_COUNT; i++) {
        assert(cur != nullptr);
        assert(cur->value == expected - (uint64_t)i);
        cur = (test_obj_t *)cur->next;
    }
    assert(cur == nullptr);

    printf("  [PASS] memo resize during collection\n");
}

static void
test_collect_skips_reserved_worker_slot(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    n00b_require(rt != nullptr, "runtime must be initialized");
    n00b_require(rt->default_arena != nullptr,
                 "default arena must be initialized");

    uint32_t slot = rt->max_threads;
    for (uint32_t i = 0; i < rt->max_threads; i++) {
        n00b_thread_t *expected = nullptr;
        if (n00b_atomic_cas(&rt->threads[i].thread,
                            &expected,
                            N00B_THREAD_SLOT_PLACEHOLDER)) {
            slot = i;
            break;
        }
    }
    n00b_require(slot < rt->max_threads,
                 "test requires an available worker slot");

    n00b_collect(rt->default_arena);

    n00b_thread_t *expected = N00B_THREAD_SLOT_PLACEHOLDER;
    n00b_require(n00b_atomic_cas(&rt->threads[slot].thread,
                                 &expected,
                                 nullptr),
                 "reserved worker slot changed during collection");

    printf("  [PASS] collect skips reserved worker slot\n");
}

#define RETAINED_SEGMENT_COUNT 1024

static uint64_t
count_retained_segments(n00b_arena_t *arena)
{
    uint64_t        count = 0;
    n00b_segment_t *seg   = arena->current_segment;

    for (; seg; seg = seg->next_segment) {
        count += seg->retained;
    }

    return count;
}

static void
test_pin_all_graph_across_retained_segments(void)
{
    if (!n00b_gc_pin_all_policy()) {
        return;
    }

    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);
    test_obj_t   *head  = nullptr;

    for (uint64_t i = 0; i < RETAINED_SEGMENT_COUNT; i++) {
        test_obj_t *node = n00b_alloc_with_opts(test_obj_t, ARENA_OPTS(arena));
        node->value      = 0x51000000ULL + i;
        node->next       = head;
        head             = node;
    }

    n00b_collect(arena);
    assert(count_retained_segments(arena) > 1);
    n00b_collect(arena);
    assert(count_retained_segments(arena) > 1);

    test_obj_t *node = head;
    for (uint64_t i = RETAINED_SEGMENT_COUNT; i > 0; i--) {
        assert(node != nullptr);
        assert(node->value == 0x51000000ULL + i - 1);
        node = node->next;
    }
    assert(node == nullptr);

    printf("  [PASS] pin-all graph across retained segments\n");
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running GC tests...\n");

    test_arena_metrics();
    test_basic_collection();
    test_object_survival();
    test_pointer_chain();
    test_unreachable_collected();
    test_multiple_collections();
    test_manual_collect();
    test_pause_counters_record_collection();
    test_header_address_false_positive();
    test_word_one_past_allocation_end();
    test_reservation_after_allocation();
    test_dead_object_past_retained_page();
    test_noscan_survival();
    test_alloc_after_collection();
    test_large_linked_list();
    test_memo_resize_during_collection();
    test_collect_skips_reserved_worker_slot();
    test_pin_all_graph_across_retained_segments();

    printf("All GC tests passed.\n");
    n00b_shutdown();
    return 0;
}
