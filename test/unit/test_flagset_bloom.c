#include <stdio.h>

#include "n00b.h"
#include "adt/bloom.h"
#include "adt/flagset.h"
#include "core/runtime.h"
#include "util/assert.h"
#include "test_check.h"

static void
test_flagset_lock_defaults(void)
{
    n00b_flagset_t *locked = n00b_flagset_new(.length = 2);
    CHECK(locked->lock != nullptr);

    n00b_flagset_t *private = n00b_flagset_new(.length = 2, .locked = false);
    CHECK(private->lock == nullptr);

    n00b_flagset_set_index(locked, 1, true);
    n00b_flagset_set_index(private, 1, true);
    CHECK(n00b_flagset_index(locked, 1));
    CHECK(n00b_flagset_index(private, 1));
    CHECK(n00b_flagset_test_and_set_index(locked, 1, true));
    CHECK(!n00b_flagset_test_and_set_index(locked, 3, true));
    CHECK(n00b_flagset_index(locked, 3));
    CHECK(n00b_flagset_test_and_set_index(locked, 3, false));
    CHECK(!n00b_flagset_index(locked, 3));
}

static void
test_flagset_resize_and_index(void)
{
    n00b_flagset_t *flags = n00b_flagset_new(.length = 3);
    CHECK(n00b_flagset_len(flags) == 3);
    CHECK(!n00b_flagset_index(flags, 1));

    n00b_flagset_set_index(flags, 1, true);
    CHECK(n00b_flagset_index(flags, 1));
    CHECK(n00b_flagset_index(flags, -2));

    CHECK(!n00b_flagset_index(flags, 130));
    CHECK(n00b_flagset_len(flags) == 3);

    n00b_flagset_set_index(flags, 130, true);
    CHECK(n00b_flagset_len(flags) == 131);
    CHECK(n00b_flagset_index(flags, 130));
    CHECK(n00b_flagset_count(flags) == 2);

    n00b_flagset_set_index(flags, 130, false);
    CHECK(!n00b_flagset_index(flags, 130));
    CHECK(n00b_flagset_count(flags) == 1);
}

static void
test_flagset_set_operations(void)
{
    n00b_flagset_t *a = n00b_flagset_new(.length = 8);
    n00b_flagset_t *b = n00b_flagset_new(.length = 72);
    n00b_flagset_set_index(a, 1, true);
    n00b_flagset_set_index(a, 7, true);
    n00b_flagset_set_index(b, 7, true);
    n00b_flagset_set_index(b, 70, true);

    n00b_flagset_t *sum = n00b_flagset_add(a, b);
    CHECK(n00b_flagset_index(sum, 1));
    CHECK(n00b_flagset_index(sum, 7));
    CHECK(n00b_flagset_index(sum, 70));
    CHECK(n00b_flagset_count(sum) == 3);

    n00b_flagset_t *intersection = n00b_flagset_test(a, b);
    CHECK(!n00b_flagset_index(intersection, 1));
    CHECK(n00b_flagset_index(intersection, 7));
    CHECK(!n00b_flagset_index(intersection, 70));
    CHECK(n00b_flagset_count(intersection) == 1);

    n00b_flagset_t *diff = n00b_flagset_sub(sum, b);
    CHECK(n00b_flagset_index(diff, 1));
    CHECK(!n00b_flagset_index(diff, 7));
    CHECK(!n00b_flagset_index(diff, 70));

    n00b_flagset_t *xor_set = n00b_flagset_xor(a, b);
    CHECK(n00b_flagset_index(xor_set, 1));
    CHECK(!n00b_flagset_index(xor_set, 7));
    CHECK(n00b_flagset_index(xor_set, 70));

    n00b_flagset_t *copy = n00b_flagset_copy(a);
    CHECK(n00b_flagset_eq(a, copy));
    CHECK(!n00b_flagset_eq(a, b));
}

static void
test_flagset_next_set_and_invert(void)
{
    n00b_flagset_t *flags = n00b_flagset_new(.length = 10);
    n00b_flagset_set_index(flags, 2, true);
    n00b_flagset_set_index(flags, 9, true);

    uint64_t ix = 0;
    CHECK(n00b_flagset_next_set(flags, 0, &ix));
    CHECK(ix == 2);
    CHECK(n00b_flagset_next_set(flags, 3, &ix));
    CHECK(ix == 9);
    CHECK(!n00b_flagset_next_set(flags, 10, &ix));

    n00b_flagset_t *inverted = n00b_flagset_invert(flags);
    CHECK(n00b_flagset_len(inverted) == 10);
    CHECK(!n00b_flagset_index(inverted, 2));
    CHECK(!n00b_flagset_index(inverted, 9));
    CHECK(n00b_flagset_count(inverted) == 8);
}

// Setting bits 0..n-1 in order is how a dense posting list fills. Each change
// of alloc_wordlen is one reallocation and a copy of the whole array, so the
// count of changes has to grow with log n, not with n/64. Both setters grow
// the array: n00b_flagset_set_index, and n00b_flagset_set_owned, which a hot
// dense posting list fills through.
static void
set_by_index(n00b_flagset_t *flags, uint64_t i)
{
    n00b_flagset_set_index(flags, (int64_t)i, true);
}

