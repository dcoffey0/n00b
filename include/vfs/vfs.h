/**
 * @file vfs.h
 * @brief VFS core: mount table, file handles, path resolution, operations.
 *
 * The VFS layer provides POSIX-ish file semantics over any storage
 * backend.  It manages:
 * - A mount table mapping VFS path prefixes to backends
 * - A file handle table with open/close state and seek offsets
 * - Hook/filter chains per mount
 *
 * Path resolution uses longest-prefix matching against the mount table.
 */
#pragma once

#include "vfs/types.h"
#include "vfs/backend.h"
#include "vfs/hooks.h"
#include "core/data_lock.h"

// ============================================================================
// Forward declarations
// ============================================================================

typedef struct n00b_vfs       n00b_vfs_t;
typedef struct n00b_vfs_mount n00b_vfs_mount_t;

// ============================================================================
// Mount point
// ============================================================================

/** @brief Mount flags. */
typedef enum {
    N00B_VFS_MOUNT_READONLY = (1 << 0),
} n00b_vfs_mount_flags_t;

/**
 * @brief A mount point binding a VFS path prefix to a backend.
 */
struct n00b_vfs_mount {
    n00b_string_t      *mount_path;  /**< VFS path prefix (e.g. "/data"). */
    n00b_vfs_backend_t *backend;
    n00b_vfs_hook_t   **hooks;       /**< Hook array (sorted by priority). */
    uint32_t            nhooks;
    uint32_t            hooks_cap;
    n00b_rwlock_t      *lock;        /**< Protects hooks and metadata. */
    n00b_allocator_t   *allocator;
    uint32_t            flags;
    bool                active;
};

// ============================================================================
// Handle state
// ============================================================================

typedef enum {
    N00B_VFS_HANDLE_OPEN,
    N00B_VFS_HANDLE_CLOSING,
    N00B_VFS_HANDLE_CLOSED,
} n00b_vfs_handle_state_t;

/**
 * @brief An open file handle in the VFS.
 */
typedef struct n00b_vfs_handle {
    n00b_vfs_fh_t            fh;
    n00b_string_t           *path;         /**< Full VFS path. */
    n00b_string_t           *backend_path; /**< Path relative to mount root. */
    uint32_t                 flags;
    _Atomic(uint64_t)        offset;
    n00b_vfs_mount_t        *mount;
    n00b_vfs_handle_state_t  state;
    n00b_buffer_t           *write_buf;    /**< Accumulated writes (for backends without random write). */
    bool                     write_committed;
} n00b_vfs_handle_t;

// ============================================================================
// VFS instance
// ============================================================================

/**
 * @brief The VFS instance — owns mount table and handle table.
 */
struct n00b_vfs {
    n00b_vfs_mount_t  **mounts;       /**< Sorted by path length desc. */
    uint32_t            nmounts;
    uint32_t            mounts_cap;
    n00b_vfs_handle_t **handles;      /**< Indexed by (fh - 1). */
    uint32_t            nhandles;
    uint32_t            handles_cap;
    _Atomic(uint64_t)   next_fh;
    n00b_rwlock_t      *mount_lock;
    n00b_rwlock_t      *handle_lock;
    n00b_allocator_t   *allocator;
};

// ============================================================================
// Lifecycle
// ============================================================================

/**
 * @brief Construct an empty VFS instance.
 *
 * @kw allocator Allocator for the VFS object and VFS-owned mount, hook, handle,
 *               and handle-buffer metadata.
 *
 * @return Ok(vfs) on success.
 */
extern n00b_result_t(n00b_vfs_t *)
n00b_vfs_new() _kargs
{
    n00b_allocator_t *allocator = nullptr;
};
/**
 * @brief Release VFS-owned process state.
 *
 * @param vfs VFS instance returned by @ref n00b_vfs_new. May be null.
 *
 * @post Open handles, mount records, hooks, and backend contexts owned by this
 *       VFS are closed/cleaned up where the backend exposes cleanup hooks.
 *       Durable backend objects are not deleted.
 */
extern void n00b_vfs_destroy(n00b_vfs_t *vfs);

// ============================================================================
// Mount management
// ============================================================================

/**
 * @brief Mount a backend at a VFS path.
 * @param vfs      VFS instance.
 * @param path     Mount path prefix (e.g. "/data").
 * @param backend  Initialized backend.
 * @param flags    Mount flags (0 for defaults).
 * @return Ok(mount) on success.
 */
extern n00b_result_t(n00b_vfs_mount_t *)
n00b_vfs_mount(n00b_vfs_t *vfs, n00b_string_t *path,
               n00b_vfs_backend_t *backend, uint32_t flags);

