/*
 * conn.c — QUIC connection wrapper around picoquic_cnx_t.
 *
 * Phase 1 ships outbound `n00b_quic_connect`, ordered close, state +
 * stats accessors.  Inbound accept lands with the secret-to-picotls
 * bridge.
 */

#define N00B_USE_INTERNAL_API
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/time.h"
#include "core/string.h"
#include "core/data_lock.h"
#include "net/quic/quic_types.h"
#include "net/quic/conn.h"
#include "net/quic/chan.h"
#include "net/quic/endpoint.h"
#include "internal/net/quic/conn_internal.h"
#include "internal/net/quic/chan_internal.h"
#include "internal/net/quic/endpoint_internal.h"
#include "internal/crypto/picotls_sni.h"

#include "picoquic.h"
#include "picoquic_utils.h"

#define N00B_PICO_ALLOC ((n00b_allocator_t *)&n00b_get_runtime()->user_pool)

/* ===========================================================================
 * Per-cnx callback — dispatch picoquic events into our channel state machine.
 *
 * picoquic invokes this from inside the run-once loop (single-threaded with
 * the rest of the n00b code path).  For each STREAM_DATA / STREAM_FIN /
 * STREAM_RESET / STOP_SENDING event we look up the corresponding
 * `n00b_quic_chan_t` on the conn's intrusive channel list and update the
 * channel state.  Connection-level events (close, application_close,
 * almost_ready, ready) are observed but require no channel-level action;
 * the conn state accessor reads picoquic's own state to surface those.
 * =========================================================================== */

/* Public-internal: visible to endpoint.c so the endpoint default
 * callback can forward the very first event on a server-accepted cnx
 * here (after wrapping the cnx and installing this as the per-cnx
 * callback). */
