#define N00B_MEM_INTERNAL_API
#define N00B_USE_INTERNAL_API

#include <signal.h>
#include <errno.h>
#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <time.h>
#else
#include "core/platform.h"
#endif

#include "n00b.h"
#include "core/mmaps.h"
#include "core/alloc_mdata.h"
#include "core/memory_info.h"
#include "core/align.h"
#include "core/atomic.h"
#include "core/thread.h"
#include "core/syscall.h"
#include "core/arena.h"
#include "core/runtime.h"
#include "core/static_objects.h"
#include "adt/interval_tree.h"
#include "adt/variant.h"
#include "text/unicode/encoding.h"
#include "text/unicode/properties.h"

// TODO
// #include "core/stw.h"

// TODO
#define n00b_fprintf fprintf
#include <stdio.h>

_Atomic uint64_t static_order_id = 1;

static inline bool
n00b_mmap_perms_known(n00b_mmap_perms_t perms)
{
    return perms != n00b_mmap_perms_unknown;
}

static inline n00b_mmap_perms_t
n00b_mmap_perms_from_bits(bool readable, bool writable)
{
    if (writable) {
        return n00b_mmap_perms_rw;
    }
    if (readable) {
        return n00b_mmap_perms_ro;
    }
    return n00b_mmap_perms_no_access;
}

