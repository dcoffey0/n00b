/*
 * test_early_topic_locks.c - Topics the runtime builds during init, such as
 * stdout's and its done topic, lock their subscription lists like any other.
 *
 * The case mirrors the two sides that meet when several threads print:
 *   - a sync write whose publisher claim fails: it subscribes one-shot to the
 *     topic's done topic, then cancels, destroys, and frees its inbox;
 *   - the stdout sink's done delivery to that same done topic.
 * A hook pauses the delivery after it has read the subscription as active.
 * The cancel must then wait for the delivery to leave the list. Without the
 * list's lock it returns at once, the inbox is freed, and the delivery
 * pushes into it.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "conduit/conduit.h"
#include "conduit/rw.h"
#include "core/futex.h"
#include "core/platform.h"
#include "core/runtime.h"
#include "core/thread.h"
#include "core/time.h"

#define HANG_GUARD_MS 30000

typedef n00b_conduit_topic_t(n00b_conduit_topic_base_t *) done_topic_t;
typedef n00b_conduit_inbox_t(n00b_conduit_topic_base_t *) done_inbox_t;

static n00b_futex_t paused      = 0;
static n00b_futex_t go          = 0;
static n00b_futex_t cancel_done = 0;

static done_topic_t             *target;
static done_inbox_t             *inbox;
static n00b_conduit_sub_handle_t handle;
static _Atomic(n00b_thread_t *)  canceler_self;

static bool
await_count(n00b_futex_t *c, uint32_t want, int64_t guard_ms)
{
    int64_t  deadline = n00b_ns_timestamp() + guard_ms * N00B_NS_PER_MS;
    uint32_t cur;
    while ((cur = n00b_atomic_load(c)) < want) {
        int64_t left = deadline - n00b_ns_timestamp();
        if (left <= 0) {
            return false;
        }
        (void)n00b_futex_wait(c, cur, (uint64_t)left);
    }
    return true;
}

static void
bump(n00b_futex_t *c)
{
    n00b_atomic_add(c, 1);
    n00b_futex_wake(c, true);
}

static void
pause_delivery(n00b_conduit_topic_base_t *topic, void *sub)
{
    (void)sub;
    if ((void *)topic == (void *)target && n00b_atomic_load(&paused) == 0) {
        bump(&paused);
        assert(await_count(&go, 1, HANG_GUARD_MS));
    }
}

// The sink's done delivery, as the transform loop sends it.
static void *
deliverer(void *arg)
{
    (void)arg;
    n00b_conduit_topic_base_t *base = (n00b_conduit_topic_base_t *)target;
    n00b_conduit_message_t(n00b_conduit_topic_base_t *) *m = n00b_alloc_with_opts(
        n00b_conduit_message_t(n00b_conduit_topic_base_t *),
        &(n00b_alloc_opts_t){.allocator = base->conduit->allocator});
    m->header.type  = N00B_CONDUIT_MSG_USER;
    m->header.topic = base;
    m->payload      = base;
    n00b_conduit_topic_deliver_msg(n00b_conduit_topic_base_t *, target, m,
                                   N00B_CONDUIT_OP_ALL);
    return nullptr;
}

// The claim-fail branch of a sync write.
static void *
canceler(void *arg)
{
    (void)arg;
    n00b_atomic_store(&canceler_self, n00b_thread_self());
    n00b_conduit_sub_cancel(handle);
    n00b_conduit_inbox_destroy(n00b_conduit_topic_base_t *, inbox);
    n00b_free(inbox);
    bump(&cancel_done);
    return nullptr;
}

// True once the canceler is blocked on the list's lock, false once it has
// finished without waiting.
static bool
canceler_waits_for_delivery(void)
{
    int64_t deadline = n00b_ns_timestamp()
                     + (int64_t)HANG_GUARD_MS * N00B_NS_PER_MS;
    while (n00b_ns_timestamp() < deadline) {
        if (n00b_atomic_load(&cancel_done) != 0) {
            return false;
        }
        n00b_thread_t *t = n00b_atomic_load(&canceler_self);
        if (t != nullptr && t->record != nullptr
            && t->record->lock_wait_target != nullptr
            && t->record->lock_wait_target == (void *)target->subscriptions.lock) {
            return true;
        }
        base_nanosleep_ns(100ULL * 1000);
    }
    assert(false);
}

static void
test_cancel_waits_for_a_done_delivery(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    target = (done_topic_t *)n00b_atomic_load(&rt->stdout_topic->done_topic);
    assert(target != nullptr);

    inbox = n00b_alloc_with_opts(
        done_inbox_t,
        &(n00b_alloc_opts_t){.allocator =
                                 (n00b_allocator_t *)&rt->conduit_pool});
    n00b_conduit_inbox_init(n00b_conduit_topic_base_t *, inbox,
                            rt->default_conduit, N00B_CONDUIT_BP_DROP_NEWEST, 1);
    handle = n00b_conduit_subscribe(n00b_conduit_topic_base_t *, target, inbox,
                                    .flags = N00B_CONDUIT_SUB_F_ONE_SHOT);
    assert(handle != N00B_CONDUIT_INVALID_SUB_HANDLE);

    n00b_conduit_topic_deliver_hook = pause_delivery;
    auto d = n00b_thread_spawn(deliverer, nullptr);
    assert(n00b_result_is_ok(d));
    assert(await_count(&paused, 1, HANG_GUARD_MS));

    auto c = n00b_thread_spawn(canceler, nullptr);
    assert(n00b_result_is_ok(c));
    bool waited = canceler_waits_for_delivery();

    // Without the lock the inbox is already freed, and this push into it
    // fails on its condition variable.
    bump(&go);
    n00b_thread_join(n00b_result_get(d));
    n00b_thread_join(n00b_result_get(c));
    n00b_conduit_topic_deliver_hook = nullptr;

    if (!waited) {
        printf("  [FAIL] a cancel on stdout's done topic did not wait for the"
               " delivery inside its list (list lock %p)\n",
               (void *)target->subscriptions.lock);
        assert(false);
    }
    printf("  [PASS] a cancel on stdout's done topic waits for its delivery\n");
}

int
main(int argc, char *argv[])
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_early_topic_locks:\n");
    fflush(stdout);
    test_cancel_waits_for_a_done_delivery();
    fflush(stdout);

    printf("All early_topic_locks tests passed.\n");
    n00b_shutdown();
    return 0;
}
