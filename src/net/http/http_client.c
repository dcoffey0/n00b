/*
 * http_client.c — Public HTTP client dispatcher.
 *
 * Phase 6 chunk 5 (sub-chunk 5a — synchronous form).  Wires the
 * URL parser (chunk 1), h1 transport (chunk 2), h3 transport
 * (chunk 3), and the future connection pool (chunk 4) into a
 * single public entry point.
 *
 * Layout:
 *   §1   Per-runtime state (loss cache only at this revision)
 *   §2   Internal response struct + accessors
 *   §3   Race + fallback dispatcher
 *   §4   Synchronous public entry point
 */

#define N00B_USE_INTERNAL_API
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  define N00B_HTTP_PLAIN_SOCK_T   SOCKET
#  define N00B_HTTP_PLAIN_BAD_SOCK INVALID_SOCKET
#  define N00B_HTTP_PLAIN_CLOSE(s) closesocket((SOCKET)(s))
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <sys/select.h>
#  include <unistd.h>
#  define N00B_HTTP_PLAIN_SOCK_T   int
#  define N00B_HTTP_PLAIN_BAD_SOCK (-1)
#  define N00B_HTTP_PLAIN_CLOSE(s) close(s)
#endif

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/buffer.h"
#include "core/string.h"
#include "core/time.h"
#include "core/thread.h"
#include "adt/result.h"
#include "conduit/conduit.h"
#include "conduit/topic.h"
#include "conduit/service.h"
#include "conduit/socket.h"
#include "conduit/fd_managed.h"
#include "core/runtime.h"
#include "text/strings/format.h"
#include "net/dns.h"
#include "net/quic/quic_types.h"
#include "net/quic/h3.h"
#include "net/http/http_client.h"
#include "net/http/http_auth.h"
#include "internal/net/http/http_url.h"
#include "internal/net/http/http_h1.h"
#include "internal/net/http/http_h3.h"
#include "internal/net/http/http_pool.h"
#include "internal/net/http/http_cookies.h"
#include "internal/net/http/http_compression.h"
#include "internal/net/http/http_client.h"
#include "internal/net/http/http_proxy.h"
#include "util/ascii_ci.h"
#include "text/unicode/idna.h"

/* ===========================================================================
 * §1   Per-runtime state
 *
 * Loss cache: per-origin sticky note that says "h3 didn't work here
 * recently, skip it for `LOSS_CACHE_TTL_MS` and go straight to h1."
 * Stored as a small open-addressed table — typical operator workload
 * has < 100 distinct origins.
 *
 * The cache is process-global state, but we store it via a static
 * pointer that's allocated lazily from the runtime's conduit pool.
 * That keeps the user's "no globals; per-runtime only" rule honest:
 * the bookkeeping is owned by the runtime's allocator, not by libc
 * static memory.
 * =========================================================================== */

#define LOSS_CACHE_TTL_MS  (5 * 60 * 1000ULL)   /* 5 minutes */
#define LOSS_CACHE_SLOTS   64

typedef struct {
    n00b_string_t *origin;        /* nullptr = empty slot */
    uint64_t       expires_at_ms;
} loss_slot_t;

typedef struct {
    loss_slot_t slots[LOSS_CACHE_SLOTS];
} loss_cache_t;

static _Atomic(loss_cache_t *) g_loss_cache = nullptr;

static loss_cache_t *
loss_cache_get(void)
{
    loss_cache_t *cur = atomic_load_explicit(&g_loss_cache,
                                              memory_order_acquire);
    if (cur) return cur;
    n00b_allocator_t *cp =
        (n00b_allocator_t *)&n00b_get_runtime()->conduit_pool;
    loss_cache_t *fresh = n00b_alloc_with_opts(
        loss_cache_t,
        &(n00b_alloc_opts_t){.allocator = cp});
    loss_cache_t *expected = nullptr;
    if (!atomic_compare_exchange_strong_explicit(
            &g_loss_cache, &expected, fresh,
            memory_order_acq_rel, memory_order_acquire)) {
        return expected;
    }
    return fresh;
}

static uint32_t
loss_hash(n00b_string_t *origin)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < origin->u8_bytes; i++) {
        h ^= (unsigned char)origin->data[i];
        h *= 16777619u;
    }
    return h;
}

static bool
loss_cache_h3_blocked(n00b_string_t *origin)
{
    loss_cache_t *cache = atomic_load_explicit(&g_loss_cache,
                                                memory_order_acquire);
    if (!cache) return false;
    uint64_t now = n00b_ns_timestamp() / 1000000ULL;
    uint32_t idx = loss_hash(origin) & (LOSS_CACHE_SLOTS - 1);
    for (uint32_t probe = 0; probe < LOSS_CACHE_SLOTS; probe++) {
        loss_slot_t *s = &cache->slots[(idx + probe) & (LOSS_CACHE_SLOTS - 1)];
        if (!s->origin) return false;
        if (s->origin->u8_bytes == origin->u8_bytes
            && memcmp(s->origin->data, origin->data,
                      origin->u8_bytes) == 0) {
            return now < s->expires_at_ms;
        }
    }
    return false;
}

static void
loss_cache_record(n00b_string_t *origin)
{
    loss_cache_t *cache = loss_cache_get();
    uint64_t now    = n00b_ns_timestamp() / 1000000ULL;
    uint64_t expire = now + LOSS_CACHE_TTL_MS;
    uint32_t idx    = loss_hash(origin) & (LOSS_CACHE_SLOTS - 1);
    /* Find existing entry or the first expired slot. */
    int32_t free_slot = -1;
    for (uint32_t probe = 0; probe < LOSS_CACHE_SLOTS; probe++) {
        uint32_t at = (idx + probe) & (LOSS_CACHE_SLOTS - 1);
        loss_slot_t *s = &cache->slots[at];
        if (!s->origin) {
            if (free_slot < 0) free_slot = (int32_t)at;
            break;
        }
        if (s->origin->u8_bytes == origin->u8_bytes
            && memcmp(s->origin->data, origin->data,
                      origin->u8_bytes) == 0) {
            s->expires_at_ms = expire;
            return;
        }
        if (s->expires_at_ms < now && free_slot < 0) {
            free_slot = (int32_t)at;
        }
    }
    if (free_slot < 0) {
        /* Cache full of live entries — clobber the bucket head.  Some
         * collateral damage on a tiny cache is fine; entries are
         * advisory. */
        free_slot = (int32_t)idx;
    }
    // The cache outlives the request, so it keeps its own copy of the origin.
    // A replaced copy is not freed: lookups read slots without a lock.
    n00b_allocator_t *cp =
        (n00b_allocator_t *)&n00b_get_runtime()->conduit_pool;
    cache->slots[free_slot].origin =
        n00b_string_from_raw(origin->data, (int64_t)origin->u8_bytes,
                             .allocator = cp);
    cache->slots[free_slot].expires_at_ms = expire;
}

void
n00b_http_loss_cache_reset(void)
{
    loss_cache_t *cache = atomic_load_explicit(&g_loss_cache,
                                                memory_order_acquire);
    if (!cache) return;
    memset(cache->slots, 0, sizeof(cache->slots));
}

/* ===========================================================================
 * §2   Response struct + accessors
 * =========================================================================== */

typedef struct {
    n00b_string_t *name;
    n00b_buffer_t *value;
} resp_header_t;

struct n00b_http_response {
    int                    status;
    n00b_buffer_t         *body;
    resp_header_t         *headers;
    size_t                 n_headers;
    n00b_http_transport_t  transport;
    int32_t                error;     /**< Transport-level err; 0 on success. */
    n00b_allocator_t      *allocator;
};

int
n00b_http_response_status(n00b_http_response_t *resp)
{
    return resp ? resp->status : 0;
}

n00b_buffer_t *
n00b_http_response_body(n00b_http_response_t *resp)
{
    return resp ? resp->body : nullptr;
}

n00b_http_transport_t
n00b_http_response_transport(n00b_http_response_t *resp)
{
    return resp ? resp->transport : N00B_HTTP_TRANSPORT_UNKNOWN;
}

int32_t
n00b_http_response_error(n00b_http_response_t *resp)
{
    return resp ? resp->error : N00B_HTTP_ERR_NULL_ARG;
}

static void
http_free_string(n00b_string_t *s)
{
    if (s == nullptr) {
        return;
    }
    n00b_free(s->data);
    n00b_free(s);
}

static void
http_free_buffer(n00b_buffer_t *b)
{
    if (b == nullptr) {
        return;
    }
    n00b_buffer_free(b);
    n00b_free(b);
}

void
n00b_http_response_free(n00b_http_response_t *resp)
{
    if (resp == nullptr) {
        return;
    }

    http_free_buffer(resp->body);
    for (size_t i = 0; i < resp->n_headers; i++) {
        http_free_string(resp->headers[i].name);
        http_free_buffer(resp->headers[i].value);
    }
    n00b_free(resp->headers);
    n00b_free(resp);
}

// Per-call scratch for everything a request builds on the way to its
// response: the parsed URL, header bags, request bytes, the raw read, and
// parse and decompression temporaries. It never moves, because the IO thread
// reads request bytes out of it, and the GC scans it, because its objects can
// point at caller-owned strings. Destroying it frees all of that at once.
static n00b_allocator_t *
http_scratch_open(n00b_pool_t *pool)
{
    return n00b_pool_init(pool,
                          .hidden         = false,
                          .inline_headers = true,
                          .use_epochs     = false,
                          .name           = "http_request_scratch");
}

static n00b_string_t *http_worker_copy_string(n00b_string_t *s,
                                              n00b_allocator_t *a);
static n00b_buffer_t *http_worker_copy_buffer(n00b_buffer_t *b,
                                              n00b_allocator_t *a);

// Copy a response built in scratch into `a`, the runtime default when null,
// so it outlives the scratch.
static n00b_http_response_t *
http_response_export(n00b_http_response_t *src, n00b_allocator_t *a)
{
    n00b_http_response_t *r = n00b_alloc_with_opts(
        n00b_http_response_t,
        &(n00b_alloc_opts_t){.allocator = a});
    r->status    = src->status;
    r->transport = src->transport;
    r->error     = src->error;
    r->allocator = a;
    r->body      = http_worker_copy_buffer(src->body, a);
    r->n_headers = src->n_headers;
    r->headers   = src->n_headers > 0
                     ? n00b_alloc_array_with_opts(resp_header_t,
                                                  src->n_headers,
                                                  &(n00b_alloc_opts_t){.allocator = a})
                     : nullptr;
    for (size_t i = 0; i < src->n_headers; i++) {
        r->headers[i].name  = http_worker_copy_string(src->headers[i].name, a);
        r->headers[i].value = http_worker_copy_buffer(src->headers[i].value, a);
    }
    return r;
}

// Export a scratch-built result into `a`, then destroy the scratch.
static n00b_result_t(n00b_http_response_t *)
http_scratch_close(n00b_result_t(n00b_http_response_t *) rr,
                   n00b_allocator_t                     *scratch,
                   n00b_allocator_t                     *a)
{
    if (n00b_result_is_ok(rr)) {
        rr = n00b_result_ok(n00b_http_response_t *,
                            http_response_export(n00b_result_get(rr), a));
    }
    n00b_allocator_destroy(scratch);
    return rr;
}

static bool
http_response_topic_is_owned_user_event(n00b_conduit_topic_base_t *base,
                                        uint64_t                  *key_out)
{
    if (base == nullptr || base->conduit == nullptr) {
        return false;
    }
    if (!n00b_variant_is_type(base->uri, uint64_t)) {
        return false;
    }

    uint64_t key = n00b_variant_get(base->uri, uint64_t);
    if (!N00B_CONDUIT_URI_IS_USER_EVENT(key)) {
        return false;
    }

    if (key_out != nullptr) {
        *key_out = key;
    }
    return true;
}

static void
http_response_topic_release(n00b_conduit_topic_base_t *base)
{
    uint64_t key = 0;
    if (!http_response_topic_is_owned_user_event(base, &key)) {
        return;
    }

    for (int i = 0; i < 100; i++) {
        n00b_conduit_publisher_t *pub = n00b_atomic_load(&base->publisher);
        n00b_conduit_topic_state_t state = n00b_atomic_load(&base->state);
        if (pub == nullptr && state == N00B_CONDUIT_TOPIC_CLOSED) {
            break;
        }
        base_nanosleep_ns(100000ULL);
    }

    n00b_conduit_topic_state_t state = n00b_atomic_load(&base->state);
    if (n00b_atomic_load(&base->publisher) != nullptr
        || state != N00B_CONDUIT_TOPIC_CLOSED) {
        return;
    }

    n00b_conduit_t *c = base->conduit;
    (void)_n00b_dict_untyped_remove(&c->int_topics, (void *)(uintptr_t)key);

    base->on_first_subscribe     = nullptr;
    base->on_first_subscribe_ctx = nullptr;
    n00b_atomic_store(&base->sub_list_head, (void *)nullptr);

    n00b_conduit_topic_t(n00b_http_response_t *) *topic =
        (n00b_conduit_topic_t(n00b_http_response_t *) *)base;
    if (topic->subscriptions.data != nullptr) {
        size_t n = n00b_list_len(topic->subscriptions);
        for (size_t i = 0; i < n; i++) {
            n00b_free(n00b_list_get(topic->subscriptions, i));
        }
        n00b_list_free(topic->subscriptions);
    }
    n00b_free(topic);
}

void
n00b_http_response_message_free(
    n00b_conduit_message_t(n00b_http_response_t *) *msg)
{
    if (msg == nullptr) {
        return;
    }

    n00b_conduit_topic_base_t *topic = msg->header.topic;
    n00b_free(msg);
    http_response_topic_release(topic);
}

size_t
n00b_http_response_n_headers(n00b_http_response_t *resp)
{
    return resp ? resp->n_headers : 0;
}

bool
n00b_http_response_header_at(n00b_http_response_t *resp,
                              size_t                idx,
                              n00b_string_t       **name_out,
                              n00b_buffer_t       **value_out)
{
    if (!resp || idx >= resp->n_headers) return false;
    if (name_out)  *name_out  = resp->headers[idx].name;
    if (value_out) *value_out = resp->headers[idx].value;
    return true;
}

n00b_buffer_t *
n00b_http_response_header(n00b_http_response_t *resp, n00b_string_t *name)
{
    if (!resp || !name) return nullptr;
    for (size_t i = 0; i < resp->n_headers; i++) {
        n00b_string_t *n = resp->headers[i].name;
        if (n->u8_bytes == name->u8_bytes
            && n00b_ascii_ci_eq_n(n->data, name->data, n->u8_bytes)) {
            return resp->headers[i].value;
        }
    }
    return nullptr;
}

