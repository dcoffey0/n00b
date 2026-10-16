/*
 * h3_client.c — HTTP/3 client lifecycle (RFC 9114).
 *
 * Wires `n00b_h3_client_t` on top of an already-connected
 * `n00b_quic_conn_t` (ALPN must have negotiated to "h3").
 *
 * The client owns three uni streams it generates:
 *   - Control stream (kind 0x00) — carries SETTINGS + GOAWAY.
 *   - QPACK encoder stream (kind 0x02) — outbound encoder
 *     instructions.
 *   - QPACK decoder stream (kind 0x03) — outbound section ack +
 *     insert-count-increment.
 *
 * Each request is one bidi stream.  HEADERS + DATA frames flow on
 * the same stream; the response comes back as HEADERS + DATA.
 *
 * IO model: the client is "pumped" by `n00b_h3_client_drive`.  That
 * call drains the recv buffers of all known channels, parses any
 * complete frames, dispatches them, and queues outbound bytes.  The
 * caller is responsible for separately pumping the underlying
 * picoquic IO via `n00b_quic_endpoint_run_once`.
 *
 * Threading: a single client mutex guards the lifecycle + per-request
 * state.  Per-request mutexes guard recv buffering during drive().
 */
#define N00B_USE_INTERNAL_API
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/buffer.h"
#include "core/string.h"
#include "adt/option.h"
#include "adt/result.h"
#include "util/parse_num.h"
#include "net/quic/quic_types.h"
#include "net/quic/conn.h"
#include "net/quic/chan.h"
#include "net/quic/endpoint.h"
#include "net/quic/qpack.h"
#include "net/quic/h3.h"
#include "net/quic/h3_types.h"
#include "internal/net/quic/h3_internal.h"
#include "internal/net/quic/chan_internal.h"

#ifdef N00B_DEBUG
void (*_n00b_h3_debug_hook)(struct n00b_quic_chan *chan, n00b_h3_debug_point_t point) = nullptr;
#endif

/* ===========================================================================
 * Allocation helpers (conduit_pool throughout)
 * =========================================================================== */

static uint8_t *
h3_dup_bytes(const uint8_t *src, size_t len)
{
    if (len == 0) {
        uint8_t *p = n00b_alloc_array_with_opts(uint8_t, 1,
                            &(n00b_alloc_opts_t){
                                .allocator = n00b_h3_alloc(),
                                .no_scan   = true,
                            });
        return p;
    }
    uint8_t *p = n00b_alloc_array_with_opts(uint8_t, (int64_t)len,
                        &(n00b_alloc_opts_t){
                            .allocator = n00b_h3_alloc(),
                            .no_scan   = true,
                        });
    memcpy(p, src, len);
    return p;
}

/* ===========================================================================
 * Recv-buffer growth (per-request)
 * =========================================================================== */

static void
req_recv_append(n00b_h3_request_t *req, const uint8_t *bytes, size_t len)
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
req_recv_consume(n00b_h3_request_t *req, size_t n)
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
 * Uni-stream init
 *
 * Each uni stream begins with a single varint identifying its kind
 * (RFC 9114 § 6.2).  We send that varint as the very first bytes on
 * the stream right after open.
 * =========================================================================== */

n00b_result_t(bool)
n00b_h3_client_init_uni_stream(n00b_h3_client_t          *client,
                               n00b_quic_chan_t          *chan,
                               n00b_h3_uni_stream_kind_t  kind)
{
    (void)client;
    /* Build a tiny scratch buffer holding the varint, then send it. */
    n00b_buffer_t b;
    memset(&b, 0, sizeof(b));
    n00b_buffer_init(&b, .length = 0, .allocator = n00b_h3_alloc(),
                     .no_lock = true);
    if (n00b_h3_varint_append(&b, (uint64_t)kind) == 0) {
        return n00b_result_err(bool, N00B_QUIC_ERR_PROTOCOL);
    }

    n00b_result_t(size_t) sr = n00b_quic_chan_send(chan, (const uint8_t *)b.data,
                                                    (size_t)b.byte_len);
    if (n00b_result_is_err(sr)) {
        return n00b_result_err(bool, n00b_result_get_err(sr));
    }
    return n00b_result_ok(bool, true);
}

/* ===========================================================================
 * SETTINGS dispatch
 * =========================================================================== */

static n00b_result_t(bool)
emit_initial_settings(n00b_h3_client_t *client)
{
    /* Build SETTINGS body. */
    n00b_buffer_t body;
    memset(&body, 0, sizeof(body));
    n00b_buffer_init(&body, .length = 0, .allocator = n00b_h3_alloc(),
                     .no_lock = true);

    n00b_result_t(bool) br = n00b_h3_settings_emit_body(
        &body,
        client->local_settings.qpack_max_table_capacity,
        client->local_settings.qpack_blocked_streams,
        client->local_settings.max_field_section_size);
    if (n00b_result_is_err(br)) {
        return br;
    }

    /* Wrap in a SETTINGS frame envelope. */
    n00b_buffer_t frame;
    memset(&frame, 0, sizeof(frame));
    n00b_buffer_init(&frame, .length = 0, .allocator = n00b_h3_alloc(),
                     .no_lock = true);

    n00b_result_t(bool) fr = n00b_h3_frame_emit(
        &frame,
        N00B_H3_FRAME_SETTINGS,
        (const uint8_t *)body.data, (size_t)body.byte_len);
    if (n00b_result_is_err(fr)) {
        return fr;
    }

    /* Send on control stream (which already starts with the kind byte). */
    n00b_result_t(size_t) sr = n00b_quic_chan_send(
        client->control_uni,
        (const uint8_t *)frame.data, (size_t)frame.byte_len);
    if (n00b_result_is_err(sr)) {
        return n00b_result_err(bool, n00b_result_get_err(sr));
    }
    return n00b_result_ok(bool, true);
}

/* ===========================================================================
 * Client lifecycle
 * =========================================================================== */

