/*
 * socket.c - Socket abstraction for conduit
 *
 * Implements listeners (accept loop) and connections (managed FD wrapper).
 */

#include "conduit/conduit.h"
#include "conduit/socket.h"
#include "conduit/io.h"
#if defined(__linux__)
#include "core/syscall.h"
#endif
#include "util/path.h"
#include <errno.h>
#include <string.h>

#ifdef _WIN32
#include "internal/win32_sockets.h"
#define N00B_CLOSE_SOCKET(fd) closesocket((SOCKET)(fd))
#define N00B_SOCK_ERRNO       WSAGetLastError()
#define N00B_EWOULDBLOCK      WSAEWOULDBLOCK
#define N00B_EINPROGRESS      WSAEWOULDBLOCK
#define N00B_ECONNREFUSED     WSAECONNREFUSED
#define N00B_ECONNRESET       WSAECONNRESET
#define N00B_ETIMEDOUT        WSAETIMEDOUT
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#define N00B_SOCK_ERRNO       errno
#define N00B_EWOULDBLOCK      EWOULDBLOCK
#define N00B_EINPROGRESS      EINPROGRESS
#define N00B_ECONNREFUSED     ECONNREFUSED
#define N00B_ECONNRESET       ECONNRESET
#define N00B_ETIMEDOUT        ETIMEDOUT
#endif

#if !defined(_WIN32) && defined(__linux__)
static int
n00b_socket_close_raw(base_socket_t fd)
{
    long rc = _n00b_raw_linux_syscall1(SYS_close, (long)fd);
    return rc < 0 ? (int)-rc : 0;
}
#define N00B_CLOSE_SOCKET(fd) ((void)n00b_socket_close_raw((fd)))
#elif !defined(_WIN32)
#define N00B_CLOSE_SOCKET(fd) close(fd)
#endif

// ============================================================================
// Listener dict helpers
// ============================================================================

static void
listener_insert(n00b_conduit_t *c, n00b_conduit_listener_t *listener)
{
    n00b_dict_untyped_put(&c->listeners,
                          (void *)(uintptr_t)listener->fd,
                          listener);
    n00b_atomic_store(&listener->registry_registered, true);
}

static void
listener_remove(n00b_conduit_t *c, n00b_conduit_listener_t *listener)
{
    bool expected = true;
    if (n00b_atomic_cas(&listener->registry_registered, &expected, false)) {
        n00b_dict_untyped_remove(&c->listeners,
                                 (void *)(uintptr_t)listener->fd);
    }
}

static void
listener_release_native_once(n00b_conduit_listener_t *listener)
{
    bool previous = n00b_atomic_read_then_set(&listener->native_released, true);
    if (!previous) {
        N00B_CLOSE_SOCKET(listener->fd);
    }
}

n00b_option_t(n00b_conduit_listener_t *)
n00b_conduit_listener_get(n00b_conduit_t *c, base_socket_t fd)
{
    if (!c || fd == BASE_INVALID_SOCKET) {
        return n00b_option_none(n00b_conduit_listener_t *);
    }

    bool found = false;
    void *val = n00b_dict_untyped_get(&c->listeners,
                                      (void *)(uintptr_t)fd,
                                      &found);
    if (found) {
        return n00b_option_set(n00b_conduit_listener_t *, (n00b_conduit_listener_t *)val);
    }
    return n00b_option_none(n00b_conduit_listener_t *);
}

// ============================================================================
// Listener implementation
// ============================================================================

/*
 * Shared post-bind/post-listen wireup. Caller owns `fd` on entry; the
 * helper takes ownership on success (and on internal failure paths
 * the helper closes `fd` before returning the error). Used by both
 * the AF_INET and the AF_UNIX listener entry points so the
 * make_nonblocking / accept_topic / io_watch / listener_insert
 * sequence lives in exactly one place. `allocator == nullptr` selects
 * `c->allocator`.
 */
static n00b_result_t(n00b_conduit_listener_t *)
finalize_listener(n00b_conduit_t            *c,
                  n00b_conduit_io_backend_t *io,
                  base_socket_t              fd,
                  n00b_allocator_t          *allocator);

/*
 * Shared post-socket wireup for outbound connects. On entry `fd` is
 * an open socket chosen by the caller. The helper allocates the conn
 * struct, attaches an fd_owner, wires up the status topic and the
 * connect-completion hook, and returns the conn ready for
 * `connect_finalize`. TCP callers run non-blocking connect before
 * finalization; AF_UNIX callers may complete connect before this helper
 * so stale local endpoints can return a synchronous errno without
 * registering a managed fd.
 * `allocator == nullptr` selects `c->allocator`.
 */
