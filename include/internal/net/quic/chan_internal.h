/**
 * @file chan_internal.h
 * @internal
 * @brief Layout of `n00b_quic_chan_t`.
 */
#pragma once

#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include "n00b.h"
#include "net/quic/quic_types.h"

/* Forward decl: opaque outside of quic/auth_policy.h. */
struct n00b_quic_auth_policy;

/**
 * @internal
 * @brief One run of received stream bytes, or the peer's FIN, published by
 *        the endpoint pump.
 *
 * Immutable once published except for @c next and @c off, which only the
 * channel's reader touches after taking the chunk from the inbox.
 */
typedef struct n00b_quic_rx_chunk {
    struct n00b_quic_rx_chunk *next;
    size_t                     len;
    size_t                     off; /**< Bytes the reader has consumed. */
    bool                       fin; /**< The peer's FIN; carries no bytes. */
    uint8_t                    bytes[];
} n00b_quic_rx_chunk_t;

/**
 * @internal
 * @brief Events that move a channel's state.
 */
typedef enum : uint8_t {
    N00B_QUIC_CHAN_EV_LOCAL_FIN,
    N00B_QUIC_CHAN_EV_PEER_FIN,
    N00B_QUIC_CHAN_EV_LOCAL_RESET,
    N00B_QUIC_CHAN_EV_PEER_RESET,
    N00B_QUIC_CHAN_EV_CLOSE,
} n00b_quic_chan_event_t;

struct n00b_quic_chan {
    n00b_quic_conn_t       *conn;        /**< Owning connection; borrowed. */
    n00b_quic_chan_t       *next_in_conn;/**< Intrusive list link; conn-owned. */
    uint64_t                stream_id;   /**< Picoquic-assigned stream ID. */
    n00b_quic_chan_kind_t   kind;
    /* Written by the pump and the drivers alike, always through
     * _n00b_quic_chan_transition. */
    _Atomic(n00b_quic_chan_state_t) state;
    /* Phase 3 § 10: declarative auth policy attached at chan_open
     * time.  Borrowed; lifetime is the application's. */
    struct n00b_quic_auth_policy *auth_policy;
    uint64_t                bytes_sent;
    uint64_t                bytes_received;
    uint64_t                app_err_local;
    uint64_t                app_err_peer;
    uint64_t                last_activity_ns;

    /* Receive state, FRAMED and BYTES kinds only.  The endpoint pump
     * publishes each run of received bytes, and then the peer's FIN, as
     * chunks on rx_inbox and touches nothing else.  The channel's one
     * reader owns the rest: it moves published chunks onto
     * rx_head..rx_tail in arrival order and consumes them there, so it
     * reaches the FIN only after every byte ahead of it. */
    _Atomic(n00b_quic_rx_chunk_t *) rx_inbox;
    n00b_quic_rx_chunk_t           *rx_head;
    n00b_quic_rx_chunk_t           *rx_tail;
#ifdef N00B_DEBUG
    /* OS thread id of the thread inside a reader call, or 0. */
    _Atomic(int64_t)                rx_reader;
#endif

    /* DGRAM-only: queue of received datagram payloads.  Each entry
     * is one complete datagram (RFC 9221 atomic delivery semantics
     * — no concatenation, no fragmentation).  The list is a struct
     * (value-semantic — n00b_list_t isn't a pointer type); it gets
     * initialized when the singleton DGRAM channel is created in
     * `_n00b_quic_conn_dgram_chan`.  Unused on stream-kind channels. */
    n00b_list_t(n00b_buffer_t *)  dgram_recv_queue;
    bool                          dgram_queue_inited;

    bool                    bidi;
    bool                    sent_fin;    /**< Local FIN already sent. */
    bool                    closed;      /**< Local close called. */
};

/**
 * @internal
 * @brief Append a received datagram payload to a DGRAM channel's queue.
 *
 * Each call pushes one whole datagram; the application pops them one
 * at a time via `n00b_quic_chan_recv_dgram`.  Called from the
 * picoquic per-cnx callback when a `picoquic_callback_datagram` event
 * fires.
 */
extern void
_n00b_quic_chan_push_dgram(n00b_quic_chan_t *chan,
                           const uint8_t    *bytes,
                           size_t            len);

/**
 * @internal
 * @brief Publish bytes received from picoquic to a channel's reader.
 *
 * Copies the bytes into a chunk and pushes it on the channel's inbox.  Safe
 * to call concurrently with the reader.
 */
extern void
_n00b_quic_chan_append_recv(n00b_quic_chan_t *chan,
                            const uint8_t    *bytes,
                            size_t            len);

/**
 * @internal
 * @brief Put bytes the reader took back in front of everything unread.
 *
 * Reader-only, like n00b_quic_chan_recv: the H3 drivers use it to return
 * the bytes they read past a stream-type varint.
 */
extern void
_n00b_quic_chan_unread(n00b_quic_chan_t *chan,
                       const uint8_t    *bytes,
                       size_t            len);

/**
 * @internal
 * @brief Publish the peer's FIN behind every byte already published.
 *
 * Safe to call concurrently with the reader, which sees the FIN only once it
 * has consumed the bytes ahead of it.
 */
extern void
_n00b_quic_chan_append_fin(n00b_quic_chan_t *chan);

/**
 * @internal
 * @brief Apply @p ev to the channel's state and return the new state.
 *
 * A compare-and-swap loop, so the pump and a driver moving the state at the
 * same time both take effect.
 */
extern n00b_quic_chan_state_t
_n00b_quic_chan_transition(n00b_quic_chan_t *chan, n00b_quic_chan_event_t ev);

#ifdef N00B_DEBUG
/**
 * @internal
 * @brief Windows where a debug hook can hold one thread so a test can run
 *        another inside it.
 */
typedef enum : uint8_t {
    N00B_QUIC_DEBUG_PUBLISH,      /**< Pump filled a chunk, not yet published. */
    N00B_QUIC_DEBUG_READ,         /**< Reader inside n00b_quic_chan_recv. */
    N00B_QUIC_DEBUG_TRANSITION,   /**< State loaded, new state not yet stored. */
    N00B_QUIC_DEBUG_REGISTER,     /**< List head read, channel not yet linked. */
} n00b_quic_debug_point_t;

extern void (*_n00b_quic_debug_hook)(n00b_quic_conn_t       *conn,
                                     n00b_quic_chan_t       *chan,
                                     n00b_quic_debug_point_t point);

#define N00B_QUIC_DEBUG_POINT(conn, chan, point)                    \
    do {                                                            \
        void (*_qdp_hook)(n00b_quic_conn_t *,                       \
                          n00b_quic_chan_t *,                       \
                          n00b_quic_debug_point_t)                  \
            = _n00b_quic_debug_hook;                                \
        if (_qdp_hook != nullptr) {                                 \
            _qdp_hook((conn), (chan), (point));                     \
        }                                                           \
    } while (0)
#else
#define N00B_QUIC_DEBUG_POINT(conn, chan, point) ((void)0)
#endif
