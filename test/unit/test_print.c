/*
 * test_print.c — Tests for the print API.
 *
 * Tests 1-7: n00b_to_string conversions.
 * Tests 8-11: n00b_print via conduit topic wired to a pipe.
 * Tests 12-13: runtime stdout/stderr topic sanity checks.
 * Tests 14-16: n00b_printf via conduit topic wired to a pipe.
 * Test 17: n00b_eprintf via dup2 redirect of fd 2.
 */

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdatomic.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
// Matches test_rocs_plan_pathological.c, which already pulls windows.h in on
// this lane: the lean define keeps the winsock1 declarations out, which
// otherwise collide with the winsock2 the conduit headers use.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#endif
#ifdef __linux__
#include <sys/syscall.h>
#include "core/stw.h"
#endif

#include "n00b.h"
#include "conduit/print.h"
#include "conduit/conduit.h"
#include "conduit/io.h"
#include "conduit/fd_managed.h"
#include "conduit/fd_writer.h"
#include "conduit/service.h"
#include "conduit/timer.h"
#include "conduit/xform_types.h"
#include "core/alloc.h"
#include "adt/dict_untyped.h"
#include "core/runtime.h"
#include "core/thread.h"
#include "core/platform.h" // base_nanosleep_ns -- portable, unlike nanosleep
#include "core/syscall.h"  // n00b_raw_write_stall_hook
#include "conduit/fd_writer.h"
#include "core/type_info.h"
#include "core/string.h"
#include "core/buffer.h"
#include "text/strings/string_convert.h"
#include "text/strings/fmt_numbers.h"

// Helper: create a pipe and wire a conduit topic + fd_writer to the
// write end, so that n00b_print(.topic = tp.topic) works.
typedef struct {
    int                                    read_fd;
    int                                    write_fd;
    n00b_conduit_topic_t(n00b_buffer_t *) *topic;
} test_pipe_t;

static _Atomic(uint64_t) test_pipe_id = 1;

static int
test_pipe_create(int fds[2])
{
#ifdef _WIN32
    return _pipe(fds, 4096, _O_BINARY);
#else
    return pipe(fds);
#endif
}

static int
test_fd_close(int fd)
{
#ifdef _WIN32
    return _close(fd);
#else
    return close(fd);
#endif
}

static int
test_fd_read(int fd, char *buf, int len)
{
#ifdef _WIN32
    return _read(fd, buf, (unsigned int)len);
#else
    return (int)read(fd, buf, (size_t)len);
#endif
}

static int
test_fd_dup(int fd)
{
#ifdef _WIN32
    return _dup(fd);
#else
    return dup(fd);
#endif
}

static int
test_fd_dup2(int oldfd, int newfd)
{
#ifdef _WIN32
    return _dup2(oldfd, newfd);
#else
    return dup2(oldfd, newfd);
#endif
}

// Redirecting fd 1 for a test that asserts on print's RAW fallback needs both
// mechanisms on Windows, because the two paths resolve the descriptor
// differently:
//
//   - the conduit path goes through the CRT fd table
//     (_get_osfhandle((int)owner->fd), src/conduit/fd_managed.c), so _dup2
//     is what redirects it;
//   - the fallback is n00b_raw_write_all, which resolves fd 1/2 via
//     GetStdHandle -- a Win32 standard handle that _dup2 does not touch.
//
// Redirect only with _dup2 and the fallback writes to the real stdout while
// the test reads an empty pipe. Every other dup2 test in this file asserts on
// the conduit path, which is why this only bites here.
//
// Nothing is lost in the real scenario: when a shell or CreateProcess
// redirects the child, the Win32 standard handle IS the pipe.
typedef struct {
    int   saved_fd;
#ifdef _WIN32
    void *saved_handle;
#endif
} test_stdout_redirect_t;

// True when the pipe already holds bytes. Peeks without consuming, so a later
// read still sees everything.
static bool
pipe_has_bytes(int read_fd)
{
#ifdef _WIN32
    HANDLE h     = (HANDLE)_get_osfhandle(read_fd);
    DWORD  avail = 0;
    return h != INVALID_HANDLE_VALUE
        && PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)
        && avail > 0;
#else
    struct pollfd pfd = {.fd = read_fd, .events = POLLIN};
    return poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN);
#endif
}

static test_stdout_redirect_t
test_redirect_stdout(int to_fd)
{
    test_stdout_redirect_t r = {0};

    r.saved_fd = test_fd_dup(1);
    assert(r.saved_fd >= 0);
    assert(test_fd_dup2(to_fd, 1) >= 0);

#ifdef _WIN32
    r.saved_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE h       = (HANDLE)_get_osfhandle(to_fd);
    assert(h != INVALID_HANDLE_VALUE);
    assert(SetStdHandle(STD_OUTPUT_HANDLE, h));
#endif

    return r;
}

