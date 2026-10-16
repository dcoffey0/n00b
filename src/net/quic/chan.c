/*
 * chan.c — Channel (single QUIC stream) wrapper.
 *
 * Phase 1 (this revision): open, send (with FIN), state, reset,
 * stop_sending, close.  Recv path lands with the per-cnx callback
 * fan-out work.
 */

#define N00B_USE_INTERNAL_API
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/time.h"
#include "core/data_lock.h"
#include "net/quic/quic_types.h"
#include "net/quic/chan.h"
#include "net/quic/conn.h"
#include "net/quic/metrics.h"
#include "internal/net/quic/chan_internal.h"
#include "internal/net/quic/conn_internal.h"
#include "internal/net/quic/endpoint_internal.h"
#include "internal/net/quic/metrics_internal.h"

#include "picoquic.h"
#include "picoquic_internal.h"  /* picoquic_find_stream + stream_head_t —
                                   see docs/net/quic/vendored.md
                                   "Internal header dependencies". */

#define N00B_PICO_ALLOC ((n00b_allocator_t *)&n00b_get_runtime()->user_pool)

/* Phase 5 § 5.1 — metrics helpers; no-ops when no registry attached. */
static void
chan_metrics_on_open(n00b_quic_chan_t *chan)
{
    if (!chan || !chan->conn || !chan->conn->endpoint
        || !chan->conn->endpoint->metrics_registry) return;
    n00b_quic_endpoint_t *ep = chan->conn->endpoint;
    const char *kind_str =
        (chan->kind == N00B_QUIC_CHAN_BYTES) ? "bytes" :
        (chan->kind == N00B_QUIC_CHAN_DGRAM) ? "dgram" : "framed";
    n00b_list_t(n00b_buffer_t *) *lv = n00b_alloc(
        n00b_list_t(n00b_buffer_t *));
    *lv = n00b_list_new(n00b_buffer_t *);
    n00b_list_push(*lv, n00b_buffer_from_cstr(kind_str));
    if (ep->m_chan_opens_total) {
        n00b_quic_metric_counter_inc(ep->m_chan_opens_total, 1,
            .label_values = lv);
    }
    if (ep->m_chan_active) {
        int64_t now = atomic_fetch_add(&ep->_chan_active_counts[(int)chan->kind], 1)
                      + 1;
        n00b_quic_metric_gauge_set(ep->m_chan_active, (double)now,
            .label_values = lv);
    }
}

static void
chan_metrics_on_close(n00b_quic_chan_t *chan)
{
    if (!chan || !chan->conn || !chan->conn->endpoint
        || !chan->conn->endpoint->metrics_registry) return;
    n00b_quic_endpoint_t *ep = chan->conn->endpoint;
    if (!ep->m_chan_active) return;
    const char *kind_str =
        (chan->kind == N00B_QUIC_CHAN_BYTES) ? "bytes" :
        (chan->kind == N00B_QUIC_CHAN_DGRAM) ? "dgram" : "framed";
    n00b_list_t(n00b_buffer_t *) *lv = n00b_alloc(
        n00b_list_t(n00b_buffer_t *));
    *lv = n00b_list_new(n00b_buffer_t *);
    n00b_list_push(*lv, n00b_buffer_from_cstr(kind_str));
    int64_t now = atomic_fetch_sub(&ep->_chan_active_counts[(int)chan->kind], 1)
                  - 1;
    if (now < 0) now = 0;
    n00b_quic_metric_gauge_set(ep->m_chan_active, (double)now,
        .label_values = lv);
}

/* ===========================================================================
 * Open
 * =========================================================================== */

