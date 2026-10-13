#include "vfs/backend_local.h"
#include "core/alloc.h"
#include "core/file.h"
#include "util/path.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#ifdef _WIN32
#include "internal/win32_sockets.h"
#ifndef ENOTSUP
#define ENOTSUP 129
#endif
#ifndef S_ISLNK
#define S_ISLNK(mode) 0
#endif
#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#endif
// _O_BINARY is load-bearing. The MSVC CRT opens in TEXT mode when neither
// _O_TEXT nor _O_BINARY is given: _read then collapses every \r\n to \n and
// treats a 0x1A byte as end of file, so a binary object reads back SHORT with
// no error. The ROCS catalog is binary (u64 seal timestamps), so a store that
// closed cleanly failed to reopen whenever a timestamp byte happened to be
// 0x1A -- nondeterministic, Windows-only, and invisible to the store (it saw
// CORRUPT). Every other _open in this tree already passes _O_BINARY.
#define N00B_LOCAL_OPEN(path, flags) _open((path), (flags) | _O_BINARY)
#define N00B_LOCAL_READ(fd, buf, len) _read((fd), (buf), (unsigned int)(len))
#define N00B_LOCAL_CLOSE(fd) _close(fd)
#define N00B_LOCAL_LSEEK(fd, offset, whence) \
    _lseeki64((fd), (__int64)(offset), (whence))
#define N00B_LOCAL_UNLINK(path) _unlink(path)
#define N00B_LOCAL_MKDIR(path, mode) _mkdir(path)

typedef struct {
    char d_name[MAX_PATH + 1];
} n00b_local_dirent_t;

typedef struct {
    intptr_t            handle;
    struct _finddata_t  data;
    bool                first;
    n00b_local_dirent_t ent;
} n00b_local_dir_t;

static n00b_local_dir_t *
n00b_local_opendir(n00b_allocator_t *allocator, const char *path)
{
    size_t len     = strlen(path);
    bool   has_sep = len > 0 && (path[len - 1] == '/' || path[len - 1] == '\\');
    char  *pattern = n00b_alloc_array_with_opts(char,
                                                len + (has_sep ? 2 : 3),
                                                &(n00b_alloc_opts_t){.allocator = allocator});
    memcpy(pattern, path, len);
    size_t ix = len;
    if (!has_sep) {
        pattern[ix++] = '/';
    }
    pattern[ix++] = '*';
    pattern[ix]   = '\0';

    n00b_local_dir_t *dir =
        n00b_alloc_with_opts(n00b_local_dir_t, &(n00b_alloc_opts_t){.allocator = allocator});
    dir->handle = _findfirst(pattern, &dir->data);
    if (dir->handle == -1) {
        return nullptr;
    }
    dir->first = true;
    return dir;
}

static n00b_local_dirent_t *
n00b_local_readdir(n00b_local_dir_t *dir)
{
    if (!dir->first && _findnext(dir->handle, &dir->data) != 0) {
        return nullptr;
    }
    dir->first = false;

    strncpy(dir->ent.d_name, dir->data.name, MAX_PATH);
    dir->ent.d_name[MAX_PATH] = '\0';
    return &dir->ent;
}

static void
n00b_local_closedir(n00b_local_dir_t *dir)
{
    if (dir != nullptr && dir->handle != -1) {
        _findclose(dir->handle);
    }
}
static int
n00b_local_link(const char *target, const char *link_path)
{
    if (CreateHardLinkA(link_path, target, nullptr)) {
        return 0;
    }

    errno = EIO;
    return -1;
}
#define N00B_LOCAL_LINK(target, link_path) n00b_local_link(target, link_path)
#else
#define N00B_LOCAL_OPEN(path, flags) open((path), (flags))
#define N00B_LOCAL_READ(fd, buf, len) read((fd), (buf), (len))
#define N00B_LOCAL_CLOSE(fd) close(fd)
#define N00B_LOCAL_LSEEK(fd, offset, whence) lseek((fd), (off_t)(offset), (whence))
#define N00B_LOCAL_UNLINK(path) unlink(path)
#define N00B_LOCAL_MKDIR(path, mode) mkdir(path, mode)
#define N00B_LOCAL_LINK(target, link_path) link(target, link_path)
#endif

