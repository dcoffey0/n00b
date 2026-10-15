/**
 * @file flagset.h
 * @brief Dynamic bit-backed flag set.
 */
#pragma once

#include "n00b.h"
#include "core/alloc.h"
#include "core/data_lock.h"

#include <stdint.h>

typedef struct n00b_flagset_t {
    uint64_t         *contents;
    uint64_t          num_flags;
    uint64_t          alloc_wordlen;
    n00b_allocator_t *allocator;
    n00b_rwlock_t    *lock;
} n00b_flagset_t;

extern void
n00b_flagset_write_lock(n00b_flagset_t *self);

extern void
n00b_flagset_unlock(n00b_flagset_t *self);

extern n00b_flagset_t *
n00b_flagset_new() _kargs
{
    uint64_t          length    = 64;
    n00b_allocator_t *allocator = nullptr;
    bool              locked    = true;
};

extern void
n00b_flagset_init(n00b_flagset_t *self) _kargs
{
    uint64_t          length    = 64;
    n00b_allocator_t *allocator = nullptr;
    bool              locked    = true;
};

extern void            n00b_flagset_trim(n00b_flagset_t *self);
extern n00b_flagset_t *n00b_flagset_copy(const n00b_flagset_t *self);

/**
 * @brief Set a bit in a set that has one writer and lock-free readers.
 *
 * For a set built with `.locked = false`. Only one thread may call this on a
 * given set at a time, and no other mutator may run alongside it. Growth
 * leaves the previous array allocated, since a reader may still hold it.
 *
 * @return true when the bit was clear before.
 */
extern bool n00b_flagset_set_owned(n00b_flagset_t *self, uint64_t index);

/**
 * @brief Borrow the words of a set written by n00b_flagset_set_owned.
 *
 * Safe against the one writer without a lock. A bit is visible once the
 * reader has synchronized with a release the writer made after setting it:
 * the length store when that bit extended the set, or whatever the caller
 * publishes otherwise. Read each word with a relaxed atomic load, since the
 * writer may be setting other bits in it.
 */
extern void n00b_flagset_view(const n00b_flagset_t *self,
                              const uint64_t      **words,
                              uint64_t             *wordlen);
extern n00b_flagset_t *n00b_flagset_invert(const n00b_flagset_t *self);
extern n00b_flagset_t *n00b_flagset_add(const n00b_flagset_t *self,
                                        const n00b_flagset_t *with);
extern n00b_flagset_t *n00b_flagset_sub(const n00b_flagset_t *self,
                                        const n00b_flagset_t *with);
extern n00b_flagset_t *n00b_flagset_test(const n00b_flagset_t *self,
                                         const n00b_flagset_t *with);
extern n00b_flagset_t *n00b_flagset_xor(const n00b_flagset_t *self,
                                        const n00b_flagset_t *with);
extern bool            n00b_flagset_eq(const n00b_flagset_t *self,
                                       const n00b_flagset_t *other);
extern uint64_t        n00b_flagset_len(const n00b_flagset_t *self);
extern bool            n00b_flagset_index(n00b_flagset_t *self, int64_t index);
extern void            n00b_flagset_set_index(n00b_flagset_t *self,
                                              int64_t          index,
                                              bool             value);
extern bool            n00b_flagset_test_and_set_index(n00b_flagset_t *self,
                                                       int64_t          index,
                                                       bool             value);
extern uint64_t        n00b_flagset_count(const n00b_flagset_t *self);

extern bool
n00b_flagset_next_set(const n00b_flagset_t *self,
                      uint64_t              after,
                      uint64_t             *out_index);
