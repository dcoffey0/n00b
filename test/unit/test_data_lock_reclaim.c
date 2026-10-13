/* test_data_lock_reclaim.c: a structure's lock goes when the structure does.
 *
 * A buffer takes its lock from the system pool, and a list from the pool it
 * is made in, such as the conduit pool for an HTTP/1 header bag. Freeing the
 * structure is what frees the lock. A freed lock's storage is the next one's,
 * so a loop that makes and frees structures by hand should see few lock
 * addresses. Each lock the free leaves behind adds a new one.
 *
 * Run with no argument for every case, or with a case name for one.
 */

#define N00B_USE_INTERNAL_API
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "adt/list.h"
#include "core/buffer.h"
#include "core/runtime.h"

#define ROUNDS 1000
#define SEEN   64

static int g_failures;

typedef struct {
    void *seen[SEEN];
    int   n;
} addrs_t;

static void
note(addrs_t *a, void *lock)
{
    assert(lock != nullptr);
    for (int j = 0; j < a->n; j++) {
        if (a->seen[j] == lock) {
            return;
        }
    }
    if (a->n < SEEN) {
        a->seen[a->n++] = lock;
    }
}

static void
report(const char *name, addrs_t *a)
{
    printf("    %s: %d owners' locks used %s%d addresses\n", name, ROUNDS,
           a->n == SEEN ? "at least " : "", a->n);
    if (a->n > SEEN / 2) {
        fprintf(stderr, "  [FAIL] %s keep their locks\n", name);
        g_failures++;
        return;
    }
    printf("  [PASS] %s free their locks\n", name);
}

static void
case_buffers(void)
{
    addrs_t a = {};
    for (int i = 0; i < ROUNDS; i++) {
        n00b_buffer_t *b = n00b_buffer_empty();
        note(&a, b->lock);
        n00b_buffer_free(b);
    }
    report("freed buffers", &a);
}

// Lists in the conduit pool, made and freed the way an HTTP/1 header bag
// makes and frees its item list.
static void
case_lists(void)
{
    n00b_allocator_t *cpool = (n00b_allocator_t *)&n00b_get_runtime()->conduit_pool;
    addrs_t           a     = {};
    for (int i = 0; i < ROUNDS; i++) {
        n00b_list_t(void *) l = n00b_list_new(void *, .allocator = cpool);
        note(&a, l.lock);
        n00b_list_free(l);
    }
    report("freed pool lists", &a);
}

static const struct {
    const char *name;
    void (*fn)(void);
} cases[] = {
    {"buffers", case_buffers},
    {"lists", case_lists},
};

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_data_lock_reclaim:\n");
    bool ran = false;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        if (argc > 1 && strcmp(argv[1], cases[i].name) != 0) {
            continue;
        }
        cases[i].fn();
        ran = true;
    }
    if (!ran) {
        fprintf(stderr, "  [FAIL] no case named %s\n", argv[1]);
        g_failures++;
    }
    n00b_shutdown();
    return g_failures == 0 ? 0 : 1;
}