static n00b_result_t(n00b_conduit_conn_t *)
prepare_outbound_conn(n00b_conduit_t            *c,
                      n00b_conduit_io_backend_t *io,
                      base_socket_t              fd,
                      n00b_allocator_t          *allocator);

/*
 * Post-connect() finishing for outbound conns. `connect_ret` is the
 * return code from connect(2) on the conn's fd; `connect_errno` is
 * the captured errno on a non-zero return.
 *
 *  - ret == 0: synchronously fire CONNECTED on the status topic.
 *  - ret < 0, errno == EINPROGRESS: leave pending; on_first_writable
 *    will fire when the kernel completes the handshake.
 *  - otherwise: close the fd, return errno.
 */
static n00b_result_t(n00b_conduit_conn_t *)
connect_finalize(n00b_conduit_conn_t *conn,
                 int                  connect_ret,
                 int                  connect_errno);

static int
unix_socket_errno(int err)
{
#ifdef _WIN32
    switch (err) {
    case WSAEADDRINUSE:   return EADDRINUSE;
    case WSAECONNREFUSED: return ECONNREFUSED;
    case WSAETIMEDOUT:    return ETIMEDOUT;
    case WSAEACCES:       return EACCES;
    case WSAEINVAL:       return EINVAL;
    case WSAEAFNOSUPPORT: return EAFNOSUPPORT;
    default:              return err;
    }
#else
    return err;
#endif
}

static n00b_result_t(int)
make_nonblocking(base_socket_t fd)
{
#ifdef _WIN32
    u_long mode = 1;
    if (ioctlsocket((SOCKET)fd, FIONBIO, &mode) != 0)
        return n00b_result_err(int, N00B_SOCK_ERRNO);
    return n00b_result_ok(int, 0);
#elif defined(__linux__)
    long flags = _n00b_raw_linux_syscall3(SYS_fcntl, (long)fd, F_GETFL, 0);
    if (flags < 0) {
        return n00b_result_err(int, (int)-flags);
    }
    long rc = _n00b_raw_linux_syscall3(SYS_fcntl,
                                       (long)fd,
                                       F_SETFL,
                                       flags | O_NONBLOCK);
    if (rc < 0) {
        return n00b_result_err(int, (int)-rc);
    }
    return n00b_result_ok(int, 0);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return n00b_result_err(int, errno);
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return n00b_result_err(int, errno);
    return n00b_result_ok(int, 0);
#endif
}

#if defined(__linux__)
static base_socket_t
socket_accept_raw(base_socket_t listener_fd,
                  struct sockaddr *addr,
                  socklen_t *addr_len,
                  int *err_out)
{
    *err_out = 0;
    long fd = _n00b_raw_linux_syscall4(SYS_accept4,
                                       (long)listener_fd,
                                       (long)(uintptr_t)addr,
                                       (long)(uintptr_t)addr_len,
                                       SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
        *err_out = (int)-fd;
        return BASE_INVALID_SOCKET;
    }
    return (base_socket_t)fd;
}

static int
socket_get_so_error_raw(base_socket_t fd)
{
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    long rc = _n00b_raw_linux_syscall5(SYS_getsockopt,
                                       (long)fd,
                                       SOL_SOCKET,
                                       SO_ERROR,
                                       (long)(uintptr_t)&so_error,
                                       (long)(uintptr_t)&len);
    if (rc < 0) {
        return (int)-rc;
    }
    return so_error;
}
#endif

n00b_result_t(n00b_conduit_listener_t *)
n00b_conduit_listen_tcp(n00b_conduit_t *c, n00b_conduit_io_backend_t *io,
                        n00b_string_t *host, uint16_t port, int backlog)
    _kargs {
        n00b_allocator_t *allocator = nullptr;
    }
{
    if (!c || !io) {
        return n00b_result_err(n00b_conduit_listener_t *, EINVAL);
    }

    base_socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == BASE_INVALID_SOCKET) {
        return n00b_result_err(n00b_conduit_listener_t *, N00B_SOCK_ERRNO);
    }

    int opt = 1;
#ifdef _WIN32
    int reuse_opt = SO_EXCLUSIVEADDRUSE;
#else
    int reuse_opt = SO_REUSEADDR;
