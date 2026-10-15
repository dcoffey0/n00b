#include <stdio.h>
#include <assert.h>

#include "n00b.h"
#include "core/codegen_abi.h"        // GC/marshal codegen-ABI structs used by value
#include "core/codegen_abi_inject.h" // scan-cb externs (n00b_gc_scan_cb_type_layout)
#include "core/alloc.h"
#include "core/arena.h"
#include "core/gc.h"
#include "core/gc_map.h"
#include "core/runtime.h"
#include "core/stw.h"
#include "adt/result.h"

#define RESULT_ARENA_OPTS(a) &(n00b_alloc_opts_t){.allocator = (n00b_allocator_t *)(a)}
#define RESULT_WORDS(T)      ((sizeof(T) + sizeof(void *) - 1) / sizeof(void *))
#define RESULT_BITMAP_WORDS(T) (((RESULT_WORDS(T)) + 63) >> 6)

typedef struct {
    uint64_t value;
    void    *next;
} result_payload_t;

typedef struct {
    uint64_t value;
} result_other_payload_t;

typedef struct {
    n00b_result_t(int) result;
} result_payload_holder_t;

// ============================================================================
// 1. Construction
// ============================================================================

static void
test_construction(void)
{
    n00b_result_t(int) ok = n00b_result_ok(int, 42);
    assert(n00b_result_is_ok(ok));
    assert(!n00b_result_is_err(ok));

    n00b_result_t(int) err = n00b_result_err(int, -1);
    assert(n00b_result_is_err(err));
    assert(!n00b_result_is_ok(err));

    printf("  [PASS] construction\n");
}

// ============================================================================
// 2. Get value
// ============================================================================

static void
test_get(void)
{
    n00b_result_t(int) ok = n00b_result_ok(int, 99);
    assert(n00b_result_get(ok) == 99);

    n00b_result_t(int) err = n00b_result_err(int, 5);
    assert(n00b_result_get_err(err) == 5);

    printf("  [PASS] get\n");
}

// ============================================================================
// 3. Get or else
// ============================================================================

static void
test_get_or_else(void)
{
    int fallback = -999;

    n00b_result_t(int) ok = n00b_result_ok(int, 42);
    assert(n00b_result_get_or_else(ok, fallback) == 42);

    n00b_result_t(int) err = n00b_result_err(int, 1);
    assert(n00b_result_get_or_else(err, fallback) == -999);

    printf("  [PASS] get_or_else\n");
}

// ============================================================================
// 4. Match
// ============================================================================

static void
test_match(void)
{
    n00b_result_t(int) ok = n00b_result_ok(int, 10);
    int val               = n00b_result_match(ok, 1, 0);
    assert(val == 1);

    n00b_result_t(int) err = n00b_result_err(int, 3);
    val                     = n00b_result_match(err, 1, 0);
    assert(val == 0);

    printf("  [PASS] match\n");
}

// ============================================================================
// 5. Pointer type
// ============================================================================

static void
test_pointer_type(void)
{
    int  value = 7;
    int *ptr   = &value;

    n00b_result_t(int *) ok = n00b_result_ok(int *, ptr);
    assert(n00b_result_is_ok(ok));
    assert(n00b_result_get(ok) == ptr);

    n00b_result_t(int *) err = n00b_result_err(int *, 42);
    assert(n00b_result_is_err(err));
    assert(n00b_result_get_err(err) == 42);

    printf("  [PASS] pointer type\n");
}

// ============================================================================
// 6. Postfix ! operator (auto-unwrap / early return)
// ============================================================================

static n00b_result_t(int)
returns_ok(void)
{
    return n00b_result_ok(int, 10);
}

static n00b_result_t(int)
returns_err(void)
{
    return n00b_result_err(int, -1);
}

static n00b_result_t(int)
returns_carrier_err(void)
{
    n00b_result_error_t carrier = {
        .kind         = N00B_RESULT_ERROR_CODE,
        .code         = -33,
        .payload_type = 0,
        .payload      = nullptr,
    };

    return n00b_result_err(int, carrier);
}

static uint64_t payload_sentinel = 0xfeedfacecafebeefull;

static n00b_result_error_t
payload_carrier(void)
{
    return (n00b_result_error_t){
        .kind         = N00B_RESULT_ERROR_PAYLOAD,
        .code         = -501,
        .payload_type = 0x0c0010c0010c0010ull,
        .payload      = &payload_sentinel,
    };
}

static void
assert_payload_carrier(n00b_result_error_t carrier)
{
    assert(carrier.kind == N00B_RESULT_ERROR_PAYLOAD);
    assert(carrier.code == -501);
    assert(carrier.payload_type == 0x0c0010c0010c0010ull);
    assert(carrier.payload == &payload_sentinel);
}

static n00b_result_t(int)
returns_payload_err(void)
{
    return n00b_result_err(int, payload_carrier());
}

