/*
 * h3_server.c — HTTP/3 server lifecycle (RFC 9114).
 *
 * Mirrors `h3_client.c` on the server side: builds a `n00b_h3_server_t`
 * over a listen-mode `n00b_quic_endpoint_t`, watches the endpoint's
 * accept topic, wraps each new conn in a `n00b_h3_server_conn_t`
 * (auto-opening control + QPACK enc + QPACK dec uni streams + emitting
 * SETTINGS), discovers + classifies the peer's three uni streams, and
 * surfaces inbound bidi (client-initiated) streams as
 * `n00b_h3_inbound_request_t` events on the server's request topic.
 *
 * Flow per inbound request:
 *   - peer FRAMED bidi stream appears in the conn's channel list
 *   - the recv-buffer accumulates HEADERS [+ DATA] [+ FIN]
 *   - HEADERS gets QPACK-decoded; pseudo-headers extracted, the rest
 *     stashed on the request struct
 *   - DATA bytes append onto req->body
 *   - on FIN, the request is published as a topic event
 *   - the application calls `_respond` which emits HEADERS + DATA + FIN
 *     back on the same stream
 *
 * GOAWAY semantics (RFC 9114 § 5.2): on close we emit GOAWAY with
 * `(highest-issued-client-stream-id) + 4` on each per-conn control
 * stream.  Per § 5.2 that limit applies to client-initiated streams;
 * +4 is "the next ID the client could have used".  Streams beyond
 * the limit are dropped without processing.
 *
 * Server push (uni stream kind 0x01, PUSH_PROMISE frame 0x05) is NOT
 * supported in v1.  The client side already STOP_SENDINGs unknown
 * uni-stream kinds; on the server side, PUSH_PROMISE on a request
 * stream triggers RESET_STREAM with H3_FRAME_UNEXPECTED.
 */
#define N00B_USE_INTERNAL_API
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/buffer.h"
#include "core/string.h"
#include "adt/option.h"
#include "adt/result.h"
#include "conduit/conduit.h"
#include "conduit/topic.h"
#include "conduit/publisher.h"
#include "net/quic/quic_types.h"
#include "net/quic/conn.h"
#include "net/quic/chan.h"
#include "net/quic/endpoint.h"
#include "net/quic/qpack.h"
#include "net/quic/h3.h"
#include "net/quic/h3_types.h"
#include "internal/net/quic/h3_internal.h"
#include "internal/net/quic/chan_internal.h"

/* ===========================================================================
 * Allocation helpers (conduit_pool throughout).
 *
 * We reuse `n00b_h3_alloc()` from h3_frame.c.
 * =========================================================================== */

static uint8_t *
h3_dup_bytes(const uint8_t *src, size_t len)
{
    uint8_t *p = n00b_alloc_array_with_opts(uint8_t,
                        (int64_t)(len == 0 ? 1 : len),
                        &(n00b_alloc_opts_t){
                            .allocator = n00b_h3_alloc(),
                            .no_scan   = true,
                        });
    if (len > 0) memcpy(p, src, len);
    return p;
}

static char *
h3_dup_cstr(const uint8_t *src, size_t len)
{
    char *p = n00b_alloc_array_with_opts(char, (int64_t)(len + 1),
                        &(n00b_alloc_opts_t){
                            .allocator = n00b_h3_alloc(),
                            .no_scan   = true,
                        });
    if (len > 0) memcpy(p, src, len);
    p[len] = 0;
    return p;
}

