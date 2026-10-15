/*
 * http_h1.c — HTTP/1.1 transport for the n00b HTTP client.
 *
 * Phase 6 chunk 2.  Successor to `acme_http.c`; chunk 11 retires
 * the old shim.  This file currently holds the header bag + the
 * response parser; chunk 2.2 adds the request builder and the
 * `request → wire bytes → response` driver that wraps the existing
 * `n00b_acme_tls_round_trip` TLS layer.
 *
 * Layout:
 *   §1   Header bag (case-insensitive ASCII)
 *   §2   Response parsing (status line + headers + Content-Length /
 *        Transfer-Encoding: chunked + Connection keep-alive)
 *
 * RFC references:
 *   - RFC 9110 (HTTP semantics)
 *   - RFC 9112 (HTTP/1.1 message syntax) — esp. § 9.6 (persistence)
 *   - RFC 7230 (legacy chunked transfer-coding rules; carried over
 *     in 9112 § 7.1)
 */

#define N00B_USE_INTERNAL_API
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/buffer.h"
#include "core/string.h"
#include "adt/list.h"
#include "adt/result.h"
#include "util/parse_num.h"
#include "internal/net/http/http_url.h"
#include "internal/net/http/http_h1.h"
#include "internal/net/http/http_pool.h"
#include "internal/net/http/http_proxy.h"
#include "internal/crypto/acme_tls.h"
#include "net/quic/quic_types.h"
#include "crypto/trust.h"
#include "net/http/http_auth.h"
#include "util/ascii_ci.h"

/* Conduit-native TLS transport (src/conduit/xform_tls.c) — the idiomatic
 * replacement for the synchronous acme_tls connect/send/recv on the common
 * (non-mTLS) HTTPS path.  acme_tls stays reachable for the mTLS-client-auth
 * case below. */
#include "conduit/conduit.h"
#include "conduit/socket.h"
#include "conduit/service.h"
#include "conduit/rw.h"
#include "conduit/inbox.h"
#include "conduit/print.h"
#include "conduit/xform_tls.h"
#include "core/condition.h"
#include "core/env.h"
#include "core/time.h"

/* ===========================================================================
 * §1   Header bag
 * =========================================================================== */

typedef struct h1_header_node {
    char *name;   /* lowercased, NUL-terminated */
    char *value;  /* NUL-terminated, value bytes verbatim */
} h1_header_node_t;

struct n00b_http_h1_headers {
    n00b_list_t(h1_header_node_t *) *items;
    n00b_allocator_t                *allocator;
};

static int32_t
h1_remaining_timeout_ms(uint64_t deadline_ns)
{
    uint64_t now = n00b_ns_timestamp();
    if (now >= deadline_ns) {
        return 0;
    }
    uint64_t rem_ms = (deadline_ns - now + 999999ULL) / 1000000ULL;
    return rem_ms > 2147483647ULL ? 2147483647 : (int32_t)rem_ms;
}

static n00b_allocator_t *
default_pool(void)
{
    return (n00b_allocator_t *)&n00b_get_runtime()->conduit_pool;
}

static char *
h1_strdup(const char *s, size_t len, n00b_allocator_t *allocator)
{
    char *out = n00b_alloc_array_with_opts(char,
                                           (int64_t)len + 1,
                                           &(n00b_alloc_opts_t){
                                               .allocator = allocator,
                                               .no_scan   = true,
                                           });
    if (len > 0) {
        memcpy(out, s, len);
    }
    out[len] = '\0';
    return out;
}

static void
h1_lowercase(char *s)
{
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z') {
            *s = (char)(*s + ('a' - 'A'));
        }
    }
}

n00b_http_h1_headers_t *
n00b_http_h1_headers_new()
    _kargs {
        n00b_allocator_t *allocator = nullptr;
    }
{
    n00b_allocator_t *a = allocator ? allocator : default_pool();
    n00b_http_h1_headers_t *h = n00b_alloc_with_opts(
        n00b_http_h1_headers_t,
        &(n00b_alloc_opts_t){.allocator = a});
    h->items = n00b_alloc_with_opts(
        n00b_list_t(h1_header_node_t *),
        &(n00b_alloc_opts_t){.allocator = a});
    /* Allocate the list (its backing array + lock) in the SAME pool as the
     * bag. `a` is typically the runtime conduit_pool, which is hidden/non-GC:
     * the collector does not trace into it, so a list left in the default GC
     * heap would have its backing+lock reclaimed/moved out from under this
     * untraced pointer (stale-lock crash when the bag is read after a GC, e.g.
     * on the h3 egress path). Passing .allocator keeps everything co-resident
     * and stable. */
    *h->items           = n00b_list_new(h1_header_node_t *, .allocator = a);
    h->items->allocator = a;
    h->allocator        = a;
    return h;
}

void
n00b_http_h1_headers_set(n00b_http_h1_headers_t *h,
                         const char             *name,
                         const char             *value)
{
    if (!h || !name || !value) {
        return;
    }
    size_t name_len  = strlen(name);
    size_t value_len = strlen(value);

    /* Find existing entry (case-insensitive). */
    size_t n_items = (size_t)n00b_list_len(*h->items);
    for (size_t i = 0; i < n_items; i++) {
        h1_header_node_t *cur = n00b_list_get(*h->items, i);
        if (strlen(cur->name) == name_len
            && n00b_ascii_ci_eq_n(cur->name, name, name_len)) {
            n00b_free(cur->value);
            cur->value = h1_strdup(value, value_len, h->allocator);
            return;
        }
    }

    /* Append new entry. */
    h1_header_node_t *node = n00b_alloc_with_opts(
        h1_header_node_t,
        &(n00b_alloc_opts_t){.allocator = h->allocator});
    node->name = h1_strdup(name, name_len, h->allocator);
    h1_lowercase(node->name);
    node->value = h1_strdup(value, value_len, h->allocator);
    n00b_list_push(*h->items, node);
}

const char *
n00b_http_h1_headers_get_cstr(n00b_http_h1_headers_t *h, const char *name)
{
    if (!h || !name) {
        return nullptr;
    }
    size_t name_len = strlen(name);
    size_t n_items  = (size_t)n00b_list_len(*h->items);
    for (size_t i = 0; i < n_items; i++) {
        h1_header_node_t *cur = n00b_list_get(*h->items, i);
        if (strlen(cur->name) == name_len
            && n00b_ascii_ci_eq_n(cur->name, name, name_len)) {
            return cur->value;
        }
    }
    return nullptr;
}

size_t
n00b_http_h1_headers_len(n00b_http_h1_headers_t *h)
{
    if (!h) return 0;
    return (size_t)n00b_list_len(*h->items);
}

bool
n00b_http_h1_headers_at(n00b_http_h1_headers_t *h,
                        size_t                  idx,
                        const char            **name_out,
                        const char            **value_out)
{
    if (!h || idx >= (size_t)n00b_list_len(*h->items)) {
        return false;
    }
    h1_header_node_t *cur = n00b_list_get(*h->items, idx);
    if (name_out)  *name_out  = cur->name;
    if (value_out) *value_out = cur->value;
    return true;
}

void
n00b_http_h1_headers_free(n00b_http_h1_headers_t *h)
{
    if (h == nullptr) {
        return;
    }

    size_t n_items = (size_t)n00b_list_len(*h->items);
    for (size_t i = 0; i < n_items; i++) {
        h1_header_node_t *cur = n00b_list_get(*h->items, i);
        if (cur == nullptr) {
            continue;
        }
        n00b_free(cur->name);
        n00b_free(cur->value);
        n00b_free(cur);
    }
    n00b_list_free(*h->items);
    n00b_free(h->items);
    n00b_free(h);
}

/* ===========================================================================
 * §2   Response parsing
 * =========================================================================== */

static size_t
find_crlfcrlf(const char *buf, size_t len)
{
    for (size_t i = 0; i + 4 <= len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n'
            && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return i;
        }
    }
    return (size_t)-1;
}

static bool
parse_status_line(const char       *line,
                  size_t            len,
                  int              *status_out,
                  uint8_t          *minor_out,
                  char            **status_text_out,
                  n00b_allocator_t *allocator)
{
    /* "HTTP/1.X SSS REASON" — minimum length is "HTTP/1.X SSS". */
    if (len < 12) return false;
    if (memcmp(line, "HTTP/1.", 7) != 0) return false;
    char minor_c = line[7];
    if (minor_c != '0' && minor_c != '1') return false;
    size_t i = 8;
    while (i < len && line[i] == ' ') i++;
    if (i + 3 > len) return false;
    if (line[i]     < '0' || line[i]     > '9'
        || line[i + 1] < '0' || line[i + 1] > '9'
        || line[i + 2] < '0' || line[i + 2] > '9') {
        return false;
    }
    int s = (line[i] - '0') * 100
          + (line[i + 1] - '0') * 10
          + (line[i + 2] - '0');
    if (s < 100 || s > 599) return false;
    *status_out = s;
    *minor_out  = (uint8_t)(minor_c - '0');

    i += 3;
    while (i < len && line[i] == ' ') i++;
    *status_text_out = h1_strdup(line + i, len - i, allocator);
    return true;
}

