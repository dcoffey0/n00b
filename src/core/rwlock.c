/*
 * Futex-based reader-writer lock implementation.
 *
 * The futex word encodes both the reader count (low 30 bits) and the
 * writer-lock bit (bit 30, N00B_RW_W_LOCK).  Writers set the W_LOCK bit
 * to block new readers, then wait for the reader count to reach zero.
 *
 * Reader-to-writer upgrade is supported: the upgrading reader first
 * decrements the reader count (to avoid self-deadlock), then competes
 * for the write bit normally.
 */

#define N00B_USE_INTERNAL_API

#include "n00b.h"
#include "core/runtime.h"
#include "core/thread.h"
#include "core/rwlock.h"
#include "core/stw.h"
#include "core/alloc.h"
#include "core/atomic.h"
#include "core/futex.h"

static inline bool
stw_bypass_for_lock(n00b_runtime_t *rt, n00b_rwlock_t *lock)
{
    if (!n00b_atomic_load(&rt->stw_active)) {
        return false;
    }
    if (lock != &rt->critical_execution) {
        return true;
    }
    // A late worker must wait for the gate owner or it can collect beside it.
    return n00b_atomic_load(&lock->data).owner == n00b_self_os_id();
}

static n00b_thread_read_log_t *
find_read_lock_record(n00b_rwlock_t *lock, n00b_thread_t *thread)
{
    n00b_thread_record_t   *rec = thread->record;
    n00b_thread_read_log_t *log = n00b_atomic_load(&rec->read_locks);

    while (log != nullptr) {
        if (log->obj == lock) {
            return log;
        }
        log = log->next_entry;
    }

    return nullptr;
}

static inline n00b_thread_read_log_t *
acquire_read_record(n00b_rwlock_t *lock, n00b_thread_t *thread)
{
    n00b_thread_record_t   *rec = thread->record;
    n00b_thread_read_log_t *log;
    n00b_thread_read_log_t *prev;

    if (n00b_atomic_load(&rec->log_alloc_cache)) {
        log = n00b_atomic_load(&rec->log_alloc_cache);
        n00b_atomic_store(&rec->log_alloc_cache, log->next_entry);

        if (n00b_atomic_load(&rec->log_alloc_cache)) {
            n00b_atomic_load(&rec->log_alloc_cache)->prev_entry = nullptr;
        }

        log->next_entry = nullptr;
    }
    else {
        n00b_runtime_t   *rt = n00b_get_runtime();
        n00b_allocator_t *sp = (n00b_allocator_t *)&rt->system_pool;

        log = n00b_alloc_with_opts(n00b_thread_read_log_t,
                                   &(n00b_alloc_opts_t){.allocator = sp});
    }
    log->obj   = lock;
    log->level = 0;
    prev       = n00b_atomic_load(&rec->read_locks);

    log->next_entry = prev;
    n00b_atomic_store(&rec->read_locks, log);

    if (prev) {
        prev->prev_entry = log;
    }

    return log;
}

static void
register_read(n00b_rwlock_t          *lock,
              n00b_thread_t          *thread,
              int                     value,
              n00b_thread_read_log_t *log,
              char                   *loc)
{
    if (!log) {
        log = acquire_read_record(lock, thread);
    }

    log->level++;
    _n00b_rlock_accounting(lock, log, thread, value, loc);
}

void
_n00b_rw_init(n00b_rwlock_t *lock, char *loc)
{
    n00b_lock_init_accounting((void *)lock, N00B_NLT_RW, loc);
    n00b_futex_init(&lock->futex);
}

