/**
 * @file fd_managed.h
 * @brief Managed FD I/O layer for the conduit system.
 *
 * Provides two layers of FD I/O:
 * - **Layer 1** (as-done): Owner does non-blocking I/O, publishes immutable
 *   shared buffers as events. All subscribers see every buffer (zero-copy).
 * - **Layer 2** (stream): Consumers request N bytes and block on their inbox.
 *   An accumulator stitches as-done buffers into contiguous responses.
 *
 * Single-owner model: one owner per FD manages the full lifecycle.
 *
 * Usage:
 * @code
 *     n00b_conduit_fd_owner_t *owner =
 *         n00b_conduit_fd_manage(c, io, fd, true);
 *     // Subscribe to owner->read_topic for Layer 1 read events ...
 *     // Or create a stream reader for Layer 2:
 *     n00b_conduit_stream_reader_t *sr =
 *         n00b_conduit_stream_reader_new(c, owner);
 * @endcode
 */
#pragma once

#include "conduit/conduit.h"
#include "conduit/io.h"
#include "core/buffer.h"
#include <stdint.h>
#include <stdbool.h>

// n00b_buffer_t * is the standard wire type for FD read events
// and byte-oriented pipelines.
N00B_CONDUIT_FULL_IMPL(n00b_buffer_t *);

// Forward declarations
typedef struct n00b_conduit_stream_reader n00b_conduit_stream_reader_t;

// ============================================================================
// FD Status Events
// ============================================================================

/**
 * @brief FD status event flags.
 */
typedef enum {
    N00B_CONDUIT_FD_ST_READ_EOF    = 1 << 0, /**< Read side reached EOF */
    N00B_CONDUIT_FD_ST_WRITE_EPIPE = 1 << 1, /**< Write side received EPIPE */
    N00B_CONDUIT_FD_ST_READ_ERR    = 1 << 2, /**< Read side encountered error */
    N00B_CONDUIT_FD_ST_WRITE_ERR   = 1 << 3, /**< Write side encountered error */
    N00B_CONDUIT_FD_ST_CLOSED      = 1 << 4, /**< FD has been closed */
} n00b_conduit_fd_status_op_t;

// ============================================================================
// Payload Types
// ============================================================================

/**
 * @brief Status event payload for FD EOF, errors, and close.
 */
typedef struct {
    base_socket_t                fd;          /**< Native descriptor */
    n00b_conduit_fd_status_op_t  status;      /**< Status operation flags */
    int                          error_code;  /**< errno if error occurred */
} n00b_conduit_fd_status_payload_t;

/**
 * @brief Layer 1 write payload: confirmation of bytes written.
 */
typedef struct {
    base_socket_t fd;      /**< Native descriptor */
    void     *data;        /**< Immutable shared buffer that was written */
    size_t    len;         /**< Length of data in bytes */
    uint64_t  stream_pos;  /**< Stream position of first byte */
    uint64_t  request_id;  /**< Request ID for correlation */
} n00b_conduit_fd_write_payload_t;

/**
 * @brief Write request payload: consumer -> owner write request.
 */
typedef struct {
    void     *data;        /**< Buffer to write */
    size_t    len;         /**< Length of data in bytes */
    uint64_t  request_id;  /**< Unique request ID */
    void     *reply_inbox; /**< Inbox for completion event */
    bool    (*reply_push)(void *inbox, void *msg); /**< Push function */
} n00b_conduit_fd_write_req_payload_t;

/**
 * @brief Write completion payload: owner -> consumer write result.
 */
typedef struct {
    base_socket_t fd;          /**< Native descriptor */
    uint64_t  request_id;      /**< Request ID matching the original */
    size_t    bytes_written;   /**< Number of bytes written */
    bool      error;           /**< True if write failed */
    int       error_code;      /**< errno if error occurred */
} n00b_conduit_fd_write_done_payload_t;

/**
 * @brief Blocking write completion facts for an FD owner.
 *
 * `error_code` uses `N00B_CONDUIT_ERR_*` values when @c error is true.
 */
typedef struct {
    size_t bytes_written;
    bool   error;
    int    error_code;
} n00b_fd_owner_write_attempt_t;