// ============================================================================
// Internal context
// ============================================================================

typedef struct {
    n00b_string_t *root;
    n00b_allocator_t *allocator;
} local_ctx_t;

// ============================================================================
// Path helpers
// ============================================================================

/*
 * Join root directory and relative path with a '/'.
 * Returns a heap-allocated NUL-terminated C string.
 * Handles empty rel as "the root directory itself".
 *
 * If lc->root already ends with '/' (e.g. root="/" - used by the
 * runtime default VFS mount), the joining slash is suppressed to
 * avoid emitting "//rel".
 */
static char *
join_path(local_ctx_t *lc, n00b_string_t *rel)
{
    size_t rlen = lc->root->u8_bytes;
    size_t plen = rel->u8_bytes;

    // Empty relative path -> return root directory as-is.
    if (plen == 0) {
        char *buf = n00b_alloc_array_with_opts(
            char,
            rlen + 1,
            &(n00b_alloc_opts_t){.allocator = lc->allocator});
        memcpy(buf, lc->root->data, rlen);
        buf[rlen] = '\0';
        return buf;
    }

    bool   root_ends_slash = rlen > 0 && lc->root->data[rlen - 1] == '/';
    size_t sep_len         = root_ends_slash ? 0 : 1;

    // root + (maybe '/') + path + '\0'
    size_t total = rlen + sep_len + plen + 1;
    char  *buf   = n00b_alloc_array_with_opts(char,
                                              total,
                                              &(n00b_alloc_opts_t){.allocator = lc->allocator});

    memcpy(buf, lc->root->data, rlen);
    if (sep_len) {
        buf[rlen] = '/';
    }
    memcpy(buf + rlen + sep_len, rel->data, plen);
    buf[rlen + sep_len + plen] = '\0';

    return buf;
}

static n00b_err_t
errno_to_vfs_err(int e)
{
    switch (e) {
    case ENOENT:  return N00B_VFS_ERR_NOT_FOUND;
    case EEXIST:  return N00B_VFS_ERR_EXISTS;
    case EISDIR:  return N00B_VFS_ERR_IS_DIR;
    case ENOTDIR: return N00B_VFS_ERR_NOT_DIR;
    case EACCES:
    case EPERM:   return N00B_VFS_ERR_PERMISSION;
    case ENOSPC:  return N00B_VFS_ERR_NO_SPACE;
    case EXDEV:   return N00B_VFS_ERR_CROSS_DEVICE;
#ifdef ENOTSUP
    case ENOTSUP: return N00B_VFS_ERR_NOT_SUPPORTED;
#endif
#ifdef ENOSYS
    case ENOSYS:  return N00B_VFS_ERR_NOT_SUPPORTED;
#endif
    default:      return N00B_VFS_ERR_IO;
    }
}

static n00b_err_t
file_err_to_vfs_err(n00b_err_t e)
{
    switch (e) {
    case N00B_FILE_OK:                return N00B_VFS_ERR_NONE;
    case N00B_FILE_ERR_ARG:           return N00B_VFS_ERR_NULL_ARG;
    case N00B_FILE_ERR_NOT_FOUND:     return N00B_VFS_ERR_NOT_FOUND;
    case N00B_FILE_ERR_EXISTS:        return N00B_VFS_ERR_EXISTS;
    case N00B_FILE_ERR_IS_DIR:        return N00B_VFS_ERR_IS_DIR;
    case N00B_FILE_ERR_NOT_DIR:       return N00B_VFS_ERR_NOT_DIR;
    case N00B_FILE_ERR_PERMISSION:    return N00B_VFS_ERR_PERMISSION;
    case N00B_FILE_ERR_NO_SPACE:      return N00B_VFS_ERR_NO_SPACE;
    case N00B_FILE_ERR_NOT_SUPPORTED: return N00B_VFS_ERR_NOT_SUPPORTED;
    case N00B_FILE_ERR_IO:
    default:                          return N00B_VFS_ERR_IO;
    }
}

