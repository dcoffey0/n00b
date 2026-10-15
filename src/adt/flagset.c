#include "adt/flagset.h"

#include "core/data_lock.h"
#include "util/assert.h"
#include "core/gc_map.h"

static uint64_t
flagset_words_for_bits(uint64_t bits)
{
    return bits == 0 ? 1 : (bits + 63u) / 64u;
}

static uint64_t
flagset_tail_mask(uint64_t bits)
{
    uint64_t rem = bits & 63u;
    if (rem == 0) {
        return UINT64_MAX;
    }
    return (1ull << rem) - 1ull;
}

// Words that hold bits below num_flags. The array may run past this: growth
// doubles it, and every word beyond the used ones stays zero.
static uint64_t
flagset_used_words(const n00b_flagset_t *self)
{
    return n00b_min(flagset_words_for_bits(self->num_flags),
                    self->alloc_wordlen);
}

static void
flagset_zero_tail(n00b_flagset_t *self)
{
    if (self == nullptr || self->contents == nullptr || self->alloc_wordlen == 0) {
        return;
    }
    self->contents[flagset_used_words(self) - 1]
        &= flagset_tail_mask(self->num_flags);
}

static void
flagset_realloc(n00b_flagset_t *self, uint64_t new_words)
{
    // The bit array is pure data (membership bits), never pointers, so it
    // MUST be no-scan: otherwise the conservative scan / marshaler reads
    // bitmap words as candidate pointers, which (e.g. when a flagset-backed
    // dense posting list is marshaled at rocs seal) misfires as an
    // unregistered-static-pointer and fails the whole marshal.  The typed
    // (uint64_t) alloc already yields a scalar GC map; .scan_kind=NONE is
    // explicit belt-and-suspenders.
    uint64_t *contents = n00b_alloc_array_with_opts(
        uint64_t,
        new_words,
        &(n00b_alloc_opts_t){.allocator = self->allocator,
                             .scan_kind = N00B_GC_SCAN_KIND_NONE});
    memset(contents, 0, new_words * sizeof(uint64_t));
    uint64_t copy_words = n00b_min(self->alloc_wordlen, new_words);
    if (copy_words != 0 && self->contents != nullptr) {
        memcpy(contents, self->contents, copy_words * sizeof(uint64_t));
    }
    if (self->contents != nullptr) {
        n00b_free(self->contents);
    }
    self->contents      = contents;
    self->alloc_wordlen = new_words;
}

// Growth at least doubles the array, so setting bits 0..n-1 in order costs
// O(log n) reallocations and O(n) words copied in total.
static void
flagset_resize(n00b_flagset_t *self, uint64_t new_num_flags)
{
    uint64_t new_words = flagset_words_for_bits(new_num_flags);
    if (new_words > self->alloc_wordlen) {
        flagset_realloc(self, n00b_max(new_words, self->alloc_wordlen * 2));
    }
    else if (new_num_flags < self->num_flags) {
        uint64_t old_words = flagset_used_words(self);
        if (old_words > new_words) {
            memset(self->contents + new_words,
                   0,
                   (old_words - new_words) * sizeof(uint64_t));
        }
    }

    self->num_flags = new_num_flags;
    flagset_zero_tail(self);
}

static uint64_t
flagset_normalize_index(n00b_flagset_t *self, int64_t index, bool grow)
{
    if (index < 0) {
        index += (int64_t)self->num_flags;
        if (index < 0) {
            n00b_require(false, "negative flagset index is out of bounds");
        }
    }

    uint64_t normalized = (uint64_t)index;
    if (normalized >= self->num_flags && grow) {
        flagset_resize(self, normalized + 1);
    }
    return normalized;
}

n00b_flagset_t *
n00b_flagset_new() _kargs
{
    uint64_t          length    = 64;
    n00b_allocator_t *allocator = nullptr;
    bool              locked    = true;
}
{
    n00b_flagset_t *self = n00b_alloc_with_opts(
        n00b_flagset_t,
        &(n00b_alloc_opts_t){.allocator = allocator});
    n00b_flagset_init(self,
                      .length = length,
                      .allocator = allocator,
                      .locked = locked);
    return self;
}

void
n00b_flagset_init(n00b_flagset_t *self) _kargs
{
    uint64_t          length    = 64;
    n00b_allocator_t *allocator = nullptr;
    bool              locked    = true;
}
{
    if (self == nullptr) {
        return;
    }
    self->contents      = nullptr;
    self->num_flags     = 0;
    self->alloc_wordlen = 0;
    self->allocator     = allocator;
    self->lock          = locked ? n00b_data_lock_new(.allocator = allocator)
                                 : nullptr;
    flagset_resize(self, length);
}