/**
 * @brief Layer 2 stream read response: accumulated bytes.
 */
typedef struct {
    base_socket_t fd;      /**< Native descriptor */
    void     *data;        /**< Accumulated buffer */
    size_t    len;         /**< Length of data in bytes */
    uint64_t  stream_pos;  /**< Stream position of first byte */
    bool      eof;         /**< True if EOF reached */
    bool      error;       /**< True if read failed */
    int       error_code;  /**< errno if error occurred */
} n00b_conduit_fd_stream_payload_t;

// ============================================================================
// FD managed type instantiations
// ============================================================================

N00B_CONDUIT_FULL_IMPL(n00b_conduit_fd_status_payload_t);
N00B_CONDUIT_FULL_IMPL(n00b_conduit_fd_write_payload_t);
N00B_CONDUIT_FULL_IMPL(n00b_conduit_fd_write_req_payload_t);
N00B_CONDUIT_FULL_IMPL(n00b_conduit_fd_write_done_payload_t);
N00B_CONDUIT_FULL_IMPL(n00b_conduit_fd_stream_payload_t);

// ============================================================================
// Convenience type aliases
// ============================================================================

typedef n00b_conduit_message_t(n00b_conduit_fd_status_payload_t)
    n00b_conduit_fd_status_msg_t;
typedef n00b_conduit_message_t(n00b_conduit_fd_write_payload_t)
    n00b_conduit_fd_write_msg_t;
typedef n00b_conduit_message_t(n00b_conduit_fd_write_req_payload_t)
    n00b_conduit_fd_write_req_msg_t;
typedef n00b_conduit_message_t(n00b_conduit_fd_write_done_payload_t)
    n00b_conduit_fd_write_done_msg_t;
typedef n00b_conduit_message_t(n00b_conduit_fd_stream_payload_t)
    n00b_conduit_fd_stream_msg_t;

typedef n00b_conduit_inbox_t(n00b_conduit_fd_status_payload_t)
    n00b_conduit_fd_status_inbox_t;
typedef n00b_conduit_inbox_t(n00b_conduit_fd_write_payload_t)
    n00b_conduit_fd_write_inbox_t;
typedef n00b_conduit_inbox_t(n00b_conduit_fd_write_req_payload_t)
    n00b_conduit_fd_write_req_inbox_t;
typedef n00b_conduit_inbox_t(n00b_conduit_fd_write_done_payload_t)
    n00b_conduit_fd_write_done_inbox_t;
typedef n00b_conduit_inbox_t(n00b_conduit_fd_stream_payload_t)
    n00b_conduit_fd_stream_inbox_t;

// ============================================================================
// Convenience inbox macros
// ============================================================================

/** @brief Create a new inbox for FD status events. */
#define n00b_conduit_fd_status_inbox_new(c)                                    \
    ({                                                                         \
        n00b_conduit_fd_status_inbox_t *_inbox =                               \
            n00b_alloc_with_opts(n00b_conduit_fd_status_inbox_t,               \
                &(n00b_alloc_opts_t){.allocator = (c)->allocator});            \
        n00b_conduit_inbox_init(n00b_conduit_fd_status_payload_t,              \
                                _inbox, c, N00B_CONDUIT_BP_UNBOUNDED, 0);      \
        _inbox;                                                                \
    })

/** @brief Create a new inbox for FD write completion events. */
#define n00b_conduit_fd_write_inbox_new(c)                                     \
    ({                                                                         \
        n00b_conduit_fd_write_inbox_t *_inbox =                                \
            n00b_alloc_with_opts(n00b_conduit_fd_write_inbox_t,                \
                &(n00b_alloc_opts_t){.allocator = (c)->allocator});            \
        n00b_conduit_inbox_init(n00b_conduit_fd_write_payload_t,               \
                                _inbox, c, N00B_CONDUIT_BP_UNBOUNDED, 0);      \
        _inbox;                                                                \
    })