static void
test_restore_stdout(test_stdout_redirect_t *r)
{
#ifdef _WIN32
    SetStdHandle(STD_OUTPUT_HANDLE, r->saved_handle);
#endif
    test_fd_dup2(r->saved_fd, 1);
    test_fd_close(r->saved_fd);
}

static test_pipe_t
make_test_pipe(void)
{
    test_pipe_t tp = {0};
    int fds[2];
    int rc = test_pipe_create(fds);
    assert(rc == 0);

    tp.read_fd  = fds[0];
    tp.write_fd = fds[1];

    // Create a conduit topic + fd_writer for the pipe write end.
    n00b_runtime_t *rt = n00b_get_runtime();
    assert(rt && rt->default_conduit);

    auto io_opt = n00b_conduit_default_backend(rt->default_conduit);
    assert(n00b_option_is_set(io_opt));

    auto manage_r = n00b_conduit_fd_manage(rt->default_conduit,
                                           n00b_option_get(io_opt),
                                           tp.write_fd,
                                           false);
    assert(n00b_result_is_ok(manage_r));

    uint64_t id = n00b_atomic_add(&test_pipe_id, 1);
    n00b_conduit_uri_t uri = N00B_CONDUIT_URI_FD_WRITE(1000 + id);

    tp.topic = n00b_conduit_topic_init(
        n00b_buffer_t *, rt->default_conduit, uri);
    assert(tp.topic != nullptr);

    auto writer_r = n00b_conduit_fd_writer_new(rt->default_conduit,
                                               tp.topic,
                                               tp.write_fd);
    assert(n00b_result_is_ok(writer_r));

    return tp;
}

// Read bytes written by the synchronous fd_writer after the test closes the
// pipe write end. POSIX keeps the old poll/nonblocking path; Windows CRT pipes
// are read until EOF.
static int
read_pipe(int fd, char *buf, int max_len)
{
#ifdef _WIN32
    int total = 0;
    while (total < max_len) {
        int n = test_fd_read(fd, buf + total, max_len - total);
        if (n <= 0) break;
        total += n;
    }

    buf[total] = '\0';
    return total;
#else
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int rc = poll(&pfd, 1, 2000);  // 2s timeout
    if (rc <= 0) {
        buf[0] = '\0';
        return 0;
    }

    // Set non-blocking to avoid stalling on the second read.
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int total = 0;
    while (total < max_len) {
        int n = (int)read(fd, buf + total, (size_t)(max_len - total));
        if (n <= 0) break;
        total += n;
    }

    fcntl(fd, F_SETFL, flags);
    buf[total] = '\0';
    return total;
#endif
}

// ============================================================================
// 1. n00b_to_string with nullptr
// ============================================================================

static void
test_to_string_null(void)
{
    n00b_string_t *s = n00b_to_string(nullptr);
    assert(s->u8_bytes == 6);
    assert(memcmp(s->data, "(null)", 6) == 0);

    printf("  [PASS] to_string null\n");
}

// ============================================================================
// 2. n00b_to_string with int64_t
// ============================================================================

static void
test_to_string_int64(void)
{
    int64_t *p = n00b_alloc(int64_t);
    *p = 42;

    n00b_string_t *s = n00b_to_string(p);
    assert(s->u8_bytes == 2);
    assert(memcmp(s->data, "42", 2) == 0);

    printf("  [PASS] to_string int64\n");
}

// ============================================================================
// 3. n00b_to_string with uint64_t
// ============================================================================

static void
test_to_string_uint64(void)
{
    uint64_t *p = n00b_alloc(uint64_t);
    *p = 12345;

    n00b_string_t *s = n00b_to_string(p);
    assert(s->u8_bytes > 0);
    assert(s->data != nullptr);

    printf("  [PASS] to_string uint64\n");
}

// ============================================================================
// 4. n00b_to_string with bool
// ============================================================================

static void
test_to_string_bool(void)
{
    bool *p = n00b_alloc(bool);
    *p = true;

    n00b_string_t *s = n00b_to_string(p);
    assert(s->u8_bytes > 0);

    printf("  [PASS] to_string bool\n");
}

// ============================================================================
// 5. n00b_to_string with double
// ============================================================================

static void
test_to_string_double(void)
{
    double *p = n00b_alloc(double);
    *p = 3.14;

    n00b_string_t *s = n00b_to_string(p);
    assert(s->u8_bytes > 0);

    printf("  [PASS] to_string double\n");
}

// ============================================================================
// 6. n00b_to_string with n00b_string_t (identity)
// ============================================================================

static void
test_to_string_string(void)
{
    n00b_string_t *p = n00b_string_from_raw("hello", 5);

    n00b_string_t *s = n00b_to_string(p);
    assert(s->u8_bytes == 5);
    assert(memcmp(s->data, "hello", 5) == 0);

    printf("  [PASS] to_string string\n");
}