static bool
parse_headers(const char             *buf,
              size_t                  len,
              n00b_http_h1_headers_t *h,
              n00b_allocator_t       *allocator)
{
    size_t i = 0;
    while (i < len) {
        size_t eol         = i;
        bool   crlf_termed = false;
        while (eol < len) {
            if (eol + 1 < len && buf[eol] == '\r' && buf[eol + 1] == '\n') {
                crlf_termed = true;
                break;
            }
            eol++;
        }
        if (eol == i) break;             /* empty line */

        size_t colon = i;
        while (colon < eol && buf[colon] != ':') colon++;
        if (colon == eol) return false;  /* no `:` → malformed */

        size_t v_start = colon + 1;
        while (v_start < eol
               && (buf[v_start] == ' ' || buf[v_start] == '\t')) {
            v_start++;
        }
        size_t v_end = eol;
        while (v_end > v_start
               && (buf[v_end - 1] == ' ' || buf[v_end - 1] == '\t')) {
            v_end--;
        }

        char *name  = h1_strdup(buf + i, colon - i, allocator);
        char *value = h1_strdup(buf + v_start, v_end - v_start, allocator);
        n00b_http_h1_headers_set(h, name, value);

        i = crlf_termed ? eol + 2 : len;
    }
    return true;
}

static bool
decode_chunked(const char       *src,
               size_t            src_len,
               n00b_buffer_t    *out,
               n00b_allocator_t *allocator)
{
    size_t i = 0;
    while (i < src_len) {
        size_t line_end = i;
        while (line_end + 1 < src_len
               && !(src[line_end] == '\r' && src[line_end + 1] == '\n')) {
            line_end++;
        }
        if (line_end + 1 >= src_len) return false;

        size_t chunk_size = 0;
        for (size_t j = i; j < line_end && src[j] != ';'; j++) {
            char c = src[j];
            int  v;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else if (c == ' ' || c == '\t') continue;
            else return false;
            chunk_size = (chunk_size << 4) | (size_t)v;
            if (chunk_size > (1u << 28)) return false;
        }

        size_t body_start = line_end + 2;

        if (chunk_size == 0) return true;     /* trailers ignored */

        if (body_start + chunk_size + 2 > src_len) return false;

        n00b_buffer_t *piece = n00b_buffer_from_bytes(
            (char *)(src + body_start), (int64_t)chunk_size,
            .allocator = allocator);
        n00b_buffer_concat(out, piece);

        i = body_start + chunk_size;
        if (i + 1 >= src_len || src[i] != '\r' || src[i + 1] != '\n') {
            return false;
        }
        i += 2;
    }
    return true;
}

static bool
compute_keep_alive(uint8_t            http_minor,
                   const char        *connection)
{
    /* RFC 9112 § 9.6:
     *   - HTTP/1.1 default = keep-alive unless Connection: close.
     *   - HTTP/1.0 default = close unless Connection: keep-alive.
     * Header value is a comma-separated list; case-insensitive. */
    bool default_keep = (http_minor >= 1);
    if (!connection) return default_keep;
    /* Tokenize on `,` and check for the close / keep-alive directives. */
    bool        saw_close = false;
    bool        saw_keep  = false;
    const char *p         = connection;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char *tok_start = p;
        while (*p && *p != ',') p++;
        const char *tok_end = p;
        while (tok_end > tok_start
               && (tok_end[-1] == ' ' || tok_end[-1] == '\t')) {
            tok_end--;
        }
        size_t toklen = (size_t)(tok_end - tok_start);
        if (toklen == 5 && n00b_ascii_ci_eq_n(tok_start, "close", 5)) {
            saw_close = true;
        } else if (toklen == 10
                   && n00b_ascii_ci_eq_n(tok_start, "keep-alive", 10)) {
            saw_keep = true;
        }
    }
    if (saw_close) return false;
    if (saw_keep)  return true;
    return default_keep;
}

n00b_result_t(n00b_http_h1_response_t *)
n00b_http_h1_response_parse(n00b_buffer_t *raw)
    _kargs {
        n00b_allocator_t *allocator = nullptr;
    }
{
    if (!raw || !raw->data || raw->byte_len == 0) {
        return n00b_result_err(n00b_http_h1_response_t *,
                               N00B_HTTP_ERR_NULL_ARG);
    }
    n00b_allocator_t *a = allocator ? allocator : default_pool();

    const char *buf = raw->data;
    size_t      len = raw->byte_len;

    /* Status line. */
    size_t status_end = 0;
    while (status_end + 1 < len
           && !(buf[status_end] == '\r' && buf[status_end + 1] == '\n')) {
        status_end++;
    }
    if (status_end + 1 >= len) {
        return n00b_result_err(n00b_http_h1_response_t *,
                               N00B_HTTP_ERR_BAD_RESPONSE);
    }

    n00b_http_h1_response_t *resp = n00b_alloc_with_opts(
        n00b_http_h1_response_t,
        &(n00b_alloc_opts_t){.allocator = a});
    resp->headers = n00b_http_h1_headers_new(.allocator = a);

    char *status_text = nullptr;
    if (!parse_status_line(buf, status_end,
                           &resp->status, &resp->http_minor,
                           &status_text, a)) {
        return n00b_result_err(n00b_http_h1_response_t *,
                               N00B_HTTP_ERR_BAD_RESPONSE);
    }
    resp->status_text = n00b_string_from_cstr(status_text, .allocator = a);

    /* Headers. */
    size_t hdr_end = find_crlfcrlf(buf, len);
    if (hdr_end == (size_t)-1) {
        return n00b_result_err(n00b_http_h1_response_t *,
                               N00B_HTTP_ERR_BAD_RESPONSE);
    }
    if (!parse_headers(buf + status_end + 2,
                       hdr_end - (status_end + 2),
                       resp->headers, a)) {
        return n00b_result_err(n00b_http_h1_response_t *,
                               N00B_HTTP_ERR_BAD_RESPONSE);
    }

    /* Body. */
    const char *body_start = buf + hdr_end + 4;
    size_t      body_len   = len - (hdr_end + 4);

    const char *cl = n00b_http_h1_headers_get_cstr(resp->headers,
                                                    "content-length");
    const char *te = n00b_http_h1_headers_get_cstr(resp->headers,
                                                    "transfer-encoding");

    resp->body = n00b_buffer_empty(.allocator = a);

    if (te && n00b_ascii_ci_eq(te, "chunked")) {
        if (!decode_chunked(body_start, body_len, resp->body, a)) {
            return n00b_result_err(n00b_http_h1_response_t *,
                                   N00B_HTTP_ERR_BAD_RESPONSE);
        }
    } else if (cl) {
        size_t n = (size_t)n00b_parse_byte_count_span(cl, strlen(cl));
        if (n > body_len) n = body_len;
        if (n > 0) {
            n00b_buffer_t *bb = n00b_buffer_from_bytes(
                (char *)body_start, (int64_t)n,
                .allocator = a);
            n00b_buffer_concat(resp->body, bb);
        }
    } else if (body_len > 0) {
        /* No Content-Length and no Transfer-Encoding: read to EOF. */
        n00b_buffer_t *bb = n00b_buffer_from_bytes(
            (char *)body_start, (int64_t)body_len,
            .allocator = a);
        n00b_buffer_concat(resp->body, bb);
    }

    /* Connection persistence. */
    const char *conn = n00b_http_h1_headers_get_cstr(resp->headers,
                                                      "connection");
    resp->keep_alive = compute_keep_alive(resp->http_minor, conn);

    return n00b_result_ok(n00b_http_h1_response_t *, resp);
}

/* ===========================================================================
 * §3   Request builder
 *
 * Allocates from the supplied allocator (or the per-runtime conduit
 * pool) and grows the request buffer incrementally.  Generates the
 * exact bytes a server will see on the wire — `Content-Length` and
 * `Host` are derived from the parsed URL + body length, never trusted
 * from caller-supplied headers.
 *
 * "Built-in" headers (Host, User-Agent, Accept, Connection,
 * Content-Type, Content-Length) can be overridden by the caller via
 * @p extra; we use a small lookup table to skip the built-in line
 * when the caller has already set the same name (case-insensitive).
 * =========================================================================== */

static void
append_cstr(n00b_buffer_t *out, const char *s, n00b_allocator_t *a)
{
    size_t l = strlen(s);
    if (l == 0) return;
    n00b_buffer_t *piece = n00b_buffer_from_bytes((char *)s, (int64_t)l,
                                                   .allocator = a);
    n00b_buffer_concat(out, piece);
}

