#include "text/strings/format.h"
#include "text/strings/string_ops.h"
#include "text/strings/fmt_numbers.h"
#include "text/unicode/encoding.h"
#include "internal/text/unicode/raw.h"
#include "core/alloc.h"
#include "core/memory_info.h"
#include "core/string.h"
#include "core/type_info.h"
#include <string.h>

// ===================================================================
// Style stack
// ===================================================================

#define MAX_STYLE_DEPTH 32

typedef struct {
    n00b_text_style_t *styles[MAX_STYLE_DEPTH];
    int                depth;
} style_stack_t;

static void
style_push(style_stack_t *ss, n00b_text_style_t *s)
{
    assert(ss->depth < MAX_STYLE_DEPTH);
    ss->styles[ss->depth++] = s;
}

static void
style_pop(style_stack_t *ss)
{
    if (ss->depth > 0) {
        ss->depth--;
    }
}

static void
style_clear(style_stack_t *ss)
{
    ss->depth = 0;
}

static n00b_text_style_t *
style_current(style_stack_t *ss)
{
    if (ss->depth == 0) {
        return nullptr;
    }

    // Merge all stacked styles from bottom to top.
    n00b_text_style_t *acc = n00b_str_style_copy(ss->styles[0]);
    for (int i = 1; i < ss->depth; i++) {
        n00b_text_style_t *merged = n00b_str_style_merge(acc, ss->styles[i]);
        n00b_free(acc);
        acc = merged;
    }
    return acc;
}

// ===================================================================
// Output buffer
// ===================================================================

typedef struct {
    char   *buf;
    int32_t len;
    int32_t cap;
} outbuf_t;

// Maximum bytes we'll consume from a single substitution.  Anything
// larger almost certainly means the caller passed a raw C string
// where an n00b_string_t * was expected (see
// MEMORY.md → feedback_printf_cstring_trap.md): the fallback path
// would interpret the C string bytes as a struct header and read a
// garbage u8_bytes field.  64 MiB is well above any legitimate single
// substitution we expect.
#define OUTBUF_MAX_APPEND ((int32_t)(64 * 1024 * 1024))

static bool
format_bounds_for_heap_alloc(n00b_alloc_info_t info,
                             char             **start,
                             uint64_t          *len)
{
    if (info.kind == n00b_alloc_oob) {
        *start = (char *)info.hdr.oob->hcur;
        *len   = info.hdr.oob->alloc_len;
        return true;
    }
    if (info.kind == n00b_alloc_inline) {
        *start = (char *)info.hdr.in_line;
        *len   = info.hdr.in_line->alloc_len;
        return true;
    }

    return false;
}

static bool
format_arg_has_string_shape(n00b_string_t *s)
{
    if (s == nullptr) {
        return false;
    }
    if (n00b_value_is_data(s)) {
        return false;
    }

    return s->data != nullptr || s->u8_bytes == 0;
}

static bool
format_arg_is_plausible_string(n00b_string_t *s)
{
    if (!format_arg_has_string_shape(s)) {
        return false;
    }
    if (s->u8_bytes > (size_t)OUTBUF_MAX_APPEND) {
        return false;
    }
    if (s->codepoints > s->u8_bytes) {
        return false;
    }
    if (s->u8_bytes == 0) {
        return true;
    }
    if (s->data == nullptr) {
        return false;
    }

    // A raw C string passed by mistake makes the format engine read bytes from
    // the C string as an n00b_string_t header; its "data" word is usually
    // garbage or another unmanaged value. Real n00b_string_t instances point at
    // payload bytes, not at the start of another managed allocation header.
    bool              pin       = false;
    n00b_alloc_info_t dinfo     = n00b_find_alloc_info(s->data, .pin = &pin);
    bool              plausible = !n00b_value_is_data(s->data);
    if (n00b_alloc_info_is_heap(dinfo)) {
        char    *alloc_start = nullptr;
        uint64_t alloc_len   = 0;
        plausible = format_bounds_for_heap_alloc(dinfo, &alloc_start, &alloc_len)
                 && (char *)s->data > alloc_start
                 && ((char *)s->data + s->u8_bytes) <= (alloc_start + alloc_len);
    }
    n00b_alloc_hdr_unpin(pin);

    return plausible;
}