// ============================================================================
// 7. n00b_to_string fallback (unregistered type)
// ============================================================================

static void
test_to_string_fallback(void)
{
    // n00b_dict_untyped_t is registered but has no TO_STRING entry.
    n00b_dict_untyped_t *d = n00b_alloc(n00b_dict_untyped_t);
    n00b_string_t *s = n00b_to_string(d);

    // Should produce something like "<unknown@0x...>"
    assert(s->u8_bytes > 0);
    assert(s->data[0] == '<');

    printf("  [PASS] to_string fallback\n");
}

// ============================================================================
// 8. n00b_print basic (string to pipe via .topic)
// ============================================================================

static void
test_print_basic(void)
{
    test_pipe_t tp = make_test_pipe();

    n00b_string_t *msg = n00b_string_from_raw("hello", 5);

    n00b_print(msg, .topic = tp.topic);

    // Close write end to signal EOF, then read.
    test_fd_close(tp.write_fd);

    char buf[256];
    int n = read_pipe(tp.read_fd, buf, 255);

    assert(n == 6);
    assert(memcmp(buf, "hello\n", 6) == 0);

    test_fd_close(tp.read_fd);
    printf("  [PASS] print basic\n");
}

// ============================================================================
// 9. n00b_print with custom end
// ============================================================================

static void
test_print_custom_end(void)
{
    test_pipe_t tp = make_test_pipe();

    n00b_string_t *msg = n00b_string_from_raw("world", 5);

    n00b_string_t *end_str = n00b_string_from_raw("!\n", 2);

    n00b_print(msg, .topic = tp.topic, .end = n00b_option_set(n00b_string_t *, end_str));

    test_fd_close(tp.write_fd);

    char buf[256];
    int n = read_pipe(tp.read_fd, buf, 255);

    assert(n == 7);
    assert(memcmp(buf, "world!\n", 7) == 0);

    test_fd_close(tp.read_fd);
    printf("  [PASS] print custom end\n");
}

// ============================================================================
// 10. n00b_print with int64_t
// ============================================================================

static void
test_print_int64(void)
{
    test_pipe_t tp = make_test_pipe();

    int64_t *val = n00b_alloc(int64_t);
    *val = -99;

    n00b_print(val, .topic = tp.topic);

    test_fd_close(tp.write_fd);

    char buf[256];
    int n = read_pipe(tp.read_fd, buf, 255);

    assert(n > 0);
    assert(memcmp(buf, "-99\n", 4) == 0);

    test_fd_close(tp.read_fd);
    printf("  [PASS] print int64\n");
}

// ============================================================================
// 11. n00b_print with nullptr
// ============================================================================

static void
test_print_null(void)
{
    test_pipe_t tp = make_test_pipe();

    n00b_print(nullptr, .topic = tp.topic);

    test_fd_close(tp.write_fd);

    char buf[256];
    int n = read_pipe(tp.read_fd, buf, 255);

    assert(n == 7);
    assert(memcmp(buf, "(null)\n", 7) == 0);

    test_fd_close(tp.read_fd);
    printf("  [PASS] print null\n");
}

// ============================================================================
// 12. Runtime stdout topic is non-null
// ============================================================================

static void
test_stdout_topic(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    assert(rt != nullptr);
    assert(rt->stdout_topic != nullptr);

    // Second access should return the same pointer.
    n00b_conduit_topic_base_t *first = rt->stdout_topic;
    assert(rt->stdout_topic == first);

    printf("  [PASS] stdout topic\n");
}

// ============================================================================
// 13. Runtime stderr topic is non-null
// ============================================================================

static void
test_stderr_topic(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    assert(rt != nullptr);
    assert(rt->stderr_topic != nullptr);

    printf("  [PASS] stderr topic\n");
}

// ============================================================================
// 14. n00b_printf basic (format + print to pipe via .topic)
// ============================================================================

static void
test_printf_basic(void)
{
    test_pipe_t tp = make_test_pipe();

    n00b_string_t *name = n00b_string_from_raw("World", 5);

    n00b_printf("Hello [|#|]!", name, .topic = tp.topic);

    test_fd_close(tp.write_fd);

    char buf[256];
    int n = read_pipe(tp.read_fd, buf, 255);

    assert(n == 13);
    assert(memcmp(buf, "Hello World!\n", 13) == 0);

    test_fd_close(tp.read_fd);
    printf("  [PASS] printf basic\n");
}

// ============================================================================
// 15. n00b_printf with no trailing newline
// ============================================================================

