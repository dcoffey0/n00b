// test_gc_type_map_register.c — WP-A: runtime registration side of the
// type->GC-map dictionary. MIR-JIT class layouts are computed at C-runtime and
// cannot live in the link-time n00b_gcmap sections, so they register via
// n00b_gc_type_map_register and must then resolve through n00b_gc_type_map_lookup
// (and drive the alloc.c DEFAULT->CALLBACK precise-scan upgrade) exactly like a
// statically-emitted C-struct layout.

#include <stdint.h>

#include "n00b.h"
#include "core/codegen_abi.h" // GC/marshal codegen-ABI structs used by value
#include "core/codegen_abi_inject.h" // GC/marshal descriptor structs / scan-cb externs used by value
#include "core/alloc.h"
#include "core/arena.h"
#include "core/gc_map.h"
#include "core/runtime.h"
#include "util/assert.h"
#include "test_check.h"

#define ARENA_OPTS(a) &(n00b_alloc_opts_t){.allocator = (n00b_allocator_t *)(a)}

// Synthetic type hashes, chosen to be extremely unlikely to collide with any
// real typehash(T *) emitted into the static n00b_gcmap section.
#define PROBE_HASH_A UINT64_C(0x7E57000000000A01)
#define PROBE_HASH_B UINT64_C(0x7E57000000000B02)

// stride 4 words; words 1 and 3 hold pointers.
static const uint64_t                probe_offsets_a[] = {1, 3};
static const n00b_gc_struct_layout_t probe_layout_a    = {
       .stride        = 4,
       .count         = 0,
       .offset_count  = 2,
       .offsets       = probe_offsets_a,
       .variant_count = 0,
       .variants      = nullptr,
};

static const uint64_t                probe_offsets_b[] = {1};
static const n00b_gc_struct_layout_t probe_layout_b    = {
       .stride        = 2,
       .count         = 0,
       .offset_count  = 1,
       .offsets       = probe_offsets_b,
       .variant_count = 0,
       .variants      = nullptr,
};

// Distinct never-registered layout, used to prove first-registration-wins.
static const n00b_gc_struct_layout_t probe_layout_a_dup = {
    .stride        = 4,
    .count         = 0,
    .offset_count  = 0,
    .offsets       = nullptr,
    .variant_count = 0,
    .variants      = nullptr,
};

static void
test_register_lookup_roundtrip(void)
{
    // Not registered yet (and not a real static type) -> miss.
    CHECK(n00b_gc_type_map_lookup(PROBE_HASH_A) == nullptr);
    CHECK(n00b_gc_type_map_lookup(PROBE_HASH_B) == nullptr);

    n00b_gc_type_map_register(PROBE_HASH_A, &probe_layout_a);
    n00b_gc_type_map_register(PROBE_HASH_B, &probe_layout_b);

    // Forward lookup returns exactly the registered descriptor.
    CHECK(n00b_gc_type_map_lookup(PROBE_HASH_A) == &probe_layout_a);
    CHECK(n00b_gc_type_map_lookup(PROBE_HASH_B) == &probe_layout_b);

    // Reverse lookup.
    CHECK(n00b_gc_type_map_hash_for_layout(&probe_layout_a) == PROBE_HASH_A);
    CHECK(n00b_gc_type_map_hash_for_layout(&probe_layout_b) == PROBE_HASH_B);

    // Registration is idempotent on type_hash: first registration wins.
    n00b_gc_type_map_register(PROBE_HASH_A, &probe_layout_a_dup);
    CHECK(n00b_gc_type_map_lookup(PROBE_HASH_A) == &probe_layout_a);

    // Defensive: zero hash / null layout are no-ops.
    n00b_gc_type_map_register(0, &probe_layout_a);
    n00b_gc_type_map_register(PROBE_HASH_A, nullptr);
    CHECK(n00b_gc_type_map_lookup(0) == nullptr);
}

static void
test_registered_layout_upgrades_alloc(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 1024 * 1024, .use_gc = true);

    // A typed DEFAULT-scanned allocation whose type_hash was registered at
    // runtime must be upgraded to a precise CALLBACK scan driven by the
    // registered layout -- just like a statically-emitted layout.
    void *p = n00b_alloc_size_typed_with_opts(1,
                                              probe_layout_a.stride * sizeof(void *),
                                              PROBE_HASH_A,
                                              ARENA_OPTS(arena));
    CHECK(p != nullptr);

    bool              pin  = false;
    n00b_alloc_info_t info = n00b_find_alloc_info(p,
                                                  .scan_for_header = true,
                                                  .pin             = &pin);
    CHECK(info.kind == n00b_alloc_oob);
    CHECK(info.hdr.oob->tinfo == PROBE_HASH_A);
    CHECK(info.hdr.oob->scan_kind == N00B_GC_SCAN_KIND_CALLBACK);
    CHECK(info.hdr.oob->scan_cb == n00b_gc_scan_cb_type_layout);
    CHECK(info.hdr.oob->scan_user == (void *)&probe_layout_a);
    n00b_alloc_hdr_unpin(pin);
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    test_register_lookup_roundtrip();
    test_registered_layout_upgrades_alloc();

    n00b_shutdown();
    return 0;
}
