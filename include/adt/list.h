/**
 * @file list.h
 * @brief Type-safe dynamic list (growable, deque, sorted, optionally thread-safe).
 *
 * @c n00b_list_t(T) provides a growable linear buffer with push/pop at
 * both ends, insert/delete at arbitrary indices, bulk operations, sort,
 * find, and optional rwlock-based thread safety.
 *
 * Type safety is enforced through ncc's @c typeid().
 *
 * **Thread safety:** Lists are **locked by default** — @c n00b_list_new
 * creates a rwlock automatically.  Read operations acquire a shared
 * read lock; write operations acquire an exclusive write lock.  Use
 * private constructors (lock = nullptr) for single-threaded use.
 * Note that @c n00b_array_t is **not** locked by default.
 *
 * @c n00b_list_foreach acquires a **read lock** for the duration of the
 * loop — do not modify the list inside the loop body.
 *
 * Requires @c core/alloc.h to be included by the consumer for allocation,
 * ownership lookup, and backing-store release helpers.
 *
 * Usage:
 * @code
 *     n00b_list_t(int) lst = n00b_list_new(int);
 *     n00b_list_push(lst, 42);
 *     int x = n00b_list_get(lst, 0);
 *     n00b_list_free(lst);
 * @endcode
 */
#pragma once

#include <assert.h>

#include "n00b.h"
#include "core/macros.h"
#include "core/exit.h" // n00b_abort() — worker-safe termination used by the
                       // bounds-check abort in n00b_list_get / n00b_list_set.
#include "core/atomic.h"
#include "core/align.h"
#include "core/gc_map.h"
#include "adt/array.h"
#include "core/data_lock.h"
#include "adt/option.h"
#include "core/string.h"

// ============================================================================
// Constants
// ============================================================================

#define N00B_DEFAULT_LIST_SZ 16

// ============================================================================
// Type definition
// ============================================================================

#define n00b_list_tid(T) typeid("n00b_list", T)

/**
 * @brief Reference a list type for element type @p T.
 * @param T  Element type.
 *
 * Struct layout: @c { T *data; size_t len; size_t cap; n00b_rwlock_t *lock; }
 */
#define n00b_list_t(T)                                                                         \
    _generic_struct n00b_list_tid(T) {                                                         \
        T                   *data;                                                             \
        size_t               len;                                                              \
        size_t               cap;                                                              \
        n00b_rwlock_t       *lock;                                                             \
        n00b_allocator_t    *allocator;                                                        \
        n00b_gc_scan_kind_t  scan_kind;                                                        \
        n00b_gc_scan_cb_t    scan_cb;                                                          \
        void                *scan_user;                                                        \
    }

// ============================================================================
// Internal helpers  (not part of public API)
// ============================================================================

/** @internal Acquire shared (read) lock. */
#define _n00b_list_read_lock(xptr)  n00b_data_read_lock((xptr)->lock)

/** @internal Acquire exclusive (write) lock. */
#define _n00b_list_write_lock(xptr) n00b_data_write_lock((xptr)->lock)

/** @internal Release lock. */
#define _n00b_list_unlock(xptr)     n00b_data_unlock((xptr)->lock)

/**
 * @internal Grow backing store to at least @p needed elements (power-of-2).
 *           MUST be called within a locked context.  Re-applies the
 *           list's stored scan_kind / scan_cb / scan_user to the new
 *           backing store so the GC sees the same shape across grows.
 */