n00b_result_t(n00b_quic_chan_t *)
n00b_quic_chan_open(n00b_quic_conn_t *conn) _kargs
{
    n00b_quic_chan_kind_t kind     = N00B_QUIC_CHAN_FRAMED;
    bool                  bidi     = true;
    bool                  zero_rtt = false;
}
{
    n00b_allocator_scope_t pico_scope;

    if (!conn || conn->closed || !conn->cnx) {
        return n00b_result_err(n00b_quic_chan_t *,
                               N00B_QUIC_ERR_INVALID_ARG);
    }
    if (kind == N00B_QUIC_CHAN_DGRAM) {
        /* RFC 9221 datagrams have no per-channel demux header on the
         * wire, so a connection has at most one DGRAM channel.
         * Subsequent opens return the same handle (idempotent). */
        (void)bidi;
        (void)zero_rtt;
        return n00b_result_ok(n00b_quic_chan_t *,
                              _n00b_quic_conn_dgram_chan(conn));
    }
    /* zero_rtt is forward-compat surface; today it's a no-op. */
    (void)zero_rtt;

    n00b_allocator_t *alloc =
        (n00b_allocator_t *)&n00b_get_runtime()->conduit_pool;

    n00b_quic_chan_t *chan = n00b_alloc_with_opts(n00b_quic_chan_t,
                                &(n00b_alloc_opts_t){.allocator = alloc});

    chan->conn         = conn;
    chan->next_in_conn = nullptr;
    chan->kind         = kind;
    chan->bidi         = bidi;
    chan->state        = N00B_QUIC_CHAN_STATE_OPEN;
    chan->bytes_sent     = 0;
    chan->bytes_received = 0;
    chan->app_err_local  = 0;
    chan->app_err_peer   = 0;
    atomic_store(&chan->rx_inbox, (n00b_quic_rx_chunk_t *)nullptr);
    chan->rx_head        = nullptr;
    chan->rx_tail        = nullptr;
    chan->sent_fin       = false;
    chan->closed         = false;
    chan->last_activity_ns = (uint64_t)n00b_ns_timestamp();
#ifdef N00B_DEBUG
    atomic_store(&chan->rx_reader, 0);
#endif

    /* Take the picoquic lock for the stream-id allocation +
     * eager-create.  Two concurrent chan_open calls without this
     * lock observe the same "next stream id" peek and then race
     * inside picoquic_add_to_stream — the loser's stream gets a
     * stale id and is silently dropped on the wire. */
    n00b_data_write_lock(conn->endpoint->lock);
    pico_scope = n00b_allocator_scope_enter(N00B_PICO_ALLOC);
    chan->stream_id =
        picoquic_get_next_local_stream_id(conn->cnx,
                                         /*is_unidir*/ bidi ? 0 : 1);
    n00b_allocator_scope_exit(&pico_scope);

    /* picoquic_get_next_local_stream_id is a peek — it does not advance
     * the counter.  The counter advances when the stream is actually
     * created via picoquic_add_to_stream (or _with_ctx).  We open the
     * stream eagerly with a zero-byte add so two consecutive
     * chan_open calls get distinct stream IDs.  This is the canonical
     * picoquic pattern (see quicctx.c stream-id counter increment in
     * picoquic_create_stream). */
    int rc = 0;
    pico_scope = n00b_allocator_scope_enter(N00B_PICO_ALLOC);
    rc = picoquic_add_to_stream(conn->cnx, chan->stream_id,
                                nullptr, 0, /*set_fin*/ 0);
    n00b_allocator_scope_exit(&pico_scope);

    /* Register on the connection's channel list so the per-cnx callback
     * can route inbound stream events to us by stream ID.  The pump
     * registers peer streams under this same lock. */
    if (rc == 0) {
        _n00b_quic_conn_register_chan(conn, chan);
    }
    n00b_data_unlock(conn->endpoint->lock);
    if (rc != 0) {
        return n00b_result_err(n00b_quic_chan_t *,
                               N00B_QUIC_ERR_PROTOCOL);
    }

    chan_metrics_on_open(chan);
    return n00b_result_ok(n00b_quic_chan_t *, chan);
}

/* ===========================================================================
 * Accessors
 * =========================================================================== */

uint64_t
n00b_quic_chan_id(n00b_quic_chan_t *chan)
{
    return (chan && !chan->closed) ? chan->stream_id : UINT64_MAX;
}

