/* test_eintr_signals.c: calls a signal interrupts must resume, not fail.
 *
 * SA_RESTART, which every n00b signal handler sets, does not restart every
 * call. Each case here installs a handler with SA_RESTART, waits until the
 * thread under test is asleep inside one of the calls it does not restart,
 * signals that thread, and checks that the operation ends as it would with
 * no signal: a DNS lookup still resolves, a plain-HTTP connect still
 * connects, and the metrics and NFS servers still answer.
 *
 * Which calls a signal interrupts depends on the platform, so some cases can
 * only fail on one of them: a blocking TCP connect is interrupted on macOS,
 * and a socket call with a timeout on Linux. Every case runs everywhere it
 * can.
 */

#define N00B_USE_INTERNAL_API
#include <arpa/inet.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#include "n00b.h"
#include "core/runtime.h"
#include "core/buffer.h"
#include "core/string.h"
#include "text/strings/format.h"
#include "conduit/conduit.h"
#include "conduit/io.h"
#include "net/dns.h"
#include "net/http/http_client.h"
#include "net/quic/metrics.h"
#include "internal/net/quic/metrics_internal.h"
#include "vfs/vfs.h"
#include "vfs/frontend_nfs.h"

#ifdef N00B_DEBUG

#define PROBE_SIG     SIGUSR2
#define HANG_GUARD_MS 30000

// ---------------------------------------------------------------------------
// Signalling a thread once it is asleep
// ---------------------------------------------------------------------------

static _Atomic(int32_t) signals_handled;

static void
on_probe_signal(int sig)
{
    (void)sig;
    atomic_fetch_add(&signals_handled, 1);
}

[[n00b::nomap]] static struct sigaction saved_action;

static void
install_probe_handler(void)
{
    struct sigaction sa = {};
    sa.sa_handler       = on_probe_signal;
    sa.sa_flags         = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    assert(sigaction(PROBE_SIG, &sa, &saved_action) == 0);
}

static void
restore_probe_handler(void)
{
    (void)sigaction(PROBE_SIG, &saved_action, nullptr);
}

// A thread to signal. On Linux, `nr` other than -1 also requires the thread
// to be asleep in that system call.
typedef struct {
    pthread_t thread;
    pid_t     tid;
    long      nr;
} target_t;

static uint64_t
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void
nap(void)
{
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
    nanosleep(&ts, nullptr);
}

// The calling thread, as a target for the helpers below.
static target_t
this_thread(void)
{
    target_t t = {.thread = pthread_self(), .nr = -1};
#if defined(__linux__)
    t.tid = (pid_t)syscall(SYS_gettid);
#endif
    return t;
}

static bool
asleep(target_t t)
{
#if defined(__linux__)
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/task/%d/stat", (int)t.tid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    char    buf[512];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return false;
    }
    buf[n]      = '\0';
    char *paren = strrchr(buf, ')');
    if (paren == nullptr || paren[1] != ' ' || paren[2] != 'S') {
        return false;
    }
    if (t.nr < 0) {
        return true;
    }
    snprintf(path, sizeof(path), "/proc/self/task/%d/syscall", (int)t.tid);
    FILE *f = fopen(path, "r");
    if (f == nullptr) {
        return false;
    }
    long nr = -1;
    int  ok = fscanf(f, "%ld", &nr);
    fclose(f);
    return ok == 1 && nr == t.nr;
#elif defined(__APPLE__)
    thread_basic_info_data_t info  = {};
    mach_msg_type_number_t   count = THREAD_BASIC_INFO_COUNT;
    if (thread_info(pthread_mach_thread_np(t.thread),
                    THREAD_BASIC_INFO,
                    (thread_info_t)&info,
                    &count)
        != KERN_SUCCESS) {
        return false;
    }
    return info.run_state == TH_STATE_WAITING;
#else
    (void)t;
    return true;
#endif
}

