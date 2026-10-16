#include "vfs/vfs.h"
#include "vfs/backend_local.h"
#include "core/alloc.h"
#include "core/runtime.h"

#include <stdatomic.h>
#include <string.h>

#define INITIAL_MOUNTS      4
#define INITIAL_HANDLES     16
#define MAX_VFS_FILE_SIZE   ((uint64_t)1 << 34)  // 16 GB limit

// ============================================================================
// Path resolution helpers
// ============================================================================

/*
 * Check if path starts with prefix. For mount matching,
 * the path must either equal the prefix or have a '/' after the
 * prefix (to avoid "/data" matching "/datafile").
 * Special case: prefix "/" matches everything.
 */
static bool
path_has_prefix(n00b_string_t *path, n00b_string_t *prefix)
{
    size_t plen = prefix->u8_bytes;
    size_t flen = path->u8_bytes;

    // Root mount matches everything.
    if (plen == 1 && prefix->data[0] == '/') {
        return true;
    }

    if (flen < plen) {
        return false;
    }

    if (memcmp(path->data, prefix->data, plen) != 0) {
        return false;
    }

    // Exact match or followed by '/'.
    return flen == plen || path->data[plen] == '/';
}

/*
 * Strip mount prefix from a VFS path to get the backend-relative path.
 * E.g. mount="/data", path="/data/foo.txt" -> "foo.txt"
 *      mount="/", path="/foo.txt" -> "foo.txt"
 *      mount="/data", path="/data" -> "" (root of backend)
 *
 * Returns empty string for the backend root.  Backend implementations
 * must handle empty path as "root of this backend".
 */
static n00b_string_t *
strip_prefix(n00b_string_t *path, n00b_string_t *mount_path,
             n00b_allocator_t *allocator)
{
    size_t mlen = mount_path->u8_bytes;

    // Root mount: just skip the leading '/'.
    if (mlen == 1 && mount_path->data[0] == '/') {
        if (path->u8_bytes <= 1) {
            return r"";
        }
        return n00b_string_from_raw(path->data + 1, path->u8_bytes - 1,
                                    .allocator = allocator);
    }

    // Exact match: path == mount_path -> backend root.
    if (path->u8_bytes == mlen) {
        return r"";
    }

    // Skip prefix + '/'.
    size_t skip = mlen;
    if (skip < path->u8_bytes && path->data[skip] == '/') {
        skip++;
    }

    if (skip >= path->u8_bytes) {
        return r"";
    }

    return n00b_string_from_raw(path->data + skip, path->u8_bytes - skip,
                                .allocator = allocator);
}

static n00b_buffer_t *
vfs_clone_buffer(n00b_buffer_t *buf, n00b_allocator_t *allocator)
{
    if (buf == nullptr) {
        return nullptr;
    }

    int64_t len = (int64_t)n00b_buffer_len(buf);
    return n00b_buffer_from_bytes(buf->data, len, .allocator = allocator);
}

// Release a backend's original list result once n00b_vfs_readdir has deep-cloned
// it into the caller's allocator. The result struct and entries array are always
// freshly allocated by the backend, so they are freed unconditionally; the name
// strings are freed only when the backend reports it owns them
// (list_result_owns_strings). The memory backend borrows persistent metadata
// dict keys for its names, so freeing those would corrupt live state.
static void
vfs_free_backend_list_result(n00b_vfs_list_result_t *list, bool owns_strings)
{
    if (list == nullptr) {
        return;
    }
    if (list->entries != nullptr) {
        if (owns_strings) {
            for (uint32_t i = 0; i < list->count; i++) {
                n00b_free(list->entries[i].name);
            }
        }
        n00b_free(list->entries);
    }
    if (owns_strings) {
        n00b_free(list->continuation);
    }
    n00b_free(list);
}

static n00b_vfs_list_result_t *
vfs_clone_list_result(n00b_vfs_list_result_t *src,
                      n00b_allocator_t       *allocator)
{
    if (src == nullptr) {
        return nullptr;
    }

    n00b_vfs_list_result_t *dst =
        n00b_alloc_with_opts(n00b_vfs_list_result_t,
                             &(n00b_alloc_opts_t){.allocator = allocator});
    dst->count        = src->count;
    dst->truncated    = src->truncated;
    dst->continuation =
        src->continuation == nullptr
            ? nullptr
            : n00b_string_from_raw(src->continuation->data,
                                   (int64_t)src->continuation->u8_bytes,
                                   .allocator = allocator);
    dst->entries = nullptr;

    if (src->count == 0 || src->entries == nullptr) {
        return dst;
    }

    dst->entries = n00b_alloc_array_with_opts(n00b_vfs_list_entry_t,
                                              src->count,
                                              &(n00b_alloc_opts_t){.allocator = allocator});
    for (uint32_t i = 0; i < src->count; i++) {
        dst->entries[i] = src->entries[i];
        if (src->entries[i].name != nullptr) {
            dst->entries[i].name =
                n00b_string_from_raw(src->entries[i].name->data,
                                     (int64_t)src->entries[i].name->u8_bytes,
                                     .allocator = allocator);
        }
    }

    return dst;
}

/*
 * Find the mount with the longest matching prefix for path.
 * Must be called with mount_lock held for read.
 */
static n00b_vfs_mount_t *
resolve_mount(n00b_vfs_t *vfs, n00b_string_t *path)
{
    // Mounts are sorted longest-first, so first match is best.
    for (uint32_t i = 0; i < vfs->nmounts; i++) {
        n00b_vfs_mount_t *m = vfs->mounts[i];
        if (m->active && path_has_prefix(path, m->mount_path)) {
            return m;
        }
    }

    return nullptr;
}

// ============================================================================
// Handle table helpers
// ============================================================================

static n00b_vfs_handle_t *
lookup_handle(n00b_vfs_t *vfs, n00b_vfs_fh_t fh)
{
    if (fh == N00B_VFS_FH_INVALID || fh > vfs->nhandles) {
        return nullptr;
    }

    n00b_vfs_handle_t *h = vfs->handles[fh - 1];
    if (h == nullptr || h->state != N00B_VFS_HANDLE_OPEN) {
        return nullptr;
    }

    return h;
}