/**
 * @brief Unmount a backend at the given path.
 *
 * @param vfs  VFS instance.
 * @param path Absolute mount path previously passed to @ref n00b_vfs_mount.
 *
 * @return Ok(true) when the mount is deactivated. Returns
 *         @c N00B_VFS_ERR_NOT_FOUND if the mount is absent and
 *         @c N00B_VFS_ERR_NOT_EMPTY if open handles still reference it.
 * @post The backend is no longer selected for future path resolution.
 */
extern n00b_result_t(bool)
n00b_vfs_unmount(n00b_vfs_t *vfs, n00b_string_t *path);

// ============================================================================
// Hook registration
// ============================================================================

/**
 * @brief Register a hook on a mount.
 * @param mount     Target mount.
 * @param point     Hook interception point.
 * @param fn        Callback function.
 * @param cookie    Opaque user data.
 * @param priority  Lower runs first (default 0).
 * @return Ok(true) on success.
 */
extern n00b_result_t(bool)
n00b_vfs_hook_add(n00b_vfs_mount_t *mount, n00b_vfs_hook_point_t point,
                  n00b_vfs_hook_fn fn, void *cookie, int32_t priority);

// ============================================================================
// File operations
// ============================================================================

/**
 * @brief Open a VFS object handle.
 *
 * @param vfs   VFS instance.
 * @param path  Absolute VFS path.
 * @param flags Bitmask of @c N00B_VFS_OPEN_* flags.
 *
 * @return Ok(handle) on success. Read-only mounts reject write flags with
 *         @c N00B_VFS_ERR_READ_ONLY. @c N00B_VFS_OPEN_EXCL requires create
 *         semantics and rejects existing objects with @c N00B_VFS_ERR_EXISTS.
 */
extern n00b_result_t(n00b_vfs_fh_t)
n00b_vfs_open(n00b_vfs_t *vfs, n00b_string_t *path, uint32_t flags);

/**
 * @brief Read up to @p length bytes from an open VFS handle.
 *
 * @param vfs    VFS instance.
 * @param fh     Open handle returned by @ref n00b_vfs_open.
 * @param length Maximum bytes to read.
 * @kw allocator Allocator for VFS-created result buffers. Backend-owned
 *               buffers are copied into this allocator when it is provided.
 * @return Ok(buffer) with possibly fewer than @p length bytes, or a typed VFS
 *         error. A zero-length buffer represents EOF.
 * @post On success, the handle offset advances by the returned byte count.
 */
extern n00b_result_t(n00b_buffer_t *)
n00b_vfs_read(n00b_vfs_t *vfs, n00b_vfs_fh_t fh, uint64_t length) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Write bytes at the handle's current offset.
 *
 * @param vfs  VFS instance.
 * @param fh   Open writable handle.
 * @param data Buffer to copy into the handle's pending write image.
 * @return Ok(bytes written) or a typed VFS error.
 */
extern n00b_result_t(uint64_t)
n00b_vfs_write(n00b_vfs_t *vfs, n00b_vfs_fh_t fh, n00b_buffer_t *data);

/**
 * @brief Close an open VFS handle, committing pending writes.
 *
 * @param vfs VFS instance.
 * @param fh  Open handle.
 * @return Ok(true) on success or a typed VFS error. Exclusive-create handles
 *         commit through backend put-if-absent semantics.
 */
extern n00b_result_t(bool)
n00b_vfs_close(n00b_vfs_t *vfs, n00b_vfs_fh_t fh);

/**
 * @brief Set an open handle's offset.
 *
 * @param vfs    VFS instance.
 * @param fh     Open handle.
 * @param offset Offset interpreted according to @p whence.
 * @param whence One of @c SEEK_SET, @c SEEK_CUR, or @c SEEK_END.
 *
 * @return Ok(new offset) or a typed VFS error.
 * @post No data is read or written; only the handle offset changes.
 */
extern n00b_result_t(uint64_t)
n00b_vfs_seek(n00b_vfs_t *vfs, n00b_vfs_fh_t fh, int64_t offset, int whence);

/**
 * @brief Truncate a file to a given size.
 *
 * @param vfs  VFS instance.
 * @param path Absolute VFS object path.
 * @param size Target byte length.
 *
 * @return Ok(true) on success or a typed VFS error.
 * @post If the file is shorter it is extended with zero bytes; if longer it is
 *       shortened.
 */
extern n00b_result_t(bool)
n00b_vfs_truncate(n00b_vfs_t *vfs, n00b_string_t *path, uint64_t size);