n00b_quic_chan_kind_t
n00b_quic_chan_kind(n00b_quic_chan_t *chan)
{
    return chan ? chan->kind : N00B_QUIC_CHAN_FRAMED;
}

n00b_quic_chan_state_t
n00b_quic_chan_state(n00b_quic_chan_t *chan)
{
    return chan ? chan->state : N00B_QUIC_CHAN_STATE_CLOSED;
}

n00b_quic_conn_t *
n00b_quic_chan_conn(n00b_quic_chan_t *chan)
{
    return chan ? chan->conn : nullptr;
}

n00b_quic_stream_stats_t
n00b_quic_chan_stats(n00b_quic_chan_t *chan)
{
    n00b_quic_stream_stats_t s = {0};
    if (!chan) return s;
    s.bytes_sent       = chan->bytes_sent;
    s.bytes_received   = chan->bytes_received;
    s.last_activity_ns = chan->last_activity_ns;
    s.app_err_local    = chan->app_err_local;
    s.app_err_peer     = chan->app_err_peer;
    s.state            = (uint8_t)chan->state;
    s.kind             = (uint8_t)chan->kind;
    s.bidi             = chan->bidi ? 1 : 0;

    /* Per-stream send / recv windows + bytes_in_flight come from
     * picoquic's stream_head_t (an internal-header type).
     *
     *   send_window     = maxdata_remote - sent_offset
     *     (peer's flow-control limit minus what we've already sent)
     *   recv_window     = maxdata_local  - consumed_offset
     *     (our flow-control limit minus what the app has consumed)
     *   bytes_in_flight = sent_offset - sack_list cumulative ack
     *     (sent bytes the peer hasn't yet acked)
     *
     * Lookup goes through picoquic_find_stream(); returns NULL if
     * picoquic has already torn down the stream (post-close /
     * post-reset), in which case the stats stay zero. */
    if (chan->conn && !chan->conn->closed && chan->conn->cnx
        && !chan->closed) {
        picoquic_stream_head_t *st =
            picoquic_find_stream(chan->conn->cnx, chan->stream_id);
        if (st) {
            s.send_window = (st->maxdata_remote > st->sent_offset)
                                ? (st->maxdata_remote - st->sent_offset)
                                : 0;
            s.recv_window = (st->maxdata_local > st->consumed_offset)
                                ? (st->maxdata_local - st->consumed_offset)
                                : 0;
            /* bytes_in_flight: cumulative-acked offset is the high
             * end of the sack_list's first (lowest-offset) range,
             * which represents the contiguous prefix acked so far.
             * Anything between that and sent_offset is in flight. */
            uint64_t acked_to = picoquic_sack_list_first(&st->sack_list);
            if (acked_to == UINT64_MAX) {
                acked_to = 0;
            }
            else {
                acked_to += 1;
            }
            s.bytes_acked     = acked_to;
            s.bytes_in_flight = (st->sent_offset > acked_to)
                                    ? (st->sent_offset - acked_to)
                                    : 0;
        }
    }

    /* write_blocked_us: requires a write-blocked timestamp we don't
     * track yet; cumulative write-blocked duration sits on the chan
     * struct as a future addition.  Field stays 0 until then. */
    s.write_blocked_us = 0;
    return s;
}

n00b_quic_chan_t *
n00b_quic_chan_next_in_conn(n00b_quic_chan_t *chan)
{
    return chan ? chan->next_in_conn : nullptr;
}

/* ===========================================================================
 * Server-side accept (internal): wrap a peer-initiated stream.
 *
 * Unlike chan_open, we do NOT call picoquic_add_to_stream to reserve
 * the ID — the stream already exists in picoquic because the peer
 * sent a STREAM frame.  We just allocate our wrapper, infer bidi
 * from the stream-ID parity (RFC 9000 §2.1), and register on the
 * conn's channel list.
 * =========================================================================== */

