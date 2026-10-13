/*
 * test_quic_h3_pump_thread.c: the endpoint pump and the H3 drivers on
 * different threads.
 *
 * One thread only runs n00b_quic_endpoint_run_once on both endpoints of an
 * in-process H3 loopback.  The test's main thread only drives the H3 client
 * and server and consumes bodies.  Every piece of receive state the pump
 * hands to a driver crosses between those two threads:
 *
 *   1. two_thread_bodies       multi-chunk bodies both ways, ending in FIN,
 *                              compared byte for byte.
 *   2. client_fin_after_drain  the response's last bytes and FIN arrive
 *                              after the client drained and before it
 *                              checked for FIN, as when one RPC call's pump
 *                              thread runs run_once while another call's
 *                              thread drives the shared client.
 *   3. client_body_grown       a reader on another thread consumes the
 *                              response body while the driver appends, as
 *                              a streaming call's pump does while another
 *                              call drives.
 *   4. server_body_grown       the same for the request body, as the RPC
 *                              server's receive pump does while the I/O
 *                              loop drives the server.
 *
 * Cases 2 to 4 hold the driver in one window with a debug hook.
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/string.h"
#include "core/buffer.h"
#include "core/data_lock.h"
#include "core/platform.h"
#include "conduit/conduit.h"
#include "conduit/io.h"
#include "net/quic/quic_types.h"
#include "net/quic/endpoint.h"
#include "net/quic/conn.h"
#include "net/quic/chan.h"
#include "net/quic/h3.h"
#include "net/quic/h3_types.h"
#include "internal/net/quic/endpoint_internal.h"
#include "internal/net/quic/conn_internal.h"
#include "internal/net/quic/chan_internal.h"
#include "internal/net/quic/h3_internal.h"
#include "picoquic.h"

#include "../fixtures/quic_test_pki.h"

// Long enough that only a stuck thread can exceed it.
#define HANG_DEADLINE_NS (60ull * 1000000000ull)
#define POLL_NS          100000ull

static bool
deadline_passed(uint64_t start)
{
    return base_monotonic_ns() - start >= HANG_DEADLINE_NS;
}

/* ============================================================================
 * Loopback scaffold
 * ============================================================================ */

typedef struct {
    char *key_pem_path;

    n00b_conduit_t            *conduit;
    n00b_conduit_io_backend_t *io;

    n00b_quic_endpoint_t *server_ep;
    n00b_quic_endpoint_t *client_ep;

    n00b_h3_server_t        *server;
    n00b_h3_client_t        *client_h3;
    n00b_quic_conn_t        *client_conn;
    n00b_h3_request_inbox_t *req_inbox;

    n00b_thread_t *pump;
    _Atomic bool   pump_stop;
} h3_loop_t;

static void
pump_both(h3_loop_t *L)
{
    (void)n00b_quic_endpoint_run_once(L->client_ep, 1);
    (void)n00b_quic_endpoint_run_once(L->server_ep, 1);
}

static void
drive_both(h3_loop_t *L)
{
    (void)n00b_h3_client_drive(L->client_h3);
    (void)n00b_h3_server_drive(L->server);
}

// The pump thread: endpoint I/O only, never an H3 call.
static void *
pump_main(void *arg)
{
    h3_loop_t *L = arg;
    while (!atomic_load(&L->pump_stop)) {
        pump_both(L);
    }
    return nullptr;
}

static void
loop_teardown(h3_loop_t *L)
{
    if (L->pump) {
        atomic_store(&L->pump_stop, true);
        n00b_thread_join(L->pump);
        L->pump = nullptr;
    }
    if (L->client_h3)   n00b_h3_client_close(L->client_h3);
    if (L->server)      n00b_h3_server_close(L->server);
    if (L->client_conn) n00b_quic_close(L->client_conn, 0);
    if (L->client_ep)   n00b_quic_endpoint_close(L->client_ep);
    if (L->server_ep)   n00b_quic_endpoint_close(L->server_ep);
    if (L->io)          n00b_conduit_io_destroy(L->io);
    if (L->conduit)     n00b_conduit_destroy(L->conduit);
    if (L->key_pem_path) {
        unlink(L->key_pem_path);
        free(L->key_pem_path);
    }
    memset(L, 0, sizeof(*L));
}

