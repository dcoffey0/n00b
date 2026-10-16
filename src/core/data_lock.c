/*
 * Data lock allocation: creates initialized rwlock for data structures.
 */

#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/thread.h"
#include "core/runtime.h"
#include "core/rwlock.h"
#include "core/data_lock.h"

#ifdef N00B_DEBUG
#define N00B_DATA_LOCK_NULL_SITES_MAX 64

static const char      *n00b_data_lock_null_site_list[N00B_DATA_LOCK_NULL_SITES_MAX];
static _Atomic(int32_t) n00b_data_lock_null_site_count;

static void
data_lock_note_null_site(const char *loc)
{
    if (loc == nullptr) {
        loc = "(unknown)";
    }
    int32_t n = n00b_atomic_load(&n00b_data_lock_null_site_count);
    for (int32_t i = 0; i < n && i < N00B_DATA_LOCK_NULL_SITES_MAX; i++) {
        const char *seen = n00b_data_lock_null_site_list[i];
        if (seen != nullptr && strcmp(seen, loc) == 0) {
            return;
        }
    }
    int32_t ix = n00b_atomic_add(&n00b_data_lock_null_site_count, 1);
    if (ix < N00B_DATA_LOCK_NULL_SITES_MAX) {
        n00b_data_lock_null_site_list[ix] = loc;
    }
}

const char *const *
n00b_data_lock_null_sites(int *count)
{
    int32_t n = n00b_atomic_load(&n00b_data_lock_null_site_count);
    *count    = n < N00B_DATA_LOCK_NULL_SITES_MAX ? n : N00B_DATA_LOCK_NULL_SITES_MAX;
    return n00b_data_lock_null_site_list;
}
#endif

n00b_rwlock_t *
n00b_data_lock_new_at() _kargs
{
    n00b_allocator_t *allocator    = nullptr;
    const char       *creation_loc = nullptr;
}
{
    // An rwlock needs the system pool and, to be taken, the taker's thread
    // record. Before those exist no second thread does either.
    if (!n00b_default_runtime_is_set()) {
        return nullptr;
    }
    if (!n00b_atomic_load(&n00b_get_runtime()->threads_may_exist)) {
#ifdef N00B_DEBUG
        data_lock_note_null_site(creation_loc);
#else
        (void)creation_loc;
#endif
        return nullptr;
    }

    n00b_allocator_t *lock_allocator =
        allocator != nullptr ?
            allocator :
            (n00b_allocator_t *)&n00b_get_runtime()->system_pool;
    n00b_rwlock_t *lock =
        n00b_alloc_with_opts(
            n00b_rwlock_t,
            &(n00b_alloc_opts_t){
                .allocator = lock_allocator,
            });

    n00b_rw_init(lock);
    return lock;
}

void
n00b_finalize_data_lock(void *lock_ptr)
{
    n00b_rwlock_t *lock = (n00b_rwlock_t *)lock_ptr;
    if (lock != nullptr) {
        n00b_free(lock);
    }
}