// Formats one request line or header line. Most fit the stack buffer; a
// longer one (a bearer token is often over 1 KB, and so is a long query) is
// formatted again into a buffer of its own size. Truncating it instead would
// also drop the line's CRLF and leave a malformed request.
static void
append_fmt(n00b_buffer_t *out, n00b_allocator_t *a, const char *fmt, ...)
{
    char    line[1024];
    va_list ap;
    va_list again;
    va_start(ap, fmt);
    va_copy(again, ap);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        va_end(again);
        return;
    }
    char *text = line;
    if ((size_t)n >= sizeof(line)) {
        text = n00b_alloc_array_with_opts(char,
                                          (int64_t)n + 1,
                                          &(n00b_alloc_opts_t){
                                              .allocator = a,
                                              .no_scan   = true,
                                          });
        (void)vsnprintf(text, (size_t)n + 1, fmt, again);
    }
    va_end(again);
    n00b_buffer_t *piece = n00b_buffer_from_bytes(text, (int64_t)n,
                                                   .allocator = a);
    n00b_buffer_concat(out, piece);
}

static bool
extra_has(n00b_http_h1_headers_t *extra, const char *name)
{
    return extra && n00b_http_h1_headers_get_cstr(extra, name) != nullptr;
}

n00b_buffer_t *
n00b_http_h1_request_build(n00b_http_url_t *url)
    _kargs {
        const char             *method       = "GET";
        n00b_buffer_t          *body         = nullptr;
        const char             *content_type = nullptr;
        n00b_http_h1_headers_t *extra        = nullptr;
        const char             *user_agent   = "n00b-http/0.1";
        bool                    keep_alive   = false;
        n00b_allocator_t       *allocator    = nullptr;
    }
{
    n00b_allocator_t *a   = allocator ? allocator : default_pool();
    n00b_buffer_t    *req = n00b_buffer_empty(.allocator = a);

    /* Path-and-query slug for the request line. */
    const char *path = url->path && url->path->u8_bytes
                            ? url->path->data : "/";
    if (url->query && url->query->u8_bytes > 0) {
        append_fmt(req, a, "%s %s?%s HTTP/1.1\r\n",
                   method, path, url->query->data);
    } else {
        append_fmt(req, a, "%s %s HTTP/1.1\r\n", method, path);
    }

    /* Host: include explicit port only if non-default OR caller used
     * an IPv6 literal (always re-bracket so the wire form is parseable
     * by the receiver). */
    if (!extra_has(extra, "Host")) {
        bool needs_port = url->has_explicit_port && url->port != 443;
        if (url->is_ipv6_literal) {
            if (needs_port) {
                append_fmt(req, a, "Host: [%s]:%u\r\n",
                           url->host->data, (unsigned)url->port);
            } else {
                append_fmt(req, a, "Host: [%s]\r\n", url->host->data);
            }
        } else if (needs_port) {
            append_fmt(req, a, "Host: %s:%u\r\n",
                       url->host->data, (unsigned)url->port);
        } else {
            append_fmt(req, a, "Host: %s\r\n", url->host->data);
        }
    }

    if (!extra_has(extra, "User-Agent")) {
        append_fmt(req, a, "User-Agent: %s\r\n", user_agent);
    }
    if (!extra_has(extra, "Accept")) {
        append_cstr(req, "Accept: */*\r\n", a);
    }
    if (!extra_has(extra, "Connection")) {
        append_cstr(req,
                    keep_alive ? "Connection: keep-alive\r\n"
                               : "Connection: close\r\n",
                    a);
    }

    if (body) {
        if (content_type && !extra_has(extra, "Content-Type")) {
            append_fmt(req, a, "Content-Type: %s\r\n", content_type);
        }
        if (!extra_has(extra, "Content-Length")) {
            append_fmt(req, a, "Content-Length: %zu\r\n",
                       (size_t)n00b_buffer_len(body));
        }
    }

    if (extra) {
        size_t n = n00b_http_h1_headers_len(extra);
        for (size_t i = 0; i < n; i++) {
            const char *name;
            const char *value;
            if (n00b_http_h1_headers_at(extra, i, &name, &value)) {
                append_fmt(req, a, "%s: %s\r\n", name, value);
            }
        }
    }

    append_cstr(req, "\r\n", a);

    if (body) {
        n00b_buffer_concat(req, body);
    }
    return req;
}

static n00b_buffer_t *
h1_request_headers_build_chunked(n00b_http_url_t        *url,
                                 const char             *method,
                                 const char             *content_type,
                                 n00b_http_h1_headers_t *extra,
                                 bool                    keep_alive,
                                 n00b_allocator_t       *a)
{
    n00b_buffer_t *req = n00b_buffer_empty(.allocator = a);

    const char *path = url->path && url->path->u8_bytes
                           ? url->path->data
                           : "/";
    if (url->query && url->query->u8_bytes > 0) {
        append_fmt(req, a, "%s %s?%s HTTP/1.1\r\n",
                   method, path, url->query->data);
    }
    else {
        append_fmt(req, a, "%s %s HTTP/1.1\r\n", method, path);
    }

    if (!extra_has(extra, "Host")) {
        bool needs_port = url->has_explicit_port && url->port != 443;
        if (url->is_ipv6_literal) {
            append_fmt(req,
                       a,
                       needs_port ? "Host: [%s]:%u\r\n" : "Host: [%s]\r\n",
                       url->host->data,
                       (unsigned)url->port);
        }
        else if (needs_port) {
            append_fmt(req, a, "Host: %s:%u\r\n",
                       url->host->data, (unsigned)url->port);
        }
        else {
            append_fmt(req, a, "Host: %s\r\n", url->host->data);
        }
    }
    if (!extra_has(extra, "User-Agent")) {
        append_cstr(req, "User-Agent: n00b-http/0.1\r\n", a);
    }
    if (!extra_has(extra, "Accept")) {
        append_cstr(req, "Accept: */*\r\n", a);
    }
    if (!extra_has(extra, "Connection")) {
        append_cstr(req,
                    keep_alive ? "Connection: keep-alive\r\n"
                               : "Connection: close\r\n",
                    a);
    }
    if (content_type && !extra_has(extra, "Content-Type")) {
        append_fmt(req, a, "Content-Type: %s\r\n", content_type);
    }
    if (!extra_has(extra, "Transfer-Encoding")) {
        append_cstr(req, "Transfer-Encoding: chunked\r\n", a);
    }

    if (extra) {
        size_t n = n00b_http_h1_headers_len(extra);
        for (size_t i = 0; i < n; i++) {
            const char *name;
            const char *value;
            if (!n00b_http_h1_headers_at(extra, i, &name, &value)) {
                continue;
            }
            if (n00b_ascii_ci_eq(name, "Content-Length")
                || n00b_ascii_ci_eq(name, "Transfer-Encoding")) {
                continue;
            }
            append_fmt(req, a, "%s: %s\r\n", name, value);
        }
    }

    append_cstr(req, "\r\n", a);
    return req;
}

/* ===========================================================================
 * §4   Round trip
 * =========================================================================== */

/* Linear case-insensitive header-bytes scan.  Returns nullptr if
 * @p name isn't present in [@p p, @p p + len). */
static const char *
find_header(const char *p, size_t len, const char *name, size_t *out_vlen)
{
    size_t nlen = strlen(name);
    /* Walk lines until end-of-headers. */
    size_t i = 0;
    while (i + 1 < len && !(p[i] == '\r' && p[i + 1] == '\n')) {
        size_t line_end = i;
        while (line_end + 1 < len
               && !(p[line_end] == '\r' && p[line_end + 1] == '\n')) {
            line_end++;
        }
        if (line_end > i + nlen + 1
            && (size_t)(line_end - i) > nlen + 1
            && n00b_ascii_ci_eq_n(p + i, name, nlen)
            && p[i + nlen] == ':') {
            size_t v = i + nlen + 1;
            while (v < line_end && (p[v] == ' ' || p[v] == '\t')) v++;
            *out_vlen = (size_t)(line_end - v);
            return p + v;
        }
        i = line_end + 2;
    }
    return nullptr;
}

/* Parse a 3-digit HTTP status code from the status line bytes.  The
 * status-line shape is `HTTP/1.X SSS REASON` (RFC 9112 § 4); we walk
 * to the first space and then read three decimal digits.  Returns 0
 * if the status-line bytes are malformed (caller treats it the same
 * as "no boundary yet"). */
static int
parse_status_for_completion(const char *bytes, size_t len)
{
    /* Skip the "HTTP/1.X " prefix.  Anything shorter than that won't
     * have a parseable status. */
    if (len < 12) return 0;
    size_t i = 0;
    while (i < len && bytes[i] != ' ' && bytes[i] != '\r') i++;
    if (i + 4 > len) return 0;
    while (i < len && bytes[i] == ' ') i++;
    if (i + 3 > len) return 0;
    if (bytes[i] < '0' || bytes[i] > '9'
        || bytes[i + 1] < '0' || bytes[i + 1] > '9'
        || bytes[i + 2] < '0' || bytes[i + 2] > '9') {
        return 0;
    }
    return (bytes[i] - '0') * 100
         + (bytes[i + 1] - '0') * 10
         + (bytes[i + 2] - '0');
}