// WP-001: adopt a record-less reader hold into a TCB read-log record.
//
// A thread holds the STW gate (critical_execution) across its WHOLE init, but
// it must take that hold BEFORE its TCB (thread->record) exists —
// n00b_thread_self() is not resolvable until the live-slot bitmap is published
// partway through init.  So the outer acquire rides the null-self reader path:
// it bumps the raw futex reader count but registers no read-log record.  Once
// the slot is published and n00b_thread_self() resolves, any NESTED gate
// acquire (the first GC-visible allocation -> mmap lookup, etc.) runs with
// have_tcb == true and consults the read log to recognize its own outstanding
// hold.  Finding no record, it would attempt a FRESH acquire and block behind a
// writer that has set W_LOCK — a writer that is itself draining for this very
// thread's outstanding reader count.  Classic reader-recursion-vs-writer
// deadlock (and exactly the soak hang observed: collector at the write-lock
// drain, the mid-init thread blocked re-acquiring the gate it already holds).
//
// Adoption closes the gap: the instant the TCB resolves, materialize a read-log
// record for the already-held count (level 1 = the one outstanding futex unit).
// Subsequent nested acquires then take the reentrant fast path (no futex
// touch); their unlocks drain the record level first, dropping the futex count
// only when the outermost hold is released.  Net futex effect is unchanged —
// the hold is simply now visible to reentrancy.  Idempotent: a no-op if a
// record for this lock already exists.
void
n00b_rw_adopt_read_hold(n00b_rwlock_t *lock, n00b_thread_t *thread)
{
    if (find_read_lock_record(lock, thread) != nullptr) {
        return;
    }

    n00b_thread_read_log_t *log = acquire_read_record(lock, thread);
    log->level                  = 1;
    // No _n00b_rlock_accounting call here (unlike register_read): the adopted
    // hold was taken on the null-self path, which never ran acquire-side
    // accounting, so there is no matching prior entry to pair with.  The level-1
    // record exists purely to make the outstanding futex unit visible to
    // reentrancy (find_read_lock_record) and to the unlock path's count math.
    // Read-lock accounting is a no-op in this build regardless (lock_accounting.c).
}

int
_n00b_rw_write_lock(n00b_rwlock_t *lock, char *loc)
{
    // STW-active short-circuit (WP-001): no-op acquire while the world is
    // stopped (the collector is the sole runner).
    if (stw_bypass_for_lock(n00b_get_runtime(), lock)) {
        return 0;
    }

    n00b_thread_t          *thread    = n00b_thread_self();
    int64_t                 tid       = n00b_thread_os_id(thread);
#ifdef N00B_DEBUG
    if (thread != nullptr && thread->record != nullptr) {
        thread->record->debug_rw_write_acquires++;
    }
#endif
    n00b_core_lock_info_t   info      = n00b_atomic_load(&lock->data);
    // No TCB => no read record to upgrade from (find_read_lock_record derefs
    // thread->record).  The only write-locker is the STW collector, which
    // always has a resolvable self+record; this guard just keeps the path
    // null-safe (self can exist before/after its record during init/destroy).
    n00b_thread_read_log_t *record    = (thread != nullptr && thread->record != nullptr)
                                          ? find_read_lock_record(lock, thread)
                                          : nullptr;
    bool                    upgrading = false;
    uint32_t                value;

    if (info.owner == tid) {
        goto post_resume;
    }

    if (record != nullptr) {
        upgrading = true;

        // Remove ourselves as a reader to avoid self-deadlock on upgrade.
        volatile uint32_t desired;

        do {
            value   = n00b_atomic_load(&lock->futex);
            desired = value - 1;
        } while (!n00b_cas(&lock->futex, &value, desired));
    }

    // Compete for the write bit.
    value = n00b_atomic_or(&lock->futex, N00B_RW_W_LOCK);

    while (value & N00B_RW_W_LOCK) {
        n00b_register_lock_wait(thread, lock, loc);
        n00b_futex_wait_for_value(&lock->futex, N00B_RW_UNLOCKED);
        value = n00b_atomic_or(&lock->futex, N00B_RW_W_LOCK);
        n00b_wait_done(thread);
    }

    // Wait for readers to drain.
    if (value) {
        n00b_register_lock_wait(thread, lock, loc);
        assert(lock);

        n00b_futex_wait_for_value(&lock->futex, N00B_RW_W_LOCK);
        n00b_barrier();
        n00b_wait_done(thread);
    }

    if (upgrading) {
        n00b_atomic_add(&lock->futex, 1);
    }

post_resume:

{
    int result = n00b_lock_acquire_accounting((void *)lock, thread, loc);

    info = n00b_atomic_load(&lock->data);
    assert(info.owner == tid);

    return result;
}
}

