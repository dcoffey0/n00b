/**
 * @file interval_tree.h
 * @brief Type-safe augmented AVL interval tree — pure macros.
 *
 * @c n00b_interval_tree_t(D) provides an augmented AVL tree of
 * half-open `[low, high)` intervals, each carrying a user-data value of
 * type @p D.
 *
 * Follows the same generic-struct pattern as @c tree.h and @c stack.h:
 * `_generic_struct` + `typeid()` give each parameterization its own
 * unique C struct.
 *
 * All operations are macros wrapping statement expressions.  The AVL
 * logic (rotations, rebalance, augmentation) operates on fixed-layout
 * fields that are identical across parameterizations; `auto` infers
 * the concrete types.
 */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <assert.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/gc_map.h"
#include "adt/stack.h"
#include "adt/result.h"
#include "core/data_lock.h"

// ============================================================================
// Error codes
// ============================================================================

#define N00B_INTERVAL_OK            0
#define N00B_INTERVAL_ERR_INVALID   1
#define N00B_INTERVAL_ERR_NOT_FOUND 2

// ============================================================================
// Type definitions
// ============================================================================

#define n00b_interval_node_tid(D) typeid("interval_node", D)
#define n00b_interval_tree_tid(D) typeid("interval_tree", D)

/**
 * @brief Interval node with data of type @p D.
 * @param D  User-data type stored in each interval node.
 */
#define n00b_interval_node_t(D)                                                                \
    _generic_struct n00b_interval_node_tid(D) {                                                \
        uint64_t low;                                                                          \
        uint64_t high;                                                                         \
        uint64_t height;                                                                       \
        uint64_t maximum;                                                                      \
        uint64_t minimum;                                                                      \
        struct n00b_interval_node_tid(D) *left;                                                \
        struct n00b_interval_node_tid(D) *right;                                               \
        D data;                                                                                \
    }

/**
 * @brief Interval tree with data type @p D.
 * @param D  User-data type for interval nodes.
 */
#define n00b_interval_tree_t(D)                                                                \
    _generic_struct n00b_interval_tree_tid(D) {                                                \
        n00b_interval_node_t(D) *root;                                                         \
        n00b_allocator_t    *allocator;                                                        \
        n00b_rwlock_t       *lock;                                                              \
        n00b_gc_scan_kind_t  scan_kind;                                                        \
        n00b_gc_scan_cb_t    scan_cb;                                                          \
        void                *scan_user;                                                        \
    }

/* All n00b_interval_node_t(D) share the same prefix layout up through
 * left/right.  This lets non-parameterized helpers (invariant check,
 * augment repair) operate on any tree by casting through this struct. */
typedef struct _n00b_itree_nbase_t {
    uint64_t                       low;
    uint64_t                       high;
    uint64_t                       height;
    uint64_t                       maximum;
    uint64_t                       minimum;
    struct _n00b_itree_nbase_t    *left;
    struct _n00b_itree_nbase_t    *right;
    /* data follows */
} _n00b_itree_nbase_t;

/**
 * @brief Plain half-open interval range used by merged-range queries.
 */
typedef struct n00b_interval_range {
    uint64_t low;
    uint64_t high;
} n00b_interval_range_t;

// ============================================================================
// Helper: derive node-pointer type from a tree pointer expression
// without evaluating the expression — uses typeof on a null cast.
// ============================================================================


// ============================================================================
// Internal AVL helpers (all operate on auto-inferred node pointers)
// ============================================================================

#define _n00b_itree_get_height(node)                                                           \
    ({                                                                                         \
        auto _igh_n = (node);                                                                  \
        (int64_t)(_igh_n == nullptr ? 0 : _igh_n->height);                                    \
    })

#define _n00b_itree_update_node(node)                                                          \
    do {                                                                                       \
        auto _iun_n = (node);                                                                  \
        if (_iun_n == nullptr) break;                                                          \
        int64_t _iun_lh = _n00b_itree_get_height(_iun_n->left);                               \
        int64_t _iun_rh = _n00b_itree_get_height(_iun_n->right);                              \
        _iun_n->height = (uint64_t)((_iun_lh > _iun_rh ? _iun_lh : _iun_rh) + 1);            \
        _iun_n->minimum = _iun_n->low;                                                        \
        _iun_n->maximum = _iun_n->high;                                                       \
        if (_iun_n->left != nullptr) {                                                         \
            if (_iun_n->left->maximum > _iun_n->maximum)                                       \
                _iun_n->maximum = _iun_n->left->maximum;                                      \
            if (_iun_n->left->minimum < _iun_n->minimum)                                      \
                _iun_n->minimum = _iun_n->left->minimum;                                      \
        }                                                                                      \
        if (_iun_n->right != nullptr) {                                                        \
            if (_iun_n->right->maximum > _iun_n->maximum)                                     \
                _iun_n->maximum = _iun_n->right->maximum;                                     \
            if (_iun_n->right->minimum < _iun_n->minimum)                                     \
                _iun_n->minimum = _iun_n->right->minimum;                                     \
        }                                                                                      \
    } while (0)