// Brings up both endpoints and the H3 pair on one thread, then hands the
// endpoints to a pump thread.
static bool
loop_setup(h3_loop_t *L)
{
    memset(L, 0, sizeof(*L));

    L->key_pem_path = n00b_quic_test_write_key_pem();
    if (!L->key_pem_path) return false;

    auto cr = n00b_conduit_new();
    if (n00b_result_is_err(cr)) goto fail;
    L->conduit = n00b_result_get(cr);

    auto ir = n00b_conduit_io_new_default(L->conduit);
    if (n00b_result_is_err(ir)) goto fail;
    L->io = n00b_result_get(ir);

    auto sr = n00b_quic_endpoint_new(L->conduit, L->io,
                                     .listen         = true,
                                     .bind_host      = "127.0.0.1",
                                     .alpn           = N00B_H3_ALPN,
                                     .cert_der_bytes = n00b_quic_test_cert_der,
                                     .cert_der_len   = n00b_quic_test_cert_der_len,
                                     .key_pem_path   = L->key_pem_path);
    if (n00b_result_is_err(sr)) goto fail;
    L->server_ep   = n00b_result_get(sr);
    uint16_t sport = n00b_quic_endpoint_local_port(L->server_ep);

    auto cur = n00b_quic_endpoint_new(L->conduit, L->io,
                                      .bind_host = "127.0.0.1",
                                      .alpn      = N00B_H3_ALPN);
    if (n00b_result_is_err(cur)) goto fail;
    L->client_ep = n00b_result_get(cur);
    picoquic_set_null_verifier(L->client_ep->quic);

    // Early publish hands the driver each request at HEADERS, so it can
    // consume the body while it arrives.
    auto serv = n00b_h3_server_new(L->server_ep, L->conduit, .early_publish = true);
    if (n00b_result_is_err(serv)) goto fail;
    L->server = n00b_result_get(serv);

    L->req_inbox = n00b_h3_request_inbox_new(L->conduit);
    n00b_h3_request_subscribe(n00b_h3_server_request_topic(L->server),
                              L->req_inbox,
                              .operations = N00B_CONDUIT_OP_ALL);

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(sport);
    inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);

    auto rr = n00b_quic_connect(L->client_ep,
                                (const struct sockaddr *)&dst,
                                n00b_string_from_cstr("quic-test.n00b.local"));
    if (n00b_result_is_err(rr)) goto fail;
    L->client_conn = n00b_result_get(rr);

    uint64_t start = base_monotonic_ns();
    while (n00b_quic_conn_state(L->client_conn) != N00B_QUIC_CONN_STATE_CONNECTED) {
        if (deadline_passed(start)) goto fail;
        pump_both(L);
        (void)n00b_h3_server_drive(L->server);
    }

    auto hr = n00b_h3_client_new(L->client_conn);
    if (n00b_result_is_err(hr)) goto fail;
    L->client_h3 = n00b_result_get(hr);

    // Let SETTINGS and the uni stream preludes cross in both directions.
    start = base_monotonic_ns();
    while (!n00b_h3_client_peer_settings(L->client_h3).received) {
        if (deadline_passed(start)) goto fail;
        pump_both(L);
        drive_both(L);
    }

    auto pr = n00b_thread_spawn(pump_main, L);
    if (n00b_result_is_err(pr)) goto fail;
    L->pump = n00b_result_get(pr);
    return true;

fail:
    loop_teardown(L);
    return false;
}

static n00b_h3_inbound_request_t *
pop_request(h3_loop_t *L)
{
    if (!n00b_h3_request_inbox_has_messages(L->req_inbox)) {
        return nullptr;
    }
    n00b_h3_request_msg_t *m = n00b_h3_request_inbox_pop(L->req_inbox);
    return m ? m->payload.req : nullptr;
}

static n00b_h3_request_t *
open_request(h3_loop_t *L)
{
    auto r = n00b_h3_client_request_raw(L->client_h3, "POST", "https", "localhost", "/",
                                        .fin = false);
    assert(n00b_result_is_ok(r));
    return n00b_result_get(r);
}

