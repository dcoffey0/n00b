/**
 * @file rwlock.h
 * @brief Futex-based reader-writer lock.
 *
 * Supports multiple concurrent readers, exclusive writers, reader-to-writer
 * upgrade, and nesting.  Integrates with the per-thread lock chain for
 * crash debugging.
 */
#pragma once

#include "n00b.h"
#include "core/lock_common.h"

// NOTE: like core/mutex.h, this header deliberately does NOT include
// core/futex.h.  futex.h carries inline functions that dereference the runtime
// struct, and core/runtime.h now embeds an n00b_rwlock_t by value (the
// critical_execution STW lock) — pulling futex.h in here would parse those
// inlines before n00b_runtime_t is complete (an include cycle).  `n00b_futex_t`
// is the typedef from n00b.h; code that calls the futex API (e.g. rwlock.c)
// includes core/futex.h directly.

/**
 * @brief Debug log entry for rwlock operations (used only in debug builds).
 */
typedef struct n00b_rwdebug_t {
    _Atomic(struct n00b_rwdebug_t *) prev;
    _Atomic(struct n00b_rwdebug_t *) next;
    char                            *loc;
    bool                             lock_op;
    int32_t                          thread_id;
    int32_t                          nest;
    char                            *trace;
} n00b_rwdebug_t;

struct n00b_rwlock_t {
    N00B_COMMON_LOCK_BASE;
    n00b_futex_t              futex;
    _Atomic(n00b_rwdebug_t *) first_entry;
    _Atomic(n00b_rwdebug_t *) last_entry;
};

/**
 * @brief Initialize a reader-writer lock.
 * @param lock RWLock to initialize.
 * @param loc  Source location (auto-filled by macro).
 */
extern void _n00b_rw_init(n00b_rwlock_t *, char *);

/**
 * @brief Acquire the write lock (exclusive).
 * @param lock RWLock to acquire.
 * @param loc  Source location (auto-filled by macro).
 * @return     0 on success.
 */
extern int  _n00b_rw_write_lock(n00b_rwlock_t *, char *);

/**
 * @brief Acquire the read lock (shared).
 * @param lock RWLock to acquire.
 * @param loc  Source location (auto-filled by macro).
 */
extern void _n00b_rw_read_lock(n00b_rwlock_t *, char *);

/**
 * @brief Release a read or write lock.
 * @param lock RWLock to release.
 * @param loc  Source location (auto-filled by macro).
 * @return     true if a lock was fully released.
 */
extern bool _n00b_rw_unlock(n00b_rwlock_t *, char *);

#ifdef N00B_DEBUG
/**
 * @brief Debug-only: the calling thread's count of rwlock write releases,
 *        each of which wakes the futex.
 */
extern uint64_t n00b_rw_debug_write_wakes(void);

/**
 * @brief Debug-only: the calling thread's count of rwlock read acquisitions.
 */
extern uint64_t n00b_rw_debug_read_acquires(void);

/**
 * @brief Debug-only: the calling thread's count of rwlock write acquisitions.
 */
extern uint64_t n00b_rw_debug_write_acquires(void);
#endif

/**
 * @brief Adopt a record-less reader hold (taken null-self, e.g. at the start of
 *        thread init before the TCB exists) into a TCB read-log record, so that
 *        later nested re-acquisitions of the same lock by this thread take the
 *        reentrant fast path instead of deadlocking against a draining writer.
 *        Idempotent. WP-001.
 * @param lock   RWLock whose outstanding reader hold should be adopted.
 * @param thread The holding thread (must have a resolvable ->record).
 */
extern void n00b_rw_adopt_read_hold(n00b_rwlock_t *lock, n00b_thread_t *thread);

/**
 * @brief True if the calling thread holds @p lock, for read or for write.
 *
 * Read holds are found through the thread's read log, so a thread without a
 * TCB, whose read holds are untracked, gets false for a read hold.
 */
extern bool n00b_rw_held_by_self(n00b_rwlock_t *lock);

/**
 * @brief Drop one reader unit from a rwlock futex, with an underflow guard.
 *
 * Internal helper shared by the unlock path and the thread-teardown sweep so
 * the gate's reader count can never wrap past zero and latch the writer bit
 * (which would deadlock every later acquirer with no writer present). A
 * balanced caller always finds count > 0; the guard makes a stray extra drop a
 * no-op instead of corrupting the futex. Wakes a draining writer when the count
 * reaches zero. Returns the resulting futex value.
 * @param lock RWLock whose reader count should be decremented.
 */
extern uint32_t _n00b_rw_drop_reader_unit(n00b_rwlock_t *lock);

#define n00b_rw_write_lock(l) _n00b_rw_write_lock((l), N00B_LOC_STRING())
#define n00b_rw_read_lock(l)  _n00b_rw_read_lock((l), N00B_LOC_STRING())
#define n00b_rw_unlock(l)     _n00b_rw_unlock((l), N00B_LOC_STRING())
#define n00b_rw_init(l)       _n00b_rw_init((l), N00B_LOC_STRING())
#define n00b_rw_lock_init(l)  n00b_rw_init(l) /**< @deprecated Use n00b_rw_init */

#define N00B_RW_UNLOCKED 0x00000000
#define N00B_RW_W_LOCK   0x40000000

// Read-lock accounting (implemented in lock_accounting.c).
extern void _n00b_rlock_accounting(n00b_rwlock_t          *lock,
                                   n00b_thread_read_log_t *record,
                                   n00b_thread_t          *thread,
                                   int                     value,
                                   char                   *loc);
extern void _n00b_runlock_accounting(n00b_rwlock_t          *lock,
                                     n00b_thread_read_log_t *record,
                                     n00b_thread_t          *thread,
                                     int                     value,
                                     char                   *loc);

/** @brief Read-lock accounting with automatic source location. */
#define n00b_rlock_accounting(lock, record, thread, value) \
    _n00b_rlock_accounting((lock), (record), (thread), (value), N00B_LOC_STRING())

/** @brief Read-unlock accounting with automatic source location. */
#define n00b_runlock_accounting(lock, record, thread, value) \
    _n00b_runlock_accounting((lock), (record), (thread), (value), N00B_LOC_STRING())