static n00b_result_t(int)
returns_typed_payload_err(void)
{
    return n00b_result_err_payload(int, uint64_t *, &payload_sentinel);
}

static n00b_result_t(int)
chain_ok(void)
{
    int x = returns_ok()!;
    return n00b_result_ok(int, x + 1);
}

static n00b_result_t(int)
chain_err(void)
{
    int x = returns_err()!; // should early-return with the error
    (void)x;
    return n00b_result_ok(int, 999); // should not be reached
}

static n00b_result_t(int)
chain_carrier_err(void)
{
    int x = returns_carrier_err()!;
    (void)x;
    return n00b_result_ok(int, 999); // should not be reached
}

static n00b_result_t(uint64_t)
chain_carrier_err_to_uint64(void)
{
    int x = returns_carrier_err()!;
    (void)x;
    return n00b_result_ok(uint64_t, 999); // should not be reached
}

static n00b_result_t(uint64_t)
chain_payload_err_to_uint64(void)
{
    int x = returns_payload_err()!;
    (void)x;
    return n00b_result_ok(uint64_t, 999); // should not be reached
}

static n00b_result_t(uint64_t)
chain_typed_payload_err_to_uint64(void)
{
    int x = returns_typed_payload_err()!;
    (void)x;
    return n00b_result_ok(uint64_t, 999); // should not be reached
}

static void
test_bang_operator(void)
{
    n00b_result_t(int) r1 = chain_ok();
    assert(n00b_result_is_ok(r1));
    assert(n00b_result_get(r1) == 11);

    n00b_result_t(int) r2 = chain_err();
    assert(n00b_result_is_err(r2));
    assert(n00b_result_get_err(r2) == -1);

    n00b_result_t(int) r3 = chain_carrier_err();
    assert(n00b_result_is_err(r3));
    assert(n00b_result_get_err(r3) == -33);

    n00b_result_t(uint64_t) r4 = chain_carrier_err_to_uint64();
    assert(n00b_result_is_err(r4));
    assert(n00b_result_get_err(r4) == -33);

    n00b_result_t(uint64_t) r5 = chain_payload_err_to_uint64();
    assert(n00b_result_is_err(r5));
    assert_payload_carrier(r5.err);

    n00b_result_t(uint64_t) r6 = chain_typed_payload_err_to_uint64();
    assert(n00b_result_is_err_payload(uint64_t *, r6));
    assert(n00b_result_get_err_payload(uint64_t *, r6) == &payload_sentinel);

    printf("  [PASS] postfix ! operator\n");
}

// ============================================================================
// 7. Raw carrier compatibility
// ============================================================================

static void
test_raw_carrier_copy(void)
{
    n00b_result_t(int) source = n00b_result_err(int, -77);

    n00b_result_t(uint64_t) copied = n00b_result_err(uint64_t, source.err);
    assert(n00b_result_is_err(copied));
    assert(n00b_result_get_err(copied) == -77);

    n00b_result_error_t carrier = {
        .kind         = N00B_RESULT_ERROR_CODE,
        .code         = 88,
        .payload_type = 0,
        .payload      = nullptr,
    };

    n00b_result_t(int *) raw = n00b_result_err(int *, carrier);
    assert(n00b_result_is_err(raw));
    assert(n00b_result_get_err(raw) == 88);

    n00b_result_t(int) payload_source = n00b_result_err(int, payload_carrier());

    n00b_result_t(uint64_t) payload_copied = n00b_result_err(uint64_t, payload_source.err);
    assert(n00b_result_is_err(payload_copied));
    assert_payload_carrier(payload_copied.err);
}

// ============================================================================
// 8. Structured payload helpers
// ============================================================================

static void
test_payload_helpers(void)
{
    result_payload_t payload = {
        .value = 0x123456789abcdef0ull,
        .next  = nullptr,
    };

    n00b_result_t(uint64_t) r =
        n00b_result_err_payload(uint64_t, result_payload_t *, &payload);

    assert(n00b_result_is_err(r));
    assert(n00b_result_is_err_payload(result_payload_t *, r));
    assert(!n00b_result_is_err_payload(result_other_payload_t *, r));

    result_payload_t *got = n00b_result_get_err_payload(result_payload_t *, r);
    assert(got == &payload);
    assert(got->value == 0x123456789abcdef0ull);

    n00b_result_error_t carrier = n00b_result_get_error(r);
    assert(carrier.kind == N00B_RESULT_ERROR_PAYLOAD);
    assert(carrier.code == 0);
    assert(carrier.payload_type == typehash(result_payload_t *));
    assert(carrier.payload == &payload);

    n00b_result_t(uint64_t) code_err = n00b_result_err(uint64_t, -44);
    assert(n00b_result_get_err(code_err) == -44);
    assert(!n00b_result_is_err_payload(result_payload_t *, code_err));
    carrier = n00b_result_get_error(code_err);
    assert(carrier.kind == N00B_RESULT_ERROR_CODE);
    assert(carrier.payload == nullptr);
}