// Single writer, lock-free readers. Contents are published before the length
// fields, and n00b_flagset_view loads them in the opposite order, so a reader
// that sees a length also sees an array at least that long. A grown-out array
// is never freed, because a reader may still be walking it; it goes back to
// the allocator with everything else the set's pool holds.
bool
n00b_flagset_set_owned(n00b_flagset_t *self, uint64_t index)
{
    n00b_assert(self != nullptr && self->lock == nullptr);

    uint64_t  num_flags = __atomic_load_n(&self->num_flags, __ATOMIC_RELAXED);
    uint64_t *words     = __atomic_load_n(&self->contents, __ATOMIC_RELAXED);
    uint64_t  wordlen   = __atomic_load_n(&self->alloc_wordlen, __ATOMIC_RELAXED);

    if (index >= num_flags) {
        uint64_t need = flagset_words_for_bits(index + 1);
        if (need > wordlen) {
            uint64_t  grown_len = n00b_max(need, wordlen * 2);
            uint64_t *grown     = n00b_alloc_array_with_opts(
                uint64_t,
                grown_len,
                &(n00b_alloc_opts_t){.allocator = self->allocator,
                                     .scan_kind = N00B_GC_SCAN_KIND_NONE});
            memset(grown, 0, grown_len * sizeof(uint64_t));
            if (words != nullptr && wordlen != 0) {
                memcpy(grown, words, wordlen * sizeof(uint64_t));
            }
            __atomic_store_n(&self->contents, grown, __ATOMIC_RELEASE);
            __atomic_store_n(&self->alloc_wordlen, grown_len, __ATOMIC_RELEASE);
            words = grown;
        }
    }

    uint64_t *slot  = &words[index >> 6];
    uint64_t  bit   = 1ull << (index & 63u);
    uint64_t  word  = __atomic_load_n(slot, __ATOMIC_RELAXED);
    bool      fresh = (word & bit) == 0;
    if (fresh) {
        __atomic_store_n(slot, word | bit, __ATOMIC_RELAXED);
    }
    if (index >= num_flags) {
        __atomic_store_n(&self->num_flags, index + 1, __ATOMIC_RELEASE);
    }
    return fresh;
}

void
n00b_flagset_view(const n00b_flagset_t *self,
                  const uint64_t      **words,
                  uint64_t             *wordlen)
{
    uint64_t bits = __atomic_load_n(&self->num_flags, __ATOMIC_ACQUIRE);
    *words        = __atomic_load_n(&self->contents, __ATOMIC_ACQUIRE);
    *wordlen      = (bits + 63u) / 64u;
}

// Release the doubling slack, so an image marshaled from the set carries only
// the words that hold bits.
void
n00b_flagset_trim(n00b_flagset_t *self)
{
    if (self == nullptr) {
        return;
    }
    n00b_data_write_lock(self->lock);
    uint64_t used = flagset_used_words(self);
    if (self->contents != nullptr && self->alloc_wordlen > used) {
        flagset_realloc(self, used);
    }
    n00b_data_unlock(self->lock);
}

n00b_flagset_t *
n00b_flagset_copy(const n00b_flagset_t *self)
{
    if (self == nullptr) {
        return nullptr;
    }
    n00b_data_read_lock(self->lock);
    n00b_flagset_t *result =
        n00b_flagset_new(.length = self->num_flags,
                         .allocator = self->allocator);
    memcpy(result->contents,
           self->contents,
           flagset_used_words(self) * sizeof(uint64_t));
    n00b_data_unlock(self->lock);
    return result;
}

n00b_flagset_t *
n00b_flagset_invert(const n00b_flagset_t *self)
{
    n00b_flagset_t *result = n00b_flagset_copy(self);
    if (result == nullptr) {
        return nullptr;
    }

    for (uint64_t i = 0; i < flagset_used_words(result); i++) {
        result->contents[i] = ~result->contents[i];
    }
    flagset_zero_tail(result);
    return result;
}

static n00b_flagset_t *
flagset_binary_new(const n00b_flagset_t *self, const n00b_flagset_t *with)
{
    if (self == nullptr || with == nullptr) {
        return nullptr;
    }
    return n00b_flagset_new(.length = n00b_max(self->num_flags,
                                               with->num_flags),
                            .allocator = self->allocator);
}

n00b_flagset_t *
n00b_flagset_add(const n00b_flagset_t *self, const n00b_flagset_t *with)
{
    if (self == nullptr || with == nullptr) {
        return nullptr;
    }
    bool same = self == with;
    n00b_data_read_lock(self->lock);
    if (!same) {
        n00b_data_read_lock(with->lock);
    }

    n00b_flagset_t *result = flagset_binary_new(self, with);
    if (result == nullptr) {
        if (!same) {
            n00b_data_unlock(with->lock);
        }
        n00b_data_unlock(self->lock);
        return nullptr;
    }

    uint64_t self_words = flagset_used_words(self);
    uint64_t with_words = flagset_used_words(with);
    uint64_t min_words  = n00b_min(self_words, with_words);
    for (uint64_t i = 0; i < min_words; i++) {
        result->contents[i] = self->contents[i] | with->contents[i];
    }
    for (uint64_t i = min_words; i < flagset_used_words(result); i++) {
        result->contents[i] = self_words > with_words
                                  ? self->contents[i]
                                  : with->contents[i];
    }
    flagset_zero_tail(result);
    if (!same) {
        n00b_data_unlock(with->lock);
    }
    n00b_data_unlock(self->lock);
    return result;
}