#define _n00b_list_ensure_cap(xptr, needed)                                                    \
    do {                                                                                       \
        size_t _bl_need = (needed);                                                            \
        if (_bl_need > (xptr)->cap) {                                                          \
            size_t               _bl_nc = n00b_align_closest_pow2_ceil(_bl_need);              \
            /* Grow with a TYPED allocation keyed on the element type recovered  \
             * from the data pointer: ncc resolves typehash(typeof(*data)*) to    \
             * the concrete element type here, so _n00b_alloc_raw upgrades the     \
             * backing store to the element's precise GC layout automatically.    \
             * Any explicit scan_kind/scan_cb/scan_user the list carries is still  \
             * passed through (preserved for lists using a custom scan). */         \
            typeof((xptr)->data) _bl_nd = n00b_alloc_array_with_opts(                          \
                typeof(*(xptr)->data), _bl_nc,                                                 \
                &(n00b_alloc_opts_t){                                                          \
                    .allocator = (xptr)->allocator,                                            \
                    .scan_kind = (xptr)->scan_kind,                                            \
                    .scan_cb   = (xptr)->scan_cb,                                              \
                    .scan_user = (xptr)->scan_user,                                            \
                });                                                                            \
            if ((xptr)->len > 0) {                                                             \
                memcpy(_bl_nd, (xptr)->data, (xptr)->len * sizeof(*(xptr)->data));             \
            }                                                                                  \
            if ((xptr)->data) {                                                                \
                n00b_allocator_opt_t _bl_alloc_opt = n00b_mem_get_allocator((xptr)->data);      \
                if (n00b_option_is_set(_bl_alloc_opt)) {                                       \
                    n00b_free((xptr)->data);                                                   \
                }                                                                              \
                else if ((xptr)->allocator != nullptr) {                                       \
                    n00b_free_from_allocator((xptr)->allocator, (xptr)->data);                  \
                }                                                                              \
                else {                                                                         \
                    n00b_free((xptr)->data);                                                   \
                }                                                                              \
            }                                                                                  \
            (xptr)->data = _bl_nd;                                                             \
            (xptr)->cap  = _bl_nc;                                                             \
        }                                                                                      \
    } while (0)

// ============================================================================
// Construction / destruction
// ============================================================================

/**
 * @brief Create a new list with default capacity.
 *
 * Lists are **locked by default**.  Use `n00b_list_new_private(T)` for
 * an unlocked list (no rwlock).
 *
 * @param T    Element type.
 * @param ...  Optional allocator.
 */
#define n00b_list_new(T, ...)              _n00b_list_new_sel(T, true, ##__VA_ARGS__)

/// @brief Create an unlocked (private) list — no rwlock.
#define n00b_list_new_private(T, ...)      _n00b_list_new_sel(T, false, ##__VA_ARGS__)

#define _n00b_list_new_sel(T, locked, ...)                                                     \
    ({                                                                                         \
        n00b_alloc_opts_t _bl_o = (n00b_alloc_opts_t){__VA_ARGS__};                            \
        (n00b_list_t(T)){                                                                      \
            .data = n00b_alloc_array_with_opts(T, N00B_DEFAULT_LIST_SZ, &_bl_o),               \
            .len       = 0,                                                                    \
            .cap       = N00B_DEFAULT_LIST_SZ,                                                 \
            .lock      = (locked) ? n00b_data_lock_new(.allocator = _bl_o.allocator)           \
                                  : (n00b_rwlock_t *)nullptr,                                  \
            .allocator = _bl_o.allocator,                                                      \
            .scan_kind = _bl_o.scan_kind,                                                      \
            .scan_cb   = _bl_o.scan_cb,                                                        \
            .scan_user = _bl_o.scan_user,                                                      \
        };                                                                                     \
    })

/**
 * @brief Create a new list with specific capacity (rounded up to pow2).
 *
 * @param T       Element type.
 * @param N       Requested minimum capacity.
 * @param locked  Whether to create a rwlock (default: true).
 * @param ...     Optional allocator.
 */
#define n00b_list_new_cap(T, N, ...)              _n00b_list_new_cap_sel(T, N, true, ##__VA_ARGS__)
#define n00b_list_new_cap_private(T, N, ...)      _n00b_list_new_cap_sel(T, N, false, ##__VA_ARGS__) /**< @deprecated Use n00b_list_new_cap(T, N, false) */

