/**
 * @file topic.h
 * @brief Topic identification, URI system, and parameterized topic type.
 *
 * Topics are uniquely identifiable, linearizable event resources. Each topic
 * has a URI (integer or string via variant) and a generation counter to handle
 * resource reuse (e.g., file descriptor recycling).
 *
 * @c n00b_conduit_topic_t(T) is the full parameterized topic struct,
 * containing a typed subscription list and typed inbox.  The common
 * (non-parameterized) fields are duplicated in every instantiation
 * with identical layout so that code operating only on common fields
 * can cast any @c n00b_conduit_topic_t(T)* to
 * @c n00b_conduit_topic_base_t* safely.
 */
#pragma once

#include "conduit/subscription.h"
#include "conduit/publisher.h"
#include "adt/variant.h"
#include "core/string.h"

// ============================================================================
// URI type (variant of uint64_t | n00b_string_t)
// ============================================================================

typedef n00b_variant_t(uint64_t, n00b_string_t *) n00b_conduit_uri_t;

// ============================================================================
// Integer URI type tags (top 16 bits of 64-bit integer)
// ============================================================================

typedef enum : uint16_t {
    N00B_CONDUIT_TAG_FD,
    N00B_CONDUIT_TAG_TIMER,
    N00B_CONDUIT_TAG_SIGNAL,
    N00B_CONDUIT_TAG_FD_READ,
    N00B_CONDUIT_TAG_FD_WRITE,
    N00B_CONDUIT_TAG_FD_STATUS,
    N00B_CONDUIT_TAG_FD_WREQ,
    N00B_CONDUIT_TAG_SOCK_ACCEPT,
    N00B_CONDUIT_TAG_SOCK_STATUS,
    N00B_CONDUIT_TAG_LOCAL_ACCEPT,
    N00B_CONDUIT_TAG_LOCAL_READ,
    N00B_CONDUIT_TAG_LOCAL_WRITE,
    N00B_CONDUIT_TAG_LOCAL_STATUS,
    N00B_CONDUIT_TAG_PROC,
    N00B_CONDUIT_TAG_VNODE,
    N00B_CONDUIT_TAG_USER_EVENT,
    N00B_CONDUIT_TAG_XFORM,        /**< Pipeline transform */
    N00B_CONDUIT_TAG_DONE,         /**< Completion notification topic */
    N00B_CONDUIT_TAG_UDP_RECV,     /**< UDP socket recv (datagram in) */
    N00B_CONDUIT_TAG_QUIC_ACCEPT,  /**< QUIC server endpoint: new conn accepted */
    N00B_CONDUIT_TAG_QUIC_OUTBOUND,/**< QUIC endpoint: outbound queued sends */
} n00b_conduit_int_uri_tag_t;

/** @brief Mask for extracting URI tag (top 16 bits) */
#define N00B_CONDUIT_URI_TAG_MASK 0xFFFF000000000000ULL
/** @brief Mask for extracting URI ID (bottom 48 bits) */
#define N00B_CONDUIT_URI_ID_MASK  0x0000FFFFFFFFFFFFULL

/** @brief Extract tag from integer URI */
#define N00B_CONDUIT_URI_TAG(uri) ((uri) & N00B_CONDUIT_URI_TAG_MASK)
/** @brief Extract ID from integer URI */
#define N00B_CONDUIT_URI_ID(uri)  ((uri) & N00B_CONDUIT_URI_ID_MASK)

// ============================================================================
// URI type-check macros
// ============================================================================

#define N00B_CONDUIT_URI_IS_FD(uri) \
    (N00B_CONDUIT_URI_TAG(uri) == ((uint64_t)N00B_CONDUIT_TAG_FD << 48))
#define N00B_CONDUIT_URI_IS_TIMER(uri) \
    (N00B_CONDUIT_URI_TAG(uri) == ((uint64_t)N00B_CONDUIT_TAG_TIMER << 48))
#define N00B_CONDUIT_URI_IS_SIGNAL(uri) \
    (N00B_CONDUIT_URI_TAG(uri) == ((uint64_t)N00B_CONDUIT_TAG_SIGNAL << 48))
#define N00B_CONDUIT_URI_IS_FD_READ(uri) \
    (N00B_CONDUIT_URI_TAG(uri) == ((uint64_t)N00B_CONDUIT_TAG_FD_READ << 48))
