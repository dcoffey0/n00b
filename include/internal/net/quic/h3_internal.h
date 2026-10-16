/**
 * @file h3_internal.h
 * @internal
 * @brief Internal H3 client structures shared across `h3_client.c`,
 *        `h3_frame.c`, and `h3_settings.c`.
 *
 * Not part of the public API.  Anything in here may change without
 * notice.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "n00b.h"
#include "core/buffer.h"
#include "core/data_lock.h"
#include "conduit/conduit.h"
#include "conduit/topic.h"
#include "net/quic/quic_types.h"
#include "net/quic/endpoint.h"
#include "net/quic/h3.h"
#include "net/quic/h3_types.h"
#include "net/quic/qpack.h"

struct n00b_quic_conn;
struct n00b_quic_chan;

/* ===========================================================================
 * Internal varint helpers (RFC 9000 § 16; same encoding as QUIC frames)
 *
 * The H3 frame layer uses RFC 9000 varints for both type and length.
 * We expose internal-only helpers here for h3_frame.c + h3_settings.c
 * (no public surface — the framer module already re-exports the
 * primitives at `quic/framer.h`).
 * =========================================================================== */

/**
 * @brief Append a QUIC varint to a buffer.
 *
 * @param out   Destination buffer (grown).
 * @param value Value to encode (0 ≤ value ≤ 2^62 - 1).
 *
 * @return Bytes appended on success (1, 2, 4, or 8); 0 if @p value is
 *         too large.
 */
extern size_t
n00b_h3_varint_append(n00b_buffer_t *out, uint64_t value);

/**
 * @brief Decode a QUIC varint from raw bytes.
 *
 * @param in        Source bytes.
 * @param in_len    Bytes available.
 * @param out_value Decoded value (must be non-null).
 *
 * @return On success, bytes consumed (1, 2, 4, or 8).
 *         0 if @p in is truncated.
 *         < 0 on a malformed varint (right now: shouldn't happen for
 *         a well-formed RFC 9000 encoding — this branch exists to
 *         keep the contract symmetric with the framer's API).
 */
extern int64_t
n00b_h3_varint_decode(const uint8_t *in, size_t in_len, uint64_t *out_value);

/* ===========================================================================
 * Per-request state
 * =========================================================================== */

typedef enum : uint8_t {
    N00B_H3_REQ_STATE_NEW            = 0,  /**< Allocated; nothing sent. */
    N00B_H3_REQ_STATE_HEADERS_SENT   = 1,  /**< HEADERS + body queued. */
    N00B_H3_REQ_STATE_HEADERS_RECVD  = 2,  /**< Server HEADERS arrived. */
    N00B_H3_REQ_STATE_DONE           = 3,  /**< Body fully received. */
    N00B_H3_REQ_STATE_RESET          = 4,  /**< Stream reset locally or peer. */
} n00b_h3_req_state_t;

struct n00b_h3_request {
    n00b_h3_client_t        *client;       /**< Borrowed; outlives request. */
    struct n00b_quic_chan   *chan;         /**< Bidi request stream. */

    n00b_rwlock_t           *lock;
    n00b_h3_req_state_t      state;

    /* Inbound buffering — bytes received from picoquic accumulate
     * here until enough exist for a frame parse.  Lazily allocated
     * from the H3 conduit pool. */
    n00b_buffer_t           *recv_buf;

    /* Response state once HEADERS has been parsed. */
    uint16_t                 status;
    n00b_h3_header_t        *resp_headers;
    size_t                   resp_n_headers;

    /* Concatenated DATA frame contents (response body). */
    n00b_buffer_t           *resp_body;
    /* Streaming-side cursor: number of resp_body bytes already drained
     * by `n00b_h3_request_consume_body`.  Always <= resp_body->byte_len. */
    size_t                   resp_body_consumed;

    bool                     local_fin_sent;
    bool                     peer_fin_seen;

    /* ---- Per-call response-body cap (mid-stream enforcement). -----------
     *
     * `max_body_size = 0` means "no cap" (existing callers see identical
     * behavior).  When non-zero, the frame processor enforces the cap on
     * each DATA frame and on the response's `content-length` header
     * (when present) — overrun resets the stream with
     * `N00B_H3_ERR_EXCESSIVE_LOAD` and flips the request into the RESET
     * state with `body_cap_exceeded = true` so callers can distinguish
     * cap-driven resets from peer-initiated resets.
     *
     * Set via `n00b_h3_request_await`'s `.max_body_size` kwarg (or by
     * direct field write from a higher layer; see http_h3.c). */
    uint64_t                 max_body_size;
    bool                     body_cap_exceeded;
};

/* ===========================================================================
 * H3 client struct
 * =========================================================================== */

struct n00b_h3_client {
    struct n00b_quic_conn   *conn;             /**< Underlying connection. */
    n00b_rwlock_t           *lock;

    /* Local + peer SETTINGS. */
    n00b_h3_settings_t       local_settings;
    n00b_h3_settings_t       peer_settings;