[[gnu::noinline]] static void
install_gc_payload(result_payload_holder_t *holder, n00b_arena_t *arena)
{
    result_payload_t *payload =
        n00b_alloc_with_opts(result_payload_t, RESULT_ARENA_OPTS(arena));
    payload->value = 0x0badc0ffee0ddf00ull;
    payload->next  = nullptr;

    holder->result = n00b_result_err_payload(int, result_payload_t *, payload);
}

static void
assert_payload_holder_scan_layout(result_payload_holder_t *holder)
{
    bool              pin  = false;
    n00b_alloc_info_t info = n00b_find_alloc_info(holder, .pin = &pin);
    assert(info.kind == n00b_alloc_oob || info.kind == n00b_alloc_inline);

    n00b_gc_scan_kind_t scan_kind;
    n00b_gc_scan_cb_t   scan_cb;
    void               *scan_user;

    if (info.kind == n00b_alloc_oob) {
        scan_kind = info.hdr.oob->scan_kind;
        scan_cb   = info.hdr.oob->scan_cb;
        scan_user = info.hdr.oob->scan_user;
    }
    else {
        scan_kind = info.hdr.in_line->scan_kind;
        scan_cb   = info.hdr.in_line->scan_cb;
        scan_user = info.hdr.in_line->scan_user;
    }
    n00b_alloc_hdr_unpin(pin);

    const uint64_t holder_words = RESULT_WORDS(result_payload_holder_t);
    const size_t   payload_offset =
        (size_t)((char *)&holder->result.err.payload - (char *)holder);
    const size_t payload_type_offset =
        (size_t)((char *)&holder->result.err.payload_type - (char *)holder);

    assert(payload_offset % sizeof(void *) == 0);
    assert(payload_type_offset % sizeof(void *) == 0);

    const uint64_t payload_word      = payload_offset / sizeof(void *);
    const uint64_t payload_type_word = payload_type_offset / sizeof(void *);

    assert(scan_kind == N00B_GC_SCAN_KIND_CALLBACK);
    assert(scan_cb == n00b_gc_scan_cb_type_layout);
    assert(scan_user != nullptr);

    const n00b_gc_struct_layout_t *layout = scan_user;
    assert(layout->stride == holder_words);
    assert(layout->offset_count == 1);
    assert(layout->offsets[0] == payload_word);

    uint64_t      bits[RESULT_BITMAP_WORDS(result_payload_holder_t)] = {};
    n00b_gc_map_t map = {
        .user_ptr  = holder,
        .num_words = holder_words,
        .bitmap    = bits,
    };

    scan_cb(&map, scan_user);
    for (uint64_t i = 0; i < holder_words; i++) {
        assert(n00b_gc_map_is_set(&map, i) == (i == payload_word));
    }
    assert(!n00b_gc_map_is_set(&map, payload_type_word));
}

static void
test_payload_gc_visibility(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);

    result_payload_holder_t probe = {};
    const size_t            payload_offset =
        (size_t)((char *)&probe.result.err.payload - (char *)&probe);

    assert(sizeof(result_payload_holder_t) % sizeof(void *) == 0);
    assert(payload_offset % sizeof(void *) == 0);

    uint64_t               payload_offsets[] = {payload_offset / sizeof(void *)};
    n00b_gc_struct_layout_t holder_layout     = {
        .stride       = sizeof(result_payload_holder_t) / sizeof(void *),
        .count        = 0,
        .offset_count = 1,
        .offsets      = payload_offsets,
    };
    n00b_alloc_opts_t holder_opts = {
        .allocator = (n00b_allocator_t *)arena,
        .scan_kind = N00B_GC_SCAN_KIND_CALLBACK,
        .scan_cb   = n00b_gc_scan_cb_type_layout,
        .scan_user = &holder_layout,
    };

    result_payload_holder_t *holder =
        n00b_alloc_with_opts(result_payload_holder_t, &holder_opts);
    holder->result = n00b_result_err(int, -1);

    n00b_gc_register_root(holder);
    install_gc_payload(holder, arena);
    assert_payload_holder_scan_layout(holder);

    n00b_stop_the_world();
    n00b_collect(arena);
    n00b_restart_the_world();

    assert(n00b_result_is_err_payload(result_payload_t *, holder->result));
    result_payload_t *payload =
        n00b_result_get_err_payload(result_payload_t *, holder->result);
    assert(payload != nullptr);
    assert(payload->value == 0x0badc0ffee0ddf00ull);
    assert(payload->next == nullptr);

    n00b_gc_unregister_root(holder);
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running result tests...\n");

    test_construction();
    test_get();
    test_get_or_else();
    test_match();
    test_pointer_type();
    test_bang_operator();
    test_raw_carrier_copy();
    test_payload_helpers();
    test_payload_gc_visibility();

    printf("All result tests passed.\n");
    n00b_shutdown();
    return 0;
}