static uint64_t
stat_atime_ns(const struct stat *st)
{
#ifdef __APPLE__
    return (uint64_t)st->st_atimespec.tv_sec * 1000000000ULL
         + (uint64_t)st->st_atimespec.tv_nsec;
#elif defined(_WIN32)
    return (uint64_t)st->st_atime * 1000000000ULL;
#else
    return (uint64_t)st->st_atim.tv_sec * 1000000000ULL
         + (uint64_t)st->st_atim.tv_nsec;
#endif
}

static uint64_t
stat_mtime_ns(const struct stat *st)
{
#ifdef __APPLE__
    return (uint64_t)st->st_mtimespec.tv_sec * 1000000000ULL
         + (uint64_t)st->st_mtimespec.tv_nsec;
#elif defined(_WIN32)
    return (uint64_t)st->st_mtime * 1000000000ULL;
#else
    return (uint64_t)st->st_mtim.tv_sec * 1000000000ULL
         + (uint64_t)st->st_mtim.tv_nsec;
#endif
}

static uint64_t
stat_ctime_ns(const struct stat *st)
{
#ifdef __APPLE__
    return (uint64_t)st->st_ctimespec.tv_sec * 1000000000ULL
         + (uint64_t)st->st_ctimespec.tv_nsec;
#elif defined(_WIN32)
    return (uint64_t)st->st_ctime * 1000000000ULL;
#else
    return (uint64_t)st->st_ctim.tv_sec * 1000000000ULL
         + (uint64_t)st->st_ctim.tv_nsec;
#endif
}

// ============================================================================
// Vtable implementations
// ============================================================================

static n00b_string_t *
local_name(void)
{
    return r"local";
}

static void *
local_init(n00b_vfs_backend_t *be)
{
    local_ctx_t *lc = n00b_alloc_with_opts(local_ctx_t,
                                           &(n00b_alloc_opts_t){.allocator = be->allocator});
    lc->root      = be->root;
    lc->allocator = be->allocator;
    return lc;
}

static void
local_cleanup(void *ctx)
{
    (void)ctx;
}

static n00b_result_t(n00b_buffer_t *)
local_get(void *ctx, n00b_string_t *path)
{
    local_ctx_t *lc   = ctx;
    char        *full = join_path(lc, path);

    int fd = N00B_LOCAL_OPEN(full, O_RDONLY);
    if (fd < 0) {
        return n00b_result_err(n00b_buffer_t *, errno_to_vfs_err(errno));
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        int e = errno;
        N00B_LOCAL_CLOSE(fd);
        return n00b_result_err(n00b_buffer_t *, errno_to_vfs_err(e));
    }

    size_t         size = (size_t)st.st_size;
    n00b_buffer_t *buf  = n00b_buffer_new((int64_t)size,
                                          .allocator = lc->allocator);

    if (size > 0) {
        n00b_buffer_resize(buf, size);
        char *dst = n00b_buffer_to_c(buf, nullptr);

        size_t  total = 0;
        while (total < size) {
            ssize_t r = N00B_LOCAL_READ(fd, dst + total, size - total);
            if (r <= 0) {
                break;
            }
            total += (size_t)r;
        }
    }

    N00B_LOCAL_CLOSE(fd);
    return n00b_result_ok(n00b_buffer_t *, buf);
}

