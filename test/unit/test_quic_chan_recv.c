/*
 * test_quic_chan_recv.c — Synthetic-event tests for the recv path.
 *
 * We can't drive a real QUIC handshake yet (no server cert), so the
 * tests here invoke the conn-level callback's helpers directly to
 * verify dispatch, recv-buffer accumulation, and state transitions.
 * Once cert wiring lands, a second test file exercises the same
 * surface end-to-end over the wire.
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/string.h"
#include "core/data_lock.h"
#include "conduit/conduit.h"
#include "conduit/io.h"
#include "net/quic/quic_types.h"
#include "net/quic/endpoint.h"
#include "net/quic/conn.h"
#include "net/quic/chan.h"
#include "internal/net/quic/conn_internal.h"
#include "internal/net/quic/chan_internal.h"
#include "internal/net/quic/endpoint_internal.h"
#include "core/platform.h"
#include "picoquic.h"

/* Fixture: a connecting conn (no live handshake) + a chan opened on it. */
typedef struct {
    n00b_conduit_t            *c;
    n00b_conduit_io_backend_t *io;
    n00b_quic_endpoint_t      *ep;
    n00b_quic_conn_t          *conn;
    n00b_quic_chan_t          *chan;
} fixture_t;

static bool
fixture_setup(fixture_t *f)
{
    auto cr = n00b_conduit_new();
    if (n00b_result_is_err(cr)) return false;
    f->c = n00b_result_get(cr);
    auto ir = n00b_conduit_io_new_default(f->c);
    if (n00b_result_is_err(ir)) return false;
    f->io = n00b_result_get(ir);
    auto er = n00b_quic_endpoint_new(f->c, f->io,
                                     .bind_host = "127.0.0.1",
                                     .alpn      = "n00b-echo/1");
    if (n00b_result_is_err(er)) return false;
    f->ep = n00b_result_get(er);

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(54321);
    inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);

    auto rr = n00b_quic_connect(f->ep, (const struct sockaddr *)&dst,
                                n00b_string_from_cstr("test.invalid"));
    if (n00b_result_is_err(rr)) return false;
    f->conn = n00b_result_get(rr);

    auto chanr = n00b_quic_chan_open(f->conn);
    if (n00b_result_is_err(chanr)) return false;
    f->chan = n00b_result_get(chanr);
    return true;
}

static void
fixture_teardown(fixture_t *f)
{
    if (f->chan) n00b_quic_chan_close(f->chan);
    if (f->conn) n00b_quic_close(f->conn, 0);
    if (f->ep)   n00b_quic_endpoint_close(f->ep);
    if (f->io)   n00b_conduit_io_destroy(f->io);
    if (f->c)    n00b_conduit_destroy(f->c);
}

/* ============================================================================
 * 1. Channels register on conn — find_chan locates them by stream_id
 * ============================================================================ */

static void
test_recv_find_chan(void)
{
    fixture_t f = {0};
    if (!fixture_setup(&f)) {
        printf("  [SKIP] find_chan (fixture setup failed)\n");
        fixture_teardown(&f);
        return;
    }

    /* Open a couple more channels so the lookup walks the list. */
    auto r2 = n00b_quic_chan_open(f.conn);
    auto r3 = n00b_quic_chan_open(f.conn);
    n00b_quic_chan_t *chan2 = n00b_result_get(r2);
    n00b_quic_chan_t *chan3 = n00b_result_get(r3);

    /* Each chan is findable by its own stream_id. */
    assert(_n00b_quic_conn_find_chan(f.conn, n00b_quic_chan_id(f.chan)) == f.chan);
    assert(_n00b_quic_conn_find_chan(f.conn, n00b_quic_chan_id(chan2)) == chan2);
    assert(_n00b_quic_conn_find_chan(f.conn, n00b_quic_chan_id(chan3)) == chan3);
    /* Unknown stream_id → NULL. */
    assert(_n00b_quic_conn_find_chan(f.conn, 999999) == nullptr);

    fixture_teardown(&f);
    printf("  [PASS] conn registers chans + find_chan walks list\n");
}

/* ============================================================================
 * 2. Recv buffer accumulates + recv pulls bytes + memmove-shift works
 * ============================================================================ */