/* Decide whether the bytes accumulated so far form a complete
 * HTTP/1.1 response.  Returns:
 *   1 — complete; safe to stop reading + parse
 *   0 — need more bytes
 *  -1 — protocol error (no Content-Length / no Transfer-Encoding;
 *       caller must read until peer close to find the boundary).
 *
 * RFC 9112 § 6.3: HEAD responses and 1xx / 204 / 304 status codes are
 * always terminated by the empty line after the header block,
 * regardless of any Content-Length or Transfer-Encoding fields the
 * server may carry over from the resource description.  Failing to
 * honor that case sends the reader into a forever-wait for body
 * bytes that will never arrive — manifesting at the libn00b call
 * site as N00B_QUIC_ERR_TIMEOUT (-10) on what should be a quick
 * HEAD round-trip. */
static int
h1_response_complete(const char *bytes, size_t len, bool was_head)
{
    /* Locate end-of-headers (CRLF CRLF). */
    if (len < 4) return 0;
    const char *eoh = nullptr;
    for (size_t i = 0; i + 4 <= len; i++) {
        if (bytes[i] == '\r' && bytes[i + 1] == '\n'
            && bytes[i + 2] == '\r' && bytes[i + 3] == '\n') {
            eoh = bytes + i + 4;
            break;
        }
    }
    if (!eoh) return 0;
    size_t header_len = (size_t)(eoh - bytes);

    /* HEAD response — boundary IS end-of-headers per RFC 9112 § 6.3
     * case 1.  Decided before scanning Content-Length / Transfer-
     * Encoding so any such advisory header is ignored. */
    if (was_head) return 1;

    /* Status codes that never carry a body — same RFC 9112 § 6.3
     * case 1 boundary rule applies. */
    int status = parse_status_for_completion(bytes, len);
    if (status == 204 || status == 304
        || (status >= 100 && status < 200)) {
        return 1;
    }

    /* Skip the status line (first \r\n) for header scan. */
    const char *hp = bytes;
    size_t hl = header_len;
    for (size_t i = 0; i + 1 < hl; i++) {
        if (hp[i] == '\r' && hp[i + 1] == '\n') {
            hp = bytes + i + 2;
            hl = header_len - (i + 2);
            break;
        }
    }

    size_t vlen = 0;
    const char *cl = find_header(hp, hl, "Content-Length", &vlen);
    const char *te = find_header(hp, hl, "Transfer-Encoding", &vlen);

    if (te && vlen >= 7
        && n00b_ascii_ci_eq_n(te, "chunked", 7)) {
        /* Walk the chunked-body frames properly so a literal
         * "0\r\n\r\n" byte sequence inside a chunk's payload doesn't
         * spuriously look like the terminator.  Each frame is:
         *   <hex-size> [;ext]* CRLF <size bytes> CRLF
         * The terminator is a frame with size 0; any trailers follow
         * up to the final empty CRLF.  Returns 0 (need more) on a
         * truncated frame, 1 on a complete terminator, and stays 0
         * on a chunk-with-trailers — read-until-close still wins
         * there. */
        size_t pos = (size_t)(eoh - bytes);
        while (pos < len) {
            /* Find size-line CRLF. */
            size_t eol = pos;
            while (eol + 1 < len
                   && !(bytes[eol] == '\r' && bytes[eol + 1] == '\n')) {
                eol++;
            }
            if (eol + 1 >= len) return 0;       /* size line truncated */
            /* Parse hex size up to ';' or end-of-line. */
            size_t   chunk_size = 0;
            size_t   p          = pos;
            bool     have_digit = false;
            while (p < eol && bytes[p] != ';') {
                char ch = bytes[p];
                int  d  = -1;
                if (ch >= '0' && ch <= '9')      d = ch - '0';
                else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
                else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
                else break;
                chunk_size = (chunk_size << 4) | (size_t)d;
                have_digit = true;
                p++;
            }
            if (!have_digit) return 0;           /* malformed; wait */
            size_t data_off = eol + 2;
            if (chunk_size == 0) {
                /* Terminator: optional trailers terminated by CRLF. */
                if (data_off + 1 >= len) return 0;
                /* Walk trailer lines until empty line. */
                size_t t = data_off;
                while (t + 1 < len) {
                    if (bytes[t] == '\r' && bytes[t + 1] == '\n') {
                        return 1;
                    }
                    /* Skip trailer line. */
                    while (t + 1 < len
                           && !(bytes[t] == '\r' && bytes[t + 1] == '\n')) {
                        t++;
                    }
                    if (t + 1 >= len) return 0;
                    t += 2;
                }
                return 0;
            }
            /* Need data_off + chunk_size + 2 (CRLF after data) bytes. */
            if (data_off + chunk_size + 2 > len) return 0;
            if (bytes[data_off + chunk_size] != '\r'
                || bytes[data_off + chunk_size + 1] != '\n') {
                return -1;                       /* protocol violation */
            }
            pos = data_off + chunk_size + 2;
        }
        return 0;
    }
    if (cl) {
        size_t  declared = (size_t)n00b_parse_byte_count_span(cl, vlen);
        size_t  body_have = len - header_len;
        return body_have >= declared ? 1 : 0;
    }
    /* Neither Content-Length nor Transfer-Encoding — message
     * boundary is the connection close.  Caller cannot pool. */
    return -1;
}

/* ===========================================================================
 * §2.5  Conduit-native TLS transport path
 *
 * The synchronous acme_tls path (below) hands a caller's GC-managed buffer
 * straight to `ptls_send`, so a moving collection on a worker thread could
 * relocate the plaintext mid-encrypt (the acme_tls SIGSEGV class).  The
 * conduit TLS transport makes TLS an ordinary pipeline stage: plaintext rides
 * `n00b_conduit_tls_write` (which copies into a pinned conduit-pool buffer) and
 * the conduit owns every byte across the encrypt/decrypt boundary, so the
 * crash class is gone by construction.  It is also async over the shared IO
 * thread instead of a per-call blocking poll() loop.
 *
 * This is the transport for the common HTTPS case.  mTLS client auth is not
 * yet expressible on the conduit TLS connect API, so requests carrying a
 * client identity fall through to the acme_tls path, which stays reachable.
 * =========================================================================== */

/* A pooled conduit-TLS connection: the session plus the read subscription that
 * drains decrypted plaintext into `inbox`.  The subscription is created once at
 * connect and reused across pooled keep-alive requests; it is torn down only
 * when the connection is discarded (h1_tls_conn_close).  Allocated from the
 * conduit pool (pinned, non-moving) so the pool's stored pointer and the
 * inbox's cross-thread CV/lock never move under a collection. */
typedef struct {
    n00b_conduit_tls_t                    *tls;
    n00b_conduit_inbox_t(n00b_buffer_t *) *inbox;
    n00b_conduit_sub_handle_t              sub;
    uint32_t                               use_count;
} h1_tls_conn_t;

#define H1_TLS_CONN_MAX_KEEPALIVE_REQUESTS 64

static void
h1_tls_conn_drain_inbox(h1_tls_conn_t *tc)
{
    if (!tc || !tc->inbox) {
        return;
    }
    n00b_conduit_message_t(n00b_buffer_t *) *msg;
    while ((msg = n00b_conduit_inbox_pop_msg(n00b_buffer_t *, tc->inbox))
           != nullptr) {
        n00b_buffer_t *payload = msg->payload;
        msg->payload = nullptr;
        n00b_buffer_free_with_allocator_hint(payload, default_pool());
        n00b_free_with_allocator_hint(default_pool(), msg);
    }
    n00b_conduit_sys_msg_t *sys;
    while ((sys = n00b_conduit_inbox_pop_sys(tc->inbox)) != nullptr) {
        n00b_free_with_allocator_hint(default_pool(), sys);
    }
}

static bool
h1_tls_conn_reusable(h1_tls_conn_t *tc)
{
    if (!tc || !tc->tls || !tc->inbox || !n00b_conduit_tls_is_ready(tc->tls)) {
        return false;
    }
    if (tc->use_count >= H1_TLS_CONN_MAX_KEEPALIVE_REQUESTS) {
        return false;
    }
    bool saw_sys = false;
    bool saw_payload = n00b_conduit_inbox_has_msg(n00b_buffer_t *, tc->inbox);
    while (n00b_conduit_inbox_has_sys(tc->inbox)) {
        n00b_conduit_sys_msg_t *sys = n00b_conduit_inbox_pop_sys(tc->inbox);
        saw_sys = true;
        n00b_free_with_allocator_hint(default_pool(), sys);
    }
    if (saw_sys || saw_payload) {
        h1_tls_conn_drain_inbox(tc);
        return false;
    }
    return true;
}