// Drives until the server publishes the request on stream @p sid.
static n00b_h3_inbound_request_t *
await_inbound(h3_loop_t *L, uint64_t sid)
{
    uint64_t start = base_monotonic_ns();
    while (!deadline_passed(start)) {
        drive_both(L);
        n00b_h3_inbound_request_t *ireq = pop_request(L);
        if (ireq != nullptr && n00b_h3_inbound_request_stream_id(ireq) == sid) {
            return ireq;
        }
        base_nanosleep_ns(POLL_NS);
    }
    return nullptr;
}

static bool
peer_fin_arrived(n00b_quic_chan_t *chan)
{
    n00b_quic_chan_state_t st = n00b_quic_chan_state(chan);
    return st == N00B_QUIC_CHAN_STATE_RECV_HALF_CLOSED || st == N00B_QUIC_CHAN_STATE_CLOSED;
}

static uint8_t
pattern(uint64_t sid, size_t i, uint8_t salt)
{
    return (uint8_t)((sid * 131u + i * 7u + (i >> 8) + salt) | 1u);
}

/* ============================================================================
 * 1. Multi-chunk bodies both ways with the pump on its own thread
 * ============================================================================ */

#define NREQ       6
#define BODY_BYTES (48 * 1024)
#define REQ_SALT   0x11
#define RESP_SALT  0x5a

typedef struct {
    n00b_h3_request_t         *creq;
    n00b_h3_inbound_request_t *ireq;
    uint64_t                   sid;
    uint8_t                   *req_body;
    size_t                     req_got;
    uint8_t                   *resp_body;
    size_t                     resp_got;
    bool                       responded;
    bool                       finished;
} exchange_t;

static size_t
chunk_len(size_t i)
{
    return 1 + (i * 977) % 4000;
}

static void
send_body(n00b_h3_request_t *creq, n00b_h3_inbound_request_t *ireq, uint64_t sid, uint8_t salt)
{
    static uint8_t chunk[4096];
    size_t         off = 0;
    for (size_t i = 0; off < BODY_BYTES; i++) {
        size_t len = chunk_len(i);
        if (len > BODY_BYTES - off) {
            len = BODY_BYTES - off;
        }
        for (size_t k = 0; k < len; k++) {
            chunk[k] = pattern(sid, off + k, salt);
        }
        bool last = off + len == BODY_BYTES;
        if (creq != nullptr) {
            auto r = n00b_h3_request_send_data_raw(creq, chunk, len, last);
            assert(n00b_result_is_ok(r));
        }
        else {
            auto r = n00b_h3_inbound_request_send_data_raw(ireq, chunk, len, last);
            assert(n00b_result_is_ok(r));
        }
        off += len;
    }
}

static size_t
count_mismatches(const uint8_t *got, size_t n, uint64_t sid, uint8_t salt)
{
    size_t bad = n > BODY_BYTES ? n - BODY_BYTES : BODY_BYTES - n;
    size_t m   = n < BODY_BYTES ? n : BODY_BYTES;
    for (size_t i = 0; i < m; i++) {
        bad += got[i] != pattern(sid, i, salt);
    }
    return bad;
}