int
_n00b_quic_conn_default_callback(picoquic_cnx_t              *cnx,
                                 uint64_t                     stream_id,
                                 uint8_t                     *bytes,
                                 size_t                       length,
                                 picoquic_call_back_event_t   event,
                                 void                        *callback_ctx,
                                 void                        *stream_ctx)
{
    n00b_allocator_scope_t pico_scope;

    (void)cnx;
    (void)stream_ctx;

    n00b_quic_conn_t *conn = (n00b_quic_conn_t *)callback_ctx;
    if (!conn) {
        return 0;
    }

    switch (event) {
    case picoquic_callback_stream_data: {
        n00b_quic_chan_t *chan = _n00b_quic_conn_find_chan(conn, stream_id);
        if (!chan) {
            /* RFC 9000 § 2.1 — bit 0 of the stream id distinguishes the
             * initiator (0 = client, 1 = server).  Auto-wrap any stream
             * the *peer* initiates.  We never auto-wrap streams *we*
             * initiated; those must be opened via n00b_quic_chan_open.
             * H3 (RFC 9114 § 6.2) requires the client side to receive
             * server-initiated uni streams (CONTROL + QPACK enc/dec). */
            bool peer_initiated = ((stream_id & 0x1u) == 1u)
                                  ? conn->client_mode
                                  : !conn->client_mode;
            if (peer_initiated) {
                chan = _n00b_quic_chan_accept_internal(conn, stream_id);
            }
        }
        if (chan && !chan->closed) {
            _n00b_quic_chan_append_recv(chan, bytes, length);
        }
        break;
    }
    case picoquic_callback_stream_fin: {
        n00b_quic_chan_t *chan = _n00b_quic_conn_find_chan(conn, stream_id);
        if (!chan) {
            bool peer_initiated = ((stream_id & 0x1u) == 1u)
                                  ? conn->client_mode
                                  : !conn->client_mode;
            if (peer_initiated) {
                chan = _n00b_quic_chan_accept_internal(conn, stream_id);
            }
        }
        if (chan && !chan->closed) {
            if (length > 0) {
                _n00b_quic_chan_append_recv(chan, bytes, length);
            }
            _n00b_quic_chan_append_fin(chan);
            (void)_n00b_quic_chan_transition(chan, N00B_QUIC_CHAN_EV_PEER_FIN);
        }
        break;
    }
    case picoquic_callback_stream_reset: {
        n00b_quic_chan_t *chan = _n00b_quic_conn_find_chan(conn, stream_id);
        if (chan && !chan->closed) {
            /* picoquic exposes the peer's RESET_STREAM application
             * error code via the per-stream accessor.  Stash it so
             * the application can inspect why the peer reset. */
            chan->app_err_peer = picoquic_get_remote_stream_error(cnx,
                                                                  stream_id);
            (void)_n00b_quic_chan_transition(chan, N00B_QUIC_CHAN_EV_PEER_RESET);
        }
        break;
    }
    case picoquic_callback_stop_sending: {
        n00b_quic_chan_t *chan = _n00b_quic_conn_find_chan(conn, stream_id);
        if (chan && !chan->closed) {
            /* Peer asked us to stop sending.  Record their app error
             * code, reset our send side defensively. */
            chan->app_err_peer = picoquic_get_remote_stream_error(cnx,
                                                                  stream_id);
            pico_scope = n00b_allocator_scope_enter(N00B_PICO_ALLOC);
            (void)picoquic_reset_stream(cnx, stream_id, 0);
            n00b_allocator_scope_exit(&pico_scope);
            (void)_n00b_quic_chan_transition(chan, N00B_QUIC_CHAN_EV_LOCAL_RESET);
        }
        break;
    }
    case picoquic_callback_close:
    case picoquic_callback_application_close:
    case picoquic_callback_stateless_reset:
        /* Connection-level close.  Capture the close-reason metadata
         * before any side-effects so n00b_quic_conn_close_info() can
         * report it back to the application.  conn_state() reads
         * picoquic's state directly, so we don't mark conn->closed
         * here (that would race with the local close path).
         *
         * For server-side accepted conns: unlink from the endpoint's
         * accepted list so it doesn't leak.  Application code that
         * holds its own pointer to the conn (e.g., from popping the
         * accept inbox) keeps it valid via that reference; the GC
         * will reclaim once nothing else references it. */
        picoquic_get_close_reasons(cnx,
                                   &conn->local_reason,
                                   &conn->remote_reason,
                                   &conn->local_app_reason,
                                   &conn->remote_app_reason);
        conn->close_source =
            (event == picoquic_callback_stateless_reset)
                ? N00B_QUIC_CLOSE_SOURCE_STATELESS_RESET
                : (event == picoquic_callback_application_close)
                      ? N00B_QUIC_CLOSE_SOURCE_PEER_APPLICATION
                      : N00B_QUIC_CLOSE_SOURCE_PEER_TRANSPORT;
        if (!conn->client_mode && conn->endpoint) {
            n00b_quic_endpoint_t *ep = conn->endpoint;
            n00b_quic_conn_t **slot  = &ep->accepted;
            while (*slot && *slot != conn) {
                slot = &(*slot)->next_in_endpoint;
            }
            if (*slot == conn) {
                *slot = conn->next_in_endpoint;
                conn->next_in_endpoint = nullptr;
            }
        }

        /* Drop any per-cnx side-table entry held by the SNI router.
         * Routed via the endpoint's `sni_state` field (set only by
         * `n00b_quic_picotls_sni_install`); the cleanup is a no-op
         * when the field is NULL.  We deliberately do NOT introspect
         * `quic->tls_master_ctx->on_client_hello` to discover the
         * state — that path was unsafe because picoquic's default
         * fallback callback isn't our stub layout. */
        if (conn->endpoint && conn->endpoint->sni_state) {
            n00b_quic_picotls_sni_cleanup_cnx(conn->endpoint->sni_state, cnx);
        }
        break;

    case picoquic_callback_almost_ready:
    case picoquic_callback_ready:
        /* Handshake progress; surfaced through conn_state. */
        break;

    case picoquic_callback_datagram: {
        /* RFC 9221 datagram received.  `bytes` is borrowed for the
         * duration of the call; the push helper copies into a
         * conduit-pool buffer before returning. */
        n00b_quic_chan_t *dchan = _n00b_quic_conn_dgram_chan(conn);
        if (dchan && !dchan->closed) {
            _n00b_quic_chan_push_dgram(dchan, bytes, length);
        }
        break;
    }

    case picoquic_callback_datagram_acked:
    case picoquic_callback_datagram_lost:
    case picoquic_callback_datagram_spurious:
        /* TODO: feed into chan_stats for the DGRAM channel
         * (bytes_in_flight tracking).  Phase-1 surface doesn't
         * promise these counters yet. */
        break;

    default:
        /* prepare_to_send, path events, version negotiation, etc.
         * Not dispatched in this revision. */
        break;
    }
    return 0;
}