#define N00B_CONDUIT_URI_IS_FD_WRITE(uri) \
    (N00B_CONDUIT_URI_TAG(uri) == ((uint64_t)N00B_CONDUIT_TAG_FD_WRITE << 48))
#define N00B_CONDUIT_URI_IS_FD_STATUS(uri) \
    (N00B_CONDUIT_URI_TAG(uri) == ((uint64_t)N00B_CONDUIT_TAG_FD_STATUS << 48))
#define N00B_CONDUIT_URI_IS_FD_WREQ(uri) \
    (N00B_CONDUIT_URI_TAG(uri) == ((uint64_t)N00B_CONDUIT_TAG_FD_WREQ << 48))
#define N00B_CONDUIT_URI_IS_SOCK_ACCEPT(uri) \
    (N00B_CONDUIT_URI_TAG(uri) == ((uint64_t)N00B_CONDUIT_TAG_SOCK_ACCEPT << 48))
#define N00B_CONDUIT_URI_IS_SOCK_STATUS(uri) \
    (N00B_CONDUIT_URI_TAG(uri) == ((uint64_t)N00B_CONDUIT_TAG_SOCK_STATUS << 48))
#define N00B_CONDUIT_URI_IS_LOCAL_ACCEPT(uri) \
    (N00B_CONDUIT_URI_TAG(uri) == ((uint64_t)N00B_CONDUIT_TAG_LOCAL_ACCEPT << 48))
#define N00B_CONDUIT_URI_IS_LOCAL_READ(uri) \
    (N00B_CONDUIT_URI_TAG(uri) == ((uint64_t)N00B_CONDUIT_TAG_LOCAL_READ << 48))
#define N00B_CONDUIT_URI_IS_LOCAL_WRITE(uri) \
    (N00B_CONDUIT_URI_TAG(uri) == ((uint64_t)N00B_CONDUIT_TAG_LOCAL_WRITE << 48))
#define N00B_CONDUIT_URI_IS_LOCAL_STATUS(uri) \
    (N00B_CONDUIT_URI_TAG(uri) == ((uint64_t)N00B_CONDUIT_TAG_LOCAL_STATUS << 48))
#define N00B_CONDUIT_URI_IS_PROC(uri) \
    (N00B_CONDUIT_URI_TAG(uri) == ((uint64_t)N00B_CONDUIT_TAG_PROC << 48))
#define N00B_CONDUIT_URI_IS_VNODE(uri) \
    (N00B_CONDUIT_URI_TAG(uri) == ((uint64_t)N00B_CONDUIT_TAG_VNODE << 48))
#define N00B_CONDUIT_URI_IS_USER_EVENT(uri) \
    (N00B_CONDUIT_URI_TAG(uri) == ((uint64_t)N00B_CONDUIT_TAG_USER_EVENT << 48))

// ============================================================================
// URI constructors
// ============================================================================

static inline n00b_conduit_uri_t
n00b_conduit_int_uri(uint16_t tag, uint64_t id)
{
    uint64_t uri = (uint64_t)tag;

    uri <<= 48;
    uri |= (id & N00B_CONDUIT_URI_ID_MASK);

    return n00b_variant_set(n00b_conduit_uri_t, uint64_t, uri);
}

static inline n00b_conduit_uri_t
n00b_conduit_str_uri(n00b_string_t *s)
{
    return n00b_variant_set(n00b_conduit_uri_t, n00b_string_t *, s);
}

/** @brief Convenience: create a file descriptor URI */
#define N00B_CONDUIT_URI_FD(fd) \
    n00b_conduit_int_uri(N00B_CONDUIT_TAG_FD, (uint64_t)(fd))
/** @brief Convenience: create a timer URI */
#define N00B_CONDUIT_URI_TIMER(id) \
    n00b_conduit_int_uri(N00B_CONDUIT_TAG_TIMER, (uint64_t)(id))
/** @brief Convenience: create a signal URI */
#define N00B_CONDUIT_URI_SIGNAL(n) \
    n00b_conduit_int_uri(N00B_CONDUIT_TAG_SIGNAL, (uint64_t)(n))

/** @brief Convenience: create an FD-read URI */
#define N00B_CONDUIT_URI_FD_READ(fd) \
    n00b_conduit_int_uri(N00B_CONDUIT_TAG_FD_READ, (uint64_t)(fd))