n00b_quic_chan_t *
_n00b_quic_chan_accept_internal(n00b_quic_conn_t *conn, uint64_t stream_id)
{
    if (!conn || conn->closed) {
        return nullptr;
    }
    n00b_allocator_t *alloc =
        (n00b_allocator_t *)&n00b_get_runtime()->conduit_pool;

    n00b_quic_chan_t *chan = n00b_alloc_with_opts(n00b_quic_chan_t,
                                &(n00b_alloc_opts_t){.allocator = alloc});

    /* RFC 9000 §2.1: stream-ID bit 1 distinguishes uni (1) from bidi
     * (0).  Bit 0 distinguishes server-initiated (1) from
     * client-initiated (0). */
    bool bidi = (stream_id & 0x2u) == 0;

    chan->conn         = conn;
    chan->next_in_conn = nullptr;
    chan->stream_id    = stream_id;
    chan->kind         = N00B_QUIC_CHAN_FRAMED;
    chan->bidi         = bidi;
    chan->state        = N00B_QUIC_CHAN_STATE_OPEN;
    chan->bytes_sent   = 0;
    chan->bytes_received = 0;
    chan->app_err_local  = 0;
    chan->app_err_peer   = 0;
    atomic_store(&chan->rx_inbox, (n00b_quic_rx_chunk_t *)nullptr);
    chan->rx_head        = nullptr;
    chan->rx_tail        = nullptr;
    chan->sent_fin       = false;
    chan->closed         = false;
    chan->last_activity_ns = (uint64_t)n00b_ns_timestamp();
#ifdef N00B_DEBUG
    atomic_store(&chan->rx_reader, 0);
#endif

    _n00b_quic_conn_register_chan(conn, chan);
    return chan;
}

/* ===========================================================================
 * Send
 * =========================================================================== */

n00b_result_t(size_t)
n00b_quic_chan_send(n00b_quic_chan_t *chan,
                    const uint8_t    *payload,
                    size_t            len) _kargs
{
    bool fin = false;
}
{
    n00b_allocator_scope_t pico_scope;

    if (!chan || chan->closed || !chan->conn || chan->conn->closed ||
        !chan->conn->cnx) {
        return n00b_result_err(size_t, N00B_QUIC_ERR_INVALID_ARG);
    }
    if (chan->state == N00B_QUIC_CHAN_STATE_LOCAL_RESET ||
        chan->state == N00B_QUIC_CHAN_STATE_PEER_RESET ||
        chan->state == N00B_QUIC_CHAN_STATE_CLOSED) {
        return n00b_result_err(size_t, N00B_QUIC_ERR_INVALID_ARG);
    }
    if (chan->sent_fin) {
        /* Already finned this direction; can't send more. */
        return n00b_result_err(size_t, N00B_QUIC_ERR_INVALID_ARG);
    }
    if (!payload && len > 0) {
        return n00b_result_err(size_t, N00B_QUIC_ERR_NULL_ARG);
    }

    /* DGRAM path: one datagram per send call.  FIN is meaningless for
     * datagrams (no logical EOF — they're fire-and-forget); ignore. */
    if (chan->kind == N00B_QUIC_CHAN_DGRAM) {
        (void)fin;
        n00b_data_write_lock(chan->conn->endpoint->lock);
        int drc = 0;
        pico_scope = n00b_allocator_scope_enter(N00B_PICO_ALLOC);
        drc = picoquic_queue_datagram_frame(chan->conn->cnx, len, payload);
        n00b_allocator_scope_exit(&pico_scope);
        n00b_data_unlock(chan->conn->endpoint->lock);
        if (drc != 0) {
            /* picoquic returns PICOQUIC_ERROR_DATAGRAM_TOO_LONG when
             * the payload exceeds the negotiated frame size, or other
             * errors when datagrams aren't negotiated. */
            return n00b_result_err(size_t, N00B_QUIC_ERR_FRAME_TOO_LARGE);
        }
        chan->bytes_sent       += len;
        chan->last_activity_ns  = (uint64_t)n00b_ns_timestamp();
        return n00b_result_ok(size_t, len);
    }

    n00b_data_write_lock(chan->conn->endpoint->lock);
    int rc = 0;
    pico_scope = n00b_allocator_scope_enter(N00B_PICO_ALLOC);
    rc = picoquic_add_to_stream(chan->conn->cnx,
                                chan->stream_id,
                                payload,
                                len,
                                fin ? 1 : 0);
    n00b_allocator_scope_exit(&pico_scope);
    n00b_data_unlock(chan->conn->endpoint->lock);
    if (rc != 0) {
        return n00b_result_err(size_t, N00B_QUIC_ERR_FLOW_BLOCKED);
    }

    chan->bytes_sent       += len;
    chan->last_activity_ns  = (uint64_t)n00b_ns_timestamp();
    if (fin) {
        chan->sent_fin = true;
        (void)_n00b_quic_chan_transition(chan, N00B_QUIC_CHAN_EV_LOCAL_FIN);
    }
    return n00b_result_ok(size_t, len);
}

