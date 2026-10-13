/**
 * @file data_lock.h
 * @brief Conditional locking helpers for mutable data structures.
 *
 * Every mutable container stores an `n00b_rwlock_t *lock` field.
 * When non-null, the helpers acquire/release the rwlock; when null
 * (private / single-threaded), they are no-ops.
 *
 * `n00b_data_lock_new()` allocates and initializes a fresh rwlock.
 */
#pragma once

#include "n00b.h"
#include "core/macros.h"

// Extern rwlock API (declared in core/rwlock.h, but we avoid including
// it here to prevent a circular dependency through futex.h → stw.h).
extern void _n00b_rw_init(n00b_rwlock_t *, char *);
extern int  _n00b_rw_write_lock(n00b_rwlock_t *, char *);
extern void _n00b_rw_read_lock(n00b_rwlock_t *, char *);
extern bool _n00b_rw_unlock(n00b_rwlock_t *, char *);

/**
 * @brief Allocate and initialize a new rwlock for a data structure.
 *
 * @kw allocator    Optional allocator for the rwlock. Defaults to the runtime
 *                  system pool when no allocator is supplied.
 * @kw creation_loc "file:line" of the caller, filled in by the
 *                  @c n00b_data_lock_new macro.
 *
 * @return Initialized rwlock pointer, or nullptr during early init.
 */
extern n00b_rwlock_t *n00b_data_lock_new_at() _kargs
{
    n00b_allocator_t *allocator    = nullptr;
    const char       *creation_loc = nullptr;
};

#define n00b_data_lock_new(...)                                                \
    n00b_data_lock_new_at(.creation_loc = N00B_LOC_STRING() __VA_OPT__(, __VA_ARGS__))

/**
 * @brief Finalizer callback that frees a data-structure rwlock.
 * @param lock_ptr Pointer to the `n00b_rwlock_t` to free.
 */
extern void n00b_finalize_data_lock(void *lock_ptr);

#ifdef N00B_DEBUG
/**
 * @brief The distinct call sites, as "file:line", where
 *        @ref n00b_data_lock_new returned nullptr while a runtime existed,
 *        so the structure it was made for has no lock for life. Sets
 *        @p *count and returns the array. Only under @c N00B_DEBUG.
 */
extern const char *const *n00b_data_lock_null_sites(int *count);
#endif

/**
 * @brief Acquire a shared (read) lock, if the lock pointer is non-null.
 * @param lock Lock pointer (may be null).
 */
static inline void
n00b_data_read_lock(n00b_rwlock_t *lock)
{
    if (lock != nullptr) {
        _n00b_rw_read_lock(lock, N00B_LOC_STRING());
    }
}

/**
 * @brief Acquire an exclusive (write) lock, if the lock pointer is non-null.
 * @param lock Lock pointer (may be null).
 */
static inline void
n00b_data_write_lock(n00b_rwlock_t *lock)
{
    if (lock != nullptr) {
        _n00b_rw_write_lock(lock, N00B_LOC_STRING());
    }
}

/**
 * @brief Release a read or write lock, if the lock pointer is non-null.
 * @param lock Lock pointer (may be null).
 */
static inline void
n00b_data_unlock(n00b_rwlock_t *lock)
{
    if (lock != nullptr) {
        _n00b_rw_unlock(lock, N00B_LOC_STRING());
    }
}
