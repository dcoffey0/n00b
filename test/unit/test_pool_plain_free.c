/* test_pool_plain_free.c: a plain n00b_free returns pool memory, for every
 * kind of pool.
 *
 * Freed storage goes back to its pool's free list and the next allocation of
 * that size takes it again, so a loop that frees what it allocates keeps
 * reusing one address. Counting distinct addresses counts the allocations
 * that were never freed. Some pools keep their pages out of the mmap tree,
 * where n00b_free looks owners up, so each kind runs the same loop.
 *
 * The churn case makes and destroys more pools than the pool registry has
 * slots, then holds many live at once, and checks that the newest still
 * frees and that no pool missed the registry.
 *
 * Run with no argument for every case, or with a case name for one.
 */

#define N00B_USE_INTERNAL_API
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "n00b.h"
#include "core/pool.h"
#include "core/runtime.h"

#define ROUNDS      2000
#define SEEN        64
#define ALLOC_BYTES 200
#define CHURN       70000
#define LIVE        4000

static int g_failures;

static int
distinct_after_plain_frees(n00b_allocator_t *a)
{
    void *seen[SEEN];
    int   n = 0;
    for (int i = 0; i < ROUNDS; i++) {
        void *p = n00b_alloc_array_with_opts(char, ALLOC_BYTES,
                                             &(n00b_alloc_opts_t){.allocator = a});
        bool known = false;
        for (int j = 0; j < n; j++) {
            if (seen[j] == p) {
                known = true;
            }
        }
        if (!known) {
            if (n == SEEN) {
                return ROUNDS;
            }
            seen[n++] = p;
        }
        n00b_free(p);
    }
    return n;
}

static void
check(const char *kind, n00b_allocator_t *a)
{
    int n = distinct_after_plain_frees(a);
    printf("    %s: %d alloc/free pairs used %s%d addresses\n", kind, ROUNDS,
           n == ROUNDS ? "at least " : "", n == ROUNDS ? SEEN : n);
    if (n != 1) {
        fprintf(stderr, "  [FAIL] %s: a plain free leaves memory allocated\n",
                kind);
        g_failures++;
        return;
    }
    printf("  [PASS] %s: a plain free returns memory\n", kind);
}

[[n00b::nomap]] static n00b_pool_t g_pool;

static void
case_system(void)
{
    check("system pool", (n00b_allocator_t *)&n00b_get_runtime()->system_pool);
}

static void
case_hidden(void)
{
    check("hidden pool", n00b_pool_init(&g_pool, .hidden = true));
    n00b_allocator_destroy((n00b_allocator_t *)&g_pool);
}

static void
case_hidden_inline(void)
{
    check("hidden pool with inline headers",
          n00b_pool_init(&g_pool, .hidden = true, .inline_headers = true));
    n00b_allocator_destroy((n00b_allocator_t *)&g_pool);
}

static void
case_external_md(void)
{
    check("hidden pool with external metadata",
          n00b_pool_init(&g_pool, .hidden = true, .external_metadata = true));
    n00b_allocator_destroy((n00b_allocator_t *)&g_pool);
}

static void
case_visible(void)
{
    check("GC-visible pool", n00b_pool_init(&g_pool, .hidden = false));
    n00b_allocator_destroy((n00b_allocator_t *)&g_pool);
}

static void
case_md(void)
{
    n00b_allocator_t *owner = n00b_pool_init(&g_pool, .external_metadata = true);
    assert(owner->metadata_pool != nullptr);
    check("metadata pool", owner->metadata_pool);
    n00b_allocator_destroy(owner);
}

static void
case_churn(void)
{
    uint64_t overflow0 = n00b_pool_global_stats().registry_overflow_count;

    for (int i = 0; i < CHURN; i++) {
        n00b_pool_init(&g_pool, .hidden = true, .scrub_locks_on_destroy = false);
        n00b_allocator_destroy((n00b_allocator_t *)&g_pool);
    }

    size_t       bytes = sizeof(n00b_pool_t) * LIVE;
    n00b_pool_t *live  = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(live != MAP_FAILED);
    for (int i = 0; i < LIVE; i++) {
        n00b_pool_init(&live[i], .hidden = true, .scrub_locks_on_destroy = false);
    }
    check("hidden pool after churn", (n00b_allocator_t *)&live[LIVE - 1]);
    for (int i = 0; i < LIVE; i++) {
        n00b_allocator_destroy((n00b_allocator_t *)&live[i]);
    }
    munmap(live, bytes);

    uint64_t missed = n00b_pool_global_stats().registry_overflow_count - overflow0;
    printf("    %d pools made and destroyed, %d held at once, %llu missed the "
           "registry\n",
           CHURN, LIVE, (unsigned long long)missed);
    if (missed != 0) {
        fprintf(stderr, "  [FAIL] pools missed the registry\n");
        g_failures++;
    }
}

static const struct {
    const char *name;
    void (*fn)(void);
} cases[] = {
    {"system", case_system},
    {"hidden", case_hidden},
    {"hidden_inline", case_hidden_inline},
    {"external_md", case_external_md},
    {"visible", case_visible},
    {"md", case_md},
    {"churn", case_churn},
};

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_pool_plain_free:\n");
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