const char *
n00b_http_response_header_cstr(n00b_http_response_t *resp, const char *name)
{
    if (!resp || !name) return nullptr;
    size_t nlen = strlen(name);
    for (size_t i = 0; i < resp->n_headers; i++) {
        n00b_string_t *hn = resp->headers[i].name;
        if (hn->u8_bytes != nlen) continue;
        if (!n00b_ascii_ci_eq_n(hn->data, name, nlen)) continue;
        n00b_buffer_t *val = resp->headers[i].value;
        if (!val) return nullptr;
        size_t vlen = (size_t)val->byte_len;
        char  *cstr = n00b_alloc_array_with_opts(
            char,
            vlen + 1,
            &(n00b_alloc_opts_t){.allocator = resp->allocator});
        memcpy(cstr, val->data, vlen);
        cstr[vlen] = '\0';
        return cstr;
    }
    return nullptr;
}

/* ===========================================================================
 * §3   Response building
 *
 * Adapts h1 + h3 internal response shapes to the public
 * n00b_http_response_t.
 * =========================================================================== */

static n00b_http_response_t *
build_from_h1(n00b_http_h1_response_t *h1,
              n00b_allocator_t        *a)
{
    n00b_http_response_t *r = n00b_alloc_with_opts(
        n00b_http_response_t,
        &(n00b_alloc_opts_t){.allocator = a});
    r->status    = h1->status;
    r->body      = h1->body;
    r->transport = N00B_HTTP_TRANSPORT_H1;
    r->error     = 0;
    r->allocator = a;

    size_t n = n00b_http_h1_headers_len(h1->headers);
    r->headers = (n > 0)
        ? n00b_alloc_array_with_opts(resp_header_t, n, &(n00b_alloc_opts_t){.allocator = a})
        : nullptr;
    r->n_headers = n;
    for (size_t i = 0; i < n; i++) {
        const char *name;
        const char *value;
        if (!n00b_http_h1_headers_at(h1->headers, i, &name, &value)) {
            continue;
        }
        r->headers[i].name  = n00b_string_from_cstr((char *)name,
                                                     .allocator = a);
        r->headers[i].value = n00b_buffer_from_bytes((char *)value,
                                                      (int64_t)strlen(value),
                                                      .allocator = a);
    }
    return r;
}

static n00b_http_response_t *
build_from_h3(n00b_h3_response_t *h3,
              n00b_allocator_t   *a)
{
    n00b_http_response_t *r = n00b_alloc_with_opts(
        n00b_http_response_t,
        &(n00b_alloc_opts_t){.allocator = a});
    r->status    = (int)h3->status;
    r->body      = h3->body;
    r->transport = N00B_HTTP_TRANSPORT_H3;
    r->error     = 0;
    r->allocator = a;
    r->n_headers = h3->n_headers;
    r->headers   = (h3->n_headers > 0)
        ? n00b_alloc_array_with_opts(resp_header_t,
                                     h3->n_headers,
                                     &(n00b_alloc_opts_t){.allocator = a})
        : nullptr;
    for (size_t i = 0; i < h3->n_headers; i++) {
        r->headers[i].name = n00b_string_from_raw(
            (const char *)h3->headers[i].name,
            (int64_t)h3->headers[i].name_len, .allocator = a);
        r->headers[i].value = n00b_buffer_from_bytes(
            (char *)h3->headers[i].value,
            (int64_t)h3->headers[i].value_len, .allocator = a);
    }
    return r;
}

/* Forward declarations used by the plain-HTTP path below.  These
 * are defined further down for the https / dispatcher path. */
static const char *method_cstr(n00b_string_t *method);
static const char *content_type_cstr(n00b_string_t *ct);

/* ===========================================================================
 * § 3.5  Plain HTTP (one-shot, no TLS, no pool)
 *
 * Used only when the caller passes `allow_plain_http = true` and the
 * URL scheme is `http://`.  Intended for local-development loopback
 * callers (same-host CLIs, local backing stores) that legitimately
 * need a plain TCP HTTP/1.1 path because TLS is not configured.
 *
 * Each request opens a fresh socket and closes it after the response
 * is fully received.  No connection pool, no redirect handling, no
 * cookie jar, no TLS / mTLS / H3 — those are all https-only
 * concerns.  Request building and response parsing reuse the
 * existing `n00b_http_h1_request_build` / `n00b_http_h1_response_parse`
 * helpers so the HTTP/1.1 wire format stays in one place.
 * =========================================================================== */

#ifndef _WIN32
static int plain_tcp_wait(N00B_HTTP_PLAIN_SOCK_T fd,
                          short                  events,
                          uint64_t               deadline_ns);

// A connect that a signal interrupts carries on in the kernel, and POSIX
// does not allow calling connect() again on the socket to wait for it. Wait
// for the socket to turn writable instead, then read the attempt's outcome.
// Returns 0 once connected, -1 with errno set when the attempt failed, or
// N00B_QUIC_ERR_TIMEOUT when the deadline passes first.
static int
plain_tcp_connect_finish(N00B_HTTP_PLAIN_SOCK_T fd, uint64_t deadline_ns)
{
    int rc = plain_tcp_wait(fd, POLLOUT, deadline_ns);
    if (rc == N00B_QUIC_ERR_TIMEOUT) {
        return rc;
    }
    if (rc != 0) {
        return -1;
    }
    int       err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
        return -1;
    }
    if (err != 0) {
        errno = err;
        return -1;
    }
    return 0;
}
#endif

static int
plain_tcp_connect(const char *host,
                  uint16_t    port,
                  int32_t     timeout_ms,
                  uint64_t    deadline_ns,
                  N00B_HTTP_PLAIN_SOCK_T *out_fd)
{
    *out_fd = N00B_HTTP_PLAIN_BAD_SOCK;

#ifdef _WIN32
    static bool winsock_started = false;
    if (!winsock_started) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            return N00B_HTTP_ERR_INVALID_URL;
        }
        winsock_started = true;
    }
#endif

    N00B_HTTP_PLAIN_SOCK_T fd = N00B_HTTP_PLAIN_BAD_SOCK;

#ifdef _WIN32
    /* Windows is not an off-libc target, so getaddrinfo is safe here and
     * n00b's UDP resolver is a stub on this platform. */
    char port_str[8];
    int  pn = snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    if (pn <= 0) {
        return N00B_HTTP_ERR_INVALID_URL;
    }

    struct addrinfo  hints = {0};
    struct addrinfo *res   = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || res == nullptr) {
        return N00B_HTTP_ERR_INVALID_URL;
    }

    struct addrinfo *ai;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == N00B_HTTP_PLAIN_BAD_SOCK) {
            continue;
        }
        // eintr: Winsock calls never fail with EINTR.
        if (connect(fd, ai->ai_addr, (socklen_t)ai->ai_addrlen) == 0) {
            break;
        }
        N00B_HTTP_PLAIN_CLOSE(fd);
        fd = N00B_HTTP_PLAIN_BAD_SOCK;
    }
    freeaddrinfo(res);
#else
    /* Resolve without the libc resolver: this can run on an n00b worker
     * thread, where getaddrinfo's internal libc malloc traps under the
     * off-libc runtime. n00b_dns_resolve_addrs uses n00b's own UDP resolver. */
    n00b_resolved_addr_t addrs[8];
    int                  naddr = n00b_dns_resolve_addrs(
        n00b_string_from_cstr(host), port, addrs,
        (int)(sizeof(addrs) / sizeof(addrs[0])));
    if (naddr <= 0) {
        return N00B_HTTP_ERR_INVALID_URL;
    }

    for (int i = 0; i < naddr; i++) {
        fd = socket(addrs[i].ss.ss_family, SOCK_STREAM, 0);
        if (fd == N00B_HTTP_PLAIN_BAD_SOCK) {
            continue;
        }
        // eintr: macOS never restarts a blocking TCP connect, and
        // plain_tcp_connect_finish waits for an interrupted one to finish.
        int rc = connect(fd, (struct sockaddr *)&addrs[i].ss, addrs[i].len);
        if (rc != 0 && errno == EINTR) {
            rc = plain_tcp_connect_finish(fd, deadline_ns);
            if (rc == N00B_QUIC_ERR_TIMEOUT) {
                N00B_HTTP_PLAIN_CLOSE(fd);
                return rc;
            }
        }
        if (rc == 0) {
            break;
        }
        N00B_HTTP_PLAIN_CLOSE(fd);
        fd = N00B_HTTP_PLAIN_BAD_SOCK;
    }
#endif

    if (fd == N00B_HTTP_PLAIN_BAD_SOCK) {
        return N00B_HTTP_ERR_INVALID_URL;
    }

#ifndef _WIN32
    /* Apply a recv timeout if the caller asked for one.  Sends use
     * the same timeout via SO_SNDTIMEO. */
    if (timeout_ms > 0) {
        struct timeval tv;
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
#else
    if (timeout_ms > 0) {
        DWORD ms = (DWORD)timeout_ms;
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                         (const char *)&ms, sizeof(ms));
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                         (const char *)&ms, sizeof(ms));
    }
#endif

    *out_fd = fd;
    return 0;
}

#ifndef _WIN32
// Wait until `fd` is ready for `events` or `deadline_ns` passes (zero means
// no deadline). The wait is a poll, which a signal always interrupts rather
// than restarts, so each retry waits only for what is left. Sends and
// receives then run with MSG_DONTWAIT and come back here when the socket is
// not ready, because a blocking call cannot be bounded the same way: under
// SA_RESTART, macOS restarts an interrupted call inside the kernel with the
// full socket timeout again, never returning EINTR.
static int
plain_tcp_wait(N00B_HTTP_PLAIN_SOCK_T fd, short events, uint64_t deadline_ns)
{
    for (;;) {
        int wait_ms = -1;
        if (deadline_ns != 0) {
            uint64_t now = n00b_ns_timestamp();
            if (now >= deadline_ns) {
                return N00B_QUIC_ERR_TIMEOUT;
            }
            wait_ms = (int)((deadline_ns - now + N00B_NS_PER_MS - 1)
                            / N00B_NS_PER_MS);
        }
        struct pollfd pfd = {.fd = fd, .events = events};
        // eintr: this loop retries EINTR for what is left of the deadline.
        int           rc  = poll(&pfd, 1, wait_ms);
        if (rc > 0) {
            return 0;
        }
        if (rc == 0) {
            return N00B_QUIC_ERR_TIMEOUT;
        }
        if (errno != EINTR) {
            return N00B_HTTP_ERR_BAD_RESPONSE;
        }
    }
}
#endif

static int
plain_tcp_send_all(N00B_HTTP_PLAIN_SOCK_T fd,
                   const char            *bytes,
                   size_t                 len,
                   uint64_t               deadline_ns)
{
    size_t off = 0;
    while (off < len) {
#ifdef _WIN32
        int chunk = (len - off) > INT_MAX ? INT_MAX : (int)(len - off);
        // eintr: Winsock calls never fail with EINTR.
        int rc = send(fd, bytes + off, chunk, 0);
#else
        int ready = plain_tcp_wait(fd, POLLOUT, deadline_ns);
        if (ready != 0) {
            return ready;
        }
        // eintr: MSG_DONTWAIT never sleeps, and the loop retries EINTR.
        ssize_t rc = send(fd, bytes + off, len - off, MSG_DONTWAIT);
#endif
#ifndef _WIN32
        // A signal that lands mid-send is not a transport failure.
        //
        // n00b's stop-the-world suspend signal (N00B_STW_SUSPEND_SIG, RT 40)
        // is installed with SA_RESTART, which would normally hide this. It
        // does not here: plain_tcp_connect arms SO_SNDTIMEO/SO_RCVTIMEO
        // whenever timeout_ms > 0, which it is by default (30 s), and a
        // socket with a send/receive timeout set is explicitly NOT restarted
        // by SA_RESTART -- POSIX requires EINTR in that case. So every
        // request made by this client is interruptible, and a caller that
        // makes many of them eats one eventually.
        //
        // Measured in n00b#473: a run of HTTP requests against a loopback
        // server failed with BAD_RESPONSE and errno EINTR after a varying
        // number of successes (347, 135, 328, 90 across runs), and the very
        // next request to the same still-live server succeeded.
        //
        // rc < 0 means nothing was sent for this call, so `off` is unchanged
        // and resuming the loop re-sends from the same offset.
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
#else
        (void)deadline_ns;
#endif
        if (rc <= 0) {
            return N00B_HTTP_ERR_BAD_RESPONSE;
        }
        off += (size_t)rc;
    }
    return 0;
}

static int
plain_tcp_recv_all(N00B_HTTP_PLAIN_SOCK_T  fd,
                   uint64_t                max_body_size,
                   uint64_t                deadline_ns,
                   n00b_allocator_t       *a,
                   n00b_buffer_t         **out_bytes)
{
    n00b_buffer_t *buf = n00b_buffer_empty(.allocator = a);
    char           chunk[8192];

    while (true) {
#ifdef _WIN32
        // eintr: Winsock calls never fail with EINTR.
        int rc = recv(fd, chunk, (int)sizeof(chunk), 0);
#else
        int ready = plain_tcp_wait(fd, POLLIN, deadline_ns);
        if (ready != 0) {
            return ready;
        }
        // eintr: MSG_DONTWAIT never sleeps, and the loop retries EINTR.
        ssize_t rc = recv(fd, chunk, sizeof(chunk), MSG_DONTWAIT);
#endif
        if (rc == 0) {
            break;
        }
#ifndef _WIN32
        // Same as the send loop, and the same reason it is reachable despite
        // SA_RESTART: SO_RCVTIMEO is set on this socket. rc < 0 consumed no
        // bytes, so the accumulated buffer is still correct and the read
        // resumes. Note this is checked after the rc == 0 test above, since
        // zero means a clean EOF and must still end the loop.
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
#else
        (void)deadline_ns;
#endif
        if (rc < 0) {
            return N00B_HTTP_ERR_BAD_RESPONSE;
        }
        if (max_body_size > 0
            && (uint64_t)buf->byte_len + (uint64_t)rc > max_body_size) {
            return N00B_HTTP_ERR_RESPONSE_TOO_LARGE;
        }
        n00b_buffer_append_bytes(buf, chunk, (size_t)rc);
    }

    *out_bytes = buf;
    return 0;
}