#if defined(_WIN32)
static inline n00b_mmap_perms_t
n00b_mmap_perms_from_win_protect(DWORD protect)
{
    if (protect & (PAGE_READWRITE | PAGE_WRITECOPY
                   | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) {
        return n00b_mmap_perms_rw;
    }
    if (protect & (PAGE_READONLY | PAGE_EXECUTE_READ | PAGE_EXECUTE)) {
        return n00b_mmap_perms_ro;
    }
    return n00b_mmap_perms_no_access;
}
#endif

static n00b_memperm_pipe_t *
n00b_memperm_pipe_get(void)
{
    n00b_thread_t *self = n00b_thread_self();
    return self == nullptr ? nullptr : &self->memperm_pipe;
}

#if defined(__linux__)
static inline int
n00b_memperm_raw_pipe(int fds[2])
{
#if defined(SYS_pipe2)
    return (int)_n00b_raw_linux_syscall3(SYS_pipe2,
                                         (long)(uintptr_t)fds,
                                         0,
                                         0);
#elif defined(SYS_pipe)
    return (int)_n00b_raw_linux_syscall3(SYS_pipe,
                                         (long)(uintptr_t)fds,
                                         0,
                                         0);
#else
#error "Linux memperm probe requires SYS_pipe2 or SYS_pipe."
#endif
}

static inline int
n00b_memperm_raw_fcntl(int fd, int cmd, long arg)
{
    return (int)_n00b_raw_linux_syscall3(SYS_fcntl,
                                         (long)fd,
                                         (long)cmd,
                                         arg);
}

static inline long
n00b_memperm_raw_write(int fd, const void *buf, unsigned long len)
{
    return _n00b_raw_linux_syscall3(SYS_write,
                                    (long)fd,
                                    (long)(uintptr_t)buf,
                                    (long)len);
}

static inline long
n00b_memperm_raw_read(int fd, void *buf, unsigned long len)
{
    return _n00b_raw_linux_syscall3(SYS_read,
                                    (long)fd,
                                    (long)(uintptr_t)buf,
                                    (long)len);
}

static inline int
n00b_memperm_raw_poll(struct pollfd *fds, unsigned long nfds, int timeout_ms)
{
#if defined(SYS_poll)
    return (int)_n00b_raw_linux_syscall3(SYS_poll,
                                         (long)(uintptr_t)fds,
                                         (long)nfds,
                                         (long)timeout_ms);
#elif defined(SYS_ppoll)
    struct timespec timeout;
    struct timespec *timeout_ptr = nullptr;
    if (timeout_ms >= 0) {
        timeout = (struct timespec){
            .tv_sec  = timeout_ms / 1000,
            .tv_nsec = (timeout_ms % 1000) * 1000000,
        };
        timeout_ptr = &timeout;
    }
    return (int)_n00b_raw_linux_syscall5(SYS_ppoll,
                                         (long)(uintptr_t)fds,
                                         (long)nfds,
                                         (long)(uintptr_t)timeout_ptr,
                                         0,
                                         0);
#else
#error "Linux memperm probe requires SYS_poll or SYS_ppoll."
#endif
}

static inline void
n00b_memperm_raw_close(int fd)
{
    (void)_n00b_raw_linux_syscall1(SYS_close, (long)fd);
}

#define N00B_MEMPERM_PIPE(fds)           n00b_memperm_raw_pipe((fds))
#define N00B_MEMPERM_FCNTL(fd, cmd, arg) n00b_memperm_raw_fcntl((fd), (cmd), (long)(arg))
#define N00B_MEMPERM_WRITE(fd, buf, len) n00b_memperm_raw_write((fd), (buf), (len))
#define N00B_MEMPERM_READ(fd, buf, len)  n00b_memperm_raw_read((fd), (buf), (len))
#define N00B_MEMPERM_POLL(fds, n, to)    n00b_memperm_raw_poll((fds), (n), (to))
#define N00B_MEMPERM_CLOSE(fd)           n00b_memperm_raw_close((fd))
/* Raw syscalls return -errno directly; libc returns -1 and sets errno.
 * Normalised here so the EFAULT test below reads the same on both (n00b#399). */
#define N00B_MEMPERM_ERRNO(rc)           ((rc) < 0 ? (int)-(rc) : 0)
#else
#define N00B_MEMPERM_PIPE(fds)           pipe((fds))
#define N00B_MEMPERM_FCNTL(fd, cmd, arg) fcntl((fd), (cmd), (arg))
#define N00B_MEMPERM_WRITE(fd, buf, len) write((fd), (buf), (len))
#define N00B_MEMPERM_READ(fd, buf, len)  read((fd), (buf), (len))
#define N00B_MEMPERM_POLL(fds, n, to)    poll((fds), (n), (to))
#define N00B_MEMPERM_CLOSE(fd)           close((fd))
#define N00B_MEMPERM_ERRNO(rc)           ((rc) < 0 ? errno : 0)
#endif

extern void n00b_debug_memory_info(bool);

// For right now, this is dumb. But it should ultimately keep sorted
// while inserting, and binary search. Will do that when we move it to
// a proper list.
//
// Note that we never record data ranges; anything not registered as a
// segment by the time a call returns here is considered data.

#define MINCORE_TEST_BIT 1

#if defined(__linux__)
#include <link.h>

static int
n00b_extract_lib_info(struct dl_phdr_info *info, size_t size, void *unused)
{
    int64_t intaddr;
    void   *startp;
    void   *endp;

    for (uint32_t i = 0; i < info->dlpi_phnum; i++) {
        size    = info->dlpi_phdr[i].p_vaddr;
        intaddr = (int64_t)(info->dlpi_addr + size);
        startp  = (void *)intaddr;
        endp    = (void *)(intaddr + info->dlpi_phdr[i].p_memsz);

        if (startp == endp) {
            continue;
        }
        n00b_mmap_perms_t perms = startp
                                    ? n00b_mmap_perms_from_bits(
                                          (info->dlpi_phdr[i].p_flags & PF_R) != 0,
                                          (info->dlpi_phdr[i].p_flags & PF_W) != 0)
                                    : n00b_mmap_perms_no_access;

        /* Same hazard as the Apple path above (n00b#290): p_memsz is the
         * segment's virtual size and p_filesz is what the file actually
         * backs. The .bss tail beyond p_filesz is zero-fill the kernel may
         * not have committed, so claiming p_flags across the whole span
         * lets n00b_check_memory_perms hand out a readable verdict for a
         * page that faults on the next load.
         *
         * Registering the tail `unknown` makes perms_known() false there,
         * which routes it to the live probe instead of a value nothing
         * verified. Only the tail pays that cost. */
        void *backed_endp = (void *)(intaddr + info->dlpi_phdr[i].p_filesz);
        if (startp && info->dlpi_phdr[i].p_filesz < info->dlpi_phdr[i].p_memsz) {
            if (backed_endp > startp) {
                (void)n00b_mmap_register(startp,
                                         backed_endp,
                                         n00b_mmap_static,
                                         .file          = (char *)info->dlpi_name,
                                         .binary_offset = info->dlpi_phdr[i].p_offset,
                                         .slide         = -(intptr_t)info->dlpi_addr,
                                         .order_id      = n00b_atomic_add(&static_order_id, 1),
                                         .perms         = perms,
                                         .definitely_unique = false);
            }
            (void)n00b_mmap_register(backed_endp > startp ? backed_endp : startp,
                                     endp,
                                     n00b_mmap_static,
                                     .file          = (char *)info->dlpi_name,
                                     .binary_offset = info->dlpi_phdr[i].p_offset,
                                     .slide         = -(intptr_t)info->dlpi_addr,
                                     .order_id      = n00b_atomic_add(&static_order_id, 1),
                                     .perms         = n00b_mmap_perms_unknown,
                                     .definitely_unique = false);
            continue;
        }

        (void)n00b_mmap_register(startp,
                                 endp,
                                 startp ? n00b_mmap_static : n00b_mmap_zero_page,
                                 .file              = (char *)info->dlpi_name,
                                 .binary_offset     = info->dlpi_phdr[i].p_offset,
                                 .slide             = -(intptr_t)info->dlpi_addr,
                                 .order_id          = n00b_atomic_add(&static_order_id, 1),
                                 .perms             = perms,
                                 .definitely_unique = false);
    }
    return 0;
}

#if defined(N00B_ALWAYS_RECHECK_STATIC_POINTERS)
static inline n00b_option_t(n00b_mmap_info_t *)
n00b_check_static_maps(void *addr)
{
    dl_iterate_phdr(n00b_extract_lib_info, nullptr);
    return n00b_mmap_by_address(addr);
}
#endif

void
n00b_load_static_ranges(void)
{
    dl_iterate_phdr(n00b_extract_lib_info, nullptr);
    (void)n00b_static_objects_register_all();
}

#elifdef __APPLE__
#include <dlfcn.h>         // for dladdr, Dl_info
#include <mach-o/dyld.h>   // for _dyld_register_func_for_add_image
#include <mach-o/loader.h> // for segment_command_64, mach_header_64, LC_...
#include <mach/vm_prot.h>

void
n00b_on_lib_load(const struct mach_header *hdr, intptr_t slide)
{
    if (hdr->magic != MH_MAGIC_64) {
        n00b_fprintf(stderr, "Only 64-bit supported.\n");
        abort();
    }

    struct mach_header_64     *header  = (struct mach_header_64 *)hdr;
    char                      *start   = (char *)&header[1];
    struct segment_command_64 *command = (struct segment_command_64 *)start;

    Dl_info info;
    dladdr(header, &info);

    for (uint32_t i = 0; i < header->ncmds; i++) {
        uint64_t seg_start = command->vmaddr + slide;
        uint64_t seg_end   = seg_start + command->vmsize;

        if (command->cmd == LC_SEGMENT_64 && seg_start != seg_end
            && command->initprot == VM_PROT_NONE && command->vmaddr == 0
            && slide != 0) {
            /* n00b-lang/n00b#375: __PAGEZERO of a position-independent image.
             * It is vmaddr 0, vmsize 4 GB, filesize 0, no access, and it is
             * NOT slid: the real zero page is [0, 4 GB) regardless of where
             * dyld put the image. Applying the slide registered
             * [slide, slide + 4 GB) as a static range with unknown perms, a
             * region that covers most of the low address space. Every
             * conservative scan then took any word in it -- a packed
             * `flags << 32 | small` scalar, say -- for a static pointer, and
             * the marshaller rejected the object it lived in. Nothing can be
             * addressed through this segment, so nothing is lost by not
             * recording it; the unslid (slide == 0) case still registers it
             * as the zero page below. */
            start += command->cmdsize;
            command = (void *)start;
            continue;
        }

        if (command->cmd == LC_SEGMENT_64 && seg_start != seg_end) {
            n00b_mmap_perms_t perms = seg_start
                                        ? n00b_mmap_perms_from_bits(
                                              (command->initprot & VM_PROT_READ) != 0,
                                              (command->initprot & VM_PROT_WRITE) != 0)
                                        : n00b_mmap_perms_no_access;

            /* n00b#290: vmsize is the segment's VIRTUAL size; only filesize
             * bytes are backed by the file. The tail is reserved address
             * space that the kernel has not necessarily committed, and
             * reading it SIGBUSes (not SIGSEGV -- the address IS mapped).
             *
             * The gap is not marginal. In a n00b test binary __DATA carries
             * vmsize 13,041,664 against filesize 212,992: 98.4% of the
             * segment is unbacked.
             *
             * Recording initprot across the whole span told
             * n00b_check_memory_perms the tail was readable, and its
             * registry fast path returns that answer without asking the
             * kernel. _find_sentinal's page-safety check then cleared a
             * page it could not actually read, and the very next load --
             * the last word of the last mapped page -- took the signal.
             *
             * So the backed part keeps its recorded perms and its fast
             * path, and the tail is registered `unknown` instead. That is
             * the honest answer, and it makes perms_known() false there so
             * the check falls through to the live syscall probe rather than
             * trusting a value nothing ever verified. Only the tail pays
             * the probe, which matters because the conservative scan is
             * already syscall-bound (n00b#275). */
            uint64_t backed_end = seg_start + command->filesize;
            if (command->filesize < command->vmsize && seg_start) {
                if (backed_end > seg_start) {
                    (void)n00b_mmap_register((void *)seg_start,
                                             (void *)backed_end,
                                             n00b_mmap_static,
                                             .file          = (char *)info.dli_fname,
                                             .binary_offset = command->fileoff,
                                             .slide         = -slide,
                                             .order_id      = n00b_atomic_add(&static_order_id, 1),
                                             .perms         = perms,
                                             .definitely_unique = false);
                }
                (void)n00b_mmap_register((void *)(backed_end > seg_start
                                                      ? backed_end
                                                      : seg_start),
                                         (void *)seg_end,
                                         n00b_mmap_static,
                                         .file          = (char *)info.dli_fname,
                                         .binary_offset = command->fileoff,
                                         .slide         = -slide,
                                         .order_id      = n00b_atomic_add(&static_order_id, 1),
                                         .perms         = n00b_mmap_perms_unknown,
                                         .definitely_unique = false);
            }
            else {
                (void)n00b_mmap_register((void *)seg_start,
                                         (void *)seg_end,
                                         seg_start ? n00b_mmap_static : n00b_mmap_zero_page,
                                         .file              = (char *)info.dli_fname,
                                         .binary_offset     = command->fileoff,
                                         .slide             = -slide,
                                         .order_id          = n00b_atomic_add(&static_order_id, 1),
                                         .perms             = perms,
                                         .definitely_unique = false);
            }
            assert(static_order_id != 1);
        }

        start += command->cmdsize;
        command = (void *)start;
    }

    (void)n00b_static_objects_register_macho_image(hdr, slide);
}

void
n00b_load_static_ranges(void)
{
    _dyld_register_func_for_add_image(n00b_on_lib_load);
}
#elifdef _WIN32

void
n00b_load_static_ranges(void)
{
    (void)n00b_static_objects_register_all();
}

#else
#error "Unsupported OS"
#endif

static n00b_option_t(n00b_mmap_info_t *)
n00b_check_kernel_page_map(const void *addr)
{
    char              *start = n00b_align_to_page_start((void *)addr);
    n00b_mmap_perms_t perms  = n00b_mmap_perms_unknown;

    if (!start) {
        return n00b_option_none(n00b_mmap_info_t *);
    }

#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(start, &mbi, sizeof(mbi))) {
        return n00b_option_none(n00b_mmap_info_t *);
    }
    if (mbi.State != MEM_COMMIT) {
        return n00b_option_none(n00b_mmap_info_t *);
    }
    perms = n00b_mmap_perms_from_win_protect(mbi.Protect);
#else
    char status[1];
    // This cast is crucial due to different signatures across
    // mac + linux (signed vs. unsigned.
    if (mincore(start, n00b_page_size, (void *)status)) {
        return n00b_option_none(n00b_mmap_info_t *);
    }
    if (!(status[0] & MINCORE_TEST_BIT)) {
        return n00b_option_none(n00b_mmap_info_t *);
    }
#endif

    /* Cache just this one page -- through the BOUNDED probe cache, not the
     * general registry. Nothing in n00b owns this mapping, so there is no
     * unmap path that would ever unregister the record; before n00b#213 that
     * made this the one registration site in the process with no counterpart,
     * and a long-running consumer accumulated a permanent single-page record
     * for every distinct foreign page it ever asked about. Every registry
     * search in the process then pays for them, including the conservative
     * scan's (n00b#275). */
    return n00b_mmap_register_probe_page(start,
                                         start + n00b_page_size,
                                         start ? n00b_mmap_unmanaged
                                               : n00b_mmap_zero_page,
                                         start ? perms
                                               : n00b_mmap_perms_no_access);
}