static n00b_result_t(n00b_buffer_t *)
local_get_range(void *ctx, n00b_string_t *path, uint64_t offset,
                uint64_t length)
{
    local_ctx_t *lc   = ctx;
    char        *full = join_path(lc, path);

    int fd = N00B_LOCAL_OPEN(full, O_RDONLY);
    if (fd < 0) {
        return n00b_result_err(n00b_buffer_t *, errno_to_vfs_err(errno));
    }

    if (N00B_LOCAL_LSEEK(fd, offset, SEEK_SET) < 0) {
        int e = errno;
        N00B_LOCAL_CLOSE(fd);
        return n00b_result_err(n00b_buffer_t *, errno_to_vfs_err(e));
    }

    n00b_buffer_t *buf = n00b_buffer_new((int64_t)length,
                                         .allocator = lc->allocator);
    n00b_buffer_resize(buf, length);
    char *dst = n00b_buffer_to_c(buf, nullptr);

    size_t total = 0;
    while (total < length) {
        ssize_t r = N00B_LOCAL_READ(fd, dst + total, length - total);
        if (r <= 0) {
            break;
        }
        total += (size_t)r;
    }

    // Shrink if we read less than requested.
    if (total < length) {
        n00b_buffer_resize(buf, total);
    }

    N00B_LOCAL_CLOSE(fd);
    return n00b_result_ok(n00b_buffer_t *, buf);
}

static n00b_result_t(bool)
local_put_impl(void *ctx, n00b_string_t *path, n00b_buffer_t *data,
               bool exclusive)
{
    local_ctx_t *lc   = ctx;
    char        *full = join_path(lc, path);
    n00b_string_t *destination =
        n00b_string_from_cstr(full, .allocator = lc->allocator);

    auto temp_r = n00b_new_sibling_temp_file(destination,
                                             .file_mode = 0644,
                                             .allocator = lc->allocator);
    if (n00b_result_is_err(temp_r)) {
        return n00b_result_err(bool,
                               errno_to_vfs_err(n00b_result_get_err(temp_r)));
    }

    n00b_sibling_temp_file_t *temp = n00b_result_get(temp_r);
    auto write_r = n00b_file_write_all(temp->file, data);
    if (n00b_result_is_err(write_r)) {
        n00b_err_t err = n00b_result_get_err(write_r);
        (void)n00b_file_close_result(temp->file);
        (void)n00b_file_unlink(temp->path, .ignore_missing = true);
        return n00b_result_err(bool, errno_to_vfs_err(err));
    }
    if (n00b_result_get(write_r) != n00b_buffer_len(data)) {
        (void)n00b_file_close_result(temp->file);
        (void)n00b_file_unlink(temp->path, .ignore_missing = true);
        return n00b_result_err(bool, N00B_VFS_ERR_IO);
    }

    auto close_r = n00b_file_close_result(temp->file);
    if (n00b_result_is_err(close_r)) {
        n00b_err_t err = n00b_result_get_err(close_r);
        (void)n00b_file_unlink(temp->path, .ignore_missing = true);
        return n00b_result_err(bool, errno_to_vfs_err(err));
    }

    auto commit_r = n00b_path_commit_exact(
        temp->path,
        destination,
        .policy = exclusive ? N00B_PATH_COMMIT_REJECT_EXISTING
                            : N00B_PATH_COMMIT_REPLACE_EXISTING);
    if (n00b_result_is_err(commit_r)) {
        n00b_err_t err = n00b_result_get_err(commit_r);
        (void)n00b_file_unlink(temp->path, .ignore_missing = true);
        return n00b_result_err(bool, errno_to_vfs_err(err));
    }

    return n00b_result_ok(bool, true);
}

static n00b_result_t(bool)
local_put(void *ctx, n00b_string_t *path, n00b_buffer_t *data)
{
    return local_put_impl(ctx, path, data, false);
}

static n00b_result_t(bool)
local_put_if_absent(void *ctx, n00b_string_t *path, n00b_buffer_t *data)
{
    return local_put_impl(ctx, path, data, true);
}