static n00b_result_t(n00b_http_response_t *)
plain_http_request_sync(n00b_string_t           *url,
                        n00b_string_t           *method,
                        n00b_buffer_t           *body,
                        n00b_string_t           *content_type,
                        n00b_http_h1_headers_t  *extra,
                        n00b_string_t           *body_encoding,
                        bool                     auto_decompress,
                        int32_t                  timeout_ms,
                        uint64_t                 max_body_size,
                        n00b_allocator_t        *a)
{
    /* Compress the request body if the caller asked (mirrors the
     * https path's pre-loop compression). */
    if (body_encoding && body_encoding->u8_bytes > 0 && body) {
        auto cr = n00b_http_compress(body, body_encoding->data,
                                       .allocator = a);
        if (n00b_result_is_err(cr)) {
            return n00b_result_err(n00b_http_response_t *,
                                    n00b_result_get_err(cr));
        }
        body = n00b_result_get(cr);
    }

    auto ur = n00b_http_url_parse(url,
                                   .allocator = a,
                                   .allow_plain_http = true);
    if (n00b_result_is_err(ur)) {
        return n00b_result_err(n00b_http_response_t *,
                                n00b_result_get_err(ur));
    }
    n00b_http_url_t *u = n00b_result_get(ur);

    /* Build the H1 request bytes.  The plain path always sends
     * Connection: close — no idle connection is pooled. */
    n00b_http_h1_headers_t *headers = extra;
    if (auto_decompress) {
        if (!headers) {
            headers = n00b_http_h1_headers_new(.allocator = a);
        }
        n00b_string_t *ae = n00b_http_accept_encoding_header(.allocator = a);
        if (ae) {
            n00b_http_h1_headers_set(headers, "Accept-Encoding", ae->data);
        }
    }
    if (extra && extra != headers) {
        /* Caller's extras override our auto headers. */
        size_t n = n00b_http_h1_headers_len(extra);
        for (size_t i = 0; i < n; i++) {
            const char *name;
            const char *value;
            if (n00b_http_h1_headers_at(extra, i, &name, &value)) {
                n00b_http_h1_headers_set(headers, name, value);
            }
        }
    }

    n00b_buffer_t *request = n00b_http_h1_request_build(
        u,
        .method       = method_cstr(method),
        .body         = body,
        .content_type = content_type ? content_type->data : nullptr,
        .extra        = headers,
        .keep_alive   = false,
        .allocator    = a);

    /* One-shot socket: connect, send, recv, close. The send and recv share
     * one deadline however many times a signal interrupts them. */
    uint64_t deadline_ns = timeout_ms > 0
                             ? n00b_ns_timestamp()
                                   + (uint64_t)timeout_ms * N00B_NS_PER_MS
                             : 0;
    N00B_HTTP_PLAIN_SOCK_T fd = N00B_HTTP_PLAIN_BAD_SOCK;
    int rc = plain_tcp_connect(u->host->data,
                                u->port,
                                timeout_ms,
                                deadline_ns,
                                &fd);
    if (rc != 0) {
        return n00b_result_err(n00b_http_response_t *, rc);
    }

    rc = plain_tcp_send_all(fd, request->data, request->byte_len,
                            deadline_ns);
    if (rc != 0) {
        N00B_HTTP_PLAIN_CLOSE(fd);
        return n00b_result_err(n00b_http_response_t *, rc);
    }

    n00b_buffer_t *raw = nullptr;
    rc = plain_tcp_recv_all(fd, max_body_size, deadline_ns, a, &raw);
    N00B_HTTP_PLAIN_CLOSE(fd);
    if (rc != 0) {
        return n00b_result_err(n00b_http_response_t *, rc);
    }

    auto pr = n00b_http_h1_response_parse(raw, .allocator = a);
    if (n00b_result_is_err(pr)) {
        return n00b_result_err(n00b_http_response_t *,
                                n00b_result_get_err(pr));
    }
    n00b_http_h1_response_t *h1 = n00b_result_get(pr);

    /* Optional response-body decompression (mirrors the https path). */
    if (auto_decompress) {
        const char *enc = n00b_http_h1_headers_get_cstr(h1->headers,
                                                         "Content-Encoding");
        if (enc && enc[0] && h1->body && h1->body->byte_len > 0) {
            auto dr = n00b_http_decompress(h1->body, enc, .allocator = a);
            if (n00b_result_is_ok(dr)) {
                h1->body = n00b_result_get(dr);
            }
        }
    }

    return n00b_result_ok(n00b_http_response_t *, build_from_h1(h1, a));
}

/* ===========================================================================
 * § 3.6  Plain HTTP/1.1 over an AF_UNIX socket (conduit transport)
 *
 * Like the plain-TCP path above, but the transport is a unix-domain
 * socket driven through the conduit fd-managed layer (no raw sockets):
 * connect via `n00b_conduit_conn_unix`, write the request with
 * `n00b_fd_owner_write`, and drain the response to EOF with
 * `n00b_fd_owner_read_all` (the peer answers `Connection: close`).
 * Request building + response parsing reuse the shared H1 helpers.
 * =========================================================================== */

static n00b_result_t(n00b_http_response_t *)
unix_http_request_sync(n00b_string_t          *socket_path,
                       n00b_string_t          *path,
                       n00b_string_t          *method,
                       n00b_buffer_t          *body,
                       n00b_string_t          *content_type,
                       n00b_http_h1_headers_t *extra,
                       bool                    auto_decompress,
                       int32_t                 timeout_ms,
                       uint64_t                max_body_size,
                       n00b_allocator_t       *a)
{
    // Synthesize a loopback URL so the request line + Host header reuse
    // the shared H1 builder; the real transport is the unix socket.
    n00b_string_t *url_str = n00b_cformat("http://localhost«#»", path);
    auto           ur      = n00b_http_url_parse(url_str,
                                                 .allocator        = a,
                                                 .allow_plain_http = true);
    if (n00b_result_is_err(ur)) {
        return n00b_result_err(n00b_http_response_t *, n00b_result_get_err(ur));
    }
    n00b_http_url_t *u = n00b_result_get(ur);

    n00b_http_h1_headers_t *headers = extra;
    if (auto_decompress) {
        if (!headers) {
            headers = n00b_http_h1_headers_new(.allocator = a);
        }
        n00b_string_t *ae = n00b_http_accept_encoding_header(.allocator = a);
        if (ae) {
            n00b_http_h1_headers_set(headers, "Accept-Encoding", ae->data);
        }
    }
    if (extra && extra != headers) {
        size_t n = n00b_http_h1_headers_len(extra);
        for (size_t i = 0; i < n; i++) {
            const char *name;
            const char *value;
            if (n00b_http_h1_headers_at(extra, i, &name, &value)) {
                n00b_http_h1_headers_set(headers, name, value);
            }
        }
    }

    n00b_buffer_t *request = n00b_http_h1_request_build(
        u,
        .method       = method_cstr(method),
        .body         = body,
        .content_type = content_type ? content_type->data : nullptr,
        .extra        = headers,
        .keep_alive   = false,
        .allocator    = a);

    n00b_runtime_t *rt = n00b_get_runtime();
    if (rt == nullptr || rt->default_conduit == nullptr
        || rt->default_service == nullptr) {
        return n00b_result_err(n00b_http_response_t *, N00B_HTTP_ERR_BAD_RESPONSE);
    }
    n00b_option_t(n00b_conduit_svc_thread_t *) st_opt =
        n00b_conduit_service_default_io(rt->default_service);
    if (!n00b_option_is_set(st_opt)) {
        return n00b_result_err(n00b_http_response_t *, N00B_HTTP_ERR_BAD_RESPONSE);
    }
    n00b_option_t(n00b_conduit_io_backend_t *) io_opt =
        n00b_conduit_svc_thread_io(n00b_option_get(st_opt));
    if (!n00b_option_is_set(io_opt)) {
        return n00b_result_err(n00b_http_response_t *, N00B_HTTP_ERR_BAD_RESPONSE);
    }

    auto conn_r = n00b_conduit_conn_unix(rt->default_conduit,
                                         n00b_option_get(io_opt),
                                         socket_path);
    if (n00b_result_is_err(conn_r)) {
        return n00b_result_err(n00b_http_response_t *, N00B_HTTP_ERR_BAD_RESPONSE);
    }
    n00b_conduit_conn_t *conn = n00b_result_get(conn_r);

    // AF_UNIX connect completes synchronously to CONNECTED.
    if (n00b_atomic_load(&conn->conn_state) != N00B_CONDUIT_CONN_ST_CONNECTED) {
        n00b_conduit_conn_close(conn);
        return n00b_result_err(n00b_http_response_t *, N00B_HTTP_ERR_BAD_RESPONSE);
    }

    n00b_option_t(n00b_conduit_fd_owner_t *) owner_opt =
        n00b_conduit_conn_fd_owner(conn);
    if (!n00b_option_is_set(owner_opt)) {
        n00b_conduit_conn_close(conn);
        return n00b_result_err(n00b_http_response_t *, N00B_HTTP_ERR_BAD_RESPONSE);
    }
    n00b_conduit_fd_owner_t *owner = n00b_option_get(owner_opt);

    auto wr = n00b_fd_owner_write(owner, request->data, (size_t)request->byte_len);
    if (n00b_result_is_err(wr)) {
        n00b_conduit_conn_close(conn);
        return n00b_result_err(n00b_http_response_t *, N00B_HTTP_ERR_BAD_RESPONSE);
    }

    auto rr = n00b_fd_owner_read_all(owner,
                                     .timeout_ms = timeout_ms,
                                     .allocator = a);
    n00b_conduit_conn_close(conn);
    if (n00b_result_is_err(rr)) {
        return n00b_result_err(n00b_http_response_t *, N00B_HTTP_ERR_BAD_RESPONSE);
    }
    n00b_buffer_t *raw = n00b_result_get(rr);

    if (max_body_size > 0 && (uint64_t)raw->byte_len > max_body_size) {
        return n00b_result_err(n00b_http_response_t *,
                               N00B_HTTP_ERR_RESPONSE_TOO_LARGE);
    }

    auto pr = n00b_http_h1_response_parse(raw, .allocator = a);
    if (n00b_result_is_err(pr)) {
        return n00b_result_err(n00b_http_response_t *, n00b_result_get_err(pr));
    }
    n00b_http_h1_response_t *h1 = n00b_result_get(pr);

    if (auto_decompress) {
        const char *enc = n00b_http_h1_headers_get_cstr(h1->headers,
                                                        "Content-Encoding");
        if (enc && enc[0] && h1->body && h1->body->byte_len > 0) {
            auto dr = n00b_http_decompress(h1->body, enc, .allocator = a);
            if (n00b_result_is_ok(dr)) {
                h1->body = n00b_result_get(dr);
            }
        }
    }

    return n00b_result_ok(n00b_http_response_t *, build_from_h1(h1, a));
}

n00b_result_t(n00b_http_response_t *)
n00b_http_request_unix_sync(n00b_string_t *socket_path, n00b_string_t *path)
    _kargs {
        n00b_string_t          *method          = nullptr;
        n00b_buffer_t          *body            = nullptr;
        n00b_string_t          *content_type    = nullptr;
        n00b_http_h1_headers_t *extra           = nullptr;
        bool                    auto_decompress = true;
        int32_t                 timeout_ms      = 30000;
        uint64_t                max_body_size   = 0;
        n00b_allocator_t       *allocator       = nullptr;
    }
{
    if (socket_path == nullptr || path == nullptr) {
        return n00b_result_err(n00b_http_response_t *, N00B_HTTP_ERR_INVALID_URL);
    }

    n00b_pool_t       scratch_pool = {};
    n00b_allocator_t *scratch      = http_scratch_open(&scratch_pool);
    auto              rr           = unix_http_request_sync(socket_path,
                                                            path,
                                                            method,
                                                            body,
                                                            content_type,
                                                            extra,
                                                            auto_decompress,
                                                            timeout_ms,
                                                            max_body_size,
                                                            scratch);
    return http_scratch_close(rr, scratch, allocator);
}

// ===========================================================================
// Streaming unix HTTP client (conduit stream reader)
//
// Reads the response incrementally off the conduit fd stream and invokes
// `on_line` for each '\n'-delimited body line as it arrives (NDJSON), rather
// than buffering the whole body. The accumulator is compacted after each batch
// of lines so client memory stays bounded regardless of body size. Uses the
// conduit stream-reader API + the same process/await pattern as the server.
// ===========================================================================

#define N00B_HTTP_CLIENT_STREAM_MAX_TICKS 600

static bool
client_stream_push(void *inbox, void *msg)
{
    return n00b_conduit_inbox_push_msg(
        n00b_conduit_fd_stream_payload_t,
        (n00b_conduit_fd_stream_inbox_t *)inbox,
        (n00b_conduit_fd_stream_msg_t *)msg);
}

// `*held` is the reply the previous await returned. It is freed here, so a
// returned payload's bytes stay valid until the next await; the caller frees
// the last one.
static void
client_stream_release(n00b_conduit_fd_stream_msg_t **held)
{
    if (*held != nullptr) {
        n00b_conduit_stream_msg_free(*held);
        *held = nullptr;
    }
}

static void
client_stream_close(n00b_conduit_conn_t            *conn,
                    n00b_conduit_stream_reader_t   *reader,
                    n00b_conduit_fd_stream_inbox_t *inbox,
                    n00b_conduit_fd_stream_msg_t  **held)
{
    client_stream_release(held);
    n00b_conduit_stream_reader_destroy(reader);
    n00b_conduit_inbox_destroy(n00b_conduit_fd_stream_payload_t, inbox);
    n00b_free(inbox);
    n00b_conduit_conn_close(conn);
}

static n00b_conduit_fd_stream_payload_t
client_stream_await(n00b_conduit_stream_reader_t   *reader,
                    n00b_conduit_fd_stream_inbox_t *inbox,
                    n00b_conduit_fd_stream_msg_t  **held,
                    bool                           *ok)
{
    *ok = true;
    client_stream_release(held);
    for (int tick = 0; tick < N00B_HTTP_CLIENT_STREAM_MAX_TICKS; tick++) {
        n00b_conduit_stream_reader_process(reader);
        n00b_conduit_fd_stream_msg_t *msg =
            n00b_conduit_inbox_pop_msg(n00b_conduit_fd_stream_payload_t, inbox);
        if (msg != nullptr) {
            *held = msg;
            return msg->payload;
        }
        if (reader->eof || reader->error) {
            *ok = !reader->error;
            return (n00b_conduit_fd_stream_payload_t){.eof   = true,
                                                      .error = reader->error};
        }
        n00b_condition_lock(&reader->internal_inbox->cv);
        if (!n00b_conduit_inbox_has_msg(n00b_buffer_t *, reader->internal_inbox)
            && !n00b_conduit_inbox_has_sys(reader->internal_inbox)) {
            n00b_condition_wait(&reader->internal_inbox->cv,
                                .timeout_ms  = 100,
                                .auto_unlock = true);
        }
        else {
            n00b_condition_unlock(&reader->internal_inbox->cv);
        }
    }
    *ok = false;
    return (n00b_conduit_fd_stream_payload_t){.eof = true};
}