static void
ensure_handles_cap(n00b_vfs_t *vfs, uint32_t needed)
{
    if (needed <= vfs->handles_cap) {
        return;
    }

    uint32_t new_cap = vfs->handles_cap;
    while (new_cap < needed) {
        new_cap *= 2;
    }

    n00b_vfs_handle_t **new_arr =
        n00b_alloc_array_with_opts(n00b_vfs_handle_t *,
                                   new_cap,
                                   &(n00b_alloc_opts_t){.allocator = vfs->allocator});
    if (vfs->nhandles > 0) {
        memcpy(new_arr, vfs->handles,
               vfs->nhandles * sizeof(n00b_vfs_handle_t *));
    }

    vfs->handles     = new_arr;
    vfs->handles_cap = new_cap;
}

// ============================================================================
// Mount table helpers
// ============================================================================

static void
ensure_mounts_cap(n00b_vfs_t *vfs, uint32_t needed)
{
    if (needed <= vfs->mounts_cap) {
        return;
    }

    uint32_t new_cap = vfs->mounts_cap;
    while (new_cap < needed) {
        new_cap *= 2;
    }

    n00b_vfs_mount_t **new_arr =
        n00b_alloc_array_with_opts(n00b_vfs_mount_t *,
                                   new_cap,
                                   &(n00b_alloc_opts_t){.allocator = vfs->allocator});
    if (vfs->nmounts > 0) {
        memcpy(new_arr, vfs->mounts,
               vfs->nmounts * sizeof(n00b_vfs_mount_t *));
    }

    vfs->mounts     = new_arr;
    vfs->mounts_cap = new_cap;
}

/*
 * Insert a mount into the sorted array (longest path first).
 */
static void
insert_mount_sorted(n00b_vfs_t *vfs, n00b_vfs_mount_t *m)
{
    ensure_mounts_cap(vfs, vfs->nmounts + 1);

    size_t mlen = m->mount_path->u8_bytes;
    uint32_t pos = vfs->nmounts;

    for (uint32_t i = 0; i < vfs->nmounts; i++) {
        if (vfs->mounts[i]->mount_path->u8_bytes < mlen) {
            pos = i;
            break;
        }
    }

    // Shift right.
    for (uint32_t i = vfs->nmounts; i > pos; i--) {
        vfs->mounts[i] = vfs->mounts[i - 1];
    }

    vfs->mounts[pos] = m;
    vfs->nmounts++;
}

// ============================================================================
// Hook helpers
// ============================================================================

/*
 * Collect hooks matching point from a mount.
 * Returns a freshly allocated array of only the matching hooks,
 * already sorted by priority (inherited from insertion order).
 */
static void
collect_hooks(n00b_vfs_mount_t *m, n00b_vfs_hook_point_t point,
              n00b_vfs_hook_t ***out, uint32_t *out_count)
{
    *out       = nullptr;
    *out_count = 0;

    if (m->nhooks == 0) {
        return;
    }

    // Count matching hooks.
    uint32_t count = 0;
    for (uint32_t i = 0; i < m->nhooks; i++) {
        if (m->hooks[i]->point == point) {
            count++;
        }
    }

    if (count == 0) {
        return;
    }

    // Build filtered array.
    n00b_vfs_hook_t **filtered =
        n00b_alloc_array_with_opts(n00b_vfs_hook_t *,
                                   count,
                                   &(n00b_alloc_opts_t){.allocator = m->allocator});
    uint32_t ix = 0;
    for (uint32_t i = 0; i < m->nhooks && ix < count; i++) {
        if (m->hooks[i]->point == point) {
            filtered[ix++] = m->hooks[i];
        }
    }

    *out       = filtered;
    *out_count = count;
}

static n00b_err_t
run_hooks(n00b_vfs_mount_t *m, n00b_vfs_hook_ctx_t *ctx)
{
    ctx->mount = m;

    n00b_vfs_hook_t **hooks;
    uint32_t          nhooks;

    collect_hooks(m, ctx->point, &hooks, &nhooks);
    return _n00b_vfs_hooks_run(hooks, nhooks, ctx);
}

// ============================================================================
// Lifecycle
// ============================================================================

n00b_result_t(n00b_vfs_t *)
n00b_vfs_new() _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    n00b_vfs_t *vfs = n00b_alloc_with_opts(n00b_vfs_t,
                                           &(n00b_alloc_opts_t){.allocator = allocator});

    vfs->mounts      = n00b_alloc_array_with_opts(n00b_vfs_mount_t *,
                                                  INITIAL_MOUNTS,
                                                  &(n00b_alloc_opts_t){.allocator = allocator});
    vfs->mounts_cap  = INITIAL_MOUNTS;
    vfs->nmounts     = 0;
    vfs->handles     = n00b_alloc_array_with_opts(n00b_vfs_handle_t *,
                                                  INITIAL_HANDLES,
                                                  &(n00b_alloc_opts_t){.allocator = allocator});
    vfs->handles_cap = INITIAL_HANDLES;
    vfs->nhandles    = 0;
    atomic_store(&vfs->next_fh, 1);

    vfs->mount_lock  = n00b_data_lock_new(.allocator = allocator);
    vfs->handle_lock = n00b_data_lock_new(.allocator = allocator);
    if (vfs->mount_lock == nullptr || vfs->handle_lock == nullptr) {
        return n00b_result_err(n00b_vfs_t *, N00B_VFS_ERR_ALLOC);
    }
    vfs->allocator   = allocator;

    return n00b_result_ok(n00b_vfs_t *, vfs);
}

static n00b_result_t(bool)
vfs_commit_handle(n00b_vfs_handle_t *h)
{
    if (h == nullptr || h->write_buf == nullptr
        || !(h->flags & N00B_VFS_OPEN_WRITE)) {
        return n00b_result_ok(bool, true);
    }

    if (h->write_committed) {
        return n00b_result_ok(bool, true);
    }

    n00b_vfs_mount_t *m = h->mount;
    n00b_result_t(bool) pr;
    if (h->flags & N00B_VFS_OPEN_EXCL) {
        if (m->backend->ops->put_if_absent == nullptr) {
            return n00b_result_err(bool, N00B_VFS_ERR_NOT_SUPPORTED);
        }
        pr = m->backend->ops->put_if_absent(m->backend->ctx,
                                            h->backend_path,
                                            h->write_buf);
    }
    else {
        pr = m->backend->ops->put(m->backend->ctx, h->backend_path,
                                  h->write_buf);
    }

    if (n00b_result_is_err(pr)) {
        return pr;
    }

    h->write_committed = true;
    return n00b_result_ok(bool, true);
}