/* ===========================================================================
 * Per-conn channel list
 * =========================================================================== */

n00b_quic_chan_t *
_n00b_quic_conn_find_chan(n00b_quic_conn_t *conn, uint64_t stream_id)
{
    if (!conn) {
        return nullptr;
    }
    n00b_quic_chan_t *c;
    for (c = atomic_load_explicit(&conn->channels, memory_order_acquire); c != nullptr;
         c = c->next_in_conn) {
        if (c->stream_id == stream_id) {
            return c;
        }
    }
    return nullptr;
}

void
_n00b_quic_conn_register_chan(n00b_quic_conn_t *conn, n00b_quic_chan_t *chan)
{
    if (!conn || !chan) {
        return;
    }
    n00b_assert(!conn->endpoint || !conn->endpoint->lock
                || n00b_lock_already_owner((n00b_lock_base_t *)conn->endpoint->lock));

    chan->next_in_conn = atomic_load_explicit(&conn->channels, memory_order_relaxed);
    N00B_QUIC_DEBUG_POINT(conn, chan, N00B_QUIC_DEBUG_REGISTER);
    atomic_store_explicit(&conn->channels, chan, memory_order_release);
}

/* ===========================================================================
 * Singleton DGRAM channel
 *
 * RFC 9221 datagrams travel on the connection — there's no per-stream
 * multiplexing.  Modelling that as "one DGRAM channel per conn" keeps
 * the chan abstraction usable for datagrams without forcing the app
 * to invent its own demux header.  This getter is idempotent: first
 * call allocates and registers; subsequent calls return the same
 * handle.
 * =========================================================================== */

n00b_quic_chan_t *
_n00b_quic_conn_dgram_chan(n00b_quic_conn_t *conn)
{
    if (!conn) {
        return nullptr;
    }
    n00b_quic_chan_t *existing = atomic_load_explicit(&conn->dgram_chan, memory_order_acquire);
    if (existing) {
        return existing;
    }

    // Registration needs the endpoint lock.  The pump arrives holding it,
    // and n00b_quic_chan_open takes it here.
    n00b_rwlock_t *lock = conn->endpoint ? conn->endpoint->lock : nullptr;
    n00b_data_write_lock(lock);
    existing = atomic_load_explicit(&conn->dgram_chan, memory_order_relaxed);
    if (existing) {
        n00b_data_unlock(lock);
        return existing;
    }

    n00b_allocator_t *alloc =
        (n00b_allocator_t *)&n00b_get_runtime()->conduit_pool;

    n00b_quic_chan_t *chan = n00b_alloc_with_opts(
        n00b_quic_chan_t,
        &(n00b_alloc_opts_t){.allocator = alloc});

    chan->conn             = conn;
    chan->next_in_conn     = nullptr;
    chan->kind             = N00B_QUIC_CHAN_DGRAM;
    chan->state            = N00B_QUIC_CHAN_STATE_OPEN;
    /* Datagrams have no stream id.  UINT64_MAX is the sentinel; it's
     * outside the QUIC varint range (< 2^62) and so cannot collide
     * with a real stream id. */
    chan->stream_id        = UINT64_MAX;
    chan->bidi             = true;  /* Conceptually full-duplex. */
    chan->bytes_sent       = 0;
    chan->bytes_received   = 0;
    chan->app_err_local    = 0;
    chan->app_err_peer     = 0;
    atomic_store(&chan->rx_inbox, (n00b_quic_rx_chunk_t *)nullptr);
    chan->rx_head          = nullptr;
    chan->rx_tail          = nullptr;
    chan->dgram_recv_queue = n00b_list_new(n00b_buffer_t *, .allocator = alloc);
    chan->dgram_queue_inited = true;
    chan->sent_fin         = false;
    chan->closed           = false;
    chan->last_activity_ns = (uint64_t)n00b_ns_timestamp();
#ifdef N00B_DEBUG
    atomic_store(&chan->rx_reader, 0);
#endif

    _n00b_quic_conn_register_chan(conn, chan);
    atomic_store_explicit(&conn->dgram_chan, chan, memory_order_release);
    n00b_data_unlock(lock);
    return chan;
}