n00b_flagset_t *
n00b_flagset_sub(const n00b_flagset_t *self, const n00b_flagset_t *with)
{
    if (self == nullptr || with == nullptr) {
        return nullptr;
    }
    bool same = self == with;
    n00b_data_read_lock(self->lock);
    if (!same) {
        n00b_data_read_lock(with->lock);
    }

    n00b_flagset_t *result = flagset_binary_new(self, with);
    if (result == nullptr) {
        if (!same) {
            n00b_data_unlock(with->lock);
        }
        n00b_data_unlock(self->lock);
        return nullptr;
    }

    uint64_t self_words = flagset_used_words(self);
    uint64_t with_words = flagset_used_words(with);
    uint64_t min_words  = n00b_min(self_words, with_words);
    for (uint64_t i = 0; i < min_words; i++) {
        result->contents[i] = self->contents[i] & ~with->contents[i];
    }
    for (uint64_t i = min_words; i < self_words; i++) {
        result->contents[i] = self->contents[i];
    }
    flagset_zero_tail(result);
    if (!same) {
        n00b_data_unlock(with->lock);
    }
    n00b_data_unlock(self->lock);
    return result;
}

n00b_flagset_t *
n00b_flagset_test(const n00b_flagset_t *self, const n00b_flagset_t *with)
{
    if (self == nullptr || with == nullptr) {
        return nullptr;
    }
    bool same = self == with;
    n00b_data_read_lock(self->lock);
    if (!same) {
        n00b_data_read_lock(with->lock);
    }

    n00b_flagset_t *result = flagset_binary_new(self, with);
    if (result == nullptr) {
        if (!same) {
            n00b_data_unlock(with->lock);
        }
        n00b_data_unlock(self->lock);
        return nullptr;
    }

    uint64_t self_words = flagset_used_words(self);
    uint64_t with_words = flagset_used_words(with);
    uint64_t min_words  = n00b_min(self_words, with_words);
    for (uint64_t i = 0; i < min_words; i++) {
        result->contents[i] = self->contents[i] & with->contents[i];
    }
    if (!same) {
        n00b_data_unlock(with->lock);
    }
    n00b_data_unlock(self->lock);
    return result;
}

n00b_flagset_t *
n00b_flagset_xor(const n00b_flagset_t *self, const n00b_flagset_t *with)
{
    if (self == nullptr || with == nullptr) {
        return nullptr;
    }
    bool same = self == with;
    n00b_data_read_lock(self->lock);
    if (!same) {
        n00b_data_read_lock(with->lock);
    }

    n00b_flagset_t *result = flagset_binary_new(self, with);
    if (result == nullptr) {
        if (!same) {
            n00b_data_unlock(with->lock);
        }
        n00b_data_unlock(self->lock);
        return nullptr;
    }

    uint64_t self_words = flagset_used_words(self);
    uint64_t with_words = flagset_used_words(with);
    uint64_t min_words  = n00b_min(self_words, with_words);
    for (uint64_t i = 0; i < min_words; i++) {
        result->contents[i] = self->contents[i] ^ with->contents[i];
    }
    for (uint64_t i = min_words; i < flagset_used_words(result); i++) {
        result->contents[i] = self_words > with_words
                                  ? self->contents[i]
                                  : with->contents[i];
    }
    flagset_zero_tail(result);
    if (!same) {
        n00b_data_unlock(with->lock);
    }
    n00b_data_unlock(self->lock);
    return result;
}

bool
n00b_flagset_eq(const n00b_flagset_t *self, const n00b_flagset_t *other)
{
    if (self == nullptr || other == nullptr) {
        return self == other;
    }

    bool same = self == other;
    n00b_data_read_lock(self->lock);
    if (!same) {
        n00b_data_read_lock(other->lock);
    }
    uint64_t sum       = 0;
    uint64_t self_words  = flagset_used_words(self);
    uint64_t other_words = flagset_used_words(other);
    uint64_t low_words   = n00b_min(self_words, other_words);
    for (uint64_t i = 0; i < low_words; i++) {
        sum |= self->contents[i] ^ other->contents[i];
    }

    const n00b_flagset_t *high = self_words > other_words
                                     ? self
                                     : other;
    uint64_t high_words = n00b_max(self_words, other_words);
    for (uint64_t i = low_words; i < high_words; i++) {
        sum |= high->contents[i];
    }
    bool result = sum == 0;
    if (!same) {
        n00b_data_unlock(other->lock);
    }
    n00b_data_unlock(self->lock);
    return result;
}