void
n00b_vfs_destroy(n00b_vfs_t *vfs)
{
    if (vfs == nullptr) {
        return;
    }

    // Close all open handles and free write buffers.
    for (uint32_t i = 0; i < vfs->nhandles; i++) {
        n00b_vfs_handle_t *h = vfs->handles[i];
        if (h == nullptr) continue;
        if (h->state == N00B_VFS_HANDLE_OPEN) {
            // Flush pending writes before closing.
            if (h->write_buf != nullptr && (h->flags & N00B_VFS_OPEN_WRITE)
                && h->mount != nullptr && h->mount->backend != nullptr) {
                (void)vfs_commit_handle(h);
            }
            h->state = N00B_VFS_HANDLE_CLOSED;
        }
        if (h->write_buf != nullptr) {
            n00b_buffer_free(h->write_buf);
            h->write_buf = nullptr;
        }
    }

    // Cleanup all mounts.
    for (uint32_t i = 0; i < vfs->nmounts; i++) {
        n00b_vfs_mount_t *m = vfs->mounts[i];
        if (m == nullptr) continue;
        if (m->backend != nullptr) {
            n00b_vfs_backend_cleanup(m->backend);
        }
    }
}

// ============================================================================
// Mount management
// ============================================================================

n00b_result_t(n00b_vfs_mount_t *)
n00b_vfs_mount(n00b_vfs_t *vfs, n00b_string_t *path,
               n00b_vfs_backend_t *backend, uint32_t flags)
{
    if (vfs == nullptr || path == nullptr || backend == nullptr) {
        return n00b_result_err(n00b_vfs_mount_t *, N00B_VFS_ERR_NULL_ARG);
    }

    n00b_data_write_lock(vfs->mount_lock);

    // Check for duplicate mount path.
    for (uint32_t i = 0; i < vfs->nmounts; i++) {
        n00b_vfs_mount_t *existing = vfs->mounts[i];
        if (existing->active
            && existing->mount_path->u8_bytes == path->u8_bytes
            && memcmp(existing->mount_path->data, path->data,
                      path->u8_bytes) == 0) {
            n00b_data_unlock(vfs->mount_lock);
            return n00b_result_err(n00b_vfs_mount_t *, N00B_VFS_ERR_EXISTS);
        }
    }

    n00b_vfs_mount_t *m =
        n00b_alloc_with_opts(n00b_vfs_mount_t,
                             &(n00b_alloc_opts_t){.allocator = vfs->allocator});

    m->mount_path = path;
    m->backend    = backend;
    m->hooks      = nullptr;
    m->nhooks     = 0;
    m->hooks_cap  = 0;
    m->flags      = flags;
    m->active     = true;
    m->allocator  = vfs->allocator;
    m->lock       = n00b_data_lock_new(.allocator = vfs->allocator);

    insert_mount_sorted(vfs, m);

    n00b_data_unlock(vfs->mount_lock);
    return n00b_result_ok(n00b_vfs_mount_t *, m);
}

n00b_result_t(bool)
n00b_vfs_unmount(n00b_vfs_t *vfs, n00b_string_t *path)
{
    if (vfs == nullptr || path == nullptr) {
        return n00b_result_err(bool, N00B_VFS_ERR_NULL_ARG);
    }

    n00b_data_write_lock(vfs->mount_lock);

    for (uint32_t i = 0; i < vfs->nmounts; i++) {
        n00b_vfs_mount_t *m = vfs->mounts[i];
        if (m->active
            && m->mount_path->u8_bytes == path->u8_bytes
            && memcmp(m->mount_path->data, path->data, path->u8_bytes) == 0) {
            n00b_data_read_lock(vfs->handle_lock);
            for (uint32_t j = 0; j < vfs->nhandles; j++) {
                n00b_vfs_handle_t *h = vfs->handles[j];
                if (h != nullptr && h->state == N00B_VFS_HANDLE_OPEN
                    && h->mount == m) {
                    n00b_data_unlock(vfs->handle_lock);
                    n00b_data_unlock(vfs->mount_lock);
                    return n00b_result_err(bool, N00B_VFS_ERR_NOT_EMPTY);
                }
            }
            n00b_data_unlock(vfs->handle_lock);
            m->active = false;
            n00b_data_unlock(vfs->mount_lock);
            return n00b_result_ok(bool, true);
        }
    }

    n00b_data_unlock(vfs->mount_lock);
    return n00b_result_err(bool, N00B_VFS_ERR_NOT_FOUND);
}

// ============================================================================
// Hook registration
// ============================================================================

n00b_result_t(bool)
n00b_vfs_hook_add(n00b_vfs_mount_t *mount, n00b_vfs_hook_point_t point,
                  n00b_vfs_hook_fn fn, void *cookie, int32_t priority)
{
    if (mount == nullptr || fn == nullptr) {
        return n00b_result_err(bool, N00B_VFS_ERR_NULL_ARG);
    }

    n00b_vfs_hook_t *h =
        n00b_alloc_with_opts(n00b_vfs_hook_t,
                             &(n00b_alloc_opts_t){.allocator = mount->allocator});
    h->point    = point;
    h->fn       = fn;
    h->cookie   = cookie;
    h->priority = priority;

    n00b_data_write_lock(mount->lock);

    // Grow hook array if needed.
    if (mount->nhooks >= mount->hooks_cap) {
        uint32_t new_cap = mount->hooks_cap == 0 ? 4 : mount->hooks_cap * 2;
        n00b_vfs_hook_t **new_arr =
            n00b_alloc_array_with_opts(n00b_vfs_hook_t *,
                                       new_cap,
                                       &(n00b_alloc_opts_t){.allocator = mount->allocator});
        if (mount->nhooks > 0) {
            memcpy(new_arr, mount->hooks,
                   mount->nhooks * sizeof(n00b_vfs_hook_t *));
        }
        mount->hooks     = new_arr;
        mount->hooks_cap = new_cap;
    }

    // Insert sorted by priority (lower first).
    uint32_t pos = mount->nhooks;
    for (uint32_t i = 0; i < mount->nhooks; i++) {
        if (mount->hooks[i]->priority > priority) {
            pos = i;
            break;
        }
    }

    for (uint32_t i = mount->nhooks; i > pos; i--) {
        mount->hooks[i] = mount->hooks[i - 1];
    }

    mount->hooks[pos] = h;
    mount->nhooks++;

    n00b_data_unlock(mount->lock);
    return n00b_result_ok(bool, true);
}

