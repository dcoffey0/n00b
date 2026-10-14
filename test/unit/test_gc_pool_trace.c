/*
 * n00b#547: the collector traces THROUGH a default-configured, GC-visible pool.
 *
 * A pool initialized with n00b_pool_init's defaults is GC-visible: its pages go
 * into the per-collect scan tree, so the mark recognises a pointer INTO it.
 * Before this fix those defaults also gave it neither an inline header nor an
 * OOB record, so the mark could never resolve an object there and never
 * followed a pointer OUT of it. A GC-heap object whose only holder lived in such
 * a pool was invisible to the collector:
 *
 *   - copying: the referent moved (or was simply not copied) and the pool's
 *     pointer was never rewritten -- a stale pointer into from-space;
 *   - pin-all: the referent was unreachable, so its pages could be reclaimed
 *     out from under the pool's pointer.
 *
 * crashappsec/wax hits exactly this: each query's view and cursor come from a
 * stack-resident n00b_pool_t with the default options, and both hold the store
 * and its shards. test_rocs_query_race reproduces it as a race; this is the
 * deterministic form, with no race in it.
 *
 * Shape: one holder allocated from a default pool, reachable only through a
 * stack local; one payload on the default GC heap, reachable only through the
 * holder. Collect, churn the heap so a reclaimed payload would be overwritten,
 * and check the payload is intact and still where the holder says it is.
 * Registered under both collectors.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/arena.h"
#include "core/gc.h"
#include "core/pool.h"
#include "core/runtime.h"
#include "core/stw.h"
#include "util/assert.h"

#define CHECK(expr)                                                            \
    do {                                                                       \
        n00b_require((expr), "pool trace check failed: " #expr);               \
    } while (0)

#define PAYLOAD_MAGIC UINT64_C(0x547547547547C0DE)
// Large enough to occupy pages of its own, so under pin-all an unreachable
// payload's pages hold no other survivor and are actually reclaimed.
#define PAYLOAD_WORDS (64 * 1024 / 8)

typedef struct {
    uint64_t magic;
    uint64_t words[PAYLOAD_WORDS - 1];
} payload_t;

typedef struct {
    uint64_t   tag;
    payload_t *ref;
} holder_t;

static int failures;

static void
check(bool ok, const char *what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

// The payload is created in a frame that is gone by the time the collection
// runs, so no stack word can be what keeps it alive -- only holder->ref.
__attribute__((noinline)) static void
attach_payload(holder_t *holder)
{
    payload_t *p = n00b_alloc(payload_t);
    CHECK(p != nullptr);
    p->magic = PAYLOAD_MAGIC;
    for (uint64_t i = 0; i < PAYLOAD_WORDS - 1; i++) {
        p->words[i] = PAYLOAD_MAGIC ^ i;
    }
    holder->ref = p;
}

// Overwrite stack slots below this frame so a stale copy of the payload's
// address cannot pin it by accident.
__attribute__((noinline)) static void
scrub_stack(void)
{
    volatile uint8_t junk[64 * 1024];
    memset((void *)junk, 0, sizeof(junk));
    __asm__ __volatile__("" ::: "memory");
}

// Fill the heap with fresh garbage so a payload whose pages were reclaimed
// would read back as something else.
__attribute__((noinline)) static void
churn(void)
{
    for (int i = 0; i < 64; i++) {
        payload_t *g = n00b_alloc(payload_t);
        CHECK(g != nullptr);
        memset(g, 0xA5, sizeof(*g));
    }
}

static void
collect(void)
{
    n00b_arena_t *arena = n00b_get_runtime()->default_arena;
    n00b_stop_the_world();
    n00b_collect(arena);
    n00b_restart_the_world();
}

static bool
payload_intact(const payload_t *p)
{
    if (p == nullptr || p->magic != PAYLOAD_MAGIC) {
        return false;
    }
    for (uint64_t i = 0; i < PAYLOAD_WORDS - 1; i++) {
        if (p->words[i] != (PAYLOAD_MAGIC ^ i)) {
            return false;
        }
    }
    return true;
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("n00b#547: the collector traces through a default pool (%s)\n",
           n00b_gc_pin_all_policy() ? "pin-all" : "copying");

    // The pool defaults, exactly as wax's per-query pool uses them.
    n00b_pool_t       pool  = {};
    n00b_allocator_t *palloc = n00b_pool_init(&pool, .name = "pool_trace");

    check(!palloc->hidden, "a default pool is GC-visible");
    check(palloc->add_inline_header || palloc->metadata != nullptr,
          "a default pool can describe its own allocations");

    holder_t *holder = n00b_alloc_with_opts(holder_t,
                                            &(n00b_alloc_opts_t){
                                                .allocator = palloc,
                                            });
    CHECK(holder != nullptr);
    holder->tag = PAYLOAD_MAGIC;
    attach_payload(holder);
    check(payload_intact(holder->ref), "payload is intact before collecting");

    for (int round = 0; round < 3; round++) {
        scrub_stack();
        collect();
        churn();
    }

    // Read the tag before dereferencing ref: if the holder itself had been
    // damaged, report that rather than faulting through it.
    check(holder->tag == PAYLOAD_MAGIC, "holder is intact");
    check(payload_intact(holder->ref),
          "payload held only by a pool object survives three collections");

    n00b_allocator_destroy(palloc);

    if (failures) {
        printf("\nn00b#547: %d check(s) failed\n", failures);
        n00b_shutdown();
        return 1;
    }
    printf("\nn00b#547: a default pool's references are traced.\n");
    n00b_shutdown();
    return 0;
}