static int
test_two_thread_bodies(void)
{
    h3_loop_t L;
    if (!loop_setup(&L)) {
        printf("  [SKIP] two_thread_bodies (loop_setup failed)\n");
        return 0;
    }

    static exchange_t x[NREQ];
    memset(x, 0, sizeof(x));
    for (int r = 0; r < NREQ; r++) {
        x[r].creq      = open_request(&L);
        x[r].sid       = n00b_h3_request_stream_id(x[r].creq);
        x[r].req_body  = calloc(1, BODY_BYTES + 4096);
        x[r].resp_body = calloc(1, BODY_BYTES + 4096);
        send_body(x[r].creq, nullptr, x[r].sid, REQ_SALT);
    }

    uint8_t  buf[3000];
    int      finished = 0;
    uint64_t start    = base_monotonic_ns();
    while (finished < NREQ && !deadline_passed(start)) {
        drive_both(&L);

        n00b_h3_inbound_request_t *ireq;
        while ((ireq = pop_request(&L)) != nullptr) {
            uint64_t sid = n00b_h3_inbound_request_stream_id(ireq);
            for (int r = 0; r < NREQ; r++) {
                if (x[r].sid == sid) {
                    x[r].ireq = ireq;
                }
            }
        }

        for (int r = 0; r < NREQ; r++) {
            exchange_t *e = &x[r];
            if (e->ireq != nullptr && !e->responded) {
                // Read the FIN flag before draining, so a body is complete
                // once a drain after a true flag comes up empty.
                bool   fin = n00b_h3_inbound_request_peer_fin(e->ireq);
                size_t n;
                while ((n = n00b_h3_inbound_request_consume_body(e->ireq, buf, sizeof(buf))) > 0) {
                    if (e->req_got + n <= BODY_BYTES + 4096) {
                        memcpy(e->req_body + e->req_got, buf, n);
                    }
                    e->req_got += n;
                }
                if (fin) {
                    auto hr = n00b_h3_inbound_request_send_headers_raw(e->ireq, 200, nullptr, 0);
                    assert(n00b_result_is_ok(hr));
                    send_body(nullptr, e->ireq, e->sid, RESP_SALT);
                    e->responded = true;
                }
            }
            if (!e->finished) {
                bool   fin = n00b_h3_request_recv_fin(e->creq);
                size_t n;
                while ((n = n00b_h3_request_consume_body(e->creq, buf, sizeof(buf))) > 0) {
                    if (e->resp_got + n <= BODY_BYTES + 4096) {
                        memcpy(e->resp_body + e->resp_got, buf, n);
                    }
                    e->resp_got += n;
                }
                if (fin) {
                    e->finished = true;
                    finished++;
                }
            }
        }
        base_nanosleep_ns(POLL_NS);
    }

    size_t bad = 0;
    size_t moved = 0;
    for (int r = 0; r < NREQ; r++) {
        bad += count_mismatches(x[r].req_body, x[r].req_got, x[r].sid, REQ_SALT);
        bad += count_mismatches(x[r].resp_body, x[r].resp_got, x[r].sid, RESP_SALT);
        moved += x[r].req_got + x[r].resp_got;
        free(x[r].req_body);
        free(x[r].resp_body);
    }
    printf("  %d exchanges of %d-byte bodies each way: %d finished, %zu bytes "
           "moved, %zu wrong or missing\n",
           NREQ,
           BODY_BYTES,
           finished,
           moved,
           bad);

    loop_teardown(&L);
    if (finished != NREQ || bad != 0) {
        printf("  [FAIL] two_thread_bodies\n");
        return 1;
    }
    printf("  [PASS] two_thread_bodies\n");
    return 0;
}

#if defined(N00B_DEBUG)
/* ============================================================================
 * Window harness: a debug hook holds the driver at one point on one channel
 * while a worker thread runs a step, then lets the driver go on.
 * ============================================================================ */

// Long enough for a worker that is not blocked to finish its step.  A worker
// blocked on a lock the driver holds cannot finish however long this is.
#define WINDOW_NS (1000ull * 1000000ull)

typedef struct {
    n00b_quic_chan_t     *chan;
    n00b_h3_debug_point_t point;
    uint64_t              wait_ns;
    void (*step)(void *);
    void        *arg;
    _Atomic bool fired;
    _Atomic bool go;
    _Atomic bool done;
} h3_window_t;

static h3_window_t *h3_window;

static void *
window_worker(void *arg)
{
    h3_window_t *w     = arg;
    uint64_t     start = base_monotonic_ns();
    while (!atomic_load(&w->go) && !deadline_passed(start)) {
        base_nanosleep_ns(POLL_NS);
    }
    if (atomic_load(&w->fired)) {
        w->step(w->arg);
    }
    atomic_store(&w->done, true);
    return nullptr;
}

