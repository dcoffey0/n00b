/**
 * NFSv3 userspace server frontend for Linux.
 *
 * Implements a minimal NFSv3 subset over TCP using hand-rolled XDR/RPC.
 * The kernel NFS client mounts against this server on localhost.
 *
 * Only compiled on Linux (#if defined(__linux__) in meson.build).
 */

#include "vfs/frontend_nfs.h"
#include "core/eintr.h"
#include "internal/vfs/nfs_xdr.h"
#include "internal/vfs/nfs_rpc.h"
#include "core/alloc.h"
#include "core/thread.h"
#include "core/runtime.h"
#include "core/condition.h"
#include "conduit/conduit.h"
#include "conduit/socket.h"
#include "conduit/inbox.h"

#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// ============================================================================
// NFS3 status codes
// ============================================================================

#define NFS3_OK             0
#define NFS3ERR_PERM        1
#define NFS3ERR_NOENT       2
#define NFS3ERR_IO          5
#define NFS3ERR_NXIO        6
#define NFS3ERR_ACCES       13
#define NFS3ERR_EXIST       17
#define NFS3ERR_XDEV        18
#define NFS3ERR_NODEV       19
#define NFS3ERR_NOTDIR      20
#define NFS3ERR_ISDIR       21
#define NFS3ERR_INVAL       22
#define NFS3ERR_FBIG        27
#define NFS3ERR_NOSPC       28
#define NFS3ERR_ROFS        30
#define NFS3ERR_NAMETOOLONG 63
#define NFS3ERR_NOTEMPTY    66
#define NFS3ERR_STALE       70
#define NFS3ERR_BADHANDLE   10001
#define NFS3ERR_NOTSUPP     10004
#define NFS3ERR_SERVERFAULT 10006

// NFS3 procedure numbers
#define NFSPROC3_NULL        0
#define NFSPROC3_GETATTR     1
#define NFSPROC3_SETATTR     2
#define NFSPROC3_LOOKUP      3
#define NFSPROC3_ACCESS      4
#define NFSPROC3_READ        6
#define NFSPROC3_WRITE       7
#define NFSPROC3_CREATE      8
#define NFSPROC3_MKDIR       9
#define NFSPROC3_REMOVE      12
#define NFSPROC3_RMDIR       13
#define NFSPROC3_RENAME      14
#define NFSPROC3_READDIR     16
#define NFSPROC3_READDIRPLUS 17
#define NFSPROC3_FSSTAT      18
#define NFSPROC3_FSINFO      19
#define NFSPROC3_PATHCONF    20
#define NFSPROC3_COMMIT      21

// MOUNT procedure numbers
#define MOUNTPROC3_NULL 0
#define MOUNTPROC3_MNT  1
#define MOUNTPROC3_UMNT 3

// NFS file handle size
#define NFS_FH_SIZE 32

// ============================================================================
// File handle encoding
//
// File handles are 32-byte opaque tokens.  We store a VFS path hash
// and maintain a handle→path mapping table.
// ============================================================================

typedef struct {
    uint64_t       id;
    n00b_string_t *path;
    n00b_vfs_fh_t  vfs_rw_fh;   /**< Cached VFS handle open for RW (0 = none). */
} nfs_fh_entry_t;

typedef struct {
    n00b_vfs_frontend_t              *frontend;
    n00b_vfs_t                       *vfs;
    n00b_conduit_t                   *conduit;
    n00b_conduit_listener_t          *listener;
    n00b_conduit_sock_accept_inbox_t *accept_inbox;
    uint16_t                          port;
    n00b_thread_t                    *thread;

    nfs_fh_entry_t     **fh_table;
    uint32_t             fh_count;
    uint32_t             fh_cap;
    uint64_t             next_fh_id;
} nfs_ctx_t;

// ============================================================================
// File handle helpers
// ============================================================================

static void
fh_encode(uint64_t id, uint8_t *out)
{
    memset(out, 0, NFS_FH_SIZE);
    for (int i = 7; i >= 0; i--) {
        out[i] = (uint8_t)(id & 0xFF);
        id >>= 8;
    }
}

static uint64_t
fh_decode(const uint8_t *data)
{
    uint64_t id = 0;
    for (int i = 0; i < 8; i++) {
        id = (id << 8) | data[i];
    }
    return id;
}

static uint64_t
fh_alloc(nfs_ctx_t *nc, n00b_string_t *path)
{
    // Check existing.
    for (uint32_t i = 0; i < nc->fh_count; i++) {
        if (nc->fh_table[i]->path->u8_bytes == path->u8_bytes
            && memcmp(nc->fh_table[i]->path->data, path->data,
                      path->u8_bytes) == 0) {
            return nc->fh_table[i]->id;
        }
    }

    // Allocate new.
    if (nc->fh_count >= nc->fh_cap) {
        uint32_t new_cap = nc->fh_cap * 2;
        nfs_fh_entry_t **new_t = n00b_alloc_array(nfs_fh_entry_t *, new_cap);
        memcpy(new_t, nc->fh_table, nc->fh_count * sizeof(nfs_fh_entry_t *));
        nc->fh_table = new_t;
        nc->fh_cap   = new_cap;
    }

    nfs_fh_entry_t *e = n00b_alloc(nfs_fh_entry_t);
    e->id   = nc->next_fh_id++;
    e->path = path;
    nc->fh_table[nc->fh_count++] = e;
    return e->id;
}

static n00b_string_t *
fh_lookup(nfs_ctx_t *nc, uint64_t id)
{
    for (uint32_t i = 0; i < nc->fh_count; i++) {
        if (nc->fh_table[i]->id == id) {
            return nc->fh_table[i]->path;
        }
    }
    return nullptr;
}

/**
 * Get a cached VFS read-write handle for an NFS file handle.
 * Opens the file if not already open.  Returns N00B_VFS_FH_INVALID on failure.
 */
static n00b_vfs_fh_t
nfs_get_vfs_fh(nfs_ctx_t *nc, uint64_t nfs_id)
{
    for (uint32_t i = 0; i < nc->fh_count; i++) {
        if (nc->fh_table[i]->id == nfs_id) {
            nfs_fh_entry_t *e = nc->fh_table[i];
            if (e->vfs_rw_fh != N00B_VFS_FH_INVALID) {
                return e->vfs_rw_fh;
            }
            // Open RW+CREATE for this path.
            n00b_result_t(n00b_vfs_fh_t) r =
                n00b_vfs_open(nc->vfs, e->path, N00B_VFS_O_RW);
            if (n00b_result_is_ok(r)) {
                e->vfs_rw_fh = n00b_result_get(r);
                return e->vfs_rw_fh;
            }
            return N00B_VFS_FH_INVALID;
        }
    }
    return N00B_VFS_FH_INVALID;
}

/**
 * Get a cached VFS read-only handle.  Tries the RW handle first;
 * if not available, opens read-only (doesn't cache since read-only
 * handles are cheap and stateless from the NFS perspective).
 */
static n00b_vfs_fh_t
nfs_get_vfs_fh_read(nfs_ctx_t *nc, uint64_t nfs_id, n00b_string_t *path)
{
    // Try the cached RW handle first.
    n00b_vfs_fh_t fh = nfs_get_vfs_fh(nc, nfs_id);
    if (fh != N00B_VFS_FH_INVALID) {
        return fh;
    }
    // Fall back to opening read-only (not cached).
    n00b_result_t(n00b_vfs_fh_t) r =
        n00b_vfs_open(nc->vfs, path, N00B_VFS_O_R);
    if (n00b_result_is_ok(r)) {
        return n00b_result_get(r);
    }
    return N00B_VFS_FH_INVALID;
}

// ============================================================================
// Error mapping
// ============================================================================