// Parse "HTTP/1.x NNN ..." → NNN; returns 0 on malformed.
static int
client_parse_status(const char *buf, size_t len)
{
    size_t i = 0;
    while (i < len && buf[i] != ' ') {
        i++; // skip "HTTP/1.x"
    }
    while (i < len && buf[i] == ' ') {
        i++;
    }
    int status = 0;
    int digits = 0;
    while (i < len && buf[i] >= '0' && buf[i] <= '9' && digits < 3) {
        status = status * 10 + (buf[i] - '0');
        i++;
        digits++;
    }
    return digits == 3 ? status : 0;
}

static n00b_result_t(int)
unix_http_request_stream(n00b_string_t          *socket_path,
                         n00b_string_t          *path,
                         n00b_http_line_cb_t     on_line,
                         void                   *ctx,
                         n00b_string_t          *method,
                         n00b_buffer_t          *body,
                         n00b_string_t          *content_type,
                         n00b_http_h1_headers_t *extra,
                         n00b_allocator_t       *a)
{

    n00b_string_t *url_str = n00b_cformat("http://localhost«#»", path);
    auto ur = n00b_http_url_parse(url_str, .allocator = a, .allow_plain_http = true);
    if (n00b_result_is_err(ur)) {
        return n00b_result_err(int, n00b_result_get_err(ur));
    }
    n00b_http_url_t *u       = n00b_result_get(ur);
    n00b_buffer_t   *request = n00b_http_h1_request_build(
        u,
        .method       = (method && method->u8_bytes) ? method->data : "GET",
        .body         = body,
        .content_type = content_type ? content_type->data : nullptr,
        .extra        = extra,
        .keep_alive   = false,
        .allocator    = a);

    n00b_runtime_t *rt = n00b_get_runtime();
    if (rt == nullptr || rt->default_conduit == nullptr
        || rt->default_service == nullptr) {
        return n00b_result_err(int, N00B_HTTP_ERR_BAD_RESPONSE);
    }
    n00b_option_t(n00b_conduit_svc_thread_t *) st_opt =
        n00b_conduit_service_default_io(rt->default_service);
    if (!n00b_option_is_set(st_opt)) {
        return n00b_result_err(int, N00B_HTTP_ERR_BAD_RESPONSE);
    }
    n00b_option_t(n00b_conduit_io_backend_t *) io_opt =
        n00b_conduit_svc_thread_io(n00b_option_get(st_opt));
    if (!n00b_option_is_set(io_opt)) {
        return n00b_result_err(int, N00B_HTTP_ERR_BAD_RESPONSE);
    }

    auto conn_r = n00b_conduit_conn_unix(rt->default_conduit,
                                         n00b_option_get(io_opt),
                                         socket_path);
    if (n00b_result_is_err(conn_r)) {
        return n00b_result_err(int, N00B_HTTP_ERR_BAD_RESPONSE);
    }
    n00b_conduit_conn_t *conn = n00b_result_get(conn_r);
    if (n00b_atomic_load(&conn->conn_state) != N00B_CONDUIT_CONN_ST_CONNECTED) {
        n00b_conduit_conn_close(conn);
        return n00b_result_err(int, N00B_HTTP_ERR_BAD_RESPONSE);
    }
    n00b_option_t(n00b_conduit_fd_owner_t *) owner_opt =
        n00b_conduit_conn_fd_owner(conn);
    if (!n00b_option_is_set(owner_opt)) {
        n00b_conduit_conn_close(conn);
        return n00b_result_err(int, N00B_HTTP_ERR_BAD_RESPONSE);
    }
    n00b_conduit_fd_owner_t *owner = n00b_option_get(owner_opt);
    auto wr = n00b_fd_owner_write(owner, request->data, (size_t)request->byte_len);
    if (n00b_result_is_err(wr)) {
        n00b_conduit_conn_close(conn);
        return n00b_result_err(int, N00B_HTTP_ERR_BAD_RESPONSE);
    }

    auto reader_r = n00b_conduit_stream_reader_new(rt->default_conduit, owner);
    if (n00b_result_is_err(reader_r)) {
        n00b_conduit_conn_close(conn);
        return n00b_result_err(int, N00B_HTTP_ERR_BAD_RESPONSE);
    }
    n00b_conduit_stream_reader_t   *reader = n00b_result_get(reader_r);
    n00b_conduit_fd_stream_inbox_t *inbox  =
        n00b_conduit_fd_stream_inbox_new(rt->default_conduit);
    n00b_conduit_fd_stream_msg_t   *held   = nullptr;

    // Growable accumulator, compacted after each line batch so it only ever
    // holds the unconsumed tail (one read chunk + a partial line).
    size_t cap          = 65536;
    size_t len          = 0;
    char  *buf          = n00b_alloc_array(char, cap);
    bool   headers_done = false;
    int    status       = 0;
    bool   eof          = false;

    while (!eof) {
        n00b_conduit_stream_read(reader, 65536, inbox, client_stream_push);
        bool ok;
        n00b_conduit_fd_stream_payload_t p = client_stream_await(reader,
                                                                inbox,
                                                                &held,
                                                                &ok);
        if (p.len > 0) {
            if (len + p.len > cap) {
                while (len + p.len > cap) {
                    cap *= 2;
                }
                char *next = n00b_alloc_array(char, cap);
                memcpy(next, buf, len);
                buf = next;
            }
            memcpy(buf + len, p.data, p.len);
            len += p.len;
        }
        eof = (!ok || p.eof);

        if (!headers_done) {
            // Find end of header block (CRLFCRLF).
            size_t i = 0;
            size_t hdr_end = (size_t)-1;
            if (len >= 4) {
                for (i = 0; i + 4 <= len; i++) {
                    if (buf[i] == '\r' && buf[i + 1] == '\n'
                        && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                        hdr_end = i + 4;
                        break;
                    }
                }
            }
            if (hdr_end != (size_t)-1) {
                status = client_parse_status(buf, len);
                // Drop the header block; keep body bytes.
                memmove(buf, buf + hdr_end, len - hdr_end);
                len -= hdr_end;
                headers_done = true;
            }
        }

        if (headers_done) {
            // Emit each complete '\n'-terminated line; compact the remainder.
            size_t line_start = 0;
            for (size_t i = 0; i < len; i++) {
                if (buf[i] != '\n') {
                    continue;
                }
                size_t llen = i - line_start;
                if (llen > 0 && buf[line_start + llen - 1] == '\r') {
                    llen--; // strip CR
                }
                if (llen > 0) {
                    n00b_string_t *line = n00b_string_from_raw(
                        buf + line_start,
                        (int64_t)llen);
                    if (!on_line(ctx, line)) {
                        eof = true; // consumer asked to stop
                        break;
                    }
                }
                line_start = i + 1;
            }
            if (line_start > 0) {
                memmove(buf, buf + line_start, len - line_start);
                len -= line_start;
            }
        }
    }

    client_stream_close(conn, reader, inbox, &held);

    if (!headers_done || status <= 0) {
        return n00b_result_err(int, N00B_HTTP_ERR_BAD_RESPONSE);
    }
    return n00b_result_ok(int, status);
}

n00b_result_t(int)
n00b_http_request_unix_stream(n00b_string_t      *socket_path,
                              n00b_string_t      *path,
                              n00b_http_line_cb_t on_line,
                              void               *ctx) _kargs {
    n00b_string_t          *method       = nullptr;
    n00b_buffer_t          *body         = nullptr;
    n00b_string_t          *content_type = nullptr;
    n00b_http_h1_headers_t *extra        = nullptr;
}
{
    if (socket_path == nullptr || path == nullptr || on_line == nullptr) {
        return n00b_result_err(int, N00B_HTTP_ERR_INVALID_URL);
    }

    n00b_pool_t       scratch_pool = {};
    n00b_allocator_t *scratch      = http_scratch_open(&scratch_pool);
    auto              rr           = unix_http_request_stream(socket_path,
                                                              path,
                                                              on_line,
                                                              ctx,
                                                              method,
                                                              body,
                                                              content_type,
                                                              extra,
                                                              scratch);
    n00b_allocator_destroy(scratch);
    return rr;
}

/* ===========================================================================
 * § 3.7  Plain HTTP/1.1 over a loopback TCP socket (conduit transport)
 *
 * Same shape as the AF_UNIX local-API client above, but the transport is a
 * loopback TCP connection (127.0.0.1:port) driven through the conduit
 * fd-managed layer. Some tools cannot dial an AF_UNIX socket, so the local
 * API is served over loopback TCP; these are the client-side counterparts to
 * n00b_http_request_unix_{sync,stream}. n00b_conduit_conn_tcp does a
 * NON-blocking connect (unlike AF_UNIX which lands synchronously), so we wait
 * for CONNECTED via the connection status topic before using the fd owner.
 * =========================================================================== */

static int64_t
h1_tcp_now_ms(void)
{
    return (int64_t)(n00b_ns_timestamp() / N00B_NS_PER_MS);
}

/* Block until `conn` reaches CONNECTED, ERROR/CLOSED, or `deadline_ms`.
 * Mirrors xform_tls.c's tls_wait_connected. Returns true on CONNECTED. */
static bool
h1_tcp_wait_connected(n00b_conduit_t *c, n00b_conduit_conn_t *conn,
                      int64_t deadline_ms)
{
    int state = n00b_atomic_load(&conn->conn_state);
    if (state == N00B_CONDUIT_CONN_ST_CONNECTED) {
        return true;
    }
    if (state == N00B_CONDUIT_CONN_ST_ERROR
        || state == N00B_CONDUIT_CONN_ST_CLOSED) {
        return false;
    }

    n00b_option_t(n00b_conduit_topic_base_t *) status_opt =
        n00b_conduit_conn_status_topic(conn);
    n00b_conduit_sock_status_inbox_t *inbox = nullptr;
    n00b_conduit_sub_handle_t         sub   = N00B_CONDUIT_INVALID_SUB_HANDLE;
    if (n00b_option_is_set(status_opt)) {
        n00b_conduit_topic_base_t *st = n00b_option_get(status_opt);
        inbox = n00b_conduit_sock_status_inbox_new(c);
        sub   = n00b_conduit_sock_status_subscribe(
            st, inbox, .operations = N00B_CONDUIT_OP_ALL);
    }

    bool ok = false;
    while (h1_tcp_now_ms() < deadline_ms) {
        state = n00b_atomic_load(&conn->conn_state);
        if (state == N00B_CONDUIT_CONN_ST_CONNECTED) {
            ok = true;
            break;
        }
        if (state == N00B_CONDUIT_CONN_ST_ERROR
            || state == N00B_CONDUIT_CONN_ST_CLOSED) {
            break;
        }
        if (inbox) {
            n00b_conduit_sock_status_msg_t *m =
                n00b_conduit_sock_status_inbox_pop(inbox);
            if (m != nullptr) {
                n00b_free(m);
            }
            n00b_condition_wait(&inbox->cv, .timeout_ms = 50, .auto_unlock = true);
        }
    }
    if (sub != N00B_CONDUIT_INVALID_SUB_HANDLE) {
        n00b_conduit_sub_cancel(sub);
    }
    if (inbox) {
        n00b_conduit_inbox_destroy(n00b_conduit_sock_status_payload_t, inbox);
        n00b_free(inbox);
    }
    return ok;
}

/* Connect a loopback-TCP conduit connection and return its fd owner, or
 * nullptr on failure (closing the conn). *out_conn receives the conn so the
 * caller can close it after the round-trip. host must be a numeric loopback
 * literal (conn_tcp does not resolve names). */
static n00b_conduit_fd_owner_t *
h1_tcp_connect(n00b_string_t *host, uint16_t port, int32_t timeout_ms,
               n00b_conduit_conn_t **out_conn)
{
    *out_conn          = nullptr;
    n00b_runtime_t *rt = n00b_get_runtime();
    if (rt == nullptr || rt->default_conduit == nullptr
        || rt->default_service == nullptr) {
        return nullptr;
    }
    n00b_option_t(n00b_conduit_svc_thread_t *) st_opt =
        n00b_conduit_service_default_io(rt->default_service);
    if (!n00b_option_is_set(st_opt)) {
        return nullptr;
    }
    n00b_option_t(n00b_conduit_io_backend_t *) io_opt =
        n00b_conduit_svc_thread_io(n00b_option_get(st_opt));
    if (!n00b_option_is_set(io_opt)) {
        return nullptr;
    }

    auto conn_r = n00b_conduit_conn_tcp(rt->default_conduit,
                                        n00b_option_get(io_opt), host, port);
    if (n00b_result_is_err(conn_r)) {
        return nullptr;
    }
    n00b_conduit_conn_t *conn = n00b_result_get(conn_r);

    int64_t deadline_ms = h1_tcp_now_ms() + (timeout_ms > 0 ? timeout_ms : 30000);
    if (!h1_tcp_wait_connected(rt->default_conduit, conn, deadline_ms)) {
        n00b_conduit_conn_close(conn);
        return nullptr;
    }
    n00b_option_t(n00b_conduit_fd_owner_t *) owner_opt =
        n00b_conduit_conn_fd_owner(conn);
    if (!n00b_option_is_set(owner_opt)) {
        n00b_conduit_conn_close(conn);
        return nullptr;
    }
    *out_conn = conn;
    return n00b_option_get(owner_opt);
}