/* ===========================================================================
 * Connect (outbound)
 * =========================================================================== */

n00b_result_t(n00b_quic_conn_t *)
n00b_quic_connect(n00b_quic_endpoint_t  *ep,
                  const struct sockaddr *remote_addr,
                  n00b_string_t         *sni) _kargs
{
    int32_t        timeout_ms = 10000;
    n00b_string_t *alpn_pref  = nullptr;
    bool           zero_rtt   = false;
}
{
    n00b_allocator_scope_t pico_scope;

    if (!ep || ep->closed || !ep->quic) {
        return n00b_result_err(n00b_quic_conn_t *, N00B_QUIC_ERR_INVALID_ARG);
    }
    if (!remote_addr) {
        return n00b_result_err(n00b_quic_conn_t *, N00B_QUIC_ERR_NULL_ARG);
    }
    if (!sni || !sni->data) {
        return n00b_result_err(n00b_quic_conn_t *, N00B_QUIC_ERR_NULL_ARG);
    }

    /* timeout_ms is wired through to picoquic in a follow-up; today we
     * surface it on the public API for forward-compatibility but only
     * advise it.  zero_rtt similarly. */
    (void)timeout_ms;
    (void)zero_rtt;

    n00b_allocator_t *alloc =
        (n00b_allocator_t *)&n00b_get_runtime()->conduit_pool;

    n00b_quic_conn_t *conn = n00b_alloc_with_opts(n00b_quic_conn_t,
                                &(n00b_alloc_opts_t){.allocator = alloc});

    conn->endpoint    = ep;
    conn->client_mode = true;
    conn->closed      = false;
    conn->app_err     = 0;
    conn->channels    = nullptr;

    /* picoquic_start_client_cnx requires an ALPN on the per-cnx
     * basis (PICOQUIC_ERROR_NO_ALPN_PROVIDED otherwise).  The endpoint
     * caches its default ALPN at construction; use the per-connect
     * override if given, else fall back to the endpoint default. */
    const char *alpn = (alpn_pref && alpn_pref->data) ? alpn_pref->data
                                                      : ep->alpn;
    if (!alpn) {
        return n00b_result_err(n00b_quic_conn_t *, N00B_QUIC_ERR_INVALID_ARG);
    }

    char *sni_cstr = n00b_alloc_array_with_opts(
        char,
        (int64_t)sni->u8_bytes + 1,
        &(n00b_alloc_opts_t){
            .allocator = alloc,
            .scan_kind = N00B_GC_SCAN_KIND_NONE,
        });
    memcpy(sni_cstr, sni->data, sni->u8_bytes);
    sni_cstr[sni->u8_bytes] = '\0';

    /* Take the picoquic lock for the create+start sequence — these
     * mutate the endpoint's connection table and the new cnx's TLS
     * state alongside any concurrent endpoint_run_once call. */
    n00b_data_write_lock(ep->lock);

    uint64_t        now = (uint64_t)n00b_us_timestamp();
#if defined(__linux__)
    ep->pico_now_us = now;
#endif
    picoquic_cnx_t *cnx = nullptr;
    pico_scope = n00b_allocator_scope_enter(N00B_PICO_ALLOC);
    cnx = picoquic_create_cnx(
        ep->quic,
        picoquic_null_connection_id,
        picoquic_null_connection_id,
        (struct sockaddr *)remote_addr,
        now,
        /* proposed_version */ 0,
        sni_cstr,
        alpn,
        /* client_mode */ 1);
    n00b_allocator_scope_exit(&pico_scope);

    if (!cnx) {
        n00b_data_unlock(ep->lock);
        return n00b_result_err(n00b_quic_conn_t *, N00B_QUIC_ERR_HANDSHAKE);
    }

    /* Attach our conn to the picoquic_cnx_t so the per-cnx callback can
     * find us.  picoquic_set_callback also installs our callback in
     * place of the endpoint default for this connection. */
    pico_scope = n00b_allocator_scope_enter(N00B_PICO_ALLOC);
    picoquic_set_callback(cnx, _n00b_quic_conn_default_callback, conn);
    n00b_allocator_scope_exit(&pico_scope);

    int rc = 0;
    pico_scope = n00b_allocator_scope_enter(N00B_PICO_ALLOC);
    rc = picoquic_start_client_cnx(cnx);
    n00b_allocator_scope_exit(&pico_scope);
    n00b_data_unlock(ep->lock);
    if (rc != 0) {
        /* picoquic owns and will free `cnx` when its own context closes,
         * but we should not return a handle pointing at it. */
        conn->cnx    = nullptr;
        conn->closed = true;
        return n00b_result_err(n00b_quic_conn_t *, N00B_QUIC_ERR_HANDSHAKE);
    }

    conn->cnx = cnx;
    return n00b_result_ok(n00b_quic_conn_t *, conn);
}

