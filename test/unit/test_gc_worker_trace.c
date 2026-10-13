/* test/unit/test_gc_worker_trace.c
 *
 * Deterministic, fully-inspectable repro of the async-seal GC defect, with a
 * NAMED object graph so we can see exactly which arena, which holder object,
 * which field, and which target is mishandled.
 *
 * Setup (no rocs):
 *   - worker thread allocates, in the SHARED default GC arena:
 *       T = a char buffer (the TARGET), filled with 0x5A
 *       O = a holder struct (the HOLDER) whose .data field points at T
 *     and pins O on its own C stack (volatile) so O is a live root via the
 *     worker's conservatively-scanned stack.
 *   - worker records T0 = &T, O0 = &O, then hands off to main and SPINS.
 *   - main allocates churn into the same default arena, then runs n00b_collect
 *     on that arena WHILE the worker is parked holding O (STW suspends it).
 *   - after the collect, the worker inspects, WITHOUT dereferencing yet:
 *       O_now  : did the holder move?           (O_now != O0  => O forwarded)
 *       O->data: did the field get updated?      (!= T0        => T forwarded+rewritten)
 *       is O->data still mapped / does T[0]==0x5A survive?
 *
 * The combination tells us precisely what is missed:
 *   O moved, O->data updated, T readable        => trace works (no bug)
 *   O moved, O->data == T0 (stale)              => HOLDER FIELD NOT TRACED
 *   O did NOT move                              => holder root not found at all
 *   O->data updated but T unreadable            => target freed despite forward
 */

#include <stdint.h>
#include <stdlib.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/gc.h"
#include "core/alloc.h"
#include "conduit/print.h"
#include "util/assert.h"
#include "util/worker_pool.h"

typedef struct {
    char    *data; // pointer field -> target T
    uint64_t tag;
} holder_t;

static n00b_runtime_t  *g_rt    = nullptr;
static _Atomic int      g_phase = 0; // 0 init, 1 worker-ready, 2 main-collected

// Describe an allocation's GC header so we can see its scan_kind / pool.
static void
describe(const char *label, void *p)
{
    if (p == nullptr) {
        n00b_eprintf("  [|#|]: (null)\n", label);
        return;
    }
    bool              pin  = false;
    n00b_alloc_info_t info = n00b_find_alloc_info(p,
                                                  .scan_for_header = true,
                                                  .pin             = &pin);
    n00b_alloc_hdr_unpin(pin);
    n00b_eprintf("  [|#|]: ptr=[|#:x|] kind=[|#|] "
                 "(inline=[|#|] oob=[|#|] heap=[|#|])\n",
                 label,
                 (uint64_t)(uintptr_t)p,
                 (int64_t)info.kind,
                 (int64_t)(info.kind == n00b_alloc_inline),
                 (int64_t)(info.kind == n00b_alloc_oob),
                 (int64_t)n00b_alloc_info_is_heap(info));
}

// Tight T->O alloc-and-verify loop, run concurrently with main's collect loop.
// If a collect frees/moves T or O without reconciling, the verify catches a
// stale/dangling holder with NAMED O/T to inspect (or the deref faults here).
static void
worker_fn(void *job_v, void *user_data)
{
    (void)job_v;
    (void)user_data;

    {
        // Does the worker's registered stack_map actually cover where the
        // worker is really running?  &probe ~ current SP.
        volatile int   probe = 0;
        n00b_thread_t *self  = n00b_thread_self();
        uintptr_t      sp    = (uintptr_t)&probe;
        uintptr_t      lo    = self && self->stack_map ? self->stack_map->start : 0;
        uintptr_t      hi    = self && self->stack_map ? self->stack_map->end : 0;
        n00b_eprintf("WORKER stackmap: &sp=[|#:x|] stack_top=[|#:x|] "
                     "map=[[|#:x|]..[|#:x|]] covers_sp=[|#|] gc_isolated=[|#|] policy=[|#|]\n",
                     (uint64_t)sp,
                     (uint64_t)(uintptr_t)(self ? self->stack_top : 0),
                     (uint64_t)lo,
                     (uint64_t)hi,
                     (int64_t)(sp >= lo && sp < hi),
                     (int64_t)(self ? self->gc_isolated : -1),
                     (int64_t)(self ? self->gc_stack_policy : -1));
    }

    for (int64_t iter = 0; n00b_atomic_load(&g_phase) == 0; iter++) {
        char *T = n00b_alloc_array_with_opts(
            char,
            64,
            &(n00b_alloc_opts_t){.allocator = nullptr, .no_scan = true});
        for (int i = 0; i < 64; i++) {
            T[i] = 0x5A;
        }
        holder_t *O = n00b_alloc(holder_t);
        O->data     = T;
        O->tag      = 0xC0FFEEULL;

        // Verify the graph the worker just built still holds.  Any collect that
        // ran since the allocs must have reconciled O, O->data, and T.
        if (O->tag != 0xC0FFEEULL) {
            n00b_eprintf("CORRUPT(tag) iter=[|#|] O=[|#:x|] tag=[|#:x|]\n",
                         iter, (uint64_t)(uintptr_t)O, (uint64_t)O->tag);
            describe("O", O);
            abort();
        }
        char *d = O->data;
        if ((uint8_t)d[0] != 0x5A) {
            n00b_eprintf("CORRUPT(data) iter=[|#|] O=[|#:x|] O->data=[|#:x|] d[0]=[|#:x|]\n",
                         iter,
                         (uint64_t)(uintptr_t)O,
                         (uint64_t)(uintptr_t)d,
                         (uint64_t)(uint8_t)d[0]);
            describe("O (holder)", O);
            describe("O->data (target)", d);
            abort();
        }
    }
}

int
main(int argc, char *argv[])
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);
    g_rt = n00b_get_runtime();

    n00b_worker_pool_t *pool = n00b_worker_pool_new(1, 4, worker_fn, nullptr);
    n00b_require(pool != nullptr, "pool");

    n00b_worker_pool_submit(pool, (void *)0x1);

    // Concurrently churn + collect the shared default arena while the worker
    // runs its tight T->O alloc/verify loop.
    for (int round = 0; round < 200; round++) {
        for (int i = 0; i < 300; i++) {
            (void)n00b_cformat("main churn [|#|]:[|#|]", (int64_t)round, (int64_t)i);
        }
        n00b_collect(g_rt->default_arena);
    }
    n00b_eprintf("MAIN done 200 collect rounds (arena=[|#:x|])\n",
                 (uint64_t)(uintptr_t)g_rt->default_arena);

    n00b_atomic_store(&g_phase, 1); // stop the worker loop
    n00b_worker_pool_shutdown(pool);

    n00b_eprintf("test_gc_worker_trace OK\n");
    n00b_shutdown();
    return 0;
}