static void
outbuf_append(outbuf_t *ob, const char *data, int32_t data_len)
{
    // Defend against int32 overflow in the sizing loop.  Reject
    // implausibly large data_len (negative or > our hard cap) — the
    // alternative is a hang, since `while (new_cap < ob->len +
    // data_len) new_cap *= 2;` never terminates when data_len wraps
    // negative.
    if (data_len <= 0 || data_len > OUTBUF_MAX_APPEND) {
        return;
    }
    int64_t needed = (int64_t)ob->len + (int64_t)data_len;
    if (needed > OUTBUF_MAX_APPEND) {
        return;
    }
    if ((int32_t)needed > ob->cap) {
        int64_t new_cap = ob->cap ? (int64_t)ob->cap * 2 : 256;
        while (new_cap < needed) {
            new_cap *= 2;
        }
        if (new_cap > OUTBUF_MAX_APPEND) {
            new_cap = OUTBUF_MAX_APPEND;
        }
        char *new_buf = n00b_alloc_array(char, (int32_t)new_cap);
        if (ob->buf) {
            memcpy(new_buf, ob->buf, ob->len);
            n00b_free(ob->buf);
        }
        ob->buf = new_buf;
        ob->cap = (int32_t)new_cap;
    }
    memcpy(ob->buf + ob->len, data, data_len);
    ob->len += data_len;
}

// ===================================================================
// Style record collector
// ===================================================================

typedef struct {
    n00b_style_record_t *records;
    int32_t              count;
    int32_t              cap;
} rec_list_t;

static void
rec_push(rec_list_t *rl, n00b_text_style_t *style, int32_t start)
{
    if (rl->count >= rl->cap) {
        int32_t new_cap            = rl->cap ? rl->cap * 2 : 16;
        n00b_style_record_t *new_r = n00b_alloc_array(n00b_style_record_t,
                                                       new_cap);
        if (rl->records) {
            memcpy(new_r, rl->records,
                   rl->count * sizeof(n00b_style_record_t));
            n00b_free(rl->records);
        }
        rl->records = new_r;
        rl->cap     = new_cap;
    }

    rl->records[rl->count++] = (n00b_style_record_t){
        .info  = style,
        .tag   = nullptr,
        .start = (size_t)start,
        .end   = n00b_option_none(size_t),
    };
}

// Close the most recently opened record that has no end set.
static void
rec_close_last(rec_list_t *rl, int32_t end_pos)
{
    for (int i = rl->count - 1; i >= 0; i--) {
        if (!n00b_option_is_set(rl->records[i].end)) {
            rl->records[i].end = n00b_option_set(size_t, (size_t)end_pos);
            return;
        }
    }
}

// ===================================================================
// Formatting engine
// ===================================================================

static void
append_text_with_style(outbuf_t *ob, rec_list_t *rl,
                       style_stack_t *ss,
                       const char *text, int32_t text_len)
{
    if (text_len <= 0) {
        return;
    }

    n00b_text_style_t *cur = style_current(ss);
    int32_t start          = ob->len;
    outbuf_append(ob, text, text_len);

    if (cur && !n00b_str_style_is_empty(cur)) {
        rec_push(rl, cur, start);
        // We'll close it immediately since it spans exactly this text.
        rl->records[rl->count - 1].end =
            n00b_option_set(size_t, (size_t)ob->len);
    }
    else if (cur) {
        n00b_free(cur);
    }
}