/* ===========================================================================
 * Close
 * =========================================================================== */

void
n00b_quic_close(n00b_quic_conn_t *conn, uint64_t app_err) _kargs
{
    n00b_string_t *reason = nullptr;
}
{
    n00b_allocator_scope_t pico_scope;

    if (!conn || conn->closed || !conn->cnx) {
        return;
    }

    conn->app_err            = app_err;
    conn->local_app_reason   = app_err;
    conn->close_source       = N00B_QUIC_CLOSE_SOURCE_LOCAL;
    conn->local_reason_phrase = reason;  /* GC owns it; we just retain a ref. */
    conn->closed             = true;

    /* Use picoquic_close_ex when the caller supplied a reason string,
     * so the phrase rides along on the wire's CONNECTION_CLOSE frame
     * (peer can observe it via picoquic_get_close_reasons in their
     * close callback).  reason->data is NUL-terminated by
     * n00b_string contract; cap_len defends against an over-long
     * phrase that would exceed the QUIC frame size. */
    const char *phrase_c = nullptr;
    if (reason && reason->data && reason->u8_bytes > 0) {
        phrase_c = reason->data;
    }

    if (conn->endpoint) {
        n00b_data_write_lock(conn->endpoint->lock);
        if (phrase_c) {
            pico_scope = n00b_allocator_scope_enter(N00B_PICO_ALLOC);
            picoquic_close_ex(conn->cnx, app_err, phrase_c);
            n00b_allocator_scope_exit(&pico_scope);
        } else {
            pico_scope = n00b_allocator_scope_enter(N00B_PICO_ALLOC);
            picoquic_close(conn->cnx, app_err);
            n00b_allocator_scope_exit(&pico_scope);
        }
        n00b_data_unlock(conn->endpoint->lock);
    } else if (phrase_c) {
        pico_scope = n00b_allocator_scope_enter(N00B_PICO_ALLOC);
        picoquic_close_ex(conn->cnx, app_err, phrase_c);
        n00b_allocator_scope_exit(&pico_scope);
    } else {
        pico_scope = n00b_allocator_scope_enter(N00B_PICO_ALLOC);
        picoquic_close(conn->cnx, app_err);
        n00b_allocator_scope_exit(&pico_scope);
    }
}

