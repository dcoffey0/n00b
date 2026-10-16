/**
 * @file conn_internal.h
 * @internal
 * @brief Layout of `n00b_quic_conn_t`.
 */
#pragma once

#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include "n00b.h"
#include "net/quic/quic_types.h"

/* Forward decls — picoquic headers are heavy; consumers include them. */
typedef struct st_picoquic_cnx_t picoquic_cnx_t;

struct n00b_quic_conn {
    n00b_quic_endpoint_t *endpoint;     /**< Owning endpoint; borrowed. */
    picoquic_cnx_t       *cnx;          /**< picoquic connection context. */
    /* Head of the intrusive channel list.  Channels are only ever pushed,
     * under endpoint->lock; readers walk it without a lock. */
    _Atomic(n00b_quic_chan_t *) channels;
    n00b_quic_conn_t     *next_in_endpoint; /**< Server: link in endpoint->accepted list. */
    uint64_t              app_err;      /**< Last app close code we sent. */
    /* Close-reason metadata, captured when picoquic_callback_close /
     * application_close / stateless_reset fires.  Read via picoquic
     * accessors (picoquic_get_close_reasons / picoquic_get_local_error
     * / picoquic_get_remote_error / picoquic_get_application_error).
     * Set once on the close event so the app can inspect after the
     * fact.  See n00b_quic_conn_close_info().  */
    uint64_t              local_reason;
    uint64_t              remote_reason;
    uint64_t              local_app_reason;
    uint64_t              remote_app_reason;
    /* Caller-supplied close reason phrase from n00b_quic_close(.reason).
     * Copied at close-call time so the original string can be freed. */
    n00b_string_t        *local_reason_phrase;
    /* Source of the close (which callback fired it).  Lets the app
     * distinguish "we closed", "peer closed gracefully", "peer
     * stateless-reset". */
    n00b_quic_close_source_t close_source;
    /* Singleton DGRAM channel for this connection.  RFC 9221
     * datagrams have no demux header, so the conn has at most one
     * DGRAM channel.  Lazily created the first time the app opens a
     * DGRAM channel OR the first time the peer sends a datagram. */
    _Atomic(n00b_quic_chan_t *) dgram_chan;
    bool                  client_mode;  /**< True for outbound; false for accepted. */
    bool                  closed;       /**< Local close called. */
};

/**
 * @internal
 * @brief Get-or-create the singleton DGRAM channel for this connection.
 *
 * Returns the existing one if already created; otherwise allocates,
 * registers on the conn's channel list, and returns it.  The stream_id
 * field is set to `UINT64_MAX` (sentinel — datagrams have no stream).
 */
extern n00b_quic_chan_t *
_n00b_quic_conn_dgram_chan(n00b_quic_conn_t *conn);

/**
 * @internal
 * @brief Look up a channel on this connection by stream ID.
 * @return The channel, or NULL if no open channel matches.
 */
extern n00b_quic_chan_t *
_n00b_quic_conn_find_chan(n00b_quic_conn_t *conn, uint64_t stream_id);

/**
 * @internal
 * @brief Register a freshly opened channel onto its connection's list.
 *
 * The caller holds the connection's endpoint lock.
 */
extern void
_n00b_quic_conn_register_chan(n00b_quic_conn_t *conn, n00b_quic_chan_t *chan);

/**
 * @internal
 * @brief Wrap a picoquic-created server-side cnx as an n00b_quic_conn_t.
 *
 * Used by the endpoint default callback to wrap a freshly-accepted
 * picoquic connection without going through `picoquic_create_cnx`
 * (which is the client path).  Installs `conn_default_callback` on
 * the cnx via `picoquic_set_callback` so subsequent events route
 * through our channel dispatch.
 *
 * @param ep   Owning endpoint.
 * @param cnx  picoquic connection (already created by picoquic).
 *
 * @return The new n00b_quic_conn_t, or NULL on alloc failure.
 */
extern n00b_quic_conn_t *
_n00b_quic_conn_accept_internal(n00b_quic_endpoint_t *ep, picoquic_cnx_t *cnx);

/**
 * @internal
 * @brief Construct a server-side `n00b_quic_chan_t` for a peer-initiated
 *        stream — picoquic already knows about the stream, so we don't
 *        call `picoquic_add_to_stream` to reserve it.
 *
 * @param conn        Owning connection (server-side).
 * @param stream_id   Stream ID picked by the peer.
 *
 * @return The new chan, owned by the conn's channel list.  NULL on
 *         allocation failure.
 */
extern n00b_quic_chan_t *
_n00b_quic_chan_accept_internal(n00b_quic_conn_t *conn, uint64_t stream_id);

/* Forward to the picoquic per-cnx callback type so endpoint.c can
 * invoke it after wrapping a new server-accepted cnx.  Pulling in
 * picoquic.h here lets us match the signature exactly. */
#include "picoquic.h"

/**
 * @internal
 * @brief Per-connection picoquic callback.  Exposed so the endpoint's
 *        accept-default callback can forward the very first event on
 *        a freshly-wrapped server-side cnx.
 */
extern int
_n00b_quic_conn_default_callback(picoquic_cnx_t              *cnx,
                                 uint64_t                     stream_id,
                                 uint8_t                     *bytes,
                                 size_t                       length,
                                 picoquic_call_back_event_t   event,
                                 void                        *callback_ctx,
                                 void                        *stream_ctx);