/** @brief Create a new inbox for FD write request events. */
#define n00b_conduit_fd_write_req_inbox_new(c)                                 \
    ({                                                                         \
        n00b_conduit_fd_write_req_inbox_t *_inbox =                            \
            n00b_alloc_with_opts(n00b_conduit_fd_write_req_inbox_t,            \
                &(n00b_alloc_opts_t){.allocator = (c)->allocator});            \
        n00b_conduit_inbox_init(n00b_conduit_fd_write_req_payload_t,           \
                                _inbox, c, N00B_CONDUIT_BP_UNBOUNDED, 0);      \
        _inbox;                                                                \
    })

/** @brief Create a new inbox for FD write done events. */
#define n00b_conduit_fd_write_done_inbox_new(c)                                \
    ({                                                                         \
        n00b_conduit_fd_write_done_inbox_t *_inbox =                           \
            n00b_alloc_with_opts(n00b_conduit_fd_write_done_inbox_t,           \
                &(n00b_alloc_opts_t){.allocator = (c)->allocator});            \
        n00b_conduit_inbox_init(n00b_conduit_fd_write_done_payload_t,          \
                                _inbox, c, N00B_CONDUIT_BP_UNBOUNDED, 0);      \
        _inbox;                                                                \
    })

/** @brief Create a new inbox for FD stream events. */
#define n00b_conduit_fd_stream_inbox_new(c)                                    \
    ({                                                                         \
        n00b_conduit_fd_stream_inbox_t *_inbox =                               \
            n00b_alloc_with_opts(n00b_conduit_fd_stream_inbox_t,               \
                &(n00b_alloc_opts_t){.allocator = (c)->allocator});            \
        n00b_conduit_inbox_init(n00b_conduit_fd_stream_payload_t,              \
                                _inbox, c, N00B_CONDUIT_BP_UNBOUNDED, 0);      \
        _inbox;                                                                \
    })

// ============================================================================
// Convenience subscribe macros
// ============================================================================

/** @brief Subscribe to FD status events. */
#define n00b_conduit_fd_status_subscribe(topic, inbox, ...)                    \
    n00b_conduit_subscribe(n00b_conduit_fd_status_payload_t,                   \
                           (n00b_conduit_topic_t(n00b_conduit_fd_status_payload_t) *)(topic), \
                           inbox, __VA_ARGS__)

/** @brief Subscribe to FD write completion events. */
#define n00b_conduit_fd_write_subscribe(topic, inbox, ...)                    \
    n00b_conduit_subscribe(n00b_conduit_fd_write_payload_t,                   \
                           (n00b_conduit_topic_t(n00b_conduit_fd_write_payload_t) *)(topic), \
                           inbox, __VA_ARGS__)

// ============================================================================
// Convenience pop macros
// ============================================================================

#define n00b_conduit_fd_status_inbox_pop(inbox) \
    n00b_conduit_inbox_pop_msg(n00b_conduit_fd_status_payload_t, inbox)
#define n00b_conduit_fd_write_inbox_pop(inbox) \
    n00b_conduit_inbox_pop_msg(n00b_conduit_fd_write_payload_t, inbox)
#define n00b_conduit_fd_write_req_inbox_pop(inbox) \
    n00b_conduit_inbox_pop_msg(n00b_conduit_fd_write_req_payload_t, inbox)
#define n00b_conduit_fd_write_done_inbox_pop(inbox) \
    n00b_conduit_inbox_pop_msg(n00b_conduit_fd_write_done_payload_t, inbox)
#define n00b_conduit_fd_stream_inbox_pop(inbox) \
    n00b_conduit_inbox_pop_msg(n00b_conduit_fd_stream_payload_t, inbox)

// ============================================================================
// Convenience has_messages macros
// ============================================================================

#define n00b_conduit_fd_status_inbox_has_messages(inbox) \
    n00b_conduit_inbox_has_msg(n00b_conduit_fd_status_payload_t, inbox)
#define n00b_conduit_fd_write_inbox_has_messages(inbox) \
    n00b_conduit_inbox_has_msg(n00b_conduit_fd_write_payload_t, inbox)
#define n00b_conduit_fd_stream_inbox_has_messages(inbox) \
    n00b_conduit_inbox_has_msg(n00b_conduit_fd_stream_payload_t, inbox)

// ============================================================================
// FD Lifecycle States
// ============================================================================

/**
 * @brief FD lifecycle states.
 */