static void
test_recv_append_and_pull(void)
{
    fixture_t f = {0};
    if (!fixture_setup(&f)) {
        printf("  [SKIP] append_and_pull (fixture setup failed)\n");
        fixture_teardown(&f);
        return;
    }

    /* Synthetic STREAM_DATA: append "hello" then "world" via the helper. */
    _n00b_quic_chan_append_recv(f.chan, (const uint8_t *)"hello", 5);
    _n00b_quic_chan_append_recv(f.chan, (const uint8_t *)"world", 5);

    assert(n00b_quic_chan_has_data(f.chan));

    /* Pull 4 bytes — should see "hell". */
    uint8_t buf[16] = {0};
    auto    r1     = n00b_quic_chan_recv(f.chan, buf, 4);
    assert(n00b_result_is_ok(r1));
    assert(n00b_result_get(r1) == 4);
    assert(memcmp(buf, "hell", 4) == 0);
    assert(n00b_quic_chan_has_data(f.chan));   /* still 6 left */

    /* Pull 100 — should get the remaining 6 bytes "oworld". */
    auto r2 = n00b_quic_chan_recv(f.chan, buf, 100);
    assert(n00b_result_is_ok(r2));
    assert(n00b_result_get(r2) == 6);
    assert(memcmp(buf, "oworld", 6) == 0);

    /* Now empty. */
    assert(!n00b_quic_chan_has_data(f.chan));
    auto r3 = n00b_quic_chan_recv(f.chan, buf, 100);
    assert(n00b_result_is_ok(r3));
    assert(n00b_result_get(r3) == 0);

    /* bytes_received counter accumulated everything. */
    assert(f.chan->bytes_received == 10);

    fixture_teardown(&f);
    printf("  [PASS] recv-buffer append + pull + counter\n");
}

/* ============================================================================
 * 3. Recv buffer grows past initial capacity (geometric growth)
 * ============================================================================ */

static void
test_recv_grow(void)
{
    fixture_t f = {0};
    if (!fixture_setup(&f)) {
        printf("  [SKIP] grow (fixture setup failed)\n");
        fixture_teardown(&f);
        return;
    }

    /* Default initial cap is 256.  Append 10 KB to force several grows. */
    static const size_t total = 10 * 1024;
    uint8_t            *src   = malloc(total);
    for (size_t i = 0; i < total; i++) {
        src[i] = (uint8_t)(i & 0xff);
    }
    _n00b_quic_chan_append_recv(f.chan, src, total);

    /* Drain in 1 KB increments and verify byte values. */
    uint8_t out[1024];
    size_t  drained = 0;
    while (n00b_quic_chan_has_data(f.chan)) {
        auto r = n00b_quic_chan_recv(f.chan, out, sizeof(out));
        assert(n00b_result_is_ok(r));
        size_t n = n00b_result_get(r);
        for (size_t i = 0; i < n; i++) {
            assert(out[i] == (uint8_t)((drained + i) & 0xff));
        }
        drained += n;
    }
    assert(drained == total);
    free(src);

    fixture_teardown(&f);
    printf("  [PASS] recv-buffer grows geometrically + ordering preserved\n");
}

/* ============================================================================
 * 4. The peer's FIN reaches the reader only after the bytes ahead of it
 * ============================================================================ */

// Delivers one picoquic event the way the endpoint pump does: under the
// endpoint lock, through the connection's callback.
static void
pump_event(fixture_t                 *f,
           uint64_t                   stream_id,
           const char                *bytes,
           picoquic_call_back_event_t event)
{
    size_t len = bytes ? strlen(bytes) : 0;
    n00b_data_write_lock(f->ep->lock);
    (void)_n00b_quic_conn_default_callback(f->conn->cnx,
                                           stream_id,
                                           (uint8_t *)bytes,
                                           len,
                                           event,
                                           f->conn,
                                           nullptr);
    n00b_data_unlock(f->ep->lock);
}