#endif
    if (setsockopt(fd, SOL_SOCKET, reuse_opt,
                   (const char *)&opt, sizeof(opt)) != 0) {
        int err = N00B_SOCK_ERRNO;
        N00B_CLOSE_SOCKET(fd);
        return n00b_result_err(n00b_conduit_listener_t *, err);
    }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);

    if (host && host->u8_bytes) {
        char *host_cstr = n00b_alloc_array_with_opts(
            char,
            host->u8_bytes + 1,
            &(n00b_alloc_opts_t){.allocator = allocator});
        memcpy(host_cstr, host->data, host->u8_bytes);
        host_cstr[host->u8_bytes] = '\0';
        int pton_rc = inet_pton(AF_INET, host_cstr, &addr.sin_addr);
        n00b_free(host_cstr);
        if (pton_rc != 1) {
            N00B_CLOSE_SOCKET(fd);
            return n00b_result_err(n00b_conduit_listener_t *, EINVAL);
        }
    }
    else {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        N00B_CLOSE_SOCKET(fd);
        return n00b_result_err(n00b_conduit_listener_t *, N00B_SOCK_ERRNO);
    }

    if (backlog <= 0) {
        backlog = 128;
    }
    if (listen(fd, backlog) < 0) {
        N00B_CLOSE_SOCKET(fd);
        return n00b_result_err(n00b_conduit_listener_t *, N00B_SOCK_ERRNO);
    }

    return finalize_listener(c, io, fd, allocator);
}

static n00b_result_t(n00b_conduit_listener_t *)
finalize_listener(n00b_conduit_t            *c,
                  n00b_conduit_io_backend_t *io,
                  base_socket_t              fd,
                  n00b_allocator_t          *allocator)
{
    {
        auto nb_r = make_nonblocking(fd);
        if (n00b_result_is_err(nb_r)) {
            N00B_CLOSE_SOCKET(fd);
            return n00b_result_err(n00b_conduit_listener_t *,
                                    n00b_result_get_err(nb_r));
        }
    }

    n00b_allocator_t *alloc = allocator ? allocator : c->allocator;

    n00b_conduit_listener_t *listener =
        n00b_alloc_with_opts(n00b_conduit_listener_t,
                              &(n00b_alloc_opts_t){.allocator = alloc});

    listener->conduit     = c;
    listener->io          = io;
    listener->fd          = fd;
    listener->listener_id = n00b_atomic_add(&c->next_listener_id, 1);
    n00b_atomic_store(&listener->active, true);
    n00b_atomic_store(&listener->registry_registered, false);
    n00b_atomic_store(&listener->native_released, false);
    n00b_atomic_store(&listener->close_generation, 0);

    /* Typed initializer so subscriptions is pool-allocated — see the matching
     * comment in n00b_conduit_conn_from_fd. */
    n00b_conduit_topic_t(n00b_conduit_sock_accept_payload_t) *accept_topic =
        n00b_conduit_topic_init(n00b_conduit_sock_accept_payload_t,
                                c, N00B_CONDUIT_URI_SOCK_ACCEPT(listener->listener_id));
    if (!accept_topic) {
        N00B_CLOSE_SOCKET(fd);
        return n00b_result_err(n00b_conduit_listener_t *, ENOMEM);
    }
    listener->accept_topic = (n00b_conduit_topic_base_t *)accept_topic;

    // Register with I/O backend for read events (accept readiness).
    // Wrap in a variant so the IO dispatch loop can discriminate.
    n00b_conduit_io_target_t *target =
        n00b_alloc_with_opts(n00b_conduit_io_target_t,
                              &(n00b_alloc_opts_t){.allocator = alloc});
    _n00b_variant_set_ptr(target, n00b_conduit_listener_t *, listener);
    listener->io_target = target;
    auto watch_r = n00b_conduit_io_watch(io, fd, N00B_CONDUIT_IO_READ, target);
    if (n00b_result_is_err(watch_r)) {
        n00b_conduit_topic_close(listener->accept_topic);
        n00b_free(listener->io_target);
        listener->io_target = nullptr;
        N00B_CLOSE_SOCKET(fd);
        return n00b_result_err(n00b_conduit_listener_t *,
                               n00b_result_get_err(watch_r));
    }

    listener_insert(c, listener);

    return n00b_result_ok(n00b_conduit_listener_t *, listener);
}

n00b_option_t(n00b_conduit_topic_base_t *)
n00b_conduit_listener_accept_topic(n00b_conduit_listener_t *listener)
{
    return n00b_option_from_nullable(n00b_conduit_topic_base_t *,
                                     listener ? listener->accept_topic : nullptr);
}