typedef enum {
    N00B_CONDUIT_FD_ACTIVE,       /**< FD is open for read and write */
    N00B_CONDUIT_FD_READ_CLOSED,  /**< Read side has been closed */
    N00B_CONDUIT_FD_WRITE_CLOSED, /**< Write side has been closed */
    N00B_CONDUIT_FD_CLOSED,       /**< FD is fully closed */
} n00b_conduit_fd_state_t;

// ============================================================================
// FD owner dispatch operations
// ============================================================================

/** @brief FD is readable (has data available). */
#define N00B_CONDUIT_FD_OP_READ_DATA   (1 << 0)
/** @brief FD is writable (can accept data). */
#define N00B_CONDUIT_FD_OP_WRITE_DATA  (1 << 1)
/** @brief FD status change (EOF, error, closed). */
#define N00B_CONDUIT_FD_OP_STATUS      (1 << 2)

// ============================================================================
// Write Queue Entry
// ============================================================================

/**
 * @brief A single pending write in the FD owner's FIFO write queue.
 *
 * Tracks partial writes across poll cycles so that short writes and
 * EAGAIN never lose data.
 */
typedef struct n00b_conduit_write_entry {
    const uint8_t  *data;          /**< Data pointer (GC-managed copy) */
    size_t          total_len;     /**< Total length of the write */
    size_t          bytes_sent;    /**< Bytes already written */
    uint64_t        request_id;    /**< Correlation ID */
    void           *reply_inbox;   /**< Where to send completion */
    bool          (*reply_push)(void *, void *); /**< Push function */
    struct n00b_conduit_write_entry *next; /**< FIFO linkage */
} n00b_conduit_write_entry_t;

// ============================================================================
// FD Owner Structure
// ============================================================================

/**
 * @brief FD owner -- manages the full lifecycle of a single file descriptor.
 */
struct n00b_conduit_fd_owner {
    n00b_conduit_t              *conduit;        /**< Parent conduit */
    n00b_conduit_io_backend_t   *io;             /**< I/O backend */
    base_socket_t                fd;             /**< Managed native descriptor */
    bool                         close_on_done;  /**< Close FD when done */
#ifdef _WIN32
    bool                         win_socket;     /**< True when fd is a Winsock SOCKET */
#endif

    _Atomic(int)                 state;          /**< n00b_conduit_fd_state_t */

    n00b_conduit_topic_base_t   *read_topic;     /**< Read data events */
    n00b_conduit_topic_base_t   *write_topic;    /**< Write completion events */
    n00b_conduit_topic_base_t   *status_topic;   /**< Status events (EOF, err) */
    n00b_conduit_topic_base_t   *wreq_topic;     /**< Write requests (external monitoring) */

    /** @name Write queue (FIFO)
     *  Tracks pending writes across poll cycles for short-write safety.
     *  @{ */
    n00b_conduit_write_entry_t  *wq_head;        /**< Oldest pending write */
    n00b_conduit_write_entry_t  *wq_tail;        /**< Newest pending write */
    /** @} */

    _Atomic(uint64_t)            read_pos;       /**< Read stream position */
    _Atomic(uint64_t)            write_pos;      /**< Write stream position */
    _Atomic(uint64_t)            next_request_id; /**< Write request ID counter */
    _Atomic(bool)                read_active;    /**< True if reads activated */
    _Atomic(bool)                write_active;   /**< True if writes activated */
    _Atomic(bool)                write_draining; /**< True while a writer drains wq */
    _Atomic(bool)                read_again;     /**< A read pass was asked for while another held the read claim */
    _Atomic(bool)                registry_registered; /**< True while in c->fd_owners */
    _Atomic(bool)                native_released; /**< True after owned FD was released */
    _Atomic(uint64_t)            close_generation; /**< 0 until teardown starts */
    _Atomic(uint32_t)            terminal_status_count; /**< Terminal close statuses sent */

    n00b_conduit_io_target_t    *io_target;       /**< Variant stored in IO backend (GC root) */

    void                       (*on_first_writable)(n00b_conduit_fd_owner_t *owner,
                                                    void *ctx);
    void                        *on_first_writable_ctx;

};

