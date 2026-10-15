/*
 * What a posting list says about its own size, and who may trust it.
 *
 * A dense list keeps membership in a bitmap and its size in a separate count.
 * Sealing recomputes that count from the bitmap, so sealed readers are safe
 * whatever the hot side did; a hot reader gets whatever insert left there.
 * The df probe reads it on both sides, which is what makes the hot side's
 * bookkeeping observable.
 */

#include <stdint.h>

#include "n00b.h"
#include "conduit/print.h"
#include "core/pool.h"
#include "core/runtime.h"
#include "text/strings/format.h"
#include "util/assert.h"

#include <rocs/n00b_rocs.h>

#include "internal/rocs/index.h"
#include "internal/rocs/map.h"
#include "internal/rocs/eval.h"
#include "internal/rocs/plan_ir.h"
#include "rocs_test_support.h"
#include "test_check.h"

#define SHARD_ID UINT64_C(0xD0E5)
#define SEAL_TS  UINT64_C(909)
#define RECORDS  UINT64_C(5)

typedef struct {
    n00b_store_shard_t *shard;
    n00b_store_index_t *level;
    n00b_store_index_t *kind;
    n00b_store_index_t *message;
} sample_t;

typedef struct {
    n00b_store_map_t       *map;
    n00b_store_map_shard_t *root;
} mapped_t;

static n00b_store_index_t *
index_for(n00b_string_t *field, n00b_store_postings_kind_t postings)
{
    auto index_r = n00b_store_index_new(field, N00B_STORE_INDEX_TERM, .postings = postings);
    CHECK(n00b_result_is_ok(index_r));
    CHECK(n00b_result_get(index_r) != nullptr);
    return n00b_result_get(index_r);
}

static n00b_store_index_t *
fulltext_for(n00b_string_t *field)
{
    auto index_r = n00b_store_index_new(field, N00B_STORE_INDEX_FULLTEXT);
    CHECK(n00b_result_is_ok(index_r));
    CHECK(n00b_result_get(index_r) != nullptr);
    return n00b_result_get(index_r);
}

static n00b_json_node_t *
record_with(n00b_string_t *level, n00b_string_t *kind, n00b_string_t *message)
{
    n00b_json_node_t *record = n00b_json_object_new();
    n00b_json_object_put_n00b(record, r"level", n00b_json_string_new_from_n00b(level));
    n00b_json_object_put_n00b(record, r"kind", n00b_json_string_new_from_n00b(kind));
    n00b_json_object_put_n00b(record, r"message", n00b_json_string_new_from_n00b(message));
    return record;
}

static sample_t
sample(void)
{
    auto shard_r
        = n00b_store_shard_new(.shard_id = SHARD_ID, .allocator = test_shard_allocator());
    CHECK(n00b_result_is_ok(shard_r));

    sample_t out = {
        .shard   = n00b_result_get(shard_r),
        .level   = index_for(r"level", N00B_STORE_POSTINGS_DENSE),
        .kind    = index_for(r"kind", N00B_STORE_POSTINGS_SPARSE),
        .message = fulltext_for(r"message"),
    };

    n00b_string_t *levels[]   = {r"error", r"warn", r"error", r"info", r"error"};
    n00b_string_t *kinds[]    = {r"build", r"build", r"test", r"build", r"test"};
    // "alpha" appears in four, "beta" in one. An intersecting lookup for both
    // matches one record, which the smallest term's count bounds at one.
    n00b_string_t *messages[] = {
        r"alpha one",
        r"alpha two",
        r"alpha beta",
        r"alpha three",
        r"gamma four",
    };

    for (uint64_t i = 0; i < RECORDS; i++) {
        auto append_r
            = n00b_store_shard_append(out.shard, record_with(levels[i], kinds[i], messages[i]));
        CHECK(n00b_result_is_ok(append_r));
        uint64_t ordinal = n00b_result_get(append_r);

        CHECK(n00b_result_is_ok(n00b_store_index_add(out.level, out.shard, ordinal)));
        CHECK(n00b_result_is_ok(n00b_store_index_add(out.kind, out.shard, ordinal)));
        CHECK(n00b_result_is_ok(n00b_store_index_add(out.message, out.shard, ordinal)));
    }

    return out;
}

