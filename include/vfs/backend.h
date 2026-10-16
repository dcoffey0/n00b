/**
 * @file backend.h
 * @brief Storage backend vtable and instance types.
 *
 * A backend provides stateless, eventually consistent, atomic
 * operations over a storage medium (local directory, S3, in-memory,
 * etc.).  Each backend implements @c n00b_vfs_backend_ops_t and is
 * instantiated as an @c n00b_vfs_backend_t with a root path/prefix.
 *
 * All operations receive an opaque @c ctx pointer returned by
 * @c init().  Backends must not hold open file handles or other
 * stateful resources across calls — each operation is self-contained.
 */
#pragma once

#include "vfs/types.h"

// ============================================================================
// Forward declarations
// ============================================================================

typedef struct n00b_vfs_backend     n00b_vfs_backend_t;
typedef struct n00b_vfs_backend_ops n00b_vfs_backend_ops_t;

// ============================================================================
// Backend operations vtable
// ============================================================================

/**
 * @brief Vtable for storage backend operations.
 *
 * All data operations return @c n00b_result_t and use VFS error codes.
 * Backends that do not support an optional operation (range read, rename,
 * hard link) should set the function pointer to nullptr and return false
 * from the corresponding @c supports_* probe.
 */
struct n00b_vfs_backend_ops {

    /** @brief Human-readable backend name (e.g. "local", "s3", "memory"). */
    n00b_string_t *(*name)(void);

    /**
     * @brief Initialize backend-specific context.
     * @param be  The backend instance (root, allocator are set).
     * @return    Opaque context pointer passed to all operations.
     */
    void *(*init)(n00b_vfs_backend_t *be);

    /** @brief Tear down backend context and release resources. */
    void (*cleanup)(void *ctx);

    // ── Core object operations ─────────────────────────────────────

    /** @brief Retrieve entire object. */
    n00b_result_t(n00b_buffer_t *) (*get)(void *ctx, n00b_string_t *path);

    /**
     * @brief Retrieve a byte range.
     *
     * May be nullptr if @c supports_range_read returns false.
     */
    n00b_result_t(n00b_buffer_t *) (*get_range)(void *ctx,
                                                 n00b_string_t *path,
                                                 uint64_t       offset,
                                                 uint64_t       length);

    /** @brief Store object bytes atomically. Replaces if exists. */
    n00b_result_t(bool) (*put)(void *ctx, n00b_string_t *path,
                                n00b_buffer_t *data);

    /**
     * @brief Store object bytes only when the object does not already exist.
     *
     * Backends should make this atomic for their substrate when possible.
     * Existing objects return @c N00B_VFS_ERR_EXISTS.
     */
    n00b_result_t(bool) (*put_if_absent)(void          *ctx,
                                          n00b_string_t *path,
                                          n00b_buffer_t *data);

    /** @brief Delete an object. */
    n00b_result_t(bool) (*del)(void *ctx, n00b_string_t *path);

    /** @brief Stat an object (metadata without body). */
    n00b_result_t(n00b_vfs_obj_stat_t) (*stat)(void *ctx,
                                                n00b_string_t *path);

    /**
     * @brief List objects under a prefix.
     * @param continuation  Opaque token from a prior call (nullptr for first).
     * @param max_keys      Maximum entries to return.
     */
    n00b_result_t(n00b_vfs_list_result_t *) (*list)(void          *ctx,
                                                     n00b_string_t *prefix,
                                                     n00b_string_t *continuation,
                                                     uint32_t       max_keys);

    /** @brief Rename/move an object. Atomic within a single backend. */
    n00b_result_t(bool) (*rename)(void *ctx, n00b_string_t *old_path,
                                   n00b_string_t *new_path);

    /** @brief Create a directory marker. */
    n00b_result_t(bool) (*mkdir)(void *ctx, n00b_string_t *path);

    /** @brief Durably sync one object or directory when the backend supports it. */
    n00b_result_t(bool) (*sync)(void *ctx, n00b_string_t *path);

    /**
     * @brief Return a materialized local filesystem path for an object.
     *
     * Non-local backends leave this null. Callers use this only for APIs that
     * require a real path, such as direct mmap of immutable shard files.
     */
    n00b_result_t(n00b_string_t *) (*local_path)(void *ctx, n00b_string_t *path);

    // ── Capability probes ──────────────────────────────────────────

    bool (*supports_range_read)(void *ctx);
    bool (*supports_rename)(void *ctx);
    bool (*supports_link)(void *ctx);
    bool (*supports_durable_sync)(void *ctx);

    /**
     * @brief Whether each n00b_vfs_list_result_t name string returned by @c
     *        list is freshly owned by the result (vs. borrowed from persistent
     *        backend state).
     *
     * When true, n00b_vfs_readdir may free those name strings after it deep-
     * clones the result into the caller's allocator. The local and s3 backends
     * mint fresh name strings per listing (true); the memory backend borrows
     * its persistent metadata dict keys, which must NOT be freed (false, the
     * default for a zero-initialized ops table).
     */
    bool list_result_owns_strings;

    /**
     * @brief Append bytes to the end of an object, creating it when absent.
     *
     * Optional. Without it, n00b_vfs_append rewrites the whole object
     * through get and put.
     */
    n00b_result_t(bool) (*append)(void          *ctx,
                                   n00b_string_t *path,
                                   n00b_buffer_t *data);

    // ── Optional: hard link (local backend only) ───────────────────

    n00b_result_t(bool) (*link)(void *ctx, n00b_string_t *target,
                                 n00b_string_t *link_path);
};

// ============================================================================
// Backend instance
// ============================================================================

/**
 * @brief A configured storage backend.
 *
 * Created by a backend-specific constructor (e.g.
 * @c n00b_vfs_backend_memory_new).  The @c ctx is populated by
 * calling @c ops->init().
 */
struct n00b_vfs_backend {
    const n00b_vfs_backend_ops_t *ops;
    void                         *ctx;
    n00b_string_t                *root;
    n00b_allocator_t             *allocator;
};

// ============================================================================
// Backend lifecycle helpers
// ============================================================================

/**
 * @brief Initialize a backend: call ops->init and store the context.
 * @pre  be->ops and be->root are set.
 * @post be->ctx is populated.
 */
extern n00b_result_t(bool) n00b_vfs_backend_init(n00b_vfs_backend_t *be);

/**
 * @brief Tear down a backend: call ops->cleanup.
 * @post be->ctx is nullptr.
 */
extern void n00b_vfs_backend_cleanup(n00b_vfs_backend_t *be);