// ============================================================================
// Stream Reader (Layer 2)
// ============================================================================

/**
 * @brief Pending read request in the stream reader.
 */
typedef struct n00b_conduit_stream_request {
    size_t    requested;         /**< Bytes requested (0 = delimiter mode) */
    uint8_t   delimiter;         /**< Delimiter byte for read_until */
    size_t    max_bytes;         /**< Max bytes for read_until */
    bool      use_delimiter;     /**< True = read_until, false = read N */
    void     *reply_inbox;       /**< Inbox for completion event */
    bool    (*reply_push)(void *inbox, void *msg); /**< Push function */
    struct n00b_conduit_stream_request *next; /**< FIFO linkage */
} n00b_conduit_stream_request_t;

/**
 * @brief Stream reader -- accumulates Layer 1 as-done buffers into
 *        Layer 2 responses.
 */
struct n00b_conduit_stream_reader {
    n00b_conduit_t                *conduit;
    n00b_conduit_fd_owner_t       *owner;

    n00b_conduit_inbox_t(n00b_buffer_t *) *internal_inbox;
    n00b_conduit_sub_handle_t      sub_handle;

    n00b_buffer_t                 *accum;
    uint64_t                       accum_pos;

    n00b_conduit_stream_request_t *pending_head;
    n00b_conduit_stream_request_t *pending_tail;

    bool                           eof;
    bool                           error;
    int                            error_code;
};

// ============================================================================
// Layer 1 API
// ============================================================================

/**
 * @brief Manage an FD -- creates owner, 4 topics, registers with I/O backend.
 * @param c             Conduit instance.
 * @param io            I/O backend for this FD.
 * @param fd            File descriptor to manage.
 * @param close_on_done If true, close FD when owner is destroyed.
 * @return Ok(owner) on success, or Err(errno) on failure.
 */
extern n00b_result_t(n00b_conduit_fd_owner_t *)
n00b_conduit_fd_manage(n00b_conduit_t *c, n00b_conduit_io_backend_t *io,
                       base_socket_t fd, bool close_on_done);

/**
 * @brief Lookup owner for a managed FD.
 * @param c  Conduit instance.
 * @param fd File descriptor to look up.
 * @return Some(owner) if found, None otherwise.
 */
extern n00b_option_t(n00b_conduit_fd_owner_t *)
n00b_conduit_fd_get_owner(n00b_conduit_t *c, base_socket_t fd);

/**
 * @brief Get read topic for direct Layer 1 subscription (untyped base).
 * @return Ok(topic) on success, Err on null owner.
 */
extern n00b_result_t(n00b_conduit_topic_base_t *)
n00b_conduit_fd_read_topic(n00b_conduit_fd_owner_t *owner);

/**
 * @brief Get typed read topic for direct buffer subscription.
 *
 * Returns the same topic as `n00b_conduit_fd_read_topic` but with
 * the correct parameterized type (`n00b_buffer_t *`), suitable for
 * wiring into transforms, fd_writer, or direct subscription.
 */
static inline n00b_conduit_topic_t(n00b_buffer_t *) *
n00b_conduit_fd_read_topic_typed(n00b_conduit_fd_owner_t *owner)
{
    auto r = n00b_conduit_fd_read_topic(owner);
    return n00b_result_is_ok(r)
        ? (n00b_conduit_topic_t(n00b_buffer_t *) *)n00b_result_get(r)
        : nullptr;
}

/**
 * @brief Get write topic for direct Layer 1 subscription (untyped base).
 * @return Ok(topic) on success, Err on null owner.
 */
extern n00b_result_t(n00b_conduit_topic_base_t *)
n00b_conduit_fd_write_topic(n00b_conduit_fd_owner_t *owner);

/**
 * @brief Get typed write topic for direct transform subscription.
 */
static inline n00b_conduit_topic_t(n00b_conduit_fd_write_payload_t) *
n00b_conduit_fd_write_topic_typed(n00b_conduit_fd_owner_t *owner)
{
    auto r = n00b_conduit_fd_write_topic(owner);
    return n00b_result_is_ok(r)
        ? (n00b_conduit_topic_t(n00b_conduit_fd_write_payload_t) *)n00b_result_get(r)
        : nullptr;
}