static mapped_t
seal_and_map(n00b_store_shard_t *shard)
{
    auto seal_r = n00b_store_shard_seal(shard, .seal_ts = SEAL_TS, .base_address = 0xD00000u);
    CHECK(n00b_result_is_ok(seal_r));

    auto map_r = n00b_store_map_open_buffer(n00b_result_get(seal_r));
    CHECK(n00b_result_is_ok(map_r));

    auto root_r = n00b_store_map_root(n00b_result_get(map_r));
    CHECK(n00b_result_is_ok(root_r));

    return (mapped_t){
        .map  = n00b_result_get(map_r),
        .root = n00b_result_get(root_r),
    };
}

static void
check_postings(n00b_store_postings_t *postings,
               uint64_t               generation,
               const uint64_t        *expected,
               uint64_t               expected_len)
{
    auto len_r = n00b_store_postings_len(postings);
    CHECK(n00b_result_is_ok(len_r));
    CHECK(n00b_result_get(len_r) == expected_len);

    for (uint64_t i = 0; i < expected_len; i++) {
        auto posting_r = n00b_store_postings_get(postings, i);
        CHECK(n00b_result_is_ok(posting_r));
        n00b_option_t(n00b_store_posting_t) opt = n00b_result_get(posting_r);
        CHECK(n00b_option_is_set(opt));
        n00b_store_posting_t posting = n00b_option_get(opt);
        CHECK(posting.pos.shard_id == SHARD_ID);
        CHECK(posting.pos.generation == generation);
        CHECK(posting.pos.ordinal == expected[i]);
    }
}

static void
check_df(n00b_result_t(n00b_store_index_stats_t) stats_r, uint64_t df)
{
    CHECK(n00b_result_is_ok(stats_r));
    n00b_store_index_stats_t stats = n00b_result_get(stats_r);
    CHECK(stats.record_count == RECORDS);
    CHECK(stats.document_frequency == df);
}

// A dense index and a sparse one over the same shard answer identically. Both
// resolve their postings by walking the representation rather than by trusting
// the count, so this passes either way and is here as the parity baseline the
// df assertions below are measured against.
static void
test_dense_and_sparse_agree_hot_and_sealed(void)
{
    sample_t          s     = sample();
    n00b_json_node_t *error = n00b_json_string_new_from_n00b(r"error");
    n00b_json_node_t *build = n00b_json_string_new_from_n00b(r"build");

    uint64_t level_error[] = {0, 2, 4};
    uint64_t kind_build[]  = {0, 1, 3};

    auto hot_dense_r = n00b_store_index_lookup(s.level, s.shard, error);
    CHECK(n00b_result_is_ok(hot_dense_r));
    check_postings(n00b_result_get(hot_dense_r), 0, level_error, 3);
    check_df(n00b_store_index_stats_hot(s.level, s.shard, error), 3);

    auto hot_sparse_r = n00b_store_index_lookup(s.kind, s.shard, build);
    CHECK(n00b_result_is_ok(hot_sparse_r));
    check_postings(n00b_result_get(hot_sparse_r), 0, kind_build, 3);
    check_df(n00b_store_index_stats_hot(s.kind, s.shard, build), 3);

    mapped_t mapped = seal_and_map(s.shard);

    auto sealed_dense_r = n00b_store_index_lookup_mapped(s.level, mapped.root, error);
    CHECK(n00b_result_is_ok(sealed_dense_r));
    check_postings(n00b_result_get(sealed_dense_r), SEAL_TS, level_error, 3);
    check_df(n00b_store_index_stats_mapped(s.level, mapped.root, error), 3);

    auto sealed_sparse_r = n00b_store_index_lookup_mapped(s.kind, mapped.root, build);
    CHECK(n00b_result_is_ok(sealed_sparse_r));
    check_postings(n00b_result_get(sealed_sparse_r), SEAL_TS, kind_build, 3);
    check_df(n00b_store_index_stats_mapped(s.kind, mapped.root, build), 3);

    CHECK(n00b_result_is_ok(n00b_store_map_close(mapped.map)));

    n00b_printf("  [PASS] dense and sparse postings agree, hot and sealed");
}