void
_n00b_rw_read_lock(n00b_rwlock_t *lock, char *loc)
{
    n00b_runtime_t *rt = n00b_get_runtime();

    // STW-active short-circuit (WP-001): no-op acquire while the world is
    // stopped (the collector is the sole runner).
    if (stw_bypass_for_lock(rt, lock)) {
        return;
    }

    n00b_thread_t *thread = n00b_thread_self();

    // Null self is permitted ONLY for the STW gate: a thread holds it across its
    // whole init / destroy, before / after its TCB (thread->record) exists.  In
    // that window we ride the SAME reader path but skip the per-thread read-log
    // record and lock-accounting (the only parts that deref the TCB); the futex
    // reader count — which is what the collector's write lock actually drains —
    // is taken unconditionally.  Reentrancy for TCB-bearing threads still flows
    // through the read log below (so a holder that re-enters a gate critical
    // section, e.g. metadata teardown -> n00b_free -> mmap lookup, does NOT take
    // a fresh futex count and cannot deadlock against a waiting writer).
    // "TCB available" means the per-thread read log exists: both the thread
    // struct AND its record (n00b_thread_record_t).  During init/destroy
    // n00b_thread_self() can be non-null while thread->record is still null (or
    // already torn down); find_read_lock_record / register_read deref the
    // record, so treat that window like null-self too.
    bool have_tcb = (thread != nullptr && thread->record != nullptr);
#ifdef N00B_DEBUG
    if (have_tcb) {
        thread->record->debug_rw_read_acquires++;
    }
#endif

    // n00b#521: a TCB-less reader is permitted on ANY lock, not just the STW
    // gate. A thread n00b did not create -- Windows' KERNELBASE!CtrlRoutine,
    // say -- has no TCB for its whole life, so the old
    // assert(lock == &rt->critical_execution) aborted a daemon on Ctrl-C.
    // Every TCB-deref below is gated on have_tcb, and the futex reader count
    // a draining writer waits on is taken unconditionally, so such a reader
    // already rode a correct path.
    //
    // CONSTRAINT: a foreign thread must not RE-ENTER a read lock it already
    // holds. With no read log it cannot be recognized as the holder, so
    // re-entry is a fresh acquire, and a fresh acquire parks on W_LOCK. If a
    // writer is draining at that moment the thread parks holding the very
    // unit the writer waits for -- deadlock. MEASURED, not theorized: with
    // the re-entry forced under a queued writer, both threads stall and the
    // watchdog in test_rwlock.c fires. Single acquire/release is safe and is
    // what a console handler does; test_foreign_reader_with_writer_queued
    // pins that boundary.

    n00b_core_lock_info_t   info    = n00b_atomic_load(&lock->data);
    n00b_thread_read_log_t *record  = have_tcb ? find_read_lock_record(lock, thread)
                                               : nullptr;
    uint32_t                value   = 0;
    volatile uint32_t       desired = 0;

    n00b_barrier();

    if (have_tcb && info.owner == n00b_thread_os_id(thread)) {
        n00b_lock_acquire_accounting((void *)lock, thread, loc);
        return;
    }

    // Fast path for nested reads (TCB-tracked reentrancy; no futex touch).
    if (record) {
        register_read(lock, thread, -1, record, loc);
        return;
    }

    // Fast path: no contention.
    if (n00b_cas(&lock->futex, &value, 1)) {
        if (have_tcb) {
            register_read(lock, thread, desired, nullptr, loc);
        }
        return;
    }

    n00b_barrier();

    value = n00b_atomic_load(&lock->futex);
    while (true) {
        if (value & N00B_RW_W_LOCK) {
            if (have_tcb) {
                n00b_register_lock_wait(thread, lock, loc);
            }
            n00b_futex_wait_forever(&lock->futex, value);
            if (have_tcb) {
                n00b_wait_done(thread);
            }

            value = n00b_atomic_load(&lock->futex);
            continue;
        }

        /* Bugfix: the previous `do { ... } while (cas(..., desired))`
         * form skipped this assignment when the W_LOCK branch did
         * `continue` — `continue` in a do-while jumps to the loop
         * condition, so the cas would run with the *previous*
         * iteration's `desired` (or the initial 0 on first pass).
         * That caused two failure modes:
         *   1. After waking from a W_LOCK wait, the cas would attempt
         *      `value -> 0`, swallowing the reader-count increment.
         *   2. The subsequent reader-unlock would underflow the count
         *      to UINT_MAX, pinning W_LOCK + a huge reader count and
         *      deadlocking every later acquirer.
         * Recomputing `desired` immediately before each cas attempt
         * keeps the count math correct. */
        desired = value + 1;
        if (n00b_cas((volatile _Atomic(uint32_t) *)&lock->futex,
                     &value, desired)) {
            break;
        }
    }

    if (have_tcb) {
        register_read(lock, thread, desired, nullptr, loc);
    }

    n00b_barrier();
}