/**
 * @brief Get status topic for direct Layer 1 subscription (untyped base).
 * @return Ok(topic) on success, Err on null owner.
 */
extern n00b_result_t(n00b_conduit_topic_base_t *)
n00b_conduit_fd_status_topic(n00b_conduit_fd_owner_t *owner);

/**
 * @brief Get typed status topic for direct transform subscription.
 */
static inline n00b_conduit_topic_t(n00b_conduit_fd_status_payload_t) *
n00b_conduit_fd_status_topic_typed(n00b_conduit_fd_owner_t *owner)
{
    auto r = n00b_conduit_fd_status_topic(owner);
    return n00b_result_is_ok(r)
        ? (n00b_conduit_topic_t(n00b_conduit_fd_status_payload_t) *)n00b_result_get(r)
        : nullptr;
}

/**
 * @brief Submit a write request (non-blocking).
 * @param owner       FD owner.
 * @param data        Data to write (caller retains ownership; contents are
 *                    copied internally before the call returns).
 * @param len         Length in bytes.
 * @param reply_inbox Inbox for completion event (borrowed — must remain valid
 *                    until the write-complete message arrives).
 * @param reply_push  Push function for reply.
 * @return Ok(request_id) for correlation, or Err(errno) on failure.
 *
 * @pre  @p owner is non-null and was returned by @c n00b_conduit_fd_manage.
 * @post On success, a write-complete event will be published to @p reply_inbox
 *       when the data has been handed to the kernel.
 */
extern n00b_result_t(uint64_t)
n00b_conduit_fd_write_submit(n00b_conduit_fd_owner_t *owner,
                             const void *data, size_t len,
                             void *reply_inbox,
                             bool (*reply_push)(void *, void *));

/**
 * @brief Activate reads on a managed FD.
 *
 * Called automatically when subscribing to the read topic or creating
 * a stream reader.
 */
extern void
n00b_conduit_fd_activate_reads(n00b_conduit_fd_owner_t *owner);

/**
 * @brief Deactivate read monitoring on a managed FD.
 *
 * CAS-guarded: only modifies the IO backend if reads are currently active.
 * Called automatically via the topic's `on_last_unsubscribe` callback when
 * the last subscriber is removed from the FD's read topic.
 */
extern void
n00b_conduit_fd_deactivate_reads(n00b_conduit_fd_owner_t *owner);

/**
 * @brief Activate write monitoring on a managed FD.
 *
 * CAS-guarded: only modifies the IO backend if writes are not already active.
 * The normal write path arms this implicitly via the write queue, but an
 * outbound non-blocking connect has no queued bytes yet — it needs the IO
 * backend watching for writability so the connect-completion hook
 * (`on_first_writable`) fires. `fd_owner_do_writes` disarms write monitoring
 * again once the queue drains, so this stays a one-shot arm for the connect.
 */
extern void
n00b_conduit_fd_activate_writes(n00b_conduit_fd_owner_t *owner);

/**
 * @internal Dispatch readiness event to managed FD owner.
 */
extern void
n00b_conduit_fd_owner_dispatch(n00b_conduit_fd_owner_t *owner, uint32_t io_ops);

/**
 * @brief Close and tear down an FD owner.
 *
 * Unwatches the FD from the IO backend, closes all four topics
 * (read, write, status, wreq), transitions to CLOSED state, and
 * closes the underlying FD if `close_on_done` was set.
 *
 * Idempotent: safe to call on an already-closed owner.
 *
 * @param owner  FD owner to close (may be null — no-op).
 */
extern void
n00b_conduit_fd_owner_close(n00b_conduit_fd_owner_t *owner);

/**
 * @brief Close and tear down an FD owner, reporting observed close failure.
 *
 * Performs the same owner teardown as @ref n00b_conduit_fd_owner_close. If the
 * owner has `close_on_done` set and this call closes the underlying host FD,
 * the host close error is reported as `Err(errno)`.
 *
 * @param owner FD owner to close. May be null.
 *
 * @return `Ok(true)` when this call newly closed @p owner without observed
 *         error, `Ok(false)` when @p owner was null or already closed, or
 *         `Err(errno)` for an observed host close failure.
 */