    /* QPACK encoder/decoder (created at construction; mirror the
     * settings advertised). */
    n00b_qpack_encoder_t    *qpack_enc;
    n00b_qpack_decoder_t    *qpack_dec;

    /* Uni streams owned by this client. */
    struct n00b_quic_chan   *control_uni;          /**< Outbound CONTROL. */
    struct n00b_quic_chan   *qpack_enc_uni;        /**< Outbound QPACK enc. */
    struct n00b_quic_chan   *qpack_dec_uni;        /**< Outbound QPACK dec. */

    /* Inbound peer uni streams (lazily attached as they appear). */
    struct n00b_quic_chan   *peer_control_uni;
    struct n00b_quic_chan   *peer_qpack_enc_uni;
    struct n00b_quic_chan   *peer_qpack_dec_uni;

    /* Per-uni-stream parse cursor: peer uni streams begin with a
     * varint identifying their kind (RFC 9114 § 6.2).  Until that
     * varint has been read we don't know which struct slot the
     * channel belongs in; we hold pending data in these scratch
     * buffers. */
    /* Up to 8 bytes for the leading kind varint of an unknown peer
     * uni stream (RFC 9114 § 6.2 — varint is at most 8 bytes). */
    uint8_t                  peer_uni_kind_bytes[8];
    uint8_t                  peer_uni_kind_len;

    /* Pending requests, lazily initialised on first push. */
    n00b_list_t(n00b_h3_request_t *) *requests;

    /* Per-uni-stream feeder scratch.  One buffer per peer uni-stream
     * kind so concurrent recv pumps don't trample each other.
     * Lives on the client so multiple clients in the same thread are
     * isolated (v1 used `__thread` locals — incorrect for that
     * topology).  Lazily allocated. */
    n00b_buffer_t           *peer_ctrl_buf;
    n00b_buffer_t           *peer_qe_buf;
    n00b_buffer_t           *peer_qd_buf;

    /* GOAWAY received from peer (RFC 9114 § 5.2): client must not
     * issue NEW requests on streams whose IDs are >= the value the
     * peer advertised; in-flight requests already past that limit
     * are still allowed to complete.  `goaway_received` is set
     * once we observe a GOAWAY on the peer's CONTROL stream. */
    bool                     goaway_received;
    uint64_t                 goaway_max_stream_id;

    /* Frame body cap. */
    size_t                   max_frame_body;

    bool                     local_settings_sent;
    bool                     closed;
};

/* ===========================================================================
 * Allocator
 * =========================================================================== */

extern n00b_allocator_t *n00b_h3_alloc(void);

/* ===========================================================================
 * Internal client helpers (reachable from h3_client.c only)
 * =========================================================================== */

/**
 * @internal
 * @brief Send a single byte (the uni-stream kind varint) on a uni
 *        stream right after open.
 */
extern n00b_result_t(bool)
n00b_h3_client_init_uni_stream(n00b_h3_client_t          *client,
                               struct n00b_quic_chan     *chan,
                               n00b_h3_uni_stream_kind_t  kind);

/**
 * @internal
 * @brief Append @p bytes to a request's recv-buffer and try to parse
 *        as many frames as possible.
 *
 * Takes the request's lock, which guards everything the request's
 * accessors read, the response body included.
 */
extern n00b_result_t(bool)
n00b_h3_request_feed(n00b_h3_request_t *req,
                     const uint8_t     *bytes,
                     size_t             len,
                     bool               fin);

/* ===========================================================================
 * Server structs (sub-phase 4.4)
 *
 * Mirror of the client-side state but rooted at a listen-mode endpoint.
 * One `n00b_h3_server_t` per endpoint; one `n00b_h3_server_conn_t` per
 * accepted connection; one `n00b_h3_inbound_request_t` per
 * client-initiated bidi stream.
 * =========================================================================== */

typedef enum : uint8_t {
    N00B_H3_INBOUND_STATE_NEW            = 0,
    N00B_H3_INBOUND_STATE_HEADERS_PARSED = 1, /**< Pseudo + extras decoded. */
    N00B_H3_INBOUND_STATE_BODY_COMPLETE  = 2, /**< Peer FIN'd; body finalized. */
    N00B_H3_INBOUND_STATE_PUBLISHED      = 3, /**< Event posted on topic. */
    N00B_H3_INBOUND_STATE_RESPONDED      = 4, /**< Response sent. */
    N00B_H3_INBOUND_STATE_RESET          = 5,
} n00b_h3_inbound_state_t;

struct n00b_h3_inbound_request {
    n00b_h3_server_conn_t       *server_conn;  /**< Borrowed. */
    struct n00b_quic_chan       *chan;         /**< Bidi stream. */

    n00b_rwlock_t               *lock;
    n00b_h3_inbound_state_t      state;

    /* Inbound buffering — lazily allocated from the H3 conduit pool. */
    n00b_buffer_t               *recv_buf;