/** @brief Convenience: create an FD-write URI */
#define N00B_CONDUIT_URI_FD_WRITE(fd) \
    n00b_conduit_int_uri(N00B_CONDUIT_TAG_FD_WRITE, (uint64_t)(fd))
/** @brief Convenience: create an FD-status URI */
#define N00B_CONDUIT_URI_FD_STATUS(fd) \
    n00b_conduit_int_uri(N00B_CONDUIT_TAG_FD_STATUS, (uint64_t)(fd))
/** @brief Convenience: create an FD write-request URI */
#define N00B_CONDUIT_URI_FD_WREQ(fd) \
    n00b_conduit_int_uri(N00B_CONDUIT_TAG_FD_WREQ, (uint64_t)(fd))
/** @brief Convenience: create a socket accept URI */
#define N00B_CONDUIT_URI_SOCK_ACCEPT(id) \
    n00b_conduit_int_uri(N00B_CONDUIT_TAG_SOCK_ACCEPT, (uint64_t)(id))
/** @brief Convenience: create a socket status URI */
#define N00B_CONDUIT_URI_SOCK_STATUS(fd) \
    n00b_conduit_int_uri(N00B_CONDUIT_TAG_SOCK_STATUS, (uint64_t)(fd))
/** @brief Convenience: create a local IPC accept URI */
#define N00B_CONDUIT_URI_LOCAL_ACCEPT(id) \
    n00b_conduit_int_uri(N00B_CONDUIT_TAG_LOCAL_ACCEPT, (uint64_t)(id))
/** @brief Convenience: create a local IPC read URI */
#define N00B_CONDUIT_URI_LOCAL_READ(id) \
    n00b_conduit_int_uri(N00B_CONDUIT_TAG_LOCAL_READ, (uint64_t)(id))
/** @brief Convenience: create a local IPC write URI */
#define N00B_CONDUIT_URI_LOCAL_WRITE(id) \
    n00b_conduit_int_uri(N00B_CONDUIT_TAG_LOCAL_WRITE, (uint64_t)(id))
/** @brief Convenience: create a local IPC status URI */
#define N00B_CONDUIT_URI_LOCAL_STATUS(id) \
    n00b_conduit_int_uri(N00B_CONDUIT_TAG_LOCAL_STATUS, (uint64_t)(id))
/** @brief Convenience: create a process URI */
#define N00B_CONDUIT_URI_PROC(pid) \
    n00b_conduit_int_uri(N00B_CONDUIT_TAG_PROC, (uint64_t)(pid))
/** @brief Convenience: create a vnode (file change) URI */
#define N00B_CONDUIT_URI_VNODE(fd) \
    n00b_conduit_int_uri(N00B_CONDUIT_TAG_VNODE, (uint64_t)(fd))
/** @brief Convenience: create a user event URI */
#define N00B_CONDUIT_URI_USER_EVENT(id) \
    n00b_conduit_int_uri(N00B_CONDUIT_TAG_USER_EVENT, (uint64_t)(id))
/** @brief Convenience: create a pipeline transform URI */
#define N00B_CONDUIT_URI_XFORM(id) \
    n00b_conduit_int_uri(N00B_CONDUIT_TAG_XFORM, (uint64_t)(id))
/** @brief Convenience: create a completion topic URI */
#define N00B_CONDUIT_URI_DONE(id) \
    n00b_conduit_int_uri(N00B_CONDUIT_TAG_DONE, (uint64_t)(id))
/** @brief Convenience: create a UDP recv URI */
#define N00B_CONDUIT_URI_UDP_RECV(id) \
    n00b_conduit_int_uri(N00B_CONDUIT_TAG_UDP_RECV, (uint64_t)(id))
/** @brief Convenience: create a QUIC accept URI */
#define N00B_CONDUIT_URI_QUIC_ACCEPT(id) \
    n00b_conduit_int_uri(N00B_CONDUIT_TAG_QUIC_ACCEPT, (uint64_t)(id))
/** @brief Convenience: create a QUIC outbound-send URI */
#define N00B_CONDUIT_URI_QUIC_OUTBOUND(id) \
    n00b_conduit_int_uri(N00B_CONDUIT_TAG_QUIC_OUTBOUND, (uint64_t)(id))

