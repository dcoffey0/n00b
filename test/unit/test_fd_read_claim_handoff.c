/*
 * A read pass that cannot claim the read topic must not lose the bytes it was
 * woken for.
 *
 * Only one read pass at a time holds an fd owner's read-topic claim. A pass
 * that cannot claim it leaves the reading to the one that has it. kqueue arms
 * data fds with EV_CLEAR, so readiness is reported once per arrival: if bytes
 * land after the holder's last read() and the losing pass is the one woken
 * for them, nothing wakes for them again. They stay in the socket, and a
 * reader waiting on the topic waits forever.
 *
 * conduit_local_conformance_e2e met this on macOS CI as a 30 s read stall
 * (bridge_pool_churn iteration 19). The holder there is the subscriber's own
 * first pass: subscribing to the read topic runs a pass on the subscribing
 * thread, and the peer's bytes arrive while that pass still holds the claim.
 *
 * This makes that interleaving deterministic. The hook runs at the end of the
 * subscriber's pass, after its last read() and before it gives up the claim:
 * it writes the bytes, then waits for the I/O thread's pass to be woken for
 * them and fail to claim (the read topic's epoch moves). Then the claim is
 * released, and the bytes must reach the subscriber.
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdatomic.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include "n00b.h"
#include "core/runtime.h"
#include "core/alloc.h"
#include "core/buffer.h"
#include "core/platform.h" // base_nanosleep_ns
#include "conduit/conduit.h"
#include "conduit/io.h"
#include "conduit/fd_managed.h"

#define PAYLOAD "read-claim-handoff"

static n00b_conduit_fd_owner_t *g_target   = nullptr;
static int                      g_write_fd = -1;
static _Atomic(int)             g_fired    = 0;
static _Atomic(int)             g_saw_loser = 0;

static int
test_pipe_create(int fds[2])
{
#ifdef _WIN32
    return _pipe(fds, 64 * 1024, _O_BINARY);
#else
    return pipe(fds);
#endif
}

static int
test_fd_close(int fd)
{
#ifdef _WIN32
    return _close(fd);
#else
    return close(fd);
#endif
}

static long
test_fd_write(int fd, const void *buf, size_t len)
{
#ifdef _WIN32
    return (long)_write(fd, buf, (unsigned int)len);
#else
    return (long)write(fd, buf, len);
#endif
}

static void
pass_end_hook(n00b_conduit_fd_owner_t *owner)
{
    if (owner != g_target) {
        return;
    }
    int armed = 0;
    if (!atomic_compare_exchange_strong(&g_fired, &armed, 1)) {
        return;
    }

    // This pass has read everything there was and still holds the claim.
    uint64_t epoch = n00b_atomic_load(&owner->read_topic->epoch);
    long     n     = test_fd_write(g_write_fd, PAYLOAD, strlen(PAYLOAD));
    assert(n == (long)strlen(PAYLOAD));

    // Wait for the I/O thread's pass: woken for these bytes, it finds the
    // claim taken and records that in the epoch.
    for (int ms = 0; ms < 5000; ms++) {
        if (n00b_atomic_load(&owner->read_topic->epoch) != epoch) {
            atomic_store(&g_saw_loser, 1);
            return;
        }
        base_nanosleep_ns(1000ULL * 1000);
    }
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("fd_read_claim_handoff: a pass that loses the read claim keeps its bytes\n");

    n00b_conduit_t *c = n00b_get_runtime()->default_conduit;
    assert(c != nullptr);
    auto io_opt = n00b_conduit_default_backend(c);
    assert(n00b_option_is_set(io_opt));

    int fds[2];
    assert(test_pipe_create(fds) == 0);
    g_write_fd = fds[1];

    auto manage_r = n00b_conduit_fd_manage(c, n00b_option_get(io_opt), fds[0], true);
    assert(n00b_result_is_ok(manage_r));
    n00b_conduit_fd_owner_t *owner = n00b_result_get(manage_r);

    g_target                          = owner;
    n00b_fd_owner_read_pass_end_hook  = pass_end_hook;

    n00b_conduit_inbox_t(n00b_buffer_t *) *inbox = n00b_alloc_with_opts(
        n00b_conduit_inbox_t(n00b_buffer_t *),
        &(n00b_alloc_opts_t){
            .allocator = (n00b_allocator_t *)&n00b_get_runtime()->conduit_pool,
        });
    n00b_conduit_inbox_init(n00b_buffer_t *, inbox, c,
                            N00B_CONDUIT_BP_UNBOUNDED, 0);

    // Subscribing runs the first read pass on this thread; the hook fires at
    // its end.
    n00b_conduit_sub_handle_t sub = n00b_conduit_subscribe(
        n00b_buffer_t *,
        (n00b_conduit_topic_t(n00b_buffer_t *) *)owner->read_topic,
        inbox,
        .operations = N00B_CONDUIT_FD_OP_READ_DATA);
    assert(sub != N00B_CONDUIT_INVALID_SUB_HANDLE);

    bool got = false;
    for (int ms = 0; ms < 3000 && !got; ms++) {
        got = n00b_conduit_inbox_has_msg(n00b_buffer_t *, inbox);
        if (!got) {
            base_nanosleep_ns(1000ULL * 1000);
        }
    }

    n00b_fd_owner_read_pass_end_hook = nullptr;

    int failures = 0;
    if (!atomic_load(&g_fired)) {
        printf("  [FAIL] the subscriber's read pass never reached its end\n");
        failures++;
    }
#ifndef _WIN32
    // On kqueue and epoll the I/O thread is woken for the bytes while the
    // claim is held. If that stops happening the test no longer exercises
    // anything, and should say so rather than pass.
    if (!atomic_load(&g_saw_loser)) {
        printf("  [FAIL] no read pass lost the claim; the race was not set up\n");
        failures++;
    }
#endif
    if (!got) {
        printf("  [FAIL] the bytes never reached the subscriber "
               "(loser seen: %d)\n", atomic_load(&g_saw_loser));
        failures++;
    }
    else {
        auto m = n00b_conduit_inbox_pop_msg(n00b_buffer_t *, inbox);
        assert(m != nullptr);
        int64_t len  = 0;
        char   *data = n00b_buffer_to_c(m->payload, &len);
        if (len != (int64_t)strlen(PAYLOAD) || memcmp(data, PAYLOAD, (size_t)len) != 0) {
            printf("  [FAIL] got %lld bytes, not the payload\n", (long long)len);
            failures++;
        }
    }

    n00b_conduit_sub_cancel(sub);
    test_fd_close(fds[1]);
    n00b_conduit_fd_owner_close(owner);

    if (failures) {
        fflush(stdout);
        n00b_shutdown();
        return 1;
    }
    printf("  [PASS] the bytes a losing read pass was woken for still arrive\n");
    fflush(stdout);
    n00b_shutdown();
    return 0;
}