#define _n00b_itree_rotate_left(node)                                                          \
    ({                                                                                         \
        auto _irl_n = (node);                                                                  \
        auto _irl_rc = _irl_n->right;                                                          \
        _irl_n->right = _irl_rc->left;                                                         \
        _irl_rc->left = _irl_n;                                                                \
        _n00b_itree_update_node(_irl_n);                                                       \
        _n00b_itree_update_node(_irl_rc);                                                      \
        _irl_rc;                                                                               \
    })

#define _n00b_itree_rotate_right(node)                                                         \
    ({                                                                                         \
        auto _irr_n = (node);                                                                  \
        auto _irr_lc = _irr_n->left;                                                           \
        _irr_n->left = _irr_lc->right;                                                        \
        _irr_lc->right = _irr_n;                                                              \
        _n00b_itree_update_node(_irr_n);                                                       \
        _n00b_itree_update_node(_irr_lc);                                                      \
        _irr_lc;                                                                               \
    })

#define _n00b_itree_balance_factor(node)                                                       \
    ({                                                                                         \
        auto _ibf_n = (node);                                                                  \
        (int64_t)(_ibf_n == nullptr                                                            \
            ? 0                                                                                \
            : _n00b_itree_get_height(_ibf_n->left)                                             \
              - _n00b_itree_get_height(_ibf_n->right));                                        \
    })

#define _n00b_itree_balance_node(node)                                                         \
    ({                                                                                         \
        auto _ibn_n = (node);                                                                  \
        _n00b_itree_update_node(_ibn_n);                                                       \
        int64_t _ibn_bf = _n00b_itree_balance_factor(_ibn_n);                                  \
        typeof(_ibn_n) _ibn_result = _ibn_n;                                                   \
        if (_ibn_bf == 2) {                                                                    \
            if (_n00b_itree_balance_factor(_ibn_n->left) >= 0) {                               \
                _ibn_result = _n00b_itree_rotate_right(_ibn_n);                                \
            } else {                                                                           \
                _ibn_n->left = _n00b_itree_rotate_left(_ibn_n->left);                          \
                _ibn_result = _n00b_itree_rotate_right(_ibn_n);                                \
            }                                                                                  \
        } else if (_ibn_bf == -2) {                                                            \
            if (_n00b_itree_balance_factor(_ibn_n->right) <= 0) {                              \
                _ibn_result = _n00b_itree_rotate_left(_ibn_n);                                 \
            } else {                                                                           \
                _ibn_n->right = _n00b_itree_rotate_right(_ibn_n->right);                       \
                _ibn_result = _n00b_itree_rotate_left(_ibn_n);                                 \
            }                                                                                  \
        }                                                                                      \
        _ibn_result;                                                                           \
    })

// ============================================================================
// Initialization
// ============================================================================

/**
 * @brief Initialize an interval tree.
 *
 * Uses `auto` — no type parameters needed at the call site.
 *
 * @param tree      Pointer to the tree to initialize.
 * @param alloc_arg Optional allocator (nullptr = runtime default).
 */