// This only gets called when lookup fails.
static inline n00b_option_t(n00b_mmap_info_t *)
n00b_check_for_unmanaged_map(const void *addr)
{
    n00b_option_t(n00b_mmap_info_t *) result;
    // On MacOS, we register for dynamic events, so don't need to make a
    // dynamic call; it would have been in the static list (minux some
    // race condition, where we'll just accept returning that an address
    // is data).
    //
    // On Linux, I recommend not turning on this re-checking; we
    // expose the ability to manually re-run (call
    // n00b_reload_static_ranges() yourself).

#if defined(__linux__) && defined(N00B_ALWAYS_RECHECK_STATIC_POINTERS)
    result = n00b_check_static_maps(addr);
    if (n00b_option_is_set(result)) {
        return result;
    }
#endif

    result = n00b_check_kernel_page_map(addr);

    return result;
}

n00b_option_t(n00b_mmap_info_t *)
n00b_mmap_info_lookup(const void *addr)
{
    auto result = n00b_mmap_by_address((void *)addr);

    if (n00b_option_is_set(result)) {
        if (n00b_option_get(result)->kind == n00b_mmap_zero_page) {
            return n00b_option_none(n00b_mmap_info_t *);
        }
        return result;
    }

    return n00b_check_for_unmanaged_map(addr);
}

