/*
 * test_http_h1.c — Phase 6 chunk 2 (sub-chunk 2.1) unit tests.
 *
 * Coverage for the HTTP/1.1 response parser:
 *   - Status line: HTTP/1.1 + 1.0; valid + malformed
 *   - Headers: simple set, case-insensitive lookup, last-wins on dup
 *   - Body: Content-Length, Transfer-Encoding: chunked, no-length
 *   - Keep-alive: 1.1 default, 1.0 default, explicit close, explicit
 *     keep-alive on 1.0, multi-token Connection values
 *
 * Sub-chunk 2.2 will add request-builder tests + an in-process
 * loopback round-trip; this file is parser-only.
 */

#define N00B_USE_INTERNAL_API
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/buffer.h"
#include "core/string.h"
#include "adt/result.h"
#include "internal/net/http/http_url.h"
#include "internal/net/http/http_h1.h"

static n00b_buffer_t *
B(const char *bytes, size_t len)
{
    return n00b_buffer_from_bytes((char *)bytes, (int64_t)len);
}

static n00b_buffer_t *
Bcstr(const char *bytes)
{
    return B(bytes, strlen(bytes));
}

static n00b_http_h1_response_t *
parse_ok(const char *raw, size_t len)
{
    auto r = n00b_http_h1_response_parse(B(raw, len));
    if (n00b_result_is_err(r)) {
        fprintf(stderr,
                "  [FAIL] expected parse success but got err=%d\n",
                (int)n00b_result_get_err(r));
        abort();
    }
    return n00b_result_get(r);
}

static int32_t
parse_err(const char *raw, size_t len)
{
    auto r = n00b_http_h1_response_parse(B(raw, len));
    if (n00b_result_is_ok(r)) {
        fprintf(stderr,
                "  [FAIL] expected parse failure but parser succeeded\n");
        abort();
    }
    return (int32_t)n00b_result_get_err(r);
}

static bool
body_eq(n00b_buffer_t *b, const char *cstr)
{
    size_t cl = strlen(cstr);
    return b && b->byte_len == cl
        && (cl == 0 || memcmp(b->data, cstr, cl) == 0);
}

/* ---- Tests ---- */