uint16_t
n00b_conduit_listener_local_port(n00b_conduit_listener_t *listener)
{
    if (listener == nullptr || listener->fd == BASE_INVALID_SOCKET) {
        return 0;
    }

    struct sockaddr_storage ss = {};
#ifdef _WIN32
    int len = sizeof(ss);
#else
    socklen_t len = sizeof(ss);
#endif
    if (getsockname(listener->fd, (struct sockaddr *)&ss, &len) != 0) {
        return 0;
    }
    if (ss.ss_family == AF_INET) {
        return ntohs(((struct sockaddr_in *)&ss)->sin_port);
    }
    return 0;
}

void
n00b_conduit_release_fd(base_socket_t fd)
{
    if (fd != BASE_INVALID_SOCKET) {
        N00B_CLOSE_SOCKET(fd);
    }
}

void
n00b_conduit_listener_close(n00b_conduit_listener_t *listener)
    ensures {
        listener == nullptr || listener->close_generation <= 1;
        listener == nullptr || listener->close_generation == 0 ||
            listener->active == false;
        listener == nullptr || listener->close_generation == 0 ||
            listener->registry_registered == false;
        listener == nullptr || listener->close_generation == 0 ||
            listener->native_released == true;
    }
{
    if (!listener) {
        return;
    }

    uint64_t expected = 0;
    if (!n00b_atomic_cas(&listener->close_generation, &expected, 1)) {
        return;
    }

    n00b_atomic_store(&listener->active, false);
    listener_remove(listener->conduit, listener);
    n00b_conduit_io_unwatch(listener->io, listener->fd);
    listener_release_native_once(listener);
    n00b_conduit_topic_close(listener->accept_topic);
    // The io watch target was allocated in n00b_conduit_listener_create and
    // is no longer referenced once the fd is unwatched; free it so a
    // create/close listener cycle does not leak.
    n00b_free(listener->io_target);
    listener->io_target = nullptr;
}

void
n00b_conduit_listener_dispatch(n00b_conduit_listener_t *listener, uint32_t io_ops)
{
    if (!listener || !n00b_atomic_load(&listener->active)) {
        return;
    }

    if (!(io_ops & N00B_CONDUIT_IO_READ)) {
        return;
    }

    n00b_conduit_topic_base_t *topic = listener->accept_topic;

    while (1) {
        struct sockaddr_storage client_addr;
        socklen_t addr_len = sizeof(client_addr);

#if defined(__linux__)
        int err = 0;
        base_socket_t client_fd = socket_accept_raw(listener->fd,
                                                    (struct sockaddr *)&client_addr,
                                                    &addr_len,
                                                    &err);
#else
        base_socket_t client_fd = accept(listener->fd,
                                         (struct sockaddr *)&client_addr,
                                         &addr_len);
#endif
        if (client_fd == BASE_INVALID_SOCKET) {
#if !defined(__linux__)
            int err = N00B_SOCK_ERRNO;
#endif
            if (err == N00B_EWOULDBLOCK
#ifndef _WIN32
                || err == EAGAIN
#endif
            ) {
                break;
            }
#ifndef _WIN32
            if (err == EINTR) {
                continue;
            }
#endif
            break;
        }

        (void)make_nonblocking(client_fd);

        n00b_result_t(n00b_conduit_publisher_t *) pub_res =
            n00b_conduit_publish_try_claim(topic);
        if (n00b_result_is_err(pub_res)) {
            N00B_CLOSE_SOCKET(client_fd);
            continue;
        }
        n00b_conduit_publisher_t *pub = n00b_result_get(pub_res);

        n00b_conduit_sock_accept_msg_t *msg = n00b_alloc_with_opts(
            n00b_conduit_sock_accept_msg_t,
            &(n00b_alloc_opts_t){.allocator = listener->conduit->allocator});

        msg->header.type       = N00B_CONDUIT_MSG_USER;
        msg->header.topic      = topic;
        msg->header.generation = n00b_conduit_topic_generation(topic);
        msg->header.epoch      = n00b_conduit_topic_epoch(topic);
        msg->header.timestamp  = 0;
        msg->header.next       = nullptr;

        msg->payload.client_fd = client_fd;
        msg->payload.addr      = client_addr;
        msg->payload.addr_len  = addr_len;

        n00b_conduit_topic_deliver_msg(
            n00b_conduit_sock_accept_payload_t,
            (n00b_conduit_topic_t(n00b_conduit_sock_accept_payload_t) *)topic,
            msg,
            N00B_CONDUIT_OP_ALL);

        n00b_conduit_publish_yield(pub);
    }
}

// ============================================================================
// Connection implementation
// ============================================================================