// ============================================================================
// Publisher policies
// ============================================================================

typedef enum {
    N00B_CONDUIT_POLICY_OPEN,       /**< Any thread can claim publisher role */
    N00B_CONDUIT_POLICY_MUST_SERVE, /**< Designated publisher required */
    N00B_CONDUIT_POLICY_WILLING,    /**< Prefer designated, others can claim */
} n00b_conduit_policy_t;

// ============================================================================
// Topic state
// ============================================================================

typedef enum {
    N00B_CONDUIT_TOPIC_ACTIVE,  /**< Normal operation */
    N00B_CONDUIT_TOPIC_CLOSING, /**< In process of closing */
    N00B_CONDUIT_TOPIC_CLOSED,  /**< Closed, generation incremented */
} n00b_conduit_topic_state_t;

// ============================================================================
// Topic base — common fields shared by all topic instantiations.
//
// This struct contains everything except the typed subscription list
// and typed inbox.  Any n00b_conduit_topic_t(T)* can be cast to this
// because the common fields appear at identical offsets.
// ============================================================================

typedef struct n00b_conduit_topic_base {
    n00b_conduit_uri_t                  uri;
    _Atomic(uint64_t)                   generation;
    _Atomic(n00b_conduit_topic_state_t) state;
    _Atomic(uint64_t)                   epoch;
    _Atomic(n00b_conduit_publisher_t *) publisher;
    _Atomic(int)                        policy;
    _Atomic(uint64_t)                   pub_owner; /**< Owner's thread slot and generation; 0 if none. */
    n00b_futex_t                        pub_futex;
    _Atomic(uint32_t)                   pub_waiters;
    _Atomic(const char *)               debug_name;
    n00b_conduit_t                     *conduit;
    _Atomic(void *)                     sub_list_head; /**< Untyped per-topic subscription chain */
    _Atomic(void *)                     done_topic;    /**< Completion topic (write-done notifications) */
    _Atomic(void *)                     timer_owner;   /**< n00b_conduit_timer_t for timer topics. */
    void                              (*on_first_subscribe)(struct n00b_conduit_topic_base *topic,
                                                            void *ctx);
    void                               *on_first_subscribe_ctx;
    void                              (*on_last_unsubscribe)(struct n00b_conduit_topic_base *topic,
                                                             void *ctx);
    void                               *on_last_unsubscribe_ctx;
    /* The typed subscription list's rwlock is published here before the first
     * subscribe so base-level registration, cancellation, and close use the
     * same membership boundary as typed delivery. */
    _Atomic(n00b_rwlock_t *)            sub_delivery_lock;
} n00b_conduit_topic_base_t;

// ============================================================================
// Fully parameterized topic
//
// Extends the base with a typed subscription list and typed inbox.
// The common fields are duplicated at the same offsets so casting
// to n00b_conduit_topic_base_t* is safe.
// ============================================================================

#define n00b_conduit_topic_t(T) struct typeid("n00b_conduit_topic", T)

#define _N00B_TOPIC_FN(fn, T) typeid("n00b_conduit_topic_" #fn, T)

/**
 * @brief Instantiate a fully parameterized topic for payload type T.
 *
 * Generates the topic struct, typed subscription list, typed delivery
 * inlines, and typed subscribe helper.
 *
 * @pre N00B_CONDUIT_SUBSCRIPTION_IMPL(T) must have been called.
 */
#ifdef N00B_DEBUG
/**
 * @brief Called by every typed delivery for each subscription, after it has
 *        read the subscription's state and before it acts on it, while it
 *        holds the topic's subscription list. Only under @c N00B_DEBUG.
 */
extern void (*n00b_conduit_topic_deliver_hook)(n00b_conduit_topic_base_t *topic,
                                                void                      *sub);
#define _N00B_TOPIC_DELIVER_HOOK(topic, sub)                                     \
    do {                                                                         \
        if (n00b_conduit_topic_deliver_hook != nullptr) {                        \
            n00b_conduit_topic_deliver_hook((n00b_conduit_topic_base_t *)(topic), \
                                            (void *)(sub));                       \
        }                                                                        \
    } while (0)
#else
#define _N00B_TOPIC_DELIVER_HOOK(topic, sub) ((void)0)
#endif