static void
send_probe(target_t t)
{
#if defined(__linux__)
    assert(syscall(SYS_tgkill, getpid(), t.tid, PROBE_SIG) == 0);
#else
    assert(pthread_kill(t.thread, PROBE_SIG) == 0);
#endif
}

// Waits until `t` is asleep, signals it, and waits until the handler ran.
// Returns false if `stop` was set first.
static bool
signal_when_asleep(target_t t, _Atomic(bool) *stop)
{
    int32_t  before   = atomic_load(&signals_handled);
    uint64_t give_up  = now_ms() + HANG_GUARD_MS;
    while (!asleep(t)) {
        if (stop && atomic_load(stop)) {
            return false;
        }
        assert(now_ms() < give_up);
        nap();
    }
    send_probe(t);
    while (atomic_load(&signals_handled) == before) {
        assert(now_ms() < give_up);
        nap();
    }
    return true;
}

static int
loopback_socket(int type, struct sockaddr_in *sa)
{
    int fd = socket(AF_INET, type, 0);
    assert(fd >= 0);
    *sa = (struct sockaddr_in){
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    socklen_t len = sizeof(*sa);
    assert(bind(fd, (struct sockaddr *)sa, sizeof(*sa)) == 0);
    assert(getsockname(fd, (struct sockaddr *)sa, &len) == 0);
    return fd;
}

static bool
wait_readable(int fd)
{
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    return poll(&pfd, 1, HANG_GUARD_MS) == 1;
}

// ---------------------------------------------------------------------------
// DNS: the wait for a reply
// ---------------------------------------------------------------------------

#define DNS_SIGNALS 6

typedef struct {
    int      fd;
    target_t resolver;
} dns_server_t;

// Answers every query on loopback: A queries with 127.0.0.1, others with no
// records. Before answering the first, it signals the resolving thread
// DNS_SIGNALS times while that thread waits for the reply.
static void *
dns_serve(void *raw)
{
    dns_server_t *dns       = raw;
    bool          signalled = false;
    for (;;) {
        struct pollfd pfd = {.fd = dns->fd, .events = POLLIN};
        if (poll(&pfd, 1, HANG_GUARD_MS) != 1) {
            return nullptr;
        }
        uint8_t                 msg[512];
        struct sockaddr_storage from     = {};
        socklen_t               from_len = sizeof(from);
        ssize_t                 n        = recvfrom(dns->fd,
                                                    msg,
                                                    sizeof(msg) - 16,
                                                    0,
                                                    (struct sockaddr *)&from,
                                                    &from_len);
        if (n == 1 && msg[0] == 0) {
            return nullptr;
        }
        if (n < 12) {
            continue;
        }
        if (!signalled) {
            for (int i = 0; i < DNS_SIGNALS; i++) {
                signal_when_asleep(dns->resolver, nullptr);
            }
            signalled = true;
        }
        size_t pos = 12;
        while (pos < (size_t)n && msg[pos] != 0) {
            pos += (size_t)msg[pos] + 1;
        }
        if (pos + 5 > (size_t)n) {
            continue;
        }
        bool   is_a = msg[pos + 1] == 0 && msg[pos + 2] == 1;
        size_t len  = pos + 5;
        msg[2]      = 0x81;
        msg[3]      = 0x80;
        msg[6]      = 0;
        msg[7]      = is_a ? 1 : 0;
        if (is_a) {
            static const uint8_t answer[16] = {
                0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 127, 0, 0, 1};
            memcpy(msg + len, answer, sizeof(answer));
            len += sizeof(answer);
        }
        (void)sendto(dns->fd, msg, len, 0, (struct sockaddr *)&from, from_len);
    }
}

// The resolver makes three attempts per record type. Signals during the
// wait for a reply must not use them up.
static void
test_dns_reply_wait_survives_signals(void)
{
    struct sockaddr_in sa;
    dns_server_t       dns = {.fd       = loopback_socket(SOCK_DGRAM, &sa),
                              .resolver = this_thread()};
    memcpy(&n00b_dns_test_nameserver, &sa, sizeof(sa));
    n00b_dns_test_nameserver_len = sizeof(sa);

    pthread_t server;
    assert(pthread_create(&server, nullptr, dns_serve, &dns) == 0);

    n00b_resolved_addr_t out[4] = {};
    int got = n00b_dns_resolve_addrs(n00b_string_from_cstr("eintr-probe.test"),
                                     80,
                                     out,
                                     4);

    if (got < 1) {
        fprintf(stderr,
                "dns: lookup failed after %d signals during the reply wait\n",
                (int)atomic_load(&signals_handled));
    }
    assert(got >= 1);
    assert(out[0].ss.ss_family == AF_INET);
    assert(((struct sockaddr_in *)&out[0].ss)->sin_addr.s_addr
           == htonl(INADDR_LOOPBACK));

    static const uint8_t quit = 0;
    (void)sendto(dns.fd, &quit, 1, 0, (struct sockaddr *)&sa, sizeof(sa));
    pthread_join(server, nullptr);
    close(dns.fd);
    n00b_dns_test_nameserver_len = 0;
    printf("  [PASS] dns reply wait survives %d signals\n", DNS_SIGNALS);
}

// ---------------------------------------------------------------------------
// Plain HTTP: a blocking connect
// ---------------------------------------------------------------------------

// A loopback listener whose accept queue is full, so a connect to it sleeps
// until something accepts. The fillers are the connections that fill it.
#define MAX_FILLERS 16

typedef struct {
    int                lfd;
    struct sockaddr_in sa;
    int                fillers[MAX_FILLERS];
    int                nfillers;
} full_listener_t;

static void
full_listener_open(full_listener_t *fl)
{
    *fl     = (full_listener_t){};
    fl->lfd = loopback_socket(SOCK_STREAM, &fl->sa);
    assert(listen(fl->lfd, 1) == 0);
    // Connect until one does not complete: that one is stuck behind the
    // full queue, and so is the next.
    for (;;) {
        assert(fl->nfillers < MAX_FILLERS);
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        fl->fillers[fl->nfillers++] = fd;
        int rc = connect(fd, (struct sockaddr *)&fl->sa, sizeof(fl->sa));
        if (rc != 0 && errno != EINPROGRESS) {
            break;
        }
        struct pollfd pfd = {.fd = fd, .events = POLLOUT};
        if (rc != 0 && poll(&pfd, 1, 200) == 0) {
            break;
        }
    }
}

static void
full_listener_close(full_listener_t *fl)
{
    for (int i = 0; i < fl->nfillers; i++) {
        if (fl->fillers[i] >= 0) {
            close(fl->fillers[i]);
        }
    }
    close(fl->lfd);
}

static bool
is_filler(full_listener_t *fl, struct sockaddr_in *peer)
{
    for (int i = 0; i < fl->nfillers; i++) {
        struct sockaddr_in local = {};
        socklen_t          len   = sizeof(local);
        if (fl->fillers[i] >= 0
            && getsockname(fl->fillers[i], (struct sockaddr *)&local, &len) == 0
            && local.sin_port == peer->sin_port) {
            return true;
        }
    }
    return false;
}

#define CONNECT_SIGNALS 8

typedef struct {
    full_listener_t listener;
    target_t        client;
    _Atomic(bool)   done;
} connect_server_t;

// Signals the client CONNECT_SIGNALS times while it connects, then makes
// room in the queue and answers its request.
static void *
connect_serve(void *raw)
{
    connect_server_t *cs = raw;
    full_listener_t  *fl = &cs->listener;
    for (int i = 0; i < CONNECT_SIGNALS; i++) {
        if (!signal_when_asleep(cs->client, &cs->done)) {
            return nullptr;
        }
    }
    // Abort the fillers still connecting, then take the ones queued, so the
    // client's next SYN gets in.
    for (int i = 0; i < fl->nfillers; i++) {
        struct pollfd pfd = {.fd = fl->fillers[i], .events = POLLOUT};
        if (poll(&pfd, 1, 0) == 0) {
            close(fl->fillers[i]);
            fl->fillers[i] = -1;
        }
    }
    for (;;) {
        if (!wait_readable(fl->lfd)) {
            return nullptr;
        }
        struct sockaddr_in peer = {};
        socklen_t          len  = sizeof(peer);
        int fd = accept(fl->lfd, (struct sockaddr *)&peer, &len);
        if (fd < 0) {
            continue;
        }
        if (is_filler(fl, &peer)) {
            close(fd);
            continue;
        }
        char   req[2048];
        size_t got = 0;
        while (got < sizeof(req) - 1 && wait_readable(fd)) {
            ssize_t n = read(fd, req + got, sizeof(req) - 1 - got);
            if (n <= 0) {
                break;
            }
            got += (size_t)n;
            req[got] = '\0';
            if (strstr(req, "\r\n\r\n")) {
                break;
            }
        }
        static const char resp[] = "HTTP/1.1 200 OK\r\n"
                                   "Content-Length: 2\r\n"
                                   "Connection: close\r\n\r\nok";
        (void)!write(fd, resp, sizeof(resp) - 1);
        close(fd);
        return nullptr;
    }
}

// macOS ends a blocking connect with EINTR at every signal, and the connect
// carries on in the kernel. Linux restarts it, so there the signals land in
// the restarted connect and the request must succeed the same way.
static void
test_plain_connect_survives_signals(void)
{
    connect_server_t cs = {.client = this_thread()};
    full_listener_open(&cs.listener);
    n00b_string_t *url = n00b_cformat("http://127.0.0.1:[|#|]/",
                                      (int64_t)ntohs(cs.listener.sa.sin_port));

    pthread_t server;
    assert(pthread_create(&server, nullptr, connect_serve, &cs) == 0);
    auto rr = n00b_http_request_sync(url,
                                     .timeout_ms       = HANG_GUARD_MS,
                                     .allow_plain_http = true);
    atomic_store(&cs.done, true);

    if (n00b_result_is_err(rr)) {
        fprintf(stderr,
                "plain connect: request failed after %d signals: err=%lld\n",
                (int)atomic_load(&signals_handled),
                (long long)n00b_result_get_err(rr));
    }
    assert(n00b_result_is_ok(rr));
    assert(n00b_http_response_status(n00b_result_get(rr)) == 200);

    pthread_join(server, nullptr);
    full_listener_close(&cs.listener);
    printf("  [PASS] plain connect survives %d signals\n", CONNECT_SIGNALS);
}

#if defined(__APPLE__)
typedef struct {
    target_t      client;
    _Atomic(bool) done;
} storm_t;

static void *
storm(void *raw)
{
    storm_t *s = raw;
    while (signal_when_asleep(s->client, &s->done)) {
    }
    return nullptr;
}

// Signals that never stop: the request deadline, and nothing else, ends an
// interrupted connect. Linux restarts a blocking connect, which then waits
// for the kernel's own connect timeout, so this only runs on macOS.
static void
test_plain_connect_signals_end_at_the_deadline(void)
{
    full_listener_t fl;
    full_listener_open(&fl);
    n00b_string_t *url = n00b_cformat("http://127.0.0.1:[|#|]/",
                                      (int64_t)ntohs(fl.sa.sin_port));

    storm_t s = {.client = this_thread()};
    pthread_t storm_thread;
    assert(pthread_create(&storm_thread, nullptr, storm, &s) == 0);
    auto rr = n00b_http_request_sync(url,
                                     .timeout_ms       = 300,
                                     .allow_plain_http = true);
    atomic_store(&s.done, true);

    assert(n00b_result_is_err(rr));
    if (n00b_result_get_err(rr) != N00B_QUIC_ERR_TIMEOUT) {
        fprintf(stderr,
                "plain connect under signals ended with err=%lld, not the "
                "deadline\n",
                (long long)n00b_result_get_err(rr));
    }
    assert(n00b_result_get_err(rr) == N00B_QUIC_ERR_TIMEOUT);

    pthread_join(storm_thread, nullptr);
    full_listener_close(&fl);
    printf("  [PASS] plain connect under signals ends at the deadline\n");
}
#endif

// ---------------------------------------------------------------------------
// Metrics: reads and writes on a socket with a timeout
// ---------------------------------------------------------------------------

static n00b_conduit_t            *conduit;
static n00b_conduit_io_backend_t *io;

static n00b_quic_metric_listener_t *
metrics_listener(void)
{
    if (conduit == nullptr) {
        auto cr = n00b_conduit_new();
        assert(n00b_result_is_ok(cr));
        conduit = n00b_result_get(cr);
        auto ir = n00b_conduit_io_new_default(conduit);
        assert(n00b_result_is_ok(ir));
        io = n00b_result_get(ir);
    }
    n00b_quic_metric_registry_t *reg = n00b_quic_metrics_registry_new(nullptr);
    auto g = n00b_quic_metric_gauge(reg, "n00b_eintr_test", "eintr test");
    n00b_quic_metric_gauge_set(n00b_result_get(g), 7.0);
    auto lr = n00b_quic_metrics_listener_open(
        reg,
        conduit,
        io,
        .bind_host = n00b_buffer_from_cstr("127.0.0.1"),
        .bind_port = 0);
    assert(n00b_result_is_ok(lr));
    return n00b_result_get(lr);
}

static const char metrics_request[] = "GET /metrics HTTP/1.1\r\nHost: x\r\n\r\n";

typedef struct {
    int      peer;
    target_t handler;
    size_t   junk;
    bool     send_request;
    char     resp[8192];
    size_t   resp_len;
} metrics_peer_t;

// Signals the handler once it is asleep, then sends the request if the
// handler is waiting for it, drains `junk` bytes, and reads the response.
static void *
metrics_peer(void *raw)
{
    metrics_peer_t *mp = raw;
    signal_when_asleep(mp->handler, nullptr);
    if (mp->send_request) {
        // The handler may have answered and closed already.
        (void)!write(mp->peer, metrics_request, sizeof(metrics_request) - 1);
    }
    char sink[4096];
    while (mp->junk > 0 && wait_readable(mp->peer)) {
        size_t  want = mp->junk < sizeof(sink) ? mp->junk : sizeof(sink);
        ssize_t n    = read(mp->peer, sink, want);
        if (n <= 0) {
            break;
        }
        mp->junk -= (size_t)n;
    }
    while (mp->resp_len < sizeof(mp->resp) && wait_readable(mp->peer)) {
        ssize_t n = read(mp->peer,
                         mp->resp + mp->resp_len,
                         sizeof(mp->resp) - mp->resp_len);
        if (n <= 0) {
            break;
        }
        mp->resp_len += (size_t)n;
    }
    return nullptr;
}

static void
assert_full_metrics_response(const char *name, metrics_peer_t *mp)
{
    const char *body = memmem(mp->resp, mp->resp_len, "\r\n\r\n", 4);
    bool ok = mp->resp_len >= 12 && memcmp(mp->resp, "HTTP/1.1 200", 12) == 0
           && body != nullptr;
    if (ok) {
        const char *cl = memmem(mp->resp,
                                (size_t)(body - mp->resp),
                                "Content-Length: ",
                                16);
        ok = cl != nullptr
          && (size_t)(mp->resp + mp->resp_len - (body + 4))
                 == (size_t)strtoul(cl + 16, nullptr, 10);
    }
    if (!ok) {
        fprintf(stderr,
                "%s: incomplete response after a signal (%zu bytes): %.*s\n",
                name,
                mp->resp_len,
                (int)(mp->resp_len < 80 ? mp->resp_len : 80),
                mp->resp);
    }
    assert(ok);
    printf("  [PASS] %s\n", name);
}

// Runs the metrics handler on this thread against one end of a socket pair.
static void
metrics_exchange(const char *name, bool block_in_write)
{
    n00b_quic_metric_listener_t *l = metrics_listener();
    int                          sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    metrics_peer_t *mp = calloc(1, sizeof(*mp));
    mp->peer           = sv[1];
    mp->handler        = this_thread();
    mp->handler.nr     = block_in_write ? SYS_write : SYS_read;
    mp->send_request   = !block_in_write;
    if (block_in_write) {
        // The request is already there, and the handler's send buffer is
        // full, so the handler sleeps in its first write.
        assert(write(sv[1], metrics_request, sizeof(metrics_request) - 1)
               == (ssize_t)sizeof(metrics_request) - 1);
        int fl = fcntl(sv[0], F_GETFL);
        fcntl(sv[0], F_SETFL, fl | O_NONBLOCK);
        char junk[4096] = {};
        for (;;) {
            ssize_t n = write(sv[0], junk, sizeof(junk));
            if (n <= 0) {
                break;
            }
            mp->junk += (size_t)n;
        }
        fcntl(sv[0], F_SETFL, fl);
    }

    pthread_t peer;
    assert(pthread_create(&peer, nullptr, metrics_peer, mp) == 0);
    _n00b_quic_metrics_handle_conn(l, sv[0]);
    pthread_join(peer, nullptr);
    close(sv[1]);

    assert_full_metrics_response(name, mp);
    free(mp);
    n00b_quic_metrics_listener_close(l);
}

static void
test_metrics_read_survives_a_signal(void)
{
    metrics_exchange("metrics read survives a signal", false);
}

static void
test_metrics_write_survives_a_signal(void)
{
    metrics_exchange("metrics write survives a signal", true);
}

// ---------------------------------------------------------------------------
// NFS: an idle connection, on a socket with a timeout
// ---------------------------------------------------------------------------

#if defined(__linux__)
// The listening TCP socket `n00b_vfs_frontend_start` opened: the one not
// listening in `before`.
static uint16_t
new_listener_port(const bool *before, int nfds)
{
    for (int fd = 0; fd < nfds; fd++) {
        int       on  = 0;
        socklen_t len = sizeof(on);
        if (before[fd]
            || getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &on, &len) != 0
            || !on) {
            continue;
        }
        struct sockaddr_in sa  = {};
        socklen_t          sl  = sizeof(sa);
        if (getsockname(fd, (struct sockaddr *)&sa, &sl) == 0
            && sa.sin_family == AF_INET) {
            return ntohs(sa.sin_port);
        }
    }
    return 0;
}