static n00b_result_t(bool)
local_append(void *ctx, n00b_string_t *path, n00b_buffer_t *data)
{
    local_ctx_t   *lc   = ctx;
    char          *full = join_path(lc, path);
    n00b_string_t *dest = n00b_string_from_cstr(full, .allocator = lc->allocator);

    auto open_r = n00b_file_open(dest,
                                 .mode = N00B_FILE_WRITE | N00B_FILE_CREATE
                                       | N00B_FILE_APPEND,
                                 .kind = N00B_FILE_KIND_STREAM);
    if (n00b_result_is_err(open_r)) {
        return n00b_result_err(bool,
                               errno_to_vfs_err(n00b_result_get_err(open_r)));
    }

    n00b_file_t *file    = n00b_result_get(open_r);
    auto         write_r = n00b_file_write_all(file, data);
    if (n00b_result_is_err(write_r)) {
        n00b_err_t err = n00b_result_get_err(write_r);
        (void)n00b_file_close_result(file);
        return n00b_result_err(bool, errno_to_vfs_err(err));
    }
    if (n00b_result_get(write_r) != n00b_buffer_len(data)) {
        (void)n00b_file_close_result(file);
        return n00b_result_err(bool, N00B_VFS_ERR_IO);
    }

    auto close_r = n00b_file_close_result(file);
    if (n00b_result_is_err(close_r)) {
        return n00b_result_err(bool,
                               errno_to_vfs_err(n00b_result_get_err(close_r)));
    }
    return n00b_result_ok(bool, true);
}

static n00b_result_t(bool)
local_del(void *ctx, n00b_string_t *path)
{
    local_ctx_t *lc   = ctx;
    char        *full = join_path(lc, path);

    if (N00B_LOCAL_UNLINK(full) < 0) {
        return n00b_result_err(bool, errno_to_vfs_err(errno));
    }

    return n00b_result_ok(bool, true);
}

static n00b_result_t(n00b_vfs_obj_stat_t)
local_stat(void *ctx, n00b_string_t *path)
{
    local_ctx_t *lc   = ctx;
    char        *full = join_path(lc, path);

    struct stat st;
    if (stat(full, &st) < 0) {
        return n00b_result_err(n00b_vfs_obj_stat_t, errno_to_vfs_err(errno));
    }

    n00b_vfs_obj_kind_t kind;
    if (S_ISDIR(st.st_mode)) {
        kind = N00B_VFS_OBJ_DIR;
    }
    else if (S_ISLNK(st.st_mode)) {
        kind = N00B_VFS_OBJ_SYMLINK;
    }
    else {
        kind = N00B_VFS_OBJ_FILE;
    }

    uint64_t atime_ns = stat_atime_ns(&st);
    uint64_t mtime_ns = stat_mtime_ns(&st);
    uint64_t ctime_ns = stat_ctime_ns(&st);

    return n00b_result_ok(n00b_vfs_obj_stat_t, ((n00b_vfs_obj_stat_t){
        .kind     = kind,
        .size     = (uint64_t)st.st_size,
        .atime_ns = atime_ns,
        .mtime_ns = mtime_ns,
        .ctime_ns = ctime_ns,
        .mode     = st.st_mode & 07777,
    }));
}