#define N00B_CONDUIT_TOPIC_IMPL(T)                                                             \
    n00b_conduit_topic_t(T) {                                                                  \
        /* Common fields — same layout as n00b_conduit_topic_base_t */                         \
        n00b_conduit_uri_t                  uri;                                               \
        _Atomic(uint64_t)                   generation;                                        \
        _Atomic(n00b_conduit_topic_state_t) state;                                             \
        _Atomic(uint64_t)                   epoch;                                             \
        _Atomic(n00b_conduit_publisher_t *) publisher;                                         \
        _Atomic(int)                        policy;                                            \
        _Atomic(uint64_t)                   pub_owner;                                         \
        n00b_futex_t                        pub_futex;                                         \
        _Atomic(uint32_t)                   pub_waiters;                                       \
        _Atomic(const char *)               debug_name;                                        \
        n00b_conduit_t                     *conduit;                                           \
        _Atomic(void *)                     sub_list_head;                                     \
        _Atomic(void *)                     done_topic;                                        \
        _Atomic(void *)                     timer_owner;                                       \
        void                              (*on_first_subscribe)(struct n00b_conduit_topic_base *topic, \
                                                                void *ctx);                    \
        void                               *on_first_subscribe_ctx;                            \
        void                              (*on_last_unsubscribe)(struct n00b_conduit_topic_base *topic, \
                                                                 void *ctx);                   \
        void                               *on_last_unsubscribe_ctx;                           \
        _Atomic(n00b_rwlock_t *)            sub_delivery_lock;                                  \
        /* Type-specific fields */                                                             \
        n00b_list_t(n00b_conduit_subscription_t(T) *) subscriptions;                           \
        n00b_conduit_inbox_t(T)                       *inbox;                                  \
    };                                                                                         \
                                                                                               \
    /** @brief Deliver a typed message to all matching subscribers. */                         \
    static inline void                                                                         \
    _N00B_TOPIC_FN(deliver, T)(n00b_conduit_topic_t(T)    *topic,                              \
                               n00b_conduit_message_t(T)  *msg,                                \
                               uint32_t                    op_filter)                          \
    {                                                                                          \
        extern void n00b_conduit_sub_cancel(n00b_conduit_sub_handle_t handle);                  \
        bool _original_delivered = false;                                                       \
        auto _subs = &topic->subscriptions;                                                     \
        _n00b_list_write_lock(_subs);                                                           \
        size_t _write_i = 0;                                                                    \
        for (size_t _read_i = 0; _read_i < _subs->len; _read_i++) {                            \
            n00b_conduit_subscription_t(T) *sub = _subs->data[_read_i];                        \
            if (!sub)                                                                           \
                continue;                                                                      \
            int _state = n00b_atomic_load(&sub->state);                                        \
            _N00B_TOPIC_DELIVER_HOOK(topic, sub);                                              \
            if (_state == N00B_CONDUIT_SUB_REMOVED)                                            \
                continue;                                                                      \
            if (_state == N00B_CONDUIT_SUB_ACTIVE &&                                           \
                (op_filter == N00B_CONDUIT_OP_ALL || (sub->operations & op_filter))) {         \
                n00b_conduit_message_t(T) *_deliver_msg = msg;                                 \
                if (_original_delivered) {                                                     \
                    _deliver_msg = n00b_alloc_with_opts(                                       \
                        n00b_conduit_message_t(T),                                             \
                        &(n00b_alloc_opts_t){.allocator =                                      \
                            ((n00b_conduit_topic_base_t *)topic)->conduit->allocator});        \
                    *_deliver_msg = *msg;                                                      \
                    _deliver_msg->header.next = nullptr;                                       \
                }                                                                              \
                bool _delivered = n00b_conduit_sub_deliver(T, sub, _deliver_msg);               \
                if (_delivered && _deliver_msg == msg) {                                        \
                    _original_delivered = true;                                                 \
                }                                                                              \
                else if (!_delivered && _deliver_msg != msg) {                                  \
                    n00b_free_with_allocator_hint(                                               \
                        ((n00b_conduit_topic_base_t *)topic)->conduit->allocator,                \
                        _deliver_msg);                                                          \
                }                                                                              \
                if (n00b_atomic_load(&sub->state) == N00B_CONDUIT_SUB_REMOVED) {                \
                    n00b_conduit_sub_cancel(sub->handle);                                      \
                    continue;                                                                  \
                }                                                                              \
            }                                                                                  \
            _subs->data[_write_i++] = sub;                                                     \
        }                                                                                      \
        _subs->len = _write_i;                                                                 \
        _n00b_list_unlock(_subs);                                                              \
        if (!_original_delivered) {                                                            \
            n00b_free_with_allocator_hint(                                                       \
                ((n00b_conduit_topic_base_t *)topic)->conduit->allocator, msg);                  \
        }                                                                                      \
    }                                                                                          \
                                                                                               \
    /** @brief Deliver a system message to all matching subscribers. */                        \
    static inline void                                                                         \
    _N00B_TOPIC_FN(deliver_sys, T)(n00b_conduit_topic_t(T) *topic,                             \
                                   n00b_conduit_msg_type_t   type,                             \
                                   uint32_t                  op_filter)                        \
    {                                                                                          \
        auto _subs = &topic->subscriptions;                                                     \
        _n00b_list_write_lock(_subs);                                                           \
        size_t _write_i = 0;                                                                    \
        for (size_t _read_i = 0; _read_i < _subs->len; _read_i++) {                            \
            n00b_conduit_subscription_t(T) *sub = _subs->data[_read_i];                        \
            if (!sub)                                                                           \
                continue;                                                                      \
            int _state = n00b_atomic_load(&sub->state);                                        \
            if (_state == N00B_CONDUIT_SUB_REMOVED)                                            \
                continue;                                                                      \
            _subs->data[_write_i++] = sub;                                                     \
            if (_state != N00B_CONDUIT_SUB_ACTIVE)                                             \
                continue;                                                                      \
            if (op_filter != N00B_CONDUIT_OP_ALL &&                                            \
                !(sub->operations & op_filter))                                                \
                continue;                                                                      \
            if (sub->sys_queue) {                                                              \
                n00b_allocator_t *_msg_alloc =                                                  \
                    ((n00b_conduit_topic_base_t *)topic)->conduit->allocator;                  \
                n00b_conduit_sys_msg_t *sys = n00b_alloc_with_opts(                            \
                    n00b_conduit_sys_msg_t,                                                     \
                    &(n00b_alloc_opts_t){.allocator = _msg_alloc});                            \
                sys->header.type  = type;                                                      \
                sys->header.topic = (n00b_conduit_topic_base_t *)topic;                        \
                n00b_conduit_sys_queue_push(sub->sys_queue, sys);                              \
                if (sub->inbox && n00b_conduit_inbox_notify_enabled(sub->inbox)) {             \
                    n00b_condition_notify(&sub->inbox->cv, .auto_unlock = true);               \
                }                                                                               \
            }                                                                                  \
        }                                                                                      \
        _subs->len = _write_i;                                                                 \
        _n00b_list_unlock(_subs);                                                              \
    }                                                                                          \
                                                                                               \
    /** @brief Subscribe to a topic with a typed inbox. */                                     \
    static inline n00b_conduit_sub_handle_t                                                    \
    _N00B_TOPIC_FN(subscribe, T)(n00b_conduit_topic_t(T) *topic,                               \
                                 n00b_conduit_inbox_t(T)  *inbox,                              \
                                 n00b_conduit_sub_config_t config)                             \
    {                                                                                          \
        n00b_conduit_topic_base_t *_base = (n00b_conduit_topic_base_t *)topic;                 \
        n00b_rwlock_t *_lock = topic->subscriptions.lock;                                      \
        n00b_atomic_store(&_base->sub_delivery_lock, _lock);                                   \
        n00b_data_write_lock(_lock);                                                           \
        if (!n00b_conduit_topic_is_active(_base)) {                                            \
            n00b_data_unlock(_lock);                                                           \
            return N00B_CONDUIT_INVALID_SUB_HANDLE;                                           \
        }                                                                                      \
        n00b_conduit_subscription_t(T) *sub = n00b_alloc_with_opts(                              \
            n00b_conduit_subscription_t(T),                                                    \
            &(n00b_alloc_opts_t){.allocator =                                                  \
                _base->conduit->allocator});                                                   \
        if (!sub) {                                                                            \
            n00b_data_unlock(_lock);                                                           \
            return N00B_CONDUIT_INVALID_SUB_HANDLE;                                           \
        }                                                                                      \
        sub->handle           = n00b_conduit_sub_next_handle();                                \
        sub->inbox            = inbox;                                                         \
        sub->sys_queue        = &inbox->sys_queue;                                             \
        sub->operations       = config.operations ? config.operations                          \
                                                  : N00B_CONDUIT_OP_ALL;                       \
        sub->generation       = n00b_atomic_load(&topic->generation);                          \
        sub->epoch            = n00b_atomic_load(&topic->epoch);                               \
        sub->one_shot         = !!(config.flags & N00B_CONDUIT_SUB_F_ONE_SHOT);                \
        sub->dedicated_inbox  = false;                                                         \
        sub->notify_on_delivery = !!(config.flags & N00B_CONDUIT_SUB_F_NOTIFY_ON_DELIVERY);    \
        sub->confirm_cancel   = !!(config.flags & N00B_CONDUIT_SUB_F_CONFIRM_CANCEL);          \
        sub->notify_unsub     = !!(config.flags & N00B_CONDUIT_SUB_F_NOTIFY_UNSUB);            \
        sub->timeout_relative = !!(config.flags & N00B_CONDUIT_SUB_F_TIMEOUT_RELATIVE);        \
        sub->timeout_ms       = config.timeout_ms;                                             \
        sub->backpressure     = config.backpressure;                                           \
        sub->inbox_limit      = config.inbox_limit;                                            \
        sub->topic            = (n00b_conduit_topic_base_t *)topic;                              \
        sub->next_for_topic   = nullptr;                                                       \
        n00b_atomic_store(&sub->state, N00B_CONDUIT_SUB_ACTIVE);                               \
        n00b_list_push(topic->subscriptions, sub);                                             \
        extern void _n00b_conduit_sub_register(n00b_conduit_sub_handle_t,                      \
                                                void *,                                        \
                                                n00b_conduit_topic_base_t *);                  \
        _n00b_conduit_sub_register(sub->handle, sub, _base);                                   \
        n00b_data_unlock(_lock);                                                               \
        return sub->handle;                                                                    \
    }                                                                                          \
                                                                                               \
    /** @brief Create or retrieve a typed topic and initialize its fields.                     \
     *                                                                                         \
     * Also creates a completion (done) topic of type                                          \
     * `n00b_conduit_topic_base_t *`, stored on `base->done_topic`.                            \
     * Downstream consumers publish the originating topic pointer to                           \
     * the done topic after processing; synchronous writers subscribe                          \
     * one-shot and wait on the inbox CV.                                                      \
     */                                                                                        \
    static inline n00b_conduit_topic_t(T) *                                                    \
    _N00B_TOPIC_FN(init, T)(n00b_conduit_t     *c,                                             \
                             n00b_conduit_uri_t  uri)                                           \
    {                                                                                          \
        n00b_result_t(n00b_conduit_topic_base_t *) _tr =                                                      \
            n00b_conduit_topic_get(c, uri,                                                     \
                sizeof(n00b_conduit_topic_t(T)));                                              \
        if (n00b_result_is_err(_tr)) return nullptr;                                           \
        n00b_conduit_topic_t(T) *_tp =                                                         \
            (n00b_conduit_topic_t(T) *)n00b_result_get(_tr);                         \
        if (!_tp->subscriptions.data) {                                                        \
            _tp->subscriptions =                                                               \
                n00b_list_new(n00b_conduit_subscription_t(T) *, .allocator = c->allocator);     \
            _tp->inbox = nullptr;                                                              \
        }                                                                                      \
                                                                                               \
        /* Create a done topic (payload = n00b_conduit_topic_base_t *). */                     \
        n00b_conduit_topic_base_t *_base =                                                     \
            (n00b_conduit_topic_base_t *)_tp;                                                  \
        if (!n00b_atomic_load(&_base->done_topic)) {                                           \
            extern uint64_t n00b_conduit_next_done_id(void);                                   \
            uint64_t _did = n00b_conduit_next_done_id();                                       \
            n00b_result_t(n00b_conduit_topic_base_t *) _dtr =                                                 \
                n00b_conduit_topic_get(c, N00B_CONDUIT_URI_DONE(_did),                         \
                    sizeof(n00b_conduit_topic_t(n00b_conduit_topic_base_t *)));                \
            if (n00b_result_is_ok(_dtr)) {                                                     \
                auto *_done = (n00b_conduit_topic_t(n00b_conduit_topic_base_t *) *)            \
                    n00b_result_get(_dtr);                                           \
                if (!_done->subscriptions.data) {                                              \
                    _done->subscriptions = n00b_list_new(                                      \
                        n00b_conduit_subscription_t(n00b_conduit_topic_base_t *) *, .allocator = c->allocator); \
                    _done->inbox = nullptr;                                                    \
                }                                                                              \
                /* Done topics do NOT get their own done topics. */                            \
                n00b_conduit_topic_base_t *_done_base =                                        \
                    (n00b_conduit_topic_base_t *)_done;                                        \
                n00b_atomic_store(&_done_base->done_topic, nullptr);                          \
                n00b_atomic_store(&_base->done_topic, _done);                                 \
            }                                                                                  \
        }                                                                                      \
        return _tp;                                                                            \
    }