#define _n00b_list_new_cap_sel(T, N, locked, ...)                                              \
    ({                                                                                         \
        size_t _bl_rc = n00b_align_closest_pow2_ceil(n00b_max((size_t)(N), (size_t)1));        \
        n00b_alloc_opts_t _bl_o = (n00b_alloc_opts_t){__VA_ARGS__};                            \
        (n00b_list_t(T)){                                                                      \
            .data = n00b_alloc_array_with_opts(T, _bl_rc, &_bl_o),                             \
            .len       = 0,                                                                    \
            .cap       = _bl_rc,                                                               \
            .lock      = (locked) ? n00b_data_lock_new(.allocator = _bl_o.allocator)           \
                                  : (n00b_rwlock_t *)nullptr,                                  \
            .allocator = _bl_o.allocator,                                                      \
            .scan_kind = _bl_o.scan_kind,                                                      \
            .scan_cb   = _bl_o.scan_cb,                                                        \
            .scan_user = _bl_o.scan_user,                                                      \
        };                                                                                     \
    })

/**
 * @brief Free the backing storage and the lock of a list and zero the struct.
 * @param x  List (lvalue).
 */
#define n00b_list_free(x)                                                                      \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        if (_bl_lp->data) {                                                                    \
            n00b_allocator_opt_t _bl_alloc_opt = n00b_mem_get_allocator(_bl_lp->data);          \
            if (n00b_option_is_set(_bl_alloc_opt)) {                                           \
                n00b_free(_bl_lp->data);                                                       \
            }                                                                                  \
            else if (_bl_lp->allocator != nullptr) {                                           \
                n00b_free_from_allocator(_bl_lp->allocator, _bl_lp->data);                      \
            }                                                                                  \
            else {                                                                             \
                n00b_free(_bl_lp->data);                                                       \
            }                                                                                  \
        }                                                                                      \
        if (_bl_lp->lock) {                                                                    \
            n00b_finalize_data_lock(_bl_lp->lock);                                             \
        }                                                                                      \
        *_bl_lp = (typeof(x)){};                                                               \
    })

// ============================================================================
// Access  (read-locked)
// ============================================================================

/** @brief Element count. */
#define n00b_list_len(x)                                                                       \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        _n00b_list_read_lock(_bl_lp);                                                          \
        size_t _bl_r = _bl_lp->len;                                                            \
        _n00b_list_unlock(_bl_lp);                                                             \
        _bl_r;                                                                                 \
    })

/** @brief Allocated capacity. */
#define n00b_list_cap(x)                                                                       \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        _n00b_list_read_lock(_bl_lp);                                                          \
        size_t _bl_r = _bl_lp->cap;                                                            \
        _n00b_list_unlock(_bl_lp);                                                             \
        _bl_r;                                                                                 \
    })

/**
 * @brief Get element at index (bounds-checked, aborts on OOB).
 * @param x  List (lvalue).
 * @param i  Index.
 */
#define n00b_list_get(x, i)                                                                    \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        _n00b_list_read_lock(_bl_lp);                                                          \
        size_t _bl_i = (i);                                                                    \
        if (_bl_i >= _bl_lp->len) {                                                            \
            _n00b_list_unlock(_bl_lp);                                                         \
            n00b_abort();                                                                      \
        }                                                                                      \
        typeof(*_bl_lp->data) _bl_r = _bl_lp->data[_bl_i];                                     \
        _n00b_list_unlock(_bl_lp);                                                             \
        _bl_r;                                                                                 \
    })

/**
 * @brief Set element at index (bounds-checked against len, aborts on OOB).
 * @param x    List (lvalue).
 * @param i    Index.
 * @param val  Value to assign.
 */
#define n00b_list_set(x, i, val)                                                               \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        _n00b_list_write_lock(_bl_lp);                                                         \
        size_t _bl_i = (i);                                                                    \
        if (_bl_i >= _bl_lp->len) {                                                            \
            _n00b_list_unlock(_bl_lp);                                                         \
            n00b_abort();                                                                      \
        }                                                                                      \
        _bl_lp->data[_bl_i] = (val);                                                           \
        _n00b_list_unlock(_bl_lp);                                                             \
    })