static uint32_t
vfs_err_to_nfs3(n00b_err_t err)
{
    switch (err) {
    case N00B_VFS_ERR_NOT_FOUND:      return NFS3ERR_NOENT;
    case N00B_VFS_ERR_EXISTS:         return NFS3ERR_EXIST;
    case N00B_VFS_ERR_IS_DIR:         return NFS3ERR_ISDIR;
    case N00B_VFS_ERR_NOT_DIR:        return NFS3ERR_NOTDIR;
    case N00B_VFS_ERR_NOT_EMPTY:      return NFS3ERR_NOTEMPTY;
    case N00B_VFS_ERR_PERMISSION:     return NFS3ERR_ACCES;
    case N00B_VFS_ERR_HOOK_DENIED:    return NFS3ERR_ACCES;
    case N00B_VFS_ERR_READ_ONLY:      return NFS3ERR_ROFS;
    case N00B_VFS_ERR_NO_SPACE:       return NFS3ERR_NOSPC;
    case N00B_VFS_ERR_CROSS_DEVICE:   return NFS3ERR_XDEV;
    case N00B_VFS_ERR_NOT_SUPPORTED:  return NFS3ERR_NOTSUPP;
    case N00B_VFS_ERR_STALE:          return NFS3ERR_STALE;
    case N00B_VFS_ERR_INVALID_HANDLE: return NFS3ERR_BADHANDLE;
    default:                          return NFS3ERR_IO;
    }
}

// ============================================================================
// XDR helpers for NFS types
// ============================================================================

static bool
xdr_put_fattr3(n00b_xdr_t *x, n00b_vfs_obj_stat_t *st, uint64_t fileid)
{
    uint32_t ftype;
    uint32_t type_bits;
    switch (st->kind) {
    case N00B_VFS_OBJ_DIR:     ftype = 2; type_bits = 040000;  break;
    case N00B_VFS_OBJ_SYMLINK: ftype = 5; type_bits = 0120000; break;
    default:                    ftype = 1; type_bits = 0100000; break;
    }

    n00b_xdr_put_u32(x, ftype);
    n00b_xdr_put_u32(x, (st->mode & 07777) | type_bits);  // mode
    n00b_xdr_put_u32(x, 1);            // nlink
    n00b_xdr_put_u32(x, 0);            // uid
    n00b_xdr_put_u32(x, 0);            // gid
    n00b_xdr_put_u64(x, st->size);     // size
    n00b_xdr_put_u64(x, st->size);     // used
    n00b_xdr_put_u32(x, 0);            // rdev.specdata1
    n00b_xdr_put_u32(x, 0);            // rdev.specdata2
    n00b_xdr_put_u64(x, 1);            // fsid (constant for our VFS)
    n00b_xdr_put_u64(x, fileid);       // fileid
    // atime
    n00b_xdr_put_u32(x, (uint32_t)(st->atime_ns / 1000000000ULL));
    n00b_xdr_put_u32(x, (uint32_t)(st->atime_ns % 1000000000ULL));
    // mtime
    n00b_xdr_put_u32(x, (uint32_t)(st->mtime_ns / 1000000000ULL));
    n00b_xdr_put_u32(x, (uint32_t)(st->mtime_ns % 1000000000ULL));
    // ctime
    n00b_xdr_put_u32(x, (uint32_t)(st->ctime_ns / 1000000000ULL));
    return n00b_xdr_put_u32(x, (uint32_t)(st->ctime_ns % 1000000000ULL));
}

static bool
xdr_put_post_op_attr(n00b_xdr_t *x, nfs_ctx_t *nc, n00b_string_t *path)
{
    if (path == nullptr) {
        return n00b_xdr_put_bool(x, false);
    }

    n00b_result_t(n00b_vfs_obj_stat_t) sr = n00b_vfs_stat(nc->vfs, path);
    if (n00b_result_is_err(sr)) {
        return n00b_xdr_put_bool(x, false);
    }

    n00b_vfs_obj_stat_t st = n00b_result_get(sr);
    uint64_t fid = fh_alloc(nc, path);
    n00b_xdr_put_bool(x, true);
    return xdr_put_fattr3(x, &st, fid);
}

// ============================================================================
// RPC reply helpers
// ============================================================================

static void
write_rpc_reply_hdr(n00b_xdr_t *x, uint32_t xid)
{
    n00b_xdr_put_u32(x, xid);
    n00b_xdr_put_u32(x, N00B_RPC_REPLY);
    n00b_xdr_put_u32(x, N00B_RPC_MSG_ACCEPTED);
    // auth verifier: AUTH_NONE
    n00b_xdr_put_u32(x, 0);  // flavor
    n00b_xdr_put_u32(x, 0);  // body length
    n00b_xdr_put_u32(x, N00B_RPC_SUCCESS);
}

// The client socket's send and receive timeout. On Linux a signal interrupts
// a blocking call on the socket even under SA_RESTART, where macOS restarts
// it, so each call retries until the timeout would have passed.
#define NFS_IO_TIMEOUT_SEC 5
#define NFS_IO_DEADLINE()                                                     \
    (n00b_ns_timestamp() + (uint64_t)NFS_IO_TIMEOUT_SEC * N00B_NS_PER_SEC)

static bool
send_record(int fd, uint8_t *data, uint32_t len)
{
    // TCP record marking: 4-byte header with last-fragment bit.
    uint32_t rm = n00b_rpc_rm_encode(len, true);
    uint8_t  hdr[4] = {
        (uint8_t)(rm >> 24), (uint8_t)(rm >> 16),
        (uint8_t)(rm >> 8),  (uint8_t)(rm)
    };

    // Send header + payload.
    if (n00b_retry_eintr_until(NFS_IO_DEADLINE(), write(fd, hdr, 4)) != 4) {
        return false;
    }

    size_t sent = 0;
    while (sent < len) {
        ssize_t w = n00b_retry_eintr_until(NFS_IO_DEADLINE(),
                                           write(fd, data + sent, len - sent));
        if (w <= 0) return false;
        sent += (size_t)w;
    }
    return true;
}

// ============================================================================
// NFS3 procedure handlers
// ============================================================================

static void
handle_null(nfs_ctx_t *nc, int client_fd, uint32_t xid)
{
    (void)nc;
    uint8_t    buf[64];
    n00b_xdr_t x;
    n00b_xdr_init(&x, buf, sizeof(buf));
    write_rpc_reply_hdr(&x, xid);
    send_record(client_fd, buf, x.pos);
}

static void
handle_getattr(nfs_ctx_t *nc, int client_fd, uint32_t xid, n00b_xdr_t *args)
{
    uint8_t fh_data[NFS_FH_SIZE];
    const uint8_t *fh_raw;
    uint32_t fh_len;

    n00b_xdr_get_opaque(args, &fh_raw, &fh_len);
    memset(fh_data, 0, NFS_FH_SIZE);
    if (fh_len > NFS_FH_SIZE) fh_len = NFS_FH_SIZE;
    memcpy(fh_data, fh_raw, fh_len);

    uint64_t id = fh_decode(fh_data);
    n00b_string_t *path = fh_lookup(nc, id);

    uint8_t    buf[512];
    n00b_xdr_t x;
    n00b_xdr_init(&x, buf, sizeof(buf));
    write_rpc_reply_hdr(&x, xid);

    if (path == nullptr) {
        n00b_xdr_put_u32(&x, NFS3ERR_STALE);
    }
    else {
        n00b_result_t(n00b_vfs_obj_stat_t) sr = n00b_vfs_stat(nc->vfs, path);
        if (n00b_result_is_err(sr)) {
            n00b_xdr_put_u32(&x, vfs_err_to_nfs3(n00b_result_get_err(sr)));
        }
        else {
            n00b_vfs_obj_stat_t st = n00b_result_get(sr);
            n00b_xdr_put_u32(&x, NFS3_OK);
            xdr_put_fattr3(&x, &st, id);
        }
    }

    send_record(client_fd, buf, x.pos);
}