// Drop exactly one reader unit from a rwlock futex, with an underflow guard.
//
// The reader count lives in the bits below N00B_RW_W_LOCK (mask 0x3FFFFFFF).
// A balanced unlock always finds count > 0, and `value - 1` decrements the
// count while leaving N00B_RW_W_LOCK untouched (the subtraction borrows only
// within the count bits). But if the count is ALREADY zero, `value - 1` wraps
// past zero and latches N00B_RW_W_LOCK (bit 30) plus a huge phantom reader
// count -- after which every future acquirer parks on the W_LOCK bit forever
// with no writer present to clear it and wake them. That is a hard, process-
// wide deadlock of the stop-the-world gate.
//
// A balanced caller never reaches the count==0 case. Reaching it means the
// reader unit was already released elsewhere (e.g. a double-drop across the
// foreign-thread teardown / record-less gate paths). Treat it as a no-op so
// the count stays at its correct value instead of corrupting the gate.
//
// Returns the resulting futex value (unchanged on the guarded no-op). Wakes a
// draining writer iff a unit was actually dropped and W_LOCK is set.
uint32_t
_n00b_rw_drop_reader_unit(n00b_rwlock_t *lock)
{
    uint32_t value;
    uint32_t desired = 0;
    bool     dropped = false;

    for (;;) {
        value = n00b_atomic_load(&lock->futex);
        if ((value & (N00B_RW_W_LOCK - 1)) == 0) {
            // Underflow guard: no reader unit to release.
            desired = value;
            break;
        }
        desired = value - 1;
        if (n00b_cas(&lock->futex, &value, desired)) {
            dropped = true;
            break;
        }
    }

    if (dropped && (desired & N00B_RW_W_LOCK)) {
        n00b_futex_wake(&lock->futex, true);
    }

    return desired;
}

#ifdef N00B_DEBUG
uint64_t
n00b_rw_debug_write_wakes(void)
{
    n00b_thread_t *thread = n00b_thread_self();
    return thread != nullptr && thread->record != nullptr
             ? thread->record->debug_rw_write_wakes
             : 0;
}

uint64_t
n00b_rw_debug_read_acquires(void)
{
    n00b_thread_t *thread = n00b_thread_self();
    return thread != nullptr && thread->record != nullptr
             ? thread->record->debug_rw_read_acquires
             : 0;
}

uint64_t
n00b_rw_debug_write_acquires(void)
{
    n00b_thread_t *thread = n00b_thread_self();
    return thread != nullptr && thread->record != nullptr
             ? thread->record->debug_rw_write_acquires
             : 0;
}
#endif