// A term nobody indexed has a document frequency of zero, which the planner
// reads as an exact answer rather than a missing statistic.
static void
test_absent_term_has_zero_df(void)
{
    sample_t          s      = sample();
    n00b_json_node_t *absent = n00b_json_string_new_from_n00b(r"nosuchlevel");

    check_df(n00b_store_index_stats_hot(s.level, s.shard, absent), 0);

    mapped_t mapped = seal_and_map(s.shard);
    check_df(n00b_store_index_stats_mapped(s.level, mapped.root, absent), 0);

    CHECK(n00b_result_is_ok(n00b_store_map_close(mapped.map)));

    n00b_printf("  [PASS] absent term reports zero document frequency");
}

static n00b_plan_predicate_t *
eq_leaf(n00b_string_t *field, n00b_string_t *value)
{
    auto target_r = n00b_plan_target_field(field);
    CHECK(n00b_result_is_ok(target_r));
    auto pred_r
        = n00b_plan_predicate_eq(n00b_result_get(target_r),
                                 n00b_variant_set(n00b_plan_value_t,
                                                  n00b_json_node_t *,
                                                  n00b_json_string_new_from_n00b(value)));
    CHECK(n00b_result_is_ok(pred_r));
    return n00b_result_get(pred_r);
}

static uint64_t
df_ok(n00b_result_t(uint64_t) r)
{
    CHECK(n00b_result_is_ok(r));
    return n00b_result_get(r);
}

// The probe answers the same number the resolved lookup reports, for a single
// term, and answers it without reading a record.
//
// The dense half is the regression. Sealing recomputes a dense count from the
// bitmap, so the sealed assertion holds regardless; the hot one is what fails
// when insert leaves the count behind the bits.
static void
test_df_matches_resolved_lookup(void)
{
    sample_t          s     = sample();
    n00b_json_node_t *error = n00b_json_string_new_from_n00b(r"error");
    n00b_json_node_t *build = n00b_json_string_new_from_n00b(r"build");

#ifdef N00B_DEBUG
    n00b_plan_records_scanned_reset();
#endif

    CHECK(df_ok(n00b_store_index_df_hot(s.level, s.shard, error)) == 3);
    CHECK(df_ok(n00b_store_index_df_hot(s.kind, s.shard, build)) == 3);

    mapped_t mapped = seal_and_map(s.shard);

    CHECK(df_ok(n00b_store_index_df_mapped(s.level, mapped.root, error)) == 3);
    CHECK(df_ok(n00b_store_index_df_mapped(s.kind, mapped.root, build)) == 3);

#ifdef N00B_DEBUG
    // Deciding whether a lookup is worth doing must not cost what the lookup
    // costs. Four probes, no records.
    CHECK(n00b_plan_records_scanned() == 0);
#endif

    CHECK(n00b_result_is_ok(n00b_store_map_close(mapped.map)));

    n00b_printf("  [PASS] df probe matches resolved lookup, reads no records");
}

// A term the shard never indexed bounds at zero, and that is an answer rather
// than a missing statistic: the caller may skip the lookup outright.
static void
test_df_absent_term_is_zero(void)
{
    sample_t          s      = sample();
    n00b_json_node_t *absent = n00b_json_string_new_from_n00b(r"nosuchlevel");

    CHECK(df_ok(n00b_store_index_df_hot(s.level, s.shard, absent)) == 0);

    mapped_t mapped = seal_and_map(s.shard);
    CHECK(df_ok(n00b_store_index_df_mapped(s.level, mapped.root, absent)) == 0);

    CHECK(n00b_result_is_ok(n00b_store_map_close(mapped.map)));

    n00b_printf("  [PASS] df probe bounds an absent term at zero");
}