// ============================================================================
// Typed topic operation macros
// ============================================================================

/** @brief Deliver a typed message to all matching subscribers on a topic. */
#define n00b_conduit_topic_deliver_msg(T, topic, msg, op) \
    _N00B_TOPIC_FN(deliver, T)(topic, msg, op)

/** @brief Deliver a system message to all subscribers on a topic. */
#define n00b_conduit_topic_deliver_sys(T, topic, type, op) \
    _N00B_TOPIC_FN(deliver_sys, T)(topic, type, op)

/** @brief Subscribe to a topic with a typed inbox. */
#define n00b_conduit_subscribe(T, topic, inbox, ...) \
    _N00B_TOPIC_FN(subscribe, T)(topic, inbox, (n00b_conduit_sub_config_t){__VA_ARGS__})

/** @brief Create or retrieve a typed topic, fully initialized. */
#define n00b_conduit_topic_init(T, c, uri) \
    _N00B_TOPIC_FN(init, T)(c, uri)

// ============================================================================
// Inline accessors (operate on the base pointer for common fields)
// ============================================================================

static inline uint64_t
n00b_conduit_topic_generation(n00b_conduit_topic_base_t *topic)
{
    return n00b_atomic_load(&topic->generation);
}

static inline uint64_t
n00b_conduit_topic_epoch(n00b_conduit_topic_base_t *topic)
{
    return n00b_atomic_load(&topic->epoch);
}