static void
test_basic_200(void)
{
    static const char raw[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";
    n00b_http_h1_response_t *r = parse_ok(raw, sizeof(raw) - 1);
    assert(r->status == 200);
    assert(r->http_minor == 1);
    assert(strcmp(n00b_http_h1_headers_get_cstr(r->headers,
                                                 "content-type"),
                  "text/plain") == 0);
    assert(body_eq(r->body, "hello"));
    /* HTTP/1.1 default is keep-alive when no Connection header. */
    assert(r->keep_alive == true);
    printf("  [PASS] basic 200 OK with Content-Length\n");
}

static void
test_404_no_body(void)
{
    static const char raw[] =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n";
    n00b_http_h1_response_t *r = parse_ok(raw, sizeof(raw) - 1);
    assert(r->status == 404);
    assert(body_eq(r->body, ""));
    assert(r->keep_alive == false);
    printf("  [PASS] 404 + Connection: close + empty body\n");
}

static void
test_chunked_body(void)
{
    static const char raw[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\n"
        "Hello\r\n"
        "6\r\n"
        " World\r\n"
        "0\r\n"
        "\r\n";
    n00b_http_h1_response_t *r = parse_ok(raw, sizeof(raw) - 1);
    assert(r->status == 200);
    assert(body_eq(r->body, "Hello World"));
    printf("  [PASS] chunked decoding (5+6+0)\n");
}

static void
test_chunked_with_extension(void)
{
    /* RFC 9112 § 7.1.1: chunk-ext is OK after the size, before CRLF. */
    static const char raw[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "a; foo=bar\r\n"
        "0123456789\r\n"
        "0\r\n"
        "\r\n";
    n00b_http_h1_response_t *r = parse_ok(raw, sizeof(raw) - 1);
    assert(body_eq(r->body, "0123456789"));
    printf("  [PASS] chunked size-line with chunk-extension\n");
}

static void
test_no_length_no_chunked(void)
{
    /* Read-to-EOF semantics for HTTP/1.0 responses or
     * Connection: close where body length is implicit. */
    static const char raw[] =
        "HTTP/1.0 200 OK\r\n"
        "\r\n"
        "tail bytes only";
    n00b_http_h1_response_t *r = parse_ok(raw, sizeof(raw) - 1);
    assert(r->http_minor == 0);
    assert(body_eq(r->body, "tail bytes only"));
    /* HTTP/1.0 default is close. */
    assert(r->keep_alive == false);
    printf("  [PASS] read-to-EOF body on HTTP/1.0\n");
}

static void
test_h10_keep_alive_opt_in(void)
{
    static const char raw[] =
        "HTTP/1.0 200 OK\r\n"
        "Connection: Keep-Alive\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    n00b_http_h1_response_t *r = parse_ok(raw, sizeof(raw) - 1);
    assert(r->http_minor == 0);
    assert(r->keep_alive == true);
    printf("  [PASS] HTTP/1.0 keep-alive opt-in honored\n");
}

static void
test_multitoken_connection(void)
{
    /* Connection: keep-alive, Upgrade — keep-alive directive present. */
    static const char raw[] =
        "HTTP/1.1 200 OK\r\n"
        "Connection: keep-alive, Upgrade\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    n00b_http_h1_response_t *r = parse_ok(raw, sizeof(raw) - 1);
    assert(r->keep_alive == true);
    printf("  [PASS] multi-token Connection includes keep-alive\n");
}

static void
test_connection_close_wins(void)
{
    /* If `close` is present alongside other tokens, close wins. */
    static const char raw[] =
        "HTTP/1.1 200 OK\r\n"
        "Connection: keep-alive, close\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    n00b_http_h1_response_t *r = parse_ok(raw, sizeof(raw) - 1);
    assert(r->keep_alive == false);
    printf("  [PASS] Connection: close wins over keep-alive\n");
}

static void
test_case_insensitive_lookup(void)
{
    static const char raw[] =
        "HTTP/1.1 200 OK\r\n"
        "X-Custom-Header: yes\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    n00b_http_h1_response_t *r = parse_ok(raw, sizeof(raw) - 1);
    assert(strcmp(n00b_http_h1_headers_get_cstr(r->headers,
                                                 "x-custom-header"),
                  "yes") == 0);
    assert(strcmp(n00b_http_h1_headers_get_cstr(r->headers,
                                                 "X-CUSTOM-HEADER"),
                  "yes") == 0);
    printf("  [PASS] header lookup is case-insensitive\n");
}

static void
test_last_wins_duplicate(void)
{
    static const char raw[] =
        "HTTP/1.1 200 OK\r\n"
        "X-Trace: first\r\n"
        "X-Trace: second\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    n00b_http_h1_response_t *r = parse_ok(raw, sizeof(raw) - 1);
    assert(strcmp(n00b_http_h1_headers_get_cstr(r->headers, "x-trace"),
                  "second") == 0);
    printf("  [PASS] duplicate header — last wins\n");
}

static void
test_truncated_content_length(void)
{
    /* Server advertised 100 bytes but only 5 arrived: parser
     * emits a 5-byte body rather than reading past the buffer.
     * The transport layer is responsible for surfacing the
     * truncation as an error if it matters. */
    static const char raw[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 100\r\n"
        "\r\n"
        "short";
    n00b_http_h1_response_t *r = parse_ok(raw, sizeof(raw) - 1);
    assert(body_eq(r->body, "short"));
    printf("  [PASS] over-advertised Content-Length truncated to actual\n");
}

static void
test_status_text_preserved(void)
{
    static const char raw[] =
        "HTTP/1.1 503 Service Unavailable\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    n00b_http_h1_response_t *r = parse_ok(raw, sizeof(raw) - 1);
    assert(r->status == 503);
    assert(r->status_text->u8_bytes == strlen("Service Unavailable"));
    assert(memcmp(r->status_text->data,
                  "Service Unavailable",
                  r->status_text->u8_bytes) == 0);
    printf("  [PASS] reason phrase preserved\n");
}

static void
test_malformed_status_line(void)
{
    assert(parse_err("HTTP/2.0 200 OK\r\n\r\n",
                     strlen("HTTP/2.0 200 OK\r\n\r\n"))
           == N00B_HTTP_ERR_BAD_RESPONSE);
    assert(parse_err("Not HTTP at all\r\n\r\n",
                     strlen("Not HTTP at all\r\n\r\n"))
           == N00B_HTTP_ERR_BAD_RESPONSE);
    assert(parse_err("HTTP/1.1 99 Bad\r\n\r\n",
                     strlen("HTTP/1.1 99 Bad\r\n\r\n"))
           == N00B_HTTP_ERR_BAD_RESPONSE);
    printf("  [PASS] malformed status lines rejected\n");
}

static void
test_no_header_terminator(void)
{
    static const char raw[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 0\r\n";   /* no trailing CRLFCRLF */
    assert(parse_err(raw, sizeof(raw) - 1) == N00B_HTTP_ERR_BAD_RESPONSE);
    printf("  [PASS] missing CRLFCRLF rejected\n");
}

static void
test_null_input(void)
{
    auto r = n00b_http_h1_response_parse(nullptr);
    assert(n00b_result_is_err(r));
    assert((int32_t)n00b_result_get_err(r) == N00B_HTTP_ERR_NULL_ARG);
    /* Empty buffer also rejected. */
    auto r2 = n00b_http_h1_response_parse(Bcstr(""));
    assert(n00b_result_is_err(r2));
    assert((int32_t)n00b_result_get_err(r2) == N00B_HTTP_ERR_NULL_ARG);
    printf("  [PASS] null + empty input rejected\n");
}

/* ---- Request builder helpers ---- */

static bool
buf_contains(n00b_buffer_t *b, const char *needle)
{
    if (!b || !needle) return false;
    size_t nlen = strlen(needle);
    if (b->byte_len < nlen) return false;
    for (size_t i = 0; i + nlen <= b->byte_len; i++) {
        if (memcmp(b->data + i, needle, nlen) == 0) return true;
    }
    return false;
}

static n00b_http_url_t *
URL(const char *url)
{
    auto r = n00b_http_url_parse(n00b_string_from_cstr(url));
    assert(n00b_result_is_ok(r));
    return n00b_result_get(r);
}

/* ---- Request builder tests ---- */

static void
test_request_build_basic_get(void)
{
    n00b_buffer_t *r = n00b_http_h1_request_build(URL("https://example.com/"));
    assert(buf_contains(r, "GET / HTTP/1.1\r\n"));
    assert(buf_contains(r, "Host: example.com\r\n"));
    assert(buf_contains(r, "User-Agent: n00b-http/0.1\r\n"));
    assert(buf_contains(r, "Accept: */*\r\n"));
    assert(buf_contains(r, "Connection: close\r\n"));
    /* End-of-headers marker present. */
    assert(buf_contains(r, "\r\n\r\n"));
    /* No Content-Length when no body. */
    assert(!buf_contains(r, "Content-Length"));
    printf("  [PASS] basic GET request: line + Host + UA + Accept + close\n");
}

static void
test_request_build_post_body(void)
{
    n00b_buffer_t *body = Bcstr("{\"k\":1}");
    n00b_buffer_t *r    = n00b_http_h1_request_build(
        URL("https://api.example.com:8443/v1/widgets?tag=red"),
        .method       = "POST",
        .body         = body,
        .content_type = "application/json");
    assert(buf_contains(r, "POST /v1/widgets?tag=red HTTP/1.1\r\n"));
    assert(buf_contains(r, "Host: api.example.com:8443\r\n"));
    assert(buf_contains(r, "Content-Type: application/json\r\n"));
    assert(buf_contains(r, "Content-Length: 7\r\n"));
    /* Body bytes are appended after the blank line. */
    assert(buf_contains(r, "{\"k\":1}"));
    printf("  [PASS] POST + non-default port + query + body + content-type\n");
}

static void
test_request_build_keep_alive(void)
{
    n00b_buffer_t *r = n00b_http_h1_request_build(
        URL("https://example.com/"),
        .keep_alive = true);
    assert(buf_contains(r, "Connection: keep-alive\r\n"));
    assert(!buf_contains(r, "Connection: close"));
    printf("  [PASS] keep_alive=true emits Connection: keep-alive\n");
}

static void
test_request_build_ipv6_host(void)
{
    n00b_buffer_t *r = n00b_http_h1_request_build(
        URL("https://[2001:db8::42]:9443/api"));
    /* Brackets + non-default port preserved on the wire. */
    assert(buf_contains(r, "Host: [2001:db8::42]:9443\r\n"));
    assert(buf_contains(r, "GET /api HTTP/1.1\r\n"));
    printf("  [PASS] IPv6 literal Host re-bracketed on the wire\n");
}

static void
test_request_build_ipv6_default_port(void)
{
    /* Implicit 443 → port omitted from Host even for IPv6. */
    n00b_buffer_t *r = n00b_http_h1_request_build(
        URL("https://[::1]/"));
    assert(buf_contains(r, "Host: [::1]\r\n"));
    assert(!buf_contains(r, "Host: [::1]:443"));
    printf("  [PASS] IPv6 default port omitted from Host\n");
}

static void
test_request_build_extra_overrides_builtin(void)
{
    /* When the caller supplies User-Agent in extra, the built-in one
     * is suppressed (no duplicate in the request). */
    n00b_http_h1_headers_t *extra = n00b_http_h1_headers_new();
    n00b_http_h1_headers_set(extra, "User-Agent", "test/1.0");
    n00b_http_h1_headers_set(extra, "X-Trace-Id", "abc-123");
    n00b_buffer_t *r = n00b_http_h1_request_build(
        URL("https://example.com/"),
        .extra = extra);
    assert(buf_contains(r, "user-agent: test/1.0\r\n"));
    assert(buf_contains(r, "x-trace-id: abc-123\r\n"));
    /* Built-in default must be absent — no duplicate User-Agent. */
    assert(!buf_contains(r, "User-Agent: n00b-http/0.1"));
    printf("  [PASS] caller's extra header overrides built-in\n");
}

static void
test_request_build_query_default_path(void)
{
    /* URL parser already substitutes "/" for missing path; verify the
     * builder honors it. */
    n00b_buffer_t *r = n00b_http_h1_request_build(
        URL("https://example.com?only=query"));
    assert(buf_contains(r, "GET /?only=query HTTP/1.1\r\n"));
    printf("  [PASS] missing path renders as / in request line\n");
}

static void
test_request_build_long_header_kept_whole(void)
{
    /* A WorkOS bearer token is ~1.3 KB. Header lines used to be cut at
     * 1023 bytes, losing the CRLF, which left a malformed request that
     * servers stalled on. */
    static char value[2048];
    memcpy(value, "Bearer ", 7);
    memset(value + 7, 'a', sizeof(value) - 8);
    value[sizeof(value) - 1] = '\0';

    static char expected[2200];
    snprintf(expected, sizeof(expected), "authorization: %s\r\n", value);

    n00b_http_h1_headers_t *extra = n00b_http_h1_headers_new();
    n00b_http_h1_headers_set(extra, "Authorization", value);
    n00b_http_h1_headers_set(extra, "X-After", "still-here");
    n00b_buffer_t *r = n00b_http_h1_request_build(
        URL("https://example.com/v1/token/exchange"),
        .method       = "POST",
        .body         = Bcstr("{}"),
        .content_type = "application/json",
        .extra        = extra);
    assert(buf_contains(r, expected));
    assert(buf_contains(r, "x-after: still-here\r\n"));
    assert(buf_contains(r, "\r\n\r\n{}"));
    printf("  [PASS] a header over 1 KB is sent whole, with its CRLF\n");
}

static void
test_request_build_long_request_line_kept_whole(void)
{
    static char url[1600];
    static char line[1700];
    char        query[1400];
    memset(query, 'q', sizeof(query) - 1);
    query[sizeof(query) - 1] = '\0';
    snprintf(url, sizeof(url), "https://example.com/search?k=%s", query);
    snprintf(line, sizeof(line), "GET /search?k=%s HTTP/1.1\r\n", query);

    n00b_buffer_t *r = n00b_http_h1_request_build(URL(url));
    assert(buf_contains(r, line));
    assert(buf_contains(r, "Host: example.com\r\n"));
    printf("  [PASS] a request line over 1 KB is sent whole\n");
}

static void
test_headers_iteration(void)
{
    static const char raw[] =
        "HTTP/1.1 200 OK\r\n"
        "X-One: 1\r\n"
        "X-Two: 2\r\n"
        "X-Three: 3\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    n00b_http_h1_response_t *r = parse_ok(raw, sizeof(raw) - 1);
    assert(n00b_http_h1_headers_len(r->headers) == 4);
    const char *n0;
    const char *v0;
    assert(n00b_http_h1_headers_at(r->headers, 0, &n0, &v0));
    assert(strcmp(n0, "x-one") == 0 && strcmp(v0, "1") == 0);
    /* Out-of-range index → false. */
    assert(!n00b_http_h1_headers_at(r->headers, 99, &n0, &v0));
    printf("  [PASS] headers_at iteration + bounds check\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_http_h1:\n");
    test_basic_200();
    test_404_no_body();
    test_chunked_body();
    test_chunked_with_extension();
    test_no_length_no_chunked();
    test_h10_keep_alive_opt_in();
    test_multitoken_connection();
    test_connection_close_wins();
    test_case_insensitive_lookup();
    test_last_wins_duplicate();
    test_truncated_content_length();
    test_status_text_preserved();
    test_malformed_status_line();
    test_no_header_terminator();
    test_null_input();
    test_headers_iteration();
    test_request_build_basic_get();
    test_request_build_post_body();
    test_request_build_keep_alive();
    test_request_build_ipv6_host();
    test_request_build_ipv6_default_port();
    test_request_build_extra_overrides_builtin();
    test_request_build_query_default_path();
    test_request_build_long_header_kept_whole();
    test_request_build_long_request_line_kept_whole();
    printf("All test_http_h1 tests passed.\n");

    n00b_shutdown();
    return 0;
}