static n00b_result_t(n00b_http_response_t *)
tcp_http_request_sync(n00b_string_t          *host,
                      uint16_t                port,
                      n00b_string_t          *path,
                      n00b_string_t          *method,
                      n00b_buffer_t          *body,
                      n00b_string_t          *content_type,
                      n00b_http_h1_headers_t *extra,
                      bool                    auto_decompress,
                      int32_t                 timeout_ms,
                      uint64_t                max_body_size,
                      n00b_allocator_t       *a)
{
    n00b_string_t *url_str = n00b_cformat("http://«#»:«#»«#»", host,
                                          (int64_t)port, path);
    auto ur = n00b_http_url_parse(url_str, .allocator = a, .allow_plain_http = true);
    if (n00b_result_is_err(ur)) {
        return n00b_result_err(n00b_http_response_t *, n00b_result_get_err(ur));
    }
    n00b_http_url_t *u = n00b_result_get(ur);

    n00b_http_h1_headers_t *headers = extra;
    if (auto_decompress) {
        if (!headers) {
            headers = n00b_http_h1_headers_new(.allocator = a);
        }
        n00b_string_t *ae = n00b_http_accept_encoding_header(.allocator = a);
        if (ae) {
            n00b_http_h1_headers_set(headers, "Accept-Encoding", ae->data);
        }
    }

    n00b_buffer_t *request = n00b_http_h1_request_build(
        u,
        .method       = (method && method->u8_bytes) ? method->data : "GET",
        .body         = body,
        .content_type = content_type ? content_type->data : nullptr,
        .extra        = headers,
        .keep_alive   = false,
        .allocator    = a);

    n00b_conduit_conn_t *conn  = nullptr;
    n00b_conduit_fd_owner_t *owner = h1_tcp_connect(host, port, timeout_ms, &conn);
    if (owner == nullptr) {
        return n00b_result_err(n00b_http_response_t *, N00B_HTTP_ERR_BAD_RESPONSE);
    }

    auto wr = n00b_fd_owner_write(owner, request->data, (size_t)request->byte_len);
    if (n00b_result_is_err(wr)) {
        n00b_conduit_conn_close(conn);
        return n00b_result_err(n00b_http_response_t *, N00B_HTTP_ERR_BAD_RESPONSE);
    }
    auto rr = n00b_fd_owner_read_all(owner, .timeout_ms = timeout_ms, .allocator = a);
    n00b_conduit_conn_close(conn);
    if (n00b_result_is_err(rr)) {
        return n00b_result_err(n00b_http_response_t *, N00B_HTTP_ERR_BAD_RESPONSE);
    }
    n00b_buffer_t *raw = n00b_result_get(rr);
    if (max_body_size > 0 && (uint64_t)raw->byte_len > max_body_size) {
        return n00b_result_err(n00b_http_response_t *,
                               N00B_HTTP_ERR_RESPONSE_TOO_LARGE);
    }

    auto pr = n00b_http_h1_response_parse(raw, .allocator = a);
    if (n00b_result_is_err(pr)) {
        return n00b_result_err(n00b_http_response_t *, n00b_result_get_err(pr));
    }
    n00b_http_h1_response_t *h1 = n00b_result_get(pr);
    if (auto_decompress) {
        const char *enc = n00b_http_h1_headers_get_cstr(h1->headers,
                                                        "Content-Encoding");
        if (enc && enc[0] && h1->body && h1->body->byte_len > 0) {
            auto dr = n00b_http_decompress(h1->body, enc, .allocator = a);
            if (n00b_result_is_ok(dr)) {
                h1->body = n00b_result_get(dr);
            }
        }
    }
    return n00b_result_ok(n00b_http_response_t *, build_from_h1(h1, a));
}

n00b_result_t(n00b_http_response_t *)
n00b_http_request_tcp_sync(n00b_string_t *host, uint16_t port, n00b_string_t *path)
    _kargs {
        n00b_string_t          *method          = nullptr;
        n00b_buffer_t          *body            = nullptr;
        n00b_string_t          *content_type    = nullptr;
        n00b_http_h1_headers_t *extra           = nullptr;
        bool                    auto_decompress = true;
        int32_t                 timeout_ms      = 30000;
        uint64_t                max_body_size   = 0;
        n00b_allocator_t       *allocator       = nullptr;
    }
{
    if (host == nullptr || path == nullptr) {
        return n00b_result_err(n00b_http_response_t *, N00B_HTTP_ERR_INVALID_URL);
    }

    n00b_pool_t       scratch_pool = {};
    n00b_allocator_t *scratch      = http_scratch_open(&scratch_pool);
    auto              rr           = tcp_http_request_sync(host,
                                                           port,
                                                           path,
                                                           method,
                                                           body,
                                                           content_type,
                                                           extra,
                                                           auto_decompress,
                                                           timeout_ms,
                                                           max_body_size,
                                                           scratch);
    return http_scratch_close(rr, scratch, allocator);
}

static n00b_result_t(int)
tcp_http_request_stream(n00b_string_t          *host,
                        uint16_t                port,
                        n00b_string_t          *path,
                        n00b_http_line_cb_t     on_line,
                        void                   *ctx,
                        n00b_string_t          *method,
                        n00b_buffer_t          *body,
                        n00b_string_t          *content_type,
                        n00b_http_h1_headers_t *extra,
                        n00b_allocator_t       *a)
{

    n00b_string_t *url_str = n00b_cformat("http://«#»:«#»«#»", host,
                                          (int64_t)port, path);
    auto ur = n00b_http_url_parse(url_str, .allocator = a, .allow_plain_http = true);
    if (n00b_result_is_err(ur)) {
        return n00b_result_err(int, n00b_result_get_err(ur));
    }
    n00b_http_url_t *u       = n00b_result_get(ur);
    n00b_buffer_t   *request = n00b_http_h1_request_build(
        u,
        .method       = (method && method->u8_bytes) ? method->data : "GET",
        .body         = body,
        .content_type = content_type ? content_type->data : nullptr,
        .extra        = extra,
        .keep_alive   = false,
        .allocator    = a);

    n00b_conduit_conn_t     *conn  = nullptr;
    n00b_conduit_fd_owner_t *owner = h1_tcp_connect(host, port, 30000, &conn);
    if (owner == nullptr) {
        return n00b_result_err(int, N00B_HTTP_ERR_BAD_RESPONSE);
    }
    auto wr = n00b_fd_owner_write(owner, request->data, (size_t)request->byte_len);
    if (n00b_result_is_err(wr)) {
        n00b_conduit_conn_close(conn);
        return n00b_result_err(int, N00B_HTTP_ERR_BAD_RESPONSE);
    }

    auto reader_r = n00b_conduit_stream_reader_new(n00b_get_runtime()->default_conduit,
                                                   owner);
    if (n00b_result_is_err(reader_r)) {
        n00b_conduit_conn_close(conn);
        return n00b_result_err(int, N00B_HTTP_ERR_BAD_RESPONSE);
    }
    n00b_conduit_stream_reader_t   *reader = n00b_result_get(reader_r);
    n00b_conduit_fd_stream_inbox_t *inbox  =
        n00b_conduit_fd_stream_inbox_new(n00b_get_runtime()->default_conduit);
    n00b_conduit_fd_stream_msg_t   *held   = nullptr;

    size_t cap          = 65536;
    size_t len          = 0;
    char  *buf          = n00b_alloc_array(char, cap);
    bool   headers_done = false;
    int    status       = 0;
    bool   eof          = false;

    while (!eof) {
        n00b_conduit_stream_read(reader, 65536, inbox, client_stream_push);
        bool ok;
        n00b_conduit_fd_stream_payload_t p = client_stream_await(reader, inbox, &held, &ok);
        if (p.len > 0) {
            if (len + p.len > cap) {
                while (len + p.len > cap) {
                    cap *= 2;
                }
                char *next = n00b_alloc_array(char, cap);
                memcpy(next, buf, len);
                buf = next;
            }
            memcpy(buf + len, p.data, p.len);
            len += p.len;
        }
        eof = (!ok || p.eof);

        if (!headers_done) {
            size_t hdr_end = (size_t)-1;
            if (len >= 4) {
                for (size_t i = 0; i + 4 <= len; i++) {
                    if (buf[i] == '\r' && buf[i + 1] == '\n'
                        && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                        hdr_end = i + 4;
                        break;
                    }
                }
            }
            if (hdr_end != (size_t)-1) {
                status = client_parse_status(buf, len);
                memmove(buf, buf + hdr_end, len - hdr_end);
                len -= hdr_end;
                headers_done = true;
            }
        }

        if (headers_done) {
            size_t line_start = 0;
            for (size_t i = 0; i < len; i++) {
                if (buf[i] != '\n') {
                    continue;
                }
                size_t llen = i - line_start;
                if (llen > 0 && buf[line_start + llen - 1] == '\r') {
                    llen--;
                }
                if (llen > 0) {
                    n00b_string_t *line = n00b_string_from_raw(buf + line_start,
                                                               (int64_t)llen);
                    if (!on_line(ctx, line)) {
                        eof = true;
                        break;
                    }
                }
                line_start = i + 1;
            }
            if (line_start > 0) {
                memmove(buf, buf + line_start, len - line_start);
                len -= line_start;
            }
        }
    }

    client_stream_close(conn, reader, inbox, &held);
    if (!headers_done || status <= 0) {
        return n00b_result_err(int, N00B_HTTP_ERR_BAD_RESPONSE);
    }
    return n00b_result_ok(int, status);
}

n00b_result_t(int)
n00b_http_request_tcp_stream(n00b_string_t      *host,
                             uint16_t            port,
                             n00b_string_t      *path,
                             n00b_http_line_cb_t on_line,
                             void               *ctx) _kargs {
    n00b_string_t          *method       = nullptr;
    n00b_buffer_t          *body         = nullptr;
    n00b_string_t          *content_type = nullptr;
    n00b_http_h1_headers_t *extra        = nullptr;
}
{
    if (host == nullptr || path == nullptr || on_line == nullptr) {
        return n00b_result_err(int, N00B_HTTP_ERR_INVALID_URL);
    }

    n00b_pool_t       scratch_pool = {};
    n00b_allocator_t *scratch      = http_scratch_open(&scratch_pool);
    auto              rr           = tcp_http_request_stream(host,
                                                             port,
                                                             path,
                                                             on_line,
                                                             ctx,
                                                             method,
                                                             body,
                                                             content_type,
                                                             extra,
                                                             scratch);
    n00b_allocator_destroy(scratch);
    return rr;
}

/* ===========================================================================
 * §4   Race + fallback dispatcher
 *
 * Sequential per ~/dd/quic_6.md § 5.  H3 is attempted first when
 * `prefer_h3` is set AND the loss cache says the origin's h3 path
 * is healthy; on h3 failure we fall through to h1 and record the
 * loss.  The plan's deferred Happy-Eyeballs-style parallel race is
 * a Phase-7 candidate.
 * =========================================================================== */

static const char *
method_cstr(n00b_string_t *method)
{
    return (method && method->u8_bytes) ? method->data : "GET";
}

static const char *
content_type_cstr(n00b_string_t *ct)
{
    return (ct && ct->u8_bytes) ? ct->data : nullptr;
}

/* RFC 9110 § 15.4: 301/302/303 collapse the method to GET (drop the
 * body), 307/308 preserve the original method + body. */
bool
n00b_http_status_is_redirect(int s)
{
    return s == 301 || s == 302 || s == 303 || s == 307 || s == 308;
}

bool
n00b_http_status_preserves_method(int s)
{
    return s == 307 || s == 308;
}

/* Local convenience aliases — the public names are too long for the
 * tight loop body. */
#define status_is_redirect       n00b_http_status_is_redirect
#define status_preserves_method  n00b_http_status_preserves_method

/* Resolve a Location header against the current URL per RFC 3986
 * § 5 (transform-references algorithm).  Handles:
 *
 *   - Absolute URI    `https://...`            (we still require https)
 *   - Network-path    `//host/foo`             (scheme inherited)
 *   - Absolute-path   `/foo`                   (origin inherited)
 *   - Relative-path   `foo` / `../bar`         (path merged + dot-removed)
 *   - Empty / fragment-only `#frag` / `?q=1`   (path inherited)
 *
 * The dot-segment removal (§ 5.2.4) collapses `./` and `../`
 * segments deterministically; `..` past the root is clamped (RFC
 * § 5.2.4 step 2C/D). */

static char *
remove_dot_segments(const char *path,
                    size_t      len,
                    n00b_allocator_t *a,
                    size_t          *out_len)
{
    /* Per RFC 3986 § 5.2.4: scratch buffer at most as long as input. */
    char  *out = n00b_alloc_array_with_opts(char,
                                            len + 1,
                                            &(n00b_alloc_opts_t){.allocator = a});
    size_t in  = 0;
    size_t op  = 0;

    while (in < len) {
        /* "../" / "./" prefix → strip. */
        if (in + 2 < len + 1 && path[in] == '.' && path[in + 1] == '.'
            && in + 2 < len && path[in + 2] == '/') {
            in += 3;
            continue;
        }
        if (in + 1 < len + 1 && path[in] == '.'
            && in + 1 < len && path[in + 1] == '/') {
            in += 2;
            continue;
        }
        /* "/./" → "/". */
        if (in + 2 < len && path[in] == '/' && path[in + 1] == '.'
            && path[in + 2] == '/') {
            in += 2;
            continue;
        }
        /* "/." at end → "/". */
        if (in + 1 < len + 1 && path[in] == '/'
            && in + 1 < len && path[in + 1] == '.'
            && (in + 2 == len)) {
            out[op++] = '/';
            in        = len;
            continue;
        }
        /* "/../" — pop last segment from out, then "/" remains. */
        if (in + 3 < len && path[in] == '/' && path[in + 1] == '.'
            && path[in + 2] == '.' && path[in + 3] == '/') {
            in += 3;
            /* Pop last segment from out (back up to last '/'). */
            while (op > 0 && out[op - 1] != '/') op--;
            if (op > 0) op--;       /* drop trailing '/' too */
            continue;
        }
        /* "/.." at end → push '/'. */
        if (in + 2 < len + 1 && path[in] == '/' && path[in + 1] == '.'
            && in + 2 < len && path[in + 2] == '.'
            && (in + 3 == len)) {
            in += 2;
            while (op > 0 && out[op - 1] != '/') op--;
            if (op > 0) op--;
            out[op++] = '/';
            continue;
        }
        /* Standalone "." or ".." input — drop entirely. */
        if (len - in == 1 && path[in] == '.') break;
        if (len - in == 2 && path[in] == '.' && path[in + 1] == '.') break;

        /* Copy the next path segment up to (and including) the next '/'. */
        if (path[in] == '/') {
            out[op++] = '/';
            in++;
        }
        while (in < len && path[in] != '/') {
            out[op++] = path[in++];
        }
    }
    out[op] = '\0';
    *out_len = op;
    return out;
}