/* Pool close-fn shape: close the TLS pipeline, then cancel the app read
 * subscription. Idempotent (n00b_conduit_tls_close is). */
static void
h1_tls_conn_close(void *u)
{
    h1_tls_conn_t *tc = u;
    if (!tc) {
        return;
    }
    /* Stop TLS/xform publishers before canceling the app read subscription;
     * otherwise cancel can wait forever for the decrypt xform's publisher. */
    if (tc->tls) {
        n00b_conduit_tls_close(tc->tls);
        tc->tls = nullptr;
    }
    if (tc->sub != N00B_CONDUIT_INVALID_SUB_HANDLE) {
        n00b_conduit_sub_cancel(tc->sub);
        tc->sub = N00B_CONDUIT_INVALID_SUB_HANDLE;
    }
    if (tc->inbox) {
        n00b_conduit_inbox_destroy(n00b_buffer_t *, tc->inbox);
        n00b_free_with_allocator_hint(default_pool(), tc->inbox);
        tc->inbox = nullptr;
    }
    n00b_free_with_allocator_hint(default_pool(), tc);
}

/* Open a fresh conduit-TLS connection over the per-runtime conduit + IO thread,
 * subscribe a read inbox to the plaintext topic, and bundle them. */
static n00b_result_t(h1_tls_conn_t *)
h1_tls_connect(n00b_http_url_t *url, n00b_quic_trust_t *trust,
               int32_t timeout_ms)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    if (rt == nullptr || rt->default_conduit == nullptr
        || rt->default_service == nullptr) {
        return n00b_result_err(h1_tls_conn_t *, N00B_QUIC_ERR_PROTOCOL);
    }
    n00b_option_t(n00b_conduit_svc_thread_t *) st_opt =
        n00b_conduit_service_default_io(rt->default_service);
    if (!n00b_option_is_set(st_opt)) {
        return n00b_result_err(h1_tls_conn_t *, N00B_QUIC_ERR_PROTOCOL);
    }
    n00b_option_t(n00b_conduit_io_backend_t *) io_opt =
        n00b_conduit_svc_thread_io(n00b_option_get(st_opt));
    if (!n00b_option_is_set(io_opt)) {
        return n00b_result_err(h1_tls_conn_t *, N00B_QUIC_ERR_PROTOCOL);
    }
    n00b_conduit_t            *c  = rt->default_conduit;
    n00b_conduit_io_backend_t *io = n00b_option_get(io_opt);

    /* HTTP_PROXY / HTTPS_PROXY / NO_PROXY. .active == false (no env var
     * set, or the host is NO_PROXY-excluded) leaves the proxy kwargs at
     * their nullptr/0 defaults below, i.e. today's direct-dial behavior. */
    n00b_http_proxy_route_t proxy = n00b_http_proxy_resolve(url);

    /* An `https://` proxy URL needs a TLS-wrapped connection to the proxy
     * itself before the plaintext CONNECT request goes out; the CONNECT
     * tunnel in xform_tls.c only ever speaks plaintext HTTP to the proxy.
     * Reject here, before any socket opens, rather than silently sending
     * CONNECT (and any Proxy-Authorization credential) in cleartext to
     * what the operator configured as a TLS-only proxy. */
    if (proxy.active && proxy.requires_tls) {
        return n00b_result_err(h1_tls_conn_t *,
                               N00B_HTTP_ERR_PROXY_TLS_UNSUPPORTED);
    }

    auto cr = n00b_conduit_tls_connect(c, io, url->host, url->port,
                                       .trust      = trust,
                                       .timeout_ms = timeout_ms,
                                       .proxy_host = proxy.active ? proxy.host : nullptr,
                                       .proxy_port = proxy.active ? proxy.port : 0,
                                       .proxy_extra_headers = proxy.active ? proxy.proxy_auth_header : nullptr);
    if (n00b_result_is_err(cr)) {
        return n00b_result_err(h1_tls_conn_t *, n00b_result_get_err(cr));
    }
    n00b_conduit_tls_t *tls = n00b_result_get(cr);

    auto read_topic = n00b_conduit_tls_read_topic(tls);
    if (!read_topic) {
        n00b_conduit_tls_close(tls);
        return n00b_result_err(h1_tls_conn_t *, N00B_QUIC_ERR_PROTOCOL);
    }

    /* The inbox embeds a CV/lock the decrypt worker notifies across threads, so
     * it MUST be pinned (conduit pool), never the movable default arena. */
    n00b_conduit_inbox_t(n00b_buffer_t *) *inbox = n00b_alloc_with_opts(
        n00b_conduit_inbox_t(n00b_buffer_t *),
        &(n00b_alloc_opts_t){.allocator = c->allocator});
    n00b_conduit_inbox_init(n00b_buffer_t *, inbox, c,
                            N00B_CONDUIT_BP_UNBOUNDED, 0);

    auto sr = n00b_conduit_read_async(n00b_buffer_t *, read_topic, inbox);
    if (n00b_result_is_err(sr)) {
        n00b_conduit_tls_close(tls);
        n00b_conduit_inbox_destroy(n00b_buffer_t *, inbox);
        n00b_free_with_allocator_hint(c->allocator, inbox);
        return n00b_result_err(h1_tls_conn_t *, N00B_QUIC_ERR_PROTOCOL);
    }

    h1_tls_conn_t *tc = n00b_alloc_with_opts(
        h1_tls_conn_t, &(n00b_alloc_opts_t){.allocator = c->allocator});
    tc->tls   = tls;
    tc->inbox = inbox;
    tc->sub   = n00b_result_get(sr).handle;
    return n00b_result_ok(h1_tls_conn_t *, tc);
}

/* Pop one decrypted plaintext chunk from the read inbox, blocking on the inbox
 * CV until data arrives, the peer closes, or the timeout budget is exhausted.
 * Mirrors n00b_acme_tls_recv's contract: *out_chunk may be set to a buffer (the
 * caller concats it), *peer_closed is set on TLS close_notify / read EOF. */
static int
h1_tls_recv(h1_tls_conn_t *tc, n00b_buffer_t **out_chunk, bool *peer_closed,
            int32_t timeout_ms)
{
    *out_chunk = nullptr;
    /* CV wait granularity is 50ms; budget the spins to the caller's timeout. */
    int64_t spins = (int64_t)(timeout_ms > 0 ? timeout_ms : 30000) / 50 + 1;
    while (spins-- > 0) {
        auto msg = n00b_conduit_inbox_pop_msg(n00b_buffer_t *, tc->inbox);
        if (msg) {
            *out_chunk = msg->payload;
            msg->payload = nullptr;
            n00b_free_with_allocator_hint(default_pool(), msg);
            return N00B_QUIC_OK;
        }
        if (n00b_conduit_inbox_has_sys(tc->inbox)) {
            n00b_conduit_sys_msg_t *sys =
                n00b_conduit_inbox_pop_sys(tc->inbox);
            if (sys && sys->header.type == N00B_CONDUIT_MSG_TOPIC_CLOSED) {
                *peer_closed = true;
                n00b_free_with_allocator_hint(default_pool(), sys);
                return N00B_QUIC_OK;
            }
            if (sys) {
                n00b_free_with_allocator_hint(default_pool(), sys);
            }
            continue;
        }
        n00b_condition_wait(&tc->inbox->cv, .timeout_ms = 50,
                            .auto_unlock = true);
    }
    return N00B_QUIC_ERR_TIMEOUT;
}

/* The HTTPS round trip over the conduit TLS transport.  Structure mirrors the
 * acme_tls path below (request build → write → read-to-boundary → parse →
 * pool/close); only the connect/write/recv primitives differ. */