// A multi-term lookup intersects, so the bound is the smallest term's count.
// "alpha beta" matches one record; "alpha" alone is in four and "beta" in one,
// so the bound is one and happens to be tight here.
static void
test_df_multi_term_bounds_by_smallest(void)
{
    sample_t          s     = sample();
    n00b_json_node_t *alpha = n00b_json_string_new_from_n00b(r"alpha");
    n00b_json_node_t *both  = n00b_json_string_new_from_n00b(r"alpha beta");

    CHECK(df_ok(n00b_store_index_df_hot(s.message, s.shard, alpha)) == 4);
    CHECK(df_ok(n00b_store_index_df_hot(s.message, s.shard, both)) == 1);

    // The bound never understates what the lookup returns.
    auto resolved_r = n00b_store_index_lookup(s.message, s.shard, both);
    CHECK(n00b_result_is_ok(resolved_r));
    auto len_r = n00b_store_postings_len(n00b_result_get(resolved_r));
    CHECK(n00b_result_is_ok(len_r));
    CHECK(n00b_result_get(len_r) <= df_ok(n00b_store_index_df_hot(s.message, s.shard, both)));

    mapped_t mapped = seal_and_map(s.shard);
    CHECK(df_ok(n00b_store_index_df_mapped(s.message, mapped.root, both)) == 1);

    CHECK(n00b_result_is_ok(n00b_store_map_close(mapped.map)));

    n00b_printf("  [PASS] df probe bounds a multi-term lookup by its smallest term");
}

// The catch-all unions across fields, so the sum of the covered fields' counts
// bounds it. One covered field makes that sum exact.
static void
test_df_sums_catch_all_fields(void)
{
    sample_t s = sample();

    n00b_store_index_field_list_t *fields = n00b_alloc(n00b_store_index_field_list_t);
    *fields = n00b_list_new_private(n00b_string_t *, .scan_kind = N00B_GC_SCAN_KIND_ALL);
    n00b_list_push(*fields, r"message");

    auto catch_all_r = n00b_store_index_new_catch_all(fields);
    CHECK(n00b_result_is_ok(catch_all_r));
    n00b_store_index_t *catch_all = n00b_result_get(catch_all_r);

    n00b_json_node_t *alpha = n00b_json_string_new_from_n00b(r"alpha");
    CHECK(df_ok(n00b_store_index_df_hot(catch_all, s.shard, alpha)) == 4);

    mapped_t mapped = seal_and_map(s.shard);
    CHECK(df_ok(n00b_store_index_df_mapped(catch_all, mapped.root, alpha)) == 4);
    CHECK(n00b_result_is_ok(n00b_store_map_close(mapped.map)));

    n00b_printf("  [PASS] df probe sums the catch-all's covered fields");
}

#define SCRAMBLED UINT64_C(2048)