// ============================================================================
// File operations
// ============================================================================

n00b_result_t(n00b_vfs_fh_t)
n00b_vfs_open(n00b_vfs_t *vfs, n00b_string_t *path, uint32_t flags)
{
    if (vfs == nullptr || path == nullptr) {
        return n00b_result_err(n00b_vfs_fh_t, N00B_VFS_ERR_NULL_ARG);
    }

    // Resolve mount.
    n00b_data_read_lock(vfs->mount_lock);
    n00b_vfs_mount_t *m = resolve_mount(vfs, path);
    n00b_data_unlock(vfs->mount_lock);

    if (m == nullptr) {
        return n00b_result_err(n00b_vfs_fh_t, N00B_VFS_ERR_MOUNT);
    }

    // Check read-only mount.
    if ((m->flags & N00B_VFS_MOUNT_READONLY)
        && (flags & N00B_VFS_OPEN_WRITE)) {
        return n00b_result_err(n00b_vfs_fh_t, N00B_VFS_ERR_READ_ONLY);
    }

    n00b_string_t *bpath = strip_prefix(path, m->mount_path, m->allocator);

    if ((flags & N00B_VFS_OPEN_EXCL) && !(flags & N00B_VFS_OPEN_CREATE)) {
        return n00b_result_err(n00b_vfs_fh_t, N00B_VFS_ERR_INVALID_PATH);
    }
    if (flags & N00B_VFS_OPEN_EXCL) {
        auto sr = m->backend->ops->stat(m->backend->ctx, bpath);
        if (n00b_result_is_ok(sr)) {
            return n00b_result_err(n00b_vfs_fh_t, N00B_VFS_ERR_EXISTS);
        }
        if (n00b_result_get_err(sr) != N00B_VFS_ERR_NOT_FOUND) {
            return n00b_result_err(n00b_vfs_fh_t, n00b_result_get_err(sr));
        }
    }

    // Run pre-open hooks.
    n00b_vfs_hook_ctx_t hctx = {
        .point = N00B_VFS_HOOK_PRE_OPEN,
        .path  = path,
        .flags = flags,
    };

    n00b_data_read_lock(m->lock);
    n00b_err_t herr = run_hooks(m, &hctx);
    n00b_data_unlock(m->lock);

    if (herr != N00B_VFS_ERR_NONE) {
        return n00b_result_err(n00b_vfs_fh_t, herr);
    }

    // For CREATE, the backend may need a put of empty data.
    // For non-CREATE opens, verify the object exists.
    if (!(flags & N00B_VFS_OPEN_CREATE)) {
        n00b_result_t(n00b_vfs_obj_stat_t) sr =
            m->backend->ops->stat(m->backend->ctx, bpath);
        if (n00b_result_is_err(sr)) {
            return n00b_result_err(n00b_vfs_fh_t, n00b_result_get_err(sr));
        }
    }

    // Allocate handle and insert into table atomically.
    n00b_vfs_handle_t *h =
        n00b_alloc_with_opts(n00b_vfs_handle_t,
                             &(n00b_alloc_opts_t){.allocator = m->allocator});
    h->path         = path;
    h->backend_path = bpath;
    h->flags        = flags;
    h->mount        = m;
    h->state        = N00B_VFS_HANDLE_OPEN;
    h->write_committed = false;
    atomic_store(&h->offset, 0);

    // For write mode, set up the write buffer before making the
    // handle visible.  Backend get is done outside the lock.
    if (flags & N00B_VFS_OPEN_WRITE) {
        if (flags & N00B_VFS_OPEN_TRUNC) {
            h->write_buf = n00b_buffer_empty(.allocator = m->allocator);
        }
        else {
            n00b_result_t(n00b_buffer_t *) gr =
                m->backend->ops->get(m->backend->ctx, bpath);
            if (n00b_result_is_ok(gr)) {
                h->write_buf = n00b_result_get(gr);
            }
            else {
                h->write_buf = n00b_buffer_empty(.allocator = m->allocator);
            }
        }

        if (flags & N00B_VFS_OPEN_APPEND) {
            atomic_store(&h->offset, n00b_buffer_len(h->write_buf));
        }
    }

    // Assign fh and insert under the lock so no thread can see the
    // fh value before the handle is in the table.
    n00b_data_write_lock(vfs->handle_lock);
    n00b_vfs_fh_t fh = atomic_fetch_add(&vfs->next_fh, 1);
    h->fh = fh;
    ensure_handles_cap(vfs, fh);
    if (fh > vfs->nhandles) {
        vfs->nhandles = fh;
    }
    vfs->handles[fh - 1] = h;
    n00b_data_unlock(vfs->handle_lock);

    // Run post-open hooks.
    hctx.point = N00B_VFS_HOOK_POST_OPEN;
    hctx.fh    = fh;

    n00b_data_read_lock(m->lock);
    run_hooks(m, &hctx);
    n00b_data_unlock(m->lock);

    return n00b_result_ok(n00b_vfs_fh_t, fh);
}