static void
handle_lookup(nfs_ctx_t *nc, int client_fd, uint32_t xid, n00b_xdr_t *args)
{
    // dir filehandle
    const uint8_t *fh_raw;
    uint32_t fh_len;
    n00b_xdr_get_opaque(args, &fh_raw, &fh_len);
    uint8_t fh_data[NFS_FH_SIZE] = {};
    if (fh_len > NFS_FH_SIZE) fh_len = NFS_FH_SIZE;
    memcpy(fh_data, fh_raw, fh_len);

    // filename
    const uint8_t *name_raw;
    uint32_t name_len;
    n00b_xdr_get_opaque(args, &name_raw, &name_len);

    uint64_t dir_id = fh_decode(fh_data);
    n00b_string_t *dir_path = fh_lookup(nc, dir_id);

    uint8_t    buf[512];
    n00b_xdr_t x;
    n00b_xdr_init(&x, buf, sizeof(buf));
    write_rpc_reply_hdr(&x, xid);

    if (dir_path == nullptr) {
        n00b_xdr_put_u32(&x, NFS3ERR_STALE);
        send_record(client_fd, buf, x.pos);
        return;
    }

    // Build full path: dir + "/" + name.
    size_t dlen = dir_path->u8_bytes;
    size_t total = dlen + 1 + name_len;
    char *full = n00b_alloc_array(char, total + 1);

    memcpy(full, dir_path->data, dlen);
    if (dlen > 0 && full[dlen - 1] != '/') {
        full[dlen] = '/';
        memcpy(full + dlen + 1, name_raw, name_len);
        full[dlen + 1 + name_len] = '\0';
        total = dlen + 1 + name_len;
    }
    else {
        memcpy(full + dlen, name_raw, name_len);
        full[dlen + name_len] = '\0';
        total = dlen + name_len;
    }

    n00b_string_t *child_path = n00b_string_from_raw(full, (int64_t)total);

    // Stat to verify existence.
    n00b_result_t(n00b_vfs_obj_stat_t) sr = n00b_vfs_stat(nc->vfs, child_path);
    if (n00b_result_is_err(sr)) {
        n00b_xdr_put_u32(&x, vfs_err_to_nfs3(n00b_result_get_err(sr)));
        xdr_put_post_op_attr(&x, nc, dir_path);
        send_record(client_fd, buf, x.pos);
        return;
    }

    n00b_vfs_obj_stat_t st = n00b_result_get(sr);
    uint64_t child_id = fh_alloc(nc, child_path);

    n00b_xdr_put_u32(&x, NFS3_OK);

    // child file handle
    uint8_t child_fh[NFS_FH_SIZE];
    fh_encode(child_id, child_fh);
    n00b_xdr_put_opaque(&x, child_fh, NFS_FH_SIZE);

    // child attributes (post_op_attr)
    n00b_xdr_put_bool(&x, true);
    xdr_put_fattr3(&x, &st, child_id);

    // dir attributes (post_op_attr)
    xdr_put_post_op_attr(&x, nc, dir_path);

    send_record(client_fd, buf, x.pos);
}

static void
handle_fsinfo(nfs_ctx_t *nc, int client_fd, uint32_t xid, n00b_xdr_t *args)
{
    (void)args;

    uint8_t    buf[512];
    n00b_xdr_t x;
    n00b_xdr_init(&x, buf, sizeof(buf));
    write_rpc_reply_hdr(&x, xid);

    n00b_xdr_put_u32(&x, NFS3_OK);
    xdr_put_post_op_attr(&x, nc, r"/");

    n00b_xdr_put_u32(&x, 65536);   // rtmax
    n00b_xdr_put_u32(&x, 65536);   // rtpref
    n00b_xdr_put_u32(&x, 1);       // rtmult
    n00b_xdr_put_u32(&x, 65536);   // wtmax
    n00b_xdr_put_u32(&x, 65536);   // wtpref
    n00b_xdr_put_u32(&x, 1);       // wtmult
    n00b_xdr_put_u32(&x, 65536);   // dtpref
    n00b_xdr_put_u64(&x, (uint64_t)1 << 40);  // maxfilesize
    n00b_xdr_put_u32(&x, 1);       // time_delta sec
    n00b_xdr_put_u32(&x, 0);       // time_delta nsec
    n00b_xdr_put_u32(&x, 0x001B);  // properties

    send_record(client_fd, buf, x.pos);
}

static void
handle_mount(nfs_ctx_t *nc, int client_fd, uint32_t xid, n00b_xdr_t *args)
{
    (void)args;

    // Always return success with root file handle.
    uint64_t root_id = fh_alloc(nc, r"/");
    uint8_t root_fh[NFS_FH_SIZE];
    fh_encode(root_id, root_fh);

    uint8_t    buf[512];
    n00b_xdr_t x;
    n00b_xdr_init(&x, buf, sizeof(buf));
    write_rpc_reply_hdr(&x, xid);

    n00b_xdr_put_u32(&x, 0);  // MNT3_OK
    n00b_xdr_put_opaque(&x, root_fh, NFS_FH_SIZE);
    n00b_xdr_put_u32(&x, 0);  // auth flavors count

    send_record(client_fd, buf, x.pos);
}

// ============================================================================
// Common helpers for procedure handlers
// ============================================================================

/**
 * Decode a file handle from XDR args and look up the VFS path.
 * Returns nullptr on failure (and writes NFS3ERR_STALE reply).
 */
static n00b_string_t *
decode_fh_path(nfs_ctx_t *nc, n00b_xdr_t *args, uint64_t *out_id)
{
    const uint8_t *fh_raw;
    uint32_t       fh_len;
    n00b_xdr_get_opaque(args, &fh_raw, &fh_len);

    uint8_t fh_data[NFS_FH_SIZE] = {};
    if (fh_len > NFS_FH_SIZE) fh_len = NFS_FH_SIZE;
    memcpy(fh_data, fh_raw, fh_len);

    uint64_t id = fh_decode(fh_data);
    if (out_id) *out_id = id;
    return fh_lookup(nc, id);
}

/**
 * Build child path from directory path + name.
 */
static n00b_string_t *
build_child_path(n00b_string_t *dir, const uint8_t *name, uint32_t name_len)
{
    size_t dlen = dir->u8_bytes;
    size_t total = dlen + 1 + name_len;
    char  *buf = n00b_alloc_array(char, total + 1);

    memcpy(buf, dir->data, dlen);
    if (dlen > 0 && buf[dlen - 1] != '/') {
        buf[dlen] = '/';
        memcpy(buf + dlen + 1, name, name_len);
        buf[dlen + 1 + name_len] = '\0';
        total = dlen + 1 + name_len;
    }
    else {
        memcpy(buf + dlen, name, name_len);
        buf[dlen + name_len] = '\0';
        total = dlen + name_len;
    }

    return n00b_string_from_raw(buf, (int64_t)total);
}

// ============================================================================
// NFS3 procedure: ACCESS
// ============================================================================

static void
handle_access(nfs_ctx_t *nc, int client_fd, uint32_t xid, n00b_xdr_t *args)
{
    n00b_string_t *path = decode_fh_path(nc, args, nullptr);

    uint32_t access_bits;
    n00b_xdr_get_u32(args, &access_bits);

    uint8_t    buf[512];
    n00b_xdr_t x;
    n00b_xdr_init(&x, buf, sizeof(buf));
    write_rpc_reply_hdr(&x, xid);

    if (path == nullptr) {
        n00b_xdr_put_u32(&x, NFS3ERR_STALE);
        send_record(client_fd, buf, x.pos);
        return;
    }

    n00b_xdr_put_u32(&x, NFS3_OK);
    xdr_put_post_op_attr(&x, nc, path);
    n00b_xdr_put_u32(&x, access_bits);  // Grant all requested access.

    send_record(client_fd, buf, x.pos);
}