// n00b_store_index_add is public and says nothing about the order its ordinals
// arrive in, but readers binary-search the posting list. Adding out of order
// must therefore still produce a list a search can trust, or membership tests
// answer false for records that are present, and only at the shard sizes that
// select a probe over a walk.
//
// The plan below is the shape that probes: one selective conjunct leaves a
// single candidate, so the broad conjunct asks the index about that candidate
// rather than reading its posting list.
static void
test_out_of_order_adds_still_answer_membership(void)
{
    auto shard_r = n00b_store_shard_new(.shard_id  = UINT64_C(0x50D),
                                        .allocator = test_shard_allocator());
    CHECK(n00b_result_is_ok(shard_r));
    n00b_store_shard_t *shard = n00b_result_get(shard_r);

    n00b_store_index_t *kind  = index_for(r"kind", N00B_STORE_POSTINGS_SPARSE);
    n00b_store_index_t *trace = index_for(r"trace", N00B_STORE_POSTINGS_SPARSE);

    for (uint64_t i = 0; i < SCRAMBLED; i++) {
        n00b_json_node_t *rec = n00b_json_object_new();
        n00b_json_object_put_n00b(rec, r"kind", n00b_json_string_new_from_n00b(r"log"));
        n00b_json_object_put_n00b(
            rec,
            r"trace",
            n00b_json_string_new_from_n00b(n00b_cformat("t-«#»", (int64_t)i)));
        auto a_r = n00b_store_shard_append(shard, rec);
        CHECK(n00b_result_is_ok(a_r));
    }

    // Every odd ordinal first, then every even one, so no posting list sees a
    // monotonically increasing sequence.
    for (uint64_t pass = 0; pass < 2; pass++) {
        for (uint64_t i = pass == 0 ? 1 : 0; i < SCRAMBLED; i += 2) {
            CHECK(n00b_result_is_ok(n00b_store_index_add(kind, shard, i)));
            CHECK(n00b_result_is_ok(n00b_store_index_add(trace, shard, i)));
        }
    }

    n00b_plan_index_list_t *indexes = n00b_plan_index_list_new();
    CHECK(n00b_result_is_ok(n00b_plan_index_list_append(indexes, kind)));
    CHECK(n00b_result_is_ok(n00b_plan_index_list_append(indexes, trace)));

    // Ask about a spread of ordinals, including ones a mis-sorted list would
    // place on the wrong side of a first probe.
    uint64_t probes[] = {0, 1, 2, 1023, 1024, 1025, SCRAMBLED - 1};

    for (uint64_t p = 0; p < sizeof(probes) / sizeof(probes[0]); p++) {
        uint64_t want = probes[p];

        n00b_plan_predicate_list_t *kids = n00b_plan_predicate_list_new();
        CHECK(n00b_result_is_ok(n00b_plan_predicate_list_append(
            kids,
            eq_leaf(r"trace", n00b_cformat("t-«#»", (int64_t)want)))));
        CHECK(n00b_result_is_ok(
            n00b_plan_predicate_list_append(kids, eq_leaf(r"kind", r"log"))));

        auto and_r = n00b_plan_predicate_and(kids);
        CHECK(n00b_result_is_ok(and_r));
        n00b_plan_node_t *plan = test_plan_hot(n00b_result_get(and_r), indexes, shard);

        auto set_r = n00b_plan_exec_hot(plan, shard);
        CHECK(n00b_result_is_ok(set_r));

        auto count_r = n00b_plan_ordset_count(n00b_result_get(set_r));
        CHECK(n00b_result_is_ok(count_r));
        CHECK(n00b_result_get(count_r) == 1);

        auto has_r = n00b_plan_ordset_contains(n00b_result_get(set_r), want);
        CHECK(n00b_result_is_ok(has_r));
        CHECK(n00b_result_get(has_r));
    }

    n00b_printf("  [PASS] out-of-order adds still answer membership");
}

// A sealed image only advertises ascending order after sealing has checked it,
// and readers only search rather than scan when it does. An image that lost
// the claim must still answer correctly, because the alternative is a damaged
// image returning fewer rows, with nothing raised to say so.
// The order bit only means anything on a sparse list: a dense one answers
// membership from its bitmap and rocs_mapped_postings_advertise_order reports
// it ordered without consulting a bit at all. So this uses the sparse index.
// Asserting the bit on the dense one would pass whether or not seal ever set
// it, which is the shape of check that never fails and never helps.
static void
test_sealed_image_answers_without_the_order_bit(void)
{
    sample_t          s     = sample();
    n00b_json_node_t *build = n00b_json_string_new_from_n00b(r"build");

    // "build" is on records 0, 1 and 3.
    uint64_t expected[] = {0, 1, 3};

    mapped_t img = seal_and_map(s.shard);

    // The image has to advertise its order, not merely happen to be ordered.
    // Readers binary-search only when the bit is set, so a seal that stopped
    // setting it would answer correctly and silently scan every posting list.
    CHECK(n00b_store_index_sealed_is_ordered(s.kind, img.root, build));

    auto with_r = n00b_store_index_lookup_mapped(s.kind, img.root, build);
    CHECK(n00b_result_is_ok(with_r));
    check_postings(n00b_result_get(with_r), SEAL_TS, expected, 3);

    auto probe_r = n00b_store_index_probe_mapped(s.kind, img.root, build);
    CHECK(n00b_result_is_ok(probe_r));
    for (uint64_t i = 0; i < RECORDS; i++) {
        auto has_r = n00b_store_index_probe_contains(n00b_result_get(probe_r), i);
        CHECK(n00b_result_is_ok(has_r));
        bool want = i == 0 || i == 1 || i == 3;
        CHECK(n00b_result_get(has_r) == want);
    }

#ifdef N00B_DEBUG
    // Clearing the bit leaves the ordinals ascending but unadvertised, which
    // is what an image sealed before the bit existed looks like to a reader.
    // The fallback must answer identically. Debug only: the mutator that
    // produces this shape does not exist in a release library.
    uint64_t cleared = n00b_store_index_sealed_clear_ordered(s.kind, img.root, build);
    CHECK(cleared > 0);
    CHECK(!n00b_store_index_sealed_is_ordered(s.kind, img.root, build));

    auto without_r = n00b_store_index_lookup_mapped(s.kind, img.root, build);
    CHECK(n00b_result_is_ok(without_r));
    check_postings(n00b_result_get(without_r), SEAL_TS, expected, 3);

    // Membership over the unadvertised image. This is the only path that
    // reaches the linear scan, and a binary search over a list it may not
    // trust would answer false for ordinals that are present.
    auto probe_bare_r = n00b_store_index_probe_mapped(s.kind, img.root, build);
    CHECK(n00b_result_is_ok(probe_bare_r));
    for (uint64_t i = 0; i < RECORDS; i++) {
        auto has_r = n00b_store_index_probe_contains(n00b_result_get(probe_bare_r), i);
        CHECK(n00b_result_is_ok(has_r));
        bool want = i == 0 || i == 1 || i == 3;
        CHECK(n00b_result_get(has_r) == want);
    }
#endif

    CHECK(n00b_result_is_ok(n00b_store_map_close(img.map)));

    n00b_printf("  [PASS] a sealed image answers ordered or unadvertised");
}

