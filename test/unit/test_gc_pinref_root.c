// ============================================================================
// n00b#533: a file-scope n00b_pinref_t must root what it points at.
// ============================================================================
//
// Reported against crashappsec/wax: an object published through a file-scope
// n00b_pinref_t is lost across n00b_collect(), but ONLY in a build where the
// precise GC type-map dictionary is linked (strong n00b_gcmap_count). With
// the dictionary absent the conservative collector pins everything and the
// bug is masked -- which is every shipped wax binary today, and which
// crashappsec/wax#1285 is about to change.
//
// WHY THE SHAPE MATTERS. test_gc_auto_roots_e2e.c already covers
// `static T *name;` -- a bare pointer, which ncc's --ncc-auto-gc-roots
// transform recognises and emits into the per-TU root table. A pinref is
// NOT that shape:
//
//     typedef struct { void *raw; uint32_t pins; uint32_t lock : 1; }
//         n00b_raw_pinref_t;
//     typedef _Atomic n00b_raw_pinref_t n00b_pinref_t;
//
// It is an _Atomic STRUCT whose first member happens to be the managed
// pointer. Nothing in the collector knows that: `grep -rn pinref src/ include/`
// outside pinref.h finds only rocs/store.c, where every pinref is a STRUCT
// MEMBER reached through its parent object's type map -- never a file-scope
// static. So the one place the shape is unrooted is the one place nothing
// in-tree exercises.
//
// WHAT THIS TEST PINS. Three things, in order of how sharply they fail:
//
//   1. the pinref's target survives a collection at all;
//   2. the pointer is still the SAME address afterwards (a moving collector
//      that relocated the object without updating the pinref would satisfy
//      "survives" by accident if we only checked contents);
//   3. the contents are byte-intact, so a recycled-but-overwritten address
//      cannot pass.
//
// It is deliberately written to fail LOUDLY rather than crash: the reported
// symptom is a SIGSEGV inside _n00b_rw_read_lock (the list's own lock, read
// through a stale handle), and a segfault in CI says far less than an
// assertion naming which invariant broke. So the checks read the raw pointer
// and a magic word before ever touching a lock-bearing object.
//
// NEGATIVE CONTROL. e2e-style: a sibling allocation that nothing roots must
// be collectible, so a pass cannot come from "this collector never frees
// anything". Without it, a build where n00b_collect() is a no-op would pass
// this test while proving nothing.

#include <assert.h>
#include <stdio.h>

#include "n00b.h"
#include "conduit/print.h"
#include "core/codegen_abi_inject.h"
#include "core/alloc.h"
#include "core/arena.h"
#include "core/gc.h"
#include "core/mem/pinref.h"
#include "core/runtime.h"
#include "core/stw.h"

typedef struct {
    uint64_t magic;
    uint64_t tag;
    void    *placeholder; // pointer slot, so the type is scanned
} pinref_payload_t;

#define PINREF_MAGIC   UINT64_C(0xD00DFEED53300001)
#define PINREF_TAG     UINT64_C(0xABCDEF0053300001)
#define CONTROL_MAGIC  UINT64_C(0xD00DFEED53300002)

#define ARENA_OPTS(a)                                                          \
    &(n00b_alloc_opts_t)                                                       \
    {                                                                          \
        .allocator = (n00b_allocator_t *)(a)                                   \
    }

// The surface under test: a file-scope pinref, exactly as wax declares its
// telemetry queue. NOT hand-registered with n00b_gc_register_root -- whatever
// roots this has to come from the pinref being understood, which is the whole
// point of the test.
static n00b_pinref_t g_pinref;

// Negative control: same file, same allocation site shape, deliberately
// unrooted. If a collection cannot reclaim THIS, the test proves nothing
// about the one above.
[[n00b::nomap]] static pinref_payload_t *g_unrooted;

static int failures;

static void
check(bool ok, const char *what)
{
    if (!ok) {
        n00b_printf("  [FAIL] [|#|]", n00b_string_from_cstr(what));
        failures++;
        return;
    }
    n00b_printf("  [PASS] [|#|]", n00b_string_from_cstr(what));
}

// Publish through the pinref from a frame that is dead by the time the
// collection runs, so the stack cannot be what keeps the object alive --
// the same discipline test_gc.c and test_gc_auto_roots_e2e.c use.
__attribute__((noinline)) static void *
publish(n00b_arena_t *arena)
{
    pinref_payload_t *p = n00b_alloc_with_opts(pinref_payload_t,
                                               ARENA_OPTS(arena));
    p->magic            = PINREF_MAGIC;
    p->tag              = PINREF_TAG;
    p->placeholder      = nullptr;

    n00b_pinref_init(&g_pinref, p);
    return p;
}

__attribute__((noinline)) static void
publish_unrooted(n00b_arena_t *arena)
{
    pinref_payload_t *p = n00b_alloc_with_opts(pinref_payload_t,
                                               ARENA_OPTS(arena));
    p->magic            = CONTROL_MAGIC;
    p->tag              = 0;
    p->placeholder      = nullptr;
    g_unrooted          = p;
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    n00b_runtime_t *runtime = n00b_get_runtime();
    n00b_arena_t   *arena   = runtime->default_arena;

    n00b_printf("n00b#533: a file-scope pinref roots what it points at");

    void *before = publish(arena);
    publish_unrooted(arena);

    // Sanity: the pinref reads back what we put in, before any collection.
    // If this fails the test is broken, not the collector.
    pinref_payload_t *p0 = n00b_pinref_pin(&g_pinref);
    check(p0 == before, "pinref returns the published pointer");
    check(p0 != nullptr && p0->magic == PINREF_MAGIC,
          "payload is intact before the collection");
    n00b_pinref_unpin(&g_pinref);

    n00b_stop_the_world();
    n00b_collect(arena);
    n00b_restart_the_world();

    // The reported failure. Read the RAW pointer first and check a plain
    // word before touching anything lock-bearing, so a regression reports
    // itself instead of faulting inside n00b_list_len's read lock.
    pinref_payload_t *p1 = n00b_pinref_pin(&g_pinref);

    check(p1 != nullptr, "pinref is non-null after the collection");

    if (p1 != nullptr) {
        // A moving collector that relocated the payload without updating the
        // pinref would leave a plausible-looking but stale address here.
        check(p1 == before, "pinref still points at the same object");
        check(p1->magic == PINREF_MAGIC,
              "payload magic survived the collection");
        check(p1->tag == PINREF_TAG, "payload tag survived the collection");
    }
    n00b_pinref_unpin(&g_pinref);

    if (failures) {
        n00b_printf(
            "\n[|#|] check(s) failed: an object published through a "
            "file-scope n00b_pinref_t did not survive a collection (n00b#533)",
            (int64_t)failures);
        n00b_shutdown();
        return 1;
    }

    n00b_printf("\nn00b#533: a file-scope pinref keeps its target reachable.");
    n00b_shutdown();
    return 0;
}