n00b_quic_close_info_t
n00b_quic_conn_close_info(n00b_quic_conn_t *conn)
{
    n00b_quic_close_info_t info = {0};
    if (!conn) {
        return info;
    }
    info.source              = conn->close_source;
    info.local_reason        = conn->local_reason;
    info.remote_reason       = conn->remote_reason;
    info.local_app_reason    = conn->local_app_reason;
    info.remote_app_reason   = conn->remote_app_reason;
    info.local_reason_phrase = conn->local_reason_phrase;
    return info;
}

/* ===========================================================================
 * State + stats
 * =========================================================================== */

n00b_quic_conn_state_t
n00b_quic_conn_state(n00b_quic_conn_t *conn)
{
    if (!conn || !conn->cnx) {
        return N00B_QUIC_CONN_STATE_CLOSED;
    }
    picoquic_state_enum st = picoquic_get_cnx_state(conn->cnx);
    switch (st) {
    case picoquic_state_client_init:
    case picoquic_state_client_init_sent:
    case picoquic_state_client_renegotiate:
    case picoquic_state_client_retry_received:
    case picoquic_state_client_init_resent:
    case picoquic_state_server_init:
    case picoquic_state_server_handshake:
    case picoquic_state_client_handshake_start:
    case picoquic_state_client_almost_ready:
    case picoquic_state_server_false_start:
    case picoquic_state_server_almost_ready:
    case picoquic_state_client_ready_start:
        return N00B_QUIC_CONN_STATE_CONNECTING;
    case picoquic_state_ready:
        return N00B_QUIC_CONN_STATE_CONNECTED;
    case picoquic_state_disconnecting:
    case picoquic_state_closing_received:
    case picoquic_state_closing:
    case picoquic_state_draining:
        return N00B_QUIC_CONN_STATE_CLOSING;
    case picoquic_state_handshake_failure:
    case picoquic_state_handshake_failure_resend:
        return N00B_QUIC_CONN_STATE_FAILED;
    case picoquic_state_disconnected:
        return N00B_QUIC_CONN_STATE_CLOSED;
    }
    /* Unknown state — treat as failed defensively. */
    return N00B_QUIC_CONN_STATE_FAILED;
}

bool
n00b_quic_conn_is_closed(n00b_quic_conn_t *conn)
{
    return conn == nullptr || conn->closed;
}

n00b_quic_conn_stats_t
n00b_quic_conn_stats(n00b_quic_conn_t *conn)
{
    n00b_quic_conn_stats_t s = {0};
    if (!conn || !conn->cnx || conn->closed) {
        return s;
    }
    s.bytes_sent     = picoquic_get_data_sent(conn->cnx);
    s.bytes_received = picoquic_get_data_received(conn->cnx);

    /* Pull rich path/quality stats in one call.  Picoquic populates
     * RTT, congestion window, packet counters, and bytes-in-flight
     * here; we pass them through to operators verbatim. */
    picoquic_path_quality_t pq;
    memset(&pq, 0, sizeof(pq));
    picoquic_get_default_path_quality(conn->cnx, &pq);
    s.rtt_us          = pq.rtt;
    s.rttvar_us       = pq.rtt_variant;
    s.cwnd            = pq.cwin;
    s.bytes_in_flight = pq.bytes_in_transit;
    s.packets_sent    = pq.sent;
    s.packets_lost    = pq.lost;
    /* packets_received: picoquic doesn't expose a public per-cnx
     * accessor.  Fillable from picoquic_internal.h's stats counters
     * later; struct field stays 0 for now. */

    /* Channel counters: walk our intrusive list. */
    uint64_t chans_open  = 0;
    uint64_t chans_total = 0;
    n00b_quic_chan_t *c;
    for (c = atomic_load_explicit(&conn->channels, memory_order_acquire); c;
         c = c->next_in_conn) {
        chans_total++;
        n00b_quic_chan_state_t cst = c->state;
        if (cst == N00B_QUIC_CHAN_STATE_OPEN ||
            cst == N00B_QUIC_CHAN_STATE_SEND_HALF_CLOSED ||
            cst == N00B_QUIC_CHAN_STATE_RECV_HALF_CLOSED) {
            chans_open++;
        }
    }
    s.channels_open  = chans_open;
    s.channels_total = chans_total;

    /* CC algo: picoquic registers algorithms in the order they're
     * loaded; we hand back the configured default index here.  When
     * picoquic exposes a public accessor for "which CC is this cnx
     * actually using", swap in that. */
    s.cc_algo = N00B_QUIC_CC_NEWRENO;

    return s;
}