static n00b_result_t(n00b_http_h1_response_t *)
h1_round_trip_conduit_tls(n00b_http_url_t             *url,
                          const char                  *method,
                          n00b_buffer_t               *body,
                          const char                  *content_type,
                          n00b_http_h1_headers_t      *extra,
                          int32_t                      timeout_ms,
                          n00b_http_connection_pool_t *pool,
                          n00b_quic_trust_t           *trust,
                          uint64_t                     max_body_size,
                          n00b_allocator_t            *a,
                          n00b_string_t               *bucket_origin,
                          bool                         keep_alive_intent,
                          bool                         was_head)
{
    /* Try the pool first (if enabled). */
    h1_tls_conn_t *tc = nullptr;
    if (pool) {
        void *u = n00b_http_connection_pool_acquire(
            pool, bucket_origin, N00B_HTTP_CONNECTION_POOL_TRANSPORT_H1);
        if (u) {
            tc = (h1_tls_conn_t *)u;
            if (!h1_tls_conn_reusable(tc)) {
                h1_tls_conn_close(tc);
                tc = nullptr;
            }
        }
    }
    if (!tc) {
        auto cr = h1_tls_connect(url, trust, timeout_ms);
        if (n00b_result_is_err(cr)) {
            return n00b_result_err(n00b_http_h1_response_t *,
                                   n00b_result_get_err(cr));
        }
        tc = n00b_result_get(cr);
    }

    n00b_buffer_t *req = n00b_http_h1_request_build(
        url,
        .method       = method,
        .body         = body,
        .content_type = content_type,
        .extra        = extra,
        .keep_alive   = keep_alive_intent,
        .allocator    = a);

    /* TEMP egress bisect: header/body split going onto the wire. Off unless
     * CRAYON_EGRESS_WIRE_LOG is set (cached). */
    {
        static int _wire = -1;
        if (_wire < 0) {
            _wire = n00b_getenv(r"CRAYON_EGRESS_WIRE_LOG") ? 1 : 0;
        }
        if (_wire) {
            int64_t body_len  = body ? (int64_t)n00b_buffer_len(body) : 0;
            int64_t total_len = (int64_t)n00b_buffer_len(req);
            n00b_eprintf("[tls-req] header=[|#|] body=[|#|] total=[|#|] bytes",
                         total_len - body_len,
                         body_len,
                         total_len);
        }
    }

    auto wr = n00b_conduit_tls_write(tc->tls, req);
    if (n00b_result_is_err(wr)) {
        h1_tls_conn_close(tc);
        return n00b_result_err(n00b_http_h1_response_t *,
                               n00b_result_get_err(wr));
    }

    /* Read until the response message boundary is detected (per Content-Length
     * / chunked) OR the peer closes.  Identical cap + boundary logic to the
     * acme_tls path. */
    n00b_buffer_t *raw = n00b_buffer_empty(.allocator = a);
    bool peer_closed   = false;
    bool boundary_seen = false;
    bool read_to_eof   = false;
    size_t header_len  = 0;     /* 0 = end-of-headers not yet seen */
    bool   cl_checked  = false; /* Content-Length already sniffed? */
    int32_t effective_timeout_ms = timeout_ms > 0 ? timeout_ms : 30000;
    uint64_t deadline_ns = n00b_ns_timestamp()
                         + (uint64_t)effective_timeout_ms * 1000000ULL;
    while (!peer_closed && !boundary_seen) {
        n00b_buffer_t *chunk = nullptr;
        int32_t remain_ms = h1_remaining_timeout_ms(deadline_ns);
        if (remain_ms <= 0) {
            h1_tls_conn_close(tc);
            return n00b_result_err(n00b_http_h1_response_t *,
                                   N00B_QUIC_ERR_TIMEOUT);
        }
        int rc = h1_tls_recv(tc, &chunk, &peer_closed, remain_ms);
        if (rc != N00B_QUIC_OK) {
            h1_tls_conn_close(tc);
            return n00b_result_err(n00b_http_h1_response_t *, rc);
        }
        if (chunk) {
            if (chunk->byte_len > 0) {
                n00b_buffer_concat(raw, chunk);
            }
            n00b_buffer_free_with_allocator_hint(chunk, default_pool());

            /* (a) End-of-headers sniff + Content-Length up-front cap. */
            if (max_body_size > 0 && header_len == 0) {
                const char *bytes = raw->data;
                size_t      len   = (size_t)raw->byte_len;
                for (size_t i = 0; i + 4 <= len; i++) {
                    if (bytes[i] == '\r' && bytes[i + 1] == '\n'
                        && bytes[i + 2] == '\r' && bytes[i + 3] == '\n') {
                        header_len = i + 4;
                        break;
                    }
                }
                if (header_len > 0 && !cl_checked && !was_head) {
                    cl_checked = true;
                    const char *hp = bytes;
                    size_t      hl = header_len;
                    for (size_t i = 0; i + 1 < hl; i++) {
                        if (hp[i] == '\r' && hp[i + 1] == '\n') {
                            hp = bytes + i + 2;
                            hl = header_len - (i + 2);
                            break;
                        }
                    }
                    size_t      vlen = 0;
                    const char *cl   = find_header(hp, hl, "Content-Length",
                                                   &vlen);
                    if (cl) {
                        uint64_t declared
                            = n00b_parse_byte_count_span(cl, vlen);
                        if (declared > max_body_size) {
                            h1_tls_conn_close(tc);
                            return n00b_result_err(
                                n00b_http_h1_response_t *,
                                N00B_HTTP_ERR_RESPONSE_TOO_LARGE);
                        }
                    }
                }
            }

            /* (b) Streaming guard on accumulated body bytes. */
            if (max_body_size > 0 && header_len > 0 && !was_head) {
                uint64_t body_have = (uint64_t)raw->byte_len
                                     - (uint64_t)header_len;
                if (body_have > max_body_size) {
                    h1_tls_conn_close(tc);
                    return n00b_result_err(n00b_http_h1_response_t *,
                                           N00B_HTTP_ERR_RESPONSE_TOO_LARGE);
                }
            }

            int complete = h1_response_complete(raw->data,
                                                (size_t)raw->byte_len,
                                                was_head);
            if (complete == 1) boundary_seen = true;
            else if (complete == -1) read_to_eof = true;
        }
        if (read_to_eof && !peer_closed) {
            keep_alive_intent = false;
            continue;
        }
    }

    auto pres = n00b_http_h1_response_parse(raw, .allocator = a);
    if (n00b_result_is_err(pres)) {
        h1_tls_conn_close(tc);
        return pres;
    }
    n00b_http_h1_response_t *resp = n00b_result_get(pres);

    if (pool && keep_alive_intent && resp->keep_alive
        && boundary_seen && !peer_closed) {
        tc->use_count++;
        n00b_http_connection_pool_release(
            pool, bucket_origin, N00B_HTTP_CONNECTION_POOL_TRANSPORT_H1,
            tc, h1_tls_conn_close);
    } else {
        h1_tls_conn_close(tc);
    }
    return n00b_result_ok(n00b_http_h1_response_t *, resp);
}

typedef struct {
    h1_tls_conn_t *tc;
} h1_chunked_write_ctx_t;

static n00b_result_t(bool)
h1_submit_raw(void *ctx, const void *data, size_t len)
{
    h1_chunked_write_ctx_t *w = (h1_chunked_write_ctx_t *)ctx;
    return n00b_conduit_tls_submit_raw(w->tc->tls, data, len);
}

static n00b_result_t(bool)
h1_submit_chunked(void *ctx, const void *data, size_t len)
{
    if (len == 0) {
        return n00b_result_ok(bool, true);
    }

    char hdr[32];
    int  n = snprintf(hdr, sizeof(hdr), "%zx\r\n", len);
    if (n <= 0 || (size_t)n >= sizeof(hdr)) {
        return n00b_result_err(bool, N00B_HTTP_ERR_BAD_RESPONSE);
    }

    auto hr = h1_submit_raw(ctx, hdr, (size_t)n);
    if (n00b_result_is_err(hr)) {
        return hr;
    }
    auto br = h1_submit_raw(ctx, data, len);
    if (n00b_result_is_err(br)) {
        return br;
    }
    return h1_submit_raw(ctx, "\r\n", 2);
}