// clang-format off
n00b_option_t(n00b_allocator_t *)
n00b_find_allocator(void *val)
{
    n00b_mmap_view_t view;
    if (n00b_mmap_view_by_address(val, &view)) {
        if (view.kind == n00b_mmap_zero_page) {
            return n00b_option_none(n00b_allocator_t *);
        }
        return n00b_option_from_nullable(n00b_allocator_t *, view.allocator);
    }
    return n00b_option_none(n00b_allocator_t *);
}
// clang-format on

/* Always-on probe accounting (n00b#275, n00b#395).
 *
 * The ratio of these two IS the perms-unknown rate: a registry hit with KNOWN
 * perms answers with no syscall, everything else falls through to the pipe
 * probe below and enters the kernel three times.  PR #384 recorded pool pages
 * as read/write to push traffic from the second counter to the first; it
 * shipped in 0.8.55 and the gateway still wedged, so the remaining traffic has
 * to be attributable rather than inferred.  Two relaxed atomics on a path that
 * already issues syscalls is not a measurable cost. */
_Atomic uint64_t n00b_memperm_fastpath_hits   = 0;
_Atomic uint64_t n00b_memperm_syscall_probes  = 0;

/* n00b#399: probes that failed for a reason unrelated to the address --
 * EINTR, a pipe that could not be created, a poll that never became ready.
 * These used to be reported as no_access, which truncates the conservative
 * backward guard scan. They no longer do; this counts how often the probe
 * cannot answer, so the case is visible instead of silent. */