static void
listening_fds(bool *out, int nfds)
{
    for (int fd = 0; fd < nfds; fd++) {
        int       on  = 0;
        socklen_t len = sizeof(on);
        out[fd] = getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &on, &len) == 0
               && on;
    }
}

// The thread asleep in read() on the server end of `client`.
static target_t
nfs_handler_thread(int client)
{
    struct sockaddr_in local = {};
    socklen_t          len   = sizeof(local);
    assert(getsockname(client, (struct sockaddr *)&local, &len) == 0);

    uint64_t give_up = now_ms() + HANG_GUARD_MS;
    for (;;) {
        assert(now_ms() < give_up);
        int server_fd = -1;
        for (int fd = 0; fd < 1024 && server_fd < 0; fd++) {
            struct sockaddr_in peer = {};
            socklen_t          pl   = sizeof(peer);
            if (fd != client
                && getpeername(fd, (struct sockaddr *)&peer, &pl) == 0
                && peer.sin_family == AF_INET
                && peer.sin_port == local.sin_port) {
                server_fd = fd;
            }
        }
        DIR *dir = server_fd < 0 ? nullptr : opendir("/proc/self/task");
        struct dirent *de;
        while (dir && (de = readdir(dir)) != nullptr) {
            if (de->d_name[0] == '.') {
                continue;
            }
            char path[64];
            snprintf(path, sizeof(path), "/proc/self/task/%s/syscall", de->d_name);
            FILE *f = fopen(path, "r");
            if (!f) {
                continue;
            }
            long          nr  = -1;
            unsigned long fd0 = 0;
            int           got = fscanf(f, "%ld %lx", &nr, &fd0);
            fclose(f);
            if (got == 2 && nr == SYS_read && (int)fd0 == server_fd) {
                target_t t = {.tid = (pid_t)atoi(de->d_name), .nr = SYS_read};
                closedir(dir);
                return t;
            }
        }
        if (dir) {
            closedir(dir);
        }
        nap();
    }
}