    /* Decoded pseudo-headers (NUL-terminated copies). */
    char                        *method;
    char                        *scheme;
    char                        *authority;
    char                        *path;

    /* Extra (non-pseudo) headers. */
    n00b_h3_header_t            *headers;
    size_t                       n_headers;

    /* Concatenated DATA frame contents (request body). */
    n00b_buffer_t               *body;
    /* Streaming cursor mirroring the client side: number of body bytes
     * already drained via `n00b_h3_inbound_request_consume_body`. */
    size_t                       body_consumed;

    bool                         peer_fin_seen;
    bool                         response_sent;
    /* True once HEADERS have been sent (streaming handlers may emit
     * additional DATA frames + FIN after this). */
    bool                         response_headers_sent;
};

struct n00b_h3_server_conn {
    n00b_h3_server_t             *server;        /**< Borrowed. */
    struct n00b_quic_conn        *conn;

    n00b_rwlock_t               *lock;

    /* Owned uni streams (created at conn-accept time). */
    struct n00b_quic_chan        *control_uni;
    struct n00b_quic_chan        *qpack_enc_uni;
    struct n00b_quic_chan        *qpack_dec_uni;

    /* Peer uni streams (lazily attached). */
    struct n00b_quic_chan        *peer_control_uni;
    struct n00b_quic_chan        *peer_qpack_enc_uni;
    struct n00b_quic_chan        *peer_qpack_dec_uni;

    /* Per-uni-stream feeder scratch. */
    uint8_t                       peer_uni_kind_bytes[8];
    uint8_t                       peer_uni_kind_len;

    n00b_buffer_t                *peer_ctrl_buf;
    n00b_buffer_t                *peer_qe_buf;
    n00b_buffer_t                *peer_qd_buf;

    /* Inbound requests, lazily initialised on first push. */
    n00b_list_t(n00b_h3_inbound_request_t *) *requests;

    /* Highest-seen client-initiated bidi stream id (for GOAWAY). */
    uint64_t                      max_seen_client_bidi_id;
    bool                          has_seen_client_bidi;

    bool                          local_settings_sent;
    bool                          goaway_sent;
    bool                          closed;
};

struct n00b_h3_server {
    struct n00b_quic_endpoint    *endpoint;     /**< Borrowed. */
    struct n00b_conduit          *conduit;      /**< Borrowed. */

    n00b_rwlock_t               *lock;

    /* Server-side QPACK engines.  RFC 9204 doesn't actually require
     * one engine per connection if we cap dynamic-table use to zero
     * (and we do for v1: capacity=0 keeps every section as
     * literal-only).  We still scope encoder + decoder per server
     * for forward-compat. */
    n00b_qpack_encoder_t         *qpack_enc;
    n00b_qpack_decoder_t         *qpack_dec;

    /* Accept inbox subscribed to the endpoint's accept topic. */
    n00b_quic_accept_inbox_t     *accept_inbox;

    /* Outbound topic for ready inbound requests. */
    n00b_conduit_topic_base_t    *request_topic;

    /* Per-conn list, lazily initialised on first accept. */
    n00b_list_t(n00b_h3_server_conn_t *) *conns;

    /* Local settings. */
    n00b_h3_settings_t            local_settings;

    /* Frame body cap. */
    size_t                        max_frame_body;
    uint64_t                      qpack_max_table_capacity;
    uint64_t                      qpack_blocked_streams;

    /* Sub-phase 4.7: when true, publish inbound requests on the
     * request topic as soon as HEADERS are parsed (streaming patterns
     * need this — handler must consume DATA before peer FIN). */
    bool                          early_publish;

    bool                          closed;
};

/**
 * @internal
 * @brief Per-conn drive helper.  Public-internal so the loopback test
 *        can pump conn state directly when the topic plumbing is not
 *        what's under test.  Not part of the public API.
 */
extern n00b_result_t(bool)
n00b_h3_server_conn_drive(n00b_h3_server_conn_t *sconn);

#ifdef N00B_DEBUG
/**
 * @internal
 * @brief Windows in the H3 drivers where a debug hook can hold the driver
 *        so a test can run another thread inside it.
 */
typedef enum : uint8_t {
    N00B_H3_DEBUG_DRAINED,    /**< Client request drained, FIN not checked. */
    N00B_H3_DEBUG_BODY_GROWN, /**< Body buffer grown, DATA not copied in. */
} n00b_h3_debug_point_t;

extern void (*_n00b_h3_debug_hook)(struct n00b_quic_chan *chan,
                                   n00b_h3_debug_point_t  point);

#define N00B_H3_DEBUG_POINT(chan, point)                                      \
    do {                                                                      \
        void (*_hdp_hook)(struct n00b_quic_chan *, n00b_h3_debug_point_t)     \
            = _n00b_h3_debug_hook;                                            \
        if (_hdp_hook != nullptr) {                                           \
            _hdp_hook((chan), (point));                                       \
        }                                                                     \
    } while (0)
#else
#define N00B_H3_DEBUG_POINT(chan, point) ((void)0)
#endif