static void
test_printf_no_newline(void)
{
    test_pipe_t tp = make_test_pipe();

    n00b_string_t *empty = n00b_string_from_raw("", 0);

    n00b_printf("ok", .topic = tp.topic, .end = n00b_option_set(n00b_string_t *, empty));

    test_fd_close(tp.write_fd);

    char buf[256];
    int n = read_pipe(tp.read_fd, buf, 255);

    assert(n == 2);
    assert(memcmp(buf, "ok", 2) == 0);

    test_fd_close(tp.read_fd);
    printf("  [PASS] printf no newline\n");
}

// ============================================================================
// 16. n00b_printf to pipe via .topic
// ============================================================================

static void
test_printf_topic(void)
{
    test_pipe_t tp = make_test_pipe();

    n00b_printf("err", .topic = tp.topic);

    test_fd_close(tp.write_fd);

    char buf[256];
    int n = read_pipe(tp.read_fd, buf, 255);

    assert(n == 4);
    assert(memcmp(buf, "err\n", 4) == 0);

    test_fd_close(tp.read_fd);
    printf("  [PASS] printf topic\n");
}

// ============================================================================
// 17. n00b_eprintf macro (writes to fd 2 via macro, redirected by dup2)
//
// ============================================================================
// 18. n00b#490: a print survives a contended publisher claim.
//
// n00b_write pins .timeout_ms = 0, which selects publish_TRY_claim. When
// another thread holds the stdout topic's publisher, that try fails with
// ALREADY_CLAIMED and the write returns Err having delivered nothing. print
// used to discard that result, so the whole line vanished -- no error, no
// partial output. Observed as an intermittent Windows CI flake where a help
// line or one of two consecutive eprintfs simply did not arrive.
//
// The claim is re-entrant for the claiming thread, so the contention has to
// come from a second thread.
// ============================================================================

static _Atomic(int) claim_held   = 0;
static _Atomic(int) claim_release = 0;

static void *
stdout_claim_holder(void *arg)
{
    n00b_conduit_topic_base_t *topic = arg;

    auto pub_r = n00b_conduit_publish_claim(topic);
    if (n00b_result_is_err(pub_r)) {
        atomic_store(&claim_held, -1);
        return nullptr;
    }
    n00b_conduit_publisher_t *pub = n00b_result_get(pub_r);

    atomic_store(&claim_held, 1);
    while (atomic_load(&claim_release) == 0) {
        base_nanosleep_ns(1000ULL * 1000); // 1ms
    }

    n00b_conduit_publish_yield(pub);
    return nullptr;
}

static void
test_print_survives_contended_publisher(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    assert(rt && rt->stdout_topic);

    int fds[2];
    assert(test_pipe_create(fds) == 0);

    atomic_store(&claim_held, 0);
    atomic_store(&claim_release, 0);

    auto tr = n00b_thread_spawn(stdout_claim_holder, rt->stdout_topic);
    assert(n00b_result_is_ok(tr));
    n00b_thread_t *holder = n00b_result_get(tr);

    // Wait for the other thread to actually own the publisher, so the
    // print below is guaranteed to hit the contended path rather than
    // racing it.
    while (atomic_load(&claim_held) == 0) {
        base_nanosleep_ns(1000ULL * 1000);
    }
    assert(atomic_load(&claim_held) == 1);

    // Point fd 1 at the pipe for the duration of the print. The fallback
    // writes to the descriptor, so this is what captures it.
    test_stdout_redirect_t redir = test_redirect_stdout(fds[1]);

    n00b_printf("contended-«#»", 490);

    // Restore before asserting, so a failure can still report.
    test_restore_stdout(&redir);

    atomic_store(&claim_release, 1);
    n00b_thread_join(holder);

    test_fd_close(fds[1]);

    char buf[256];
    int n = read_pipe(fds[0], buf, 255);
    test_fd_close(fds[0]);

    // Before #490 this read 0 bytes: the line was dropped on the floor.
    assert(n > 0);
    assert(strstr(buf, "contended-490") != nullptr);

    printf("  [PASS] print survives a contended publisher claim\n");
}

// ============================================================================
// 19. n00b#490: a BRIEFLY contended print still goes out over the topic.
//
// Test 18 above holds the claim for the whole print, so every attempt fails
// and the fd fallback is what saves the line -- which means deleting the
// retry loop entirely would not fail it. This case covers the other half:
// the holder lets go while the retry is still running, so the write should
// succeed through the CONDUIT, not the descriptor.
//
// Asserting on a subscriber rather than on fd 1 is the point. The fallback
// bypasses the topic, so a subscriber (a tee, a log capture) sees the bytes
// only if the retry actually won. Reverting the backoff to a bare spin makes
// this fail while test 18 keeps passing.
// ============================================================================

static _Atomic(int) brief_held = 0;