// ============================================================================
// Push / Pop — Back  (write-locked)
// ============================================================================

/**
 * @brief Append an element to the back, growing if needed.
 * @param x    List (lvalue).
 * @param val  Value to append.
 */
#define n00b_list_push(x, val)                                                                 \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        _n00b_list_write_lock(_bl_lp);                                                         \
        _n00b_list_ensure_cap(_bl_lp, _bl_lp->len + 1);                                        \
        _bl_lp->data[_bl_lp->len++] = (val);                                                   \
        _n00b_list_unlock(_bl_lp);                                                             \
    })

/**
 * @brief Remove and return the last element as an option.
 * @param T  Element type.
 * @param x  List (lvalue).
 * @return   `n00b_option_t(T)` — none if the list is empty.
 */
#define n00b_list_pop(T, x)                                                                    \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        n00b_option_t(T) _bl_opt;                                                              \
        _n00b_list_write_lock(_bl_lp);                                                         \
        if (_bl_lp->len > 0) {                                                                 \
            size_t _bl_i = --_bl_lp->len;                                                       \
            typeof(*_bl_lp->data) _bl_r = _bl_lp->data[_bl_i];                                 \
            _bl_lp->data[_bl_i] = (typeof(*_bl_lp->data)){};                                   \
            _bl_opt = n00b_option_set(T, _bl_r);                                               \
        }                                                                                      \
        else {                                                                                 \
            _bl_opt = n00b_option_none(T);                                                     \
        }                                                                                      \
        _n00b_list_unlock(_bl_lp);                                                             \
        _bl_opt;                                                                               \
    })

// ============================================================================
// Push / Pop — Front  (write-locked)
// ============================================================================

/**
 * @brief Insert an element at the front, shifting right.
 * @param x    List (lvalue).
 * @param val  Value to prepend.
 */
#define n00b_list_push_front(x, val)                                                           \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        _n00b_list_write_lock(_bl_lp);                                                         \
        _n00b_list_ensure_cap(_bl_lp, _bl_lp->len + 1);                                        \
        if (_bl_lp->len > 0) {                                                                 \
            memmove(_bl_lp->data + 1, _bl_lp->data, _bl_lp->len * sizeof(*_bl_lp->data));      \
        }                                                                                      \
        _bl_lp->data[0] = (val);                                                               \
        _bl_lp->len++;                                                                         \
        _n00b_list_unlock(_bl_lp);                                                             \
    })

/**
 * @brief Remove and return element 0, shifting left.
 * @param T  Element type.
 * @param x  List (lvalue).
 * @return   `n00b_option_t(T)` — none if the list is empty.
 */
#define n00b_list_pop_front(T, x)                                                              \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        n00b_option_t(T) _bl_opt;                                                              \
        _n00b_list_write_lock(_bl_lp);                                                         \
        if (_bl_lp->len > 0) {                                                                 \
            T _bl_r = _bl_lp->data[0];                                                         \
            _bl_lp->len--;                                                                     \
            if (_bl_lp->len > 0) {                                                             \
                memmove(_bl_lp->data, _bl_lp->data + 1,                                        \
                        _bl_lp->len * sizeof(*_bl_lp->data));                                   \
            }                                                                                  \
            _bl_lp->data[_bl_lp->len] = (typeof(*_bl_lp->data)){};                             \
            _bl_opt = n00b_option_set(T, _bl_r);                                               \
        }                                                                                      \
        else {                                                                                 \
            _bl_opt = n00b_option_none(T);                                                     \
        }                                                                                      \
        _n00b_list_unlock(_bl_lp);                                                             \
        _bl_opt;                                                                               \
    })

// ============================================================================
// Insert / Delete — Single Element  (write-locked)
// ============================================================================