static n00b_string_t *
resolve_location(n00b_http_url_t  *current,
                 n00b_buffer_t    *location,
                 n00b_allocator_t *a)
{
    if (!location || location->byte_len == 0) return nullptr;
    const char *raw = location->data;
    size_t      len = (size_t)location->byte_len;
    /* Strip trailing CR/LF if any made it into the buffer. */
    while (len > 0 && (raw[len - 1] == '\r' || raw[len - 1] == '\n')) {
        len--;
    }
    if (len == 0) return nullptr;

    /* Strip a fragment (`#…`) — RFC 3986 § 5.3 says the resolver's
     * output preserves it, but HTTP requests never carry one so we
     * drop it. */
    const char *frag = memchr(raw, '#', len);
    if (frag) len = (size_t)(frag - raw);
    if (len == 0) {
        /* Fragment-only ref: target = current URL. */
        return n00b_string_from_raw(current->origin->data,
                                     (int64_t)current->origin->u8_bytes,
                                     .allocator = a);
    }

    /* (a) Absolute URI: starts with "scheme:".  We only honor https. */
    bool has_scheme = false;
    for (size_t i = 0; i < len; i++) {
        char c = raw[i];
        if (c == ':') { has_scheme = (i > 0); break; }
        if (i == 0 && !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
            break;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
              || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.')) {
            break;
        }
    }
    if (has_scheme) {
        /* Reject if not https — the dispatcher only speaks https. */
        if (len < 8
            || (raw[0] != 'h' && raw[0] != 'H')
            || (raw[1] != 't' && raw[1] != 'T')
            || (raw[2] != 't' && raw[2] != 'T')
            || (raw[3] != 'p' && raw[3] != 'P')
            || (raw[4] != 's' && raw[4] != 'S')
            || raw[5] != ':' || raw[6] != '/' || raw[7] != '/') {
            return nullptr;
        }
        return n00b_string_from_raw(raw, (int64_t)len, .allocator = a);
    }

    /* (b) Network-path: starts with "//".  Inherit scheme (https). */
    if (len >= 2 && raw[0] == '/' && raw[1] == '/') {
        size_t total = 6 + len;          /* "https:" + // + rest */
        char  *buf   = n00b_alloc_array_with_opts(char,
                                                  total + 1,
                                                  &(n00b_alloc_opts_t){.allocator = a});
        memcpy(buf, "https:", 6);
        memcpy(buf + 6, raw, len);
        buf[total] = '\0';
        return n00b_string_from_raw(buf, (int64_t)total, .allocator = a);
    }

    /* (c) Absolute path: starts with "/".  Inherit origin. */
    if (raw[0] == '/') {
        size_t plen;
        char  *cleaned = remove_dot_segments(raw, len, a, &plen);
        size_t total   = current->origin->u8_bytes + plen;
        char  *buf     = n00b_alloc_array_with_opts(char,
                                                    total + 1,
                                                    &(n00b_alloc_opts_t){.allocator = a});
        memcpy(buf, current->origin->data, current->origin->u8_bytes);
        memcpy(buf + current->origin->u8_bytes, cleaned, plen);
        buf[total] = '\0';
        return n00b_string_from_raw(buf, (int64_t)total, .allocator = a);
    }

    /* (d) Relative path: merge with current's path's directory. */
    const char *cur_path = (current->path && current->path->u8_bytes)
                                ? current->path->data : "/";
    size_t      cur_len  = (current->path && current->path->u8_bytes)
                                ? (size_t)current->path->u8_bytes : 1;
    /* Find last '/' in current path; everything up to and including
     * that becomes the merge prefix.  RFC 3986 § 5.2.3. */
    size_t last_slash = 0;
    for (size_t i = 0; i < cur_len; i++) {
        if (cur_path[i] == '/') last_slash = i + 1;
    }
    if (last_slash == 0) last_slash = 1;   /* at least "/" */
    size_t merged_len = last_slash + len;
    char  *merged     = n00b_alloc_array_with_opts(char,
                                                   merged_len + 1,
                                                   &(n00b_alloc_opts_t){.allocator = a});
    memcpy(merged, cur_path, last_slash);
    memcpy(merged + last_slash, raw, len);
    merged[merged_len] = '\0';

    size_t plen;
    char  *cleaned = remove_dot_segments(merged, merged_len, a, &plen);
    size_t total   = current->origin->u8_bytes + plen;
    char  *buf     = n00b_alloc_array_with_opts(char,
                                                total + 1,
                                                &(n00b_alloc_opts_t){.allocator = a});
    memcpy(buf, current->origin->data, current->origin->u8_bytes);
    memcpy(buf + current->origin->u8_bytes, cleaned, plen);
    buf[total] = '\0';
    return n00b_string_from_raw(buf, (int64_t)total, .allocator = a);
}

/* ASCII-case-insensitive byte compare of two host strings.  Host
 * comparison for the redirect allowlist is intentionally
 * structural — we compare against the URL's authority host (no
 * port, no path, no fragment) so callers don't have to encode the
 * port to permit a host running on a non-default port. */