static n00b_result_t(n00b_vfs_list_result_t *)
local_list(void *ctx, n00b_string_t *prefix, n00b_string_t *continuation,
           uint32_t max_keys)
{
    (void)continuation;

    local_ctx_t *lc   = ctx;
    char        *full = join_path(lc, prefix);

    /* libc-free enumeration (worker-safe): the n00b_path module uses raw
     * getdirentries64/getdents64 + fstatat64, not opendir/readdir/stat which
     * allocate via libsystem_malloc and trap on an n00b worker thread. */
    n00b_string_t *fulls = n00b_string_from_cstr(full, .allocator = lc->allocator);
    bool           ok    = false;
    n00b_list_t(n00b_dirent_t *) ents = n00b_path_list_dir(fulls, &ok,
                                            .allocator = lc->allocator);
    if (!ok) {
        /* Release the (possibly partial) scratch listing + joined-path
         * temporaries so they don't linger in the backend allocator. */
        int64_t nerr = n00b_list_len(ents);
        for (int64_t i = 0; i < nerr; i++) {
            n00b_dirent_t *de = n00b_list_get(ents, i);
            n00b_free(de->name);
            n00b_free(de);
        }
        n00b_list_free(ents);
        n00b_free(fulls);
        n00b_free(full);
        return n00b_result_err(n00b_vfs_list_result_t *,
                               errno_to_vfs_err(errno));
    }

    int64_t  navail = n00b_list_len(ents);
    uint32_t want   = (max_keys > 0 && (int64_t)max_keys < navail)
                          ? max_keys
                          : (uint32_t)navail;
    bool     truncated = (max_keys > 0 && navail > (int64_t)max_keys);

    n00b_vfs_list_entry_t *entries = nullptr;
    if (want > 0) {
        entries = n00b_alloc_array_with_opts(n00b_vfs_list_entry_t,
                                             want,
                                             &(n00b_alloc_opts_t){.allocator = lc->allocator});
    }

    uint32_t ix = 0;
    for (int64_t i = 0; i < navail && ix < want; i++) {
        n00b_dirent_t *de    = n00b_list_get(ents, i);
        entries[ix].name     = de->name;
        entries[ix].kind     = de->is_dir ? N00B_VFS_OBJ_DIR
                                          : N00B_VFS_OBJ_FILE;
        entries[ix].size     = de->size;
        entries[ix].mtime_ns = de->mtime_ns;
        ix++;
    }

    n00b_vfs_list_result_t *res =
        n00b_alloc_with_opts(n00b_vfs_list_result_t,
                             &(n00b_alloc_opts_t){.allocator = lc->allocator});
    res->entries      = (ix > 0) ? entries : nullptr;
    res->count        = ix;
    res->continuation = nullptr;
    res->truncated    = truncated;

    /* The result borrows each transcribed dirent's name string
     * (entries[k].name = de->name above); the dirent structs, the listing
     * container, and the joined-path scratch are transient and never
     * referenced again. Free them so they don't linger in the (possibly
     * non-GC) backend allocator. n00b_free resolves each pointer's owning
     * allocator and no-ops on GC-managed storage, so this is safe for every
     * caller. Names for dirents past the truncation point were not
     * transferred to the result, so release those too. */
    for (int64_t i = 0; i < navail; i++) {
        n00b_dirent_t *de = n00b_list_get(ents, i);
        if (i >= (int64_t)ix) {
            n00b_free(de->name);
        }
        n00b_free(de);
    }
    n00b_list_free(ents);
    n00b_free(fulls);
    n00b_free(full);

    return n00b_result_ok(n00b_vfs_list_result_t *, res);
}

static n00b_result_t(bool)
local_rename(void *ctx, n00b_string_t *old_path, n00b_string_t *new_path)
{
    local_ctx_t *lc       = ctx;
    char        *old_full = join_path(lc, old_path);
    char        *new_full = join_path(lc, new_path);

    if (rename(old_full, new_full) < 0) {
        return n00b_result_err(bool, errno_to_vfs_err(errno));
    }

    return n00b_result_ok(bool, true);
}

static n00b_result_t(bool)
local_mkdir(void *ctx, n00b_string_t *path)
{
    local_ctx_t *lc   = ctx;
    char        *full = join_path(lc, path);

    if (N00B_LOCAL_MKDIR(full, 0755) < 0) {
        return n00b_result_err(bool, errno_to_vfs_err(errno));
    }

    return n00b_result_ok(bool, true);
}

static n00b_result_t(bool)
local_sync(void *ctx, n00b_string_t *path)
{
    local_ctx_t *lc   = ctx;
    char        *full = join_path(lc, path);
    n00b_string_t *full_path =
        n00b_string_from_cstr(full, .allocator = lc->allocator);
    auto sync_r = n00b_file_sync_path(full_path);
    if (n00b_result_is_err(sync_r)) {
        return n00b_result_err(bool,
                               file_err_to_vfs_err(n00b_result_get_err(sync_r)));
    }

    return n00b_result_ok(bool, true);
}