extern n00b_result_t(bool)
n00b_conduit_fd_owner_close_result(n00b_conduit_fd_owner_t *owner);

// ============================================================================
// Layer 2 API
// ============================================================================

/**
 * @brief Create a stream reader that accumulates as-done buffers.
 *
 * The reader consumes @p owner's read topic: it frees each chunk once the
 * bytes are copied into its accumulator, so it must be the topic's only
 * data subscriber.
 *
 * @return Ok(reader) on success, or Err(errno) on failure.
 */
extern n00b_result_t(n00b_conduit_stream_reader_t *)
n00b_conduit_stream_reader_new(n00b_conduit_t *c,
                               n00b_conduit_fd_owner_t *owner);

/**
 * @brief Destroy a stream reader, freeing its accumulator, pending
 *        requests, and undelivered chunks.
 */
extern void
n00b_conduit_stream_reader_destroy(n00b_conduit_stream_reader_t *reader);

/**
 * @brief Free a reply delivered by @ref n00b_conduit_stream_read or
 *        @ref n00b_conduit_stream_read_until.
 *
 * The reply's `payload.data` lives inside the message, so it is invalid
 * after this call.
 */
extern void
n00b_conduit_stream_msg_free(n00b_conduit_fd_stream_msg_t *msg);

/**
 * @brief Request N bytes (non-blocking — consumer blocks on their inbox).
 * @param reader      Stream reader.
 * @param nbytes      Number of bytes to request.
 * @param reply_inbox Inbox for the reply (borrowed — must remain valid
 *                    until the reply arrives).
 * @param reply_push  Push function for reply.
 *
 * @pre  @p reader was created via @c n00b_conduit_stream_reader_new.
 * @post Caller receives a message in @p reply_inbox with the requested data,
 *       and owns it: release it with @ref n00b_conduit_stream_msg_free.
 */
extern void
n00b_conduit_stream_read(n00b_conduit_stream_reader_t *reader, size_t nbytes,
                         void *reply_inbox,
                         bool (*reply_push)(void *, void *));

/**
 * @brief Read until delimiter (non-blocking).
 * @param reader      Stream reader.
 * @param delimiter   Byte to stop at (inclusive).
 * @param max_bytes   Upper bound (0 = unlimited).
 * @param reply_inbox Inbox for the reply (borrowed — must remain valid
 *                    until the reply arrives).
 * @param reply_push  Push function for reply.
 *
 * @pre  @p reader was created via @c n00b_conduit_stream_reader_new.
 * @post As for @ref n00b_conduit_stream_read, the caller owns the reply.
 */
extern void
n00b_conduit_stream_read_until(n00b_conduit_stream_reader_t *reader,
                               uint8_t delimiter, size_t max_bytes,
                               void *reply_inbox,
                               bool (*reply_push)(void *, void *));

/**
 * @brief Blocking write convenience (fd owner internal path).
 *
 * @param owner FD owner.
 * @param data  Data to write (caller retains ownership; copied internally).
 * @param len   Length in bytes.
 * @return n00b_result_t(int) with bytes written on success, or error.
 *
 * @pre  @p owner is non-null and has an active write topic.
 */
extern n00b_result_t(int)
n00b_fd_owner_write(n00b_conduit_fd_owner_t *owner,
                    const void *data, size_t len);

/**
 * @brief Blocking write convenience that preserves completion facts.
 *
 * Unlike @ref n00b_fd_owner_write, write errors that occur after bytes have
 * been accepted by the host are returned as `Ok(attempt)` with
 * `attempt.error == true` and `attempt.bytes_written` set to the completed
 * byte count. If no completion record arrives before the convenience wait
 * bound, the attempt carries `error_code == N00B_CONDUIT_ERR_TIMEOUT` and
 * `bytes_written == 0`: the request is still queued, and the owner goes on
 * delivering it, so the caller must not write those bytes again.
 *
 * Argument, allocation, and closed-owner failures before the write request is
 * queued return `Err(code)`.
 *
 * @param owner FD owner.
 * @param data  Data to write (caller retains ownership; copied internally).
 * @param len   Length in bytes.
 *
 * @return `Ok(attempt)` when a write request was queued, otherwise
 *         `Err(N00B_CONDUIT_ERR_*)`.
 */
