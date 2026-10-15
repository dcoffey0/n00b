/**
 * @file memory_info.h
 * @brief Memory introspection API.
 *
 * Provides tools for querying memory permissions, scanning address
 * ranges for pointers, looking up allocator ownership, and classifying
 * addresses as heap, static, or unmanaged.
 */
#pragma once

#include "n00b.h"
#include "core/alloc_mdata.h"
#include "core/mmaps.h"
#include "adt/option.h"

typedef struct n00b_memory_scan_t {
    uint64_t *cur;
    uint64_t *end;
    uint32_t  flags;
} n00b_memory_scan_t;

/**
 * @brief Test whether @p addr points to a probable UTF-8 C string.
 * @param addr    Address to test.
 * @param bytelen Output: byte length of the string (if found).
 * @param min_len Minimum codepoint count to qualify.
 * @return        Number of codepoints, or 0 if not a likely string.
 */
extern size_t n00b_address_is_probable_cstring(void *addr, size_t *bytelen, size_t min_len);

/**
 * @brief Initialize a pointer-scanning context over a memory range.
 *
 * Aligns memory and checks the first and last pages for readability.
 * The @p cat_flags field should be an OR of:
 *  - n00b_static_addr
 *  - n00b_managed_heap_addr
 *  - n00b_unmanaged_heap_addr
 * Passing anything else sets all three flags.
 *
 * @param ctx       Scan context to initialize.
 * @param s         Start of the range.
 * @param len       Length in bytes.
 * @param cat_flags Category flags for filtering.
 * @return          true on success, false if memory is unreadable.
 * @pre @p s + @p len does not overflow.
 * @post On success, @p ctx is ready for iteration via n00b_memory_scan_next().
 */
extern bool
n00b_memory_scan_init(n00b_memory_scan_t *ctx, void *s, size_t len) _kargs
{
    uint8_t cat_flags = 0;
};

/**
 * @brief Advance the scanner to the next pointer in the range.
 * @param ctx   Scan context.
 * @param tinfo Output: kind of memory the pointer points to (may be nullptr).
 * @param perms Output: permissions of the target page (may be nullptr).
 * @return      Next pointer found, or none when exhausted.
 */
extern n00b_option_t(void *) n00b_memory_scan_next(n00b_memory_scan_t   *ctx,
                                                    n00b_mmap_rec_kind_t *tinfo,
                                                    n00b_mmap_perms_t    *perms);

/**
 * @brief Check the memory permissions for the page containing @p ptr.
 * @param ptr Address to check.
 * @return    Permission category. @c n00b_mmap_perms_unknown means the
 *            question could not be answered (the probe itself failed, see
 *            n00b_memperm_indeterminate) and is NOT a statement about the
 *            address; callers must not read it as inaccessible.
 */
extern n00b_mmap_perms_t n00b_check_memory_perms(void *ptr);

/* Probe accounting for n00b#275 / n00b#395. fastpath_hits are answered from
 * the mmap registry with no kernel entry; syscall_probes each cost a
 * write/read/poll triple. Their ratio is the perms-unknown rate. */
extern _Atomic uint64_t n00b_memperm_fastpath_hits;
extern _Atomic uint64_t n00b_memperm_syscall_probes;

/* n00b#399: count of probes that could not answer for a reason unrelated to
 * the address (a pipe that could not be created, a poll that never became
 * ready, a non-EFAULT/non-EINTR failure). Such a probe returns
 * n00b_mmap_perms_unknown -- "could not determine" -- instead of no_access,
 * so it is no longer read as "not inside a live object" by a conservative
 * scan and no longer truncates it. EINTR is retried and never counted. */
extern _Atomic uint64_t n00b_memperm_indeterminate;

#if defined(N00B_MEM_INTERNAL_API)
/** @brief Initialize the per-thread memperm pipe cache (internal). */
extern void n00b_init_memperm_pipe_cache(void);
#endif

/**
 * @brief Check whether the page at @p ptr is readable.
 * @param ptr Address to check.
 * @return    true if the page has read or read-write permissions.
 */
static inline bool
n00b_memory_is_readable(void *ptr)
{
    switch (n00b_check_memory_perms(ptr)) {
    case n00b_mmap_perms_ro:
    case n00b_mmap_perms_rw:
        return true;
    default:
        return false;
    }
}