n00b_result_t(n00b_http_h1_response_t *)
n00b_http_h1_round_trip_stream(n00b_http_url_t *url,
                               n00b_http_h1_body_stream_fn body_stream,
                               void *body_ctx)
    _kargs {
        const char                  *method        = "POST";
        const char                  *content_type  = nullptr;
        n00b_http_h1_headers_t      *extra         = nullptr;
        int32_t                      timeout_ms    = 30000;
        n00b_http_connection_pool_t *pool          = nullptr;
        n00b_quic_trust_t           *trust         = nullptr;
        uint64_t                     max_body_size = 0;
        n00b_allocator_t            *allocator     = nullptr;
    }
{
    if (!url || !body_stream) {
        return n00b_result_err(n00b_http_h1_response_t *,
                               N00B_HTTP_ERR_NULL_ARG);
    }

    n00b_allocator_t *a = allocator ? allocator : default_pool();
    bool keep_alive_intent = (pool != nullptr);
    bool was_head = method != nullptr && n00b_ascii_ci_eq(method, "HEAD");

    n00b_string_t *bucket_origin = url->origin;
    h1_tls_conn_t *tc = nullptr;
    if (pool) {
        void *u = n00b_http_connection_pool_acquire(
            pool, bucket_origin, N00B_HTTP_CONNECTION_POOL_TRANSPORT_H1);
        if (u) {
            tc = (h1_tls_conn_t *)u;
            if (!h1_tls_conn_reusable(tc)) {
                h1_tls_conn_close(tc);
                tc = nullptr;
            }
        }
    }
    if (!tc) {
        auto cr = h1_tls_connect(url, trust, timeout_ms);
        if (n00b_result_is_err(cr)) {
            return n00b_result_err(n00b_http_h1_response_t *,
                                   n00b_result_get_err(cr));
        }
        tc = n00b_result_get(cr);
    }

    n00b_buffer_t *headers = h1_request_headers_build_chunked(
        url,
        method,
        content_type,
        extra,
        keep_alive_intent,
        a);
    h1_chunked_write_ctx_t write_ctx = {.tc = tc};

    auto wr = h1_submit_raw(&write_ctx,
                            headers->data,
                            (size_t)headers->byte_len);
    if (n00b_result_is_err(wr)) {
        h1_tls_conn_close(tc);
        return n00b_result_err(n00b_http_h1_response_t *,
                               n00b_result_get_err(wr));
    }

    auto sr = body_stream(body_ctx, h1_submit_chunked, &write_ctx);
    if (n00b_result_is_err(sr)) {
        h1_tls_conn_close(tc);
        return n00b_result_err(n00b_http_h1_response_t *,
                               n00b_result_get_err(sr));
    }

    auto end_r = h1_submit_raw(&write_ctx, "0\r\n\r\n", 5);
    if (n00b_result_is_err(end_r)) {
        h1_tls_conn_close(tc);
        return n00b_result_err(n00b_http_h1_response_t *,
                               n00b_result_get_err(end_r));
    }

    n00b_buffer_t *raw = n00b_buffer_empty(.allocator = a);
    bool peer_closed   = false;
    bool boundary_seen = false;
    bool read_to_eof   = false;
    size_t header_len  = 0;
    bool   cl_checked  = false;
    int32_t effective_timeout_ms = timeout_ms > 0 ? timeout_ms : 30000;
    uint64_t deadline_ns = n00b_ns_timestamp()
                         + (uint64_t)effective_timeout_ms * 1000000ULL;
    while (!peer_closed && !boundary_seen) {
        n00b_buffer_t *chunk = nullptr;
        int32_t remain_ms = h1_remaining_timeout_ms(deadline_ns);
        if (remain_ms <= 0) {
            h1_tls_conn_close(tc);
            return n00b_result_err(n00b_http_h1_response_t *,
                                   N00B_QUIC_ERR_TIMEOUT);
        }
        int rc = h1_tls_recv(tc, &chunk, &peer_closed, remain_ms);
        if (rc != N00B_QUIC_OK) {
            h1_tls_conn_close(tc);
            return n00b_result_err(n00b_http_h1_response_t *, rc);
        }
        if (chunk) {
            if (chunk->byte_len > 0) {
                n00b_buffer_concat(raw, chunk);
            }
            n00b_buffer_free_with_allocator_hint(chunk, default_pool());

            if (max_body_size > 0 && header_len == 0) {
                const char *bytes = raw->data;
                size_t      len   = (size_t)raw->byte_len;
                for (size_t i = 0; i + 4 <= len; i++) {
                    if (bytes[i] == '\r' && bytes[i + 1] == '\n'
                        && bytes[i + 2] == '\r' && bytes[i + 3] == '\n') {
                        header_len = i + 4;
                        break;
                    }
                }
                if (header_len > 0 && !cl_checked && !was_head) {
                    cl_checked = true;
                    const char *hp = bytes;
                    size_t      hl = header_len;
                    for (size_t i = 0; i + 1 < hl; i++) {
                        if (hp[i] == '\r' && hp[i + 1] == '\n') {
                            hp = bytes + i + 2;
                            hl = header_len - (i + 2);
                            break;
                        }
                    }
                    size_t      vlen = 0;
                    const char *cl   = find_header(hp, hl, "Content-Length",
                                                   &vlen);
                    if (cl) {
                        uint64_t declared
                            = n00b_parse_byte_count_span(cl, vlen);
                        if (declared > max_body_size) {
                            h1_tls_conn_close(tc);
                            return n00b_result_err(
                                n00b_http_h1_response_t *,
                                N00B_HTTP_ERR_RESPONSE_TOO_LARGE);
                        }
                    }
                }
            }

            if (max_body_size > 0 && header_len > 0 && !was_head) {
                uint64_t body_have = (uint64_t)raw->byte_len
                                     - (uint64_t)header_len;
                if (body_have > max_body_size) {
                    h1_tls_conn_close(tc);
                    return n00b_result_err(n00b_http_h1_response_t *,
                                           N00B_HTTP_ERR_RESPONSE_TOO_LARGE);
                }
            }

            int complete = h1_response_complete(raw->data,
                                                (size_t)raw->byte_len,
                                                was_head);
            if (complete == 1) {
                boundary_seen = true;
            }
            else if (complete == -1) {
                read_to_eof = true;
            }
        }
        if (read_to_eof && !peer_closed) {
            keep_alive_intent = false;
            continue;
        }
    }

    auto pres = n00b_http_h1_response_parse(raw, .allocator = a);
    if (n00b_result_is_err(pres)) {
        h1_tls_conn_close(tc);
        return pres;
    }
    n00b_http_h1_response_t *resp = n00b_result_get(pres);

    if (pool && keep_alive_intent && resp->keep_alive
        && boundary_seen && !peer_closed) {
        tc->use_count++;
        n00b_http_connection_pool_release(
            pool, bucket_origin, N00B_HTTP_CONNECTION_POOL_TRANSPORT_H1,
            tc, h1_tls_conn_close);
    }
    else {
        h1_tls_conn_close(tc);
    }
    return n00b_result_ok(n00b_http_h1_response_t *, resp);
}