// ============================================================================
// NFS3 procedure: SETATTR (minimal — only handles size truncation)
// ============================================================================

static void
handle_setattr(nfs_ctx_t *nc, int client_fd, uint32_t xid, n00b_xdr_t *args)
{
    n00b_string_t *path = decode_fh_path(nc, args, nullptr);

    // Parse sattr3: each field has a bool "set" flag followed by the value.
    // mode
    bool set_mode;
    n00b_xdr_get_bool(args, &set_mode);
    if (set_mode) { uint32_t mode; n00b_xdr_get_u32(args, &mode); (void)mode; }
    // uid
    bool set_uid;
    n00b_xdr_get_bool(args, &set_uid);
    if (set_uid) { uint32_t uid; n00b_xdr_get_u32(args, &uid); (void)uid; }
    // gid
    bool set_gid;
    n00b_xdr_get_bool(args, &set_gid);
    if (set_gid) { uint32_t gid; n00b_xdr_get_u32(args, &gid); (void)gid; }
    // size
    bool set_size;
    n00b_xdr_get_bool(args, &set_size);
    uint64_t new_size = 0;
    if (set_size) { n00b_xdr_get_u64(args, &new_size); }
    // atime: set_it enum (0=don't, 1=server_time, 2=client_time)
    uint32_t set_atime;
    n00b_xdr_get_u32(args, &set_atime);
    if (set_atime == 2) { args->pos += 8; }  // skip nfstime3
    // mtime: same
    uint32_t set_mtime;
    n00b_xdr_get_u32(args, &set_mtime);
    if (set_mtime == 2) { args->pos += 8; }

    uint8_t    buf[512];
    n00b_xdr_t x;
    n00b_xdr_init(&x, buf, sizeof(buf));
    write_rpc_reply_hdr(&x, xid);

    if (path == nullptr) {
        n00b_xdr_put_u32(&x, NFS3ERR_STALE);
        send_record(client_fd, buf, x.pos);
        return;
    }

    // Handle size truncation.
    if (set_size) {
        n00b_result_t(bool) tr = n00b_vfs_truncate(nc->vfs, path, new_size);
        if (n00b_result_is_err(tr)) {
            n00b_xdr_put_u32(&x, vfs_err_to_nfs3(n00b_result_get_err(tr)));
            n00b_xdr_put_bool(&x, false);
            xdr_put_post_op_attr(&x, nc, path);
            send_record(client_fd, buf, x.pos);
            return;
        }
    }

    n00b_xdr_put_u32(&x, NFS3_OK);
    // wcc_data: pre_op_attr (false) + post_op_attr
    n00b_xdr_put_bool(&x, false);
    xdr_put_post_op_attr(&x, nc, path);

    send_record(client_fd, buf, x.pos);
}

// ============================================================================
// NFS3 procedure: READ
// ============================================================================

#define NFS_READ_BUF_SIZE (65536 + 512)

static void
handle_read(nfs_ctx_t *nc, int client_fd, uint32_t xid, n00b_xdr_t *args)
{
    uint64_t nfs_id;
    n00b_string_t *path = decode_fh_path(nc, args, &nfs_id);

    uint64_t offset;
    uint32_t count;
    n00b_xdr_get_u64(args, &offset);
    n00b_xdr_get_u32(args, &count);

    uint8_t   *buf = n00b_alloc_array(uint8_t, NFS_READ_BUF_SIZE);
    n00b_xdr_t x;
    n00b_xdr_init(&x, buf, NFS_READ_BUF_SIZE);
    write_rpc_reply_hdr(&x, xid);

    if (path == nullptr) {
        n00b_xdr_put_u32(&x, NFS3ERR_STALE);
        send_record(client_fd, buf, x.pos);
        return;
    }

    n00b_vfs_fh_t fh = nfs_get_vfs_fh_read(nc, nfs_id, path);
    if (fh == N00B_VFS_FH_INVALID) {
        n00b_xdr_put_u32(&x, NFS3ERR_IO);
        xdr_put_post_op_attr(&x, nc, path);
        send_record(client_fd, buf, x.pos);
        return;
    }

    (void)n00b_vfs_seek(nc->vfs, fh, (int64_t)offset, 0);

    n00b_result_t(n00b_buffer_t *) rr = n00b_vfs_read(nc->vfs, fh, count);

    if (n00b_result_is_err(rr)) {
        n00b_xdr_put_u32(&x, vfs_err_to_nfs3(n00b_result_get_err(rr)));
        xdr_put_post_op_attr(&x, nc, path);
        send_record(client_fd, buf, x.pos);
        return;
    }

    n00b_buffer_t *data = n00b_result_get(rr);
    int64_t        data_len;
    char          *data_ptr = n00b_buffer_to_c(data, &data_len);

    bool eof = false;
    n00b_result_t(n00b_vfs_obj_stat_t) sr = n00b_vfs_stat(nc->vfs, path);
    if (n00b_result_is_ok(sr)) {
        eof = (offset + (uint64_t)data_len) >= n00b_result_get(sr).size;
    }

    n00b_xdr_put_u32(&x, NFS3_OK);
    xdr_put_post_op_attr(&x, nc, path);
    n00b_xdr_put_u32(&x, (uint32_t)data_len);
    n00b_xdr_put_bool(&x, eof);
    n00b_xdr_put_opaque(&x, data_ptr, (uint32_t)data_len);

    send_record(client_fd, buf, x.pos);
}

// ============================================================================
// NFS3 procedure: WRITE
// ============================================================================

static void
handle_write(nfs_ctx_t *nc, int client_fd, uint32_t xid, n00b_xdr_t *args)
{
    uint64_t nfs_id;
    n00b_string_t *path = decode_fh_path(nc, args, &nfs_id);

    uint64_t offset;
    uint32_t count, stable;
    n00b_xdr_get_u64(args, &offset);
    n00b_xdr_get_u32(args, &count);
    n00b_xdr_get_u32(args, &stable);

    const uint8_t *data_raw;
    uint32_t       data_len;
    n00b_xdr_get_opaque(args, &data_raw, &data_len);

    uint8_t    buf[512];
    n00b_xdr_t x;
    n00b_xdr_init(&x, buf, sizeof(buf));
    write_rpc_reply_hdr(&x, xid);

    if (path == nullptr) {
        n00b_xdr_put_u32(&x, NFS3ERR_STALE);
        send_record(client_fd, buf, x.pos);
        return;
    }

    n00b_vfs_fh_t fh = nfs_get_vfs_fh(nc, nfs_id);
    if (fh == N00B_VFS_FH_INVALID) {
        n00b_xdr_put_u32(&x, NFS3ERR_IO);
        n00b_xdr_put_bool(&x, false);
        xdr_put_post_op_attr(&x, nc, path);
        send_record(client_fd, buf, x.pos);
        return;
    }

    (void)n00b_vfs_seek(nc->vfs, fh, (int64_t)offset, 0);

    n00b_buffer_t *wbuf = n00b_buffer_from_bytes((char *)data_raw,
                                                  (int64_t)data_len);
    n00b_result_t(uint64_t) wr = n00b_vfs_write(nc->vfs, fh, wbuf);

    // Flush to backend if FILE_SYNC requested (stable == 2).
    if (n00b_result_is_ok(wr) && stable == 2) {
        (void)n00b_vfs_flush(nc->vfs, fh);
    }

    if (n00b_result_is_err(wr)) {
        n00b_xdr_put_u32(&x, vfs_err_to_nfs3(n00b_result_get_err(wr)));
        n00b_xdr_put_bool(&x, false);
        xdr_put_post_op_attr(&x, nc, path);
        send_record(client_fd, buf, x.pos);
        return;
    }

    n00b_xdr_put_u32(&x, NFS3_OK);
    n00b_xdr_put_bool(&x, false);  // pre_op_attr
    xdr_put_post_op_attr(&x, nc, path);
    n00b_xdr_put_u32(&x, (uint32_t)n00b_result_get(wr));  // count
    n00b_xdr_put_u32(&x, 2);  // FILE_SYNC committed
    n00b_xdr_put_u32(&x, 0);  // write verifier
    n00b_xdr_put_u32(&x, 0);

    send_record(client_fd, buf, x.pos);
}