/**
 * @brief Insert a value at index @p i, sliding existing elements right.
 * @param x    List (lvalue).
 * @param i    Insertion index (0 <= i <= len).
 * @param val  Value to insert.
 */
#define n00b_list_insert(x, i, val)                                                            \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        _n00b_list_write_lock(_bl_lp);                                                         \
        size_t _bl_i = (i);                                                                    \
        assert(_bl_i <= _bl_lp->len);                                                          \
        _n00b_list_ensure_cap(_bl_lp, _bl_lp->len + 1);                                        \
        if (_bl_i < _bl_lp->len) {                                                             \
            memmove(_bl_lp->data + _bl_i + 1,                                                  \
                    _bl_lp->data + _bl_i,                                                      \
                    (_bl_lp->len - _bl_i) * sizeof(*_bl_lp->data));                            \
        }                                                                                      \
        _bl_lp->data[_bl_i] = (val);                                                           \
        _bl_lp->len++;                                                                         \
        _n00b_list_unlock(_bl_lp);                                                             \
    })

/**
 * @brief Remove element at index @p i, sliding left.  Returns removed value.
 * @param x  List (lvalue).
 * @param i  Index to remove (0 <= i < len).
 */
#define n00b_list_delete(x, i)                                                                 \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        _n00b_list_write_lock(_bl_lp);                                                         \
        size_t _bl_i = (i);                                                                    \
        assert(_bl_i < _bl_lp->len);                                                           \
        typeof(*_bl_lp->data) _bl_r = _bl_lp->data[_bl_i];                                     \
        _bl_lp->len--;                                                                         \
        if (_bl_i < _bl_lp->len) {                                                             \
            memmove(_bl_lp->data + _bl_i,                                                      \
                    _bl_lp->data + _bl_i + 1,                                                  \
                    (_bl_lp->len - _bl_i) * sizeof(*_bl_lp->data));                            \
        }                                                                                      \
        _bl_lp->data[_bl_lp->len] = (typeof(*_bl_lp->data)){};                                 \
        _n00b_list_unlock(_bl_lp);                                                             \
        _bl_r;                                                                                 \
    })

// ============================================================================
// Insert / Delete — Bulk  (write-locked)
// ============================================================================

/**
 * @brief Insert all elements of @p src at index @p i in @p x.
 *        Both lists must be the same type.  @p src is not modified.
 *        Do NOT pass the same list as both @p x and @p src.
 * @param x    Destination list (lvalue).
 * @param i    Insertion index (0 <= i <= len).
 * @param src  Source list (lvalue, read-only during call).
 */
#define n00b_list_insert_list(x, i, src)                                                       \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        auto _bl_sp = &(src);                                                                  \
        _n00b_list_write_lock(_bl_lp);                                                         \
        size_t _bl_i  = (i);                                                                   \
        size_t _bl_sn = _bl_sp->len;                                                           \
        assert(_bl_i <= _bl_lp->len);                                                          \
        _n00b_list_ensure_cap(_bl_lp, _bl_lp->len + _bl_sn);                                   \
        if (_bl_i < _bl_lp->len) {                                                             \
            memmove(_bl_lp->data + _bl_i + _bl_sn,                                             \
                    _bl_lp->data + _bl_i,                                                      \
                    (_bl_lp->len - _bl_i) * sizeof(*_bl_lp->data));                            \
        }                                                                                      \
        memcpy(_bl_lp->data + _bl_i, _bl_sp->data, _bl_sn * sizeof(*_bl_lp->data));            \
        _bl_lp->len += _bl_sn;                                                                 \
        _n00b_list_unlock(_bl_lp);                                                             \
    })

/**
 * @brief Remove @p count elements starting at @p start.
 * @param x      List (lvalue).
 * @param start  First index to remove.
 * @param count  Number of elements to remove.
 */