n00b_result_t(n00b_h3_client_t *)
n00b_h3_client_new(n00b_quic_conn_t *conn) _kargs
{
    size_t   max_frame_body          = N00B_H3_DEFAULT_MAX_FRAME_BODY;
    uint64_t qpack_max_table_capacity = 4096;
    uint64_t qpack_blocked_streams    = 0;
}
{
    if (!conn) {
        return n00b_result_err(n00b_h3_client_t *, N00B_QUIC_ERR_NULL_ARG);
    }

    n00b_h3_client_t *client = n00b_alloc_with_opts(n00b_h3_client_t,
                                    &(n00b_alloc_opts_t){
                                        .allocator = n00b_h3_alloc(),
                                    });
    client->lock = n00b_data_lock_new();
    client->conn           = conn;
    client->max_frame_body = max_frame_body;
    client->local_settings.qpack_max_table_capacity = qpack_max_table_capacity;
    client->local_settings.qpack_blocked_streams    = qpack_blocked_streams;
    client->local_settings.max_field_section_size   = 0;
    client->local_settings.received                 = false;
    memset(&client->peer_settings, 0, sizeof(client->peer_settings));

    client->qpack_enc = n00b_qpack_encoder_new(qpack_max_table_capacity,
                                                qpack_blocked_streams);
    client->qpack_dec = n00b_qpack_decoder_new(qpack_max_table_capacity,
                                                qpack_blocked_streams);

    /* Open the three uni streams.  We tag them with kind bytes
     * immediately so the peer knows what to do with them. */
    n00b_result_t(n00b_quic_chan_t *) cr;

    cr = n00b_quic_chan_open(conn, .bidi = false);
    if (n00b_result_is_err(cr)) {
        return n00b_result_err(n00b_h3_client_t *, n00b_result_get_err(cr));
    }
    client->control_uni = n00b_result_get(cr);
    (void)n00b_h3_client_init_uni_stream(client, client->control_uni,
                                         N00B_H3_UNI_CONTROL);

    cr = n00b_quic_chan_open(conn, .bidi = false);
    if (n00b_result_is_err(cr)) {
        return n00b_result_err(n00b_h3_client_t *, n00b_result_get_err(cr));
    }
    client->qpack_enc_uni = n00b_result_get(cr);
    (void)n00b_h3_client_init_uni_stream(client, client->qpack_enc_uni,
                                         N00B_H3_UNI_QPACK_ENCODER);

    cr = n00b_quic_chan_open(conn, .bidi = false);
    if (n00b_result_is_err(cr)) {
        return n00b_result_err(n00b_h3_client_t *, n00b_result_get_err(cr));
    }
    client->qpack_dec_uni = n00b_result_get(cr);
    (void)n00b_h3_client_init_uni_stream(client, client->qpack_dec_uni,
                                         N00B_H3_UNI_QPACK_DECODER);

    /* Emit our SETTINGS. */
    n00b_result_t(bool) sr = emit_initial_settings(client);
    if (n00b_result_is_err(sr)) {
        return n00b_result_err(n00b_h3_client_t *, n00b_result_get_err(sr));
    }
    client->local_settings_sent = true;

    return n00b_result_ok(n00b_h3_client_t *, client);
}

bool
n00b_h3_client_is_closed(n00b_h3_client_t *client)
{
    if (!client) return true;
    n00b_data_write_lock(client->lock);
    bool c = client->closed;
    n00b_data_unlock(client->lock);
    return c;
}

void
n00b_h3_client_close(n00b_h3_client_t *client)
{
    if (!client) return;
    n00b_data_write_lock(client->lock);
    if (client->closed) {
        n00b_data_unlock(client->lock);
        return;
    }
    client->closed = true;
    n00b_data_unlock(client->lock);

    /* Close uni streams. */
    if (client->control_uni)   n00b_quic_chan_close(client->control_uni);
    if (client->qpack_enc_uni) n00b_quic_chan_close(client->qpack_enc_uni);
    if (client->qpack_dec_uni) n00b_quic_chan_close(client->qpack_dec_uni);

    /* Cancel any open requests. */
    if (client->requests) {
        size_t nr = (size_t)n00b_list_len(*client->requests);
        for (size_t i = 0; i < nr; i++) {
            n00b_h3_request_t *r = n00b_list_get(*client->requests, i);
            if (r->chan) n00b_quic_chan_close(r->chan);
        }
    }

    n00b_qpack_encoder_close(client->qpack_enc);
    n00b_qpack_decoder_close(client->qpack_dec);

    
}

n00b_h3_settings_t
n00b_h3_client_peer_settings(n00b_h3_client_t *client)
{
    n00b_h3_settings_t s = {0};
    if (!client) return s;
    n00b_data_write_lock(client->lock);
    s = client->peer_settings;
    n00b_data_unlock(client->lock);
    return s;
}

/* ===========================================================================
 * Request issuance
 * =========================================================================== */

n00b_result_t(n00b_h3_request_t *)
n00b_h3_client_request(n00b_h3_client_t *client,
                       n00b_h3_request_spec_t request)
{
    const char *scheme = request.scheme ? request.scheme : "https";
    const char *path   = request.path ? request.path : "/";

    return n00b_h3_client_request_raw(
        client,
        request.method,
        scheme,
        request.authority,
        path,
        .extra_headers = request.headers.items,
        .n_extra       = request.headers.count,
        .body          = request.body,
        .body_len      = request.body_len,
        .fin           = !request.keep_open);
}