static void *
brief_claim_holder(void *arg)
{
    n00b_conduit_topic_base_t *topic = arg;

    auto pub_r = n00b_conduit_publish_claim(topic);
    if (n00b_result_is_err(pub_r)) {
        atomic_store(&brief_held, -1);
        return nullptr;
    }
    n00b_conduit_publisher_t *pub = n00b_result_get(pub_r);
    atomic_store(&brief_held, 1);

    // Shorter than the retry budget (50us doubling over 5 attempts, ~1.5ms
    // total), long enough that the claim is genuinely held when the print
    // starts.
    base_nanosleep_ns(200ULL * 1000); // 200us

    n00b_conduit_publish_yield(pub);
    return nullptr;
}

static void
test_print_retry_keeps_the_topic_path(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    assert(rt && rt->stdout_topic);

    // Subscribe to the stdout topic: this is what the fd fallback would
    // bypass, so receiving here proves the retry path carried the line.
    n00b_conduit_inbox_t(n00b_buffer_t *) inbox;
    n00b_conduit_inbox_init(n00b_buffer_t *, &inbox, rt->default_conduit,
                            N00B_CONDUIT_BP_UNBOUNDED, 8);
    n00b_conduit_sub_handle_t sub = n00b_conduit_subscribe(
        n00b_buffer_t *,
        (n00b_conduit_topic_t(n00b_buffer_t *) *)rt->stdout_topic,
        &inbox);
    assert(sub != N00B_CONDUIT_INVALID_SUB_HANDLE);

    atomic_store(&brief_held, 0);
    auto tr = n00b_thread_spawn(brief_claim_holder, rt->stdout_topic);
    assert(n00b_result_is_ok(tr));
    n00b_thread_t *holder = n00b_result_get(tr);

    while (atomic_load(&brief_held) == 0) {
        base_nanosleep_ns(100ULL * 1000);
    }
    assert(atomic_load(&brief_held) == 1);

    // Contended at entry; the holder releases mid-retry.
    n00b_printf("retried-«#»", 490);

    n00b_thread_join(holder);

    bool saw = false;
    while (n00b_conduit_inbox_has_msg(n00b_buffer_t *, &inbox)) {
        auto m = n00b_conduit_inbox_pop_msg(n00b_buffer_t *, &inbox);
        if (!m) {
            break;
        }
        int64_t len  = 0;
        char   *data = n00b_buffer_to_c(m->payload, &len);
        if (data && len > 0 && strstr(data, "retried-490") != nullptr) {
            saw = true;
        }
    }

    n00b_conduit_sub_cancel(sub);

    // Fails if the retry never wins -- the line would have gone to fd 1
    // through the fallback, which no subscriber observes.
    assert(saw);

    printf("  [PASS] print retry keeps the topic path\n");
}

// ============================================================================
// 20. n00b#490 (second site): the line survives a failed MANAGED write.
//
// Tests 18 and 19 both make the TOPIC write fail, which is the only failure
// print.c can see. This covers the one it cannot: the topic write succeeds --
// published, delivered to the fd_writer sink -- and the managed write fails
// afterwards, inside the sink. Before this fix fd_writer_transform discarded
// that result with a cast to void, so print saw Ok, never reached its
// fallback, and the line was gone.
//
// That ordering is why the drop survived #491: #491 hardened the layer that
// was already reporting success.
//
// The failure is injected. A managed write that completes with an error needs
// the descriptor itself to fail, and then the fallback's own write fails too.
// ============================================================================

static void
test_print_survives_failed_managed_write(void)
{
    int fds[2];
    assert(test_pipe_create(fds) == 0);

    test_stdout_redirect_t redir = test_redirect_stdout(fds[1]);

    n00b_conduit_fd_writer_force_next_failure();
    n00b_printf("sunk-«#»", 490);

    // A sync print returns only after the sink has written its line, so the
    // fallback's bytes are in the pipe before the redirect is undone.
    bool written_before_return = pipe_has_bytes(fds[0]);

    test_restore_stdout(&redir);
    if (!written_before_return) {
        fprintf(stderr, "n00b_printf returned before its line was written\n");
    }
    assert(written_before_return);
    test_fd_close(fds[1]);

    char buf[256];
    int n = read_pipe(fds[0], buf, 255);
    test_fd_close(fds[0]);

    // Without the sink's fallback this reads 0 bytes.
    assert(n > 0);
    assert(strstr(buf, "sunk-490") != nullptr);

    printf("  [PASS] print survives a failed managed write\n");
}

// ============================================================================
// 21. Every topic gets its own done-topic.
//
// A sync write returns on the first message its topic's done-topic delivers,
// so a done-topic shared with another topic lets that topic's completions and
// closes release the write before its own line is written.
// ============================================================================