// ============================================================================
// NFS3 procedure: CREATE
// ============================================================================

static void
handle_create(nfs_ctx_t *nc, int client_fd, uint32_t xid, n00b_xdr_t *args)
{
    n00b_string_t *dir_path = decode_fh_path(nc, args, nullptr);

    const uint8_t *name_raw;
    uint32_t       name_len;
    n00b_xdr_get_opaque(args, &name_raw, &name_len);

    // createhow3: mode (UNCHECKED=0, GUARDED=1, EXCLUSIVE=2)
    uint32_t create_mode;
    n00b_xdr_get_u32(args, &create_mode);
    // Skip sattr3 for simplicity.

    uint8_t    buf[512];
    n00b_xdr_t x;
    n00b_xdr_init(&x, buf, sizeof(buf));
    write_rpc_reply_hdr(&x, xid);

    if (dir_path == nullptr) {
        n00b_xdr_put_u32(&x, NFS3ERR_STALE);
        send_record(client_fd, buf, x.pos);
        return;
    }

    n00b_string_t *child_path = build_child_path(dir_path, name_raw, name_len);

    // Create via open + close.
    uint32_t flags = N00B_VFS_O_W;
    if (create_mode == 1) flags |= N00B_VFS_OPEN_EXCL;

    n00b_result_t(n00b_vfs_fh_t) ofh = n00b_vfs_open(nc->vfs, child_path, flags);
    if (n00b_result_is_err(ofh)) {
        n00b_xdr_put_u32(&x, vfs_err_to_nfs3(n00b_result_get_err(ofh)));
        // wcc_data
        n00b_xdr_put_bool(&x, false);
        xdr_put_post_op_attr(&x, nc, dir_path);
        send_record(client_fd, buf, x.pos);
        return;
    }

    (void)n00b_vfs_close(nc->vfs, n00b_result_get(ofh));

    uint64_t child_id = fh_alloc(nc, child_path);
    uint8_t  child_fh[NFS_FH_SIZE];
    fh_encode(child_id, child_fh);

    n00b_xdr_put_u32(&x, NFS3_OK);
    // post_op_fh3: true + handle
    n00b_xdr_put_bool(&x, true);
    n00b_xdr_put_opaque(&x, child_fh, NFS_FH_SIZE);
    // post_op_attr
    xdr_put_post_op_attr(&x, nc, child_path);
    // wcc_data
    n00b_xdr_put_bool(&x, false);  // pre_op_attr
    xdr_put_post_op_attr(&x, nc, dir_path);

    send_record(client_fd, buf, x.pos);
}

// ============================================================================
// NFS3 procedure: MKDIR
// ============================================================================

static void
handle_mkdir(nfs_ctx_t *nc, int client_fd, uint32_t xid, n00b_xdr_t *args)
{
    n00b_string_t *dir_path = decode_fh_path(nc, args, nullptr);

    const uint8_t *name_raw;
    uint32_t       name_len;
    n00b_xdr_get_opaque(args, &name_raw, &name_len);
    // Skip sattr3.

    uint8_t    buf[512];
    n00b_xdr_t x;
    n00b_xdr_init(&x, buf, sizeof(buf));
    write_rpc_reply_hdr(&x, xid);

    if (dir_path == nullptr) {
        n00b_xdr_put_u32(&x, NFS3ERR_STALE);
        send_record(client_fd, buf, x.pos);
        return;
    }

    n00b_string_t *child_path = build_child_path(dir_path, name_raw, name_len);
    n00b_result_t(bool) mr = n00b_vfs_mkdir(nc->vfs, child_path);

    if (n00b_result_is_err(mr)) {
        n00b_xdr_put_u32(&x, vfs_err_to_nfs3(n00b_result_get_err(mr)));
        n00b_xdr_put_bool(&x, false);
        xdr_put_post_op_attr(&x, nc, dir_path);
        send_record(client_fd, buf, x.pos);
        return;
    }

    uint64_t child_id = fh_alloc(nc, child_path);
    uint8_t  child_fh[NFS_FH_SIZE];
    fh_encode(child_id, child_fh);

    n00b_xdr_put_u32(&x, NFS3_OK);
    n00b_xdr_put_bool(&x, true);
    n00b_xdr_put_opaque(&x, child_fh, NFS_FH_SIZE);
    xdr_put_post_op_attr(&x, nc, child_path);
    n00b_xdr_put_bool(&x, false);
    xdr_put_post_op_attr(&x, nc, dir_path);

    send_record(client_fd, buf, x.pos);
}

// ============================================================================
// NFS3 procedure: REMOVE
// ============================================================================

static void
handle_remove(nfs_ctx_t *nc, int client_fd, uint32_t xid, n00b_xdr_t *args)
{
    n00b_string_t *dir_path = decode_fh_path(nc, args, nullptr);

    const uint8_t *name_raw;
    uint32_t       name_len;
    n00b_xdr_get_opaque(args, &name_raw, &name_len);

    uint8_t    buf[512];
    n00b_xdr_t x;
    n00b_xdr_init(&x, buf, sizeof(buf));
    write_rpc_reply_hdr(&x, xid);

    if (dir_path == nullptr) {
        n00b_xdr_put_u32(&x, NFS3ERR_STALE);
        send_record(client_fd, buf, x.pos);
        return;
    }

    n00b_string_t *child_path = build_child_path(dir_path, name_raw, name_len);
    n00b_result_t(bool) dr = n00b_vfs_delete(nc->vfs, child_path);

    if (n00b_result_is_err(dr)) {
        n00b_xdr_put_u32(&x, vfs_err_to_nfs3(n00b_result_get_err(dr)));
    }
    else {
        n00b_xdr_put_u32(&x, NFS3_OK);
    }
    // wcc_data
    n00b_xdr_put_bool(&x, false);
    xdr_put_post_op_attr(&x, nc, dir_path);

    send_record(client_fd, buf, x.pos);
}

// ============================================================================
// NFS3 procedure: RMDIR
// ============================================================================

static void
handle_rmdir(nfs_ctx_t *nc, int client_fd, uint32_t xid, n00b_xdr_t *args)
{
    // Same as REMOVE — VFS delete handles both.
    handle_remove(nc, client_fd, xid, args);
}

// ============================================================================
// NFS3 procedure: RENAME
// ============================================================================

static void
handle_rename(nfs_ctx_t *nc, int client_fd, uint32_t xid, n00b_xdr_t *args)
{
    // From: dir handle + name
    n00b_string_t *from_dir = decode_fh_path(nc, args, nullptr);
    const uint8_t *from_name;
    uint32_t       from_len;
    n00b_xdr_get_opaque(args, &from_name, &from_len);

    // To: dir handle + name
    n00b_string_t *to_dir = decode_fh_path(nc, args, nullptr);
    const uint8_t *to_name;
    uint32_t       to_len;
    n00b_xdr_get_opaque(args, &to_name, &to_len);

    uint8_t    buf[512];
    n00b_xdr_t x;
    n00b_xdr_init(&x, buf, sizeof(buf));
    write_rpc_reply_hdr(&x, xid);

    if (from_dir == nullptr || to_dir == nullptr) {
        n00b_xdr_put_u32(&x, NFS3ERR_STALE);
        send_record(client_fd, buf, x.pos);
        return;
    }

    n00b_string_t *from_path = build_child_path(from_dir, from_name, from_len);
    n00b_string_t *to_path   = build_child_path(to_dir, to_name, to_len);

    n00b_result_t(bool) rr = n00b_vfs_rename(nc->vfs, from_path, to_path);

    if (n00b_result_is_err(rr)) {
        n00b_xdr_put_u32(&x, vfs_err_to_nfs3(n00b_result_get_err(rr)));
    }
    else {
        n00b_xdr_put_u32(&x, NFS3_OK);
    }
    // from wcc_data
    n00b_xdr_put_bool(&x, false);
    xdr_put_post_op_attr(&x, nc, from_dir);
    // to wcc_data
    n00b_xdr_put_bool(&x, false);
    xdr_put_post_op_attr(&x, nc, to_dir);

    send_record(client_fd, buf, x.pos);
}

