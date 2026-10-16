/*
 * n00b#557: a locked list survives a copying collection with its lock intact.
 *
 * A list allocates its lock from its own allocator, which is normally the
 * collected default arena. ncc's GC type map left that `lock` field out: for
 * any struct carrying n00b's scan triple (scan_kind, scan_cb, scan_user) it
 * skipped `lock` as runtime infrastructure. So the collector neither kept the
 * lock alive through the list nor fixed up the pointer. With the precise map
 * linked, a copying collection moved the list header and its data, released
 * the from-space holding the lock, and left `lock` pointing at it. The next
 * n00b_list_len faulted in the read lock. Under pin-all nothing moves, which
 * is why it stayed latent in every shipped binary.
 *
 * The fix is crashappsec/ncc#66: `lock` is a managed edge. This test checks
 * both what ncc must emit and what that buys at run time:
 *
 *   1. With a precise map linked, the list header's layout includes `lock`.
 *      This fails deterministically under either collector if ncc regresses.
 *   2. crashappsec/wax's telemetry queue, as it is written there, survives a
 *      collection: a heap list header made inside an allocator scope on the
 *      default heap, published through a file-scope pinref.
 *   3. A list handed the default arena explicitly does too.
 *
 * Only the copying run exercises 2 and 3, and only in a build where the map is
 * linked (build.sh's ObjC link wrapper on macOS). Elsewhere nothing moves.
 */

#include <stddef.h>
#include <stdio.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/arena.h"
#include "core/codegen_abi.h"
#include "core/gc.h"
#include "core/gc_map.h"
#include "core/mem/pinref.h"
#include "core/runtime.h"
#include "core/stw.h"

typedef struct {
    n00b_string_t *line;
    bool           drop_okay;
} item_t;

typedef n00b_list_t(item_t *) item_list_t;

#define ITEMS 3

static n00b_pinref_t g_queue;
static int           failures;

static void
check(bool ok, const char *what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

// True if the list header's GC layout names `lock` as a pointer.
static bool
layout_maps_lock(void)
{
    const n00b_gc_struct_layout_t *layout
        = n00b_gc_type_map_lookup(typehash(item_list_t *));
    if (layout == nullptr) {
        return false;
    }
    uint64_t lock_word = offsetof(item_list_t, lock) / sizeof(void *);
    for (uint64_t i = 0; i < layout->offset_count; i++) {
        if (layout->offsets[i] == lock_word) {
            return true;
        }
    }
    return false;
}

static void
push_items(item_list_t *q)
{
    for (int i = 0; i < ITEMS; i++) {
        item_t *it = n00b_alloc(item_t);
        it->line   = n00b_string_from_cstr("queued");
        n00b_list_push(*q, it);
    }
}

// Built in a frame that is gone by the collection, so only the pinref keeps
// the queue alive.
__attribute__((noinline)) static void
build_queue(void)
{
    n00b_allocator_scope_t scope
        = n00b_allocator_scope_enter(n00b_default_allocator());
    item_list_t *q = n00b_alloc(item_list_t);
    *q             = n00b_list_new(item_t *);
    n00b_allocator_scope_exit(&scope);

    n00b_pinref_init(&g_queue, q);
    push_items(q);
}

__attribute__((noinline)) static item_list_t *
build_explicit(void)
{
    item_list_t *q = n00b_alloc(item_list_t);
    *q             = n00b_list_new(item_t *, .allocator = n00b_default_allocator());
    push_items(q);
    return q;
}

static void
collect(void)
{
    n00b_stop_the_world();
    n00b_collect(n00b_get_runtime()->default_arena);
    n00b_restart_the_world();
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("n00b#557: a locked list keeps its lock across a collection (%s)\n",
           n00b_gc_pin_all_policy() ? "pin-all" : "copying");

    if (n00b_gc_type_map_available()) {
        check(layout_maps_lock(), "the list header's GC layout includes its lock");
    }
    else {
        printf("  [NOTE] no precise type map linked; the layout is not checked\n");
    }

    build_queue();

    // Held only through a stack slot, as an ordinary caller would hold it.
    item_list_t *volatile explicit_q = build_explicit();

    collect();

    item_list_t *q = n00b_pinref_pin(&g_queue);
    check(q != nullptr, "the queue survives the collection");
    if (q != nullptr) {
        check(q->lock != nullptr, "the queue still has a lock");
        // The fault #557 reported: a read lock through a stale pointer.
        check(n00b_list_len(*q) == ITEMS, "the queue's length reads back");
        push_items(q);
        check(n00b_list_len(*q) == 2 * ITEMS,
              "the queue takes its write lock after the collection");
    }
    n00b_pinref_unpin(&g_queue);

    check(n00b_list_len(*explicit_q) == ITEMS,
          "a list given the default arena reads back too");

    if (failures) {
        printf("\nn00b#557: %d check(s) failed\n", failures);
        fflush(stdout);
        n00b_shutdown();
        return 1;
    }
    printf("\nn00b#557: list locks survive collections.\n");
    fflush(stdout);
    n00b_shutdown();
    return 0;
}