static void
publish_conn_status(n00b_conduit_conn_t *conn,
                    n00b_conduit_conn_event_t event, int error_code)
{
    n00b_conduit_topic_base_t *topic = conn->status_topic;

    n00b_result_t(n00b_conduit_publisher_t *) pub_res =
        n00b_conduit_publish_try_claim(topic);
    if (n00b_result_is_err(pub_res)) {
        return;
    }
    n00b_conduit_publisher_t *pub = n00b_result_get(pub_res);

    n00b_conduit_sock_status_msg_t *msg = n00b_alloc_with_opts(
        n00b_conduit_sock_status_msg_t,
        &(n00b_alloc_opts_t){.allocator = conn->conduit->allocator});

    msg->header.type       = N00B_CONDUIT_MSG_USER;
    msg->header.topic      = topic;
    msg->header.generation = n00b_conduit_topic_generation(topic);
    msg->header.epoch      = n00b_conduit_topic_epoch(topic);
    msg->header.timestamp  = 0;
    msg->header.next       = nullptr;

    msg->payload.fd         = conn->fd;
    msg->payload.event      = event;
    msg->payload.error_code = error_code;

    n00b_conduit_topic_deliver_msg(
        n00b_conduit_sock_status_payload_t,
        (n00b_conduit_topic_t(n00b_conduit_sock_status_payload_t) *)topic,
        msg,
        N00B_CONDUIT_OP_ALL);

    n00b_conduit_publish_yield(pub);
}

n00b_result_t(n00b_conduit_conn_t *)
n00b_conduit_conn_from_fd(n00b_conduit_t *c, n00b_conduit_io_backend_t *io,
                          base_socket_t fd)
{
    if (!c || !io || fd == BASE_INVALID_SOCKET) {
        return n00b_result_err(n00b_conduit_conn_t *, EINVAL);
    }

    n00b_conduit_conn_t *conn = n00b_alloc_with_opts(
        n00b_conduit_conn_t,
        &(n00b_alloc_opts_t){.allocator = c->allocator});

    conn->conduit         = c;
    conn->fd              = fd;
    conn->connect_pending = false;
    n00b_atomic_store(&conn->conn_state, N00B_CONDUIT_CONN_ST_CONNECTED);
    n00b_atomic_store(&conn->close_generation, 0);
    n00b_atomic_store(&conn->terminal_status_count, 0);

    auto manage_r = n00b_conduit_fd_manage(c, io, fd, true);
    if (n00b_result_is_err(manage_r)) {
        return n00b_result_err(n00b_conduit_conn_t *, n00b_result_get_err(manage_r));
    }
    conn->owner = n00b_result_get(manage_r);

    /* Use the typed initializer (not bare topic_get): it creates the
     * `subscriptions` list with `.allocator = c->allocator` (the non-moving
     * conduit pool).  Bare topic_get leaves subscriptions zeroed with a null
     * allocator, so the first subscribe pushes a backing array into the
     * default GC heap — which the GC reclaims out from under this
     * pool-resident (GC-opaque) topic, dangling subscriptions.data and
     * crashing the status deliver during conn_close. */
    n00b_conduit_topic_t(n00b_conduit_sock_status_payload_t) *status_topic =
        n00b_conduit_topic_init(n00b_conduit_sock_status_payload_t,
                                c, N00B_CONDUIT_URI_SOCK_STATUS(fd));
    if (!status_topic) {
        n00b_conduit_fd_owner_close(conn->owner);
        return n00b_result_err(n00b_conduit_conn_t *, ENOMEM);
    }
    conn->status_topic = (n00b_conduit_topic_base_t *)status_topic;

    publish_conn_status(conn, N00B_CONDUIT_CONN_CONNECTED, 0);

    return n00b_result_ok(n00b_conduit_conn_t *, conn);
}

n00b_option_t(n00b_conduit_topic_base_t *)
n00b_conduit_conn_status_topic(n00b_conduit_conn_t *conn)
{
    return n00b_option_from_nullable(n00b_conduit_topic_base_t *,
                                     conn ? conn->status_topic : nullptr);
}

n00b_option_t(n00b_conduit_fd_owner_t *)
n00b_conduit_conn_fd_owner(n00b_conduit_conn_t *conn)
{
    return n00b_option_from_nullable(n00b_conduit_fd_owner_t *,
                                     conn ? conn->owner : nullptr);
}