n00b_result_t(n00b_buffer_t *)
n00b_vfs_read(n00b_vfs_t *vfs, n00b_vfs_fh_t fh, uint64_t length) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (vfs == nullptr) {
        return n00b_result_err(n00b_buffer_t *, N00B_VFS_ERR_NULL_ARG);
    }

    n00b_data_read_lock(vfs->handle_lock);
    n00b_vfs_handle_t *h = lookup_handle(vfs, fh);
    n00b_data_unlock(vfs->handle_lock);

    if (h == nullptr) {
        return n00b_result_err(n00b_buffer_t *, N00B_VFS_ERR_INVALID_HANDLE);
    }

    if (!(h->flags & N00B_VFS_OPEN_READ)) {
        return n00b_result_err(n00b_buffer_t *, N00B_VFS_ERR_PERMISSION);
    }

    n00b_vfs_mount_t *m = h->mount;
    uint64_t off = atomic_load(&h->offset);
    n00b_allocator_t *out_allocator =
        allocator == nullptr ? m->allocator : allocator;

    // Pre-read hook.
    n00b_vfs_hook_ctx_t hctx = {
        .point  = N00B_VFS_HOOK_PRE_READ,
        .path   = h->path,
        .fh     = fh,
        .offset = off,
        .length = length,
    };

    n00b_data_read_lock(m->lock);
    n00b_err_t herr = run_hooks(m, &hctx);
    n00b_data_unlock(m->lock);

    if (herr != N00B_VFS_ERR_NONE) {
        return n00b_result_err(n00b_buffer_t *, herr);
    }

    // If there's a write buffer (file opened for write too), read from it.
    n00b_buffer_t *result;
    if (h->write_buf != nullptr) {
        size_t blen = n00b_buffer_len(h->write_buf);
        if (off >= blen) {
            result = n00b_buffer_from_bytes("", 0,
                                            .allocator = out_allocator);
        }
        else {
            uint64_t avail = blen - off;
            if (length > avail) {
                length = avail;
            }
            result = n00b_buffer_get_slice(h->write_buf, (int64_t)off,
                                           (int64_t)(off + length),
                                           .allocator = out_allocator);
        }
    }
    else {
        // Read from backend.
        n00b_result_t(n00b_buffer_t *) br;
        if (m->backend->ops->get_range != nullptr
            && m->backend->ops->supports_range_read != nullptr
            && m->backend->ops->supports_range_read(m->backend->ctx)) {
            br = m->backend->ops->get_range(m->backend->ctx,
                                             h->backend_path, off, length);
        }
        else {
            br = m->backend->ops->get(m->backend->ctx, h->backend_path);
            if (n00b_result_is_ok(br)) {
                n00b_buffer_t *full = n00b_result_get(br);
                size_t blen = n00b_buffer_len(full);
                if (off >= blen) {
                    br = n00b_result_ok(
                        n00b_buffer_t *,
                        n00b_buffer_from_bytes("",
                                               0,
                                               .allocator = out_allocator));
                }
                else {
                    uint64_t avail = blen - off;
                    if (length > avail) {
                        length = avail;
                    }
                    br = n00b_result_ok(n00b_buffer_t *,
                        n00b_buffer_get_slice(full, (int64_t)off,
                                              (int64_t)(off + length),
                                              .allocator = out_allocator));
                }
            }
        }

        if (n00b_result_is_err(br)) {
            return br;
        }
        result = n00b_result_get(br);
    }

    // Post-read hook (can transform data).
    hctx.point  = N00B_VFS_HOOK_POST_READ;
    hctx.data   = result;
    hctx.length = n00b_buffer_len(result);

    n00b_data_read_lock(m->lock);
    run_hooks(m, &hctx);
    n00b_data_unlock(m->lock);

    // Hook may have replaced data.
    if (hctx.data != nullptr) {
        result = hctx.data;
    }

    if (allocator != nullptr) {
        n00b_buffer_t *clone = vfs_clone_buffer(result, allocator);
        if (clone == nullptr) {
            return n00b_result_err(n00b_buffer_t *, N00B_VFS_ERR_ALLOC);
        }
        result = clone;
    }

    // Advance offset.
    atomic_store(&h->offset, off + n00b_buffer_len(result));

    return n00b_result_ok(n00b_buffer_t *, result);
}

n00b_result_t(uint64_t)
n00b_vfs_write(n00b_vfs_t *vfs, n00b_vfs_fh_t fh, n00b_buffer_t *data)
{
    if (vfs == nullptr || data == nullptr) {
        return n00b_result_err(uint64_t, N00B_VFS_ERR_NULL_ARG);
    }

    n00b_data_read_lock(vfs->handle_lock);
    n00b_vfs_handle_t *h = lookup_handle(vfs, fh);
    n00b_data_unlock(vfs->handle_lock);

    if (h == nullptr) {
        return n00b_result_err(uint64_t, N00B_VFS_ERR_INVALID_HANDLE);
    }

    if (!(h->flags & N00B_VFS_OPEN_WRITE)) {
        return n00b_result_err(uint64_t, N00B_VFS_ERR_PERMISSION);
    }
    if ((h->flags & N00B_VFS_OPEN_EXCL) && h->write_committed) {
        return n00b_result_err(uint64_t, N00B_VFS_ERR_EXISTS);
    }

    n00b_vfs_mount_t *m = h->mount;
    uint64_t off = atomic_load(&h->offset);

    // Pre-write hook.
    n00b_vfs_hook_ctx_t hctx = {
        .point  = N00B_VFS_HOOK_PRE_WRITE,
        .path   = h->path,
        .fh     = fh,
        .data   = data,
        .offset = off,
        .length = n00b_buffer_len(data),
    };

    n00b_data_read_lock(m->lock);
    n00b_err_t herr = run_hooks(m, &hctx);
    n00b_data_unlock(m->lock);

    if (herr != N00B_VFS_ERR_NONE) {
        return n00b_result_err(uint64_t, herr);
    }

    // Hook may have replaced data.
    if (hctx.data != nullptr) {
        data = hctx.data;
    }

    size_t write_len = n00b_buffer_len(data);

    // Accumulate into write buffer.
    if (h->write_buf != nullptr) {
        // Guard against OOM from absurd offsets.
        if (off + write_len > MAX_VFS_FILE_SIZE) {
            return n00b_result_err(uint64_t, N00B_VFS_ERR_NO_SPACE);
        }

        size_t cur_len = n00b_buffer_len(h->write_buf);

        // Extend buffer if writing past end.
        if (off + write_len > cur_len) {
            n00b_buffer_resize(h->write_buf, off + write_len);
        }
        h->write_committed = false;

        // Copy data into buffer at offset.
        char   *src;
        int64_t src_len;
        src = n00b_buffer_to_c(data, &src_len);

        char *dst = n00b_buffer_to_c(h->write_buf, nullptr);
        memcpy(dst + off, src, src_len);
    }

    // Advance offset.
    atomic_store(&h->offset, off + write_len);

    // Post-write hook.
    hctx.point = N00B_VFS_HOOK_POST_WRITE;

    n00b_data_read_lock(m->lock);
    run_hooks(m, &hctx);
    n00b_data_unlock(m->lock);

    return n00b_result_ok(uint64_t, write_len);
}