static void
test_done_topics_are_not_shared(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    assert(rt && rt->stdout_topic && rt->stderr_topic);
    n00b_conduit_t *c = rt->default_conduit;

    // One topic from each place that creates done-topics: the runtime's print
    // topics, the managed std owners and a fresh managed fd (fd_managed.c, one
    // per payload type), this file's own topic_init, a timer, and a topic that
    // gets its done-topic from n00b_conduit_topic_ensure_done.
    n00b_conduit_topic_base_t *topics[24];
    int                        n = 0;

    topics[n++] = rt->stdout_topic;
    topics[n++] = rt->stderr_topic;

    test_pipe_t              tp = make_test_pipe();
    n00b_conduit_fd_owner_t *owners[4] = {
        rt->stdin_owner, rt->stdout_owner, rt->stderr_owner,
        n00b_option_get(n00b_conduit_fd_get_owner(c, tp.write_fd))};
    for (int i = 0; i < 4; i++) {
        if (owners[i]) {
            topics[n++] = owners[i]->read_topic;
            topics[n++] = owners[i]->write_topic;
            topics[n++] = owners[i]->status_topic;
            topics[n++] = owners[i]->wreq_topic;
        }
    }
    topics[n++] = (n00b_conduit_topic_base_t *)tp.topic;

    auto timer_r = n00b_conduit_timer_once(c, 60 * 1000);
    assert(n00b_result_is_ok(timer_r));
    topics[n++] = n00b_result_get(timer_r);

    auto plain_r = n00b_conduit_topic_get(c, N00B_CONDUIT_URI_USER_EVENT(0x7e57d0e),
                                          sizeof(n00b_conduit_topic_base_t));
    assert(n00b_result_is_ok(plain_r));
    n00b_conduit_topic_base_t *plain = n00b_result_get(plain_r);
    assert(n00b_conduit_topic_ensure_done(plain) != nullptr);
    topics[n++] = plain;

    for (int i = 0; i < n; i++) {
        void *di = n00b_atomic_load(&topics[i]->done_topic);
        assert(di != nullptr);
        for (int j = i + 1; j < n; j++) {
            if (di == n00b_atomic_load(&topics[j]->done_topic)) {
                fprintf(stderr, "topics %d and %d share a done-topic\n", i, j);
                assert(false);
            }
        }
    }

    n00b_conduit_timer_cancel(topics[n - 2]);
    test_fd_close(tp.write_fd);
    test_fd_close(tp.read_fd);

    printf("  [PASS] done topics are not shared\n");
}
// 22. A managed write whose completion wait expires is written once.
//
// The expired request is still queued and the owner keeps writing it, so the
// sink must not also write it directly. Holding the owner's writes keeps the
// request queued through the whole ~5s wait. n00b_conduit_fd_owner_flush
// then writes it with no later print to drive the queue.
// ============================================================================

static void
test_print_timed_out_write_is_written_once(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    assert(rt && rt->stdout_owner);

    int fds[2];
    assert(test_pipe_create(fds) == 0);

    test_stdout_redirect_t redir = test_redirect_stdout(fds[1]);

    int64_t t0 = n00b_ns_timestamp();
    n00b_conduit_fd_owner_hold_writes(true);
    n00b_printf("queued-«#»", 1);
    n00b_conduit_fd_owner_hold_writes(false);
    int64_t t1 = n00b_ns_timestamp();

    bool flushed = n00b_conduit_fd_owner_flush(rt->stdout_owner, 30000);

    n00b_printf("after-«#»", 2);
    int64_t t2 = n00b_ns_timestamp();

    test_restore_stdout(&redir);
    test_fd_close(fds[1]);

    char buf[256];
    int  got = read_pipe(fds[0], buf, 255);
    test_fd_close(fds[0]);

    if (!flushed || strcmp(buf, "queued-1\nafter-2\n") != 0) {
        // stderr, not stdout: under meson stdout is a block-buffered pipe, and
        // the abort below would take this line with it.
        fprintf(stderr, "  [FAIL] timed-out write: flushed %d, %d bytes \"",
                flushed, got);
        for (int i = 0; i < got; i++) {
            if (buf[i] == '\n') {
                fputs("\\n", stderr);
            }
            else {
                fputc(buf[i], stderr);
            }
        }
        fprintf(stderr, "\" (queued print %lld ms, flush and after print %lld ms)\n",
                (long long)((t1 - t0) / N00B_NS_PER_MS),
                (long long)((t2 - t1) / N00B_NS_PER_MS));
        assert(false);
    }

    printf("  [PASS] a timed-out managed write is written once\n");
}

// ============================================================================
// 23. print's fd fallback waits out a full non-blocking pipe.
//
// n00b_conduit_fd_manage leaves stdout non-blocking, and the fallback runs
// when another thread holds the stdout publisher, which is when a slow
// consumer has the pipe full. The fallback's write then fails with EAGAIN.
// The stall hook drains the pipe at the moment the fallback starts waiting.
// ============================================================================