void
n00b_conduit_conn_close(n00b_conduit_conn_t *conn)
    ensures {
        conn == nullptr || conn->close_generation <= 1;
        conn == nullptr || conn->close_generation == 0 ||
            conn->conn_state == N00B_CONDUIT_CONN_ST_CLOSED;
        conn == nullptr || conn->terminal_status_count <= 1;
        conn == nullptr || conn->owner == nullptr ||
            conn->owner->close_generation <= 1;
        conn == nullptr || conn->owner == nullptr ||
            conn->owner->registry_registered == false;
    }
{
    if (!conn) {
        return;
    }

    uint64_t expected = 0;
    if (!n00b_atomic_cas(&conn->close_generation, &expected, 1)) {
        return;
    }

    n00b_atomic_store(&conn->conn_state, N00B_CONDUIT_CONN_ST_CLOSED);
    uint32_t previous = n00b_atomic_read_then_set(&conn->terminal_status_count,
                                                   1);
    if (previous == 0) {
        publish_conn_status(conn, N00B_CONDUIT_CONN_CLOSED, 0);
    }
    n00b_conduit_topic_close(conn->status_topic);
    n00b_conduit_fd_owner_close(conn->owner);
}

// ============================================================================
// Outbound connect
// ============================================================================

static void
connect_completion_hook(n00b_conduit_fd_owner_t *owner, void *ctx)
{
    n00b_conduit_conn_t *conn = ctx;

#if defined(__linux__)
    int so_error = socket_get_so_error_raw(owner->fd);
#else
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (getsockopt(owner->fd, SOL_SOCKET, SO_ERROR, (char *)&so_error, &len) != 0) {
        so_error = N00B_SOCK_ERRNO;
    }
#endif

    conn->connect_pending = false;

    if (so_error == 0) {
        n00b_atomic_store(&conn->conn_state, N00B_CONDUIT_CONN_ST_CONNECTED);
        publish_conn_status(conn, N00B_CONDUIT_CONN_CONNECTED, 0);
    }
    else {
        n00b_atomic_store(&conn->conn_state, N00B_CONDUIT_CONN_ST_ERROR);

        n00b_conduit_conn_event_t event;
        switch (so_error) {
        case N00B_ECONNREFUSED:
            event = N00B_CONDUIT_CONN_REFUSED;
            break;
        case N00B_ECONNRESET:
            event = N00B_CONDUIT_CONN_RESET;
            break;
        case N00B_ETIMEDOUT:
            event = N00B_CONDUIT_CONN_TIMEOUT;
            break;
        default:
            event = N00B_CONDUIT_CONN_ERROR;
            break;
        }

        publish_conn_status(conn, event, so_error);
    }
}

static n00b_result_t(n00b_conduit_conn_t *)
prepare_outbound_conn(n00b_conduit_t            *c,
                      n00b_conduit_io_backend_t *io,
                      base_socket_t              fd,
                      n00b_allocator_t          *allocator)
{
    n00b_allocator_t *alloc = allocator ? allocator : c->allocator;

    n00b_conduit_conn_t *conn = n00b_alloc_with_opts(
        n00b_conduit_conn_t,
        &(n00b_alloc_opts_t){.allocator = alloc});

    conn->conduit         = c;
    conn->fd              = fd;
    conn->connect_pending = true;
    n00b_atomic_store(&conn->conn_state, N00B_CONDUIT_CONN_ST_CONNECTING);
    n00b_atomic_store(&conn->close_generation, 0);
    n00b_atomic_store(&conn->terminal_status_count, 0);

    auto manage_r = n00b_conduit_fd_manage(c, io, fd, true);
    if (n00b_result_is_err(manage_r)) {
        N00B_CLOSE_SOCKET(fd);
        return n00b_result_err(n00b_conduit_conn_t *,
                                n00b_result_get_err(manage_r));
    }
    conn->owner = n00b_result_get(manage_r);

    /* Typed initializer so subscriptions is pool-allocated — see the matching
     * comment in n00b_conduit_conn_from_fd. */
    n00b_conduit_topic_t(n00b_conduit_sock_status_payload_t) *status_topic =
        n00b_conduit_topic_init(n00b_conduit_sock_status_payload_t,
                                c, N00B_CONDUIT_URI_SOCK_STATUS(fd));
    if (!status_topic) {
        n00b_conduit_fd_owner_close(conn->owner);
        return n00b_result_err(n00b_conduit_conn_t *, ENOMEM);
    }
    conn->status_topic = (n00b_conduit_topic_base_t *)status_topic;

    conn->owner->on_first_writable     = connect_completion_hook;
    conn->owner->on_first_writable_ctx = conn;

    return n00b_result_ok(n00b_conduit_conn_t *, conn);
}