static void
test_recv_fin_state(void)
{
    fixture_t f = {0};
    if (!fixture_setup(&f)) {
        printf("  [SKIP] fin_state (fixture setup failed)\n");
        fixture_teardown(&f);
        return;
    }

    uint64_t sid = n00b_quic_chan_id(f.chan);
    pump_event(&f, sid, "final-", picoquic_callback_stream_data);
    pump_event(&f, sid, "bytes", picoquic_callback_stream_fin);

    assert(n00b_quic_chan_state(f.chan) == N00B_QUIC_CHAN_STATE_RECV_HALF_CLOSED);
    bool fin_before = n00b_quic_chan_recv_fin(f.chan);

    uint8_t buf[64] = {0};
    size_t  n       = 0;
    while (n00b_quic_chan_has_data(f.chan)) {
        auto r = n00b_quic_chan_recv(f.chan, buf + n, 4);
        assert(n00b_result_is_ok(r));
        n += n00b_result_get(r);
    }
    printf("  FIN with 11 bytes unread: recv_fin=%d; after reading \"%.*s\": "
           "recv_fin=%d\n",
           fin_before,
           (int)n,
           (const char *)buf,
           n00b_quic_chan_recv_fin(f.chan));
    assert(!fin_before);
    assert(n == 11 && memcmp(buf, "final-bytes", 11) == 0);
    assert(n00b_quic_chan_recv_fin(f.chan));

    fixture_teardown(&f);
    printf("  [PASS] recv_fin waits for the bytes ahead of the FIN\n");
}

/* ============================================================================
 * 5. NULL / closed args on recv API
 * ============================================================================ */

static void
test_recv_null_args(void)
{
    uint8_t buf[16];

    /* recv on NULL → INVALID_ARG. */
    auto r = n00b_quic_chan_recv(nullptr, buf, sizeof(buf));
    assert(n00b_result_is_err(r));
    assert(n00b_result_get_err(r) == N00B_QUIC_ERR_INVALID_ARG);

    /* has_data + recv_fin on NULL are safe. */
    assert(!n00b_quic_chan_has_data(nullptr));
    assert(!n00b_quic_chan_recv_fin(nullptr));

    /* recv with max=0 → ok(0) regardless of state. */
    fixture_t f = {0};
    if (fixture_setup(&f)) {
        auto r2 = n00b_quic_chan_recv(f.chan, buf, 0);
        assert(n00b_result_is_ok(r2));
        assert(n00b_result_get(r2) == 0);

        /* recv with non-zero max but NULL out → NULL_ARG. */
        _n00b_quic_chan_append_recv(f.chan, (const uint8_t *)"x", 1);
        auto r3 = n00b_quic_chan_recv(f.chan, nullptr, 16);
        assert(n00b_result_is_err(r3));
        assert(n00b_result_get_err(r3) == N00B_QUIC_ERR_NULL_ARG);

        fixture_teardown(&f);
    }

    printf("  [PASS] recv null/zero-arg behavior\n");
}

// Long enough that only a stuck thread can exceed it.
#define HANG_DEADLINE_NS (60ull * 1000000000ull)
#define POLL_NS          1000000ull

#if defined(N00B_DEBUG)
/* ============================================================================
 * 6. The pump and the reader on different threads
 * ============================================================================ */

static _Atomic bool pump_in_window;
static _Atomic bool pump_go;

// Holds the pump after it has taken in its bytes and before the reader can
// see them.
static void
hold_pump(n00b_quic_conn_t *conn, n00b_quic_chan_t *chan, n00b_quic_debug_point_t point)
{
    (void)conn;
    (void)chan;
    if (point != N00B_QUIC_DEBUG_PUBLISH) {
        return;
    }
    atomic_store(&pump_in_window, true);
    while (!atomic_load(&pump_go)) {
        base_nanosleep_ns(POLL_NS);
    }
}

static bool
wait_flag(_Atomic bool *flag)
{
    uint64_t start = base_monotonic_ns();
    while (!atomic_load(flag) && base_monotonic_ns() - start < HANG_DEADLINE_NS) {
        base_nanosleep_ns(POLL_NS);
    }
    return atomic_load(flag);
}

typedef struct {
    n00b_quic_chan_t *chan;
    _Atomic bool      done;
} pump_arg_t;