static n00b_result_t(n00b_string_t *)
local_path(void *ctx, n00b_string_t *path)
{
    if (ctx == nullptr || path == nullptr) {
        return n00b_result_err(n00b_string_t *, N00B_VFS_ERR_NULL_ARG);
    }

    local_ctx_t *lc   = ctx;
    char        *full = join_path(lc, path);
    return n00b_result_ok(n00b_string_t *,
                          n00b_string_from_raw(full,
                                               (int64_t)strlen(full),
                                               .allocator = lc->allocator));
}

static bool
local_supports_range_read(void *ctx)
{
    (void)ctx;
    return true;
}

static bool
local_supports_rename(void *ctx)
{
    (void)ctx;
    return true;
}

static bool
local_supports_link(void *ctx)
{
    (void)ctx;
    return true;
}

static bool
local_supports_durable_sync(void *ctx)
{
    (void)ctx;
#ifdef _WIN32
    return false;
#else
    return true;
#endif
}

static n00b_result_t(bool)
local_link(void *ctx, n00b_string_t *target, n00b_string_t *link_path)
{
    local_ctx_t *lc        = ctx;
    char        *tgt_full  = join_path(lc, target);
    char        *link_full = join_path(lc, link_path);

    if (N00B_LOCAL_LINK(tgt_full, link_full) < 0) {
        return n00b_result_err(bool, errno_to_vfs_err(errno));
    }

    return n00b_result_ok(bool, true);
}

// ============================================================================
// Vtable
// ============================================================================

const n00b_vfs_backend_ops_t n00b_vfs_backend_local_ops = {
    .name                = local_name,
    .init                = local_init,
    .cleanup             = local_cleanup,
    .get                 = local_get,
    .get_range           = local_get_range,
    .put                 = local_put,
    .put_if_absent       = local_put_if_absent,
    .append              = local_append,
    .del                 = local_del,
    .stat                = local_stat,
    .list                = local_list,
    .rename              = local_rename,
    .mkdir               = local_mkdir,
    .sync                = local_sync,
    .local_path          = local_path,
    .supports_range_read = local_supports_range_read,
    .supports_rename     = local_supports_rename,
    .supports_link       = local_supports_link,
    .supports_durable_sync = local_supports_durable_sync,
    .link                = local_link,
    // local_list mints a fresh name string per dirent; the readdir clone may
    // free the originals.
    .list_result_owns_strings = true,
};

// ============================================================================
// Constructor
// ============================================================================

n00b_result_t(n00b_vfs_backend_t *)
n00b_vfs_backend_local_new(n00b_string_t *root_dir) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (root_dir == nullptr) {
        return n00b_result_err(n00b_vfs_backend_t *, N00B_VFS_ERR_NULL_ARG);
    }

    // Verify root exists and is a directory.
    struct stat st;
    // NUL-terminate for stat().
    size_t rlen = root_dir->u8_bytes;
    char  *cstr = n00b_alloc_array_with_opts(char,
                                             rlen + 1,
                                             &(n00b_alloc_opts_t){.allocator = allocator});
    memcpy(cstr, root_dir->data, rlen);
    cstr[rlen] = '\0';

    if (stat(cstr, &st) < 0 || !S_ISDIR(st.st_mode)) {
        return n00b_result_err(n00b_vfs_backend_t *, N00B_VFS_ERR_NOT_DIR);
    }

    n00b_vfs_backend_t *be =
        n00b_alloc_with_opts(n00b_vfs_backend_t, &(n00b_alloc_opts_t){.allocator = allocator});
    be->ops       = &n00b_vfs_backend_local_ops;
    be->root      = root_dir;
    be->allocator = allocator;

    n00b_result_t(bool) r = n00b_vfs_backend_init(be);
    if (n00b_result_is_err(r)) {
        return n00b_result_err(n00b_vfs_backend_t *, n00b_result_get_err(r));
    }

    return n00b_result_ok(n00b_vfs_backend_t *, be);
}