n00b_result_t(bool)
n00b_vfs_close(n00b_vfs_t *vfs, n00b_vfs_fh_t fh)
{
    if (vfs == nullptr) {
        return n00b_result_err(bool, N00B_VFS_ERR_NULL_ARG);
    }

    n00b_data_read_lock(vfs->handle_lock);
    n00b_vfs_handle_t *h = lookup_handle(vfs, fh);
    n00b_data_unlock(vfs->handle_lock);

    if (h == nullptr) {
        return n00b_result_err(bool, N00B_VFS_ERR_INVALID_HANDLE);
    }

    n00b_vfs_mount_t *m = h->mount;

    // Pre-close hook.
    n00b_vfs_hook_ctx_t hctx = {
        .point = N00B_VFS_HOOK_PRE_CLOSE,
        .path  = h->path,
        .fh    = fh,
    };

    n00b_data_read_lock(m->lock);
    n00b_err_t herr = run_hooks(m, &hctx);
    n00b_data_unlock(m->lock);

    if (herr != N00B_VFS_ERR_NONE) {
        return n00b_result_err(bool, herr);
    }

    // Flush write buffer to backend.
    if (h->write_buf != nullptr && (h->flags & N00B_VFS_OPEN_WRITE)) {
        n00b_result_t(bool) pr = vfs_commit_handle(h);
        if (n00b_result_is_err(pr)) {
            return n00b_result_err(bool, n00b_result_get_err(pr));
        }
        n00b_buffer_free(h->write_buf);
        h->write_buf = nullptr;
    }

    h->state = N00B_VFS_HANDLE_CLOSED;

    // Post-close hook.
    hctx.point = N00B_VFS_HOOK_POST_CLOSE;

    n00b_data_read_lock(m->lock);
    run_hooks(m, &hctx);
    n00b_data_unlock(m->lock);

    return n00b_result_ok(bool, true);
}

n00b_result_t(uint64_t)
n00b_vfs_seek(n00b_vfs_t *vfs, n00b_vfs_fh_t fh, int64_t offset, int whence)
{
    if (vfs == nullptr) {
        return n00b_result_err(uint64_t, N00B_VFS_ERR_NULL_ARG);
    }

    n00b_data_read_lock(vfs->handle_lock);
    n00b_vfs_handle_t *h = lookup_handle(vfs, fh);
    n00b_data_unlock(vfs->handle_lock);

    if (h == nullptr) {
        return n00b_result_err(uint64_t, N00B_VFS_ERR_INVALID_HANDLE);
    }

    uint64_t cur = atomic_load(&h->offset);
    uint64_t new_off;

    switch (whence) {
    case 0: // SEEK_SET
        if (offset < 0) {
            return n00b_result_err(uint64_t, N00B_VFS_ERR_INVALID_PATH);
        }
        new_off = (uint64_t)offset;
        break;

    case 1: // SEEK_CUR
        if (offset < 0 && (uint64_t)(-offset) > cur) {
            return n00b_result_err(uint64_t, N00B_VFS_ERR_INVALID_PATH);
        }
        new_off = (uint64_t)((int64_t)cur + offset);
        break;

    case 2: { // SEEK_END
        // Need file size.
        uint64_t size = 0;
        if (h->write_buf != nullptr) {
            size = n00b_buffer_len(h->write_buf);
        }
        else {
            n00b_result_t(n00b_vfs_obj_stat_t) sr =
                h->mount->backend->ops->stat(
                    h->mount->backend->ctx, h->backend_path);
            if (n00b_result_is_ok(sr)) {
                size = n00b_result_get(sr).size;
            }
        }

        if (offset < 0 && (uint64_t)(-offset) > size) {
            return n00b_result_err(uint64_t, N00B_VFS_ERR_INVALID_PATH);
        }
        new_off = (uint64_t)((int64_t)size + offset);
        break;
    }

    default:
        return n00b_result_err(uint64_t, N00B_VFS_ERR_NOT_SUPPORTED);
    }

    atomic_store(&h->offset, new_off);
    return n00b_result_ok(uint64_t, new_off);
}

// ============================================================================
// Truncate + Flush
// ============================================================================

n00b_result_t(bool)
n00b_vfs_truncate(n00b_vfs_t *vfs, n00b_string_t *path, uint64_t size)
{
    if (vfs == nullptr || path == nullptr) {
        return n00b_result_err(bool, N00B_VFS_ERR_NULL_ARG);
    }

    if (size > MAX_VFS_FILE_SIZE) {
        return n00b_result_err(bool, N00B_VFS_ERR_NO_SPACE);
    }

    n00b_data_read_lock(vfs->mount_lock);
    n00b_vfs_mount_t *m = resolve_mount(vfs, path);
    n00b_data_unlock(vfs->mount_lock);

    if (m == nullptr) {
        return n00b_result_err(bool, N00B_VFS_ERR_MOUNT);
    }

    if (m->flags & N00B_VFS_MOUNT_READONLY) {
        return n00b_result_err(bool, N00B_VFS_ERR_READ_ONLY);
    }

    n00b_string_t *bpath = strip_prefix(path, m->mount_path, m->allocator);

    // Read existing data (or empty for new file).
    n00b_buffer_t *buf;
    n00b_result_t(n00b_buffer_t *) gr =
        m->backend->ops->get(m->backend->ctx, bpath);
    if (n00b_result_is_ok(gr)) {
        buf = n00b_result_get(gr);
    }
    else {
        buf = n00b_buffer_empty(.allocator = m->allocator);
    }

    // Resize to target.
    n00b_buffer_resize(buf, size);

    return m->backend->ops->put(m->backend->ctx, bpath, buf);
}

n00b_result_t(bool)
n00b_vfs_flush(n00b_vfs_t *vfs, n00b_vfs_fh_t fh)
{
    if (vfs == nullptr) {
        return n00b_result_err(bool, N00B_VFS_ERR_NULL_ARG);
    }

    n00b_data_read_lock(vfs->handle_lock);
    n00b_vfs_handle_t *h = lookup_handle(vfs, fh);
    n00b_data_unlock(vfs->handle_lock);

    if (h == nullptr) {
        return n00b_result_err(bool, N00B_VFS_ERR_INVALID_HANDLE);
    }

    if (h->write_buf == nullptr || !(h->flags & N00B_VFS_OPEN_WRITE)) {
        return n00b_result_ok(bool, true);  // Nothing to flush.
    }

    return vfs_commit_handle(h);
}

// ============================================================================
// Metadata operations
// ============================================================================

