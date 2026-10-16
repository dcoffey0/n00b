#include "vfs/backend_memory.h"
#include "core/alloc.h"
#include "core/hash.h"
#include "core/time.h"
#include "adt/dict.h"

// ============================================================================
// Internal context
// ============================================================================

typedef struct {
    n00b_dict_t(n00b_string_t *, n00b_buffer_t *)  data;
    n00b_dict_t(n00b_string_t *, n00b_vfs_obj_stat_t *) meta;
    n00b_allocator_t                              *allocator;
} mem_ctx_t;

// ============================================================================
// Helpers
// ============================================================================

static uint64_t
now_ns(void)
{
    n00b_duration_t ts;

    n00b_capture_timestamp(&ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static n00b_vfs_obj_stat_t *
make_stat(mem_ctx_t *mc, n00b_string_t *path, n00b_vfs_obj_kind_t kind,
          uint64_t size)
{
    n00b_vfs_obj_stat_t *st =
        n00b_alloc_with_opts(n00b_vfs_obj_stat_t,
                             &(n00b_alloc_opts_t){.allocator = mc->allocator});

    st->kind     = kind;
    st->size     = size;
    st->atime_ns = now_ns();
    st->mtime_ns = st->atime_ns;
    st->ctime_ns = st->atime_ns;
    st->mode     = (kind == N00B_VFS_OBJ_DIR) ? 0755 : 0644;

    n00b_dict_put(&mc->meta, path, st);
    return st;
}

// ============================================================================
// Vtable implementations
// ============================================================================

static n00b_string_t *
mem_name(void)
{
    return r"memory";
}

static void *
mem_init(n00b_vfs_backend_t *be)
{
    mem_ctx_t *mc = n00b_alloc_with_opts(mem_ctx_t,
                                         &(n00b_alloc_opts_t){.allocator = be->allocator});
    mc->allocator = be->allocator;

    n00b_dict_init(&mc->data,
                   .allocator = be->allocator,
                   .hash = n00b_string_hash,
                   .skip_obj_hash = true);
    n00b_dict_init(&mc->meta,
                   .allocator = be->allocator,
                   .hash = n00b_string_hash,
                   .skip_obj_hash = true);

    return mc;
}

static void
mem_cleanup(void *ctx)
{
    (void)ctx;
    // GC handles everything.
}

static n00b_result_t(n00b_buffer_t *)
mem_get(void *ctx, n00b_string_t *path)
{
    mem_ctx_t *mc = ctx;
    bool       found;

    n00b_buffer_t *buf = n00b_dict_get(&mc->data, path, &found);
    if (!found) {
        return n00b_result_err(n00b_buffer_t *, N00B_VFS_ERR_NOT_FOUND);
    }

    return n00b_result_ok(n00b_buffer_t *,
                          n00b_buffer_copy(buf,
                                           .allocator = mc->allocator));
}

static n00b_result_t(n00b_buffer_t *)
mem_get_range(void *ctx, n00b_string_t *path, uint64_t offset, uint64_t length)
{
    mem_ctx_t *mc = ctx;
    bool       found;

    n00b_buffer_t *buf = n00b_dict_get(&mc->data, path, &found);
    if (!found) {
        return n00b_result_err(n00b_buffer_t *, N00B_VFS_ERR_NOT_FOUND);
    }

    size_t blen = n00b_buffer_len(buf);
    if (offset >= blen) {
        return n00b_result_ok(n00b_buffer_t *,
                              n00b_buffer_empty(.allocator = mc->allocator));
    }

    uint64_t avail = blen - offset;
    if (length > avail) {
        length = avail;
    }

    return n00b_result_ok(n00b_buffer_t *,
                          n00b_buffer_get_slice(buf,
                                                (int64_t)offset,
                                                (int64_t)(offset + length),
                                                .allocator = mc->allocator));
}

static n00b_result_t(bool)
mem_put(void *ctx, n00b_string_t *path, n00b_buffer_t *data)
{
    mem_ctx_t *mc = ctx;

    n00b_buffer_t *copy = n00b_buffer_copy(data, .allocator = mc->allocator);
    n00b_dict_put(&mc->data, path, copy);
    make_stat(mc, path, N00B_VFS_OBJ_FILE, n00b_buffer_len(copy));

    return n00b_result_ok(bool, true);
}

static n00b_result_t(bool)
mem_put_if_absent(void *ctx, n00b_string_t *path, n00b_buffer_t *data)
{
    mem_ctx_t *mc = ctx;
    bool       found;

    (void)n00b_dict_get(&mc->data, path, &found);
    if (found) {
        return n00b_result_err(bool, N00B_VFS_ERR_EXISTS);
    }

    return mem_put(ctx, path, data);
}

static n00b_result_t(bool)
mem_append(void *ctx, n00b_string_t *path, n00b_buffer_t *data)
{
    mem_ctx_t     *mc = ctx;
    bool           found;
    n00b_buffer_t *cur = n00b_dict_get(&mc->data, path, &found);

    if (!found) {
        return mem_put(ctx, path, data);
    }

    // A reader may hold the stored buffer, so the longer one replaces it.
    n00b_buffer_t *grown = n00b_buffer_copy(cur, .allocator = mc->allocator);
    n00b_buffer_concat(grown, data);
    n00b_dict_put(&mc->data, path, grown);
    make_stat(mc, path, N00B_VFS_OBJ_FILE, n00b_buffer_len(grown));

    return n00b_result_ok(bool, true);
}

static n00b_result_t(bool)
mem_del(void *ctx, n00b_string_t *path)
{
    mem_ctx_t *mc = ctx;
    bool       found;

    (void)n00b_dict_get(&mc->data, path, &found);
    if (!found) {
        return n00b_result_err(bool, N00B_VFS_ERR_NOT_FOUND);
    }

    n00b_dict_remove(&mc->data, path);
    n00b_dict_remove(&mc->meta, path);

    return n00b_result_ok(bool, true);
}

static n00b_result_t(n00b_vfs_obj_stat_t)
mem_stat(void *ctx, n00b_string_t *path)
{
    mem_ctx_t          *mc = ctx;
    bool                found;
    n00b_vfs_obj_stat_t *st = n00b_dict_get(&mc->meta, path, &found);

    if (!found) {
        return n00b_result_err(n00b_vfs_obj_stat_t, N00B_VFS_ERR_NOT_FOUND);
    }

    return n00b_result_ok(n00b_vfs_obj_stat_t, *st);
}

static n00b_result_t(n00b_vfs_list_result_t *)
mem_list(void *ctx, n00b_string_t *prefix, n00b_string_t *continuation,
         uint32_t max_keys)
{
    (void)continuation;  // In-memory backend returns everything in one shot.

    mem_ctx_t *mc = ctx;

    // Count matching entries first.
    const char *pfx     = prefix ? prefix->data : "";
    size_t      pfx_len = prefix ? prefix->u8_bytes : 0;
    uint32_t    count   = 0;

    n00b_dict_foreach(&mc->meta, k, v, {
        (void)v;
        if (pfx_len == 0 || (k->u8_bytes >= pfx_len
            && memcmp(k->data, pfx, pfx_len) == 0)) {
            count++;
        }
    });

    bool truncated = false;
    if (max_keys > 0 && count > max_keys) {
        truncated = true;
        count     = max_keys;
    }

    n00b_vfs_list_result_t *res =
        n00b_alloc_with_opts(n00b_vfs_list_result_t,
                             &(n00b_alloc_opts_t){.allocator = mc->allocator});
    res->count        = count;
    res->continuation = nullptr;
    res->truncated    = truncated;

    if (count == 0) {
        res->entries = nullptr;
        return n00b_result_ok(n00b_vfs_list_result_t *, res);
    }

    res->entries = n00b_alloc_array_with_opts(n00b_vfs_list_entry_t,
                                              count,
                                              &(n00b_alloc_opts_t){.allocator = mc->allocator});

    uint32_t ix = 0;

    n00b_dict_foreach(&mc->meta, k, v, {
        if (ix < count && (pfx_len == 0 || (k->u8_bytes >= pfx_len
            && memcmp(k->data, pfx, pfx_len) == 0))) {
            res->entries[ix].name     = k;
            res->entries[ix].kind     = v->kind;
            res->entries[ix].size     = v->size;
            res->entries[ix].mtime_ns = v->mtime_ns;
            ix++;
        }
    });

    res->count = ix;
    return n00b_result_ok(n00b_vfs_list_result_t *, res);
}

static n00b_result_t(bool)
mem_rename(void *ctx, n00b_string_t *old_path, n00b_string_t *new_path)
{
    mem_ctx_t *mc = ctx;
    bool       found;

    n00b_buffer_t *buf = n00b_dict_get(&mc->data, old_path, &found);
    if (!found) {
        return n00b_result_err(bool, N00B_VFS_ERR_NOT_FOUND);
    }

    n00b_vfs_obj_stat_t *st = n00b_dict_get(&mc->meta, old_path, &found);

    // Copy to new key, remove old.
    n00b_dict_put(&mc->data, new_path, buf);
    if (st != nullptr) {
        n00b_dict_put(&mc->meta, new_path, st);
    }

    n00b_dict_remove(&mc->data, old_path);
    n00b_dict_remove(&mc->meta, old_path);

    return n00b_result_ok(bool, true);
}

static n00b_result_t(bool)
mem_mkdir(void *ctx, n00b_string_t *path)
{
    mem_ctx_t *mc = ctx;
    bool       found;

    (void)n00b_dict_get(&mc->meta, path, &found);
    if (found) {
        return n00b_result_err(bool, N00B_VFS_ERR_EXISTS);
    }

    make_stat(mc, path, N00B_VFS_OBJ_DIR, 0);
    return n00b_result_ok(bool, true);
}

static n00b_result_t(bool)
mem_sync(void *ctx, n00b_string_t *path)
{
    (void)ctx;
    (void)path;
    return n00b_result_err(bool, N00B_VFS_ERR_NOT_SUPPORTED);
}

static bool
mem_supports_range_read(void *ctx)
{
    (void)ctx;
    return true;
}

static bool
mem_supports_rename(void *ctx)
{
    (void)ctx;
    return true;
}

static bool
mem_supports_link(void *ctx)
{
    (void)ctx;
    return false;
}

static bool
mem_supports_durable_sync(void *ctx)
{
    (void)ctx;
    return false;
}

// ============================================================================
// Vtable
// ============================================================================

const n00b_vfs_backend_ops_t n00b_vfs_backend_memory_ops = {
    .name                = mem_name,
    .init                = mem_init,
    .cleanup             = mem_cleanup,
    .get                 = mem_get,
    .get_range           = mem_get_range,
    .put                 = mem_put,
    .put_if_absent       = mem_put_if_absent,
    .append              = mem_append,
    .del                 = mem_del,
    .stat                = mem_stat,
    .list                = mem_list,
    .rename              = mem_rename,
    .mkdir               = mem_mkdir,
    .sync                = mem_sync,
    .supports_range_read = mem_supports_range_read,
    .supports_rename     = mem_supports_rename,
    .supports_link       = mem_supports_link,
    .supports_durable_sync = mem_supports_durable_sync,
    .link                = nullptr,
};

// ============================================================================
// Constructor
// ============================================================================

n00b_result_t(n00b_vfs_backend_t *)
n00b_vfs_backend_memory_new() _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    n00b_vfs_backend_t *be =
        n00b_alloc_with_opts(n00b_vfs_backend_t, &(n00b_alloc_opts_t){.allocator = allocator});

    be->ops       = &n00b_vfs_backend_memory_ops;
    be->root      = r"";
    be->allocator = allocator;

    n00b_result_t(bool) r = n00b_vfs_backend_init(be);
    if (n00b_result_is_err(r)) {
        return n00b_result_err(n00b_vfs_backend_t *, n00b_result_get_err(r));
    }

    return n00b_result_ok(n00b_vfs_backend_t *, be);
}