// ============================================================================
// NFS3 procedure: READDIR
// ============================================================================

#define NFS_READDIR_BUF_SIZE (65536 + 512)

static void
handle_readdir(nfs_ctx_t *nc, int client_fd, uint32_t xid, n00b_xdr_t *args)
{
    n00b_string_t *dir_path = decode_fh_path(nc, args, nullptr);

    uint64_t cookie;
    n00b_xdr_get_u64(args, &cookie);
    // cookieverf (8 bytes) — skip
    args->pos += 8;
    uint32_t dir_count;
    n00b_xdr_get_u32(args, &dir_count);

    uint8_t   *buf = n00b_alloc_array(uint8_t, NFS_READDIR_BUF_SIZE);
    n00b_xdr_t x;
    n00b_xdr_init(&x, buf, NFS_READDIR_BUF_SIZE);
    write_rpc_reply_hdr(&x, xid);

    if (dir_path == nullptr) {
        n00b_xdr_put_u32(&x, NFS3ERR_STALE);
        send_record(client_fd, buf, x.pos);
        return;
    }

    n00b_result_t(n00b_vfs_list_result_t *) lr =
        n00b_vfs_readdir(nc->vfs, dir_path, dir_count);

    if (n00b_result_is_err(lr)) {
        n00b_xdr_put_u32(&x, vfs_err_to_nfs3(n00b_result_get_err(lr)));
        xdr_put_post_op_attr(&x, nc, dir_path);
        send_record(client_fd, buf, x.pos);
        return;
    }

    n00b_xdr_put_u32(&x, NFS3_OK);
    xdr_put_post_op_attr(&x, nc, dir_path);

    // cookieverf (8 bytes of zeros)
    n00b_xdr_put_u32(&x, 0);
    n00b_xdr_put_u32(&x, 0);

    n00b_vfs_list_result_t *list = n00b_result_get(lr);
    for (uint32_t i = 0; i < list->count; i++) {
        if (i < (uint32_t)cookie) continue;  // Skip entries before cookie.

        n00b_xdr_put_bool(&x, true);  // value follows

        // fileid (uint64)
        n00b_string_t *ename = list->entries[i].name;
        uint64_t fileid = (uint64_t)i + 1;
        n00b_xdr_put_u64(&x, fileid);

        // name
        n00b_xdr_put_string(&x, ename->data, (uint32_t)ename->u8_bytes);

        // cookie (uint64) — next entry index
        n00b_xdr_put_u64(&x, (uint64_t)(i + 1));
    }

    n00b_xdr_put_bool(&x, false);  // no more entries
    n00b_xdr_put_bool(&x, true);   // eof

    send_record(client_fd, buf, x.pos);
}

// ============================================================================
// NFS3 procedure: READDIRPLUS
// ============================================================================

static void
handle_readdirplus(nfs_ctx_t *nc, int client_fd, uint32_t xid,
                   n00b_xdr_t *args)
{
    n00b_string_t *dir_path = decode_fh_path(nc, args, nullptr);

    uint64_t cookie;
    n00b_xdr_get_u64(args, &cookie);
    // cookieverf (8 bytes) — skip
    args->pos += 8;
    uint32_t dir_count, max_count;
    n00b_xdr_get_u32(args, &dir_count);
    n00b_xdr_get_u32(args, &max_count);

    uint8_t   *buf = n00b_alloc_array(uint8_t, NFS_READDIR_BUF_SIZE);
    n00b_xdr_t x;
    n00b_xdr_init(&x, buf, NFS_READDIR_BUF_SIZE);
    write_rpc_reply_hdr(&x, xid);

    if (dir_path == nullptr) {
        n00b_xdr_put_u32(&x, NFS3ERR_STALE);
        send_record(client_fd, buf, x.pos);
        return;
    }

    n00b_result_t(n00b_vfs_list_result_t *) lr =
        n00b_vfs_readdir(nc->vfs, dir_path, dir_count);

    if (n00b_result_is_err(lr)) {
        n00b_xdr_put_u32(&x, vfs_err_to_nfs3(n00b_result_get_err(lr)));
        xdr_put_post_op_attr(&x, nc, dir_path);
        send_record(client_fd, buf, x.pos);
        return;
    }

    n00b_xdr_put_u32(&x, NFS3_OK);
    xdr_put_post_op_attr(&x, nc, dir_path);

    // cookieverf
    n00b_xdr_put_u32(&x, 0);
    n00b_xdr_put_u32(&x, 0);

    n00b_vfs_list_result_t *list = n00b_result_get(lr);
    for (uint32_t i = 0; i < list->count; i++) {
        if (i < (uint32_t)cookie) continue;

        n00b_xdr_put_bool(&x, true);  // value follows

        n00b_string_t *ename = list->entries[i].name;
        uint64_t fileid = (uint64_t)i + 1;
        n00b_xdr_put_u64(&x, fileid);

        n00b_xdr_put_string(&x, ename->data, (uint32_t)ename->u8_bytes);
        n00b_xdr_put_u64(&x, (uint64_t)(i + 1));  // cookie

        // name_attributes (post_op_attr for child)
        n00b_string_t *child = build_child_path(dir_path,
            (const uint8_t *)ename->data, (uint32_t)ename->u8_bytes);
        xdr_put_post_op_attr(&x, nc, child);

        // name_handle (post_op_fh3)
        uint64_t child_id = fh_alloc(nc, child);
        uint8_t  child_fh[NFS_FH_SIZE];
        fh_encode(child_id, child_fh);
        n00b_xdr_put_bool(&x, true);
        n00b_xdr_put_opaque(&x, child_fh, NFS_FH_SIZE);
    }

    n00b_xdr_put_bool(&x, false);  // no more entries
    n00b_xdr_put_bool(&x, true);   // eof

    send_record(client_fd, buf, x.pos);
}

// ============================================================================
// NFS3 procedure: FSSTAT
// ============================================================================

static void
handle_fsstat(nfs_ctx_t *nc, int client_fd, uint32_t xid, n00b_xdr_t *args)
{
    (void)args;

    uint8_t    buf[512];
    n00b_xdr_t x;
    n00b_xdr_init(&x, buf, sizeof(buf));
    write_rpc_reply_hdr(&x, xid);

    n00b_xdr_put_u32(&x, NFS3_OK);
    xdr_put_post_op_attr(&x, nc, r"/");

    uint64_t big = (uint64_t)1 << 40;
    n00b_xdr_put_u64(&x, big);   // tbytes
    n00b_xdr_put_u64(&x, big);   // fbytes
    n00b_xdr_put_u64(&x, big);   // abytes
    n00b_xdr_put_u64(&x, big);   // tfiles
    n00b_xdr_put_u64(&x, big);   // ffiles
    n00b_xdr_put_u64(&x, big);   // afiles
    n00b_xdr_put_u32(&x, 0);     // invarsec

    send_record(client_fd, buf, x.pos);
}

// ============================================================================
// NFS3 procedure: PATHCONF
// ============================================================================