n00b_result_t(n00b_vfs_obj_stat_t)
n00b_vfs_stat(n00b_vfs_t *vfs, n00b_string_t *path)
{
    if (vfs == nullptr || path == nullptr) {
        return n00b_result_err(n00b_vfs_obj_stat_t, N00B_VFS_ERR_NULL_ARG);
    }

    n00b_data_read_lock(vfs->mount_lock);
    n00b_vfs_mount_t *m = resolve_mount(vfs, path);
    n00b_data_unlock(vfs->mount_lock);

    if (m == nullptr) {
        return n00b_result_err(n00b_vfs_obj_stat_t, N00B_VFS_ERR_MOUNT);
    }

    n00b_string_t *bpath = strip_prefix(path, m->mount_path, m->allocator);

    // Pre-stat hook.
    n00b_vfs_hook_ctx_t hctx = {
        .point = N00B_VFS_HOOK_PRE_STAT,
        .path  = path,
    };

    n00b_data_read_lock(m->lock);
    n00b_err_t herr = run_hooks(m, &hctx);
    n00b_data_unlock(m->lock);

    if (herr != N00B_VFS_ERR_NONE) {
        return n00b_result_err(n00b_vfs_obj_stat_t, herr);
    }

    return m->backend->ops->stat(m->backend->ctx, bpath);
}

n00b_result_t(n00b_vfs_list_result_t *)
n00b_vfs_readdir(n00b_vfs_t *vfs, n00b_string_t *path,
                 uint32_t max_entries) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (vfs == nullptr || path == nullptr) {
        return n00b_result_err(n00b_vfs_list_result_t *, N00B_VFS_ERR_NULL_ARG);
    }

    n00b_data_read_lock(vfs->mount_lock);
    n00b_vfs_mount_t *m = resolve_mount(vfs, path);
    n00b_data_unlock(vfs->mount_lock);

    if (m == nullptr) {
        return n00b_result_err(n00b_vfs_list_result_t *, N00B_VFS_ERR_MOUNT);
    }

    n00b_string_t *bpath = strip_prefix(path, m->mount_path, m->allocator);

    auto list_r = m->backend->ops->list(m->backend->ctx,
                                        bpath,
                                        nullptr,
                                        max_entries);
    if (n00b_result_is_err(list_r) || allocator == nullptr) {
        return list_r;
    }

    n00b_vfs_list_result_t *original = n00b_result_get(list_r);
    n00b_vfs_list_result_t *clone =
        vfs_clone_list_result(original, allocator);
    if (clone == nullptr) {
        return n00b_result_err(n00b_vfs_list_result_t *, N00B_VFS_ERR_ALLOC);
    }

    // The backend result was deep-copied into the caller's allocator; release
    // the backend original so it does not linger in the backend's pool.
    vfs_free_backend_list_result(original,
                                 m->backend->ops->list_result_owns_strings);

    return n00b_result_ok(n00b_vfs_list_result_t *, clone);
}

n00b_result_t(n00b_string_t *)
n00b_vfs_local_path(n00b_vfs_t *vfs, n00b_string_t *path) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (vfs == nullptr || path == nullptr) {
        return n00b_result_err(n00b_string_t *, N00B_VFS_ERR_NULL_ARG);
    }
    if (vfs->mount_lock == nullptr) {
        return n00b_result_err(n00b_string_t *, N00B_VFS_ERR_STALE);
    }

    n00b_data_read_lock(vfs->mount_lock);
    n00b_vfs_mount_t *m = resolve_mount(vfs, path);
    n00b_data_unlock(vfs->mount_lock);

    if (m == nullptr) {
        return n00b_result_err(n00b_string_t *, N00B_VFS_ERR_MOUNT);
    }
    if (m->backend == nullptr || m->backend->ops == nullptr
        || m->backend->ops->local_path == nullptr) {
        return n00b_result_err(n00b_string_t *, N00B_VFS_ERR_NOT_SUPPORTED);
    }

    n00b_string_t *bpath = strip_prefix(path, m->mount_path, m->allocator);
    auto           path_r = m->backend->ops->local_path(m->backend->ctx, bpath);
    if (n00b_result_is_err(path_r) || allocator == nullptr) {
        return path_r;
    }

    n00b_string_t *local = n00b_result_get(path_r);
    return n00b_result_ok(n00b_string_t *,
                          n00b_string_from_raw(local->data,
                                               (int64_t)local->u8_bytes,
                                               .allocator = allocator));
}

n00b_result_t(bool)
n00b_vfs_mkdir(n00b_vfs_t *vfs, n00b_string_t *path)
{
    if (vfs == nullptr || path == nullptr) {
        return n00b_result_err(bool, N00B_VFS_ERR_NULL_ARG);
    }

    n00b_data_read_lock(vfs->mount_lock);
    n00b_vfs_mount_t *m = resolve_mount(vfs, path);
    n00b_data_unlock(vfs->mount_lock);

    if (m == nullptr) {
        return n00b_result_err(bool, N00B_VFS_ERR_MOUNT);
    }

    if (m->flags & N00B_VFS_MOUNT_READONLY) {
        return n00b_result_err(bool, N00B_VFS_ERR_READ_ONLY);
    }

    // Pre-mkdir hook.
    n00b_vfs_hook_ctx_t hctx = {
        .point = N00B_VFS_HOOK_PRE_MKDIR,
        .path  = path,
    };

    n00b_data_read_lock(m->lock);
    n00b_err_t herr = run_hooks(m, &hctx);
    n00b_data_unlock(m->lock);

    if (herr != N00B_VFS_ERR_NONE) {
        return n00b_result_err(bool, herr);
    }

    n00b_string_t *bpath = strip_prefix(path, m->mount_path, m->allocator);
    return m->backend->ops->mkdir(m->backend->ctx, bpath);
}

n00b_result_t(bool)
n00b_vfs_sync(n00b_vfs_t *vfs, n00b_string_t *path)
{
    if (vfs == nullptr || path == nullptr) {
        return n00b_result_err(bool, N00B_VFS_ERR_NULL_ARG);
    }

    n00b_data_read_lock(vfs->mount_lock);
    n00b_vfs_mount_t *m = resolve_mount(vfs, path);
    n00b_data_unlock(vfs->mount_lock);

    if (m == nullptr) {
        return n00b_result_err(bool, N00B_VFS_ERR_MOUNT);
    }

    if (m->backend == nullptr || m->backend->ops == nullptr
        || m->backend->ops->sync == nullptr
        || m->backend->ops->supports_durable_sync == nullptr
        || !m->backend->ops->supports_durable_sync(m->backend->ctx)) {
        return n00b_result_err(bool, N00B_VFS_ERR_NOT_SUPPORTED);
    }

    n00b_string_t *bpath = strip_prefix(path, m->mount_path, m->allocator);
    return m->backend->ops->sync(m->backend->ctx, bpath);
}