bool
_n00b_rw_unlock(n00b_rwlock_t *lock, char *loc)
{
    n00b_runtime_t *rt = n00b_get_runtime();

    // STW-active short-circuit (WP-001): no-op release while the world is
    // stopped (mirrors the acquire short-circuit).
    if (stw_bypass_for_lock(rt, lock)) {
        return true;
    }

    n00b_core_lock_info_t info   = n00b_atomic_load(&lock->data);
    n00b_thread_t        *thread = n00b_thread_self();

    // Writer release (the collector restarting the world).  Owner is keyed on
    // the OS thread id, so this works whether or not n00b_thread_self() is
    // resolvable; any nesting comes out of our write level.  Checked BEFORE
    // touching thread->record so the null-self gate path below is reachable.
    if (info.owner == n00b_thread_os_id(thread)) {
        if (!n00b_lock_release_accounting((void *)lock, loc)) {
            return false;
        }
        n00b_atomic_and(&lock->futex, ~N00B_RW_W_LOCK);
        n00b_futex_wake(&lock->futex, true);
#ifdef N00B_DEBUG
        if (thread != nullptr && thread->record != nullptr) {
            thread->record->debug_rw_write_wakes++;
        }
#endif

        return true;
    }

    n00b_thread_read_log_t *log    = (thread != nullptr && thread->record != nullptr)
                                       ? find_read_lock_record(lock, thread)
                                       : nullptr;

    if (!log) {
        // No read record. Two very different situations reach here, and
        // n00b#521 is about telling them apart by the right question.
        //
        // 1. The reader had NO TCB when it acquired -- a thread inside its own
        //    init/destroy, or a foreign thread n00b never created (the Windows
        //    console-control thread). It holds a raw futex unit and no log,
        //    by design. Drop the unit.
        //
        // 2. A thread WITH a TCB is unlocking something it never locked, or
        //    unlocking twice. That is a real bug and must still abort.
        //
        // So the question is about the THREAD, not the LOCK: that keeps the
        // unbalanced-unlock abort exactly as sharp for every TCB-bearing
        // caller while no longer killing a foreign thread for existing.
        //
        // It re-reads the TCB rather than trusting a flag because a thread
        // can acquire WITH one and release during its own teardown after
        // `record` is gone -- also a record-less drop, not a bug.
        bool releaser_has_tcb = (thread != nullptr && thread->record != nullptr);

        if (!releaser_has_tcb) {
            // Drop the raw futex unit this record-less reader holds. The helper
            // guards against underflow (a double-drop would otherwise wrap the
            // count and latch W_LOCK, wedging the lock) and wakes a draining
            // writer once the count reaches zero.
            _n00b_rw_drop_reader_unit(lock);
            return true;
        }

        abort();
    }

    n00b_thread_record_t *rec = thread->record;

    if (--log->level) {
        return false;
    }

    if (n00b_atomic_load(&rec->read_locks) == log) {
        n00b_atomic_store(&rec->read_locks, log->next_entry);
        assert(!log->prev_entry);
    }
    else {
        assert(log->prev_entry != log);
        log->prev_entry->next_entry = log->next_entry;
    }

    if (log->next_entry) {
        log->next_entry->prev_entry = log->prev_entry;
    }

    if (n00b_atomic_load(&rec->log_alloc_cache)) {
        n00b_atomic_load(&rec->log_alloc_cache)->prev_entry = log;
    }

    log->prev_entry = nullptr;
    log->next_entry = n00b_atomic_load(&rec->log_alloc_cache);
    n00b_atomic_store(&rec->log_alloc_cache, log);

    // Drop one futex unit for this record (underflow-guarded; wakes a draining
    // writer when the count reaches zero -- see _n00b_rw_drop_reader_unit).
    uint32_t desired = _n00b_rw_drop_reader_unit(lock);

    _n00b_runlock_accounting(lock, log, thread, desired, loc);

    return true;
}