n00b_result_t(n00b_h3_request_t *)
n00b_h3_client_request_raw(n00b_h3_client_t *client,
                       const char       *method,
                       const char       *scheme,
                       const char       *authority,
                       const char       *path) _kargs
{
    const n00b_h3_header_t *extra_headers = nullptr;
    size_t                  n_extra       = 0;
    const uint8_t          *body          = nullptr;
    size_t                  body_len      = 0;
    bool                    fin           = true;
}
{
    if (!client || !method || !scheme || !authority || !path) {
        return n00b_result_err(n00b_h3_request_t *, N00B_QUIC_ERR_NULL_ARG);
    }
    if (!extra_headers && n_extra > 0) {
        return n00b_result_err(n00b_h3_request_t *, N00B_QUIC_ERR_NULL_ARG);
    }
    if (!body && body_len > 0) {
        return n00b_result_err(n00b_h3_request_t *, N00B_QUIC_ERR_NULL_ARG);
    }

    /* Hold the client lock for the full issuance critical section.
     * The qpack encoder dynamic table + the encoder uni-stream + the
     * new request stream's HEADERS/DATA frames must all advance
     * atomically with respect to other threads issuing requests on
     * the same client; otherwise concurrent qpack_encode calls
     * corrupt the dyn table and the server's QPACK decoder loses
     * sync.  The driver thread's drive() also takes this lock, so
     * we serialize correctly with inbound frame processing too.
     *
     * Lock order — client->lock then conn->endpoint->lock — matches
     * the driver-thread order (drive() takes client->lock; the
     * chan_* calls inside take endpoint->lock).  No inversion. */
    n00b_data_write_lock(client->lock);

    /* Open a bidi request stream. */
    n00b_result_t(n00b_quic_chan_t *) cr =
        n00b_quic_chan_open(client->conn, .bidi = true);
    if (n00b_result_is_err(cr)) {
        n00b_data_unlock(client->lock);
        return n00b_result_err(n00b_h3_request_t *, n00b_result_get_err(cr));
    }
    n00b_quic_chan_t *chan = n00b_result_get(cr);

    /* RFC 9114 § 5.2 — refuse new request streams whose ID is at or
     * past the GOAWAY limit the peer advertised.  In-flight requests
     * issued before GOAWAY arrived are unaffected. */
    if (client->goaway_received) {
        uint64_t sid = n00b_quic_chan_id(chan);
        if (sid >= client->goaway_max_stream_id) {
            n00b_quic_chan_close(chan);
            n00b_data_unlock(client->lock);
            return n00b_result_err(n00b_h3_request_t *,
                                   N00B_QUIC_ERR_PEER_CLOSED);
        }
    }

    /* Build the QPACK field list: 4 pseudo-headers + extras. */
    size_t total_fields = 4 + n_extra;
    n00b_qpack_field_t *qfields = n00b_alloc_array_with_opts(
        n00b_qpack_field_t, (int64_t)total_fields,
        &(n00b_alloc_opts_t){ .allocator = n00b_h3_alloc() });

    qfields[0].name      = (const uint8_t *)":method";
    qfields[0].name_len  = strlen(":method");
    qfields[0].value     = (const uint8_t *)method;
    qfields[0].value_len = strlen(method);

    qfields[1].name      = (const uint8_t *)":scheme";
    qfields[1].name_len  = strlen(":scheme");
    qfields[1].value     = (const uint8_t *)scheme;
    qfields[1].value_len = strlen(scheme);

    qfields[2].name      = (const uint8_t *)":authority";
    qfields[2].name_len  = strlen(":authority");
    qfields[2].value     = (const uint8_t *)authority;
    qfields[2].value_len = strlen(authority);

    qfields[3].name      = (const uint8_t *)":path";
    qfields[3].name_len  = strlen(":path");
    qfields[3].value     = (const uint8_t *)path;
    qfields[3].value_len = strlen(path);

    for (size_t i = 0; i < n_extra; i++) {
        qfields[4 + i].name      = extra_headers[i].name;
        qfields[4 + i].name_len  = extra_headers[i].name_len;
        qfields[4 + i].value     = extra_headers[i].value;
        qfields[4 + i].value_len = extra_headers[i].value_len;
    }

    /* QPACK-encode.  The encoder may want to push insertions onto
     * the QPACK encoder stream — if so, send those bytes first. */
    n00b_buffer_t section, es;
    memset(&section, 0, sizeof(section));
    memset(&es, 0, sizeof(es));
    n00b_buffer_init(&section, .length = 0, .allocator = n00b_h3_alloc(),
                     .no_lock = true);
    n00b_buffer_init(&es, .length = 0, .allocator = n00b_h3_alloc(),
                     .no_lock = true);

    uint64_t stream_id = n00b_quic_chan_id(chan);
    n00b_result_t(bool) er = n00b_qpack_encode(
        client->qpack_enc, stream_id, qfields, total_fields, &section, &es);
    if (n00b_result_is_err(er)) {
        n00b_quic_chan_close(chan);
        n00b_data_unlock(client->lock);
        return n00b_result_err(n00b_h3_request_t *, n00b_result_get_err(er));
    }

    /* Push QPACK insertions on the encoder uni stream. */
    if (es.byte_len > 0) {
        n00b_result_t(size_t) sr = n00b_quic_chan_send(
            client->qpack_enc_uni,
            (const uint8_t *)es.data, (size_t)es.byte_len);
        if (n00b_result_is_err(sr)) {
            n00b_quic_chan_close(chan);
            n00b_data_unlock(client->lock);
            return n00b_result_err(n00b_h3_request_t *, n00b_result_get_err(sr));
        }
    }

    /* Build HEADERS frame: type + length + section. */
    n00b_buffer_t headers_frame;
    memset(&headers_frame, 0, sizeof(headers_frame));
    n00b_buffer_init(&headers_frame, .length = 0, .allocator = n00b_h3_alloc(),
                     .no_lock = true);
    n00b_result_t(bool) hf = n00b_h3_frame_emit(
        &headers_frame,
        N00B_H3_FRAME_HEADERS,
        (const uint8_t *)section.data, (size_t)section.byte_len);
    if (n00b_result_is_err(hf)) {
        n00b_quic_chan_close(chan);
        n00b_data_unlock(client->lock);
        return n00b_result_err(n00b_h3_request_t *, n00b_result_get_err(hf));
    }

    /* Send HEADERS.  If there's a body, follow with a DATA frame
     * and FIN at the end. */
    n00b_result_t(size_t) sr = n00b_quic_chan_send(
        chan, (const uint8_t *)headers_frame.data,
        (size_t)headers_frame.byte_len,
        .fin = (fin && body_len == 0));
    if (n00b_result_is_err(sr)) {
        n00b_quic_chan_close(chan);
        n00b_data_unlock(client->lock);
        return n00b_result_err(n00b_h3_request_t *, n00b_result_get_err(sr));
    }

    if (body_len > 0) {
        n00b_buffer_t data_frame;
        memset(&data_frame, 0, sizeof(data_frame));
        n00b_buffer_init(&data_frame, .length = 0,
                         .allocator = n00b_h3_alloc(), .no_lock = true);
        n00b_result_t(bool) df = n00b_h3_frame_emit(
            &data_frame, N00B_H3_FRAME_DATA, body, body_len);
        if (n00b_result_is_err(df)) {
            n00b_quic_chan_close(chan);
            n00b_data_unlock(client->lock);
            return n00b_result_err(n00b_h3_request_t *,
                                    n00b_result_get_err(df));
        }
        n00b_result_t(size_t) ds = n00b_quic_chan_send(
            chan, (const uint8_t *)data_frame.data,
            (size_t)data_frame.byte_len, .fin = fin);
        if (n00b_result_is_err(ds)) {
            n00b_quic_chan_close(chan);
            n00b_data_unlock(client->lock);
            return n00b_result_err(n00b_h3_request_t *,
                                    n00b_result_get_err(ds));
        }
    }

    /* Allocate request handle. */
    n00b_h3_request_t *req = n00b_alloc_with_opts(n00b_h3_request_t,
                                    &(n00b_alloc_opts_t){
                                        .allocator = n00b_h3_alloc(),
                                    });
    req->lock = n00b_data_lock_new();
    req->client          = client;
    req->chan            = chan;
    req->state           = N00B_H3_REQ_STATE_HEADERS_SENT;
    req->local_fin_sent  = fin;
    req->peer_fin_seen   = false;
    req->status          = 0;
    req->resp_headers    = nullptr;
    req->resp_n_headers  = 0;
    req->recv_buf        = nullptr;

    n00b_buffer_t *resp_body = n00b_alloc_with_opts(n00b_buffer_t,
                                    &(n00b_alloc_opts_t){
                                        .allocator = n00b_h3_alloc(),
                                    });
    n00b_buffer_init(resp_body, .length = 0, .allocator = n00b_h3_alloc(),
                     .no_lock = true);
    req->resp_body = resp_body;

    /* Link onto the client's request list.  We already hold the client lock
     * from the top of this function, so the list itself stays private. */
    n00b_data_write_lock(client->lock);
    if (!client->requests) {
        client->requests = n00b_alloc_with_opts(
            n00b_list_t(n00b_h3_request_t *),
            &(n00b_alloc_opts_t){.allocator = n00b_h3_alloc()});
        *client->requests = n00b_list_new_private(n00b_h3_request_t *,
                                                  .allocator = n00b_h3_alloc());
    }
    n00b_list_push(*client->requests, req);
    n00b_data_unlock(client->lock);

    n00b_data_unlock(client->lock);
    return n00b_result_ok(n00b_h3_request_t *, req);
}

