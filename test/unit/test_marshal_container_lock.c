/*
 * n00b#558: a container's lock never enters a marshal image.
 *
 * A locked container holds a pointer to its own n00b_rwlock_t. The marshaller
 * used to copy that pointer raw, because the GC type map left `lock` out of
 * the container's layout, so the word was treated as a scalar. An unmarshalled
 * copy then shared the original's lock in this process, pointed at nothing in
 * another, and carried a per-run address into the image's content hash.
 *
 * Once `lock` is in the layout (crashappsec/ncc#66, for n00b#557) the
 * marshaller follows it, and a lock must not be serialized either: it is
 * process state, and copying it would copy whoever holds it. So a pointer to
 * a lock marshals as null, and an unmarshalled container comes back unlocked.
 *
 * This checks the round trip and that two payloads equal in everything but
 * their locks marshal to the same bytes.
 */

#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/buffer.h"
#include "core/runtime.h"
#include "util/marshal.h"

#define ITEMS 4

typedef struct {
    uint64_t value;
} payload_item_t;

typedef struct {
    n00b_list_t(payload_item_t *) *items;
    uint64_t                       tag;
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

static holder_t *
make_holder(void)
{
    holder_t *h = n00b_alloc(holder_t);
    h->items    = n00b_alloc(n00b_list_t(payload_item_t *));
    *h->items   = n00b_list_new(payload_item_t *);
    h->tag      = 0x558;
    for (uint64_t i = 0; i < ITEMS; i++) {
        payload_item_t *it = n00b_alloc(payload_item_t);
        it->value          = 1000 + i;
        n00b_list_push(*h->items, it);
    }
    return h;
}

static bool
items_intact(n00b_list_t(payload_item_t *) *l)
{
    if (n00b_list_len(*l) != ITEMS) {
        return false;
    }
    for (uint64_t i = 0; i < ITEMS; i++) {
        payload_item_t *it = n00b_list_get(*l, i);
        if (it == nullptr || it->value != 1000 + i) {
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

    printf("n00b#558: a container's lock stays out of the marshal image\n");

    holder_t *h = make_holder();
    check(h->items->lock != nullptr, "the original list is locked");

    n00b_buffer_t *image = n00b_marshal(h);
    check(image != nullptr, "a holder of a locked list marshals");

    if (image != nullptr) {
        n00b_list_t(void *) roots = n00b_unmarshal(image);
        holder_t *back = n00b_list_len(roots) > 0 ? n00b_list_get(roots, 0) : nullptr;
        check(back != nullptr && back->tag == 0x558, "the holder round-trips");
        if (back != nullptr) {
            check(back->items->lock != h->items->lock,
                  "the copy does not share the original's lock");
            check(items_intact(back->items), "the copy's items round-trip");

            payload_item_t *extra = n00b_alloc(payload_item_t);
            extra->value          = 1000 + ITEMS;
            n00b_list_push(*back->items, extra);
            check(n00b_list_len(*back->items) == ITEMS + 1,
                  "the copy can be pushed to");
            check(n00b_list_len(*h->items) == ITEMS,
                  "the original is unchanged and still locks");
        }

        // Equal payloads with different locks: the image must not depend on
        // where a lock happens to live.
        holder_t      *h2     = make_holder();
        n00b_buffer_t *image2 = n00b_marshal(h2);
        check(image2 != nullptr
                  && image2->byte_len == image->byte_len
                  && memcmp(image2->data, image->data, (size_t)image->byte_len) == 0,
              "equal payloads marshal to the same bytes");
    }

    if (failures) {
        printf("\nn00b#558: %d check(s) failed\n", failures);
        fflush(stdout);
        n00b_shutdown();
        return 1;
    }
    printf("\nn00b#558: locks stay out of marshal images.\n");
    fflush(stdout);
    n00b_shutdown();
    return 0;
}