static void *
pump_bbbb(void *arg)
{
    pump_arg_t *p = arg;
    _n00b_quic_chan_append_recv(p->chan, (const uint8_t *)"BBBB", 4);
    atomic_store(&p->done, true);
    return nullptr;
}

static size_t
drain_into(n00b_quic_chan_t *chan, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (n < cap && n00b_quic_chan_has_data(chan)) {
        auto r = n00b_quic_chan_recv(chan, out + n, cap - n);
        assert(n00b_result_is_ok(r));
        size_t got = n00b_result_get(r);
        if (got == 0) {
            break;
        }
        n += got;
    }
    return n;
}

// A reader that drains while the pump is midway through handing over bytes
// sees the stream exactly once, in order.
static void
test_recv_drain_during_pump(void)
{
    fixture_t f = {0};
    if (!fixture_setup(&f)) {
        printf("  [SKIP] drain_during_pump (fixture setup failed)\n");
        fixture_teardown(&f);
        return;
    }

    _n00b_quic_chan_append_recv(f.chan, (const uint8_t *)"AAAA", 4);

    atomic_store(&pump_in_window, false);
    atomic_store(&pump_go, false);
    _n00b_quic_debug_hook = hold_pump;
    pump_arg_t p = {.chan = f.chan};
    auto       r = n00b_thread_spawn(pump_bbbb, &p);
    assert(n00b_result_is_ok(r));
    assert(wait_flag(&pump_in_window));

    uint8_t stream[64] = {0};
    size_t  n          = drain_into(f.chan, stream, sizeof(stream));

    atomic_store(&pump_go, true);
    assert(wait_flag(&p.done));
    n00b_thread_join(n00b_result_get(r));
    _n00b_quic_debug_hook = nullptr;

    n += drain_into(f.chan, stream + n, sizeof(stream) - n);

    printf("  drain during a pump: %zu bytes read, \"", n);
    for (size_t i = 0; i < n; i++) {
        printf("%c", stream[i] ? stream[i] : '0');
    }
    printf("\"\n");
    assert(n == 8 && memcmp(stream, "AAAABBBB", 8) == 0);

    fixture_teardown(&f);
    printf("  [PASS] drain during a pump reads every byte once, in order\n");
}
#endif

/* ============================================================================
 * 7. Bytes the reader puts back come before the unread ones
 * ============================================================================ */

static void
test_recv_unread_keeps_order(void)
{
    fixture_t f = {0};
    if (!fixture_setup(&f)) {
        printf("  [SKIP] unread_keeps_order (fixture setup failed)\n");
        fixture_teardown(&f);
        return;
    }

    // What the H3 drivers do with a stream-type varint: read a slice, keep
    // the varint, and return the rest.
    _n00b_quic_chan_append_recv(f.chan, (const uint8_t *)"K123456789", 10);
    uint8_t tmp[8];
    auto    r = n00b_quic_chan_recv(f.chan, tmp, sizeof(tmp));
    assert(n00b_result_is_ok(r) && n00b_result_get(r) == 8);
    _n00b_quic_chan_unread(f.chan, tmp + 1, 7);

    uint8_t rest[32] = {0};
    size_t  n        = 0;
    while (n00b_quic_chan_has_data(f.chan)) {
        auto rr = n00b_quic_chan_recv(f.chan, rest + n, sizeof(rest) - n);
        assert(n00b_result_is_ok(rr));
        n += n00b_result_get(rr);
    }
    printf("  after returning 7 bytes: \"%.*s\"\n", (int)n, (const char *)rest);
    assert(n == 9 && memcmp(rest, "123456789", 9) == 0);

    fixture_teardown(&f);
    printf("  [PASS] returned bytes are read before the rest\n");
}

/* ============================================================================
 * 8. A pump and a reader racing over many chunks keep the stream intact
 * ============================================================================ */

#define RACE_CHUNKS 20000

typedef struct {
    n00b_quic_chan_t *chan;
    _Atomic bool      ready;
    _Atomic bool      go;
} race_arg_t;