/**
 * @brief Look up mmap info for an address.
 * @param addr Address to look up.
 * @return     Optional mmap info record.
 */
extern n00b_option_t(n00b_mmap_info_t *) n00b_mmap_info_lookup(const void *addr);

/**
 * @brief Find the allocator responsible for an address.
 * @param val Address to look up.
 * @return    Optional allocator pointer.
 */
extern n00b_option_t(n00b_allocator_t *) n00b_find_allocator(void *val);
// Find all global mmap entries with an exact name match.
// TODO: replace
// extern n00b_list_t      *n00b_mmaps_by_file(char *);

/**
 * @brief Check whether @p ptr is a data value (not a valid pointer into a mapping).
 * @param ptr Value to check.
 * @return    true if @p ptr is not a recognized pointer.
 */
static inline bool
n00b_value_is_data(void *ptr)
{
    auto mmap_opt = n00b_mmap_info_lookup(ptr);

    return (!n00b_option_is_set(mmap_opt)
            || n00b_check_memory_perms(ptr) == n00b_mmap_perms_no_access);
}

/**
 * @brief Check whether @p ptr is likely a valid pointer.
 * @param ptr Value to check.
 * @return    true if @p ptr points into a known mapping.
 */
static inline bool
n00b_value_is_likely_pointer(void *ptr)
{
    return !n00b_value_is_data(ptr);
}

/**
 * @brief Check whether @p ptr is inside a GC-managed heap region.
 *
 * Direct mmap calls are not considered in-heap; only allocations
 * through the arena interface qualify.
 *
 * @param ptr Address to check.
 * @return    true if inside a managed segment or pool.
 */
static inline bool
n00b_in_heap(void *ptr)
{
    n00b_mmap_view_t view;

    if (n00b_mmap_view_by_address(ptr, &view)) {
        switch (view.kind) {
        case n00b_mmap_managed_segment:
        case n00b_mmap_sys_segment:
        case n00b_mmap_pool:
            return true;
        default:
            break;
        }
    }
    return false;
}

/**
 * @brief Check whether @p ptr is in a registered thread stack mapping.
 *
 * Stack memory is already scanned while the owning thread is live. It
 * must not be registered as a durable GC root because the storage stops
 * being valid when the stack frame or thread exits.
 */
static inline bool
n00b_in_stack(void *ptr)
{
    n00b_mmap_view_t view;

    return n00b_mmap_view_by_address(ptr, &view) && view.kind == n00b_mmap_stack;
}

#if defined(N00B_USE_INTERNAL_API)
extern uint64_t n00b_gc_guard;

/**
 * @brief Check whether an inline header is a metadata record.
 * @param h Inline header to check.
 * @return  true if the guard's low bit is clear (metadata record).
 */
static inline bool
n00b_is_mem_metadata_record(n00b_inline_hdr_t *h)
{
    return !(h->guard & 1);
}
#endif

#if !defined(N00B_MEM_INTERNAL_API)
/** @brief Register the process's static data segments in the mmap registry. */
extern void n00b_load_static_ranges(void);
#endif

#if defined(N00B_SHOW_ARENAS_ON_GC)
/**
 * @brief Dump arena memory info for debugging.
 * @param all If true, print all arenas; otherwise only active ones.
 */
extern void n00b_debug_memory_info(bool all);
#endif

#if defined(N00B_USE_INTERNAL_API) || defined(N00B_MEM_INTERNAL_API)
/**
 * @brief Cast an inline header to the out-of-band metadata record.
 *
 * Returns @c n00b_option_none if @p info is null or has the inline guard
 * bit set (meaning it is a true inline header, not an aliased OOB record);
 * otherwise wraps the OOB record in @c n00b_option_set.
 *
 * @param info  Pointer to an inline header (may actually be OOB).
 * @return      The OOB record wrapped in `n00b_option_t`, or
 *              `n00b_option_none` when @p info is null or genuinely inline.
 */
static inline n00b_option_t(n00b_oob_hdr_t *)
    n00b_to_mem_metadata_record(n00b_inline_hdr_t *info)
{
    if (!info || (info->guard & 1)) {
        return n00b_option_none(n00b_oob_hdr_t *);
    }

    return n00b_option_set(n00b_oob_hdr_t *, (n00b_oob_hdr_t *)info);
}
#endif

// n00b_get_arena_addr_type is defined in arena.h (needs full n00b_arena_t definition)