#define n00b_list_delete_range(x, start, count)                                                \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        _n00b_list_write_lock(_bl_lp);                                                         \
        size_t _bl_s = (start);                                                                \
        size_t _bl_c = (count);                                                                \
        assert(_bl_s + _bl_c <= _bl_lp->len);                                                  \
        size_t _bl_old_len = _bl_lp->len;                                                       \
        size_t _bl_tail = _bl_lp->len - _bl_s - _bl_c;                                         \
        if (_bl_tail > 0) {                                                                    \
            memmove(_bl_lp->data + _bl_s,                                                      \
                    _bl_lp->data + _bl_s + _bl_c,                                              \
                    _bl_tail * sizeof(*_bl_lp->data));                                         \
        }                                                                                      \
        _bl_lp->len -= _bl_c;                                                                  \
        for (size_t _bl_j = _bl_lp->len; _bl_j < _bl_old_len; _bl_j++) {                       \
            _bl_lp->data[_bl_j] = (typeof(*_bl_lp->data)){};                                   \
        }                                                                                      \
        _n00b_list_unlock(_bl_lp);                                                             \
    })

// ============================================================================
// Concatenation
// ============================================================================

/**
 * @brief Return a new list containing elements of @p a followed by @p b.
 *        Does not lock sources — caller ensures stability.
 *        If source has a lock, the new list gets a fresh lock; otherwise null.
 * @param a  First list (lvalue).
 * @param b  Second list (lvalue).
 */
#define n00b_list_concat(a, b)                                                                 \
    ({                                                                                         \
        auto      _bl_ap  = &(a);                                                              \
        auto      _bl_bp  = &(b);                                                              \
        size_t    _bl_tl  = _bl_ap->len + _bl_bp->len;                                         \
        size_t    _bl_tc  = n00b_align_closest_pow2_ceil(n00b_max(_bl_tl, (size_t)1));         \
        typeof(a) _bl_new = {                                                                  \
            .data = n00b_alloc_array_with_opts(typeof(*_bl_ap->data), _bl_tc,                  \
                        &(n00b_alloc_opts_t){                                                  \
                            .allocator = _bl_ap->allocator,                                    \
                            .scan_kind = _bl_ap->scan_kind,                                    \
                            .scan_cb   = _bl_ap->scan_cb,                                      \
                            .scan_user = _bl_ap->scan_user,                                    \
                        }),                                                                    \
            .len       = _bl_tl,                                                               \
            .cap       = _bl_tc,                                                               \
            .lock      = _bl_ap->lock ? n00b_data_lock_new(.allocator = _bl_ap->allocator)     \
                                      : (n00b_rwlock_t *)nullptr,                              \
            .allocator = _bl_ap->allocator,                                                    \
            .scan_kind = _bl_ap->scan_kind,                                                    \
            .scan_cb   = _bl_ap->scan_cb,                                                      \
            .scan_user = _bl_ap->scan_user,                                                    \
        };                                                                                     \
        if (_bl_ap->len > 0) {                                                                 \
            memcpy(_bl_new.data, _bl_ap->data, _bl_ap->len * sizeof(*_bl_ap->data));           \
        }                                                                                      \
        if (_bl_bp->len > 0) {                                                                 \
            memcpy(_bl_new.data + _bl_ap->len,                                                 \
                   _bl_bp->data,                                                               \
                   _bl_bp->len * sizeof(*_bl_bp->data));                                       \
        }                                                                                      \
        _bl_new;                                                                               \
    })

// ============================================================================
// Search  (read-locked)
// ============================================================================

/**
 * @brief Find the first index of @p val.
 * @param x    List (lvalue).
 * @param val  Value to search for (compared with @c ==).
 * @return     @c n00b_option_t(size_t) — set if found, none otherwise.
 */