// Descending arrival is the worst case for an ordered insert: every ordinal
// belongs at position 0. n00b_store_index_add constrains arrival order in no
// way, so this is a caller's choice, and the answers must not depend on it.
static void
test_descending_adds_answer_the_same(void)
{
    auto shard_r = n00b_store_shard_new(.shard_id  = UINT64_C(0xDE5C),
                                        .allocator = test_shard_allocator());
    CHECK(n00b_result_is_ok(shard_r));
    n00b_store_shard_t *shard = n00b_result_get(shard_r);
    n00b_store_index_t *kind  = index_for(r"kind", N00B_STORE_POSTINGS_SPARSE);

    uint64_t rows = 512;
    for (uint64_t i = 0; i < rows; i++) {
        n00b_json_node_t *rec = n00b_json_object_new();
        n00b_json_object_put_n00b(rec, r"kind", n00b_json_string_new_from_n00b(r"log"));
        CHECK(n00b_result_is_ok(n00b_store_shard_append(shard, rec)));
    }

    // Highest ordinal first, so each insert lands ahead of everything already
    // present.
    for (uint64_t i = rows; i-- > 0;) {
        CHECK(n00b_result_is_ok(n00b_store_index_add(kind, shard, i)));
    }

    // Not check_df: that helper asserts this file's own record count, and this
    // shard is larger.
    n00b_json_node_t *log     = n00b_json_string_new_from_n00b(r"log");
    auto              stats_r = n00b_store_index_stats_hot(kind, shard, log);
    CHECK(n00b_result_is_ok(stats_r));
    CHECK(n00b_result_get(stats_r).document_frequency == rows);
    CHECK(df_ok(n00b_store_index_df_hot(kind, shard, log)) == rows);

    auto probe_r = n00b_store_index_probe_hot(kind, shard, log);
    CHECK(n00b_result_is_ok(probe_r));
    for (uint64_t i = 0; i < rows; i++) {
        auto has_r = n00b_store_index_probe_contains(n00b_result_get(probe_r), i);
        CHECK(n00b_result_is_ok(has_r));
        CHECK(n00b_result_get(has_r));
    }

    n00b_printf("  [PASS] descending adds answer the same");
}