n00b_result_t(bool)
n00b_vfs_append(n00b_vfs_t *vfs, n00b_string_t *path, n00b_buffer_t *data)
{
    if (vfs == nullptr || path == nullptr || data == nullptr) {
        return n00b_result_err(bool, N00B_VFS_ERR_NULL_ARG);
    }

    n00b_data_read_lock(vfs->mount_lock);
    n00b_vfs_mount_t *m = resolve_mount(vfs, path);
    n00b_data_unlock(vfs->mount_lock);

    if (m == nullptr) {
        return n00b_result_err(bool, N00B_VFS_ERR_MOUNT);
    }
    if (m->flags & N00B_VFS_MOUNT_READONLY) {
        return n00b_result_err(bool, N00B_VFS_ERR_READ_ONLY);
    }

    n00b_vfs_hook_ctx_t hctx = {
        .point  = N00B_VFS_HOOK_PRE_WRITE,
        .path   = path,
        .data   = data,
        .length = n00b_buffer_len(data),
    };

    n00b_data_read_lock(m->lock);
    n00b_err_t herr = run_hooks(m, &hctx);
    n00b_data_unlock(m->lock);

    if (herr != N00B_VFS_ERR_NONE) {
        return n00b_result_err(bool, herr);
    }
    if (hctx.data != nullptr) {
        data = hctx.data;
    }

    n00b_string_t                *bpath = strip_prefix(path,
                                                       m->mount_path,
                                                       m->allocator);
    const n00b_vfs_backend_ops_t *ops   = m->backend->ops;
    n00b_result_t(bool)           put_r;
    if (ops->append != nullptr) {
        put_r = ops->append(m->backend->ctx, bpath, data);
    }
    else {
        auto get_r = ops->get(m->backend->ctx, bpath);
        if (n00b_result_is_err(get_r)) {
            if (n00b_result_get_err(get_r) != N00B_VFS_ERR_NOT_FOUND) {
                return n00b_result_err(bool, n00b_result_get_err(get_r));
            }
            put_r = ops->put(m->backend->ctx, bpath, data);
        }
        else {
            n00b_buffer_t *whole = n00b_result_get(get_r);
            n00b_buffer_concat(whole, data);
            put_r = ops->put(m->backend->ctx, bpath, whole);
        }
    }
    if (n00b_result_is_err(put_r)) {
        return put_r;
    }

    hctx.point = N00B_VFS_HOOK_POST_WRITE;
    n00b_data_read_lock(m->lock);
    run_hooks(m, &hctx);
    n00b_data_unlock(m->lock);
    return put_r;
}

n00b_result_t(bool)
n00b_vfs_delete(n00b_vfs_t *vfs, n00b_string_t *path)
{
    if (vfs == nullptr || path == nullptr) {
        return n00b_result_err(bool, N00B_VFS_ERR_NULL_ARG);
    }

    n00b_data_read_lock(vfs->mount_lock);
    n00b_vfs_mount_t *m = resolve_mount(vfs, path);
    n00b_data_unlock(vfs->mount_lock);

    if (m == nullptr) {
        return n00b_result_err(bool, N00B_VFS_ERR_MOUNT);
    }

    if (m->flags & N00B_VFS_MOUNT_READONLY) {
        return n00b_result_err(bool, N00B_VFS_ERR_READ_ONLY);
    }

    // Pre-delete hook.
    n00b_vfs_hook_ctx_t hctx = {
        .point = N00B_VFS_HOOK_PRE_DELETE,
        .path  = path,
    };

    n00b_data_read_lock(m->lock);
    n00b_err_t herr = run_hooks(m, &hctx);
    n00b_data_unlock(m->lock);

    if (herr != N00B_VFS_ERR_NONE) {
        return n00b_result_err(bool, herr);
    }

    n00b_string_t *bpath = strip_prefix(path, m->mount_path, m->allocator);
    return m->backend->ops->del(m->backend->ctx, bpath);
}

n00b_result_t(bool)
n00b_vfs_rename(n00b_vfs_t *vfs, n00b_string_t *old_path,
                n00b_string_t *new_path)
{
    if (vfs == nullptr || old_path == nullptr || new_path == nullptr) {
        return n00b_result_err(bool, N00B_VFS_ERR_NULL_ARG);
    }

    // Both paths must resolve to the same mount.
    n00b_data_read_lock(vfs->mount_lock);
    n00b_vfs_mount_t *m_old = resolve_mount(vfs, old_path);
    n00b_vfs_mount_t *m_new = resolve_mount(vfs, new_path);
    n00b_data_unlock(vfs->mount_lock);

    if (m_old == nullptr || m_new == nullptr) {
        return n00b_result_err(bool, N00B_VFS_ERR_MOUNT);
    }

    if (m_old != m_new) {
        return n00b_result_err(bool, N00B_VFS_ERR_CROSS_DEVICE);
    }

    if (m_old->flags & N00B_VFS_MOUNT_READONLY) {
        return n00b_result_err(bool, N00B_VFS_ERR_READ_ONLY);
    }

    // Pre-rename hook.
    n00b_vfs_hook_ctx_t hctx = {
        .point      = N00B_VFS_HOOK_PRE_RENAME,
        .path       = old_path,
        .rename_dst = new_path,
    };

    n00b_data_read_lock(m_old->lock);
    n00b_err_t herr = run_hooks(m_old, &hctx);
    n00b_data_unlock(m_old->lock);

    if (herr != N00B_VFS_ERR_NONE) {
        return n00b_result_err(bool, herr);
    }

    n00b_string_t *old_bpath = strip_prefix(old_path,
                                            m_old->mount_path,
                                            m_old->allocator);
    n00b_string_t *new_bpath = strip_prefix(new_path,
                                            m_old->mount_path,
                                            m_old->allocator);

    return m_old->backend->ops->rename(m_old->backend->ctx,
                                        old_bpath, new_bpath);
}