uint64_t
n00b_h3_request_stream_id(n00b_h3_request_t *req)
{
    if (!req || !req->chan) return UINT64_MAX;
    return n00b_quic_chan_id(req->chan);
}

void
n00b_h3_request_cancel(n00b_h3_request_t *req)
{
    if (!req) return;
    n00b_data_write_lock(req->lock);
    if (req->chan) {
        (void)n00b_quic_chan_reset(
            req->chan,
            (uint64_t)N00B_H3_ERR_REQUEST_CANCELLED);
        (void)n00b_quic_chan_stop_sending(
            req->chan,
            (uint64_t)N00B_H3_ERR_REQUEST_CANCELLED);
    }
    req->state = N00B_H3_REQ_STATE_RESET;
    n00b_data_unlock(req->lock);
}

/* ===========================================================================
 * Streaming-side request helpers (sub-phase 4.7)
 * =========================================================================== */

n00b_result_t(bool)
n00b_h3_request_send_data(n00b_h3_request_t *req,
                          n00b_h3_data_t      data)
{
    return n00b_h3_request_send_data_raw(req,
                                         data.body,
                                         data.body_len,
                                         data.fin);
}

n00b_result_t(bool)
n00b_h3_request_send_data_raw(n00b_h3_request_t *req,
                              const uint8_t     *body,
                              size_t             body_len,
                              bool               fin)
{
    if (!req) return n00b_result_err(bool, N00B_QUIC_ERR_NULL_ARG);
    if (!body && body_len > 0) {
        return n00b_result_err(bool, N00B_QUIC_ERR_NULL_ARG);
    }
    n00b_data_write_lock(req->lock);
    if (req->state == N00B_H3_REQ_STATE_RESET) {
        n00b_data_unlock(req->lock);
        return n00b_result_err(bool, N00B_QUIC_ERR_LOCAL_RESET);
    }
    if (req->local_fin_sent) {
        n00b_data_unlock(req->lock);
        return n00b_result_err(bool, N00B_QUIC_ERR_PROTOCOL);
    }

    /* Empty body + fin = pure FIN; emit a zero-length DATA frame so the
     * peer's H3 frame parser sees the FIN cleanly. */
    n00b_buffer_t data_frame;
    memset(&data_frame, 0, sizeof(data_frame));
    n00b_buffer_init(&data_frame, .length = 0,
                     .allocator = n00b_h3_alloc(), .no_lock = true);
    n00b_result_t(bool) df = n00b_h3_frame_emit(
        &data_frame, N00B_H3_FRAME_DATA, body, body_len);
    if (n00b_result_is_err(df)) {
        n00b_data_unlock(req->lock);
        return df;
    }
    n00b_result_t(size_t) ds = n00b_quic_chan_send(
        req->chan, (const uint8_t *)data_frame.data,
        (size_t)data_frame.byte_len, .fin = fin);
    if (n00b_result_is_err(ds)) {
        n00b_data_unlock(req->lock);
        return n00b_result_err(bool, n00b_result_get_err(ds));
    }
    if (fin) req->local_fin_sent = true;
    n00b_data_unlock(req->lock);
    return n00b_result_ok(bool, true);
}

bool
n00b_h3_request_headers_received(n00b_h3_request_t *req)
{
    if (!req) return false;
    n00b_data_write_lock(req->lock);
    bool got = (req->state == N00B_H3_REQ_STATE_HEADERS_RECVD ||
                req->state == N00B_H3_REQ_STATE_DONE) &&
               (req->resp_headers != nullptr || req->resp_n_headers == 0
                /* ^^ `resp_headers` may be null when the response has
                 * zero non-pseudo headers; status alone is a valid
                 * indicator. */) &&
               req->status != 0;
    n00b_data_unlock(req->lock);
    return got;
}

uint16_t
n00b_h3_request_status(n00b_h3_request_t *req)
{
    if (!req) return 0;
    n00b_data_write_lock(req->lock);
    uint16_t s = req->status;
    n00b_data_unlock(req->lock);
    return s;
}

const n00b_h3_header_t *
n00b_h3_request_response_headers(n00b_h3_request_t *req, size_t *n_out)
{
    if (n_out) *n_out = 0;
    if (!req) return nullptr;
    n00b_data_write_lock(req->lock);
    const n00b_h3_header_t *r = req->resp_headers;
    if (n_out) *n_out = req->resp_n_headers;
    n00b_data_unlock(req->lock);
    return r;
}

size_t
n00b_h3_request_consume_body(n00b_h3_request_t *req,
                             uint8_t           *out,
                             size_t             max)
{
    if (!req || !req->resp_body || max == 0) return 0;
    n00b_data_write_lock(req->lock);
    size_t avail = (size_t)req->resp_body->byte_len - req->resp_body_consumed;
    size_t copy  = avail < max ? avail : max;
    if (copy > 0) {
        memcpy(out,
               (const uint8_t *)req->resp_body->data + req->resp_body_consumed,
               copy);
        req->resp_body_consumed += copy;
    }
    n00b_data_unlock(req->lock);
    return copy;
}

bool
n00b_h3_request_recv_fin(n00b_h3_request_t *req)
{
    if (!req) return false;
    n00b_data_write_lock(req->lock);
    bool f = req->peer_fin_seen;
    n00b_data_unlock(req->lock);
    return f;
}

bool
n00b_h3_request_is_reset(n00b_h3_request_t *req)
{
    if (!req) return false;
    n00b_data_write_lock(req->lock);
    bool r = (req->state == N00B_H3_REQ_STATE_RESET);
    /* Also check the underlying QUIC channel: a peer-initiated reset
     * doesn't flip our local state machine until someone notices it. */
    if (!r && req->chan) {
        n00b_quic_chan_state_t cs = n00b_quic_chan_state(req->chan);
        if (cs == N00B_QUIC_CHAN_STATE_PEER_RESET ||
            cs == N00B_QUIC_CHAN_STATE_LOCAL_RESET) {
            req->state = N00B_H3_REQ_STATE_RESET;
            r = true;
        }
    }
    n00b_data_unlock(req->lock);
    return r;
}