_Atomic uint64_t n00b_memperm_indeterminate = 0;

n00b_mmap_perms_t
n00b_check_memory_perms(void *ptr)
{
    n00b_mmap_view_t view;
    if (n00b_mmap_view_by_address(ptr, &view)) {
        n00b_mmap_view_t *map = &view;
        if (map->kind == n00b_mmap_zero_page) {
            atomic_fetch_add_explicit(&n00b_memperm_fastpath_hits,
                                      1,
                                      memory_order_relaxed);
            return n00b_mmap_perms_no_access;
        }
        if (n00b_mmap_perms_known(map->perms)) {
            atomic_fetch_add_explicit(&n00b_memperm_fastpath_hits,
                                      1,
                                      memory_order_relaxed);
            return map->perms;
        }
    }

    atomic_fetch_add_explicit(&n00b_memperm_syscall_probes,
                              1,
                              memory_order_relaxed);

#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) {
        return n00b_mmap_perms_no_access;
    }
    if (mbi.State != MEM_COMMIT) {
        return n00b_mmap_perms_no_access;
    }
    return n00b_mmap_perms_from_win_protect(mbi.Protect);
#else
    bool cannot_write = false;
    bool cannot_read  = false;

    n00b_memperm_pipe_t *pipe_state = n00b_memperm_pipe_get();
    int                  local_pipe_fds[2];
    int                 *pipe_fds  = local_pipe_fds;
    bool                 use_cache = false;

#if !defined(__linux__)
    /* The probe writes to a pipe whose read end may be closed, so SIGPIPE
     * must be ignored; once per process is enough. This used to run on
     * every probe, and sigaction is process-global: n00b#275 measured it as
     * 39% of _n00b_find_alloc_info on a wedged gateway. */
    static _Atomic bool sigpipe_ignored = false;
    if (!atomic_load_explicit(&sigpipe_ignored, memory_order_relaxed)) {
        signal(SIGPIPE, SIG_IGN);
        atomic_store_explicit(&sigpipe_ignored, true, memory_order_relaxed);
    }