static void
handle_pathconf(nfs_ctx_t *nc, int client_fd, uint32_t xid, n00b_xdr_t *args)
{
    (void)args;

    uint8_t    buf[512];
    n00b_xdr_t x;
    n00b_xdr_init(&x, buf, sizeof(buf));
    write_rpc_reply_hdr(&x, xid);

    n00b_xdr_put_u32(&x, NFS3_OK);
    xdr_put_post_op_attr(&x, nc, r"/");

    n00b_xdr_put_u32(&x, 1024);   // linkmax
    n00b_xdr_put_u32(&x, 255);    // name_max
    n00b_xdr_put_bool(&x, true);  // no_trunc
    n00b_xdr_put_bool(&x, false); // chown_restricted
    n00b_xdr_put_bool(&x, true);  // case_insensitive
    n00b_xdr_put_bool(&x, true);  // case_preserving

    send_record(client_fd, buf, x.pos);
}

// ============================================================================
// NFS3 procedure: COMMIT
// ============================================================================

static void
handle_commit(nfs_ctx_t *nc, int client_fd, uint32_t xid, n00b_xdr_t *args)
{
    uint64_t nfs_id;
    n00b_string_t *path = decode_fh_path(nc, args, &nfs_id);
    // offset + count (ignored — we flush everything)
    args->pos += 12;

    uint8_t    buf[512];
    n00b_xdr_t x;
    n00b_xdr_init(&x, buf, sizeof(buf));
    write_rpc_reply_hdr(&x, xid);

    if (path == nullptr) {
        n00b_xdr_put_u32(&x, NFS3ERR_STALE);
        send_record(client_fd, buf, x.pos);
        return;
    }

    // Flush the cached VFS handle if open.
    n00b_vfs_fh_t fh = nfs_get_vfs_fh(nc, nfs_id);
    if (fh != N00B_VFS_FH_INVALID) {
        (void)n00b_vfs_flush(nc->vfs, fh);
    }

    n00b_xdr_put_u32(&x, NFS3_OK);
    // wcc_data
    n00b_xdr_put_bool(&x, false);
    xdr_put_post_op_attr(&x, nc, path);
    // write verifier (8 bytes)
    n00b_xdr_put_u32(&x, 0);
    n00b_xdr_put_u32(&x, 0);

    send_record(client_fd, buf, x.pos);
}

// ============================================================================
// Client connection handler
// ============================================================================

static bool
recv_exact(int fd, uint8_t *buf, uint32_t len)
{
    uint32_t got = 0;
    while (got < len) {
        ssize_t r = n00b_retry_eintr_until(NFS_IO_DEADLINE(),
                                           read(fd, buf + got, len - got));
        if (r <= 0) return false;
        got += (uint32_t)r;
    }
    return true;
}

static void
handle_client(nfs_ctx_t *nc, int client_fd)
{
    // The conduit listener hands us a non-blocking fd; switch it to blocking
    // with read/write timeouts so this synchronous handler neither spins nor
    // hangs (mirrors src/net/quic/metrics.c's accepted-fd handling).
    int fl = fcntl(client_fd, F_GETFL, 0);
    if (fl >= 0) {
        (void)fcntl(client_fd, F_SETFL, fl & ~O_NONBLOCK);
    }
    struct timeval tv = {.tv_sec = NFS_IO_TIMEOUT_SEC, .tv_usec = 0};
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    uint8_t rm_buf[4];

    while (recv_exact(client_fd, rm_buf, 4)) {
        uint32_t rm = ((uint32_t)rm_buf[0] << 24) | ((uint32_t)rm_buf[1] << 16)
                    | ((uint32_t)rm_buf[2] << 8)  | (uint32_t)rm_buf[3];
        uint32_t frag_len;
        bool     last;
        n00b_rpc_rm_decode(rm, &frag_len, &last);

        if (frag_len > 1048576) break;  // Sanity limit.

        uint8_t *payload = n00b_alloc_array(uint8_t, frag_len);
        if (!recv_exact(client_fd, payload, frag_len)) break;

        // Parse RPC call header.
        n00b_xdr_t args;
        n00b_xdr_init(&args, payload, frag_len);

        uint32_t xid, msg_type, rpc_version, program, version, procedure;
        n00b_xdr_get_u32(&args, &xid);
        n00b_xdr_get_u32(&args, &msg_type);
        if (msg_type != N00B_RPC_CALL) continue;

        n00b_xdr_get_u32(&args, &rpc_version);
        n00b_xdr_get_u32(&args, &program);
        n00b_xdr_get_u32(&args, &version);
        n00b_xdr_get_u32(&args, &procedure);

        // Skip auth credentials (flavor + body) and verifier.
        uint32_t auth_flavor, auth_len;
        n00b_xdr_get_u32(&args, &auth_flavor);
        n00b_xdr_get_u32(&args, &auth_len);
        args.pos += (auth_len + 3) & ~3u;  // skip body

        n00b_xdr_get_u32(&args, &auth_flavor);  // verifier flavor
        n00b_xdr_get_u32(&args, &auth_len);     // verifier body len
        args.pos += (auth_len + 3) & ~3u;

        // Dispatch.
        if (program == N00B_MOUNT_PROGRAM) {
            if (procedure == MOUNTPROC3_NULL) {
                handle_null(nc, client_fd, xid);
            }
            else if (procedure == MOUNTPROC3_MNT) {
                handle_mount(nc, client_fd, xid, &args);
            }
            else {
                handle_null(nc, client_fd, xid);
            }
        }
        else if (program == N00B_NFS_PROGRAM) {
            switch (procedure) {
            case NFSPROC3_NULL:
                handle_null(nc, client_fd, xid);
                break;
            case NFSPROC3_GETATTR:
                handle_getattr(nc, client_fd, xid, &args);
                break;
            case NFSPROC3_SETATTR:
                handle_setattr(nc, client_fd, xid, &args);
                break;
            case NFSPROC3_LOOKUP:
                handle_lookup(nc, client_fd, xid, &args);
                break;
            case NFSPROC3_ACCESS:
                handle_access(nc, client_fd, xid, &args);
                break;
            case NFSPROC3_READ:
                handle_read(nc, client_fd, xid, &args);
                break;
            case NFSPROC3_WRITE:
                handle_write(nc, client_fd, xid, &args);
                break;
            case NFSPROC3_CREATE:
                handle_create(nc, client_fd, xid, &args);
                break;
            case NFSPROC3_MKDIR:
                handle_mkdir(nc, client_fd, xid, &args);
                break;
            case NFSPROC3_REMOVE:
                handle_remove(nc, client_fd, xid, &args);
                break;
            case NFSPROC3_RMDIR:
                handle_rmdir(nc, client_fd, xid, &args);
                break;
            case NFSPROC3_RENAME:
                handle_rename(nc, client_fd, xid, &args);
                break;
            case NFSPROC3_READDIR:
                handle_readdir(nc, client_fd, xid, &args);
                break;
            case NFSPROC3_READDIRPLUS:
                handle_readdirplus(nc, client_fd, xid, &args);
                break;
            case NFSPROC3_FSSTAT:
                handle_fsstat(nc, client_fd, xid, &args);
                break;
            case NFSPROC3_FSINFO:
                handle_fsinfo(nc, client_fd, xid, &args);
                break;
            case NFSPROC3_PATHCONF:
                handle_pathconf(nc, client_fd, xid, &args);
                break;
            case NFSPROC3_COMMIT:
                handle_commit(nc, client_fd, xid, &args);
                break;
            default: {
                // Unsupported procedure — return PROC_UNAVAIL.
                uint8_t    buf[64];
                n00b_xdr_t x;
                n00b_xdr_init(&x, buf, sizeof(buf));
                n00b_xdr_put_u32(&x, xid);
                n00b_xdr_put_u32(&x, N00B_RPC_REPLY);
                n00b_xdr_put_u32(&x, N00B_RPC_MSG_ACCEPTED);
                n00b_xdr_put_u32(&x, 0);
                n00b_xdr_put_u32(&x, 0);
                n00b_xdr_put_u32(&x, N00B_RPC_PROC_UNAVAIL);
                send_record(client_fd, buf, x.pos);
                break;
            }
            }
        }
    }

    close(client_fd);
}