static void
window_hook(n00b_quic_chan_t *chan, n00b_h3_debug_point_t point)
{
    h3_window_t *w = h3_window;
    if (w == nullptr || chan != w->chan || point != w->point || atomic_exchange(&w->fired, true)) {
        return;
    }
    atomic_store(&w->go, true);
    uint64_t start = base_monotonic_ns();
    while (!atomic_load(&w->done) && base_monotonic_ns() - start < w->wait_ns) {
        base_nanosleep_ns(POLL_NS);
    }
}

static n00b_thread_t *
window_arm(h3_window_t *w)
{
    h3_window = w;
    auto r    = n00b_thread_spawn(window_worker, w);
    assert(n00b_result_is_ok(r));
    _n00b_h3_debug_hook = window_hook;
    return n00b_result_get(r);
}

static void
window_disarm(h3_window_t *w, n00b_thread_t *t)
{
    _n00b_h3_debug_hook = nullptr;
    atomic_store(&w->go, true);
    n00b_thread_join(t);
    h3_window = nullptr;
}

/* ============================================================================
 * 2. Response bytes and FIN land between the client's drain and FIN check
 * ============================================================================ */

typedef struct {
    n00b_h3_request_t         *creq;
    n00b_h3_inbound_request_t *ireq;
} pair_t;

// Server sends the last bytes with FIN, then waits for the pump to hand
// both to the client channel.
static void
server_finishes(void *arg)
{
    pair_t *p = arg;
    auto    r = n00b_h3_inbound_request_send_data_raw(p->ireq, (const uint8_t *)"part2", 5, true);
    assert(n00b_result_is_ok(r));
    uint64_t start = base_monotonic_ns();
    while (!peer_fin_arrived(p->creq->chan) && !deadline_passed(start)) {
        base_nanosleep_ns(POLL_NS);
    }
}

static int
test_client_fin_after_drain(void)
{
    h3_loop_t L;
    if (!loop_setup(&L)) {
        printf("  [SKIP] client_fin_after_drain (loop_setup failed)\n");
        return 0;
    }

    pair_t p = {.creq = open_request(&L)};
    auto   fr = n00b_h3_request_send_data_raw(p.creq, nullptr, 0, true);
    assert(n00b_result_is_ok(fr));
    p.ireq = await_inbound(&L, n00b_h3_request_stream_id(p.creq));
    assert(p.ireq != nullptr);
    auto hr = n00b_h3_inbound_request_send_headers_raw(p.ireq, 200, nullptr, 0);
    assert(n00b_result_is_ok(hr));
    auto dr = n00b_h3_inbound_request_send_data_raw(p.ireq, (const uint8_t *)"part1", 5, false);
    assert(n00b_result_is_ok(dr));

    char     body[64] = {0};
    size_t   got      = 0;
    uint64_t start    = base_monotonic_ns();
    while (got < 5 && !deadline_passed(start)) {
        drive_both(&L);
        got += n00b_h3_request_consume_body(p.creq, (uint8_t *)body + got, sizeof(body) - 1 - got);
        base_nanosleep_ns(POLL_NS);
    }

    h3_window_t    w = {.chan    = p.creq->chan,
                        .point   = N00B_H3_DEBUG_DRAINED,
                        .wait_ns = HANG_DEADLINE_NS,
                        .step    = server_finishes,
                        .arg     = &p};
    n00b_thread_t *t = window_arm(&w);
    start            = base_monotonic_ns();
    while (!n00b_h3_request_recv_fin(p.creq) && !deadline_passed(start)) {
        (void)n00b_h3_client_drive(L.client_h3);
        base_nanosleep_ns(POLL_NS);
    }
    window_disarm(&w, t);
    got += n00b_h3_request_consume_body(p.creq, (uint8_t *)body + got, sizeof(body) - 1 - got);

    printf("  client saw FIN with body \"%s\"\n", body);
    loop_teardown(&L);
    if (!atomic_load(&w.fired) || strcmp(body, "part1part2") != 0) {
        printf("  [FAIL] client_fin_after_drain\n");
        return 1;
    }
    printf("  [PASS] client_fin_after_drain\n");
    return 0;
}

/* ============================================================================
 * 3 and 4. A reader on another thread consumes a body while the driver grows
 * it
 * ============================================================================ */