static inline bool
n00b_conduit_topic_is_active(n00b_conduit_topic_base_t *topic)
{
    return n00b_atomic_load(&topic->state) == N00B_CONDUIT_TOPIC_ACTIVE;
}

static inline n00b_conduit_uri_t
n00b_conduit_topic_uri(n00b_conduit_topic_base_t *topic)
{
    return topic->uri;
}

// ============================================================================
// Master instantiation macro
// ============================================================================

/**
 * @brief Instantiate all conduit type machinery for a payload type.
 *
 * Expands to all four IMPL macros in the correct dependency order:
 *   1. `N00B_CONDUIT_MESSAGE_IMPL(T)` — typed message struct
 *   2. `N00B_CONDUIT_INBOX_IMPL_NO_MSG(T)` — typed inbox struct + inline ops
 *   3. `N00B_CONDUIT_SUBSCRIPTION_IMPL(T)` — typed subscription struct
 *   4. `N00B_CONDUIT_TOPIC_IMPL(T)` — typed topic struct + delivery/subscribe
 *
 * Use this instead of calling the four macros individually:
 * @code
 *     typedef struct { int value; } my_payload_t;
 *     N00B_CONDUIT_FULL_IMPL(my_payload_t);
 * @endcode
 */
#define N00B_CONDUIT_FULL_IMPL(T)                                                              \
    N00B_CONDUIT_MESSAGE_IMPL(T);                                                              \
    N00B_CONDUIT_INBOX_IMPL_NO_MSG(T);                                                         \
    N00B_CONDUIT_SUBSCRIPTION_IMPL(T);                                                         \
    N00B_CONDUIT_TOPIC_IMPL(T)

// Done topic payload type (n00b_conduit_topic_base_t *) is instantiated
// in conduit/conduit.h after n00b_conduit_topic_get is declared.