static n00b_result_t(n00b_conduit_conn_t *)
connect_finalize(n00b_conduit_conn_t *conn,
                 int                  connect_ret,
                 int                  connect_errno)
{
    if (connect_ret < 0) {
        if (connect_errno != N00B_EINPROGRESS) {
            // Tear down through the full conn-close path so the
            // fd_owner registered by prepare_outbound_conn is
            // unwatched and the status topic closed — closing only
            // the raw fd would leave a dangling owner in the IO
            // backend.
            n00b_conduit_conn_close(conn);
            return n00b_result_err(n00b_conduit_conn_t *, connect_errno);
        }
        /* Async connect in progress: arm write-readiness so the IO backend
         * delivers the writable event that completes the connect and fires
         * `connect_completion_hook` (via on_first_writable). Without this the
         * fd is watched with an empty mask and the connect never completes. */
        n00b_conduit_fd_activate_writes(conn->owner);
        return n00b_result_ok(n00b_conduit_conn_t *, conn);
    }

    // Immediate connection (e.g. localhost AF_INET or unix-domain).
    conn->connect_pending              = false;
    conn->owner->on_first_writable     = nullptr;
    conn->owner->on_first_writable_ctx = nullptr;
    n00b_atomic_store(&conn->conn_state, N00B_CONDUIT_CONN_ST_CONNECTED);
    publish_conn_status(conn, N00B_CONDUIT_CONN_CONNECTED, 0);
    return n00b_result_ok(n00b_conduit_conn_t *, conn);
}

n00b_result_t(n00b_conduit_conn_t *)
n00b_conduit_conn_tcp(n00b_conduit_t *c, n00b_conduit_io_backend_t *io,
                      n00b_string_t *host, uint16_t port)
    _kargs {
        n00b_allocator_t *allocator = nullptr;
    }
{
    if (!c || !io || !host || !host->u8_bytes) {
        return n00b_result_err(n00b_conduit_conn_t *, EINVAL);
    }

    base_socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == BASE_INVALID_SOCKET) {
        return n00b_result_err(n00b_conduit_conn_t *, N00B_SOCK_ERRNO);
    }

    {
        auto nb_r = make_nonblocking(fd);
        if (n00b_result_is_err(nb_r)) {
            N00B_CLOSE_SOCKET(fd);
            return n00b_result_err(n00b_conduit_conn_t *,
                                    n00b_result_get_err(nb_r));
        }
    }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);

    char *host_cstr = n00b_alloc_array_with_opts(
        char,
        host->u8_bytes + 1,
        &(n00b_alloc_opts_t){.allocator = allocator});
    memcpy(host_cstr, host->data, host->u8_bytes);
    host_cstr[host->u8_bytes] = '\0';
    int pton_rc = inet_pton(AF_INET, host_cstr, &addr.sin_addr);
    n00b_free(host_cstr);
    if (pton_rc != 1) {
        N00B_CLOSE_SOCKET(fd);
        return n00b_result_err(n00b_conduit_conn_t *, EINVAL);
    }

    auto conn_r = prepare_outbound_conn(c, io, fd, allocator);
    if (n00b_result_is_err(conn_r)) {
        return conn_r;
    }
    n00b_conduit_conn_t *conn = n00b_result_get(conn_r);

    // eintr: the socket is nonblocking, so connect does not sleep.
    int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    int saved_errno = N00B_SOCK_ERRNO;
    return connect_finalize(conn, ret, saved_errno);
}

// ============================================================================
// AF_UNIX listener + outbound connect
// ============================================================================

/*
 * Fill `addr` for an AF_UNIX socket targeting `socket_path`. Returns
 * Ok(addr_len) on success, Err(errno) if the path doesn't fit in
 * sun_path. The size of sun_path varies by platform (104 on darwin,
 * 108 on Linux); we use sizeof(addr->sun_path) which captures
 * whichever is correct at compile time.
 */
static n00b_result_t(socklen_t)
build_unix_sockaddr(struct sockaddr_un *addr, n00b_string_t *socket_path)
{
    *addr = (struct sockaddr_un){};
    addr->sun_family = AF_UNIX;

    size_t path_len = socket_path->u8_bytes;
    // Leave room for the trailing NUL byte inside sun_path.
    if (path_len >= sizeof(addr->sun_path)) {
        return n00b_result_err(socklen_t, ENAMETOOLONG);
    }
    memcpy(addr->sun_path, socket_path->data, path_len);
    addr->sun_path[path_len] = '\0';

    /*
     * SUN_LEN portability: not all platforms ship the macro.
     * We use offsetof + path_len + 1 (for the NUL) which is the
     * standard portable form. The kernel only cares about the
     * sun_family field plus the path bytes up to the terminator.
     */
    socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path)
                                + path_len + 1);
    return n00b_result_ok(socklen_t, len);
}