n00b_result_t(n00b_http_h1_response_t *)
n00b_http_h1_round_trip(n00b_http_url_t *url)
    _kargs {
        const char                  *method       = "GET";
        n00b_buffer_t               *body         = nullptr;
        const char                  *content_type = nullptr;
        n00b_http_h1_headers_t      *extra        = nullptr;
        int32_t                      timeout_ms   = 30000;
        n00b_http_connection_pool_t *pool         = nullptr;
        n00b_http_auth_t            *auth         = nullptr;
        n00b_quic_trust_t           *trust        = nullptr;
        /** Per-call response-body byte cap.  Default 0 = no cap.
         *  When non-zero, the receive loop aborts as soon as the
         *  accumulated wire bytes (status line + headers + body)
         *  would exceed the cap and surfaces
         *  `N00B_HTTP_ERR_RESPONSE_TOO_LARGE`.  An advertised
         *  `Content-Length` greater than the cap short-circuits
         *  before any body bytes are read. */
        uint64_t                     max_body_size = 0;
        n00b_allocator_t            *allocator    = nullptr;
    }
{
    if (!url) {
        return n00b_result_err(n00b_http_h1_response_t *,
                               N00B_HTTP_ERR_NULL_ARG);
    }
    n00b_allocator_t *a = allocator ? allocator : default_pool();
    bool keep_alive_intent = (pool != nullptr);
    /* HEAD responses have no body regardless of Content-Length /
     * Transfer-Encoding (RFC 9112 § 6.3 case 1).  Capture that here
     * so the read-completion check short-circuits at end-of-headers
     * instead of hanging forever waiting for body bytes that the
     * server will never send. */
    bool was_head = (method != nullptr
                     && n00b_ascii_ci_eq(method, "HEAD"));

    /* mTLS handshake material — extracted from the auth helper if
     * present.  Pool keying uses the auth pointer so identities don't
     * cross-contaminate (see pool_subkey_for_auth). */
    n00b_acme_tls_client_auth_t  mtls_auth_storage;
    n00b_acme_tls_client_auth_t *mtls_auth = nullptr;
    if (auth && auth->mtls_key && auth->mtls_cert_chain_der
        && auth->mtls_cert_chain_count > 0
        && auth->mtls_cert_chain_lens) {
        mtls_auth_storage.cert_chain_der   = auth->mtls_cert_chain_der;
        mtls_auth_storage.cert_chain_lens  = auth->mtls_cert_chain_lens;
        mtls_auth_storage.cert_chain_count = auth->mtls_cert_chain_count;
        mtls_auth_storage.key              = auth->mtls_key;
        mtls_auth = &mtls_auth_storage;
    }

    /* Pool bucket key includes a subkey derived from the auth pointer
     * so two requests with different mTLS identities — or one with
     * mTLS and one without — never share an idle connection. */
    n00b_string_t *bucket_origin = url->origin;
    if (auth) {
        char auth_tag[40];
        snprintf(auth_tag, sizeof(auth_tag), "|auth=%p", (void *)auth);
        size_t orig_len = url->origin->u8_bytes;
        size_t tag_len  = strlen(auth_tag);
        char *buf = n00b_alloc_array_with_opts(char,
                                               orig_len + tag_len + 1,
                                               &(n00b_alloc_opts_t){.allocator = a});
        memcpy(buf, url->origin->data, orig_len);
        memcpy(buf + orig_len, auth_tag, tag_len);
        buf[orig_len + tag_len] = '\0';
        bucket_origin = n00b_string_from_raw(buf,
                                             (int64_t)(orig_len + tag_len),
                                             .allocator = a);
    }

    /* Common case (no mTLS client identity): ride the conduit-native TLS
     * transport.  mTLS requests fall through to the acme_tls path below, which
     * is the only remaining acme_tls user once Phase 3 lands. */
    if (mtls_auth == nullptr) {
        return h1_round_trip_conduit_tls(url, method, body, content_type, extra,
                                         timeout_ms, pool, trust, max_body_size,
                                         a, bucket_origin, keep_alive_intent,
                                         was_head);
    }

    /* mTLS client-cert requests still ride the legacy acme_tls transport
     * (see the module comment above), which dials the origin directly and
     * has no proxy CONNECT support. Rather than let a configured proxy be
     * silently bypassed — skipping proxy logging, auth, and egress
     * controls — fail closed here, before any socket opens, whenever a
     * proxy route applies to this URL. */
    n00b_http_proxy_route_t mtls_proxy = n00b_http_proxy_resolve(url);
    if (mtls_proxy.active) {
        return n00b_result_err(n00b_http_h1_response_t *,
                               N00B_HTTP_ERR_PROXY_MTLS_UNSUPPORTED);
    }

    /* Try the pool first (if enabled). */
    n00b_acme_tls_conn_t *conn = nullptr;
    if (pool) {
        void *u = n00b_http_connection_pool_acquire(
            pool, bucket_origin, N00B_HTTP_CONNECTION_POOL_TRANSPORT_H1);
        if (u) {
            conn = (n00b_acme_tls_conn_t *)u;
            if (!n00b_acme_tls_alive(conn)) {
                n00b_acme_tls_close(conn);
                conn = nullptr;
            }
        }
    }
    if (!conn) {
        int rc = n00b_acme_tls_connect_ex(url->host->data, url->port,
                                          timeout_ms, mtls_auth,
                                          trust, &conn);
        if (rc != N00B_QUIC_OK) {
            return n00b_result_err(n00b_http_h1_response_t *, rc);
        }
    }

    n00b_buffer_t *req = n00b_http_h1_request_build(
        url,
        .method       = method,
        .body         = body,
        .content_type = content_type,
        .extra        = extra,
        .keep_alive   = keep_alive_intent,
        .allocator    = a);

    /* TEMP egress bisect: header/body split going onto the wire. Off unless
     * CRAYON_EGRESS_WIRE_LOG is set (cached). */
    {
        static int _wire = -1;
        if (_wire < 0) {
            _wire = n00b_getenv(r"CRAYON_EGRESS_WIRE_LOG") ? 1 : 0;
        }
        if (_wire) {
            int64_t body_len  = body ? (int64_t)n00b_buffer_len(body) : 0;
            int64_t total_len = (int64_t)n00b_buffer_len(req);
            n00b_eprintf("[tls-req] header=[|#|] body=[|#|] total=[|#|] bytes",
                         total_len - body_len,
                         body_len,
                         total_len);
        }
    }

    int rc = n00b_acme_tls_send(conn, req, timeout_ms);
    if (rc != N00B_QUIC_OK) {
        n00b_acme_tls_close(conn);
        return n00b_result_err(n00b_http_h1_response_t *, rc);
    }

    /* Read until the response message boundary is detected (per
     * Content-Length / chunked) OR the peer closes.
     *
     * When `max_body_size` is non-zero we also short-circuit two
     * paths so a hostile / buggy peer can never push us past the
     * caller-supplied cap:
     *
     *   (a) Once the header block lands we sniff `Content-Length`
     *       and bail if it advertises more body bytes than the cap.
     *       This is a cheap up-front check that avoids reading a
     *       single body byte for an oversized declared body.
     *   (b) On every recv chunk we recompute `body_bytes = wire -
     *       header` and bail if it exceeds the cap.  Covers
     *       chunked responses + responses with no `Content-Length`
     *       (read-to-EOF) where the up-front sniff has nothing to
     *       sniff. */
    n00b_buffer_t *raw = n00b_buffer_empty(.allocator = a);
    bool peer_closed   = false;
    bool boundary_seen = false;
    bool read_to_eof   = false;
    size_t header_len  = 0;       /* 0 = end-of-headers not yet seen */
    bool   cl_checked  = false;   /* Content-Length already sniffed? */
    int32_t effective_timeout_ms = timeout_ms > 0 ? timeout_ms : 30000;
    uint64_t deadline_ns = n00b_ns_timestamp()
                         + (uint64_t)effective_timeout_ms * 1000000ULL;
    while (!peer_closed && !boundary_seen) {
        n00b_buffer_t *chunk = nullptr;
        int32_t remain_ms = h1_remaining_timeout_ms(deadline_ns);
        if (remain_ms <= 0) {
            n00b_acme_tls_close(conn);
            return n00b_result_err(n00b_http_h1_response_t *,
                                   N00B_QUIC_ERR_TIMEOUT);
        }
        rc = n00b_acme_tls_recv(conn, 64 * 1024, &chunk,
                                 &peer_closed, remain_ms,
                                 .allocator = a);
        if (rc != N00B_QUIC_OK) {
            n00b_acme_tls_close(conn);
            return n00b_result_err(n00b_http_h1_response_t *, rc);
        }
        if (chunk && chunk->byte_len > 0) {
            n00b_buffer_concat(raw, chunk);

            /* (a) End-of-headers sniff: locate CRLFCRLF once.  We
             *     then peek `Content-Length` and short-circuit if the
             *     server's advertised body would push past the cap.
             *     Done once-only on the chunk that first contains the
             *     header terminator. */
            if (max_body_size > 0 && header_len == 0) {
                const char *bytes = raw->data;
                size_t      len   = (size_t)raw->byte_len;
                for (size_t i = 0; i + 4 <= len; i++) {
                    if (bytes[i] == '\r' && bytes[i + 1] == '\n'
                        && bytes[i + 2] == '\r' && bytes[i + 3] == '\n') {
                        header_len = i + 4;
                        break;
                    }
                }
                if (header_len > 0 && !cl_checked && !was_head) {
                    cl_checked = true;
                    /* Skip past the status line to the headers. */
                    const char *hp = bytes;
                    size_t      hl = header_len;
                    for (size_t i = 0; i + 1 < hl; i++) {
                        if (hp[i] == '\r' && hp[i + 1] == '\n') {
                            hp = bytes + i + 2;
                            hl = header_len - (i + 2);
                            break;
                        }
                    }
                    size_t      vlen = 0;
                    const char *cl   = find_header(hp, hl,
                                                   "Content-Length",
                                                   &vlen);
                    if (cl) {
                        uint64_t declared
                            = n00b_parse_byte_count_span(cl, vlen);
                        if (declared > max_body_size) {
                            n00b_acme_tls_close(conn);
                            return n00b_result_err(
                                n00b_http_h1_response_t *,
                                N00B_HTTP_ERR_RESPONSE_TOO_LARGE);
                        }
                    }
                }
            }

            /* (b) Streaming guard: body bytes accumulated so far.
             *     Once the header block has landed we know how much
             *     of the buffer is body, so any further growth past
             *     the cap aborts immediately. */
            if (max_body_size > 0 && header_len > 0 && !was_head) {
                uint64_t body_have = (uint64_t)raw->byte_len
                                     - (uint64_t)header_len;
                if (body_have > max_body_size) {
                    n00b_acme_tls_close(conn);
                    return n00b_result_err(
                        n00b_http_h1_response_t *,
                        N00B_HTTP_ERR_RESPONSE_TOO_LARGE);
                }
            }

            int complete = h1_response_complete(raw->data,
                                                 (size_t)raw->byte_len,
                                                 was_head);
            if (complete == 1) boundary_seen = true;
            else if (complete == -1) read_to_eof = true;
        }
        if (read_to_eof && !peer_closed) {
            /* No way to tell when the response ends; keep reading
             * until the peer FINs.  This forces close-after-this on
             * the connection regardless of keep-alive intent. */
            keep_alive_intent = false;
            continue;
        }
    }

    auto pres = n00b_http_h1_response_parse(raw, .allocator = a);
    if (n00b_result_is_err(pres)) {
        n00b_acme_tls_close(conn);
        return pres;
    }
    n00b_http_h1_response_t *resp = n00b_result_get(pres);

    /* Pool the connection if both sides agreed on keep-alive AND
     * the peer didn't FIN AND the message boundary was clean.  Use
     * the same bucket_origin we acquired with so reuse honors the
     * mTLS-identity scoping. */
    if (pool && keep_alive_intent && resp->keep_alive
        && boundary_seen && !peer_closed) {
        n00b_http_connection_pool_release(
            pool, bucket_origin, N00B_HTTP_CONNECTION_POOL_TRANSPORT_H1,
            conn,
            (n00b_http_connection_pool_close_fn)n00b_acme_tls_close);
    } else {
        n00b_acme_tls_close(conn);
    }
    return n00b_result_ok(n00b_http_h1_response_t *, resp);
}