#endif

    if (pipe_state) {
        if (!pipe_state->ready) {
            if (N00B_MEMPERM_PIPE(pipe_state->fds)) {
                /* No pipe, no probe: EMFILE/ENFILE on a process with many
                 * open shards is a fact about the process, not about ptr
                 * (n00b#399). */
                atomic_fetch_add_explicit(&n00b_memperm_indeterminate,
                                          1,
                                          memory_order_relaxed);
                return n00b_mmap_perms_unknown;
            }
            int flags = N00B_MEMPERM_FCNTL(pipe_state->fds[0], F_GETFL, 0);
            if (flags >= 0) {
                N00B_MEMPERM_FCNTL(pipe_state->fds[0], F_SETFL, flags | O_NONBLOCK);
            }
            pipe_state->ready = 1;
        }
        pipe_fds  = pipe_state->fds;
        use_cache = true;
    }
    else {
        if (N00B_MEMPERM_PIPE(local_pipe_fds)) {
            atomic_fetch_add_explicit(&n00b_memperm_indeterminate,
                                      1,
                                      memory_order_relaxed);
            return n00b_mmap_perms_unknown;
        }
        int flags = N00B_MEMPERM_FCNTL(local_pipe_fds[0], F_GETFL, 0);
        if (flags >= 0) {
            N00B_MEMPERM_FCNTL(local_pipe_fds[0], F_SETFL, flags | O_NONBLOCK);
        }
    }

    /* Only EFAULT means "this address is bad" (n00b#399).  EINTR -- this
     * runs inside a stop-the-world collect, where signals are the mechanism
     * -- a short write, or any other failure means the PROBE failed, which
     * is not an address verdict.  Reporting it as one made _find_sentinal
     * stop the backward guard scan early, which is how a live object's
     * header gets missed (n00b#321).  EINTR is simply retried; anything
     * else that is not EFAULT ends the probe with "could not determine",
     * which callers see as perms_unknown: not readable, not a pointer, and
     * NOT a reason to stop a scan.
     *
     * The write copies one byte OUT of ptr (readable?), the read copies one
     * byte back INTO ptr (writable?).  A byte that made it into the pipe is
     * always drained, so a cached pipe never carries a stale byte into the
     * next probe. */
    bool indeterminate = false;
    ssize_t wrc = -1;
    for (int attempt = 0; attempt < 4; attempt++) {
        wrc = N00B_MEMPERM_WRITE(pipe_fds[1], ptr, 1);
        if (wrc > 0 || N00B_MEMPERM_ERRNO(wrc) != EINTR) {
            break;
        }
    }
    if (wrc <= 0) {
        if (N00B_MEMPERM_ERRNO(wrc) == EFAULT) {
            cannot_write = true;
        }
        else {
            indeterminate = true;
        }
    }

    /* Nothing reached the pipe, so there is nothing to read back and no
     * writability verdict to be had: the address is either bad (EFAULT
     * above) or the probe failed. */
    if (wrc <= 0) {
        cannot_read = true;
    }
    else {
        struct pollfd pollset = {
            .fd     = pipe_fds[0],
            .events = POLL_IN,
        };

        int prc = -1;
        for (int attempt = 0; attempt < 4; attempt++) {
            prc = N00B_MEMPERM_POLL(&pollset, 1, 0);
            if (prc >= 0 || N00B_MEMPERM_ERRNO(prc) != EINTR) {
                break;
            }
        }
        if (prc <= 0 || !(pollset.revents & POLL_IN)) {
            /* We just wrote a byte and it is not readable back: that is
             * the pipe misbehaving, not the address. */
            indeterminate = true;
            char drain;
            (void)N00B_MEMPERM_READ(pipe_fds[0], &drain, 1);
        }
        else {
            ssize_t rrc = -1;
            for (int attempt = 0; attempt < 4; attempt++) {
                rrc = N00B_MEMPERM_READ(pipe_fds[0], ptr, 1);
                if (rrc > 0 || N00B_MEMPERM_ERRNO(rrc) != EINTR) {
                    break;
                }
            }
            if (rrc <= 0) {
                if (N00B_MEMPERM_ERRNO(rrc) == EFAULT) {
                    cannot_read = true;
                }
                else {
                    indeterminate = true;
                }
                char drain;
                (void)N00B_MEMPERM_READ(pipe_fds[0], &drain, 1);
            }
        }
    }

    if (!use_cache) {
        N00B_MEMPERM_CLOSE(pipe_fds[0]);
        N00B_MEMPERM_CLOSE(pipe_fds[1]);
    }

    if (indeterminate) {
        atomic_fetch_add_explicit(&n00b_memperm_indeterminate,
                                  1,
                                  memory_order_relaxed);
        return n00b_mmap_perms_unknown;
    }

    if (cannot_write) {
        if (cannot_read) {
            return n00b_mmap_perms_no_access;
        }
        else {
            return n00b_mmap_perms_ro;
        }
    }

    return n00b_mmap_perms_rw;