n00b_string_t *
_n00b_format_impl(const char *desc_data, int32_t desc_len,
                  n00b_vargs_t *vargs)
{
    n00b_rich_desc_t *parsed = n00b_rich_desc_parse(desc_data, desc_len);

    outbuf_t      ob = {};
    rec_list_t    rl = {};
    style_stack_t ss = {};

    for (int32_t i = 0; i < parsed->num_segments; i++) {
        n00b_rich_segment_t *seg = &parsed->segments[i];

        switch (seg->kind) {
        case N00B_RICH_TEXT:
            append_text_with_style(&ob, &rl, &ss,
                                   desc_data + seg->offset, seg->length);
            break;

        case N00B_RICH_STYLE_ON: {
            auto s_opt = n00b_str_style_lookup(seg->tag);
            if (n00b_option_is_set(s_opt)) {
                style_push(&ss, n00b_option_get(s_opt));
            }
            break;
        }
        case N00B_RICH_STYLE_OFF:
            style_pop(&ss);
            break;

        case N00B_RICH_PROP_ON: {
            n00b_text_style_t *ps = n00b_str_style_new();
            if (seg->length == -2) {
                // Text case tag.
                ps->text_case = (n00b_text_case_t)seg->offset;
            }
            else {
                // Tristate property.
                n00b_tristate_t *field =
                    (n00b_tristate_t *)((char *)ps + seg->offset);
                *field = N00B_TRI_YES;
            }
            style_push(&ss, ps);
            break;
        }
        case N00B_RICH_PROP_OFF:
            style_pop(&ss);
            break;

        case N00B_RICH_ROLE_ON: {
            auto rs_opt = n00b_str_role_lookup(seg->tag);
            if (n00b_option_is_set(rs_opt)) {
                style_push(&ss, n00b_option_get(rs_opt));
            }
            break;
        }
        case N00B_RICH_ROLE_OFF:
            style_pop(&ss);
            break;

        case N00B_RICH_RESET:
            // Close all open style records.
            for (int j = 0; j < rl.count; j++) {
                if (!n00b_option_is_set(rl.records[j].end)) {
                    rl.records[j].end =
                        n00b_option_set(size_t, (size_t)ob.len);
                }
            }
            style_clear(&ss);
            break;

        case N00B_RICH_SUBST: {
            // Get the argument.
            int arg_idx = seg->offset;
            if (!vargs || (unsigned)arg_idx >= vargs->nargs) {
                break;
            }
            void *arg = vargs->args[arg_idx];

            n00b_string_t *sub_str = nullptr;
            bool           have_str = false;

            if (seg->tag) {
                // Has format spec.
                n00b_format_spec_t fs =
                    n00b_format_spec_parse(seg->tag,
                                            (int)strlen(seg->tag));
                switch (fs.type) {
                case 'd':
                case 'i':
                case 'u':
                case 'x':
                case 'o':
                    sub_str  = n00b_str_fmt_int_ex((int64_t)arg, &fs);
                    have_str = true;
                    break;
                case 'f':
                case 'e':
                case 'g': {
                    double val = *(double *)arg;
                    sub_str    = n00b_str_fmt_float_ex(val, &fs);
                    have_str   = true;
                    break;
                }
                case 's': {
                    n00b_string_t *sp = (n00b_string_t *)arg;
                    sub_str           = n00b_str_fmt_string_ex(sp, &fs);
                    have_str          = true;
                    break;
                }
                case 'b': {
                    sub_str = n00b_fmt_bool((bool)(uintptr_t)arg,
                                            .upper = fs.upper,
                                            .word  = fs.word,
                                            .yn    = fs.yn);
                    have_str = true;
                    break;
                }
                case 'p': {
                    sub_str  = n00b_fmt_pointer(arg, .caps = fs.upper);
                    have_str = true;
                    break;
                }
                default:
                    // Default to string interpretation.
                    if (arg) {
                        sub_str  = (n00b_string_t *)arg;
                        have_str = true;
                    }
                    break;
                }
            }
            else {
                // No format spec: introspect the argument.
                //
                // 1. If arg is NULL or looks like a small integer
                //    (not inside any known memory mapping), format as
                //    int64_t.
                // 2. If arg points into the managed heap and has a
                //    TO_STRING vtable slot, call it.
                // 3. If it's a managed object without TO_STRING,
                //    format as a pointer.
                // 4. Otherwise (unmapped / data), format as int64_t.
                if (!arg) {
                    sub_str  = n00b_fmt_int((int64_t)0);
                    have_str = true;
                }
                else if (n00b_value_is_data(arg)) {
                    sub_str  = n00b_fmt_int((int64_t)arg);
                    have_str = true;
                }
                else {
                    n00b_option_t(n00b_vtable_entry) to_str_opt =
                        n00b_obj_core_method(arg, N00B_BI_TO_STRING);

                    if (n00b_option_is_set(to_str_opt)) {
                        typedef n00b_string_t *(*to_string_fn)(void *);
                        sub_str  = ((to_string_fn)n00b_option_get(to_str_opt))(arg);
                        have_str = true;
                    }
                    else {
                        n00b_string_t *sp = (n00b_string_t *)arg;

                        if (format_arg_is_plausible_string(sp)) {
                            sub_str = sp;
                        }
                        else if (format_arg_has_string_shape(sp)
                                 && sp->u8_bytes > (size_t)OUTBUF_MAX_APPEND) {
                            sub_str = r"<bad-string-arg>";
                        }
                        else {
                            // No resolvable type. Keep raw/unmanaged pointers
                            // opaque instead of treating arbitrary bytes as an
                            // n00b_string_t header.
                            sub_str = n00b_fmt_pointer(arg);
                        }
                        have_str = true;
                    }
                }
            }

            if (have_str) {
                if (seg->strip_style) {
                    // Strip styling from substituted value.
                    sub_str = n00b_str_strip_styles(sub_str);
                }
                append_text_with_style(&ob, &rl, &ss,
                                       sub_str->data,
                                       (int32_t)sub_str->u8_bytes);
            }
            break;
        }
        }
    }

    // Close any remaining open style records.
    for (int j = 0; j < rl.count; j++) {
        if (!n00b_option_is_set(rl.records[j].end)) {
            rl.records[j].end = n00b_option_none(size_t);
        }
    }

    // Build result string.
    int64_t total_cps = n00b_unicode_utf8_count_codepoints_raw(ob.buf, ob.len);
    if (total_cps < 0) {
        total_cps = 0;
    }
    n00b_string_t *result = n00b_string_from_raw(ob.buf, ob.len);

    // Attach style info if we have records.
    if (rl.count > 0) {
        n00b_string_style_info_t *info =
            n00b_alloc_flex(n00b_string_style_info_t, n00b_style_record_t,
                            rl.count);
        info->num_styles = rl.count;
        memcpy(info->styles, rl.records,
               rl.count * sizeof(n00b_style_record_t));
        result->styling = info;
    }

    if (ob.buf) {
        n00b_free(ob.buf);
    }
    if (rl.records) {
        n00b_free(rl.records);
    }

    return result;
}

// ===================================================================
// Public API
// ===================================================================

n00b_string_t *
n00b_format(n00b_string_t *desc, +)
{
    return _n00b_format_impl(desc->data, (int32_t)desc->u8_bytes, vargs);
}

n00b_string_t *
n00b_cformat(const char *desc, +)
{
    int32_t             len      = (int32_t)strlen(desc);
    n00b_allocator_t   *resolved = nullptr;
    n00b_string_scope_t scope    = n00b_string_scope_enter(&resolved);
    /* Push allocator override so every inner alloc inside
     * _n00b_format_impl that doesn't pass an explicit allocator
     * routes through the scratch (or override) we just resolved. */
    n00b_allocator_t   *prev     = n00b_push_current_allocator(resolved);
    n00b_string_t      *result   = _n00b_format_impl(desc, len, vargs);
    n00b_restore_current_allocator(prev);
    return n00b_string_scope_exit(&scope, result);
}