/* ===========================================================================
 * State transitions
 * =========================================================================== */

static n00b_quic_chan_state_t
chan_next_state(n00b_quic_chan_state_t cur, n00b_quic_chan_event_t ev)
{
    switch (ev) {
    case N00B_QUIC_CHAN_EV_LOCAL_FIN:
        if (cur == N00B_QUIC_CHAN_STATE_RECV_HALF_CLOSED) {
            return N00B_QUIC_CHAN_STATE_CLOSED;
        }
        if (cur == N00B_QUIC_CHAN_STATE_OPEN) {
            return N00B_QUIC_CHAN_STATE_SEND_HALF_CLOSED;
        }
        return cur;
    case N00B_QUIC_CHAN_EV_PEER_FIN:
        if (cur == N00B_QUIC_CHAN_STATE_SEND_HALF_CLOSED) {
            return N00B_QUIC_CHAN_STATE_CLOSED;
        }
        if (cur == N00B_QUIC_CHAN_STATE_OPEN) {
            return N00B_QUIC_CHAN_STATE_RECV_HALF_CLOSED;
        }
        return cur;
    case N00B_QUIC_CHAN_EV_LOCAL_RESET:
        return N00B_QUIC_CHAN_STATE_LOCAL_RESET;
    case N00B_QUIC_CHAN_EV_PEER_RESET:
        return N00B_QUIC_CHAN_STATE_PEER_RESET;
    case N00B_QUIC_CHAN_EV_CLOSE:
        if (cur == N00B_QUIC_CHAN_STATE_LOCAL_RESET || cur == N00B_QUIC_CHAN_STATE_PEER_RESET) {
            return cur;
        }
        return N00B_QUIC_CHAN_STATE_CLOSED;
    }
    return cur;
}

n00b_quic_chan_state_t
_n00b_quic_chan_transition(n00b_quic_chan_t *chan, n00b_quic_chan_event_t ev)
{
    n00b_quic_chan_state_t cur = atomic_load(&chan->state);
    n00b_quic_chan_state_t next;

    do {
        next = chan_next_state(cur, ev);
        if (next == cur) {
            return cur;
        }
        N00B_QUIC_DEBUG_POINT(chan->conn, chan, N00B_QUIC_DEBUG_TRANSITION);
    } while (!atomic_compare_exchange_weak(&chan->state, &cur, next));
    return next;
}

/* ===========================================================================
 * Recv — internal append + public pull
 * =========================================================================== */

#ifdef N00B_DEBUG
void (*_n00b_quic_debug_hook)(n00b_quic_conn_t       *conn,
                              n00b_quic_chan_t       *chan,
                              n00b_quic_debug_point_t point) = nullptr;

// Holds the id of the thread inside a reader call, so a second reader on
// another thread trips the assert.  Returns whether this call took it.
static bool
rx_reader_enter(n00b_quic_chan_t *chan)
{
    int64_t self = (int64_t)n00b_self_os_id();
    int64_t held = 0;

    if (atomic_compare_exchange_strong(&chan->rx_reader, &held, self)) {
        return true;
    }
    n00b_assert(held == self && "a QUIC channel allows one reader at a time");
    return false;
}