n00b_quic_endpoint_t *
n00b_quic_conn_endpoint(n00b_quic_conn_t *conn)
{
    return conn ? conn->endpoint : nullptr;
}

bool
n00b_quic_conn_remote_cid(n00b_quic_conn_t *conn, n00b_quic_cid_t *out)
{
    if (!conn || !out) return false;
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!conn->cnx) return false;
    picoquic_connection_id_t cid = picoquic_get_remote_cnxid(conn->cnx);
    size_t n = (size_t)cid.id_len;
    if (n == 0) return false;
    if (n > N00B_QUIC_MAX_CID_LEN) {
        /* picoquic shouldn't produce > 20-byte CIDs (RFC 9000 §5.1)
         * but defend against the bound anyway — silent truncation
         * is exactly the kind of footgun this signature was changed
         * to avoid. */
        return false;
    }
    memcpy(out->bytes, cid.id, n);
    out->len = n;
    return true;
}

n00b_quic_chan_t *
n00b_quic_conn_first_chan(n00b_quic_conn_t *conn)
{
    return conn ? atomic_load_explicit(&conn->channels, memory_order_acquire) : nullptr;
}

bool
n00b_quic_conn_peer_addr(n00b_quic_conn_t        *conn,
                         struct sockaddr_storage *out)
{
    if (!conn || !out || !conn->cnx) return false;
    struct sockaddr *peer = nullptr;
    picoquic_get_peer_addr(conn->cnx, &peer);
    if (!peer) return false;
    socklen_t alen = (peer->sa_family == AF_INET6)
                     ? (socklen_t)sizeof(struct sockaddr_in6)
                     : (socklen_t)sizeof(struct sockaddr_in);
    memcpy(out, peer, alen);
    return true;
}

/* ===========================================================================
 * Server-side accept (internal): wrap a picoquic-created cnx as ours.
 * =========================================================================== */

n00b_quic_conn_t *
_n00b_quic_conn_accept_internal(n00b_quic_endpoint_t *ep, picoquic_cnx_t *cnx)
{
    n00b_allocator_scope_t pico_scope;

    if (!ep || !cnx) {
        return nullptr;
    }
    n00b_allocator_t *alloc =
        (n00b_allocator_t *)&n00b_get_runtime()->conduit_pool;

    n00b_quic_conn_t *conn = n00b_alloc_with_opts(n00b_quic_conn_t,
                                &(n00b_alloc_opts_t){.allocator = alloc});

    conn->endpoint         = ep;
    conn->cnx              = cnx;
    conn->channels         = nullptr;
    conn->next_in_endpoint = nullptr;
    conn->client_mode      = false;
    conn->closed           = false;
    conn->app_err          = 0;

    /* Install our per-cnx callback so subsequent events on this cnx
     * route through conn_default_callback (which knows how to dispatch
     * to channels) instead of the endpoint's accept-default. */
    pico_scope = n00b_allocator_scope_enter(N00B_PICO_ALLOC);
    picoquic_set_callback(cnx, _n00b_quic_conn_default_callback, conn);
    n00b_allocator_scope_exit(&pico_scope);
    return conn;
}