/**
 * @brief Flush a file handle's write buffer to the backend without closing.
 *
 * @param vfs VFS instance.
 * @param fh  Open writable handle.
 *
 * @return Ok(true) on success or a typed VFS error.
 * @post Pending writes are committed to the backend but the handle remains
 *       open. The durability level is backend-specific; use @ref n00b_vfs_sync
 *       for an explicit durable barrier where supported.
 */
extern n00b_result_t(bool)
n00b_vfs_flush(n00b_vfs_t *vfs, n00b_vfs_fh_t fh);

// ============================================================================
// Metadata operations
// ============================================================================

/**
 * @brief Return metadata for a VFS object.
 *
 * @param vfs  VFS instance.
 * @param path Absolute VFS object path.
 *
 * @return Ok(stat) for an existing object or a typed VFS error.
 */
extern n00b_result_t(n00b_vfs_obj_stat_t)
n00b_vfs_stat(n00b_vfs_t *vfs, n00b_string_t *path);

/**
 * @brief List entries under a VFS directory/prefix.
 *
 * @param vfs         VFS instance.
 * @param path        Absolute VFS directory or backend prefix.
 * @param max_entries Maximum entries to return; @c 0 means backend default/all.
 * @kw allocator Allocator for the returned list result and copied entry names
 *               when provided.
 *
 * @return Ok(list result) or a typed VFS error. Backends with pagination set
 *         `truncated` and `continuation`.
 */
extern n00b_result_t(n00b_vfs_list_result_t *)
n00b_vfs_readdir(n00b_vfs_t *vfs, n00b_string_t *path,
                 uint32_t max_entries) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Resolve a VFS path to a materialized local filesystem path.
 *
 * Only local-file-backed mounts support this. Object, memory, and remote
 * backends return @c N00B_VFS_ERR_NOT_SUPPORTED. The returned path is suitable
 * for direct local file APIs such as mmap.
 */
extern n00b_result_t(n00b_string_t *)
n00b_vfs_local_path(n00b_vfs_t *vfs, n00b_string_t *path) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Create one directory at @p path.
 *
 * @param vfs  VFS instance.
 * @param path Absolute VFS directory path.
 *
 * @return Ok(true) on success or a typed VFS error. Missing parents are
 *         backend-specific and may return @c N00B_VFS_ERR_NOT_FOUND /
 *         @c N00B_VFS_ERR_NOT_DIR.
 */
extern n00b_result_t(bool)
n00b_vfs_mkdir(n00b_vfs_t *vfs, n00b_string_t *path);

/**
 * @brief Request durable visibility for a VFS object or directory.
 *
 * @param vfs  VFS instance.
 * @param path Absolute VFS object or directory path to sync.
 *
 * @return Ok(true) when the backend confirms a durable sync. Backends that
 *         cannot express a durable sync return
 *         @c N00B_VFS_ERR_NOT_SUPPORTED. Object-store style backends may make
 *         writes durable as part of @ref n00b_vfs_flush / close and therefore
 *         decline this operation.
 */
extern n00b_result_t(bool)
n00b_vfs_sync(n00b_vfs_t *vfs, n00b_string_t *path);

/**
 * @brief Append bytes to the end of one VFS object, creating it when absent.
 *
 * A backend with an append operation writes only @p data. Any other backend
 * rewrites the whole object, so there the cost is the object's size. The
 * append is not durable until the caller syncs the path, and its parent
 * when the append created it.
 *
 * @param vfs  VFS instance.
 * @param path Absolute VFS object path.
 * @param data Bytes to append.
 *
 * @return Ok(true) once the bytes are in the object, or a typed VFS error.
 */
extern n00b_result_t(bool)
n00b_vfs_append(n00b_vfs_t *vfs, n00b_string_t *path, n00b_buffer_t *data);

/**
 * @brief Delete one VFS object.
 *
 * @param vfs  VFS instance.
 * @param path Absolute VFS object path.
 *
 * @return Ok(true) on deletion or a typed VFS error. Recursive directory
 *         deletion is not implied.
 */
extern n00b_result_t(bool)
n00b_vfs_delete(n00b_vfs_t *vfs, n00b_string_t *path);

/**
 * @brief Rename one VFS object within a backend.
 *
 * @param vfs      VFS instance.
 * @param old_path Existing absolute VFS path.
 * @param new_path Destination absolute VFS path.
 *
 * @return Ok(true) on success or a typed VFS error.
 * @pre Both paths must resolve to the same mounted backend.
 * @post On success, @p old_path no longer names the object and @p new_path
 *       does.
 */
extern n00b_result_t(bool)
n00b_vfs_rename(n00b_vfs_t *vfs, n00b_string_t *old_path,
                n00b_string_t *new_path);