typedef struct {
    pair_t  *p;
    bool     server_side;
    uint8_t  out[64];
    size_t   got;
} side_reader_t;

static void
read_body_from_side(void *arg)
{
    side_reader_t *s = arg;
    s->got = s->server_side
               ? n00b_h3_inbound_request_consume_body(s->p->ireq, s->out, sizeof(s->out))
               : n00b_h3_request_consume_body(s->p->creq, s->out, sizeof(s->out));
}

static int
body_grown_case(bool server_side)
{
    const char *name = server_side ? "server_body_grown" : "client_body_grown";
    h3_loop_t   L;
    if (!loop_setup(&L)) {
        printf("  [SKIP] %s (loop_setup failed)\n", name);
        return 0;
    }

    pair_t p = {.creq = open_request(&L)};
    p.ireq   = await_inbound(&L, n00b_h3_request_stream_id(p.creq));
    assert(p.ireq != nullptr);
    if (!server_side) {
        auto hr = n00b_h3_inbound_request_send_headers_raw(p.ireq, 200, nullptr, 0);
        assert(n00b_result_is_ok(hr));
    }

    side_reader_t  s = {.p = &p, .server_side = server_side};
    h3_window_t    w = {.chan    = server_side ? p.ireq->chan : p.creq->chan,
                        .point   = N00B_H3_DEBUG_BODY_GROWN,
                        .wait_ns = WINDOW_NS,
                        .step    = read_body_from_side,
                        .arg     = &s};
    n00b_thread_t *t = window_arm(&w);
    if (server_side) {
        auto r = n00b_h3_request_send_data_raw(p.creq, (const uint8_t *)"ABCDEFGH", 8, true);
        assert(n00b_result_is_ok(r));
    }
    else {
        auto r = n00b_h3_inbound_request_send_data_raw(p.ireq, (const uint8_t *)"ABCDEFGH", 8, true);
        assert(n00b_result_is_ok(r));
    }

    uint64_t start = base_monotonic_ns();
    while (!atomic_load(&w.done) && !deadline_passed(start)) {
        if (server_side) {
            (void)n00b_h3_server_drive(L.server);
        }
        else {
            (void)n00b_h3_client_drive(L.client_h3);
        }
        base_nanosleep_ns(POLL_NS);
    }
    window_disarm(&w, t);

    char   body[64] = {0};
    size_t got      = s.got < sizeof(body) - 1 ? s.got : sizeof(body) - 1;
    for (size_t i = 0; i < got; i++) {
        body[i] = s.out[i] ? (char)s.out[i] : '0';
    }
    got += server_side
             ? n00b_h3_inbound_request_consume_body(p.ireq, (uint8_t *)body + got, sizeof(body) - 1 - got)
             : n00b_h3_request_consume_body(p.creq, (uint8_t *)body + got, sizeof(body) - 1 - got);

    printf("  %s body read by another thread mid-append: first read %zu bytes, "
           "whole body \"%s\"\n",
           server_side ? "request" : "response",
           s.got,
           body);
    loop_teardown(&L);
    if (!atomic_load(&w.fired) || strcmp(body, "ABCDEFGH") != 0) {
        printf("  [FAIL] %s\n", name);
        return 1;
    }
    printf("  [PASS] %s\n", name);
    return 0;
}
#endif

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_quic_h3_pump_thread:\n");
    fflush(stdout);

    // A name on the command line runs only that case.
    const char *only = argc > 1 ? argv[1] : nullptr;
    int         rc   = 0;

#define RUN(name, call)                                         \
    if (only == nullptr || strcmp(only, name) == 0) {           \
        rc |= (call);                                           \
        fflush(stdout);                                         \
    }

    RUN("bodies", test_two_thread_bodies());
#if defined(N00B_DEBUG)
    RUN("client-fin", test_client_fin_after_drain());
    RUN("client-body", body_grown_case(false));
    RUN("server-body", body_grown_case(true));
#endif

    n00b_shutdown();
    if (rc != 0) {
        printf("FAILED\n");
        return 1;
    }
    printf("All quic h3 pump-thread tests passed.\n");
    return 0;
}