extern n00b_result_t(n00b_fd_owner_write_attempt_t)
n00b_fd_owner_write_attempt(n00b_conduit_fd_owner_t *owner,
                            const void *data, size_t len);

/**
 * @brief Write out an FD owner's queued requests, waiting up to
 *        @p timeout_ms for the descriptor to take them.
 *
 * A request whose blocking write timed out stays queued; this is what
 * delivers it when nothing else will, such as at shutdown.
 *
 * @return true when the queue is empty, false on timeout or a closed owner.
 */
extern bool
n00b_conduit_fd_owner_flush(n00b_conduit_fd_owner_t *owner, int32_t timeout_ms);

#ifdef N00B_DEBUG
/**
 * @brief While @p hold is true, every owner's write drain returns without
 *        writing, leaving queued requests at the head of their queues.
 *        Only under @c N00B_DEBUG.
 *
 * Lets a test hold a request queued until a blocking write's completion wait
 * expires. Process-wide.
 */
extern void
n00b_conduit_fd_owner_hold_writes(bool hold);

/**
 * @brief Test-only: called at the end of each read pass, after the last
 *        read and before the pass gives up its claim on the read topic.
 *
 * The window between those two is where a read pass on another thread can
 * fail to claim, so a test can widen it deterministically.
 */
extern void (*n00b_fd_owner_read_pass_end_hook)(n00b_conduit_fd_owner_t *owner);
#endif

/**
 * @brief Blocking bulk-read: drain an FD owner's read topic to EOF.
 *
 * Subscribes to the owner's read topic, accumulates every published
 * `n00b_buffer_t *` chunk into a freshly-allocated buffer, and returns
 * once the IO thread signals end-of-stream (TOPIC_CLOSED on the read
 * inbox's sys queue, or an empty-payload chunk).  The function blocks
 * the calling thread on the inbox's condition variable while it waits
 * for chunks, releasing the inbox CV mutex between waits so the IO
 * thread can notify on close.
 *
 * Each chunk is freed once its bytes are copied, so the caller must be
 * the read topic's only data subscriber for the duration of the call.
 *
 * This is the read-side analogue of @ref n00b_fd_owner_write — both
 * are blocking convenience wrappers around the underlying Layer 1
 * primitives so callers don't have to manage subscription /
 * `TOPIC_CLOSED`-drain machinery by hand.
 *
 * @param owner  FD owner whose read topic to drain.
 *
 * @kw allocator Allocator for the returned buffer (default: runtime).
 *
 * @return @c Ok(buffer) on success — buffer is empty if the stream
 *         was already at EOF.  @c Err(code) on failure, where @p code
 *         is an `N00B_CONDUIT_ERR_*` code:
 *         - `N00B_CONDUIT_ERR_NULL_ARG` — @p owner is null.
 *         - `N00B_CONDUIT_ERR_FD_CLOSED` — read side already closed.
 *         - `N00B_CONDUIT_ERR_ALLOC` — could not allocate inbox or
 *           subscribe to the read topic.
 *         - `N00B_CONDUIT_ERR_IO` — IO thread reported a read error
 *           (status event with @c N00B_CONDUIT_FD_ST_READ_ERR).
 *
 * @pre  @p owner is non-null and was returned by
 *       @ref n00b_conduit_fd_manage; the owner's read side has not
 *       been closed.
 * @post On Ok, the returned buffer owns its bytes and is independent
 *       of @p owner's internal state.  The subscription is cancelled
 *       before return; the inbox is GC-reachable until the next sweep.
 */
extern n00b_result_t(n00b_buffer_t *)
n00b_fd_owner_read_all(n00b_conduit_fd_owner_t *owner) _kargs
{
    n00b_allocator_t *allocator = nullptr;
    int32_t           timeout_ms = 0;
};

/**
 * @brief Process pending data in a stream reader.
 */
extern void
n00b_conduit_stream_reader_process(n00b_conduit_stream_reader_t *reader);
