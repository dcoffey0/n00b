/**
 * @file chan.h
 * @brief A channel — one stream on one connection, with framing semantics.
 *
 * `n00b_quic_chan_t` is the application-visible unit on top of a QUIC
 * stream.  It carries one of three semantics, fixed at open time:
 *
 * - @c FRAMED — Length-prefixed, type-tagged frames (see @c framer.h).
 *   Suitable for request/reply or message-stream protocols.
 * - @c BYTES — Raw byte stream.  No framing.  Useful for tunneling.
 * - @c DGRAM — RFC 9221 unreliable datagram channel (per-channel
 *   datagram semantics; not yet implemented in this revision).
 *
 * ### What this revision ships
 *
 * Phase 1 (this pass): channel **open**, **send** (with optional
 * FIN), **state** observation, and **close** / reset / stop_sending
 * primitives.  *Receive* — the path that fans picoquic stream events
 * into a per-channel topic — ships in the follow-up.  Without recv
 * you can drive an outbound-only channel, which is a real (if narrow)
 * use case.
 *
 * ### Stream IDs
 *
 * Stream IDs are allocated by picoquic via
 * @c picoquic_get_next_local_stream_id; we do not pick them
 * ourselves.  Bidi vs uni and even/odd parity are handled per
 * RFC 9000.
 *
 * @see conn.h, framer.h, quic_types.h
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "n00b.h"
#include "adt/result.h"
#include "core/buffer.h"
#include "core/time.h"
#include "net/quic/quic_types.h"
#include "net/quic/stats.h"

/**
 * @brief Open a channel on a connection.
 *
 * Allocates a new channel handle, asks picoquic for the next
 * available stream ID, and registers the channel under the
 * connection.  No bytes flow until the caller calls
 * @c n00b_quic_chan_send.  Channels can be opened on a connection
 * still in @c CONNECTING — picoquic will queue the data until the
 * handshake completes.
 *
 * @param conn  Connection to open the channel on.
 *
 * @kw kind  Default: @c N00B_QUIC_CHAN_FRAMED.  See @c quic_types.h.
 * @kw bidi  Default: true.  When false, opens a uni-directional
 *           outbound (send-only) channel.
 * @kw zero_rtt Default: false.  Application asserts the channel
 *           contains only replay-safe data.  *Reserved; 0-RTT not
 *           yet plumbed end-to-end.*
 *
 * @return Result: ok with the new channel; err on connection-closed
 *         or invalid args.
 *
 * @pre @p conn is non-NULL and not closed.
 * @post On success the caller owns the handle; close with
 *       @c n00b_quic_chan_close (or one of the abrupt-termination
 *       variants).
 */
extern n00b_result_t(n00b_quic_chan_t *)
    n00b_quic_chan_open(n00b_quic_conn_t *conn)
    _kargs {
        n00b_quic_chan_kind_t kind     = N00B_QUIC_CHAN_FRAMED;
        bool                  bidi     = true;
        bool                  zero_rtt = false;
    };

/**
 * @brief Stream ID assigned by picoquic at open time.
 *
 * Useful for diagnostics and qlog correlation.  Returns
 * @c UINT64_MAX for closed / NULL channels.
 */
extern uint64_t
n00b_quic_chan_id(n00b_quic_chan_t *chan);

/**
 * @brief Channel kind (set at open).
 */
extern n00b_quic_chan_kind_t
n00b_quic_chan_kind(n00b_quic_chan_t *chan);

/**
 * @brief Snapshot per-channel stats.
 *
 * Returns a self-consistent point-in-time view: bytes sent/acked/
 * received, current state, peer/local error codes (if any), and
 * the last-activity timestamp.  Useful for stuck-stream detection
 * — a streams without recent activity that's still in @c OPEN may
 * indicate flow-control starvation or peer-side stall.
 *
 * @param chan Channel handle.
 * @return Stats; zeroed on a closed/NULL channel.
 */
extern n00b_quic_stream_stats_t
n00b_quic_chan_stats(n00b_quic_chan_t *chan);

/**
 * @brief Observable channel state.
 *
 * Returns one of @c n00b_quic_chan_state_t.  See @c quic_types.h
 * for the lifecycle.
 */
extern n00b_quic_chan_state_t
n00b_quic_chan_state(n00b_quic_chan_t *chan);

/**
 * @brief Queue bytes for sending on the channel.
 *
 * Hands @p payload to picoquic for transmission on this channel's
 * stream.  Returns the number of bytes accepted.  Picoquic does its
 * own bounded queueing internally, so this rarely returns short for
 * small payloads, but callers should still inspect the result.
 *
 * @param chan        Channel handle.
 * @param payload     Bytes to send.  May be NULL only if @p len == 0.
 * @param len         Number of payload bytes.
 *
 * @kw fin  Default: false.  When true, FIN this direction after the
 *          payload — no more sends from this side.
 *
 * @return Result of size_t: ok(N) where N is bytes accepted (today
 *         always equals @p len on success); err on closed channel
 *         or picoquic error.
 *
 * @pre @p chan is non-NULL and not in a terminal state.
 */