static void *
race_pump(void *arg)
{
    race_arg_t *a = arg;
    uint8_t     chunk[17];
    uint32_t    next = 0;

    atomic_store(&a->ready, true);
    while (!atomic_load(&a->go)) {
        __asm__ volatile("" ::: "memory");
    }
    for (int i = 0; i < RACE_CHUNKS; i++) {
        size_t len = 1 + (size_t)(i % 17);
        for (size_t j = 0; j < len; j++) {
            chunk[j] = (uint8_t)(next++ & 0xff);
        }
        _n00b_quic_chan_append_recv(a->chan, chunk, len);
    }
    return nullptr;
}

static void
test_recv_pump_and_reader_race(void)
{
    fixture_t f = {0};
    if (!fixture_setup(&f)) {
        printf("  [SKIP] pump_and_reader_race (fixture setup failed)\n");
        fixture_teardown(&f);
        return;
    }

    size_t total = 0;
    for (int i = 0; i < RACE_CHUNKS; i++) {
        total += 1 + (size_t)(i % 17);
    }

    race_arg_t a = {.chan = f.chan};
    auto       r = n00b_thread_spawn(race_pump, &a);
    assert(n00b_result_is_ok(r));
    while (!atomic_load(&a.ready)) {
        base_nanosleep_ns(POLL_NS);
    }
    atomic_store(&a.go, true);

    size_t   got        = 0;
    size_t   mismatches = 0;
    uint64_t start      = base_monotonic_ns();
    uint8_t  buf[64];
    while (got < total && base_monotonic_ns() - start < HANG_DEADLINE_NS) {
        auto rr = n00b_quic_chan_recv(f.chan, buf, sizeof(buf));
        assert(n00b_result_is_ok(rr));
        size_t n = n00b_result_get(rr);
        for (size_t i = 0; i < n; i++) {
            mismatches += buf[i] != (uint8_t)((got + i) & 0xff);
        }
        got += n;
    }
    n00b_thread_join(n00b_result_get(r));

    printf("  pump and reader racing over %d chunks: %zu of %zu bytes, %zu out "
           "of place\n",
           RACE_CHUNKS,
           got,
           total,
           mismatches);
    assert(got == total && mismatches == 0);

    fixture_teardown(&f);
    printf("  [PASS] pump and reader racing keep the stream intact\n");
}

#if defined(N00B_DEBUG)
/* ============================================================================
 * 9. The pump running inside a driver's window
 *
 * A debug hook stops the driver at one point while a pump thread delivers
 * one event the way run_once does.  Where the window sits under the
 * endpoint lock the pump cannot finish until the driver moves on, so the
 * hook gives it WINDOW_NS and then continues.
 * ============================================================================ */

// Long enough for a pump that is not blocked to deliver one event.  A
// blocked pump cannot finish however long the hook waits.
#define WINDOW_NS (1000ull * 1000000ull)

typedef struct {
    fixture_t                 *f;
    uint64_t                   stream_id;
    const char                *bytes;
    picoquic_call_back_event_t event;
    n00b_quic_debug_point_t    point;
    uint64_t                   wait_ns;
    _Atomic bool               fired;
    _Atomic bool               go;
    _Atomic bool               done;
} window_job_t;

static window_job_t *window_job;

static void *
window_pump(void *arg)
{
    window_job_t *j     = arg;
    uint64_t      start = base_monotonic_ns();
    while (!atomic_load(&j->go) && base_monotonic_ns() - start < HANG_DEADLINE_NS) {
        base_nanosleep_ns(POLL_NS);
    }
    pump_event(j->f, j->stream_id, j->bytes, j->event);
    atomic_store(&j->done, true);
    return nullptr;
}

static void
run_pump_in_window(n00b_quic_conn_t *conn, n00b_quic_chan_t *chan, n00b_quic_debug_point_t point)
{
    (void)conn;
    (void)chan;
    window_job_t *j = window_job;
    if (j == nullptr || point != j->point || atomic_exchange(&j->fired, true)) {
        return;
    }
    atomic_store(&j->go, true);
    uint64_t start = base_monotonic_ns();
    while (!atomic_load(&j->done) && base_monotonic_ns() - start < j->wait_ns) {
        base_nanosleep_ns(POLL_NS);
    }
}

static n00b_thread_t *
window_arm(window_job_t *j)
{
    window_job = j;
    auto r     = n00b_thread_spawn(window_pump, j);
    assert(n00b_result_is_ok(r));
    _n00b_quic_debug_hook = run_pump_in_window;
    return n00b_result_get(r);
}