// What the dedup actually compares. Two leaves are the same access when their
// resolved key sets match, not when their predicate objects do, so separately
// built leaves over the same value collapse while different values never do.
//
// A full-text value reduces to its tokens, so writing them in the other order
// is the closest thing to two unrelated predicates naming one set of
// postings. Whether that holds depends on whether normalization imposes an
// order on the tokens, which this pins either way.
static void
test_key_equality_tracks_postings_not_syntax(void)
{
    sample_t s = sample();

    auto a_r = n00b_store_index_keys_new(s.level, n00b_json_string_new_from_n00b(r"error"));
    auto b_r = n00b_store_index_keys_new(s.level, n00b_json_string_new_from_n00b(r"error"));
    auto c_r = n00b_store_index_keys_new(s.level, n00b_json_string_new_from_n00b(r"warn"));
    CHECK(n00b_result_is_ok(a_r) && n00b_result_is_ok(b_r) && n00b_result_is_ok(c_r));

    // Separately resolved, same value: equal, and not by pointer.
    CHECK(n00b_result_get(a_r) != n00b_result_get(b_r));
    CHECK(n00b_store_index_keys_equal(n00b_result_get(a_r), n00b_result_get(b_r)));
    CHECK(!n00b_store_index_keys_equal(n00b_result_get(a_r), n00b_result_get(c_r)));

    // Case is not folded for a term index, so these are different postings and
    // must not be collapsed.
    auto upper_r
        = n00b_store_index_keys_new(s.level, n00b_json_string_new_from_n00b(r"ERROR"));
    CHECK(n00b_result_is_ok(upper_r));
    CHECK(!n00b_store_index_keys_equal(n00b_result_get(a_r), n00b_result_get(upper_r)));

    // Full text reduces a value to its tokens. Same tokens written in the
    // other order: equal only if normalization orders them, and the dedup is
    // correct either way, so record which it is.
    auto fwd_r
        = n00b_store_index_keys_new(s.message, n00b_json_string_new_from_n00b(r"alpha beta"));
    auto rev_r
        = n00b_store_index_keys_new(s.message, n00b_json_string_new_from_n00b(r"beta alpha"));
    CHECK(n00b_result_is_ok(fwd_r) && n00b_result_is_ok(rev_r));
    CHECK(n00b_store_index_keys_count(n00b_result_get(fwd_r))
          == n00b_store_index_keys_count(n00b_result_get(rev_r)));
    // Order is preserved, so a reordered value resolves to a different key
    // sequence and the dedup leaves both leaves in place. Conservative: it
    // costs a duplicate read, never a wrong answer.
    CHECK(!n00b_store_index_keys_equal(n00b_result_get(fwd_r), n00b_result_get(rev_r)));

    n00b_printf("  [PASS] key equality tracks postings, not syntax");
}

// A posting list has one writer and lock-free readers, so it carries no lock,
// and its storage comes from the shard's allocator: growth leaves the old array
// to that pool, which is non-moving and freed with the shard.
static void
test_posting_lists_are_unlocked_in_the_shard_allocator(void)
{
    sample_t s = sample();

    bool                 found  = false;
    n00b_string_t       *field  = r"kind";
    n00b_store_column_t *column = n00b_dict_get(s.shard->columns, field, &found);
    CHECK(found && column != nullptr);

    uint64_t checked = 0;
    n00b_dict_foreach(column, key, postings, {
        (void)key;
        if (postings == nullptr || postings->kind != N00B_STORE_POSTINGS_SPARSE) {
            continue;
        }
        CHECK(postings->ordinals != nullptr);
        CHECK(postings->ordinals->lock == nullptr);
        CHECK(postings->ordinals->allocator == test_shard_allocator());

        n00b_alloc_info_t data_ai
            = n00b_find_alloc_info(postings->ordinals->data, .scan_for_header = true);
        n00b_alloc_info_t list_ai
            = n00b_find_alloc_info(postings->ordinals, .scan_for_header = true);
        CHECK(data_ai.kind == list_ai.kind);
        checked++;
    });

    CHECK(checked > 0);
    n00b_printf("  [PASS] posting lists are unlocked, in the shard allocator");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime = {};
    n00b_init(&runtime, argc, argv);

    test_dense_and_sparse_agree_hot_and_sealed();
    test_absent_term_has_zero_df();
    test_df_matches_resolved_lookup();
    test_df_absent_term_is_zero();
    test_df_multi_term_bounds_by_smallest();
    test_df_sums_catch_all_fields();
    test_out_of_order_adds_still_answer_membership();
    test_sealed_image_answers_without_the_order_bit();
    test_descending_adds_answer_the_same();
    test_key_equality_tracks_postings_not_syntax();
    test_posting_lists_are_unlocked_in_the_shard_allocator();

    n00b_shutdown();
    return 0;
}