n00b_result_t(n00b_conduit_listener_t *)
n00b_conduit_listen_unix(n00b_conduit_t            *c,
                         n00b_conduit_io_backend_t *io,
                         n00b_string_t             *socket_path,
                         int                        backlog)
    _kargs {
        bool              unlink_stale = false;
        int               mode         = 0;
        n00b_allocator_t *allocator    = nullptr;
    }
{
    if (!c || !io || !socket_path) {
        return n00b_result_err(n00b_conduit_listener_t *, EINVAL);
    }

    struct sockaddr_un addr = {};
    socklen_t addr_len = 0;
    {
        auto a_r = build_unix_sockaddr(&addr, socket_path);
        if (n00b_result_is_err(a_r)) {
            return n00b_result_err(n00b_conduit_listener_t *,
                                    n00b_result_get_err(a_r));
        }
        addr_len = n00b_result_get(a_r);
    }

    if (unlink_stale) {
        auto unlink_r = n00b_file_unlink(socket_path, .ignore_missing = true);
        if (n00b_result_is_err(unlink_r)) {
            return n00b_result_err(n00b_conduit_listener_t *,
                                    n00b_result_get_err(unlink_r));
        }
    }

    base_socket_t fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == BASE_INVALID_SOCKET) {
        return n00b_result_err(n00b_conduit_listener_t *,
                                unix_socket_errno(N00B_SOCK_ERRNO));
    }

    if (bind(fd, (struct sockaddr *)&addr, addr_len) < 0) {
        int saved_errno = unix_socket_errno(N00B_SOCK_ERRNO);
        N00B_CLOSE_SOCKET(fd);
        return n00b_result_err(n00b_conduit_listener_t *, saved_errno);
    }

    if (mode != 0) {
        auto mode_r = n00b_path_set_mode(socket_path, (uint32_t)mode);
        if (n00b_result_is_err(mode_r)) {
            int saved_errno = n00b_result_get_err(mode_r);
            N00B_CLOSE_SOCKET(fd);
            // Leave the socket file in place; the caller may want to
            // inspect it. chmod failure should not silently succeed.
            return n00b_result_err(n00b_conduit_listener_t *, saved_errno);
        }
    }

    if (backlog <= 0) {
        backlog = 128;
    }
    if (listen(fd, backlog) < 0) {
        int saved_errno = unix_socket_errno(N00B_SOCK_ERRNO);
        N00B_CLOSE_SOCKET(fd);
        return n00b_result_err(n00b_conduit_listener_t *, saved_errno);
    }

    return finalize_listener(c, io, fd, allocator);
}

n00b_result_t(n00b_conduit_conn_t *)
n00b_conduit_conn_unix(n00b_conduit_t *c, n00b_conduit_io_backend_t *io,
                       n00b_string_t *socket_path)
    _kargs {
        n00b_allocator_t *allocator = nullptr;
    }
{
    if (!c || !io || !socket_path) {
        return n00b_result_err(n00b_conduit_conn_t *, EINVAL);
    }

    struct sockaddr_un addr = {};
    socklen_t addr_len = 0;
    {
        auto a_r = build_unix_sockaddr(&addr, socket_path);
        if (n00b_result_is_err(a_r)) {
            return n00b_result_err(n00b_conduit_conn_t *,
                                    n00b_result_get_err(a_r));
        }
        addr_len = n00b_result_get(a_r);
    }

    base_socket_t fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == BASE_INVALID_SOCKET) {
        return n00b_result_err(n00b_conduit_conn_t *,
                                unix_socket_errno(N00B_SOCK_ERRNO));
    }

    // eintr: Linux restarts an AF_UNIX connect under SA_RESTART, and macOS
    // completes or refuses one without sleeping.
    int ret = connect(fd, (struct sockaddr *)&addr, addr_len);
    if (ret < 0) {
        int saved_errno = unix_socket_errno(N00B_SOCK_ERRNO);
        N00B_CLOSE_SOCKET(fd);
        return n00b_result_err(n00b_conduit_conn_t *, saved_errno);
    }

    auto nb_r = make_nonblocking(fd);
    if (n00b_result_is_err(nb_r)) {
        N00B_CLOSE_SOCKET(fd);
        return n00b_result_err(n00b_conduit_conn_t *,
                                n00b_result_get_err(nb_r));
    }

    auto conn_r = prepare_outbound_conn(c, io, fd, allocator);
    if (n00b_result_is_err(conn_r)) {
        return conn_r;
    }
    n00b_conduit_conn_t *conn = n00b_result_get(conn_r);

    return connect_finalize(conn, 0, 0);
}