/* ===========================================================================
 * Drive — pump bytes through the H3 layer
 *
 * For each known channel:
 *   1. Drain any bytes available via n00b_quic_chan_recv.
 *   2. Append to the appropriate scratch buffer.
 *   3. Try to parse + dispatch as many H3 frames as possible.
 *
 * For request streams we also accumulate response state: HEADERS →
 * decoded into req->resp_headers + req->status; DATA → appended to
 * req->resp_body.
 * =========================================================================== */

static int
parse_status(const uint8_t *value, size_t len)
{
    /* Tiny ASCII integer parse — the :status pseudo-header is
     * always a 3-digit number. */
    if (len == 0 || len > 4) return -1;
    int n = 0;
    for (size_t i = 0; i < len; i++) {
        if (value[i] < '0' || value[i] > '9') return -1;
        n = n * 10 + (value[i] - '0');
    }
    return n;
}

/* Mark the request as body-cap-exceeded and reset the underlying QUIC
 * stream with `H3_EXCESSIVE_LOAD` (RFC 9114 § 8.1).  Used by the
 * mid-stream `max_body_size` enforcement in `process_request_frame`:
 * fires both for a `content-length` header that advertises an
 * oversized body and for DATA-frame accumulation that pushes total
 * bytes past the cap.
 *
 * Pre: caller holds the appropriate locks (drive_request_streams runs
 *      under client->lock; process_request_frame is downstream of
 *      that). */
static void
req_trip_body_cap(n00b_h3_request_t *req)
{
    if (req->chan) {
        (void)n00b_quic_chan_reset(req->chan,
                                   (uint64_t)N00B_H3_ERR_EXCESSIVE_LOAD);
    }
    req->body_cap_exceeded = true;
    req->state             = N00B_H3_REQ_STATE_RESET;
}

static n00b_result_t(bool)
process_request_frame(n00b_h3_request_t      *req,
                      const n00b_h3_frame_t  *frame)
{
    n00b_h3_client_t *client = req->client;

    if (frame->type == N00B_H3_FRAME_HEADERS) {
        /* Decode the QPACK field section. */
        n00b_qpack_field_t fields[N00B_QPACK_MAX_FIELDS_PER_SECTION];
        size_t              n_fields = 0;
        n00b_buffer_t       ds;
        memset(&ds, 0, sizeof(ds));
        n00b_buffer_init(&ds, .length = 0, .allocator = n00b_h3_alloc(),
                         .no_lock = true);

        n00b_result_t(bool) dr = n00b_qpack_decode(
            client->qpack_dec,
            n00b_quic_chan_id(req->chan),
            frame->body, frame->body_len,
            fields, N00B_QPACK_MAX_FIELDS_PER_SECTION,
            &n_fields, &ds);
        if (n00b_result_is_err(dr)) {
            return n00b_result_err(bool, n00b_result_get_err(dr));
        }

        /* Send any decoder-stream bytes (Section Ack, Insert Count
         * Increment) on our outbound QPACK decoder uni stream. */
        if (ds.byte_len > 0 && client->qpack_dec_uni) {
            n00b_quic_chan_send(client->qpack_dec_uni,
                                 (const uint8_t *)ds.data,
                                 (size_t)ds.byte_len);
        }

        /* Extract :status; everything else goes into resp_headers. */
        int status = -1;
        size_t body_headers_count = 0;
        for (size_t i = 0; i < n_fields; i++) {
            if (fields[i].name_len == 7 &&
                memcmp(fields[i].name, ":status", 7) == 0) {
                status = parse_status(fields[i].value, fields[i].value_len);
            } else {
                body_headers_count++;
            }
        }
        if (status < 0 || status > 999) {
            return n00b_result_err(bool, N00B_QUIC_ERR_PROTOCOL);
        }

        /* Allocate response header array (own-the-bytes copy). */
        n00b_h3_header_t *resp = n00b_alloc_array_with_opts(
            n00b_h3_header_t, (int64_t)(body_headers_count == 0 ? 1 : body_headers_count),
            &(n00b_alloc_opts_t){ .allocator = n00b_h3_alloc() });

        size_t out_idx = 0;
        for (size_t i = 0; i < n_fields; i++) {
            if (fields[i].name_len == 7 &&
                memcmp(fields[i].name, ":status", 7) == 0) {
                continue;
            }
            uint8_t *name_copy  = h3_dup_bytes(fields[i].name,
                                                fields[i].name_len);
            uint8_t *value_copy = h3_dup_bytes(fields[i].value,
                                                fields[i].value_len);
            resp[out_idx].name      = name_copy;
            resp[out_idx].name_len  = fields[i].name_len;
            resp[out_idx].value     = value_copy;
            resp[out_idx].value_len = fields[i].value_len;
            out_idx++;
        }

        req->status         = (uint16_t)status;
        req->resp_headers   = resp;
        req->resp_n_headers = body_headers_count;
        req->state          = N00B_H3_REQ_STATE_HEADERS_RECVD;

        /* Per-call body cap: Content-Length short-circuit.
         *
         * RFC 9114 doesn't require `content-length` in H3 (DATA-frame
         * payload lengths are on the wire), but cooperative servers
         * still emit it.  When the cap is set AND the server declared a
         * `content-length` larger than the cap, abort before we accept
         * any DATA-frame bytes.  Mirrors h1's two-layered enforcement.
         *
         * Skip when status is 1xx/204/304 (no-body responses), where
         * content-length is informational rather than reflective of
         * payload that's about to land. */
        if (req->max_body_size > 0
            && !(status >= 100 && status < 200)
            && status != 204 && status != 304) {
            for (size_t i = 0; i < n_fields; i++) {
                if (fields[i].name_len != 14) continue;
                /* Case-insensitive compare: H3 header names are
                 * lowercase on the wire per RFC 9114 § 4.2, but we
                 * keep tolerance for defensive parsing. */
                bool match = true;
                static const char k[] = "content-length";
                for (size_t j = 0; j < 14; j++) {
                    char c = (char)fields[i].name[j];
                    if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
                    if (c != k[j]) { match = false; break; }
                }
                if (!match) continue;

                /* Locale-free: strtoull reads the thread's TLS locale,
                 * which is NULL on an n00b worker (n00b#467). The helper
                 * keeps the old scratch-buffer behaviour that this cap
                 * check relies on -- an unrepresentable length saturates
                 * rather than erroring, so it is still refused below. */
                uint64_t declared
                    = n00b_parse_byte_count_span((const char *)fields[i].value,
                                                 fields[i].value_len);
                if (declared > req->max_body_size) {
                    req_trip_body_cap(req);
                    return n00b_result_ok(bool, true);
                }
                break;
            }
        }
    } else if (frame->type == N00B_H3_FRAME_DATA) {
        /* Append to the request body buffer. */
        if (frame->body_len > 0) {
            size_t old = (size_t)req->resp_body->byte_len;

            /* Per-call body cap: streaming guard.  Computed BEFORE the
             * buffer grows so we never let the cap be exceeded by even
             * a single byte. */
            if (req->max_body_size > 0
                && (uint64_t)old + (uint64_t)frame->body_len
                       > req->max_body_size) {
                req_trip_body_cap(req);
                return n00b_result_ok(bool, true);
            }

            n00b_buffer_resize(req->resp_body,
                                (uint64_t)(old + frame->body_len));
            N00B_H3_DEBUG_POINT(req->chan, N00B_H3_DEBUG_BODY_GROWN);
            memcpy((uint8_t *)req->resp_body->data + old,
                    frame->body, frame->body_len);
        }
    } else if (n00b_h3_frame_type_is_grease(frame->type)) {
        /* Per RFC 9114 § 7.2.8, ignore grease. */
    } else {
        /* Unknown frame on a request stream: per RFC 9114 § 4.1, MUST
         * ignore unknown frames once minimum-viable frames are
         * understood.  Still ignore. */
    }
    return n00b_result_ok(bool, true);
}