#endif
}

static inline bool
codepoint_is_printable(int32_t cp)
{
    switch (n00b_unicode_general_category((n00b_codepoint_t)cp)) {
    case N00B_UNICODE_GC_CC: // Control
    case N00B_UNICODE_GC_CF: // Format
    case N00B_UNICODE_GC_CS: // Surrogate
        return false;
    default:
        // Reject whitespace other than plain space.
        if (cp != ' ') {
            n00b_unicode_gc_t gc = n00b_unicode_general_category((n00b_codepoint_t)cp);
            if (gc == N00B_UNICODE_GC_ZS || gc == N00B_UNICODE_GC_ZL
                || gc == N00B_UNICODE_GC_ZP) {
                return false;
            }
        }
        return true;
    }
}

size_t
n00b_address_is_probable_cstring(void *addr, size_t *bytelen, size_t min_len)
{
    // Looks for valid UTF-8 with null termination and printable content.
    // Returns the codepoint count (0 if not a plausible string).

    char *base      = (char *)addr;
    char *page_end;
    char *p         = base;

    if (n00b_value_is_data(p)) {
        return 0;
    }

    page_end = (char *)n00b_align_to_page_start(p) + n00b_page_size;

    // We decode one page at a time; within each page we use the new
    // n00b_unicode_utf8_decode() which takes (base, len, &pos).
    uint32_t total_bytes = 0;
    uint32_t total_cp    = 0;

    while (true) {
        uint32_t chunk_len = (uint32_t)(page_end - p);
        uint32_t pos       = 0;

        while (pos < chunk_len) {
            // Check for NUL terminator before decoding.
            if (p[pos] == '\0') {
                uint32_t blen = total_bytes + pos;
                if (total_cp < min_len) {
                    return 0;
                }
                *bytelen = blen;
                return total_cp;
            }

            int32_t cp = n00b_unicode_utf8_decode(p, chunk_len, &pos);
            if (cp < 0) {
                return 0;
            }
            if (!codepoint_is_printable(cp)) {
                return 0;
            }
            total_cp++;
        }

        total_bytes += chunk_len;
        p = page_end;
        page_end += n00b_page_size;

        if (n00b_value_is_data(p)) {
            return 0;
        }
    }
}

bool
n00b_memory_scan_init(n00b_memory_scan_t *ctx, void *s, size_t len) _kargs
{
    uint8_t cat_flags = 0;
}
{
    uint64_t start = (uint64_t)s;
    uint64_t end   = start + len;

    ctx->cur = (uint64_t *)n00b_align_ceil(start, sizeof(void *));
    ctx->end = (uint64_t *)n00b_align_floor(end, sizeof(void *));

    if (n00b_value_is_data(ctx->cur) || n00b_value_is_data(ctx->end)) {
        return false;
    }

    if (!cat_flags) {
        ctx->flags = n00b_mmap_type_mask;
    }
    else {
        ctx->flags = cat_flags;
    }

    return true;
}

n00b_option_t(void *)
n00b_memory_scan_next(n00b_memory_scan_t   *ctx,
                      n00b_mmap_rec_kind_t *tinfo,
                      n00b_mmap_perms_t    *perms)
{
    void                *result = nullptr;
    void                *target = nullptr;
    n00b_mmap_rec_kind_t kind   = 0;

    while (!result && (ctx->cur < ctx->end)) {
        void            *val   = (void *)*ctx->cur;
        bool             found = false;
        n00b_mmap_view_t view;

        if (n00b_mmap_view_by_address(val, &view)) {
            found = view.kind != n00b_mmap_zero_page;
            kind  = view.kind;
        }
        else {
            // Kernel-probed records are recycled in place and never freed.
            auto mmap_opt = n00b_mmap_info_lookup(val);
            if (n00b_option_is_set(mmap_opt)) {
                found = true;
                kind  = n00b_option_get(mmap_opt)->kind;
            }
        }

        if (found && (kind & ctx->flags)) {
            result   = (void *)ctx->cur;
            target   = val;
            ctx->cur = ctx->cur + 1;
            break;
        }
        ctx->cur = ctx->cur + 1;
    }

    if (!result) {
        return n00b_option_none(void *);
    }

    if (perms) {
        *perms = n00b_check_memory_perms(target);
    }

    if (tinfo) {
        *tinfo = kind;
    }

    return n00b_option_set(void *, result);
}