#define n00b_list_find(x, val)                                                                 \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        _n00b_list_read_lock(_bl_lp);                                                          \
        typeof(*_bl_lp->data) _bl_v = (val);                                                   \
        n00b_option_t(size_t)  _bl_r = n00b_option_none(size_t);                               \
        for (size_t _bl_i = 0; _bl_i < _bl_lp->len; _bl_i++) {                                 \
            if (_bl_lp->data[_bl_i] == _bl_v) {                                                \
                _bl_r = n00b_option_set(size_t, _bl_i);                                        \
                break;                                                                         \
            }                                                                                  \
        }                                                                                      \
        _n00b_list_unlock(_bl_lp);                                                             \
        _bl_r;                                                                                 \
    })

// ============================================================================
// Sort  (write-locked)
// ============================================================================

/**
 * @brief In-place sort via @c qsort.
 * @param x    List (lvalue).
 * @param cmp  Comparator with standard @c qsort signature:
 *             @c int(*)(const void*, const void*)
 */
#define n00b_list_sort(x, cmp)                                                                 \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        _n00b_list_write_lock(_bl_lp);                                                         \
        if (_bl_lp->len > 1) {                                                                 \
            qsort(_bl_lp->data, _bl_lp->len, sizeof(*_bl_lp->data), (cmp));                    \
        }                                                                                      \
        _n00b_list_unlock(_bl_lp);                                                             \
    })

// ============================================================================
// Remove by value  (write-locked)
// ============================================================================

/**
 * @brief Remove ALL instances of @p val.  Returns count removed.
 *        Single-pass compaction.
 * @param x    List (lvalue).
 * @param val  Value to remove (compared with @c ==).
 */
#define n00b_list_remove_all(x, val)                                                           \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        _n00b_list_write_lock(_bl_lp);                                                         \
        typeof(*_bl_lp->data) _bl_v = (val);                                                   \
        size_t                _bl_w = 0;                                                       \
        size_t _bl_old_len = _bl_lp->len;                                                       \
        for (size_t _bl_i = 0; _bl_i < _bl_old_len; _bl_i++) {                                 \
            if (_bl_lp->data[_bl_i] != _bl_v) {                                                \
                _bl_lp->data[_bl_w++] = _bl_lp->data[_bl_i];                                   \
            }                                                                                  \
        }                                                                                      \
        size_t _bl_removed = _bl_old_len - _bl_w;                                              \
        _bl_lp->len        = _bl_w;                                                            \
        for (size_t _bl_j = _bl_w; _bl_j < _bl_old_len; _bl_j++) {                             \
            _bl_lp->data[_bl_j] = (typeof(*_bl_lp->data)){};                                   \
        }                                                                                      \
        _n00b_list_unlock(_bl_lp);                                                             \
        _bl_removed;                                                                           \
    })

// ============================================================================
// Utilities
// ============================================================================

/**
 * @brief Deep copy — returns a new, independent list.
 *        Does not lock source — caller ensures stability.
 *        If source has a lock, the clone gets a fresh lock; otherwise null.
 * @param x  List (lvalue).
 */
#define n00b_list_clone(x)                                                                     \
    ({                                                                                         \
        auto      _bl_sp  = &(x);                                                              \
        size_t    _bl_nc  = n00b_max(_bl_sp->cap, (size_t)1);                                  \
        typeof(x) _bl_new = {                                                                  \
            .data = n00b_alloc_array_with_opts(typeof(*_bl_sp->data), _bl_nc,                  \
                        &(n00b_alloc_opts_t){                                                  \
                            .allocator = _bl_sp->allocator,                                    \
                            .scan_kind = _bl_sp->scan_kind,                                    \
                            .scan_cb   = _bl_sp->scan_cb,                                      \
                            .scan_user = _bl_sp->scan_user,                                    \
                        }),                                                                    \
            .len       = _bl_sp->len,                                                          \
            .cap       = _bl_nc,                                                               \
            .lock      = _bl_sp->lock ? n00b_data_lock_new(.allocator = _bl_sp->allocator)     \
                                      : (n00b_rwlock_t *)nullptr,                              \
            .allocator = _bl_sp->allocator,                                                    \
            .scan_kind = _bl_sp->scan_kind,                                                    \
            .scan_cb   = _bl_sp->scan_cb,                                                      \
            .scan_user = _bl_sp->scan_user,                                                    \
        };                                                                                     \
        if (_bl_sp->len > 0) {                                                                 \
            memcpy(_bl_new.data, _bl_sp->data, _bl_sp->len * sizeof(*_bl_sp->data));           \
        }                                                                                      \
        _bl_new;                                                                               \
    })