static n00b_result_t(bool)
request_feed_locked(n00b_h3_request_t *req,
                    const uint8_t     *bytes,
                    size_t             len,
                    bool               fin)
{
    /* Already terminal (DONE or RESET): silently drop the bytes.
     * Defensive against late arrivals after a body-cap trip; without
     * this guard `drive_request_streams` could re-enter feed for a
     * second chunk of the same stream and process DATA past the
     * cap. */
    if (req->state == N00B_H3_REQ_STATE_DONE
        || req->state == N00B_H3_REQ_STATE_RESET) {
        return n00b_result_ok(bool, true);
    }

    req_recv_append(req, bytes, len);

    /* Try to parse as many frames as possible. */
    while (req->recv_buf && req->recv_buf->byte_len > 0) {
        n00b_h3_frame_t frame;
        n00b_result_t(bool) pr = n00b_h3_frame_parse_bytes(
            (uint8_t *)req->recv_buf->data,
            req->recv_buf->byte_len, &frame,
            .max_size = req->client->max_frame_body);
        if (n00b_result_is_err(pr)) {
            int32_t e = n00b_result_get_err(pr);
            if (e == N00B_QUIC_ERR_NEED_MORE_DATA) break;
            return pr;
        }
        n00b_result_t(bool) dr = process_request_frame(req, &frame);
        if (n00b_result_is_err(dr)) {
            return dr;
        }
        req_recv_consume(req, frame.consumed);

        /* `process_request_frame` may have tripped the per-call body
         * cap and flipped the request into RESET.  Stop draining
         * additional frames — the stream is dead and any further
         * DATA accumulation would just be wasted work. */
        if (req->state == N00B_H3_REQ_STATE_RESET) {
            return n00b_result_ok(bool, true);
        }
    }

    if (fin) {
        req->peer_fin_seen = true;
        if (req->state == N00B_H3_REQ_STATE_HEADERS_RECVD) {
            req->state = N00B_H3_REQ_STATE_DONE;
        }
    }
    return n00b_result_ok(bool, true);
}

n00b_result_t(bool)
n00b_h3_request_feed(n00b_h3_request_t *req,
                     const uint8_t     *bytes,
                     size_t             len,
                     bool               fin)
{
    if (!req) {
        return n00b_result_err(bool, N00B_QUIC_ERR_NULL_ARG);
    }

    // The accessors, n00b_h3_request_consume_body among them, read this
    // request's state under the same lock from other threads.
    n00b_data_write_lock(req->lock);
    n00b_result_t(bool) r = request_feed_locked(req, bytes, len, fin);
    n00b_data_unlock(req->lock);
    return r;
}

/* ===========================================================================
 * Per-uni-stream feeder for the peer's CONTROL / QPACK encoder /
 * QPACK decoder uni streams.
 * =========================================================================== */

static n00b_result_t(bool)
process_control_frame(n00b_h3_client_t      *client,
                      const n00b_h3_frame_t *frame)
{
    if (frame->type == N00B_H3_FRAME_SETTINGS) {
        n00b_h3_settings_t s;
        n00b_result_t(bool) pr = n00b_h3_settings_parse(
            frame->body, frame->body_len, &s);
        if (n00b_result_is_err(pr)) return pr;
        client->peer_settings = s;
    } else if (frame->type == N00B_H3_FRAME_GOAWAY) {
        /* RFC 9114 § 5.2 — body is one varint: server's stream-id
         * limit.  We must not issue NEW requests on streams whose
         * IDs are >= that value; in-flight requests already past
         * the limit complete normally. */
        uint64_t limit = 0;
        int64_t  used  = n00b_h3_varint_decode(frame->body,
                                               frame->body_len,
                                               &limit);
        if (used <= 0) {
            return n00b_result_err(bool, N00B_QUIC_ERR_PROTOCOL);
        }
        /* Only ratchet downward (peer sending an increasing limit
         * is a protocol error per § 5.2). */
        if (client->goaway_received && limit > client->goaway_max_stream_id) {
            return n00b_result_err(bool, N00B_QUIC_ERR_PROTOCOL);
        }
        client->goaway_received      = true;
        client->goaway_max_stream_id = limit;
    } else if (frame->type == N00B_H3_FRAME_MAX_PUSH_ID ||
               frame->type == N00B_H3_FRAME_CANCEL_PUSH) {
        /* Server push not supported in v1; acknowledge silently. */
    }
    /* Reserved frame types are rejected by the parser already.
     * Grease + unknown: ignore per § 7.2.8. */
    return n00b_result_ok(bool, true);
}