static void
show_mem_info(n00b_mmap_info_t *n, bool all, unsigned int *lenp)
{
    char *kind_str = nullptr;
    bool  aseg     = false;

    if (!n) {
        return;
    }

    switch (n->kind) {
    case n00b_mmap_static:
        if (!all) {
            return;
        }
        else {
            kind_str = "Static memory ";
        }
        break;
    case n00b_mmap_arena:
        kind_str = "Arena header   "; // Currently not being registered.
        *lenp += (n->end - n->start);
        break;
    case n00b_mmap_managed_segment:
        kind_str = "Arena segment  ";
        *lenp += (n->end - n->start);
        aseg = true;
        break;
    case n00b_mmap_sys_segment:
        kind_str = "Sys segment    ";
        *lenp += (n->end - n->start);
        aseg = true;
        break;
    case n00b_mmap_stack:
        kind_str = "Thread stack   ";
        break;
    case n00b_mmap_unmanaged:
        *lenp += (n->end - n->start);
        kind_str = "Unmanaged heap ";
        break;
    case n00b_mmap_internal:
        *lenp += (n->end - n->start);
        kind_str = "N00b internal  ";
        break;
    case n00b_mmap_zero_page:
        kind_str = "Inaccessible   ";
        break;
    case n00b_mmap_pool:
        kind_str = "Pool alloc     ";
        *lenp += (n->end - n->start);
        aseg = true;
        break;
    case n00b_mmap_api_mmap:
        kind_str = "n00b_mmap()    ";
        *lenp += (n->end - n->start);
        break;
    default:
        abort();
    }

    n00b_allocator_t *a    = n->allocator;
    const char       *file = "(no file)";
    if (n->file) {
        file = n->file;
    }

    if (aseg) {
        if (!a->metadata) {
            n00b_fprintf(stderr,
                         "%s %s %p-%p (%'llu bytes, %'zu pages) arena @ %p (%s)\n",
                         kind_str,
                         file,
                         (void *)n->start,
                         (void *)n->end,
                         (unsigned long long)(n->end - n->start),
                         (size_t)(n->end - n->start) / n00b_page_size,
                         a,
                         n->file ? n->file : "*no name*");
        }
        else {
            n00b_allocator_t *link = a->metadata_pool;

            n00b_fprintf(stderr,
                         "%s %s %p-%p (%'llu bytes, %'zu pages) arena @ "
                         "%p (alloc mdata for arena '%s' @%p)\n",
                         kind_str,
                         file,
                         (void *)n->start,
                         (void *)n->end,
                         (unsigned long long)(n->end - n->start),
                         (unsigned int)(n->end - n->start) / n00b_page_size,
                         a,
                         link->debug_name ? link->debug_name : "*no name*",
                         link);
        }
    }
    else {
        n00b_fprintf(stderr,
                     "%s %s %p-%p (%'llu bytes, %'zu pages)\n",
                     kind_str,
                     file,
                     (void *)n->start,
                     (void *)n->end,
                     (unsigned long long)(n->end - n->start),
                     (unsigned int)(n->end - n->start) / n00b_page_size);
    }
}

void
n00b_debug_memory_info(bool all)
{
    unsigned int     len = 0;
    n00b_mmap_ctx_t *ctx = n00b_global_mem_map(n00b_get_runtime());

    n00b_allocator_t *alloc = (n00b_allocator_t *)&ctx->pool;
    n00b_stack_t(void *) results = n00b_stack_new(void *, .allocator = alloc);

    (void)n00b_interval_search_ordered(ctx->mmap_tree, 0, UINT64_MAX, &results);

    for (size_t i = 0; i < n00b_stack_len(results); i++) {
        auto node = results.data[i];
        n00b_mmap_data_t data = ((n00b_interval_node_t(n00b_mmap_data_t) *)node)->data;
        assert(n00b_variant_is_type(data, n00b_mmap_info_t *));
        show_mem_info(n00b_variant_get(data, n00b_mmap_info_t *), all, &len);
    }

    n00b_fprintf(stderr,
                 "\nTotal heap usage: %'u bytes (%'zu pages)\n",
                 len,
                 len / n00b_page_size);
}