static void
window_disarm(window_job_t *j, n00b_thread_t *t)
{
    _n00b_quic_debug_hook = nullptr;
    atomic_store(&j->go, true);
    n00b_thread_join(t);
    window_job = nullptr;
}

// The driver sends FIN while the pump takes in the peer's FIN or RESET, as a
// bidi RPC call's send pump does while its receive pump runs run_once.
static void
state_case(picoquic_call_back_event_t peer_event, n00b_quic_chan_state_t want, const char *what)
{
    fixture_t f = {0};
    if (!fixture_setup(&f)) {
        printf("  [SKIP] state %s (fixture setup failed)\n", what);
        fixture_teardown(&f);
        return;
    }

    window_job_t   j = {.f         = &f,
                        .stream_id = n00b_quic_chan_id(f.chan),
                        .event     = peer_event,
                        .point     = N00B_QUIC_DEBUG_TRANSITION,
                        .wait_ns   = HANG_DEADLINE_NS};
    n00b_thread_t *t = window_arm(&j);
    auto           r = n00b_quic_chan_send(f.chan, (const uint8_t *)"x", 1, .fin = true);
    window_disarm(&j, t);
    assert(n00b_result_is_ok(r));

    n00b_quic_chan_state_t st = n00b_quic_chan_state(f.chan);
    printf("  local FIN racing the peer's %s: state %d, want %d\n", what, (int)st, (int)want);
    assert(atomic_load(&j.fired));
    assert(st == want);

    fixture_teardown(&f);
    printf("  [PASS] the peer's %s survives a local FIN\n", what);
}

static void
test_state_fin_both_ways(void)
{
    state_case(picoquic_callback_stream_fin, N00B_QUIC_CHAN_STATE_CLOSED, "FIN");
    state_case(picoquic_callback_stream_reset, N00B_QUIC_CHAN_STATE_PEER_RESET, "RESET");
}

// The pump wraps a peer stream while a driver opens one of its own, as when
// an RPC call's pump thread takes in the server's control streams after
// connect while another call opens its request stream.
static void
test_register_during_open(void)
{
    fixture_t f = {0};
    if (!fixture_setup(&f)) {
        printf("  [SKIP] register_during_open (fixture setup failed)\n");
        fixture_teardown(&f);
        return;
    }

    // Stream 1 is server-initiated, so the pump wraps it for this client.
    window_job_t   j = {.f         = &f,
                        .stream_id = 1,
                        .bytes     = "peer",
                        .event     = picoquic_callback_stream_data,
                        .point     = N00B_QUIC_DEBUG_REGISTER,
                        .wait_ns   = WINDOW_NS};
    n00b_thread_t *t = window_arm(&j);
    auto           r = n00b_quic_chan_open(f.conn);
    window_disarm(&j, t);
    assert(n00b_result_is_ok(r));

    n00b_quic_chan_t *mine = n00b_result_get(r);
    n00b_quic_chan_t *peer = _n00b_quic_conn_find_chan(f.conn, 1);
    bool mine_found = _n00b_quic_conn_find_chan(f.conn, n00b_quic_chan_id(mine)) == mine;
    printf("  open racing a peer stream: opened chan %s, peer chan %s\n",
           mine_found ? "listed" : "missing",
           peer ? "listed" : "missing");
    assert(atomic_load(&j.fired));
    assert(mine_found && peer != nullptr && n00b_quic_chan_has_data(peer));

    fixture_teardown(&f);
    printf("  [PASS] both channels stay on the connection\n");
}

/* ============================================================================
 * 10. A second concurrent reader trips the debug assert
 * ============================================================================ */

static _Atomic bool reader_fired;
static _Atomic bool reader_in_window;

static void
hold_first_reader(n00b_quic_conn_t *conn, n00b_quic_chan_t *chan, n00b_quic_debug_point_t point)
{
    (void)conn;
    (void)chan;
    if (point != N00B_QUIC_DEBUG_READ || atomic_exchange(&reader_fired, true)) {
        return;
    }
    atomic_store(&reader_in_window, true);
    for (;;) {
        base_nanosleep_ns(POLL_NS);
    }
}