static n00b_result_t(bool)
drive_uni_stream_recv(n00b_h3_client_t  *client,
                      n00b_quic_chan_t  *chan,
                      n00b_buffer_t    **buf_ptr,
                      bool               is_control,
                      bool               is_qpack_enc_peer,
                      bool               is_qpack_dec_peer)
{
    if (!chan) return n00b_result_ok(bool, true);

    if (!*buf_ptr) {
        *buf_ptr = n00b_buffer_empty(.allocator = n00b_h3_alloc());
    }
    n00b_buffer_t *bb = *buf_ptr;

    /* Drain channel recv buffer in chunks. */
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
        /* Feed peer's encoder-stream bytes into our QPACK decoder. */
        n00b_buffer_t ds;
        memset(&ds, 0, sizeof(ds));
        n00b_buffer_init(&ds, .length = 0, .allocator = n00b_h3_alloc(),
                         .no_lock = true);
        n00b_result_t(size_t) cr = n00b_qpack_decoder_consume_encoder_stream(
            client->qpack_dec,
            (uint8_t *)bb->data, bb->byte_len, &ds);
        if (n00b_result_is_err(cr)) return n00b_result_err(bool, n00b_result_get_err(cr));
        size_t n = n00b_result_get(cr);
        if (n > 0) {
            memmove(bb->data, bb->data + n, bb->byte_len - n);
            n00b_buffer_resize(bb, bb->byte_len - n);
        }
        if (ds.byte_len > 0 && client->qpack_dec_uni) {
            n00b_quic_chan_send(client->qpack_dec_uni,
                                 (const uint8_t *)ds.data,
                                 (size_t)ds.byte_len);
        }
        return n00b_result_ok(bool, true);
    }

    if (is_qpack_dec_peer) {
        /* Feed peer's decoder-stream bytes into our QPACK encoder. */
        n00b_result_t(size_t) cr = n00b_qpack_encoder_consume_decoder_stream(
            client->qpack_enc,
            (uint8_t *)bb->data, bb->byte_len);
        if (n00b_result_is_err(cr)) return n00b_result_err(bool, n00b_result_get_err(cr));
        size_t n = n00b_result_get(cr);
        if (n > 0) {
            memmove(bb->data, bb->data + n, bb->byte_len - n);
            n00b_buffer_resize(bb, bb->byte_len - n);
        }
        return n00b_result_ok(bool, true);
    }

    if (is_control) {
        /* Parse + dispatch H3 frames on the control stream. */
        size_t off = 0;
        while (off < bb->byte_len) {
            n00b_h3_frame_t frame;
            n00b_result_t(bool) pr = n00b_h3_frame_parse_bytes(
                (uint8_t *)bb->data + off, bb->byte_len - off, &frame,
                .max_size = client->max_frame_body);
            if (n00b_result_is_err(pr)) {
                int32_t e = n00b_result_get_err(pr);
                if (e == N00B_QUIC_ERR_NEED_MORE_DATA) break;
                return pr;
            }
            n00b_result_t(bool) dr = process_control_frame(client, &frame);
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

/* Discover any newly-arrived peer uni streams and classify them by
 * the leading kind varint. */
static void
discover_peer_uni_streams(n00b_h3_client_t *client)
{
    n00b_quic_chan_t *c = n00b_quic_conn_first_chan(client->conn);
    while (c) {
        n00b_quic_chan_t *nx = n00b_quic_chan_next_in_conn(c);

        /* Skip our own channels (we own them; no classification
         * needed). */
        if (c == client->control_uni ||
            c == client->qpack_enc_uni ||
            c == client->qpack_dec_uni) {
            c = nx; continue;
        }
        /* Skip already-classified peer channels. */
        if (c == client->peer_control_uni ||
            c == client->peer_qpack_enc_uni ||
            c == client->peer_qpack_dec_uni) {
            c = nx; continue;
        }
        /* Skip our own request channels. */
        bool is_request = false;
        if (client->requests) {
            size_t nr = (size_t)n00b_list_len(*client->requests);
            for (size_t i = 0; i < nr; i++) {
                n00b_h3_request_t *r = n00b_list_get(*client->requests, i);
                if (r->chan == c) { is_request = true; break; }
            }
        }
        if (is_request) { c = nx; continue; }

        /* Bidi peer-initiated streams aren't part of our model; ignore. */
        if (n00b_quic_chan_kind(c) != N00B_QUIC_CHAN_FRAMED) {
            c = nx; continue;
        }

        /* Try to read the kind varint (may not have arrived yet). */
        if (!n00b_quic_chan_has_data(c)) { c = nx; continue; }

        /* Peek bytes into our scratch buffer; we may need multiple
         * arrivals to consume the varint. */
        uint8_t tmp[8];
        n00b_result_t(size_t) rr = n00b_quic_chan_recv(c, tmp, sizeof(tmp));
        if (n00b_result_is_err(rr)) { c = nx; continue; }
        size_t got = n00b_result_get(rr);
        if (got == 0) { c = nx; continue; }

        /* Append into the kind-varint scratch (max 8 bytes). */
        size_t copy = got;
        if ((size_t)client->peer_uni_kind_len + copy > 8) {
            copy = 8 - client->peer_uni_kind_len;
        }
        memcpy(client->peer_uni_kind_bytes + client->peer_uni_kind_len,
               tmp, copy);
        client->peer_uni_kind_len += (uint8_t)copy;

        uint64_t kind;
        int64_t  vn = n00b_h3_varint_decode(client->peer_uni_kind_bytes,
                                            client->peer_uni_kind_len,
                                            &kind);
        if (vn <= 0) {
            /* Not enough bytes yet — leave them in the scratch and
             * try again on the next drive(). */
            c = nx; continue;
        }

        /* Reset the scratch (one varint per uni stream). */
        client->peer_uni_kind_len = 0;

        switch ((n00b_h3_uni_stream_kind_t)kind) {
        case N00B_H3_UNI_CONTROL:
            client->peer_control_uni = c;
            break;
        case N00B_H3_UNI_QPACK_ENCODER:
            client->peer_qpack_enc_uni = c;
            break;
        case N00B_H3_UNI_QPACK_DECODER:
            client->peer_qpack_dec_uni = c;
            break;
        case N00B_H3_UNI_PUSH:
            /* Server push — defer to v1.1.  Cancel the stream via
             * STOP_SENDING + a cancel-push on our control stream. */
            (void)n00b_quic_chan_stop_sending(
                c,
                (uint64_t)N00B_H3_ERR_REQUEST_CANCELLED);
            break;
        default:
            /* Unknown stream kind: per RFC 9114 § 6.2 the peer is
             * permitted to abort it via STOP_SENDING. */
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

/* Drain bytes for our outstanding requests. */
static n00b_result_t(bool)
drive_request_streams(n00b_h3_client_t *client)
{
    if (!client->requests) return n00b_result_ok(bool, true);
    size_t nr = (size_t)n00b_list_len(*client->requests);
    for (size_t ri = 0; ri < nr; ri++) {
        n00b_h3_request_t *req = n00b_list_get(*client->requests, ri);
        if (!req->chan) continue;
        if (req->state == N00B_H3_REQ_STATE_DONE ||
            req->state == N00B_H3_REQ_STATE_RESET) {
            continue;
        }
        uint8_t buf[4096];
        bool any = false;
        while (n00b_quic_chan_has_data(req->chan)) {
            n00b_result_t(size_t) rr = n00b_quic_chan_recv(req->chan, buf, sizeof(buf));
            if (n00b_result_is_err(rr)) break;
            size_t got = n00b_result_get(rr);
            if (got == 0) break;
            any = true;
            n00b_result_t(bool) fr = n00b_h3_request_feed(req, buf, got, false);
            if (n00b_result_is_err(fr)) {
                return fr;
            }
            /* The body-cap guard inside `process_request_frame` may
             * have tripped state into RESET.  Stop draining further
             * bytes for this request — the stream is dead. */
            if (req->state == N00B_H3_REQ_STATE_RESET) {
                break;
            }
        }
        if (req->state == N00B_H3_REQ_STATE_RESET) {
            continue;
        }
        N00B_H3_DEBUG_POINT(req->chan, N00B_H3_DEBUG_DRAINED);
        bool peer_fin = n00b_quic_chan_recv_fin(req->chan);
        if (peer_fin && !req->peer_fin_seen) {
            (void)n00b_h3_request_feed(req, nullptr, 0, true);
        }
        (void)any;
    }
    return n00b_result_ok(bool, true);
}

n00b_result_t(bool)
n00b_h3_client_drive(n00b_h3_client_t *client)
{
    if (!client) {
        return n00b_result_err(bool, N00B_QUIC_ERR_NULL_ARG);
    }
    n00b_data_write_lock(client->lock);
    if (client->closed) {
        n00b_data_unlock(client->lock);
        return n00b_result_ok(bool, true);
    }

    /* Discover peer uni streams and classify them. */
    discover_peer_uni_streams(client);

    /* Pump per-uni-stream feeders.  Scratch buffers live on the
     * client so multiple clients in the same thread are isolated. */
    n00b_result_t(bool) r;
    r = drive_uni_stream_recv(client, client->peer_control_uni,
                               &client->peer_ctrl_buf,
                               /*is_control*/ true,
                               /*is_qe*/ false, /*is_qd*/ false);
    if (n00b_result_is_err(r)) { n00b_data_unlock(client->lock); return r; }

    r = drive_uni_stream_recv(client, client->peer_qpack_enc_uni,
                               &client->peer_qe_buf,
                               /*is_control*/ false,
                               /*is_qe*/ true, /*is_qd*/ false);
    if (n00b_result_is_err(r)) { n00b_data_unlock(client->lock); return r; }

    r = drive_uni_stream_recv(client, client->peer_qpack_dec_uni,
                               &client->peer_qd_buf,
                               /*is_control*/ false,
                               /*is_qe*/ false, /*is_qd*/ true);
    if (n00b_result_is_err(r)) { n00b_data_unlock(client->lock); return r; }

    r = drive_request_streams(client);
    n00b_data_unlock(client->lock);
    return r;
}

/* ===========================================================================
 * await — block for response with deadline
 * =========================================================================== */

static int64_t
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

n00b_result_t(n00b_h3_response_t *)
n00b_h3_request_await(n00b_h3_request_t *req) _kargs
{
    int32_t  deadline_ms   = 10000;
    bool     drive         = true;
    /* Per-call response-body byte cap (mid-stream enforcement).  0 =
     * no cap (existing callers see identical behavior).  When set, the
     * receive loop checks the cap on each DATA-frame append and on a
     * `content-length` response header (when the server sent one);
     * overrun resets the stream with `H3_EXCESSIVE_LOAD` and the await
     * returns `N00B_QUIC_ERR_LOCAL_RESET`.  Callers (e.g.,
     * http_h3.c) inspect `n00b_h3_request_body_cap_exceeded` to
     * distinguish cap-driven resets from peer-initiated ones and
     * translate to their preferred error namespace. */
    uint64_t max_body_size = 0;
}
{
    if (!req) {
        return n00b_result_err(n00b_h3_response_t *, N00B_QUIC_ERR_NULL_ARG);
    }

    /* Stamp the cap on the request so `process_request_frame` (downstream
     * of `n00b_h3_client_drive`) can honor it mid-stream.  A non-zero
     * value already present on the struct (e.g., from a future direct
     * setter) is preserved when the kwarg is 0. */
    if (max_body_size > 0) {
        req->max_body_size = max_body_size;
    }

    int64_t start  = now_ms();
    int64_t budget = (deadline_ms < 0) ? INT64_MAX : (int64_t)deadline_ms;
    n00b_quic_endpoint_t *ep = n00b_quic_conn_endpoint(
        n00b_quic_chan_conn(req->chan));

    while (true) {
        /* Pump UDP for a short slice — only when no external driver
         * is pumping the endpoint.  Concurrent picoquic mutation
         * across threads is unsafe. */
        if (drive && ep) (void)n00b_quic_endpoint_run_once(ep, 5);
        if (drive) (void)n00b_h3_client_drive(req->client);
        else {
            /* Yield briefly so other threads make progress. */
            base_nanosleep_ns(1ULL * 1000ULL * 1000ULL);
        }

        if (req->state == N00B_H3_REQ_STATE_DONE) {
            n00b_h3_response_t *resp = n00b_alloc_with_opts(n00b_h3_response_t,
                                            &(n00b_alloc_opts_t){
                                                .allocator = n00b_h3_alloc(),
                                            });
            resp->status    = req->status;
            resp->headers   = req->resp_headers;
            resp->n_headers = req->resp_n_headers;
            resp->body      = req->resp_body;
            return n00b_result_ok(n00b_h3_response_t *, resp);
        }
        if (req->state == N00B_H3_REQ_STATE_RESET) {
            /* Distinguish the two reset causes:
             *
             *   - body_cap_exceeded == true: WE reset the stream
             *     because the server's response body exceeded the
             *     caller-supplied cap.  Surface `LOCAL_RESET` so the
             *     caller can disambiguate via
             *     `n00b_h3_request_body_cap_exceeded`.
             *
             *   - body_cap_exceeded == false: peer-initiated reset.
             *     Preserve the historical `PEER_RESET` shape so
             *     existing callers see identical behavior. */
            n00b_quic_err_t code = req->body_cap_exceeded
                                       ? N00B_QUIC_ERR_LOCAL_RESET
                                       : N00B_QUIC_ERR_PEER_RESET;
            return n00b_result_err(n00b_h3_response_t *, code);
        }

        int64_t elapsed = now_ms() - start;
        if (elapsed >= budget) {
            return n00b_result_err(n00b_h3_response_t *,
                                    N00B_QUIC_ERR_TIMEOUT);
        }
    }
}

bool
n00b_h3_request_body_cap_exceeded(n00b_h3_request_t *req)
{
    if (!req) return false;
    n00b_data_write_lock(req->lock);
    bool b = req->body_cap_exceeded;
    n00b_data_unlock(req->lock);
    return b;
}