static void
put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t
get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// An NFS client between calls leaves the server asleep in read(). A signal
// there must not end the connection.
static void
test_nfs_connection_survives_a_signal(void)
{
    auto vr = n00b_vfs_new();
    assert(n00b_result_is_ok(vr));
    auto fr = n00b_vfs_frontend_nfs_new(n00b_result_get(vr),
                                        n00b_string_from_cstr("/eintr-probe"),
                                        0);
    assert(n00b_result_is_ok(fr));
    n00b_vfs_frontend_t *fe = n00b_result_get(fr);

    static bool before[1024];
    listening_fds(before, 1024);
    assert(n00b_result_is_ok(n00b_vfs_frontend_start(fe)));
    uint16_t port = new_listener_port(before, 1024);
    assert(port != 0);

    struct sockaddr_in sa = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    int client = socket(AF_INET, SOCK_STREAM, 0);
    assert(client >= 0);
    assert(connect(client, (struct sockaddr *)&sa, sizeof(sa)) == 0);

    signal_when_asleep(nfs_handler_thread(client), nullptr);

    // NFSPROC3_NULL, with AUTH_NONE credentials and verifier.
    static const uint32_t call[] = {
        0x80000000u | 40, 0xe1e1e1e1u, 0, 2, 100003, 3, 0, 0, 0, 0, 0};
    uint8_t wire[sizeof(call)];
    for (size_t i = 0; i < sizeof(call) / sizeof(call[0]); i++) {
        put_u32(wire + 4 * i, call[i]);
    }
    (void)!write(client, wire, sizeof(wire));

    uint8_t reply[64];
    ssize_t n = wait_readable(client) ? read(client, reply, sizeof(reply)) : -1;
    if (n < 12) {
        fprintf(stderr,
                "nfs: the connection gave %zd bytes after a signal, not a "
                "reply\n",
                n);
    }
    assert(n >= 12);
    assert(get_u32(reply + 4) == 0xe1e1e1e1u);
    assert(get_u32(reply + 8) == 1);

    close(client);
    n00b_vfs_frontend_stop(fe);
    printf("  [PASS] nfs connection survives a signal\n");
}
#endif

typedef void (*test_case_fn)(void);

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime = {};
    n00b_init(&runtime, argc, argv);
    setvbuf(stdout, nullptr, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    install_probe_handler();

    static const test_case_fn cases[] = {
        test_dns_reply_wait_survives_signals,
        test_plain_connect_survives_signals,
#if defined(__APPLE__)
        test_plain_connect_signals_end_at_the_deadline,
#endif
        test_metrics_read_survives_a_signal,
        test_metrics_write_survives_a_signal,
#if defined(__linux__)
        test_nfs_connection_survives_a_signal,
#endif
    };
    // An argument picks one case by index, so each can fail on its own.
    size_t first = 0;
    size_t count = sizeof(cases) / sizeof(cases[0]);
    if (argc > 1) {
        first = (size_t)atoi(argv[1]);
        assert(first < count);
        count = first + 1;
    }

    printf("Running EINTR signal tests...\n");
    for (size_t i = first; i < count; i++) {
        cases[i]();
    }
    printf("All EINTR signal tests passed.\n");

    restore_probe_handler();
    n00b_shutdown();
    return 0;
}

#else

// The DNS test hook exists only under N00B_DEBUG, so exit 77 tells meson to
// skip.
int
main(void)
{
    return 77;
}

#endif