static void
rx_reader_exit(n00b_quic_chan_t *chan, bool entered)
{
    if (entered) {
        atomic_store(&chan->rx_reader, 0);
    }
}

#define RX_READER_ENTER(chan) bool rx_entered_ = rx_reader_enter(chan)
#define RX_READER_EXIT(chan)  rx_reader_exit((chan), rx_entered_)
#else
#define RX_READER_ENTER(chan) ((void)0)
#define RX_READER_EXIT(chan)  ((void)0)
#endif

static n00b_quic_rx_chunk_t *
rx_chunk_new(const uint8_t *bytes, size_t len)
{
    n00b_allocator_t     *alloc = (n00b_allocator_t *)&n00b_get_runtime()->conduit_pool;
    n00b_quic_rx_chunk_t *chunk = (n00b_quic_rx_chunk_t *)n00b_alloc_array_with_opts(
        uint8_t,
        sizeof(n00b_quic_rx_chunk_t) + len,
        &(n00b_alloc_opts_t){.allocator = alloc});

    chunk->next = nullptr;
    chunk->len  = len;
    chunk->off  = 0;
    chunk->fin  = false;
    if (bytes) {
        memcpy(chunk->bytes, bytes, len);
    }
    else {
        memset(chunk->bytes, 0, len);
    }
    return chunk;
}

static void
rx_publish(n00b_quic_chan_t *chan, n00b_quic_rx_chunk_t *chunk)
{
    n00b_quic_rx_chunk_t *head = atomic_load_explicit(&chan->rx_inbox, memory_order_relaxed);
    do {
        chunk->next = head;
    } while (!atomic_compare_exchange_weak_explicit(&chan->rx_inbox,
                                                    &head,
                                                    chunk,
                                                    memory_order_release,
                                                    memory_order_relaxed));
}

void
_n00b_quic_chan_append_recv(n00b_quic_chan_t *chan,
                            const uint8_t    *bytes,
                            size_t            len)
{
    if (!chan || len == 0) {
        return;
    }

    n00b_quic_rx_chunk_t *chunk = rx_chunk_new(bytes, len);
    N00B_QUIC_DEBUG_POINT(chan->conn, chan, N00B_QUIC_DEBUG_PUBLISH);
    rx_publish(chan, chunk);

    chan->bytes_received   += len;
    chan->last_activity_ns  = (uint64_t)n00b_ns_timestamp();
}

void
_n00b_quic_chan_append_fin(n00b_quic_chan_t *chan)
{
    if (!chan) {
        return;
    }

    n00b_quic_rx_chunk_t *chunk = rx_chunk_new(nullptr, 0);
    chunk->fin                  = true;
    rx_publish(chan, chunk);
}

// Move everything the pump has published onto the reader's list, oldest
// first.  Reader-only.
static void
rx_collect(n00b_quic_chan_t *chan)
{
    n00b_quic_rx_chunk_t *newest = atomic_exchange_explicit(&chan->rx_inbox,
                                                            (n00b_quic_rx_chunk_t *)nullptr,
                                                            memory_order_acquire);
    n00b_quic_rx_chunk_t *oldest = nullptr;

    while (newest != nullptr) {
        n00b_quic_rx_chunk_t *next = newest->next;
        newest->next               = oldest;
        oldest                     = newest;
        newest                     = next;
    }
    if (oldest == nullptr) {
        return;
    }
    if (chan->rx_tail != nullptr) {
        chan->rx_tail->next = oldest;
    }
    else {
        chan->rx_head = oldest;
    }
    while (oldest->next != nullptr) {
        oldest = oldest->next;
    }
    chan->rx_tail = oldest;
}

// True when the reader's next chunk holds bytes rather than the FIN.
// Reader-only.
static bool
rx_has_bytes(n00b_quic_chan_t *chan)
{
    rx_collect(chan);
    return chan->rx_head != nullptr && !chan->rx_head->fin;
}