static size_t
h3_write_u64_decimal(char *dst, size_t cap, uint64_t value)
{
    char   tmp[20];
    size_t n = 0;

    if (cap == 0) {
        return 0;
    }

    do {
        tmp[n++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0);

    if (n + 1 > cap) {
        return 0;
    }

    for (size_t i = 0; i < n; i++) {
        dst[i] = tmp[n - 1 - i];
    }
    dst[n] = '\0';
    return n;
}

/* ===========================================================================
 * Per-request recv-buffer growth.
 * =========================================================================== */

static void
ireq_recv_append(n00b_h3_inbound_request_t *req,
                 const uint8_t             *bytes,
                 size_t                     len)
{
    if (len == 0) return;
    if (!req->recv_buf) {
        req->recv_buf = n00b_buffer_empty(.allocator = n00b_h3_alloc());
    }
    size_t old = req->recv_buf->byte_len;
    n00b_buffer_resize(req->recv_buf, old + len);
    memcpy(req->recv_buf->data + old, bytes, len);
}

static void
ireq_recv_consume(n00b_h3_inbound_request_t *req, size_t n)
{
    if (n == 0 || !req->recv_buf) return;
    size_t have = req->recv_buf->byte_len;
    if (n >= have) {
        n00b_buffer_resize(req->recv_buf, 0);
        return;
    }
    memmove(req->recv_buf->data, req->recv_buf->data + n, have - n);
    n00b_buffer_resize(req->recv_buf, have - n);
}

/* ===========================================================================
 * Uni-stream init (kind varint write).
 *
 * Same as the client's `n00b_h3_client_init_uni_stream` but kept local
 * so we don't have to widen its signature to take a server.
 * =========================================================================== */

static n00b_result_t(bool)
sconn_init_uni_stream(n00b_quic_chan_t          *chan,
                       n00b_h3_uni_stream_kind_t  kind)
{
    n00b_buffer_t b;
    memset(&b, 0, sizeof(b));
    n00b_buffer_init(&b, .length = 0, .allocator = n00b_h3_alloc(),
                     .no_lock = true);
    if (n00b_h3_varint_append(&b, (uint64_t)kind) == 0) {
        return n00b_result_err(bool, N00B_QUIC_ERR_PROTOCOL);
    }
    n00b_result_t(size_t) sr = n00b_quic_chan_send(chan,
                                                    (const uint8_t *)b.data,
                                                    (size_t)b.byte_len);
    if (n00b_result_is_err(sr)) {
        return n00b_result_err(bool, n00b_result_get_err(sr));
    }
    return n00b_result_ok(bool, true);
}

/* ===========================================================================
 * Per-conn SETTINGS dispatch.
 * =========================================================================== */

static n00b_result_t(bool)
sconn_emit_initial_settings(n00b_h3_server_conn_t *sconn)
{
    n00b_h3_server_t *server = sconn->server;

    n00b_buffer_t body;
    memset(&body, 0, sizeof(body));
    n00b_buffer_init(&body, .length = 0, .allocator = n00b_h3_alloc(),
                     .no_lock = true);

    n00b_result_t(bool) br = n00b_h3_settings_emit_body(
        &body,
        server->local_settings.qpack_max_table_capacity,
        server->local_settings.qpack_blocked_streams,
        server->local_settings.max_field_section_size);
    if (n00b_result_is_err(br)) return br;

    n00b_buffer_t frame;
    memset(&frame, 0, sizeof(frame));
    n00b_buffer_init(&frame, .length = 0, .allocator = n00b_h3_alloc(),
                     .no_lock = true);
    n00b_result_t(bool) fr = n00b_h3_frame_emit(
        &frame,
        N00B_H3_FRAME_SETTINGS,
        (const uint8_t *)body.data, (size_t)body.byte_len);
    if (n00b_result_is_err(fr)) return fr;

    n00b_result_t(size_t) sr = n00b_quic_chan_send(
        sconn->control_uni,
        (const uint8_t *)frame.data, (size_t)frame.byte_len);
    if (n00b_result_is_err(sr)) {
        return n00b_result_err(bool, n00b_result_get_err(sr));
    }
    return n00b_result_ok(bool, true);
}

/* ===========================================================================
 * Per-conn lifecycle.
 * =========================================================================== */

static n00b_h3_server_conn_t *
sconn_new(n00b_h3_server_t *server, n00b_quic_conn_t *conn)
{
    n00b_h3_server_conn_t *sconn = n00b_alloc_with_opts(n00b_h3_server_conn_t,
                                        &(n00b_alloc_opts_t){
                                            .allocator = n00b_h3_alloc(),
                                        });
    sconn->lock = n00b_data_lock_new();
    sconn->server = server;
    sconn->conn   = conn;

    /* Open the three uni streams + write kind varints. */
    n00b_result_t(n00b_quic_chan_t *) cr;

    cr = n00b_quic_chan_open(conn, .bidi = false);
    if (n00b_result_is_err(cr)) return nullptr;
    sconn->control_uni = n00b_result_get(cr);
    (void)sconn_init_uni_stream(sconn->control_uni, N00B_H3_UNI_CONTROL);

    cr = n00b_quic_chan_open(conn, .bidi = false);
    if (n00b_result_is_err(cr)) return nullptr;
    sconn->qpack_enc_uni = n00b_result_get(cr);
    (void)sconn_init_uni_stream(sconn->qpack_enc_uni, N00B_H3_UNI_QPACK_ENCODER);

    cr = n00b_quic_chan_open(conn, .bidi = false);
    if (n00b_result_is_err(cr)) return nullptr;
    sconn->qpack_dec_uni = n00b_result_get(cr);
    (void)sconn_init_uni_stream(sconn->qpack_dec_uni, N00B_H3_UNI_QPACK_DECODER);

    /* Emit SETTINGS. */
    if (n00b_result_is_err(sconn_emit_initial_settings(sconn))) {
        return nullptr;
    }
    sconn->local_settings_sent = true;

    return sconn;
}

/* ===========================================================================
 * Server lifecycle.
 * =========================================================================== */

n00b_result_t(n00b_h3_server_t *)
n00b_h3_server_new(n00b_quic_endpoint_t *endpoint,
                   n00b_conduit_t       *conduit) _kargs
{
    size_t   max_frame_body          = N00B_H3_DEFAULT_MAX_FRAME_BODY;
    uint64_t qpack_max_table_capacity = 4096;
    uint64_t qpack_blocked_streams    = 0;
    bool     early_publish           = false;
}
{
    if (!endpoint || !conduit) {
        return n00b_result_err(n00b_h3_server_t *, N00B_QUIC_ERR_NULL_ARG);
    }
    n00b_option_t(n00b_conduit_topic_base_t *) atopic_opt =
        n00b_quic_endpoint_accept_topic(endpoint);
    if (!n00b_option_is_set(atopic_opt)) {
        /* Endpoint isn't in listen mode. */
        return n00b_result_err(n00b_h3_server_t *, N00B_QUIC_ERR_INVALID_ARG);
    }
    n00b_conduit_topic_base_t *atopic = n00b_option_get(atopic_opt);

    n00b_h3_server_t *server = n00b_alloc_with_opts(n00b_h3_server_t,
                                    &(n00b_alloc_opts_t){
                                        .allocator = n00b_h3_alloc(),
                                    });
    server->lock = n00b_data_lock_new();
    server->endpoint                            = endpoint;
    server->conduit                             = conduit;
    server->max_frame_body                      = max_frame_body;
    server->qpack_max_table_capacity            = qpack_max_table_capacity;
    server->qpack_blocked_streams               = qpack_blocked_streams;
    server->local_settings.qpack_max_table_capacity = qpack_max_table_capacity;
    server->local_settings.qpack_blocked_streams    = qpack_blocked_streams;
    server->local_settings.max_field_section_size   = 0;
    server->local_settings.received                 = false;

    server->early_publish = early_publish;

    server->qpack_enc = n00b_qpack_encoder_new(qpack_max_table_capacity,
                                                qpack_blocked_streams);
    server->qpack_dec = n00b_qpack_decoder_new(qpack_max_table_capacity,
                                                qpack_blocked_streams);

    /* Subscribe to accept events. */
    server->accept_inbox = n00b_quic_accept_inbox_new(conduit);
    n00b_quic_accept_subscribe(atopic, server->accept_inbox,
                                .operations = N00B_CONDUIT_OP_ALL);

    /* Build the request topic for the application to subscribe to.
     * The URI is keyed on the server pointer (low 48 bits) so multiple
     * servers in one conduit have distinct request topics.  We use the
     * USER_EVENT tag namespace because there's no dedicated tag for H3
     * request events (yet). */
    uint64_t topic_id = ((uintptr_t)server) & N00B_CONDUIT_URI_ID_MASK;
    n00b_conduit_topic_t(n00b_h3_request_event_t) *topic =
        n00b_conduit_topic_init(n00b_h3_request_event_t,
                                conduit,
                                N00B_CONDUIT_URI_USER_EVENT(topic_id));
    if (!topic) {
        return n00b_result_err(n00b_h3_server_t *, N00B_CONDUIT_ERR_ALLOC);
    }
    server->request_topic = (n00b_conduit_topic_base_t *)topic;

    return n00b_result_ok(n00b_h3_server_t *, server);
}

n00b_conduit_topic_base_t *
n00b_h3_server_request_topic(n00b_h3_server_t *server)
{
    return server ? server->request_topic : nullptr;
}

/* ===========================================================================
 * Discovery: drain accept inbox, build per-conn state.
 * =========================================================================== */

static void
drain_accept_inbox(n00b_h3_server_t *server)
{
    while (n00b_quic_accept_inbox_has_messages(server->accept_inbox)) {
        n00b_quic_accept_msg_t *m =
            n00b_quic_accept_inbox_pop(server->accept_inbox);
        if (!m) break;
        n00b_quic_conn_t *conn = m->payload.conn;
        if (!conn) continue;

        /* Skip if we already wrap this conn (defensive). */
        bool dup = false;
        if (server->conns) {
            size_t nc = (size_t)n00b_list_len(*server->conns);
            for (size_t i = 0; i < nc; i++) {
                n00b_h3_server_conn_t *sc =
                    n00b_list_get(*server->conns, i);
                if (sc->conn == conn) { dup = true; break; }
            }
        }
        if (dup) continue;

        n00b_h3_server_conn_t *sconn = sconn_new(server, conn);
        if (!sconn) continue;

        if (!server->conns) {
            server->conns = n00b_alloc_with_opts(
                n00b_list_t(n00b_h3_server_conn_t *),
                &(n00b_alloc_opts_t){.allocator = n00b_h3_alloc()});
            *server->conns = n00b_list_new_private(
                n00b_h3_server_conn_t *,
                .allocator = n00b_h3_alloc());
        }
        n00b_list_push(*server->conns, sconn);
    }
}

/* ===========================================================================
 * Pseudo-header validation.  RFC 9114 § 4.3.1 requires :method,
 * :scheme, :authority, :path on a request HEADERS frame.
 * =========================================================================== */

static bool
pseudo_match(const n00b_qpack_field_t *f, const char *name)
{
    size_t nlen = strlen(name);
    return f->name_len == nlen && memcmp(f->name, name, nlen) == 0;
}

/* ===========================================================================
 * Inbound request — frame dispatch.
 * =========================================================================== */

static void
publish_request_event(n00b_h3_server_t          *server,
                      n00b_h3_inbound_request_t *req)
{
    if (!server->request_topic) return;
    n00b_result_t(n00b_conduit_publisher_t *) pub_res =
        n00b_conduit_publish_try_claim(server->request_topic);
    if (n00b_result_is_err(pub_res)) return;
    n00b_conduit_publisher_t *pub = n00b_result_get(pub_res);

    n00b_h3_request_msg_t *msg = n00b_alloc_with_opts(
        n00b_h3_request_msg_t,
        &(n00b_alloc_opts_t){.allocator = n00b_h3_alloc()});
    msg->header.type       = N00B_CONDUIT_MSG_USER;
    msg->header.topic      = server->request_topic;
    msg->header.generation =
        n00b_conduit_topic_generation(server->request_topic);
    msg->header.epoch      =
        n00b_conduit_topic_epoch(server->request_topic);
    msg->header.timestamp  = 0;
    msg->header.next       = nullptr;
    msg->payload.req       = req;

    n00b_conduit_topic_deliver_msg(
        n00b_h3_request_event_t,
        (n00b_conduit_topic_t(n00b_h3_request_event_t) *)server->request_topic,
        msg, N00B_CONDUIT_OP_ALL);
    n00b_conduit_publish_yield(pub);
}

static n00b_result_t(bool)
process_inbound_request_frame(n00b_h3_inbound_request_t *req,
                              const n00b_h3_frame_t     *frame)
{
    n00b_h3_server_conn_t *sconn  = req->server_conn;
    n00b_h3_server_t      *server = sconn->server;

    if (frame->type == N00B_H3_FRAME_HEADERS) {
        /* QPACK-decode. */
        n00b_qpack_field_t fields[N00B_QPACK_MAX_FIELDS_PER_SECTION];
        size_t              n_fields = 0;
        n00b_buffer_t       ds;
        memset(&ds, 0, sizeof(ds));
        n00b_buffer_init(&ds, .length = 0, .allocator = n00b_h3_alloc(),
                         .no_lock = true);

        n00b_result_t(bool) dr = n00b_qpack_decode(
            server->qpack_dec,
            n00b_quic_chan_id(req->chan),
            frame->body, frame->body_len,
            fields, N00B_QPACK_MAX_FIELDS_PER_SECTION,
            &n_fields, &ds);
        if (n00b_result_is_err(dr)) {
            return n00b_result_err(bool, n00b_result_get_err(dr));
        }

        /* Forward decoder-stream feedback. */
        if (ds.byte_len > 0 && sconn->qpack_dec_uni) {
            n00b_quic_chan_send(sconn->qpack_dec_uni,
                                 (const uint8_t *)ds.data,
                                 (size_t)ds.byte_len);
        }

        /* Pseudo-header presence check. */
        const n00b_qpack_field_t *m = nullptr, *s = nullptr;
        const n00b_qpack_field_t *a = nullptr, *p = nullptr;
        size_t extra_count = 0;
        for (size_t i = 0; i < n_fields; i++) {
            const n00b_qpack_field_t *f = &fields[i];
            if (pseudo_match(f, ":method"))         m = f;
            else if (pseudo_match(f, ":scheme"))    s = f;
            else if (pseudo_match(f, ":authority")) a = f;
            else if (pseudo_match(f, ":path"))      p = f;
            else extra_count++;
        }
        if (!m || !s || !a || !p) {
            /* RFC 9114 § 4.3.1: request lacking pseudo-headers ⇒
             * MESSAGE_ERROR; the request is rejected with reset. */
            return n00b_result_err(bool, N00B_QUIC_ERR_PROTOCOL);
        }

        req->method    = h3_dup_cstr(m->value, m->value_len);
        req->scheme    = h3_dup_cstr(s->value, s->value_len);
        req->authority = h3_dup_cstr(a->value, a->value_len);
        req->path      = h3_dup_cstr(p->value, p->value_len);

        /* Copy extra (non-pseudo) headers. */
        if (extra_count == 0) {
            req->headers   = nullptr;
            req->n_headers = 0;
        } else {
            n00b_h3_header_t *out = n00b_alloc_array_with_opts(
                n00b_h3_header_t, (int64_t)extra_count,
                &(n00b_alloc_opts_t){ .allocator = n00b_h3_alloc() });
            size_t idx = 0;
            for (size_t i = 0; i < n_fields; i++) {
                const n00b_qpack_field_t *f = &fields[i];
                if (pseudo_match(f, ":method")    ||
                    pseudo_match(f, ":scheme")    ||
                    pseudo_match(f, ":authority") ||
                    pseudo_match(f, ":path")) continue;
                uint8_t *nm = h3_dup_bytes(f->name,  f->name_len);
                uint8_t *vl = h3_dup_bytes(f->value, f->value_len);
                out[idx].name      = nm;
                out[idx].name_len  = f->name_len;
                out[idx].value     = vl;
                out[idx].value_len = f->value_len;
                idx++;
            }
            req->headers   = out;
            req->n_headers = extra_count;
        }
        req->state = N00B_H3_INBOUND_STATE_HEADERS_PARSED;
    } else if (frame->type == N00B_H3_FRAME_DATA) {
        if (frame->body_len > 0) {
            size_t old = (size_t)req->body->byte_len;
            n00b_buffer_resize(req->body,
                               (uint64_t)(old + frame->body_len));
            N00B_H3_DEBUG_POINT(req->chan, N00B_H3_DEBUG_BODY_GROWN);
            memcpy((uint8_t *)req->body->data + old,
                   frame->body, frame->body_len);
        }
    } else if (frame->type == N00B_H3_FRAME_PUSH_PROMISE) {
        /* Server can't receive PUSH_PROMISE on a request stream
         * (RFC 9114 § 7.2.5: only the server sends PUSH_PROMISE).
         * Treat as protocol error. */
        return n00b_result_err(bool, N00B_QUIC_ERR_PROTOCOL);
    } else if (n00b_h3_frame_type_is_grease(frame->type)) {
        /* Ignore. */
    } else {
        /* Unknown frame on a request stream: per RFC 9114 § 4.1
         * MUST ignore, so we let it pass quietly. */
    }
    return n00b_result_ok(bool, true);
}

static n00b_result_t(bool)
inbound_request_feed_locked(n00b_h3_inbound_request_t *req,
                            const uint8_t             *bytes,
                            size_t                     len,
                            bool                       fin)
{
    ireq_recv_append(req, bytes, len);

    n00b_h3_server_t *server = req->server_conn->server;

    while (req->recv_buf && req->recv_buf->byte_len > 0) {
        n00b_h3_frame_t frame;
        n00b_result_t(bool) pr = n00b_h3_frame_parse_bytes(
            (uint8_t *)req->recv_buf->data,
            req->recv_buf->byte_len, &frame,
            .max_size = server->max_frame_body);
        if (n00b_result_is_err(pr)) {
            int32_t e = n00b_result_get_err(pr);
            if (e == N00B_QUIC_ERR_NEED_MORE_DATA) break;
            return pr;
        }
        bool just_parsed_headers =
            (frame.type == N00B_H3_FRAME_HEADERS) &&
            (req->state == N00B_H3_INBOUND_STATE_NEW);
        n00b_result_t(bool) dr = process_inbound_request_frame(req, &frame);
        if (n00b_result_is_err(dr)) {
            return dr;
        }
        ireq_recv_consume(req, frame.consumed);

        /* Early-publish: as soon as HEADERS are decoded (before peer FIN),
         * publish the request so streaming handlers can start consuming
         * DATA frames. */
        if (just_parsed_headers && server->early_publish &&
            req->state == N00B_H3_INBOUND_STATE_HEADERS_PARSED) {
            publish_request_event(server, req);
            req->state = N00B_H3_INBOUND_STATE_PUBLISHED;
        }
    }

    if (fin) {
        req->peer_fin_seen = true;
        if (req->state == N00B_H3_INBOUND_STATE_HEADERS_PARSED) {
            req->state = N00B_H3_INBOUND_STATE_BODY_COMPLETE;
            publish_request_event(server, req);
            req->state = N00B_H3_INBOUND_STATE_PUBLISHED;
        }
    }
    return n00b_result_ok(bool, true);
}

static n00b_result_t(bool)
inbound_request_feed(n00b_h3_inbound_request_t *req,
                     const uint8_t             *bytes,
                     size_t                     len,
                     bool                       fin)
{
    if (!req) return n00b_result_err(bool, N00B_QUIC_ERR_NULL_ARG);

    // A handler reads this request's body and state under the same lock
    // from other threads.
    n00b_data_write_lock(req->lock);
    n00b_result_t(bool) r = inbound_request_feed_locked(req, bytes, len, fin);
    n00b_data_unlock(req->lock);
    return r;
}

/* ===========================================================================
 * Per-conn drive: discover inbound bidi streams + drain.
 * =========================================================================== */

static n00b_h3_inbound_request_t *
inbound_request_new(n00b_h3_server_conn_t *sconn, n00b_quic_chan_t *chan)
{
    n00b_h3_inbound_request_t *req = n00b_alloc_with_opts(
        n00b_h3_inbound_request_t,
        &(n00b_alloc_opts_t){ .allocator = n00b_h3_alloc() });
    req->lock = n00b_data_lock_new();
    req->server_conn = sconn;
    req->chan        = chan;
    req->state       = N00B_H3_INBOUND_STATE_NEW;

    n00b_buffer_t *body = n00b_alloc_with_opts(n00b_buffer_t,
                                &(n00b_alloc_opts_t){
                                    .allocator = n00b_h3_alloc(),
                                });
    n00b_buffer_init(body, .length = 0, .allocator = n00b_h3_alloc(),
                     .no_lock = true);
    req->body = body;
    return req;
}

/* Is this a client-initiated bidi stream?  RFC 9000 § 2.1: stream id
 * mod 4 == 0. */
static bool
is_client_initiated_bidi(uint64_t sid)
{
    return (sid & 0x3) == 0;
}

static void
sconn_discover_streams(n00b_h3_server_conn_t *sconn)
{
    n00b_quic_conn_t *conn = sconn->conn;
    n00b_quic_chan_t *c    = n00b_quic_conn_first_chan(conn);
    while (c) {
        n00b_quic_chan_t *nx = n00b_quic_chan_next_in_conn(c);

        /* Skip our own owned uni streams. */
        if (c == sconn->control_uni ||
            c == sconn->qpack_enc_uni ||
            c == sconn->qpack_dec_uni) {
            c = nx; continue;
        }
        /* Skip already-classified peer uni streams. */
        if (c == sconn->peer_control_uni ||
            c == sconn->peer_qpack_enc_uni ||
            c == sconn->peer_qpack_dec_uni) {
            c = nx; continue;
        }
        /* Skip already-attached request streams. */
        bool already = false;
        if (sconn->requests) {
            size_t nr = (size_t)n00b_list_len(*sconn->requests);
            for (size_t i = 0; i < nr; i++) {
                n00b_h3_inbound_request_t *r =
                    n00b_list_get(*sconn->requests, i);
                if (r->chan == c) { already = true; break; }
            }
        }
        if (already) { c = nx; continue; }

        uint64_t sid = n00b_quic_chan_id(c);
        if (n00b_quic_chan_kind(c) != N00B_QUIC_CHAN_FRAMED) {
            c = nx; continue;
        }

        /* Bidi vs uni: client-initiated bidi has sid % 4 == 0. */
        if (is_client_initiated_bidi(sid)) {
            /* New inbound request. */
            n00b_h3_inbound_request_t *req = inbound_request_new(sconn, c);
            if (!sconn->requests) {
                sconn->requests = n00b_alloc_with_opts(
                    n00b_list_t(n00b_h3_inbound_request_t *),
                    &(n00b_alloc_opts_t){.allocator = n00b_h3_alloc()});
                *sconn->requests =
                    n00b_list_new_private(n00b_h3_inbound_request_t *,
                                          .allocator = n00b_h3_alloc());
            }
            n00b_list_push(*sconn->requests, req);
            if (!sconn->has_seen_client_bidi || sid > sconn->max_seen_client_bidi_id) {
                sconn->has_seen_client_bidi = true;
                sconn->max_seen_client_bidi_id = sid;
            }
            c = nx; continue;
        }

        /* Otherwise: peer uni stream — try to read its kind varint. */
        if (!n00b_quic_chan_has_data(c)) { c = nx; continue; }

        uint8_t tmp[8];
        n00b_result_t(size_t) rr = n00b_quic_chan_recv(c, tmp, sizeof(tmp));
        if (n00b_result_is_err(rr)) { c = nx; continue; }
        size_t got = n00b_result_get(rr);
        if (got == 0) { c = nx; continue; }

        size_t copy = got;
        if ((size_t)sconn->peer_uni_kind_len + copy > 8) {
            copy = 8 - sconn->peer_uni_kind_len;
        }
        memcpy(sconn->peer_uni_kind_bytes + sconn->peer_uni_kind_len,
               tmp, copy);
        sconn->peer_uni_kind_len += (uint8_t)copy;

        uint64_t kind;
        int64_t  vn = n00b_h3_varint_decode(sconn->peer_uni_kind_bytes,
                                            sconn->peer_uni_kind_len,
                                            &kind);
        if (vn <= 0) {
            c = nx; continue;
        }

        sconn->peer_uni_kind_len = 0;

        switch ((n00b_h3_uni_stream_kind_t)kind) {
        case N00B_H3_UNI_CONTROL:
            sconn->peer_control_uni = c;
            break;
        case N00B_H3_UNI_QPACK_ENCODER:
            sconn->peer_qpack_enc_uni = c;
            break;
        case N00B_H3_UNI_QPACK_DECODER:
            sconn->peer_qpack_dec_uni = c;
            break;
        case N00B_H3_UNI_PUSH:
            /* Server should never receive a PUSH stream from the
             * client.  Per RFC 9114 § 6.2.2: only the server initiates
             * push streams.  STOP_SENDING. */
            (void)n00b_quic_chan_stop_sending(
                c,
                (uint64_t)N00B_H3_ERR_STREAM_CREATION);
            break;
        default:
            /* Unknown stream kind — abort it (RFC 9114 § 6.2). */
            (void)n00b_quic_chan_stop_sending(
                c,
                (uint64_t)N00B_H3_ERR_STREAM_CREATION);
            break;
        }
        // The bytes read past the varint go back in front of the rest.
        if ((size_t)vn < got) {
            _n00b_quic_chan_unread(c, tmp + vn, got - (size_t)vn);
        }
        c = nx;
    }
}

/* ===========================================================================
 * Per-uni-stream feeder (mirror of the client's drive_uni_stream_recv).
 * =========================================================================== */

static n00b_result_t(bool)
sconn_process_control_frame(n00b_h3_server_conn_t *sconn,
                             const n00b_h3_frame_t *frame)
{
    n00b_h3_server_t *server = sconn->server;
    if (frame->type == N00B_H3_FRAME_SETTINGS) {
        n00b_h3_settings_t s;
        n00b_result_t(bool) pr = n00b_h3_settings_parse(
            frame->body, frame->body_len, &s);
        if (n00b_result_is_err(pr)) return pr;
        /* Stash on the server (single peer-settings shape per server
         * is fine for v1 — every conn negotiates the same window). */
        (void)s;
        (void)server;
    } else if (frame->type == N00B_H3_FRAME_GOAWAY) {
        /* Client has issued GOAWAY: we should refuse new push streams
         * (we don't push) but otherwise honor in-flight requests.  No
         * action required for v1. */
    } else if (frame->type == N00B_H3_FRAME_MAX_PUSH_ID ||
               frame->type == N00B_H3_FRAME_CANCEL_PUSH) {
        /* Server push not supported in v1. */
    }
    return n00b_result_ok(bool, true);
}

static n00b_result_t(bool)
sconn_drive_uni_stream(n00b_h3_server_conn_t *sconn,
                        n00b_quic_chan_t      *chan,
                        n00b_buffer_t        **buf_ptr,
                        bool                   is_control,
                        bool                   is_qpack_enc_peer,
                        bool                   is_qpack_dec_peer)
{
    if (!chan) return n00b_result_ok(bool, true);
    n00b_h3_server_t *server = sconn->server;

    if (!*buf_ptr) {
        *buf_ptr = n00b_buffer_empty(.allocator = n00b_h3_alloc());
    }
    n00b_buffer_t *bb = *buf_ptr;

    uint8_t buf[4096];
    while (n00b_quic_chan_has_data(chan)) {
        n00b_result_t(size_t) rr = n00b_quic_chan_recv(chan, buf, sizeof(buf));
        if (n00b_result_is_err(rr)) break;
        size_t got = n00b_result_get(rr);
        if (got == 0) break;
        size_t old = bb->byte_len;
        n00b_buffer_resize(bb, old + got);
        memcpy(bb->data + old, buf, got);
    }

    if (is_qpack_enc_peer) {
        n00b_buffer_t ds;
        memset(&ds, 0, sizeof(ds));
        n00b_buffer_init(&ds, .length = 0, .allocator = n00b_h3_alloc(),
                         .no_lock = true);
        n00b_result_t(size_t) cr = n00b_qpack_decoder_consume_encoder_stream(
            server->qpack_dec,
            (uint8_t *)bb->data, bb->byte_len, &ds);
        if (n00b_result_is_err(cr)) {
            return n00b_result_err(bool, n00b_result_get_err(cr));
        }
        size_t n = n00b_result_get(cr);
        if (n > 0) {
            memmove(bb->data, bb->data + n, bb->byte_len - n);
            n00b_buffer_resize(bb, bb->byte_len - n);
        }
        if (ds.byte_len > 0 && sconn->qpack_dec_uni) {
            n00b_quic_chan_send(sconn->qpack_dec_uni,
                                 (const uint8_t *)ds.data,
                                 (size_t)ds.byte_len);
        }
        return n00b_result_ok(bool, true);
    }

    if (is_qpack_dec_peer) {
        n00b_result_t(size_t) cr = n00b_qpack_encoder_consume_decoder_stream(
            server->qpack_enc,
            (uint8_t *)bb->data, bb->byte_len);
        if (n00b_result_is_err(cr)) {
            return n00b_result_err(bool, n00b_result_get_err(cr));
        }
        size_t n = n00b_result_get(cr);
        if (n > 0) {
            memmove(bb->data, bb->data + n, bb->byte_len - n);
            n00b_buffer_resize(bb, bb->byte_len - n);
        }
        return n00b_result_ok(bool, true);
    }

    if (is_control) {
        size_t off = 0;
        while (off < bb->byte_len) {
            n00b_h3_frame_t frame;
            n00b_result_t(bool) pr = n00b_h3_frame_parse_bytes(
                (uint8_t *)bb->data + off, bb->byte_len - off, &frame,
                .max_size = server->max_frame_body);
            if (n00b_result_is_err(pr)) {
                int32_t e = n00b_result_get_err(pr);
                if (e == N00B_QUIC_ERR_NEED_MORE_DATA) break;
                return pr;
            }
            n00b_result_t(bool) dr = sconn_process_control_frame(sconn, &frame);
            if (n00b_result_is_err(dr)) return dr;
            off += frame.consumed;
        }
        if (off > 0) {
            memmove(bb->data, bb->data + off, bb->byte_len - off);
            n00b_buffer_resize(bb, bb->byte_len - off);
        }
        return n00b_result_ok(bool, true);
    }

    return n00b_result_ok(bool, true);
}

static n00b_result_t(bool)
sconn_drive_request_streams(n00b_h3_server_conn_t *sconn)
{
    if (!sconn->requests) return n00b_result_ok(bool, true);
    size_t nr = (size_t)n00b_list_len(*sconn->requests);
    for (size_t ri = 0; ri < nr; ri++) {
        n00b_h3_inbound_request_t *req =
            n00b_list_get(*sconn->requests, ri);
        if (!req->chan) continue;
        if (req->state == N00B_H3_INBOUND_STATE_RESET) continue;

        uint8_t buf[4096];
        while (n00b_quic_chan_has_data(req->chan)) {
            n00b_result_t(size_t) rr = n00b_quic_chan_recv(req->chan,
                                                            buf, sizeof(buf));
            if (n00b_result_is_err(rr)) break;
            size_t got = n00b_result_get(rr);
            if (got == 0) break;
            n00b_result_t(bool) fr = inbound_request_feed(req, buf, got, false);
            if (n00b_result_is_err(fr)) {
                /* Reset the stream and surface the error.  We do NOT
                 * fail the whole conn — bad headers on one request
                 * shouldn't take down the others. */
                int32_t e = n00b_result_get_err(fr);
                uint64_t app_err = (e == N00B_QUIC_ERR_PROTOCOL)
                                      ? (uint64_t)N00B_H3_ERR_MESSAGE_ERROR
                                      : (uint64_t)N00B_H3_ERR_INTERNAL_ERROR;
                (void)n00b_quic_chan_reset(req->chan, app_err);
                req->state = N00B_H3_INBOUND_STATE_RESET;
                break;
            }
        }
        bool peer_fin = n00b_quic_chan_recv_fin(req->chan);
        if (peer_fin && !req->peer_fin_seen) {
            (void)inbound_request_feed(req, nullptr, 0, true);
        }
    }
    return n00b_result_ok(bool, true);
}

n00b_result_t(bool)
n00b_h3_server_conn_drive(n00b_h3_server_conn_t *sconn)
{
    if (!sconn) return n00b_result_err(bool, N00B_QUIC_ERR_NULL_ARG);
    sconn_discover_streams(sconn);

    n00b_result_t(bool) r;
    r = sconn_drive_uni_stream(sconn, sconn->peer_control_uni,
                                &sconn->peer_ctrl_buf,
                                true, false, false);
    if (n00b_result_is_err(r)) return r;

    r = sconn_drive_uni_stream(sconn, sconn->peer_qpack_enc_uni,
                                &sconn->peer_qe_buf,
                                false, true, false);
    if (n00b_result_is_err(r)) return r;

    r = sconn_drive_uni_stream(sconn, sconn->peer_qpack_dec_uni,
                                &sconn->peer_qd_buf,
                                false, false, true);
    if (n00b_result_is_err(r)) return r;

    return sconn_drive_request_streams(sconn);
}

n00b_result_t(bool)
n00b_h3_server_drive(n00b_h3_server_t *server)
{
    if (!server) return n00b_result_err(bool, N00B_QUIC_ERR_NULL_ARG);
    n00b_data_write_lock(server->lock);
    if (server->closed) {
        n00b_data_unlock(server->lock);
        return n00b_result_ok(bool, true);
    }

    drain_accept_inbox(server);

    if (server->conns) {
        size_t nc = (size_t)n00b_list_len(*server->conns);
        for (size_t i = 0; i < nc; i++) {
            n00b_h3_server_conn_t *sc =
                n00b_list_get(*server->conns, i);
            if (sc->closed) continue;
            n00b_result_t(bool) r = n00b_h3_server_conn_drive(sc);
            if (n00b_result_is_err(r)) {
                n00b_data_unlock(server->lock);
                return r;
            }
        }
    }
    n00b_data_unlock(server->lock);
    return n00b_result_ok(bool, true);
}

/* ===========================================================================
 * GOAWAY emission on close.
 *
 * RFC 9114 § 5.2: server emits GOAWAY whose body is the
 * lowest-stream-id-of-an-unprocessed-client-initiated-bidi.  In our
 * implementation we use `(highest-seen + 4)` as a conservative limit
 * — the next stream id the client could have used (4-step because
 * client-initiated bidi stream IDs are 0, 4, 8, ...).
 *
 * If we never saw a request, the limit is 0 (refusing all future
 * requests on this connection — appropriate when the server is
 * winding down).
 * =========================================================================== */

static void
sconn_emit_goaway(n00b_h3_server_conn_t *sconn)
{
    if (!sconn->control_uni || sconn->goaway_sent) return;

    uint64_t limit;
    if (sconn->has_seen_client_bidi) {
        limit = sconn->max_seen_client_bidi_id + 4;
    } else {
        limit = 0;
    }

    /* Build varint(limit) body. */
    n00b_buffer_t body;
    memset(&body, 0, sizeof(body));
    n00b_buffer_init(&body, .length = 0, .allocator = n00b_h3_alloc(),
                     .no_lock = true);
    if (n00b_h3_varint_append(&body, limit) == 0) return;

    n00b_buffer_t frame;
    memset(&frame, 0, sizeof(frame));
    n00b_buffer_init(&frame, .length = 0, .allocator = n00b_h3_alloc(),
                     .no_lock = true);
    n00b_result_t(bool) fr = n00b_h3_frame_emit(
        &frame, N00B_H3_FRAME_GOAWAY,
        (const uint8_t *)body.data, (size_t)body.byte_len);
    if (n00b_result_is_err(fr)) return;

    n00b_quic_chan_send(sconn->control_uni,
                         (const uint8_t *)frame.data,
                         (size_t)frame.byte_len);
    sconn->goaway_sent = true;
}

void
n00b_h3_server_close(n00b_h3_server_t *server)
{
    if (!server) return;
    n00b_data_write_lock(server->lock);
    if (server->closed) {
        n00b_data_unlock(server->lock);
        return;
    }
    server->closed = true;

    /* GOAWAY each conn + close uni streams + cancel pending inbound
     * requests. */
    if (server->conns) {
        size_t nc = (size_t)n00b_list_len(*server->conns);
        for (size_t i = 0; i < nc; i++) {
            n00b_h3_server_conn_t *sc =
                n00b_list_get(*server->conns, i);
            sconn_emit_goaway(sc);
            if (sc->control_uni)   n00b_quic_chan_close(sc->control_uni);
            if (sc->qpack_enc_uni) n00b_quic_chan_close(sc->qpack_enc_uni);
            if (sc->qpack_dec_uni) n00b_quic_chan_close(sc->qpack_dec_uni);
            if (sc->requests) {
                size_t nr = (size_t)n00b_list_len(*sc->requests);
                for (size_t ri = 0; ri < nr; ri++) {
                    n00b_h3_inbound_request_t *r =
                        n00b_list_get(*sc->requests, ri);
                    if (r->chan && r->state != N00B_H3_INBOUND_STATE_RESET &&
                        r->state != N00B_H3_INBOUND_STATE_RESPONDED) {
                        /* RFC 9114 §4.1.1: server signals unprocessed
                         * requests with H3_REQUEST_REJECTED so the
                         * peer's HTTP/3 stack knows to retry on a
                         * different connection.  chan_close (graceful
                         * FIN-then-close) would have the peer thinking
                         * the request was processed. */
                        (void)n00b_quic_chan_reset(r->chan,
                                            N00B_H3_ERR_REQUEST_REJECTED);
                        r->state = N00B_H3_INBOUND_STATE_RESET;
                    }
                }
            }
            sc->closed = true;
        }
    }

    n00b_qpack_encoder_close(server->qpack_enc);
    n00b_qpack_decoder_close(server->qpack_dec);

    n00b_data_unlock(server->lock);
}

/* ===========================================================================
 * Inbound request — accessors.
 * =========================================================================== */

const char *
n00b_h3_inbound_request_method(n00b_h3_inbound_request_t *req)
{
    return req ? req->method : nullptr;
}

const char *
n00b_h3_inbound_request_scheme(n00b_h3_inbound_request_t *req)
{
    return req ? req->scheme : nullptr;
}

const char *
n00b_h3_inbound_request_authority(n00b_h3_inbound_request_t *req)
{
    return req ? req->authority : nullptr;
}

const char *
n00b_h3_inbound_request_path(n00b_h3_inbound_request_t *req)
{
    return req ? req->path : nullptr;
}

const n00b_h3_header_t *
n00b_h3_inbound_request_headers(n00b_h3_inbound_request_t *req,
                                size_t                    *n_out)
{
    if (!req) {
        if (n_out) *n_out = 0;
        return nullptr;
    }
    if (n_out) *n_out = req->n_headers;
    return req->headers;
}

n00b_buffer_t *
n00b_h3_inbound_request_body(n00b_h3_inbound_request_t *req)
{
    return req ? req->body : nullptr;
}

uint64_t
n00b_h3_inbound_request_stream_id(n00b_h3_inbound_request_t *req)
{
    if (!req || !req->chan) return UINT64_MAX;
    return n00b_quic_chan_id(req->chan);
}

bool
n00b_h3_inbound_request_peer_addr(n00b_h3_inbound_request_t *req,
                                  struct sockaddr_storage   *out)
{
    if (!req || !req->chan || !out) return false;
    n00b_quic_conn_t *conn = n00b_quic_chan_conn(req->chan);
    return n00b_quic_conn_peer_addr(conn, out);
}

/* ===========================================================================
 * Inbound request — respond.
 *
 * Build a HEADERS frame whose field section starts with `:status` and
 * is followed by the application's extra response headers; QPACK-encode
 * it; emit (HEADERS frame || optional DATA frame || optional FIN) on
 * the request stream.
 *
 * QPACK encoder may push insertions onto the encoder uni stream; we
 * forward those bytes first before HEADERS, mirroring the client's
 * pattern.
 * =========================================================================== */

/* Internal: emit only the HEADERS frame (no body).  Shared between the
 * one-shot respond path and the streaming send-headers helper.  Caller
 * has already validated state + arguments. */
static n00b_result_t(bool)
ireq_emit_headers_internal(n00b_h3_inbound_request_t *req,
                            uint16_t                   status,
                            const n00b_h3_header_t    *headers,
                            size_t                     n_headers,
                            bool                       fin_after_headers)
{
    n00b_h3_server_conn_t *sconn  = req->server_conn;
    n00b_h3_server_t      *server = sconn->server;

    char   status_buf[8];
    size_t n = h3_write_u64_decimal(status_buf,
                                    sizeof(status_buf),
                                    (uint64_t)status);
    if (n == 0) return n00b_result_err(bool, N00B_QUIC_ERR_PROTOCOL);

    size_t total_fields = 1 + n_headers;
    n00b_qpack_field_t *qfields = n00b_alloc_array_with_opts(
        n00b_qpack_field_t, (int64_t)total_fields,
        &(n00b_alloc_opts_t){ .allocator = n00b_h3_alloc() });

    qfields[0].name      = (const uint8_t *)":status";
    qfields[0].name_len  = strlen(":status");
    qfields[0].value     = (const uint8_t *)status_buf;
    qfields[0].value_len = (size_t)n;

    for (size_t i = 0; i < n_headers; i++) {
        qfields[1 + i].name      = headers[i].name;
        qfields[1 + i].name_len  = headers[i].name_len;
        qfields[1 + i].value     = headers[i].value;
        qfields[1 + i].value_len = headers[i].value_len;
    }

    n00b_buffer_t section, es;
    memset(&section, 0, sizeof(section));
    memset(&es, 0, sizeof(es));
    n00b_buffer_init(&section, .length = 0, .allocator = n00b_h3_alloc(),
                     .no_lock = true);
    n00b_buffer_init(&es, .length = 0, .allocator = n00b_h3_alloc(),
                     .no_lock = true);

    uint64_t stream_id = n00b_quic_chan_id(req->chan);
    n00b_result_t(bool) er = n00b_qpack_encode(
        server->qpack_enc, stream_id, qfields, total_fields, &section, &es);
    if (n00b_result_is_err(er)) {
        return n00b_result_err(bool, n00b_result_get_err(er));
    }

    if (es.byte_len > 0 && sconn->qpack_enc_uni) {
        /* Queued path: handlers may run on any thread; the I/O
         * thread replays this onto picoquic via run_once. */
        n00b_result_t(bool) ssr = n00b_quic_chan_send_queued(
            sconn->qpack_enc_uni,
            (const uint8_t *)es.data, (size_t)es.byte_len);
        if (n00b_result_is_err(ssr)) {
            return n00b_result_err(bool, n00b_result_get_err(ssr));
        }
    }

    n00b_buffer_t headers_frame;
    memset(&headers_frame, 0, sizeof(headers_frame));
    n00b_buffer_init(&headers_frame, .length = 0,
                     .allocator = n00b_h3_alloc(), .no_lock = true);
    n00b_result_t(bool) hf = n00b_h3_frame_emit(
        &headers_frame,
        N00B_H3_FRAME_HEADERS,
        (const uint8_t *)section.data, (size_t)section.byte_len);
    if (n00b_result_is_err(hf)) {
        return n00b_result_err(bool, n00b_result_get_err(hf));
    }

    n00b_result_t(bool) sr = n00b_quic_chan_send_queued(
        req->chan, (const uint8_t *)headers_frame.data,
        (size_t)headers_frame.byte_len, .fin = fin_after_headers);
    if (n00b_result_is_err(sr)) {
        return n00b_result_err(bool, n00b_result_get_err(sr));
    }

    req->response_headers_sent = true;
    if (fin_after_headers) {
        req->response_sent = true;
        req->state         = N00B_H3_INBOUND_STATE_RESPONDED;
    }
    return n00b_result_ok(bool, true);
}

/* Internal: emit a single DATA frame, optionally with FIN. */
static n00b_result_t(bool)
ireq_emit_data_internal(n00b_h3_inbound_request_t *req,
                         const uint8_t             *body,
                         size_t                     body_len,
                         bool                       fin)
{
    n00b_buffer_t data_frame;
    memset(&data_frame, 0, sizeof(data_frame));
    n00b_buffer_init(&data_frame, .length = 0,
                     .allocator = n00b_h3_alloc(), .no_lock = true);
    n00b_result_t(bool) df = n00b_h3_frame_emit(
        &data_frame, N00B_H3_FRAME_DATA, body, body_len);
    if (n00b_result_is_err(df)) {
        return n00b_result_err(bool, n00b_result_get_err(df));
    }
    n00b_result_t(bool) ds = n00b_quic_chan_send_queued(
        req->chan, (const uint8_t *)data_frame.data,
        (size_t)data_frame.byte_len, .fin = fin);
    if (n00b_result_is_err(ds)) {
        return n00b_result_err(bool, n00b_result_get_err(ds));
    }
    if (fin) {
        req->response_sent = true;
        req->state         = N00B_H3_INBOUND_STATE_RESPONDED;
    }
    return n00b_result_ok(bool, true);
}

n00b_result_t(bool)
n00b_h3_inbound_request_respond(n00b_h3_inbound_request_t *req,
                                n00b_h3_response_spec_t    response)
{
    return n00b_h3_inbound_request_respond_raw(
        req,
        response.status,
        response.headers.items,
        response.headers.count,
        response.body,
        response.body_len,
        .fin = !response.keep_open);
}

n00b_result_t(bool)
n00b_h3_inbound_request_respond_raw(n00b_h3_inbound_request_t *req,
                                uint16_t                   status,
                                const n00b_h3_header_t    *headers,
                                size_t                     n_headers,
                                const uint8_t             *body,
                                size_t                     body_len) _kargs
{
    bool fin = true;
}
{
    if (!req) return n00b_result_err(bool, N00B_QUIC_ERR_NULL_ARG);
    if (status == 0 || status > 999) {
        return n00b_result_err(bool, N00B_QUIC_ERR_PROTOCOL);
    }
    if (!headers && n_headers > 0) {
        return n00b_result_err(bool, N00B_QUIC_ERR_NULL_ARG);
    }
    if (!body && body_len > 0) {
        return n00b_result_err(bool, N00B_QUIC_ERR_NULL_ARG);
    }
    if (req->state == N00B_H3_INBOUND_STATE_RESET) {
        return n00b_result_err(bool, N00B_QUIC_ERR_LOCAL_RESET);
    }
    if (req->response_sent) {
        return n00b_result_err(bool, N00B_QUIC_ERR_PROTOCOL);
    }

    /* Hold the server lock for the headers + body emit pair.  The
     * qpack encoder dynamic table mutates inside ireq_emit_headers_
     * internal — concurrent handler threads on the same server
     * would otherwise corrupt it.  Mirrors the client-side issuance
     * lock added in chunk-5b. */
    n00b_h3_server_t *server = req->server_conn->server;
    n00b_data_write_lock(server->lock);

    n00b_result_t(bool) hr = ireq_emit_headers_internal(
        req, status, headers, n_headers,
        /* fin_after_headers = */ (fin && body_len == 0));
    if (n00b_result_is_err(hr)) {
        n00b_data_unlock(server->lock);
        return hr;
    }

    if (body_len > 0) {
        n00b_result_t(bool) dr = ireq_emit_data_internal(
            req, body, body_len, fin);
        if (n00b_result_is_err(dr)) {
            n00b_data_unlock(server->lock);
            return dr;
        }
    }

    n00b_data_unlock(server->lock);
    return n00b_result_ok(bool, true);
}

n00b_result_t(bool)
n00b_h3_inbound_request_send_headers(n00b_h3_inbound_request_t *req,
                                     n00b_h3_response_headers_t headers)
{
    return n00b_h3_inbound_request_send_headers_raw(
        req,
        headers.status,
        headers.headers.items,
        headers.headers.count,
        .fin = headers.fin);
}

n00b_result_t(bool)
n00b_h3_inbound_request_send_headers_raw(n00b_h3_inbound_request_t *req,
                                     uint16_t                   status,
                                     const n00b_h3_header_t    *headers,
                                     size_t                     n_headers) _kargs
{
    bool fin = false;
}
{
    if (!req) return n00b_result_err(bool, N00B_QUIC_ERR_NULL_ARG);
    if (status == 0 || status > 999) {
        return n00b_result_err(bool, N00B_QUIC_ERR_PROTOCOL);
    }
    if (!headers && n_headers > 0) {
        return n00b_result_err(bool, N00B_QUIC_ERR_NULL_ARG);
    }
    if (req->state == N00B_H3_INBOUND_STATE_RESET) {
        return n00b_result_err(bool, N00B_QUIC_ERR_LOCAL_RESET);
    }
    if (req->response_headers_sent || req->response_sent) {
        return n00b_result_err(bool, N00B_QUIC_ERR_PROTOCOL);
    }
    n00b_h3_server_t *server = req->server_conn->server;
    n00b_data_write_lock(server->lock);
    n00b_result_t(bool) r = ireq_emit_headers_internal(
        req, status, headers, n_headers, fin);
    n00b_data_unlock(server->lock);
    return r;
}

n00b_result_t(bool)
n00b_h3_inbound_request_send_data(n00b_h3_inbound_request_t *req,
                                  n00b_h3_data_t             data)
{
    return n00b_h3_inbound_request_send_data_raw(req,
                                                data.body,
                                                data.body_len,
                                                data.fin);
}

n00b_result_t(bool)
n00b_h3_inbound_request_send_data_raw(n00b_h3_inbound_request_t *req,
                                      const uint8_t             *body,
                                      size_t                     body_len,
                                      bool                       fin)
{
    if (!req) return n00b_result_err(bool, N00B_QUIC_ERR_NULL_ARG);
    if (!body && body_len > 0) {
        return n00b_result_err(bool, N00B_QUIC_ERR_NULL_ARG);
    }
    if (req->state == N00B_H3_INBOUND_STATE_RESET) {
        return n00b_result_err(bool, N00B_QUIC_ERR_LOCAL_RESET);
    }
    if (!req->response_headers_sent) {
        /* Spec violation: DATA must follow HEADERS on a request stream. */
        return n00b_result_err(bool, N00B_QUIC_ERR_PROTOCOL);
    }
    if (req->response_sent) {
        return n00b_result_err(bool, N00B_QUIC_ERR_PROTOCOL);
    }
    return ireq_emit_data_internal(req, body, body_len, fin);
}

size_t
n00b_h3_inbound_request_consume_body(n00b_h3_inbound_request_t *req,
                                     uint8_t                   *out,
                                     size_t                     max)
{
    if (!req || !req->body || max == 0) return 0;
    n00b_data_write_lock(req->lock);
    size_t avail = (size_t)req->body->byte_len - req->body_consumed;
    size_t copy  = avail < max ? avail : max;
    if (copy > 0) {
        memcpy(out,
               (const uint8_t *)req->body->data + req->body_consumed, copy);
        req->body_consumed += copy;
    }
    n00b_data_unlock(req->lock);
    return copy;
}

bool
n00b_h3_inbound_request_peer_fin(n00b_h3_inbound_request_t *req)
{
    if (!req) return false;
    n00b_data_write_lock(req->lock);
    bool f = req->peer_fin_seen;
    n00b_data_unlock(req->lock);
    return f;
}

bool
n00b_h3_inbound_request_is_reset(n00b_h3_inbound_request_t *req)
{
    if (!req) return false;
    n00b_data_write_lock(req->lock);
    bool r = (req->state == N00B_H3_INBOUND_STATE_RESET);
    if (!r && req->chan) {
        n00b_quic_chan_state_t cs = n00b_quic_chan_state(req->chan);
        if (cs == N00B_QUIC_CHAN_STATE_PEER_RESET ||
            cs == N00B_QUIC_CHAN_STATE_LOCAL_RESET) {
            req->state = N00B_H3_INBOUND_STATE_RESET;
            r = true;
        }
    }
    n00b_data_unlock(req->lock);
    return r;
}

void
n00b_h3_inbound_request_reset(n00b_h3_inbound_request_t *req,
                              uint64_t                   app_err)
{
    if (!req) return;
    n00b_data_write_lock(req->lock);
    if (req->chan) {
        (void)n00b_quic_chan_reset(req->chan, app_err);
    }
    req->state = N00B_H3_INBOUND_STATE_RESET;
    n00b_data_unlock(req->lock);
}