// ============================================================================
// Server thread
// ============================================================================

// Per-connection thread: NFS clients (notably macOS) keep SEPARATE TCP
// connections open concurrently for the MOUNT and NFS services, so the server
// must service connections in parallel — a single accept→handle→accept loop
// deadlocks the moment a client holds one connection open while opening a
// second. Each accepted connection runs handle_client on its own n00b thread
// (fire-and-forget: the runtime reaps it; no join needed).
typedef struct {
    nfs_ctx_t *nc;
    int        client_fd;
} nfs_conn_arg_t;

static void *
nfs_conn_thread(void *arg)
{
    nfs_conn_arg_t *ca        = arg;
    nfs_ctx_t      *nc        = ca->nc;
    int             client_fd = ca->client_fd;

    n00b_free(ca);
    handle_client(nc, client_fd);

    return nullptr;
}

static void *
nfs_server_thread(void *arg)
{
    nfs_ctx_t *nc = arg;

    atomic_store(&nc->frontend->running, true);

    while (atomic_load(&nc->frontend->running)) {
        // Drain all currently-accepted connections; one worker thread per fd.
        n00b_conduit_sock_accept_msg_t *msg;
        while ((msg = n00b_conduit_sock_accept_inbox_pop(nc->accept_inbox))
               != nullptr) {
            int client = msg->payload.client_fd;
            if (client < 0) {
                continue;
            }

            nfs_conn_arg_t *ca = n00b_alloc(nfs_conn_arg_t);
            ca->nc             = nc;
            ca->client_fd      = client;

            if (n00b_result_is_err(n00b_thread_spawn(nfs_conn_thread, ca))) {
                // Fall back to inline handling rather than drop the connection.
                n00b_free(ca);
                handle_client(nc, client);
            }
        }

        if (!atomic_load(&nc->frontend->running)) {
            break;
        }

        // Block until the IO backend delivers more accepts (bounded so we
        // periodically re-check the running flag for clean shutdown).
        n00b_condition_lock(&nc->accept_inbox->cv);
        if (!n00b_conduit_inbox_has_msg(n00b_conduit_sock_accept_payload_t,
                                        nc->accept_inbox)
            && !n00b_conduit_inbox_has_sys(nc->accept_inbox)) {
            n00b_condition_wait(&nc->accept_inbox->cv,
                                .timeout_ms  = 200,
                                .auto_unlock = true);
        }
        else {
            n00b_condition_unlock(&nc->accept_inbox->cv);
        }
    }

    return nullptr;
}

// ============================================================================
// Frontend vtable
// ============================================================================

static n00b_string_t *
nfs_fe_name(void)
{
    return r"nfs";
}

static n00b_result_t(bool)
nfs_fe_start(n00b_vfs_frontend_t *fe)
{
    nfs_ctx_t *nc = fe->ctx;

    // Bind+listen via the n00b conduit listener (no raw listen socket). Use
    // the runtime's default conduit + IO backend — no per-frontend service.
    n00b_conduit_t *c = n00b_get_runtime()->default_conduit;
    if (c == nullptr) {
        return n00b_result_err(bool, N00B_VFS_ERR_IO);
    }

    auto io_opt = n00b_conduit_default_backend(c);
    if (!n00b_option_is_set(io_opt)) {
        return n00b_result_err(bool, N00B_VFS_ERR_IO);
    }
    n00b_conduit_io_backend_t *io = n00b_option_get(io_opt);

    auto lr = n00b_conduit_listen_tcp(c,
                                      io,
                                      r"127.0.0.1",
                                      nc->port,
                                      16);
    if (n00b_result_is_err(lr)) {
        return n00b_result_err(bool, N00B_VFS_ERR_IO);
    }
    n00b_conduit_listener_t *listener = n00b_result_get(lr);

    auto at_opt = n00b_conduit_listener_accept_topic(listener);
    if (!n00b_option_is_set(at_opt)) {
        n00b_conduit_listener_close(listener);
        return n00b_result_err(bool, N00B_VFS_ERR_IO);
    }
    n00b_conduit_topic_base_t *accept_topic = n00b_option_get(at_opt);

    n00b_conduit_sock_accept_inbox_t *inbox =
        n00b_conduit_sock_accept_inbox_new(c);

    if (n00b_conduit_sock_accept_subscribe(accept_topic,
                                           inbox,
                                           .operations = N00B_CONDUIT_OP_ALL)
        == N00B_CONDUIT_INVALID_SUB_HANDLE) {
        n00b_conduit_listener_close(listener);
        return n00b_result_err(bool, N00B_VFS_ERR_IO);
    }

    nc->conduit      = c;
    nc->listener     = listener;
    nc->accept_inbox = inbox;

    // Read back the actual port if an ephemeral (0) port was requested.
    if (nc->port == 0 && listener->fd >= 0) {
        struct sockaddr_in addr = {};
        socklen_t          alen = sizeof(addr);
        if (getsockname(listener->fd, (struct sockaddr *)&addr, &alen) == 0) {
            nc->port = ntohs(addr.sin_port);
        }
    }

    // Allocate root file handle.
    fh_alloc(nc, r"/");

    // Acceptor runs on an n00b thread (GC-aware, in the n00b thread lifecycle).
    auto spawn_r = n00b_thread_spawn(nfs_server_thread, nc);

    if (n00b_result_is_err(spawn_r)) {
        n00b_conduit_listener_close(listener);
        return n00b_result_err(bool, N00B_VFS_ERR_IO);
    }

    nc->thread = n00b_result_get(spawn_r);

    return n00b_result_ok(bool, true);
}

static void
nfs_fe_stop(n00b_vfs_frontend_t *fe)
{
    nfs_ctx_t *nc = fe->ctx;

    atomic_store(&fe->running, false);
    if (nc->listener != nullptr) {
        n00b_conduit_listener_close(nc->listener);
        nc->listener = nullptr;
    }

    // The acceptor thread wakes within its wait timeout, sees !running, exits.
    n00b_thread_join(nc->thread);
}

static bool
nfs_fe_is_running(n00b_vfs_frontend_t *fe)
{
    return atomic_load(&fe->running);
}

const n00b_vfs_frontend_ops_t n00b_vfs_frontend_nfs_ops = {
    .name       = nfs_fe_name,
    .start      = nfs_fe_start,
    .stop       = nfs_fe_stop,
    .is_running = nfs_fe_is_running,
};

// ============================================================================
// Constructor
// ============================================================================

n00b_result_t(n00b_vfs_frontend_t *)
n00b_vfs_frontend_nfs_new(n00b_vfs_t *vfs, n00b_string_t *mount_point,
                          uint16_t port)
{
    if (vfs == nullptr || mount_point == nullptr) {
        return n00b_result_err(n00b_vfs_frontend_t *, N00B_VFS_ERR_NULL_ARG);
    }

    nfs_ctx_t *nc = n00b_alloc(nfs_ctx_t);
    nc->port       = port;
    nc->fh_table   = n00b_alloc_array(nfs_fh_entry_t *, 64);
    nc->fh_cap     = 64;
    nc->fh_count   = 0;
    nc->next_fh_id = 1;

    n00b_vfs_frontend_t *fe = n00b_alloc(n00b_vfs_frontend_t);
    fe->ops         = &n00b_vfs_frontend_nfs_ops;
    fe->vfs         = vfs;
    fe->mount_point = mount_point;
    fe->ctx         = nc;
    atomic_store(&fe->running, false);

    nc->frontend = fe;
    nc->vfs      = vfs;

    return n00b_result_ok(n00b_vfs_frontend_t *, fe);
}