static void *
first_reader(void *arg)
{
    uint8_t b[4];
    (void)n00b_quic_chan_recv((n00b_quic_chan_t *)arg, b, sizeof(b));
    return nullptr;
}

// Runs in a child process: one reader is held inside recv while a second
// reads.  Exits 0 only if nothing stopped the second reader.
static int
second_reader_child(void)
{
    fixture_t f = {0};
    if (!fixture_setup(&f)) {
        return 2;
    }
    _n00b_quic_chan_append_recv(f.chan, (const uint8_t *)"abcd", 4);
    _n00b_quic_debug_hook = hold_first_reader;
    auto r = n00b_thread_spawn(first_reader, f.chan);
    assert(n00b_result_is_ok(r));
    assert(wait_flag(&reader_in_window));

    uint8_t b[4];
    (void)n00b_quic_chan_recv(f.chan, b, sizeof(b));
    fprintf(stderr, "second reader ran unchecked\n");
    fflush(stderr);
    _exit(0);
}

static void
test_second_reader_asserts(const char *self)
{
    int fds[2];
    assert(pipe(fds) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        execl(self, self, "second-reader-child", (char *)nullptr);
        _exit(43);
    }
    close(fds[1]);

    static char out[65536];
    size_t      n = 0;
    ssize_t     got;
    while (n < sizeof(out) - 1 && (got = read(fds[0], out + n, sizeof(out) - 1 - n)) > 0) {
        n += (size_t)got;
    }
    out[n] = '\0';
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);

    bool caught = strstr(out, "one reader at a time") != nullptr;
    printf("  second concurrent reader: child %s %d, assert %s\n",
           WIFSIGNALED(status) ? "signal" : "exit",
           WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status),
           caught ? "fired" : "did not fire");
    assert(caught);
    printf("  [PASS] a second concurrent reader is caught\n");
}
#endif

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    // A name on the command line runs only that test.
    const char *only = argc > 1 ? argv[1] : nullptr;
#if defined(N00B_DEBUG)
    if (only != nullptr && strcmp(only, "second-reader-child") == 0) {
        return second_reader_child();
    }
#endif

    printf("test_quic_chan_recv:\n");
    fflush(stdout);

    if (only != nullptr) {
#if defined(N00B_DEBUG)
        if (strcmp(only, "pump") == 0) {
            test_recv_drain_during_pump();
        }
        if (strcmp(only, "state-fin") == 0) {
            state_case(picoquic_callback_stream_fin, N00B_QUIC_CHAN_STATE_CLOSED, "FIN");
        }
        if (strcmp(only, "state-reset") == 0) {
            state_case(picoquic_callback_stream_reset, N00B_QUIC_CHAN_STATE_PEER_RESET, "RESET");
        }
        if (strcmp(only, "register") == 0) {
            test_register_during_open();
        }
        if (strcmp(only, "second-reader") == 0) {
            test_second_reader_asserts(argv[0]);
        }
#endif
        if (strcmp(only, "fin") == 0) {
            test_recv_fin_state();
        }

        if (strcmp(only, "unread") == 0) {
            test_recv_unread_keeps_order();
        }
        if (strcmp(only, "race") == 0) {
            test_recv_pump_and_reader_race();
        }
        n00b_shutdown();
        return 0;
    }

    test_recv_find_chan();         fflush(stdout);
    test_recv_append_and_pull();   fflush(stdout);
    test_recv_grow();              fflush(stdout);
    test_recv_fin_state();         fflush(stdout);
    test_recv_null_args();         fflush(stdout);
#if defined(N00B_DEBUG)
    test_recv_drain_during_pump(); fflush(stdout);
#endif
    test_recv_unread_keeps_order(); fflush(stdout);
    test_recv_pump_and_reader_race(); fflush(stdout);
#if defined(N00B_DEBUG)
    test_state_fin_both_ways();    fflush(stdout);
    test_register_during_open();   fflush(stdout);
    test_second_reader_asserts(argv[0]); fflush(stdout);
#endif

    printf("All quic chan_recv tests passed.\n");
    n00b_shutdown();
    return 0;
}