#ifndef _WIN32
static int    stall_drain_fd = -1;
static size_t stall_drained  = 0;

static void
drain_on_stall(int fd)
{
    (void)fd;

    char    chunk[4096];
    ssize_t n;
    while ((n = read(stall_drain_fd, chunk, sizeof chunk)) > 0) {
        stall_drained += (size_t)n;
    }
}

static void
test_print_fallback_waits_for_a_full_pipe(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    assert(rt && rt->stdout_topic);

    int fds[2];
    assert(test_pipe_create(fds) == 0);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL) | O_NONBLOCK);

    char fill[4096];
    memset(fill, 'x', sizeof fill);
    size_t filled = 0;
    ssize_t w;
    while ((w = write(fds[1], fill, sizeof fill)) > 0) {
        filled += (size_t)w;
    }
    while (write(fds[1], fill, 1) == 1) {
        filled++;
    }

    atomic_store(&claim_held, 0);
    atomic_store(&claim_release, 0);

    auto tr = n00b_thread_spawn(stdout_claim_holder, rt->stdout_topic);
    assert(n00b_result_is_ok(tr));
    n00b_thread_t *holder = n00b_result_get(tr);

    while (atomic_load(&claim_held) == 0) {
        base_nanosleep_ns(1000ULL * 1000);
    }
    assert(atomic_load(&claim_held) == 1);

    test_stdout_redirect_t redir = test_redirect_stdout(fds[1]);

    stall_drain_fd            = fds[0];
    stall_drained             = 0;
    n00b_raw_write_stall_hook = drain_on_stall;

    n00b_printf("full-pipe-«#»", 491);

    n00b_raw_write_stall_hook = nullptr;
    test_restore_stdout(&redir);

    atomic_store(&claim_release, 1);
    n00b_thread_join(holder);

    test_fd_close(fds[1]);

    // Whatever the hook did not drain: the line, or the filler it is missing
    // from.
    static char rest[1 << 18];
    size_t      got = (size_t)read_pipe(fds[0], rest, (int)sizeof rest - 1);
    test_fd_close(fds[0]);

    if (stall_drained != filled || strcmp(rest, "full-pipe-491\n") != 0) {
        printf("  [FAIL] full pipe: filled %zu, drained %zu, %zu bytes left,"
               " ending \"%s\"\n",
               filled, stall_drained, got,
               got > 16 ? rest + got - 16 : rest);
        assert(false);
    }

    printf("  [PASS] print fallback waits for a full pipe\n");
}

// ============================================================================
// 24. The fallback's wait survives collections.
//
// Each collection signals every thread on Linux, and the signal interrupts
// ppoll. A helper runs one collection each time the printing thread is parked
// in ppoll, one more time than n00b_raw_write_all may wait, then drains the
// pipe. The line must still go out.
// ============================================================================

#ifdef __linux__
static pid_t        stw_print_tid   = 0;
static int          stw_drain_fd    = -1;
static size_t       stw_filled      = 0;
static _Atomic(int) stw_print_done  = 0;
static _Atomic(int) stw_collections = 0;

#define STW_COLLECTIONS (N00B_RAW_WRITE_MAX_WAITS + 1)

// Runs on an n00b worker, a raw clone thread with no full libc TCB, so it
// sticks to raw syscalls and does its own formatting and parsing.
static bool
thread_in_ppoll(pid_t tid)
{
    char path[64] = "/proc/self/task/";
    char digits[16];
    int  nd  = 0;
    int  len = (int)strlen(path);
    for (unsigned v = (unsigned)tid; v != 0 || nd == 0; v /= 10) {
        digits[nd++] = (char)('0' + v % 10);
    }
    while (nd > 0) {
        path[len++] = digits[--nd];
    }
    memcpy(path + len, "/syscall", sizeof "/syscall");

    long fd = _n00b_raw_linux_syscall4(SYS_openat, AT_FDCWD,
                                       (long)(uintptr_t)path, O_RDONLY, 0);
    assert(fd >= 0);
    char buf[32];
    long n = _n00b_raw_linux_syscall3(SYS_read, fd, (long)(uintptr_t)buf,
                                      (long)sizeof buf);
    _n00b_raw_linux_syscall1(SYS_close, fd);

    // "running" while on a CPU; the syscall number while blocked in one.
    long nr = 0;
    long i  = 0;
    for (; i < n && buf[i] >= '0' && buf[i] <= '9'; i++) {
        nr = nr * 10 + (buf[i] - '0');
    }
    return i > 0 && nr == SYS_ppoll;
}