void
_n00b_quic_chan_unread(n00b_quic_chan_t *chan, const uint8_t *bytes, size_t len)
{
    if (!chan || len == 0) {
        return;
    }

    RX_READER_ENTER(chan);
    n00b_quic_rx_chunk_t *chunk = rx_chunk_new(bytes, len);
    chunk->next                 = chan->rx_head;
    chan->rx_head               = chunk;
    if (chan->rx_tail == nullptr) {
        chan->rx_tail = chunk;
    }
    RX_READER_EXIT(chan);
}

n00b_result_t(size_t)
n00b_quic_chan_recv(n00b_quic_chan_t *chan, uint8_t *out, size_t max)
{
    if (!chan || chan->closed) {
        return n00b_result_err(size_t, N00B_QUIC_ERR_INVALID_ARG);
    }
    if (chan->kind == N00B_QUIC_CHAN_DGRAM) {
        /* DGRAM is atomic — bytes can't be flattened into a stream.
         * Caller must use n00b_quic_chan_recv_dgram. */
        return n00b_result_err(size_t, N00B_QUIC_ERR_INVALID_ARG);
    }
    if (max == 0) {
        return n00b_result_ok(size_t, 0);
    }
    if (!out) {
        return n00b_result_err(size_t, N00B_QUIC_ERR_NULL_ARG);
    }

    RX_READER_ENTER(chan);
    rx_collect(chan);
    N00B_QUIC_DEBUG_POINT(chan->conn, chan, N00B_QUIC_DEBUG_READ);

    size_t n = 0;
    while (n < max && chan->rx_head != nullptr && !chan->rx_head->fin) {
        n00b_quic_rx_chunk_t *chunk = chan->rx_head;
        size_t                take  = chunk->len - chunk->off;
        if (take > max - n) {
            take = max - n;
        }
        memcpy(out + n, chunk->bytes + chunk->off, take);
        chunk->off += take;
        n          += take;
        if (chunk->off == chunk->len) {
            chan->rx_head = chunk->next;
            if (chan->rx_head == nullptr) {
                chan->rx_tail = nullptr;
            }
            n00b_free(chunk);
        }
    }
    RX_READER_EXIT(chan);
    return n00b_result_ok(size_t, n);
}

bool
n00b_quic_chan_has_data(n00b_quic_chan_t *chan)
{
    if (!chan || chan->closed) {
        return false;
    }

    RX_READER_ENTER(chan);
    bool has = rx_has_bytes(chan);
    RX_READER_EXIT(chan);
    return has;
}

/* ===========================================================================
 * Datagram recv path
 *
 * Datagrams are atomic — each picoquic_callback_datagram delivery is
 * one whole payload.  We allocate a buffer with a copy of the bytes
 * (the picoquic callback's pointer is borrowed, valid only for the
 * duration of the call) and push it onto the channel's dgram queue.
 * The application drains via n00b_quic_chan_recv_dgram.
 * =========================================================================== */

void
_n00b_quic_chan_push_dgram(n00b_quic_chan_t *chan,
                           const uint8_t    *bytes,
                           size_t            len)
{
    if (!chan || chan->kind != N00B_QUIC_CHAN_DGRAM
        || !chan->dgram_queue_inited) {
        return;
    }
    n00b_allocator_t *alloc =
        (n00b_allocator_t *)&n00b_get_runtime()->conduit_pool;
    n00b_buffer_t *buf = n00b_buffer_from_bytes((char *)bytes,
                                                (int64_t)len,
                                                .allocator = alloc);
    n00b_list_push(chan->dgram_recv_queue, buf);
    chan->bytes_received   += len;
    chan->last_activity_ns  = (uint64_t)n00b_ns_timestamp();
}