static bool
host_eq_ascii_ci(const char *a, size_t alen,
                 const char *b, size_t blen)
{
    if (alen != blen) return false;
    for (size_t i = 0; i < alen; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

/* IDNA / UTS-46 canonicalization of a domain string for allowlist
 * matching.  Collapses every failure mode into a single "skip"
 * signal: returns `nullptr` iff
 *
 *   - input is null or empty (callers don't pass empty here; this is
 *     defensive against pathological allowlist entries), OR
 *   - `n00b_unicode_idna_to_ascii` reports any non-OK error code
 *     (`_DISALLOWED`, `_BIDI_ERROR`, `_PUNYCODE_ERROR`, `_LABEL_TOO_LONG`,
 *     `_DOMAIN_TOO_LONG`, `_CONTEXTJ_ERROR`, `_CONTEXTO_ERROR`,
 *     `_LEADING_COMBINING`, `_EMPTY_LABEL`, `_INVALID_ACE`,
 *     `_PROCESSING_ERROR`).
 *
 * On success returns the ACE form (pure-ASCII Punycode where
 * needed, byte-identical for already-ASCII inputs modulo
 * UTS-46 case folding).
 *
 * The result is transient — allocated on the runtime default
 * allocator, immediately consumed by a byte compare, then dropped
 * to the GC.  The matcher's documented "side-effect-free" contract
 * is preserved at the API boundary; transient internal allocations
 * are within scope per § 4.4 (the GC reclaims them).
 *
 * Contract note: as of DF-Y, `n00b_unicode_idna_to_ascii` returns
 * `_PROCESSING_ERROR` (not OK + empty) on invalid-UTF-8 input, so
 * the matcher no longer needs to second-guess an OK return with an
 * empty `u8_bytes`. */
static n00b_string_t *
idna_canonicalize_or_null(const char *src, size_t src_len)
{
    if (!src || src_len == 0) return nullptr;

    n00b_string_t *in = n00b_string_from_raw(src, (int64_t)src_len);
    n00b_unicode_idna_result_t r = n00b_unicode_idna_to_ascii(in);
    if (r.error != N00B_UNICODE_IDNA_OK) return nullptr;
    if (!r.value) return nullptr;
    return r.value;
}

/* Returns true iff @p host (assumed already pure-ASCII canonical
 * form — lowercased + UTS-46 ToASCII applied by the URL parser per
 * DF-X) is matched by some entry in @p allowlist.  Entries are run
 * through UTS-46 `to_ascii` so Unicode-authored entries cross-match
 * the ACE-form host.
 *
 * Two entry shapes are recognized:
 *
 *   - Exact entries (no `*` anywhere): canonicalize the whole
 *     entry, then ASCII-CI byte-compare against the host.
 *     Pure-ASCII entries land at the same bytes they would have hit
 *     pre-IDNA (ASCII is a fixed point of `to_ascii` modulo case
 *     folding), preserving the cand-#6 contract byte-identically.
 *
 *   - Wildcard entries (`*.DOMAIN` with at least one label after
 *     the dot, e.g. `*.example.com` or `*.例え.com`): classify on
 *     the RAW bytes (the `*.` prefix is literal ASCII and must NOT
 *     be fed to the IDNA pipeline — it would reject `*` as
 *     disallowed), then canonicalize only the `DOMAIN` portion
 *     after the `*.`.  Match any host of the form `X.DOMAIN` for
 *     some non-empty `X` after the entry's `DOMAIN` is
 *     canonicalized.  The apex `DOMAIN` itself is NOT matched.
 *
 *   - Any other `*` placement (`foo.*.com`, `**.example.com`,
 *     `*example.com`, bare `*`) is malformed and silently skipped.
 *
 * IDNA-error handling:
 *
 *   - Entry canonicalization failure (exact-side or DOMAIN-portion)
 *     → skip that entry, continue scanning the rest of the list.
 *     Same skip-as-malformed semantics as cand #6.
 *
 * An empty allowlist (0 entries) returns false — "no hosts
 * permitted".  A nullptr allowlist is *not* passed here; the
 * caller checks `redirect_host_allowlist != nullptr` before
 * calling this.
 *
 * DF-X note: prior versions of this matcher ran the host through
 * `idna_canonicalize_or_null` here.  That step is now redundant
 * because the URL parser canonicalizes the host upstream at parse
 * time; the host bytes that reach this function are guaranteed to
 * be the ACE form already.  Removing the host-side pass eliminates
 * the per-call canonicalization cost while preserving the contract
 * (host and entries still meet in ACE space). */
bool
n00b_http_host_in_allowlist(n00b_string_t                *host,
                            n00b_list_t(n00b_string_t *) *allowlist)
{
    if (!host || !allowlist) return false;

    /* DF-X: the URL parser delivers `host` in canonical ACE form
     * already, so we compare against the host bytes directly. */
    const char *hdat = host->data;
    size_t      hlen = host->u8_bytes;
    if (hlen == 0) return false;

    /* Lists are value types; the kwarg is a pointer to the
     * caller's lvalue.  The list macros expect lvalue access so we
     * dereference at every call site. */
    size_t n = n00b_list_len(*allowlist);
    for (size_t i = 0; i < n; i++) {
        n00b_string_t *entry = n00b_list_get(*allowlist, i);
        if (!entry) continue;
        size_t      elen = entry->u8_bytes;
        const char *edat = entry->data;

        /* Classify on the raw entry bytes: scan for any `*` byte. */
        bool   has_star      = false;
        size_t first_star_at = 0;
        for (size_t k = 0; k < elen; k++) {
            if (edat[k] == '*') {
                has_star      = true;
                first_star_at = k;
                break;
            }
        }

        if (!has_star) {
            /* Exact path — canonicalize the whole entry, then
             * ASCII-CI compare against the canonical host. */
            n00b_string_t *centry = idna_canonicalize_or_null(edat, elen);
            if (!centry) continue;
            if (host_eq_ascii_ci(hdat, hlen,
                                  centry->data, centry->u8_bytes)) {
                return true;
            }
            continue;
        }

        /* Wildcard candidate.  Accept iff:
         *   - the first (and only) `*` is at offset 0,
         *   - byte 1 is `.`,
         *   - there is at least one more byte after the dot
         *     (the DOMAIN part is non-empty),
         *   - no further `*` appears in the entry. */
        if (first_star_at != 0) continue;          /* e.g. `foo.*.com` */
        if (elen < 3)            continue;          /* `*` or `*.`   */
        if (edat[1] != '.')      continue;          /* e.g. `*example.com` */

        bool further_star = false;
        for (size_t k = 1; k < elen; k++) {
            if (edat[k] == '*') {
                further_star = true;
                break;
            }
        }
        if (further_star) continue;                 /* e.g. `**.example.com` */

        /* Canonicalize the DOMAIN portion only — the `*.` prefix
         * is literal ASCII and would be rejected by the IDNA
         * pipeline.  `domain` is everything after the `.`. */
        const char *domain     = edat + 2;
        size_t      domain_len = elen - 2;
        n00b_string_t *cdomain = idna_canonicalize_or_null(domain,
                                                           domain_len);
        if (!cdomain) continue;

        /* Form the canonical suffix `.<canonical-DOMAIN>` in a
         * tiny stack buffer.  Canonical ASCII domains cap at 253
         * bytes per IDNA (`_DOMAIN_TOO_LONG` rejects above that),
         * so 256 is sufficient. */
        char   suffix_buf[260];
        size_t cdomain_len = cdomain->u8_bytes;
        if (cdomain_len + 1 > sizeof(suffix_buf)) continue;
        suffix_buf[0] = '.';
        memcpy(suffix_buf + 1, cdomain->data, cdomain_len);
        const char *suffix     = suffix_buf;
        size_t      suffix_len = cdomain_len + 1;

        /* Strict-greater length + ASCII-CI tail match.  The strict
         * inequality guarantees at least one wildcarded-label byte
         * sits before the leading `.` in `suffix`, so the `.` acts
         * as the label boundary without an explicit leading-byte
         * check.  Punycode-encoded labels are pure ASCII, so the
         * invariant survives canonicalization unchanged. */
        if (hlen <= suffix_len) continue;
        const char *tail = hdat + (hlen - suffix_len);
        if (host_eq_ascii_ci(tail, suffix_len, suffix, suffix_len)) {
            return true;
        }
    }
    return false;
}

/* Single-shot dispatch (no redirect follow).  Factored out so the
 * redirect loop can call it repeatedly with adjusted args.
 *
 * `max_body_size` is threaded into both transports so the caller's
 * per-call body cap (DF-014) is enforced before the body
 * materializes past the limit.  0 = no cap, preserving existing
 * behavior for callers who don't pass the kwarg. */
static n00b_result_t(n00b_http_response_t *)
dispatch_once(n00b_http_url_t             *u,
              const char                  *method_str,
              n00b_buffer_t               *body,
              const char                  *ct_str,
	              n00b_http_h1_headers_t      *extra,
	              bool                         prefer_h3,
	              int32_t                      h3_handshake_ms,
	              int32_t                      timeout_ms,
	              n00b_quic_trust_t           *trust,
	              n00b_http_connection_pool_t *pool,
	              n00b_http_auth_t            *auth,
	              uint64_t                     max_body_size,
	              n00b_allocator_t            *a)
{
    /* n00b_http_h3_round_trip() dials the origin directly over QUIC — it
     * has no proxy CONNECT support (unlike the h1 path, which resolves
     * HTTP_PROXY/HTTPS_PROXY in h1_tls_connect()). Skip H3 whenever a
     * proxy route applies so a caller who set HTTPS_PROXY and took the
     * default prefer_h3=true can't have their request silently sent
     * direct-to-origin over UDP/QUIC before proxy policy is ever
     * consulted; falling through sends it via the proxy-aware h1 path
     * below instead. */
    bool proxy_applies = n00b_http_proxy_resolve(u).active;
    if (prefer_h3 && !proxy_applies && !loss_cache_h3_blocked(u->origin)) {
        auto rr = n00b_http_h3_round_trip(
            u,
            .method        = method_str,
            .body          = body,
            .content_type  = ct_str,
            .extra         = extra,
            .handshake_ms  = h3_handshake_ms,
            .await_ms      = timeout_ms,
            .trust         = trust,
            .pool          = pool,
            .auth          = auth,
            .max_body_size = max_body_size,
            .allocator     = a);
        if (n00b_result_is_ok(rr)) {
            return n00b_result_ok(n00b_http_response_t *,
                                   build_from_h3(n00b_result_get(rr), a));
        }
        /* An over-cap response on the h3 path is a policy verdict,
         * not an h3-transport failure — don't poison the loss cache
         * (the h1 path would hit the same cap).  Surface the error
         * directly so callers see a single, distinguishable code. */
        int32_t h3_err = (int32_t)n00b_result_get_err(rr);
        if (h3_err == N00B_HTTP_ERR_RESPONSE_TOO_LARGE) {
            return n00b_result_err(n00b_http_response_t *, h3_err);
        }
        loss_cache_record(u->origin);
    }
    auto rr1 = n00b_http_h1_round_trip(
        u,
	        .method        = method_str,
	        .body          = body,
	        .content_type  = ct_str,
	        .extra         = extra,
	        .timeout_ms    = timeout_ms,
	        .pool          = pool,
	        .auth          = auth,
	        .trust         = trust,
	        .max_body_size = max_body_size,
	        .allocator     = a);
    if (n00b_result_is_err(rr1)) {
        return n00b_result_err(n00b_http_response_t *,
                                n00b_result_get_err(rr1));
    }
    return n00b_result_ok(n00b_http_response_t *,
                           build_from_h1(n00b_result_get(rr1), a));
}

// The request itself. Every allocation, the returned response included, comes
// from `allocator`, which n00b_http_request_sync points at per-call scratch.
static n00b_result_t(n00b_http_response_t *)
http_request_sync_in(n00b_string_t *url)
    _kargs {
        n00b_string_t          *method            = nullptr;
        n00b_buffer_t          *body              = nullptr;
        n00b_string_t          *content_type      = nullptr;
        n00b_http_h1_headers_t *extra             = nullptr;
        bool                    prefer_h3         = true;
        int32_t                 h3_handshake_ms   = 1500;
        int32_t                 timeout_ms        = 30000;
        n00b_quic_trust_t      *trust             = nullptr;
        bool                    follow_redirects  = false;
        int32_t                 max_redirects     = 5;
        bool                    auto_decompress   = true;
	        n00b_string_t          *body_encoding     = nullptr;
	        n00b_http_cookie_jar_t *cookie_jar        = nullptr;
	        n00b_http_auth_t       *auth              = nullptr;
	        n00b_http_connection_pool_t *pool         = nullptr;
	        uint64_t                max_body_size     = 0;
        n00b_list_t(n00b_string_t *) *redirect_host_allowlist = nullptr;
        bool                    allow_plain_http  = false;
        n00b_allocator_t       *allocator         = nullptr;
    }
{
    n00b_allocator_t *a = allocator;

    /* The plain-HTTP path is a one-shot socket; it never touches
     * the connection pool, redirect loop, cookie jar, or TLS auth
     * helpers (which are all https-only concerns).  Branch early
     * so the rest of this function stays focused on the https
     * dispatcher. */
    if (allow_plain_http) {
        return plain_http_request_sync(url,
                                        method,
                                        body,
                                        content_type,
                                        extra,
                                        body_encoding,
                                        auto_decompress,
                                        timeout_ms,
                                        max_body_size,
                                        a);
    }

    /* Default to the per-runtime connection pool when the caller
     * did not pass an explicit one. */
    if (!pool) {
        pool = n00b_http_get_connection_pool();
    }

    n00b_string_t *cur_url     = url;
    n00b_string_t *cur_method  = method;
    n00b_buffer_t *cur_body    = body;
    n00b_string_t *cur_ct      = content_type;

    /* Apply request-body compression up front when requested.  We
     * compress once before any redirect loop iterations so that
     * 307/308 redirects send the same compressed bytes. */
    if (body_encoding && body_encoding->u8_bytes > 0 && cur_body) {
        auto cr = n00b_http_compress(cur_body, body_encoding->data,
                                       .allocator = a);
        if (n00b_result_is_err(cr)) {
            return n00b_result_err(n00b_http_response_t *,
                                    n00b_result_get_err(cr));
        }
        cur_body = n00b_result_get(cr);
    }

    int32_t hops = 0;
    while (true) {
        auto ur = n00b_http_url_parse(cur_url, .allocator = a);
        if (n00b_result_is_err(ur)) {
            return n00b_result_err(n00b_http_response_t *,
                                    n00b_result_get_err(ur));
        }
        n00b_http_url_t *u = n00b_result_get(ur);

        /* Build the outgoing-headers bag.  Order matters:
         *   1. Auth helper drops Bearer + DPoP onto a fresh bag.
         *   2. Cookie jar's Cookie: header.
         *   3. Accept-Encoding (when auto_decompress).
         *   4. Caller's `extra` overlays last so explicit values win.
         */
        n00b_http_h1_headers_t *headers = n00b_http_auth_apply(
            auth, nullptr, method_cstr(cur_method), u, .allocator = a);

        if (cookie_jar) {
            n00b_string_t *cookie = n00b_http_cookie_jar_header_for(
                cookie_jar, u, .allocator = a);
            if (cookie) {
                n00b_http_h1_headers_set(headers, "Cookie", cookie->data);
            }
        }
        if (auto_decompress) {
            n00b_string_t *ae = n00b_http_accept_encoding_header(
                .allocator = a);
            if (ae) {
                n00b_http_h1_headers_set(headers, "Accept-Encoding",
                                          ae->data);
            }
        }
        if (body_encoding && body_encoding->u8_bytes > 0 && cur_body) {
            /* Compressed body — advertise the encoding so the server
             * decodes correctly. */
            n00b_http_h1_headers_set(headers, "Content-Encoding",
                                      body_encoding->data);
        }
        if (extra) {
            size_t n = n00b_http_h1_headers_len(extra);
            for (size_t i = 0; i < n; i++) {
                const char *name;
                const char *value;
                if (n00b_http_h1_headers_at(extra, i, &name, &value)) {
                    n00b_http_h1_headers_set(headers, name, value);
                }
            }
        }

        auto rr = dispatch_once(u,
                                 method_cstr(cur_method),
                                 cur_body,
                                 content_type_cstr(cur_ct),
                                 headers,
                                 prefer_h3,
	                                 h3_handshake_ms,
	                                 timeout_ms,
	                                 trust,
	                                 pool,
	                                 auth,
	                                 max_body_size,
	                                 a);
        if (n00b_result_is_err(rr)) return rr;
        n00b_http_response_t *resp = n00b_result_get(rr);

        /* Update the cookie jar from any Set-Cookie headers.
         * We iterate every response header (case-insensitive name
         * match) so we capture all Set-Cookie lines, not just the
         * last one. */
        if (cookie_jar) {
            for (size_t i = 0; i < resp->n_headers; i++) {
                n00b_string_t *name  = resp->headers[i].name;
                n00b_buffer_t *value = resp->headers[i].value;
                if (!name || !value) continue;
                if (name->u8_bytes != 10) continue;
                if (!n00b_ascii_ci_eq_n(name->data, "set-cookie", 10)) {
                    continue;
                }
                /* value is a buffer; copy to NUL-terminated for the
                 * parser. */
                char *cstr = n00b_alloc_array_with_opts(char,
                                                        (size_t)value->byte_len + 1,
                                                        &(n00b_alloc_opts_t){.allocator = a});
                memcpy(cstr, value->data, (size_t)value->byte_len);
                cstr[value->byte_len] = '\0';
                n00b_http_cookie_jar_set_from_response(cookie_jar, u, cstr);
            }
        }

        /* Auto-decompress when the response advertises a
         * Content-Encoding the client supports. */
        if (auto_decompress && resp->body && resp->body->byte_len > 0) {
            n00b_buffer_t *enc = n00b_http_response_header(
                resp, r"content-encoding");
            if (enc && enc->byte_len > 0) {
                /* Build a NUL-terminated copy so the codec switch works. */
                size_t el = (size_t)enc->byte_len;
                char  *etmp = n00b_alloc_array_with_opts(char,
                                                         el + 1,
                                                         &(n00b_alloc_opts_t){.allocator = a});
                memcpy(etmp, enc->data, el);
                etmp[el] = '\0';
                auto dr = n00b_http_decompress(resp->body, etmp,
                                                .allocator = a);
                if (n00b_result_is_ok(dr)) {
                    resp->body = n00b_result_get(dr);
                }
                /* On decode failure we leave the body as-is; the
                 * caller can still inspect via the existing body
                 * accessor. */
            }
        }

        /* Optional response verifier hook. */
        if (auth && !n00b_http_auth_verify_response(auth, resp)) {
            return n00b_result_err(n00b_http_response_t *,
                                    N00B_HTTP_ERR_BAD_RESPONSE);
        }

        if (!follow_redirects || !status_is_redirect(resp->status)
            || hops >= max_redirects) {
            return n00b_result_ok(n00b_http_response_t *, resp);
        }

        /* Look for Location.  If absent, surface the redirect to the
         * caller as-is (some servers issue 3xx without Location). */
        n00b_buffer_t *loc =
            n00b_http_response_header(resp, r"location");
        if (!loc) {
            return n00b_result_ok(n00b_http_response_t *, resp);
        }
        n00b_string_t *next_url = resolve_location(u, loc, a);
        if (!next_url) {
            /* Unresolvable Location — surface the original response. */
            return n00b_result_ok(n00b_http_response_t *, resp);
        }

        /* Host-allowlist enforcement (DF-015): when the caller
         * supplied a non-null `redirect_host_allowlist`, the next
         * hop's authority host must be in the list.  We parse the
         * resolved URL to extract the canonical host (already
         * lowercased by the parser per RFC 3986 § 3.2.2) so the
         * comparison is structural and not bytewise-string. */
        if (redirect_host_allowlist) {
            auto next_ur = n00b_http_url_parse(next_url, .allocator = a);
            if (n00b_result_is_err(next_ur)) {
                return n00b_result_err(
                    n00b_http_response_t *,
                    n00b_result_get_err(next_ur));
            }
            n00b_http_url_t *next_u = n00b_result_get(next_ur);
            if (!n00b_http_host_in_allowlist(next_u->host,
                                             redirect_host_allowlist)) {
                return n00b_result_err(
                    n00b_http_response_t *,
                    N00B_HTTP_ERR_HOST_REDIRECT_NOT_ALLOWED);
            }
        }

        /* Method preservation per RFC 9110 § 15.4. */
        if (!status_preserves_method(resp->status)) {
            /* 301/302/303 → GET, drop body. */
            cur_method = r"GET";
            cur_body   = nullptr;
            cur_ct     = nullptr;
        }
        cur_url = next_url;
        hops++;
    }
}

n00b_result_t(n00b_http_response_t *)
n00b_http_request_sync(n00b_string_t *url)
    _kargs {
        n00b_string_t          *method            = nullptr;
        n00b_buffer_t          *body              = nullptr;
        n00b_string_t          *content_type      = nullptr;
        n00b_http_h1_headers_t *extra             = nullptr;
        bool                    prefer_h3         = true;
        int32_t                 h3_handshake_ms   = 1500;
        int32_t                 timeout_ms        = 30000;
        n00b_quic_trust_t      *trust             = nullptr;
        bool                    follow_redirects  = false;
        int32_t                 max_redirects     = 5;
        bool                    auto_decompress   = true;
        n00b_string_t          *body_encoding     = nullptr;
        n00b_http_cookie_jar_t *cookie_jar        = nullptr;
        n00b_http_auth_t       *auth              = nullptr;
        n00b_http_connection_pool_t *pool         = nullptr;
        uint64_t                max_body_size     = 0;
        n00b_list_t(n00b_string_t *) *redirect_host_allowlist = nullptr;
        bool                    allow_plain_http  = false;
        n00b_allocator_t       *allocator         = nullptr;
    }
{
    if (!url) {
        return n00b_result_err(n00b_http_response_t *,
                               N00B_HTTP_ERR_NULL_ARG);
    }

    n00b_pool_t       scratch_pool = {};
    n00b_allocator_t *scratch      = http_scratch_open(&scratch_pool);

    auto rr = http_request_sync_in(
        url,
        .method                  = method,
        .body                    = body,
        .content_type            = content_type,
        .extra                   = extra,
        .prefer_h3               = prefer_h3,
        .h3_handshake_ms         = h3_handshake_ms,
        .timeout_ms              = timeout_ms,
        .trust                   = trust,
        .follow_redirects        = follow_redirects,
        .max_redirects           = max_redirects,
        .auto_decompress         = auto_decompress,
        .body_encoding           = body_encoding,
        .cookie_jar              = cookie_jar,
        .auth                    = auth,
        .pool                    = pool,
        .max_body_size           = max_body_size,
        .redirect_host_allowlist = redirect_host_allowlist,
        .allow_plain_http        = allow_plain_http,
        .allocator               = scratch);
    return http_scratch_close(rr, scratch, allocator);
}

/* ===========================================================================
 * §5   Topic-shaped dispatcher (chunk 5b)
 *
 * Wraps `n00b_http_request_sync` in a worker thread whose result is
 * delivered on a one-shot conduit topic.  Sync callers
 * `n00b_conduit_read(...)` it; async callers
 * `n00b_conduit_subscribe(...)`.  Either way the worker publishes
 * exactly one response and closes the topic.
 * =========================================================================== */

static n00b_http_response_t *
make_error_response(int32_t err, n00b_allocator_t *a)
{
    n00b_http_response_t *r = n00b_alloc_with_opts(
        n00b_http_response_t,
        &(n00b_alloc_opts_t){.allocator = a});
    r->status    = 0;
    r->body      = n00b_buffer_empty(.allocator = a);
    r->headers   = nullptr;
    r->n_headers = 0;
    r->transport = N00B_HTTP_TRANSPORT_UNKNOWN;
    r->error     = err;
    r->allocator = a;
    return r;
}

static n00b_string_t *
http_worker_copy_string(n00b_string_t *s, n00b_allocator_t *a)
{
    if (s == nullptr) {
        return nullptr;
    }
    return n00b_string_from_raw(s->data, s->u8_bytes, .allocator = a);
}

static n00b_buffer_t *
http_worker_copy_buffer(n00b_buffer_t *b, n00b_allocator_t *a)
{
    if (b == nullptr) {
        return nullptr;
    }
    return n00b_buffer_copy(b, .allocator = a);
}

static n00b_http_h1_headers_t *
http_worker_copy_headers(n00b_http_h1_headers_t *h, n00b_allocator_t *a)
{
    if (h == nullptr) {
        return nullptr;
    }
    n00b_http_h1_headers_t *copy = n00b_http_h1_headers_new(.allocator = a);
    size_t                  n    = n00b_http_h1_headers_len(h);
    for (size_t i = 0; i < n; i++) {
        const char *name  = nullptr;
        const char *value = nullptr;
        if (n00b_http_h1_headers_at(h, i, &name, &value)) {
            n00b_http_h1_headers_set(copy, name, value);
        }
    }
    return copy;
}

static n00b_list_t(n00b_string_t *) *
http_worker_copy_string_list(n00b_list_t(n00b_string_t *) *src,
                             n00b_allocator_t             *a)
{
    if (src == nullptr) {
        return nullptr;
    }
    n00b_list_t(n00b_string_t *) *copy =
        n00b_alloc_with_opts(n00b_list_t(n00b_string_t *),
                             &(n00b_alloc_opts_t){.allocator = a});
    *copy = n00b_list_new(n00b_string_t *, .allocator = a);
    size_t n = n00b_list_len(*src);
    for (size_t i = 0; i < n; i++) {
        n00b_list_push(*copy,
                       http_worker_copy_string(n00b_list_get(*src, i), a));
    }
    return copy;
}

typedef struct {
    /* Owned heap copies — caller may drop refs after request returns. */
    n00b_conduit_t                                       *c;
    n00b_conduit_topic_t(n00b_http_response_t *)         *topic;
    n00b_string_t                                        *url;
    n00b_string_t                                        *method;
    n00b_buffer_t                                        *body;
    n00b_string_t                                        *content_type;
    n00b_http_h1_headers_t                               *extra;
    bool                                                  prefer_h3;
    int32_t                                               h3_handshake_ms;
    int32_t                                               timeout_ms;
    n00b_quic_trust_t                                    *trust;
    bool                                                  follow_redirects;
    int32_t                                               max_redirects;
    bool                                                  auto_decompress;
    n00b_string_t                                        *body_encoding;
    n00b_http_cookie_jar_t                               *cookie_jar;
    n00b_http_auth_t                                     *auth;
    n00b_http_connection_pool_t                          *pool;
    uint64_t                                              max_body_size;
    n00b_list_t(n00b_string_t *)                         *redirect_host_allowlist;
    n00b_allocator_t                                     *allocator;
} http_worker_args_t;

static void
http_worker_args_free(http_worker_args_t *a)
{
    if (a == nullptr) {
        return;
    }

    if (a->topic != nullptr) {
        n00b_conduit_topic_base_t *base =
            (n00b_conduit_topic_base_t *)a->topic;
        if (base->on_first_subscribe_ctx == a) {
            base->on_first_subscribe_ctx = nullptr;
            base->on_first_subscribe     = nullptr;
        }
    }

    http_free_string(a->url);
    http_free_string(a->method);
    http_free_buffer(a->body);
    http_free_string(a->content_type);
    n00b_http_h1_headers_free(a->extra);
    http_free_string(a->body_encoding);
    if (a->redirect_host_allowlist != nullptr) {
        size_t n = n00b_list_len(*a->redirect_host_allowlist);
        for (size_t i = 0; i < n; i++) {
            http_free_string(n00b_list_get(*a->redirect_host_allowlist, i));
        }
        n00b_list_free(*a->redirect_host_allowlist);
        n00b_free(a->redirect_host_allowlist);
    }
    n00b_free(a);
}

#ifdef N00B_DEBUG
void (*n00b_http_test_before_response_close)(n00b_conduit_topic_base_t *)
    = nullptr;
#endif

static void
publish_response(http_worker_args_t          *a,
                 n00b_http_response_t        *resp)
{
    /* Claim the publisher (we are the only producer). */
    n00b_result_t(n00b_conduit_publisher_t *) pres =
        n00b_conduit_publish_try_claim(
            (n00b_conduit_topic_base_t *)a->topic);
    if (n00b_result_is_err(pres)) {
        /* No subscriber will ever see this; close and return. */
        n00b_http_response_free(resp);
        n00b_conduit_topic_close((n00b_conduit_topic_base_t *)a->topic);
        return;
    }
    n00b_conduit_publisher_t *pub = n00b_result_get(pres);

    n00b_conduit_message_t(n00b_http_response_t *) *msg =
        n00b_alloc_with_opts(
            n00b_conduit_message_t(n00b_http_response_t *),
            &(n00b_alloc_opts_t){.allocator = a->allocator});
    msg->header.type       = N00B_CONDUIT_MSG_USER;
    msg->header.topic      = (n00b_conduit_topic_base_t *)a->topic;
    msg->header.generation =
        n00b_conduit_topic_generation((n00b_conduit_topic_base_t *)a->topic);
    msg->header.epoch      =
        n00b_conduit_topic_epoch((n00b_conduit_topic_base_t *)a->topic);
    msg->header.next       = nullptr;
    msg->payload           = resp;

    n00b_conduit_topic_deliver_msg(
        n00b_http_response_t *,
        a->topic, msg, N00B_CONDUIT_OP_ALL);

    n00b_conduit_publish_yield(pub);

#ifdef N00B_DEBUG
    if (n00b_http_test_before_response_close) {
        n00b_http_test_before_response_close(
            (n00b_conduit_topic_base_t *)a->topic);
    }
#endif

    /* One-shot topic: close after the publish so subscribers waiting
     * on the done-topic receive the closure signal and any further
     * read attempts fail fast. */
    n00b_conduit_topic_close((n00b_conduit_topic_base_t *)a->topic);
}

static void *
http_worker_main(void *arg)
{
    http_worker_args_t *a = (http_worker_args_t *)arg;

    auto rr = n00b_http_request_sync(
        a->url,
        .method                  = a->method,
        .body                    = a->body,
        .content_type            = a->content_type,
        .extra                   = a->extra,
        .prefer_h3               = a->prefer_h3,
        .h3_handshake_ms         = a->h3_handshake_ms,
        .timeout_ms              = a->timeout_ms,
        .trust                   = a->trust,
        .follow_redirects        = a->follow_redirects,
        .max_redirects           = a->max_redirects,
        .auto_decompress         = a->auto_decompress,
        .body_encoding           = a->body_encoding,
        .cookie_jar              = a->cookie_jar,
        .auth                    = a->auth,
        .pool                    = a->pool,
        .max_body_size           = a->max_body_size,
        .redirect_host_allowlist = a->redirect_host_allowlist,
        .allocator               = a->allocator);

    n00b_http_response_t *resp;
    if (n00b_result_is_ok(rr)) {
        resp = n00b_result_get(rr);
    } else {
        resp = make_error_response((int32_t)n00b_result_get_err(rr),
                                   a->allocator);
    }
    publish_response(a, resp);
    http_worker_args_free(a);
    return nullptr;
}

/* Adapter: the conduit_service work-fn signature is `void(void*)`,
 * but http_worker_main is `void *(*)(void *)` (n00b_thread_spawn
 * shape).  This adapter just calls through. */
static void
http_worker_submit_adapter(void *arg)
{
    (void)http_worker_main(arg);
}

/* on_first_subscribe hook: submits the worker job once the first
 * subscriber attaches.  This avoids the publish-before-subscribe
 * race where a fast worker fans out to zero subscribers and the
 * caller's read() never wakes.
 *
 * We submit via the conduit's service threadpool (lazy-spawned on
 * first call) so concurrent requests share a small set of worker
 * threads instead of paying a thread-create per call. */
static void
http_request_first_sub_cb(n00b_conduit_topic_base_t *topic, void *ctx)
{
    (void)topic;
    http_worker_args_t *wargs = (http_worker_args_t *)ctx;

    /* Get-or-create the conduit's service, and start it: submit rejects a
     * service that was never started. Running on the service matters
     * beyond reuse, because n00b_conduit_destroy stops the service and
     * waits for its jobs before it frees the topic this worker closes. */
    n00b_conduit_t *c = wargs->c;
    n00b_conduit_service_t *svc = c->service;
    if (!svc) {
        auto sr = n00b_conduit_service_new(c);
        if (n00b_result_is_ok(sr)) {
            svc = n00b_result_get(sr);
        }
    }
    if (svc && n00b_result_is_err(n00b_conduit_service_start(svc))) {
        svc = nullptr;
    }

    if (svc) {
        auto sub = n00b_conduit_service_submit_grow(
            svc, http_worker_submit_adapter, wargs);
        if (n00b_result_is_ok(sub)) return;
    }

    /* Service submit unavailable — fall back to per-request
     * thread spawn rather than fail the request entirely. Nothing
     * joins this thread, so destroying the conduit while it runs
     * frees the topic out from under it. */
    auto tr = n00b_thread_spawn(http_worker_main, wargs);
    if (n00b_result_is_err(tr)) {
        /* Last-resort: synthesize and publish inline (caller is
         * already subscribed, so this delivers cleanly). */
        publish_response(wargs,
                         make_error_response(N00B_HTTP_ERR_INVALID_URL,
                                              wargs->allocator));
        http_worker_args_free(wargs);
        return;
    }
    (void)n00b_result_get(tr);
}

n00b_result_t(n00b_conduit_topic_t(n00b_http_response_t *) *)
n00b_http_request(n00b_conduit_t *c, n00b_string_t *url)
    _kargs {
        n00b_string_t          *method            = nullptr;
        n00b_buffer_t          *body              = nullptr;
        n00b_string_t          *content_type      = nullptr;
        n00b_http_h1_headers_t *extra             = nullptr;
        bool                    prefer_h3         = true;
        int32_t                 h3_handshake_ms   = 1500;
        int32_t                 timeout_ms        = 30000;
        n00b_quic_trust_t      *trust             = nullptr;
        bool                    follow_redirects  = false;
        int32_t                 max_redirects     = 5;
        bool                    auto_decompress   = true;
	        n00b_string_t          *body_encoding     = nullptr;
	        n00b_http_cookie_jar_t *cookie_jar        = nullptr;
	        n00b_http_auth_t       *auth              = nullptr;
	        n00b_http_connection_pool_t *pool         = nullptr;
	        uint64_t                max_body_size     = 0;
        n00b_list_t(n00b_string_t *) *redirect_host_allowlist = nullptr;
        n00b_allocator_t       *allocator         = nullptr;
    }
{
    if (!c) {
        return n00b_result_err(
            n00b_conduit_topic_t(n00b_http_response_t *) *,
            N00B_HTTP_ERR_NULL_ARG);
    }
    if (!url) {
        return n00b_result_err(
            n00b_conduit_topic_t(n00b_http_response_t *) *,
            N00B_HTTP_ERR_NULL_ARG);
    }

    (void)allocator;
    n00b_allocator_t *a = c->allocator
        ? c->allocator
        : (n00b_allocator_t *)&n00b_get_runtime()->conduit_pool;

    /* Allocate a fresh topic.  USER_EVENT tag + atomic id keeps
     * URIs unique per request. */
    static _Atomic(uint64_t) next_req_id = 1;
    uint64_t           id  = atomic_fetch_add(&next_req_id, 1);
    n00b_conduit_uri_t uri =
        n00b_conduit_int_uri(N00B_CONDUIT_TAG_USER_EVENT, id);

    n00b_result_t(n00b_conduit_topic_base_t *) tres =
        n00b_conduit_topic_get(
            c, uri,
            sizeof(n00b_conduit_topic_t(n00b_http_response_t *)));
    if (n00b_result_is_err(tres)) {
        return n00b_result_err(
            n00b_conduit_topic_t(n00b_http_response_t *) *,
            n00b_result_get_err(tres));
    }
    n00b_conduit_topic_t(n00b_http_response_t *) *topic =
        (n00b_conduit_topic_t(n00b_http_response_t *) *)
            n00b_result_get(tres);

    /* The TOPIC_IMPL macro generated `_N00B_TOPIC_FN(init, T)` which
     * sets up subscriptions list + done-topic.  Calling it idempotent
     * for fresh topics. */
    topic->subscriptions =
        n00b_list_new(n00b_conduit_subscription_t(n00b_http_response_t *) *,
                      .allocator = c->allocator);
    topic->inbox = nullptr;

    /* Stash request kwargs in a heap struct the worker owns. */
    http_worker_args_t *wargs = n00b_alloc_with_opts(
        http_worker_args_t,
        &(n00b_alloc_opts_t){.allocator = a});
    wargs->c                       = c;
    wargs->topic                   = topic;
    wargs->url                     = http_worker_copy_string(url, a);
    wargs->method                  = http_worker_copy_string(method, a);
    wargs->body                    = http_worker_copy_buffer(body, a);
    wargs->content_type            = http_worker_copy_string(content_type, a);
    wargs->extra                   = http_worker_copy_headers(extra, a);
    wargs->prefer_h3               = prefer_h3;
    wargs->h3_handshake_ms         = h3_handshake_ms;
    wargs->timeout_ms              = timeout_ms;
    wargs->trust                   = trust;
    wargs->follow_redirects        = follow_redirects;
    wargs->max_redirects           = max_redirects;
    wargs->auto_decompress         = auto_decompress;
    wargs->body_encoding           = http_worker_copy_string(body_encoding, a);
    wargs->cookie_jar              = cookie_jar;
    wargs->auth                    = auth;
    wargs->pool                    = pool;
    wargs->max_body_size           = max_body_size;
    wargs->redirect_host_allowlist =
        http_worker_copy_string_list(redirect_host_allowlist, a);
    wargs->allocator               = a;

    /* Defer the worker spawn until the first subscriber attaches.
     * The conduit topic's on_first_subscribe hook fires from inside
     * the subscribe() path, so by the time the worker publishes, the
     * subscriber's inbox is in place. */
    n00b_conduit_topic_base_t *base =
        (n00b_conduit_topic_base_t *)topic;
    base->on_first_subscribe_ctx = wargs;
    base->on_first_subscribe     = http_request_first_sub_cb;

    return n00b_result_ok(
        n00b_conduit_topic_t(n00b_http_response_t *) *, topic);
}