/**
 * @brief Test whether the list is empty.
 * @param x  List (lvalue).
 * @return true if the list has no elements.
 */
#define n00b_list_is_empty(x) (n00b_list_len(x) == 0)

/**
 * @brief Reset len to 0, keeping allocated capacity.
 * @param x  List (lvalue).
 */
#define n00b_list_clear(x)                                                                     \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        _n00b_list_write_lock(_bl_lp);                                                         \
        for (size_t _bl_i = 0; _bl_i < _bl_lp->len; _bl_i++) {                                 \
            _bl_lp->data[_bl_i] = (typeof(*_bl_lp->data)){};                                   \
        }                                                                                      \
        _bl_lp->len = 0;                                                                       \
        _n00b_list_unlock(_bl_lp);                                                             \
    })

/**
 * @brief Pointer-based iteration loop (lock-aware).
 *
 * Acquires a shared read lock before iteration and releases it
 * afterwards.  If the list has no lock (lock == nullptr), the
 * lock/unlock calls are no-ops and the loop runs at zero overhead.
 *
 * Do **not** modify the list inside the loop body (that would
 * require a write lock).
 *
 * Example:
 * @code
 *     n00b_list_foreach(lst, p) {
 *         printf("%d\n", *p);
 *     }
 * @endcode
 */
#define n00b_list_foreach(x, var)                                                              \
    for (int _lfl_once = (n00b_data_read_lock((x).lock), 1); _lfl_once; )                     \
        for (typeof((x).data) var = (x).data;                                                  \
             (var) < (x).data + (x).len                                                        \
                 ? 1                                                                            \
                 : (n00b_data_unlock((x).lock), _lfl_once = 0);                                \
             ++(var))

#define n00b_list_foreach_locked(x, var)  n00b_list_foreach(x, var) /**< @deprecated Use n00b_list_foreach */

// ============================================================================
// Conversion
// ============================================================================

/**
 * @brief Move a list's data into an `n00b_array_t` of the same element type.
 *
 * The list is consumed: its data pointer is transferred to the array
 * and the list is zeroed.
 *
 * @param T  Element type (must match the list's element type).
 * @param x  List (lvalue) — will be zeroed after conversion.
 * @return An `n00b_array_t(T)` owning the former list data.
 *
 * @pre  No other thread holds the list lock.
 * @post @p x has len/cap 0 and a nullptr data pointer.
 */
#define n00b_list_to_array(T, x)                                                                   \
    ({                                                                                             \
        auto _bl_lp = &(x);                                                                        \
        n00b_array_t(T) _bl_arr = {                                                                \
            .data      = _bl_lp->data,                                                             \
            .len       = _bl_lp->len,                                                              \
            .cap       = _bl_lp->cap,                                                              \
            .allocator = _bl_lp->allocator,                                                        \
            .scan_kind = _bl_lp->scan_kind,                                                        \
            .scan_cb   = _bl_lp->scan_cb,                                                          \
            .scan_user = _bl_lp->scan_user,                                                        \
        };                                                                                         \
        _bl_lp->data      = nullptr;                                                               \
        _bl_lp->len       = 0;                                                                     \
        _bl_lp->cap       = 0;                                                                     \
        if (_bl_lp->lock) {                                                                        \
            n00b_finalize_data_lock(_bl_lp->lock);                                                 \
        }                                                                                          \
        _bl_lp->lock      = nullptr;                                                               \
        _bl_lp->allocator = nullptr;                                                               \
        _bl_arr;                                                                                   \
    })