static void *
stw_interrupter(void *arg)
{
    (void)arg;

    for (int i = 0; i < STW_COLLECTIONS; i++) {
        while (!thread_in_ppoll(stw_print_tid)) {
            if (atomic_load(&stw_print_done)) {
                goto drain;
            }
            base_nanosleep_ns(100ULL * 1000);
        }
        n00b_stop_the_world();
        n00b_restart_the_world();
        atomic_fetch_add(&stw_collections, 1);
    }

drain:;
    char   chunk[4096];
    size_t got = 0;
    while (got < stw_filled) {
        size_t want = stw_filled - got;
        long   n    = _n00b_raw_linux_syscall3(
            SYS_read, stw_drain_fd, (long)(uintptr_t)chunk,
            (long)(want < sizeof chunk ? want : sizeof chunk));
        if (n > 0) {
            got += (size_t)n;
        }
    }
    return nullptr;
}

static void
test_print_fallback_wait_survives_collections(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    assert(rt && rt->stdout_topic);

    int fds[2];
    assert(test_pipe_create(fds) == 0);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL) | O_NONBLOCK);

    char fill[4096];
    memset(fill, 'x', sizeof fill);
    size_t  filled = 0;
    ssize_t w;
    while ((w = write(fds[1], fill, sizeof fill)) > 0) {
        filled += (size_t)w;
    }
    while (write(fds[1], fill, 1) == 1) {
        filled++;
    }

    atomic_store(&claim_held, 0);
    atomic_store(&claim_release, 0);

    auto hr = n00b_thread_spawn(stdout_claim_holder, rt->stdout_topic);
    assert(n00b_result_is_ok(hr));
    n00b_thread_t *holder = n00b_result_get(hr);
    while (atomic_load(&claim_held) == 0) {
        base_nanosleep_ns(1000ULL * 1000);
    }
    assert(atomic_load(&claim_held) == 1);

    test_stdout_redirect_t redir = test_redirect_stdout(fds[1]);

    stw_print_tid = (pid_t)syscall(SYS_gettid);
    stw_drain_fd  = fds[0];
    stw_filled    = filled;
    atomic_store(&stw_print_done, 0);
    atomic_store(&stw_collections, 0);

    auto ir = n00b_thread_spawn(stw_interrupter, nullptr);
    assert(n00b_result_is_ok(ir));
    n00b_thread_t *interrupter = n00b_result_get(ir);

    n00b_printf("collected-«#»", 503);
    atomic_store(&stw_print_done, 1);

    test_restore_stdout(&redir);
    atomic_store(&claim_release, 1);
    n00b_thread_join(holder);
    n00b_thread_join(interrupter);

    test_fd_close(fds[1]);

    char buf[256];
    read_pipe(fds[0], buf, 255);
    test_fd_close(fds[0]);

    int collections = atomic_load(&stw_collections);
    if (collections != STW_COLLECTIONS
        || strcmp(buf, "collected-503\n") != 0) {
        printf("  [FAIL] collections: %d of %d, got \"%s\"\n",
               collections, STW_COLLECTIONS, buf);
        assert(false);
    }

    printf("  [PASS] print fallback wait survives collections\n");
}
#endif

// ============================================================================
// 25. A negative timeout waits for writability with no bound.
// ============================================================================

static void
test_raw_wait_writable_without_timeout(void)
{
    int fds[2];
    assert(test_pipe_create(fds) == 0);

    bool writable = _n00b_raw_wait_writable(fds[1], -1);

    test_fd_close(fds[0]);
    test_fd_close(fds[1]);

    if (!writable) {
        printf("  [FAIL] empty pipe not writable with timeout -1\n");
        assert(false);
    }

    printf("  [PASS] raw wait without a timeout\n");
}
#endif

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running print tests...\n");

    // n00b_to_string tests
    test_to_string_null();
    test_to_string_int64();
    test_to_string_uint64();
    test_to_string_bool();
    test_to_string_double();
    test_to_string_string();
    test_to_string_fallback();

    // n00b_print tests (conduit topic → pipe)
    test_print_basic();
    test_print_custom_end();
    test_print_int64();
    test_print_null();

    // Runtime topic sanity checks
    test_stdout_topic();
    test_stderr_topic();

    // n00b_printf / n00b_eprintf tests
    test_printf_basic();
    test_printf_no_newline();
    test_printf_topic();

    // n00b#490
    test_print_survives_contended_publisher();
    test_print_retry_keeps_the_topic_path();
    test_print_survives_failed_managed_write();
    test_done_topics_are_not_shared();
    test_print_timed_out_write_is_written_once();
#ifndef _WIN32
    test_print_fallback_waits_for_a_full_pipe();
#ifdef __linux__
    test_print_fallback_wait_survives_collections();
#endif
    test_raw_wait_writable_without_timeout();
#endif
    printf("All print tests passed.\n");
    n00b_shutdown();
    return 0;
}