extern n00b_result_t(size_t)
    n00b_quic_chan_send(n00b_quic_chan_t *chan,
                        const uint8_t    *payload,
                        size_t            len)
    _kargs {
        bool fin = false;
    };

/**
 * @brief Pull received bytes from the channel into a caller buffer.
 *
 * Copies up to @p max bytes that have arrived on this stream into
 * @p out and consumes them from the channel's recv buffer.  Returns
 * 0 if no bytes are currently available.  This call does **not**
 * block; it always returns the bytes that are buffered at call time.
 *
 * A channel has one reader.  This call, @c n00b_quic_chan_has_data,
 * and @c n00b_quic_chan_recv_fin may run on any thread, including one
 * other than the endpoint pump, but never on two threads at once for
 * the same channel; serialize them, as the H3 drivers do under their
 * drive lock.  Debug builds assert on a second concurrent reader.
 *
 * @param chan Channel handle.
 * @param out  Destination buffer.
 * @param max  Maximum bytes to copy.
 *
 * @return Result of size_t: ok with the number of bytes copied
 *         (0 ≤ N ≤ @p max); err on closed channel or NULL args.
 *
 * @pre @p chan is non-NULL and not in a terminal state.
 * @pre @p out is non-NULL when @p max > 0.
 */
extern n00b_result_t(size_t)
n00b_quic_chan_recv(n00b_quic_chan_t *chan, uint8_t *out, size_t max);

/**
 * @brief Are there bytes available in the channel's recv buffer?
 *
 * Cheap predicate; useful in poll loops.  A reader call: see
 * @c n00b_quic_chan_recv.
 *
 * @param chan Channel handle.
 * @return true iff at least one byte is buffered.
 */
extern bool
n00b_quic_chan_has_data(n00b_quic_chan_t *chan);

/**
 * @brief Has the reader consumed everything up to the peer's FIN?
 *
 * True once the peer has sent FIN and every byte it sent before the
 * FIN has been read, so a true result means no more data is coming.
 * Bytes still unread keep it false.  A reader call: see
 * @c n00b_quic_chan_recv.
 */
extern bool
n00b_quic_chan_recv_fin(n00b_quic_chan_t *chan);

/**
 * @brief Pop one received datagram from a DGRAM channel.
 *
 * RFC 9221 datagrams are atomic: each receive returns exactly one
 * datagram payload, or `Option::none` if the queue is empty.  No
 * concatenation across datagrams; no partial deliveries.
 *
 * Only valid on channels opened with `kind = N00B_QUIC_CHAN_DGRAM`.
 * Returns `Result::err(N00B_QUIC_ERR_INVALID_ARG)` for stream-kind
 * channels.
 *
 * @param chan DGRAM channel handle.
 *
 * @return Result of option of buffer:
 *           - ok+some(buf) on each successful pop;
 *           - ok+none when the receive queue is empty;
 *           - err(INVALID_ARG) on non-DGRAM channel or closed.
 *
 * @pre @p chan is non-NULL, open, and DGRAM kind.
 */
extern n00b_result_t(n00b_option_t(n00b_buffer_t *))
n00b_quic_chan_recv_dgram(n00b_quic_chan_t *chan);

/**
 * @brief Send a CONNECTION_CLOSE-equivalent for this stream.
 *
 * Signals the peer that we abandon this stream's outgoing data;
 * picoquic emits a RESET_STREAM frame with @p app_err.  Idempotent.
 *
 * @param chan    Channel handle.
 * @param app_err 62-bit application-defined error code.
 *
 * @return Result of bool: ok(true) on success; err on invalid args
 *         or already-closed channel.
 */
extern n00b_result_t(bool)
n00b_quic_chan_reset(n00b_quic_chan_t *chan, uint64_t app_err);

/**
 * @brief Ask the peer to stop sending on this channel.
 *
 * Picoquic emits a STOP_SENDING frame with @p app_err.  The peer
 * is expected to reset its sending side.  Idempotent.
 *
 * @param chan    Channel handle.
 * @param app_err 62-bit application-defined error code.
 *
 * @return Result of bool: ok(true) on success; err on invalid args
 *         or already-closed channel.
 */
extern n00b_result_t(bool)
n00b_quic_chan_stop_sending(n00b_quic_chan_t *chan, uint64_t app_err);

/**
 * @brief Send FIN if needed and mark the channel locally closed.
 *
 * Idempotent.  After return the channel is in @c CLOSED locally;
 * peer ack and remote-FIN may still arrive but are handled by
 * picoquic without further app involvement.
 *
 * @param chan Channel handle (may be NULL).
 */
extern void
n00b_quic_chan_close(n00b_quic_chan_t *chan);

/**
 * @brief Owning connection of a channel.
 */
extern n00b_quic_conn_t *
n00b_quic_chan_conn(n00b_quic_chan_t *chan);

/**
 * @brief Next channel on the same connection (intrusive list walk).
 *
 * Pairs with @c n00b_quic_conn_first_chan for iteration.  Returns
 * NULL at end of list.
 *
 * @param chan Channel handle.
 * @return Next channel on this connection, or NULL.
 */
extern n00b_quic_chan_t *
n00b_quic_chan_next_in_conn(n00b_quic_chan_t *chan);