static void
set_owned(n00b_flagset_t *flags, uint64_t i)
{
    CHECK(n00b_flagset_set_owned(flags, i));
}

static void
check_ascending_growth(const char *name,
                       void (*set)(n00b_flagset_t *, uint64_t))
{
    const uint64_t  n       = UINT64_C(1) << 16;
    n00b_flagset_t *flags   = n00b_flagset_new(.length = 64, .locked = false);
    uint64_t        reallocs = 0;
    uint64_t        words    = flags->alloc_wordlen;

    for (uint64_t i = 0; i < n; i++) {
        set(flags, i);
        if (flags->alloc_wordlen != words) {
            reallocs++;
            words = flags->alloc_wordlen;
        }
    }

    // 64 bits start in one word, and n bits need n/64 = 2^10 words, so
    // doubling takes 10 steps. Exact-fit growth takes 1023.
    printf("flagset ascending growth through %s: %llu reallocations for %llu "
           "bits\n",
           name,
           (unsigned long long)reallocs,
           (unsigned long long)n);
    CHECK(reallocs <= 11);
    CHECK(n00b_flagset_len(flags) == n);
    CHECK(n00b_flagset_count(flags) == n);
    CHECK(flags->alloc_wordlen >= n / 64);
}

static void
test_flagset_ascending_growth_reallocates_logarithmically(void)
{
    check_ascending_growth("set_index", set_by_index);
    check_ascending_growth("set_owned", set_owned);
}

// A set grown past its exact size carries zero words after the last used one.
// Every operation reads only the used words, so the slack never shows.
static void
test_flagset_operations_ignore_growth_slack(void)
{
    n00b_flagset_t *flags = n00b_flagset_new(.length = 64);
    n00b_flagset_set_index(flags, 64, true);
    n00b_flagset_set_index(flags, 128, true);
    CHECK(n00b_flagset_len(flags) == 129);
    CHECK(flags->alloc_wordlen == 4);

    n00b_flagset_t *copy = n00b_flagset_copy(flags);
    CHECK(n00b_flagset_eq(flags, copy));
    CHECK(n00b_flagset_eq(copy, flags));
    CHECK(n00b_flagset_count(copy) == 2);

    n00b_flagset_t *inverted = n00b_flagset_invert(flags);
    CHECK(n00b_flagset_count(inverted) == 127);
    CHECK(!n00b_flagset_index(inverted, 128));

    n00b_flagset_t *exact = n00b_flagset_new(.length = 129);
    n00b_flagset_set_index(exact, 64, true);
    n00b_flagset_set_index(exact, 128, true);
    CHECK(exact->alloc_wordlen == 3);
    CHECK(n00b_flagset_eq(flags, exact));
    CHECK(n00b_flagset_count(n00b_flagset_xor(flags, exact)) == 0);
    CHECK(n00b_flagset_count(n00b_flagset_add(flags, exact)) == 2);
    CHECK(n00b_flagset_count(n00b_flagset_test(flags, exact)) == 2);
    CHECK(n00b_flagset_count(n00b_flagset_sub(flags, exact)) == 0);

    uint64_t ix = 0;
    CHECK(n00b_flagset_next_set(flags, 65, &ix));
    CHECK(ix == 128);
    CHECK(!n00b_flagset_next_set(flags, 129, &ix));

    // Growing within the slack exposes only zero bits.
    n00b_flagset_set_index(flags, 200, true);
    CHECK(flags->alloc_wordlen == 4);
    CHECK(n00b_flagset_count(flags) == 3);

    // Trimming keeps the used words, which here are all of them.
    n00b_flagset_trim(flags);
    CHECK(flags->alloc_wordlen == 4);
    CHECK(n00b_flagset_count(flags) == 3);

    n00b_flagset_t *grown = n00b_flagset_new(.length = 64);
    n00b_flagset_set_index(grown, 64, true);
    n00b_flagset_set_index(grown, 128, true);
    CHECK(grown->alloc_wordlen == 4);
    n00b_flagset_trim(grown);
    CHECK(grown->alloc_wordlen == 3);
    CHECK(n00b_flagset_eq(grown, exact));
}

static void
test_bloom_basic_membership(void)
{
    n00b_bloom_t *bloom = n00b_bloom_new(.set_size = 32,
                                         .false_pct = 0.001);
    n00b_string_t *alpha = r"alpha";
    n00b_string_t *beta  = r"beta";

    CHECK(!n00b_bloom_contains(bloom, alpha));
    n00b_bloom_add(bloom, alpha);
    n00b_bloom_add(bloom, beta);
    CHECK(n00b_bloom_contains(bloom, alpha));
    CHECK(n00b_bloom_contains(bloom, beta));
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    test_flagset_lock_defaults();
    test_flagset_resize_and_index();
    test_flagset_set_operations();
    test_flagset_next_set_and_invert();
    test_flagset_ascending_growth_reallocates_logarithmically();
    test_flagset_operations_ignore_growth_slack();
    test_bloom_basic_membership();

    n00b_shutdown();
    return 0;
}