n00b_result_t(n00b_option_t(n00b_buffer_t *))
n00b_quic_chan_recv_dgram(n00b_quic_chan_t *chan)
{
    if (!chan || chan->closed || chan->kind != N00B_QUIC_CHAN_DGRAM
        || !chan->dgram_queue_inited) {
        return n00b_result_err(n00b_option_t(n00b_buffer_t *),
                               N00B_QUIC_ERR_INVALID_ARG);
    }
    if (n00b_list_len(chan->dgram_recv_queue) == 0) {
        return n00b_result_ok(n00b_option_t(n00b_buffer_t *),
                              n00b_option_none(n00b_buffer_t *));
    }
    n00b_option_t(n00b_buffer_t *) popped =
        n00b_list_pop(n00b_buffer_t *, chan->dgram_recv_queue);
    return n00b_result_ok(n00b_option_t(n00b_buffer_t *), popped);
}

bool
n00b_quic_chan_recv_fin(n00b_quic_chan_t *chan)
{
    if (!chan) {
        return false;
    }

    RX_READER_ENTER(chan);
    rx_collect(chan);
    bool fin = chan->rx_head != nullptr && chan->rx_head->fin;
    RX_READER_EXIT(chan);
    return fin;
}

/* ===========================================================================
 * Reset / stop_sending / close
 * =========================================================================== */

n00b_result_t(bool)
n00b_quic_chan_reset(n00b_quic_chan_t *chan, uint64_t app_err)
{
    n00b_allocator_scope_t pico_scope;

    if (!chan || chan->closed || !chan->conn || !chan->conn->cnx) {
        return n00b_result_err(bool, N00B_QUIC_ERR_INVALID_ARG);
    }
    n00b_data_write_lock(chan->conn->endpoint->lock);
    int rc = 0;
    pico_scope = n00b_allocator_scope_enter(N00B_PICO_ALLOC);
    rc = picoquic_reset_stream(chan->conn->cnx, chan->stream_id, app_err);
    n00b_allocator_scope_exit(&pico_scope);
    n00b_data_unlock(chan->conn->endpoint->lock);
    if (rc != 0) {
        return n00b_result_err(bool, N00B_QUIC_ERR_PROTOCOL);
    }
    chan->app_err_local = app_err;
    (void)_n00b_quic_chan_transition(chan, N00B_QUIC_CHAN_EV_LOCAL_RESET);
    return n00b_result_ok(bool, true);
}

n00b_result_t(bool)
n00b_quic_chan_stop_sending(n00b_quic_chan_t *chan, uint64_t app_err)
{
    n00b_allocator_scope_t pico_scope;

    if (!chan || chan->closed || !chan->conn || !chan->conn->cnx) {
        return n00b_result_err(bool, N00B_QUIC_ERR_INVALID_ARG);
    }
    n00b_data_write_lock(chan->conn->endpoint->lock);
    int rc = 0;
    pico_scope = n00b_allocator_scope_enter(N00B_PICO_ALLOC);
    rc = picoquic_stop_sending(chan->conn->cnx, chan->stream_id, app_err);
    n00b_allocator_scope_exit(&pico_scope);
    n00b_data_unlock(chan->conn->endpoint->lock);
    if (rc != 0) {
        return n00b_result_err(bool, N00B_QUIC_ERR_PROTOCOL);
    }
    return n00b_result_ok(bool, true);
}

void
n00b_quic_chan_close(n00b_quic_chan_t *chan)
{
    n00b_allocator_scope_t pico_scope;

    if (!chan || chan->closed) {
        return;
    }
    /* Send FIN if we haven't already; ignore failures (the connection
     * may have been torn down underneath us). */
    if (!chan->sent_fin && chan->conn && !chan->conn->closed && chan->conn->cnx) {
        n00b_data_write_lock(chan->conn->endpoint->lock);
        pico_scope = n00b_allocator_scope_enter(N00B_PICO_ALLOC);
        (void)picoquic_add_to_stream(chan->conn->cnx, chan->stream_id,
                                     nullptr, 0, /*set_fin=*/1);
        n00b_allocator_scope_exit(&pico_scope);
        n00b_data_unlock(chan->conn->endpoint->lock);
        chan->sent_fin = true;
    }
    chan->closed = true;
    (void)_n00b_quic_chan_transition(chan, N00B_QUIC_CHAN_EV_CLOSE);
    chan_metrics_on_close(chan);
}