uint64_t
n00b_flagset_len(const n00b_flagset_t *self)
{
    if (self == nullptr) {
        return 0;
    }
    n00b_data_read_lock(self->lock);
    uint64_t result = self->num_flags;
    n00b_data_unlock(self->lock);
    return result;
}

bool
n00b_flagset_index(n00b_flagset_t *self, int64_t index)
{
    if (self == nullptr) {
        return false;
    }

    n00b_data_read_lock(self->lock);
    uint64_t normalized = flagset_normalize_index(self, index, false);
    if (normalized >= self->num_flags) {
        n00b_data_unlock(self->lock);
        return false;
    }
    bool result =
        (self->contents[normalized >> 6] & (1ull << (normalized & 63u))) != 0;
    n00b_data_unlock(self->lock);
    return result;
}

void
n00b_flagset_set_index(n00b_flagset_t *self, int64_t index, bool value)
{
    if (self == nullptr) {
        return;
    }

    n00b_data_write_lock(self->lock);
    uint64_t normalized = flagset_normalize_index(self, index, true);
    uint64_t flag       = 1ull << (normalized & 63u);
    uint64_t word       = normalized >> 6;
    if (value) {
        self->contents[word] |= flag;
    }
    else {
        self->contents[word] &= ~flag;
    }
    n00b_data_unlock(self->lock);
}

// Lock the set for a caller that needs more than one operation to happen
// together. A population count kept beside the bits is the motivating case:
// updating the bit and the count under separate acquisitions lets a reader see
// one without the other.
void
n00b_flagset_write_lock(n00b_flagset_t *self)
{
    if (self != nullptr) {
        n00b_data_write_lock(self->lock);
    }
}

void
n00b_flagset_unlock(n00b_flagset_t *self)
{
    if (self != nullptr) {
        n00b_data_unlock(self->lock);
    }
}

// The body of test_and_set, without taking the lock, for a caller already
// holding the write lock so that the bit and a count beside it move together.
// Not exported: the rwlock is reentrant, so the locked entry point is already
// safe to call under the lock, and an unlocked variant on the public surface
// would only invite an unlocked caller.
static bool
n00b_flagset_test_and_set_index_unlocked(n00b_flagset_t *self,
                                         int64_t         index,
                                         bool            value)
{
    if (self == nullptr) {
        return false;
    }
    uint64_t normalized = flagset_normalize_index(self, index, true);
    uint64_t flag       = 1ull << (normalized & 63u);
    uint64_t word       = normalized >> 6;
    bool     old        = (self->contents[word] & flag) != 0;
    if (value) {
        self->contents[word] |= flag;
    }
    else {
        self->contents[word] &= ~flag;
    }
    return old;
}

bool
n00b_flagset_test_and_set_index(n00b_flagset_t *self, int64_t index, bool value)
{
    if (self == nullptr) {
        return false;
    }

    n00b_data_write_lock(self->lock);
    bool old = n00b_flagset_test_and_set_index_unlocked(self, index, value);
    n00b_data_unlock(self->lock);
    return old;
}

uint64_t
n00b_flagset_count(const n00b_flagset_t *self)
{
    if (self == nullptr) {
        return 0;
    }

    n00b_data_read_lock(self->lock);
    uint64_t count = 0;
    for (uint64_t i = 0; i < flagset_used_words(self); i++) {
        count += (uint64_t)__builtin_popcountll(self->contents[i]);
    }
    n00b_data_unlock(self->lock);
    return count;
}

bool
n00b_flagset_next_set(const n00b_flagset_t *self,
                      uint64_t              after,
                      uint64_t             *out_index)
{
    if (self == nullptr || out_index == nullptr) {
        return false;
    }

    n00b_data_read_lock(self->lock);
    if (after >= self->num_flags) {
        n00b_data_unlock(self->lock);
        return false;
    }

    uint64_t word_ix = after >> 6;
    uint64_t word    = self->contents[word_ix] & (~0ull << (after & 63u));
    while (true) {
        if (word != 0) {
            uint64_t bit = (uint64_t)__builtin_ctzll(word);
            uint64_t ix  = (word_ix << 6) + bit;
            if (ix >= self->num_flags) {
                n00b_data_unlock(self->lock);
                return false;
            }
            *out_index = ix;
            n00b_data_unlock(self->lock);
            return true;
        }

        word_ix++;
        if (word_ix >= flagset_used_words(self)) {
            n00b_data_unlock(self->lock);
            return false;
        }
        word = self->contents[word_ix];
    }
}