#define n00b_interval_tree_init(tree, ...)                                                     \
    _n00b_interval_tree_init_impl(tree, true, ##__VA_ARGS__)

/// @brief Initialize an interval tree with no rwlock, for a caller that
/// guards it with a lock of its own.
#define n00b_interval_tree_init_private(tree, ...)                                             \
    _n00b_interval_tree_init_impl(tree, false, ##__VA_ARGS__)

#define _n00b_interval_tree_init_impl(tree, locked, ...)                                       \
    do {                                                                                       \
        auto _iti_t = (tree);                                                                  \
        n00b_alloc_opts_t _iti_o = (n00b_alloc_opts_t){__VA_ARGS__};                           \
        _iti_t->root = nullptr;                                                                \
        /* Descent stack is PER-CALL (allocated locally in each insert/delete/   \
         * search), not shared on the tree: a shared stack is clobbered when a   \
         * tree op nests another (insert can re-enter via alloc -> finalizer ->  \
         * n00b_mmap_by_address) or when the STW short-circuit lets an           \
         * unsuspended thread walk concurrently.  Each op sizes its local stack  \
         * to 256 (>= AVL depth bound 1.44*log2(N+2)) so it never grows. */      \
        _iti_t->allocator = _iti_o.allocator;                                                  \
        _iti_t->scan_kind = _iti_o.scan_kind;                                                  \
        _iti_t->scan_cb   = _iti_o.scan_cb;                                                    \
        _iti_t->scan_user = _iti_o.scan_user;                                                  \
        _iti_t->lock = (locked) ? n00b_data_lock_new(.allocator = _iti_t->allocator)           \
                                : (n00b_rwlock_t *)nullptr;                                    \
    } while (0)

// ============================================================================
// Insert
// ============================================================================

/**
 * @brief Insert a half-open interval `[low, high)` with associated data.
 *
 * @param tree  Pointer to the tree.
 * @param lo    Interval lower bound.
 * @param hi    Interval upper bound.
 * @param dval  User data to associate.
 * @return      Result containing the new node pointer, or error.
 */
#define n00b_interval_insert(tree, lo, hi, dval)                                               \
    ({                                                                                         \
        auto _ii_tree = (tree);                                                                \
        typedef typeof(_ii_tree->root) _ii_np;                                                 \
        uint64_t _ii_lo = (lo);                                                                \
        uint64_t _ii_hi = (hi);                                                                \
        _ii_np _ii_result_node = nullptr;                                                      \
        bool _ii_err = false;                                                                  \
                                                                                               \
        if (_ii_lo > _ii_hi) {                                                                 \
            _ii_err = true;                                                                    \
        } else {                                                                               \
            n00b_data_write_lock(_ii_tree->lock);                                              \
                                                                                               \
            _ii_np _ii_node = (_ii_np)n00b_alloc_with_opts(                                    \
                typeof(*_ii_tree->root),                                                       \
                &(n00b_alloc_opts_t){                                                          \
                    .allocator = _ii_tree->allocator,                                          \
                    .scan_kind = _ii_tree->scan_kind,                                          \
                    .scan_cb   = _ii_tree->scan_cb,                                            \
                    .scan_user = _ii_tree->scan_user,                                          \
                });                                                                            \
                                                                                               \
            _ii_node->low = _ii_lo;                                                            \
            _ii_node->high = _ii_hi;                                                           \
            _ii_node->minimum = _ii_lo;                                                        \
            _ii_node->maximum = _ii_hi;                                                        \
            _ii_node->height = 1;                                                              \
            /* Pool allocations may not be fully zeroed (pool_free's     \
             * memset is sized as ix*64, not (1<<ix)*64, so bytes past   \
             * byte 64 retain stale data — including the right pointer  \
             * at offset 64).  Initialize the structural fields. */     \
            _ii_node->left = nullptr;                                                          \
            _ii_node->right = nullptr;                                                         \
            _ii_node->data = (dval);                                                           \
                                                                                               \
            if (_ii_tree->root == nullptr) {                                                   \
                _ii_tree->root = _ii_node;                                                     \
                _ii_result_node = _ii_node;                                                    \
            } else {                                                                           \
                _ii_np _ii_cur = _ii_tree->root;                                               \
                /* Per-call private descent stack (WP-001): a shared tree     \
                 * stack is clobbered when a tree op nests another (this op    \
                 * can re-enter via alloc -> finalizer -> mmap lookup) or when \
                 * the STW short-circuit lets an unsuspended thread walk       \
                 * concurrently.  A local, unlocked stack is per-call safe.    \
                 * Cap 256 >= AVL depth bound (1.44*log2(N+2)), so it never    \
                 * grows (no n00b_free recursion). */                          \
                n00b_stack_t(void *) _ii_stack = n00b_stack_new_cap_private(   \
                    void *, 256, .allocator = _ii_tree->allocator);            \
                                                                               \
                while (_ii_cur != nullptr) {                                                   \
                    n00b_stack_push(_ii_stack, (void *)_ii_cur);                               \
                    if (_ii_lo < _ii_cur->low) {                                               \
                        if (_ii_cur->left == nullptr) {                                        \
                            _ii_cur->left = _ii_node;                                          \
                            break;                                                             \
                        }                                                                      \
                        _ii_cur = _ii_cur->left;                                               \
                    } else {                                                                   \
                        if (_ii_cur->right == nullptr) {                                       \
                            _ii_cur->right = _ii_node;                                         \
                            break;                                                             \
                        }                                                                      \
                        _ii_cur = _ii_cur->right;                                              \
                    }                                                                          \
                }                                                                              \
                                                                                               \
                _ii_result_node = _ii_node;                                                    \
                _ii_np _ii_parent;                                                             \
                _ii_np _ii_child = _ii_node;                                                   \
                size_t _ii_cnt;                                                                \
                for (_ii_cnt = n00b_stack_len(_ii_stack);                                      \
                     _ii_cnt > 0; _ii_cnt--) {                                                 \
                    _ii_parent = (_ii_np)n00b_option_get(                                      \
                        n00b_stack_pop(void *, _ii_stack));                                    \
                    if (_ii_parent->left == _ii_child) {                                       \
                        _ii_parent->left = _n00b_itree_balance_node(_ii_child);                \
                    } else {                                                                   \
                        _ii_parent->right = _n00b_itree_balance_node(_ii_child);               \
                    }                                                                          \
                    _ii_child = _ii_parent;                                                    \
                }                                                                              \
                _ii_tree->root = _n00b_itree_balance_node(_ii_child);                          \
                n00b_stack_free(_ii_stack);                                                    \
            }                                                                                  \
            n00b_data_unlock(_ii_tree->lock);                                                  \
        }                                                                                      \
                                                                                               \
        _ii_err                                                                                \
            ? n00b_result_err(void *, N00B_INTERVAL_ERR_INVALID)                               \
            : n00b_result_ok(void *, (void *)_ii_result_node);                                 \
    })

// ============================================================================
// Max / Min
// ============================================================================

/** @brief Global maximum endpoint across all intervals. */
#define n00b_interval_max(tree)                                                                \
    ({                                                                                         \
        auto _imx_t = (tree);                                                                  \
        n00b_data_read_lock(_imx_t->lock);                                                    \
        n00b_result_t(uint64_t) _imx_r;                                                       \
        if (_imx_t->root == nullptr) {                                                         \
            _imx_r = n00b_result_err(uint64_t, 1);                                            \
        } else {                                                                               \
            _imx_r = n00b_result_ok(uint64_t, _imx_t->root->maximum);                         \
        }                                                                                      \
        n00b_data_unlock(_imx_t->lock);                                                       \
        _imx_r;                                                                                \
    })

/** @brief Global minimum endpoint across all intervals. */
#define n00b_interval_min(tree)                                                                \
    ({                                                                                         \
        auto _imn_t = (tree);                                                                  \
        n00b_data_read_lock(_imn_t->lock);                                                    \
        n00b_result_t(uint64_t) _imn_r;                                                       \
        if (_imn_t->root == nullptr) {                                                         \
            _imn_r = n00b_result_err(uint64_t, 1);                                            \
        } else {                                                                               \
            _imn_r = n00b_result_ok(uint64_t, _imn_t->root->minimum);                         \
        }                                                                                      \
        n00b_data_unlock(_imn_t->lock);                                                       \
        _imn_r;                                                                                \
    })

// ============================================================================
// Search any
// ============================================================================

/** @brief Find any single interval overlapping `[low, high)`. */
#define n00b_interval_search_any(tree, lo, hi)                                                 \
    ({                                                                                         \
        auto _isa_t = (tree);                                                                  \
        typedef typeof(_isa_t->root) _isa_np;                                                  \
        uint64_t _isa_lo = (lo);                                                               \
        uint64_t _isa_hi = (hi);                                                               \
        _isa_np _isa_found = nullptr;                                                          \
        bool _isa_err = false;                                                                 \
                                                                                               \
        if (_isa_lo > _isa_hi) {                                                               \
            _isa_err = true;                                                                   \
        } else {                                                                               \
            n00b_data_read_lock(_isa_t->lock);                                                 \
            if (_isa_t->root != nullptr) {                                                     \
                n00b_stack_t(void *) _isa_stack = n00b_stack_new_cap_private( \
                    void *, 256, .allocator = _isa_t->allocator);              \
                n00b_stack_push(_isa_stack, (void *)_isa_t->root);                             \
                while (n00b_stack_len(_isa_stack) != 0) {                                      \
                    _isa_np _isa_n = (_isa_np)n00b_option_get(                                 \
                        n00b_stack_pop(void *, _isa_stack));                                   \
                    if (_isa_n->low < _isa_hi && _isa_lo < _isa_n->high) {                     \
                        _isa_found = _isa_n;                                                   \
                        break;                                                                 \
                    }                                                                          \
                    if (_isa_n->left != nullptr                                                \
                        && _isa_n->left->maximum > _isa_lo                                     \
                        && _isa_n->left->minimum < _isa_hi) {                                  \
                        n00b_stack_push(_isa_stack, (void *)_isa_n->left);                     \
                    }                                                                          \
                    if (_isa_n->right != nullptr                                               \
                        && _isa_n->right->maximum > _isa_lo                                    \
                        && _isa_n->right->minimum < _isa_hi) {                                 \
                        n00b_stack_push(_isa_stack, (void *)_isa_n->right);                    \
                    }                                                                          \
                }                                                                              \
                n00b_stack_free(_isa_stack);                                                   \
            }                                                                                  \
            n00b_data_unlock(_isa_t->lock);                                                    \
        }                                                                                      \
                                                                                               \
        _isa_err                                                                               \
            ? n00b_result_err(void *, N00B_INTERVAL_ERR_INVALID)                               \
            : n00b_result_ok(void *, (void *)_isa_found);                                      \
    })

// ============================================================================
// Search (all overlapping)
// ============================================================================

/** @brief Find all intervals overlapping `[low, high)`. */
#define n00b_interval_search(tree, lo, hi, hits_ptr)                                           \
    ({                                                                                         \
        auto _is_t = (tree);                                                                   \
        uint64_t _is_lo = (lo);                                                                \
        uint64_t _is_hi = (hi);                                                                \
        auto _is_hits = (hits_ptr);                                                            \
        bool _is_err = false;                                                                  \
                                                                                               \
        if (_is_lo > _is_hi) {                                                                 \
            _is_err = true;                                                                    \
        } else {                                                                               \
            n00b_data_read_lock(_is_t->lock);                                                  \
            if (_is_t->root != nullptr) {                                                      \
                n00b_stack_t(void *) _is_stack = n00b_stack_new_cap_private(  \
                    void *, 256, .allocator = _is_t->allocator);               \
                n00b_stack_push(_is_stack, (void *)_is_t->root);                               \
                while (n00b_stack_len(_is_stack) != 0) {                                       \
                    auto _is_n = _is_t->root;                                                  \
                    _is_n = (typeof(_is_n))n00b_option_get(                                    \
                        n00b_stack_pop(void *, _is_stack));                                    \
                    if (_is_n->low < _is_hi && _is_lo < _is_n->high) {                         \
                        n00b_stack_push(*_is_hits, (void *)_is_n);                             \
                    }                                                                          \
                    if (_is_n->left != nullptr                                                 \
                        && _is_n->left->maximum > _is_lo                                       \
                        && _is_n->left->minimum < _is_hi) {                                    \
                        n00b_stack_push(_is_stack, (void *)_is_n->left);                       \
                    }                                                                          \
                    if (_is_n->right != nullptr                                                \
                        && _is_n->right->maximum > _is_lo                                      \
                        && _is_n->right->minimum < _is_hi) {                                   \
                        n00b_stack_push(_is_stack, (void *)_is_n->right);                      \
                    }                                                                          \
                }                                                                              \
                n00b_stack_free(_is_stack);                                                    \
            }                                                                                  \
            n00b_data_unlock(_is_t->lock);                                                     \
        }                                                                                      \
                                                                                               \
        _is_err ? n00b_result_err(int, N00B_INTERVAL_ERR_INVALID)                              \
                : n00b_result_ok(int, 0);                                                      \
    })

// ============================================================================
// Search ordered (in-order traversal)
// ============================================================================

/** @brief Find all intervals overlapping `[low, high)`, sorted by low bound. */
#define n00b_interval_search_ordered(tree, lo, hi, hits_ptr)                                   \
    ({                                                                                         \
        auto _iso_t = (tree);                                                                  \
        uint64_t _iso_lo = (lo);                                                               \
        uint64_t _iso_hi = (hi);                                                               \
        auto _iso_hits = (hits_ptr);                                                           \
        bool _iso_err = false;                                                                 \
                                                                                               \
        if (_iso_lo > _iso_hi) {                                                               \
            _iso_err = true;                                                                   \
        } else {                                                                               \
            n00b_data_read_lock(_iso_t->lock);                                                 \
            auto _iso_n = _iso_t->root;                                                        \
            if (_iso_n != nullptr) {                                                           \
                n00b_stack_t(void *) _iso_stack = n00b_stack_new_cap_private( \
                    void *, 256, .allocator = _iso_t->allocator);              \
                int _iso_searching = 1;                                                        \
                while (_iso_searching) {                                                       \
                    if (_iso_n->maximum > _iso_lo && _iso_n->minimum < _iso_hi) {              \
                        if (_iso_n->left != nullptr) {                                         \
                            n00b_stack_push(_iso_stack, (void *)_iso_n);                       \
                            _iso_n = _iso_n->left;                                             \
                            continue;                                                          \
                        }                                                                      \
                        if (_iso_n->low < _iso_hi && _iso_lo < _iso_n->high)                   \
                            n00b_stack_push(*_iso_hits, (void *)_iso_n);                       \
                        if (_iso_n->right != nullptr) {                                        \
                            _iso_n = _iso_n->right;                                            \
                            continue;                                                          \
                        }                                                                      \
                    }                                                                          \
                    while ((_iso_searching = !!n00b_stack_len(_iso_stack))) {                   \
                        _iso_n = (typeof(_iso_n))n00b_option_get(                              \
                            n00b_stack_pop(void *, _iso_stack));                               \
                        if (_iso_n->low < _iso_hi && _iso_lo < _iso_n->high)                   \
                            n00b_stack_push(*_iso_hits, (void *)_iso_n);                       \
                        if (_iso_n->right != nullptr) {                                        \
                            _iso_n = _iso_n->right;                                            \
                            break;                                                             \
                        }                                                                      \
                    }                                                                          \
                }                                                                              \
                n00b_stack_free(_iso_stack);                                                   \
            }                                                                                  \
            n00b_data_unlock(_iso_t->lock);                                                    \
        }                                                                                      \
                                                                                               \
        _iso_err ? n00b_result_err(int, N00B_INTERVAL_ERR_INVALID)                             \
                 : n00b_result_ok(int, 0);                                                     \
    })

// ============================================================================
// Merge overlapping ranges
// ============================================================================

/**
 * @brief Merge all intervals overlapping `[low, high)` into clipped ranges.
 *
 * The output stack is cleared before use. Adjacent ranges are coalesced, so
 * `[10, 20)` followed by `[20, 30)` yields `[10, 30)`.
 *
 * @param tree       Pointer to the interval tree.
 * @param lo         Query lower bound.
 * @param hi         Query upper bound.
 * @param ranges_ptr Pointer to `n00b_stack_t(n00b_interval_range_t)`.
 *
 * @return `Ok(0)` or `Err(N00B_INTERVAL_ERR_INVALID)`.
 */
#define n00b_interval_merge_ranges(tree, lo, hi, ranges_ptr)                                   \
    ({                                                                                         \
        auto _imr_t = (tree);                                                                  \
        uint64_t _imr_lo = (lo);                                                               \
        uint64_t _imr_hi = (hi);                                                               \
        auto _imr_ranges = (ranges_ptr);                                                       \
        bool _imr_err = false;                                                                 \
                                                                                               \
        if (_imr_t == nullptr || _imr_ranges == nullptr || _imr_lo > _imr_hi) {                \
            _imr_err = true;                                                                   \
        } else {                                                                               \
            n00b_stack_clear(*_imr_ranges);                                                    \
            if (_imr_lo != _imr_hi) {                                                          \
                n00b_stack_t(void *) _imr_hits = n00b_stack_new_private(                       \
                    void *, .allocator = _imr_t->allocator);                                   \
                auto _imr_search = n00b_interval_search_ordered(                               \
                    _imr_t, _imr_lo, _imr_hi, &_imr_hits);                                     \
                                                                                               \
                if (n00b_result_is_err(_imr_search)) {                                         \
                    _imr_err = true;                                                           \
                } else {                                                                       \
                    bool _imr_have = false;                                                    \
                    uint64_t _imr_cur_low = 0;                                                 \
                    uint64_t _imr_cur_high = 0;                                                \
                                                                                               \
                    n00b_stack_foreach(_imr_hits, _imr_hit) {                                  \
                        auto _imr_node = (typeof(_imr_t->root))*_imr_hit;                      \
                        uint64_t _imr_nlow = _imr_node->low < _imr_lo                          \
                                           ? _imr_lo : _imr_node->low;                         \
                        uint64_t _imr_nhigh = _imr_node->high > _imr_hi                        \
                                            ? _imr_hi : _imr_node->high;                       \
                                                                                               \
                        if (_imr_nlow >= _imr_nhigh) {                                        \
                            continue;                                                          \
                        }                                                                      \
                                                                                               \
                        if (!_imr_have) {                                                      \
                            _imr_cur_low = _imr_nlow;                                          \
                            _imr_cur_high = _imr_nhigh;                                        \
                            _imr_have = true;                                                  \
                        } else if (_imr_nlow <= _imr_cur_high) {                               \
                            if (_imr_nhigh > _imr_cur_high) {                                  \
                                _imr_cur_high = _imr_nhigh;                                    \
                            }                                                                  \
                        } else {                                                               \
                            n00b_stack_push(*_imr_ranges,                                      \
                                            ((n00b_interval_range_t){                          \
                                                .low = _imr_cur_low,                           \
                                                .high = _imr_cur_high,                         \
                                            }));                                               \
                            _imr_cur_low = _imr_nlow;                                          \
                            _imr_cur_high = _imr_nhigh;                                        \
                        }                                                                      \
                    }                                                                          \
                                                                                               \
                    if (_imr_have) {                                                           \
                        n00b_stack_push(*_imr_ranges,                                          \
                                        ((n00b_interval_range_t){                              \
                                            .low = _imr_cur_low,                               \
                                            .high = _imr_cur_high,                             \
                                        }));                                                   \
                    }                                                                          \
                }                                                                              \
                                                                                               \
                n00b_stack_free(_imr_hits);                                                    \
            }                                                                                  \
        }                                                                                      \
                                                                                               \
        _imr_err ? n00b_result_err(int, N00B_INTERVAL_ERR_INVALID)                             \
                 : n00b_result_ok(int, 0);                                                     \
    })

// ============================================================================
// Next low
// ============================================================================

/** @brief Find the node whose low bound is the smallest value >= @p point. */
#define n00b_interval_next_low(tree, point)                                                    \
    ({                                                                                         \
        auto _inl_t = (tree);                                                                  \
        typedef typeof(_inl_t->root) _inl_np;                                                  \
        uint64_t _inl_pt = (point);                                                            \
        _inl_np _inl_found = nullptr;                                                          \
                                                                                               \
        n00b_data_read_lock(_inl_t->lock);                                                     \
        if (_inl_t->root != nullptr) {                                                         \
            _inl_np _inl_n = _inl_t->root;                                                     \
            _inl_np _inl_prev = nullptr;                                                       \
            while (_inl_n != nullptr) {                                                        \
                if (_inl_pt == _inl_n->low) {                                                  \
                    _inl_found = _inl_n;                                                       \
                    break;                                                                     \
                }                                                                              \
                if (_inl_pt < _inl_n->low) {                                                   \
                    if (_inl_n->left != nullptr) {                                             \
                        _inl_prev = _inl_n;                                                    \
                        _inl_n = _inl_n->left;                                                 \
                        continue;                                                              \
                    }                                                                          \
                    _inl_found = _inl_n;                                                       \
                    break;                                                                     \
                }                                                                              \
                if (_inl_n->right == nullptr) {                                                \
                    _inl_found = _inl_prev;                                                    \
                    break;                                                                     \
                }                                                                              \
                _inl_n = _inl_n->right;                                                        \
            }                                                                                  \
        }                                                                                      \
        n00b_data_unlock(_inl_t->lock);                                                        \
                                                                                               \
        n00b_result_ok(void *, (void *)_inl_found);                                            \
    })

// ============================================================================
// Delete
// ============================================================================

/** @brief Delete a specific node from the tree. */
#define n00b_interval_delete(tree, target)                                                     \
    ({                                                                                         \
        auto _id_tree = (tree);                                                                \
        typedef typeof(_id_tree->root) _id_np;                                                 \
        _id_np _id_target = (_id_np)(target);                                                  \
        n00b_result_t(int) _id_result;                                                         \
                                                                                               \
        if (_id_tree == nullptr || _id_target == nullptr                                       \
            || _id_tree->root == nullptr) {                                                    \
            _id_result = n00b_result_err(int, N00B_INTERVAL_ERR_NOT_FOUND);                    \
        } else {                                                                               \
            n00b_data_write_lock(_id_tree->lock);                                              \
            n00b_stack_t(void *) _id_stack = n00b_stack_new_cap_private(     \
                void *, 256, .allocator = _id_tree->allocator);                \
            _id_np _id_cur = _id_tree->root;                                                    \
                                                                                               \
            /* Walk BST to find target by pointer identity. */                                 \
            while (_id_cur != _id_target) {                                                    \
                if (_id_cur == nullptr) {                                                      \
                    int _id_retrying = 0;                                                      \
                    while (n00b_stack_len(_id_stack) > 0) {                              \
                        void *_id_top = n00b_option_get(                                       \
                            n00b_stack_pop(void *, _id_stack));                          \
                        if ((uintptr_t)_id_top & 1) {                                         \
                            _id_cur = (_id_np)(                                                \
                                (uintptr_t)_id_top & ~(uintptr_t)1);                           \
                            n00b_stack_push(_id_stack, (void *)_id_cur);                 \
                            _id_cur = _id_cur->left;                                           \
                            _id_retrying = 1;                                                  \
                            break;                                                             \
                        }                                                                      \
                    }                                                                          \
                    if (!_id_retrying) {                                                       \
                        _id_cur = nullptr;                                                     \
                        break;                                                                 \
                    }                                                                          \
                    continue;                                                                  \
                }                                                                              \
                if (_id_target->low < _id_cur->low) {                                          \
                    n00b_stack_push(_id_stack, (void *)_id_cur);                         \
                    _id_cur = _id_cur->left;                                                   \
                } else if (_id_target->low > _id_cur->low) {                                   \
                    n00b_stack_push(_id_stack, (void *)_id_cur);                         \
                    _id_cur = _id_cur->right;                                                  \
                } else {                                                                       \
                    n00b_stack_push(_id_stack,                                           \
                        (void *)((uintptr_t)_id_cur | 1));                                     \
                    _id_cur = _id_cur->right;                                                  \
                }                                                                              \
            }                                                                                  \
                                                                                               \
            if (_id_cur != _id_target) {                                                       \
                n00b_stack_free(_id_stack);                                              \
                n00b_data_unlock(_id_tree->lock);                                              \
                _id_result = n00b_result_err(int, N00B_INTERVAL_ERR_NOT_FOUND);                \
            } else if (_id_target->left != nullptr                                             \
                       && _id_target->right != nullptr) {                                      \
                /* Two children: in-order successor. */                                        \
                n00b_stack_push(_id_stack, (void *)_id_target);                          \
                _id_np _id_succ = _id_target->right;                                            \
                while (_id_succ->left != nullptr) {                                            \
                    n00b_stack_push(_id_stack, (void *)_id_succ);                        \
                    _id_succ = _id_succ->left;                                                 \
                }                                                                              \
                _id_np _id_succ_child = _id_succ->right;                                       \
                _id_np _id_subtree = _id_succ_child;                                           \
                _id_np _id_old = _id_succ;                                                     \
                _id_np _id_par;                                                                \
                                                                                               \
                while (n00b_stack_len(_id_stack) > 0) {                                  \
                    _id_par = (_id_np)((uintptr_t)n00b_option_get(                             \
                        n00b_stack_pop(void *, _id_stack))                               \
                        & ~(uintptr_t)1);                                                      \
                    if (_id_par == _id_target) {                                               \
                        n00b_stack_push(_id_stack, (void *)_id_target);                  \
                        break;                                                                 \
                    }                                                                          \
                    if (_id_par->left == _id_old)                                              \
                        _id_par->left = _id_subtree;                                           \
                    else                                                                       \
                        _id_par->right = _id_subtree;                                          \
                    _id_old = _id_par;                                                         \
                    _id_subtree = _n00b_itree_balance_node(_id_par);                           \
                }                                                                              \
                                                                                               \
                _id_succ->left = _id_target->left;                                             \
                _id_succ->right = (_id_old == _id_succ)                                        \
                    ? _id_succ_child : _id_subtree;                                            \
                _n00b_itree_update_node(_id_succ);                                             \
                _id_subtree = _n00b_itree_balance_node(_id_succ);                              \
                _id_old = _id_target;                                                          \
                                                                                               \
                (void)n00b_stack_pop(void *, _id_stack);                                 \
                                                                                               \
                while (n00b_stack_len(_id_stack) > 0) {                                  \
                    _id_par = (_id_np)((uintptr_t)n00b_option_get(                             \
                        n00b_stack_pop(void *, _id_stack))                               \
                        & ~(uintptr_t)1);                                                      \
                    if (_id_par->left == _id_old)                                              \
                        _id_par->left = _id_subtree;                                           \
                    else                                                                       \
                        _id_par->right = _id_subtree;                                          \
                    _id_old = _id_par;                                                         \
                    _id_subtree = _n00b_itree_balance_node(_id_par);                           \
                }                                                                              \
                _id_tree->root = _id_subtree;                                                  \
                n00b_stack_free(_id_stack);                                              \
                n00b_data_unlock(_id_tree->lock);                                              \
                if (_id_tree->allocator != nullptr                                            \
                    && _id_tree->allocator->free != nullptr) {                                \
                    (*_id_tree->allocator->free)(_id_tree->allocator, _id_target);            \
                }                                                                              \
                _id_result = n00b_result_ok(int, 0);                                           \
            } else {                                                                           \
                /* Zero or one child. */                                                       \
                _id_np _id_child =                                                             \
                    _id_target->left != nullptr                                                \
                        ? _id_target->left : _id_target->right;                                \
                _id_np _id_subtree2 = _id_child;                                               \
                _id_np _id_old2 = _id_target;                                                  \
                                                                                               \
                while (n00b_stack_len(_id_stack) > 0) {                                  \
                    _id_np _id_par2;                                                           \
                    _id_par2 = (_id_np)((uintptr_t)n00b_option_get(                            \
                        n00b_stack_pop(void *, _id_stack))                               \
                        & ~(uintptr_t)1);                                                      \
                    if (_id_par2->left == _id_old2)                                            \
                        _id_par2->left = _id_subtree2;                                         \
                    else                                                                       \
                        _id_par2->right = _id_subtree2;                                        \
                    _id_old2 = _id_par2;                                                       \
                    _id_subtree2 = _n00b_itree_balance_node(_id_par2);                         \
                }                                                                              \
                _id_tree->root = _id_subtree2;                                                 \
                n00b_stack_free(_id_stack);                                              \
                n00b_data_unlock(_id_tree->lock);                                              \
                if (_id_tree->allocator != nullptr                                            \
                    && _id_tree->allocator->free != nullptr) {                                \
                    (*_id_tree->allocator->free)(_id_tree->allocator, _id_target);            \
                }                                                                              \
                _id_result = n00b_result_ok(int, 0);                                           \
            }                                                                                  \
        }                                                                                      \
        _id_result;                                                                            \
    })
