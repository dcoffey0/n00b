#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "n00b.h"
#include "core/alloc.h"
#include "adt/array.h"
#include "adt/list.h"
#include "core/runtime.h"
#include "core/string.h"
#include "display/render/canvas.h"
#include "display/render/plane.h"
#include "display/render/draw_cmd.h"
#include "display/backend_stream_internal.h"
#include "display/render/box.h"
#include "display/render/composite.h"
#include "display/render/types.h"
#include "display/widget.h"
#include "text/strings/string_style.h"
#include "text/strings/theme.h"
#include "text/strings/text_style.h"
#include "text/strings/string_ops.h"

// ====================================================================
// Tests
// ====================================================================

static void
test_canvas_new_destroy(void)
{
    n00b_canvas_t *c = n00b_new_kargs(n00b_canvas_t, canvas, .vtable = &n00b_renderer_stream);
    assert(c != nullptr);
    assert(c->vtable == &n00b_renderer_stream);
    assert(c->frame_rows > 0);
    assert(c->frame_cols > 0);

    n00b_canvas_destroy(c);
    printf("  [PASS] canvas new and destroy\n");
}

static void
test_canvas_init_caller_owned_storage(void)
{
    n00b_canvas_t canvas;
    memset(&canvas, 0xA5, sizeof(canvas));

    n00b_canvas_init(&canvas, .vtable = &n00b_renderer_stream);

    assert(canvas.vtable == &n00b_renderer_stream);
    assert(canvas.backend_ctx != nullptr);
    assert(canvas.cell_px_w == 1);
    assert(canvas.cell_px_h == 1);
    assert(canvas.frame_rows == 25);
    assert(canvas.frame_cols == 80);

    n00b_canvas_deinit(&canvas);
    printf("  [PASS] canvas init resets caller-owned storage\n");
}

static void
test_canvas_add_remove_plane(void)
{
    n00b_canvas_t *c = n00b_new_kargs(n00b_canvas_t, canvas, .vtable = &n00b_renderer_stream);
    n00b_plane_t  *p = n00b_new_kargs(n00b_plane_t, plane);

    n00b_canvas_add_plane(c, p);
    assert(n00b_list_len(c->planes) == 1);

    bool removed = n00b_canvas_remove_plane(c, p);
    assert(removed);
    assert(n00b_list_len(c->planes) == 0);

    // Remove non-existent.
    removed = n00b_canvas_remove_plane(c, p);
    assert(!removed);

    n00b_plane_destroy(p);
    n00b_canvas_destroy(c);
    printf("  [PASS] canvas add/remove plane\n");
}

static void
test_canvas_render_empty(void)
{
    n00b_canvas_t *c = n00b_new_kargs(n00b_canvas_t, canvas, .vtable = &n00b_renderer_stream);

    n00b_canvas_render(c);

    n00b_string_t *buf = n00b_stream_backend_get_buffer(c->backend_ctx);
    assert(buf->data != nullptr);
    // Empty frame should produce only spaces/newlines.

    n00b_canvas_destroy(c);
    printf("  [PASS] canvas render empty\n");
}

static void
test_canvas_render_single_plane(void)
{
    n00b_canvas_t *c = n00b_new_kargs(n00b_canvas_t, canvas, .vtable = &n00b_renderer_stream);

    // Set stream backend to small size.
    n00b_stream_backend_set_size(c->backend_ctx, 5, 10);
    n00b_canvas_resize(c, 5, 10);

    n00b_plane_t *p = n00b_new_kargs(n00b_plane_t, plane);
    p->width = 10;
    p->height = 5;
    n00b_plane_draw_text(p, 0, 0, n00b_string_from_cstr("Hi"));

    n00b_canvas_add_plane(c, p);
    n00b_canvas_render(c);

    n00b_string_t *buf = n00b_stream_backend_get_buffer(c->backend_ctx);
    assert(buf->data != nullptr);
    // First two chars of first line should be "Hi".
    assert(buf->data[0] == 'H');
    assert(buf->data[1] == 'i');

    n00b_plane_destroy(p);
    n00b_canvas_destroy(c);
    printf("  [PASS] canvas render single plane\n");
}

static void
test_canvas_z_order(void)
{
    n00b_canvas_t *c = n00b_new_kargs(n00b_canvas_t, canvas, .vtable = &n00b_renderer_stream);
    n00b_stream_backend_set_size(c->backend_ctx, 5, 10);
    n00b_canvas_resize(c, 5, 10);

    // Back plane (z=0) writes 'B' at (0,0).
    n00b_plane_t *back = n00b_new_kargs(n00b_plane_t, plane, .z = 0);
    back->width = 10;
    back->height = 5;
    n00b_plane_draw_glyph(back, 0, 0, 'B');

    // Front plane (z=1) writes 'F' at (0,0).
    n00b_plane_t *front = n00b_new_kargs(n00b_plane_t, plane, .z = 1);
    front->width = 10;
    front->height = 5;
    n00b_plane_draw_glyph(front, 0, 0, 'F');

    n00b_canvas_add_plane(c, back);
    n00b_canvas_add_plane(c, front);
    n00b_canvas_render(c);

    n00b_string_t *buf = n00b_stream_backend_get_buffer(c->backend_ctx);
    // Front plane should overwrite back plane.
    assert(buf->data[0] == 'F');

    n00b_plane_destroy(back);
    n00b_plane_destroy(front);
    n00b_canvas_destroy(c);
    printf("  [PASS] canvas z-order compositing\n");
}

static void
test_canvas_plane_offset(void)
{
    n00b_canvas_t *c = n00b_new_kargs(n00b_canvas_t, canvas, .vtable = &n00b_renderer_stream);
    n00b_stream_backend_set_size(c->backend_ctx, 5, 20);
    n00b_canvas_resize(c, 5, 20);

    n00b_plane_t *p = n00b_new_kargs(n00b_plane_t, plane);
    p->width = 5;
    p->height = 3;
    n00b_plane_move(p, 3, 1);  // x=3, y=1
    n00b_plane_draw_glyph(p, 0, 0, 'X');

    n00b_canvas_add_plane(c, p);
    n00b_canvas_render(c);

    n00b_string_t *buf = n00b_stream_backend_get_buffer(c->backend_ctx);
    // 'X' should be at frame position (row=1, col=3).
    assert(n00b_unicode_str_contains(buf, r"X"));

    n00b_plane_destroy(p);
    n00b_canvas_destroy(c);
    printf("  [PASS] canvas plane offset\n");
}

static void
test_canvas_invalidate(void)
{
    n00b_canvas_t *c = n00b_new_kargs(n00b_canvas_t, canvas, .vtable = &n00b_renderer_stream);

    n00b_canvas_render(c);
    assert(!c->needs_full_redraw);

    n00b_canvas_invalidate(c);
    assert(c->needs_full_redraw);

    n00b_canvas_destroy(c);
    printf("  [PASS] canvas invalidate\n");
}

typedef struct {
    int render_count;
} metrics_refresh_backend_ctx_t;

static int32_t
metrics_refresh_backend_text_width(void *ctx,
                                   n00b_string_t *text,
                                   n00b_text_style_t *style)
{
    (void)ctx;
    (void)text;
    (void)style;
    return 777;
}

static int32_t
metrics_refresh_backend_line_height(void *ctx, n00b_text_style_t *style)
{
    (void)ctx;
    (void)style;
    return 99;
}

static int32_t
metrics_refresh_backend_ascent(void *ctx, n00b_text_style_t *style)
{
    (void)ctx;
    (void)style;
    return 88;
}

static void *
metrics_refresh_backend_init(n00b_conduit_topic_t(n00b_buffer_t *) *output)
{
    (void)output;
    metrics_refresh_backend_ctx_t *ctx = calloc(1, sizeof(*ctx));
    assert(ctx != nullptr);
    return ctx;
}

static void
metrics_refresh_backend_destroy(void *vctx)
{
    free(vctx);
}

static n00b_render_cap_t
metrics_refresh_backend_caps(void *vctx)
{
    (void)vctx;
    return N00B_RCAP_NONE;
}

static n00b_render_size_t
metrics_refresh_backend_get_size(void *vctx)
{
    metrics_refresh_backend_ctx_t *ctx = vctx;
    if (ctx->render_count == 0) {
        return (n00b_render_size_t){
            .rows = 6,
            .cols = 8,
            .cell_pixel_w = 1,
            .cell_pixel_h = 1,
        };
    }

    return (n00b_render_size_t){
        .rows = 6,
        .cols = 8,
        .cell_pixel_w = 2,
        .cell_pixel_h = 3,
    };
}

static void
metrics_refresh_backend_render_frame(void *vctx,
                                     n00b_rcell_t *cells,
                                     n00b_isize_t rows,
                                     n00b_isize_t cols,
                                     n00b_rcell_t *prev_cells)
{
    (void)vctx;
    (void)cells;
    (void)rows;
    (void)cols;
    (void)prev_cells;
}

static void
metrics_refresh_backend_flush(void *vctx)
{
    (void)vctx;
}

static void
metrics_refresh_backend_render_planes(void *vctx,
                                      const n00b_composite_entry_t *entries,
                                      n00b_isize_t count,
                                      n00b_isize_t total_rows,
                                      n00b_isize_t total_cols,
                                      n00b_text_style_t *default_style,
                                      n00b_render_cap_t caps)
{
    metrics_refresh_backend_ctx_t *ctx = vctx;
    (void)entries;
    (void)count;
    (void)total_rows;
    (void)total_cols;
    (void)default_style;
    (void)caps;
    ctx->render_count++;
}

static n00b_font_metrics_provider_t
metrics_refresh_backend_get_font_metrics(void *vctx)
{
    return (n00b_font_metrics_provider_t){
        .text_width = metrics_refresh_backend_text_width,
        .line_height = metrics_refresh_backend_line_height,
        .ascent = metrics_refresh_backend_ascent,
        .ctx = vctx,
    };
}

static const n00b_renderer_vtable_t metrics_refresh_backend = {
    .name = "metrics_refresh_backend",
    .version = N00B_RENDERER_ABI_VERSION,
    .init = metrics_refresh_backend_init,
    .destroy = metrics_refresh_backend_destroy,
    .capabilities = metrics_refresh_backend_caps,
    .get_size = metrics_refresh_backend_get_size,
    .render_frame = metrics_refresh_backend_render_frame,
    .flush = metrics_refresh_backend_flush,
    .render_planes = metrics_refresh_backend_render_planes,
    .get_font_metrics = metrics_refresh_backend_get_font_metrics,
};

static void
test_canvas_refreshes_fallback_metrics_when_font_metrics_cap_absent(void)
{
    n00b_canvas_t *canvas = n00b_new_kargs(n00b_canvas_t,
                                           canvas,
                                           .vtable = &metrics_refresh_backend);

    assert(canvas->metrics.line_height(canvas->metrics.ctx, nullptr) == 1);
    assert(canvas->metrics.text_width(canvas->metrics.ctx,
                                      n00b_string_from_cstr("ab"),
                                      nullptr) == 2);

    n00b_canvas_render(canvas);
    assert(canvas->metrics.line_height(canvas->metrics.ctx, nullptr) == 1);

    n00b_canvas_render(canvas);
    assert(canvas->cell_px_w == 2);
    assert(canvas->cell_px_h == 3);
    assert(canvas->metrics.line_height(canvas->metrics.ctx, nullptr) == 3);
    assert(canvas->metrics.text_width(canvas->metrics.ctx,
                                      n00b_string_from_cstr("ab"),
                                      nullptr) == 4);

    n00b_canvas_destroy(canvas);
    printf("  [PASS] canvas refreshes fallback metrics without font metrics cap\n");
}

static void
test_composite_flatten(void)
{
    n00b_plane_t *p1 = n00b_new_kargs(n00b_plane_t, plane, .z = 2);
    p1->width = 10;
    p1->height = 5;
    n00b_plane_t *p2 = n00b_new_kargs(n00b_plane_t, plane, .z = 0);
    p2->width = 10;
    p2->height = 5;
    n00b_plane_t *p3 = n00b_new_kargs(n00b_plane_t, plane, .z = 1);
    p3->width = 10;
    p3->height = 5;

    n00b_plane_t *planes[] = { p1, p2, p3 };

    n00b_array_t(n00b_composite_entry_t) entries =
        n00b_composite_flatten(planes, 3, 1, 1);

    assert(n00b_array_len(entries) == 3);
    // Should be sorted by z: 0, 1, 2.
    assert(entries.data[0].abs_z == 0);
    assert(entries.data[1].abs_z == 1);
    assert(entries.data[2].abs_z == 2);

    n00b_array_free(entries);
    n00b_plane_destroy(p1);
    n00b_plane_destroy(p2);
    n00b_plane_destroy(p3);
    printf("  [PASS] composite flatten and z-sort\n");
}

static void
test_canvas_widget_state_styling(void)
{
    n00b_canvas_t *c = n00b_new_kargs(n00b_canvas_t, canvas, .vtable = &n00b_renderer_stream);
    n00b_stream_backend_set_size(c->backend_ctx, 5, 10);
    n00b_canvas_resize(c, 5, 10);

    n00b_plane_t *p = n00b_new_kargs(n00b_plane_t, plane);
    p->width = 10;
    p->height = 5;
    n00b_plane_draw_glyph(p, 0, 0, 'S');

    // Set up a box with state-based styling.
    n00b_box_props_t *box = n00b_box_props_new(
        .theme = &n00b_border_plain
    );

    n00b_text_style_t focused_style = { .bold = N00B_TRI_YES };
    n00b_state_style_t focused_state = { .text_style = &focused_style };
    box->state_styles[N00B_WSTATE_FOCUSED] = &focused_state;

    n00b_plane_set_box(p, box);
    n00b_plane_set_state(p, N00B_WSTATE_FOCUSED);

    n00b_canvas_add_plane(c, p);
    n00b_canvas_render(c);

    // Just verify it doesn't crash and produces output.
    n00b_string_t *buf = n00b_stream_backend_get_buffer(c->backend_ctx);
    assert(buf->data != nullptr);
    assert(n00b_stream_backend_get_length(c->backend_ctx) > 0);

    n00b_plane_destroy(p);
    n00b_canvas_destroy(c);
    printf("  [PASS] canvas widget state styling\n");
}

static void
test_composite_honors_string_style_ranges(void)
{
    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane);
    n00b_plane_t *planes[] = { plane };
    n00b_array_t(n00b_composite_entry_t) entries;
    n00b_composite_style_pool_t style_pool = {};
    n00b_text_style_t default_style = {};
    n00b_text_style_t base_style = {
        .font_index = -1,
        .fg_palette_ix = -1,
        .bg_palette_ix = -1,
        .fg_rgb = n00b_color_make(0x112233),
    };
    n00b_text_style_t selection_style = {
        .font_index = -1,
        .fg_palette_ix = N00B_PAL_SELECTION_FG,
        .bg_palette_ix = N00B_PAL_SELECTION_BG,
    };
    n00b_string_t *text = n00b_string_from_cstr("AB");
    n00b_rcell_t   frame[2] = {};

    plane->width = 2;
    plane->height = 1;

    text = n00b_str_set_base_style(text, &base_style);
    text = n00b_str_add_style(text,
                              &selection_style,
                              1,
                              n00b_option_set(size_t, 2));
    n00b_plane_draw_text(plane, 0, 0, text);

    entries = n00b_composite_flatten(planes, 1, 1, 1);
    n00b_composite_commands_to_grid(entries.data,
                                    (n00b_isize_t)entries.len,
                                    frame,
                                    1,
                                    2,
                                    1,
                                    1,
                                    &default_style,
                                    N00B_RCAP_UNICODE,
                                    &style_pool);

    assert(frame[0].style != nullptr);
    assert(frame[1].style != nullptr);
    assert(frame[0].style->fg_rgb == base_style.fg_rgb);
    assert(frame[0].style->bg_palette_ix == -1);
    assert(frame[1].style->fg_palette_ix == N00B_PAL_SELECTION_FG);
    assert(frame[1].style->bg_palette_ix == N00B_PAL_SELECTION_BG);

    n00b_composite_style_pool_destroy(&style_pool);
    n00b_array_free(entries);
    n00b_plane_destroy(plane);
    printf("  [PASS] composite honors string style ranges\n");
}

static void
test_composite_equal_cells_ignore_style_pointer_identity(void)
{
    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane);
    n00b_plane_t *planes[] = { plane };
    n00b_array_t(n00b_composite_entry_t) entries;
    n00b_composite_style_pool_t pool_a = {};
    n00b_composite_style_pool_t pool_b = {};
    n00b_text_style_t default_style = {};
    n00b_text_style_t base_style = {
        .font_index    = -1,
        .fg_palette_ix = -1,
        .bg_palette_ix = -1,
        .fg_rgb        = n00b_color_make(0x112233),
    };
    n00b_text_style_t accent_style = {
        .font_index    = -1,
        .fg_palette_ix = -1,
        .bg_palette_ix = -1,
        .fg_rgb        = n00b_color_make(0x99aa55),
    };
    n00b_string_t *text = n00b_string_from_cstr("AB");
    n00b_rcell_t frame_a[2] = {};
    n00b_rcell_t frame_b[2] = {};

    plane->width = 2;
    plane->height = 1;

    text = n00b_str_set_base_style(text, &base_style);
    text = n00b_str_add_style(text,
                              &accent_style,
                              1,
                              n00b_option_set(size_t, 2));
    n00b_plane_draw_text(plane, 0, 0, text);

    entries = n00b_composite_flatten(planes, 1, 1, 1);
    n00b_composite_commands_to_grid(entries.data,
                                    (n00b_isize_t)entries.len,
                                    frame_a,
                                    1,
                                    2,
                                    1,
                                    1,
                                    &default_style,
                                    N00B_RCAP_UNICODE,
                                    &pool_a);
    n00b_composite_commands_to_grid(entries.data,
                                    (n00b_isize_t)entries.len,
                                    frame_b,
                                    1,
                                    2,
                                    1,
                                    1,
                                    &default_style,
                                    N00B_RCAP_UNICODE,
                                    &pool_b);

    assert(frame_a[0].style != nullptr);
    assert(frame_b[0].style != nullptr);
    assert(frame_a[0].style != frame_b[0].style);
    assert(frame_a[1].style != nullptr);
    assert(frame_b[1].style != nullptr);
    assert(frame_a[1].style != frame_b[1].style);
    assert(n00b_rcell_equal(&frame_a[0], &frame_b[0]));
    assert(n00b_rcell_equal(&frame_a[1], &frame_b[1]));

    n00b_composite_style_pool_destroy(&pool_a);
    n00b_composite_style_pool_destroy(&pool_b);
    n00b_array_free(entries);
    n00b_plane_destroy(plane);
    printf("  [PASS] composite equal cells ignore style pointer identity\n");
}

static void
test_stream_backend_style_pool_does_not_accumulate_across_frames(void)
{
    n00b_canvas_t *canvas = n00b_new_kargs(n00b_canvas_t, canvas,
                                           .vtable = &n00b_renderer_stream);
    n00b_text_style_t base_style = {
        .font_index    = -1,
        .fg_palette_ix = -1,
        .bg_palette_ix = -1,
        .fg_rgb        = n00b_color_make(0x112233),
    };
    n00b_text_style_t accent_style = {
        .font_index    = -1,
        .fg_palette_ix = -1,
        .bg_palette_ix = -1,
        .fg_rgb        = n00b_color_make(0x99aa55),
    };
    n00b_string_t *text = n00b_string_from_cstr("AB");
    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane);
    size_t first_count;
    size_t second_count;

    n00b_stream_backend_set_size(canvas->backend_ctx, 1, 2);
    n00b_canvas_resize(canvas, 1, 2);

    plane->width = 2;
    plane->height = 1;
    text = n00b_str_set_base_style(text, &base_style);
    text = n00b_str_add_style(text,
                              &accent_style,
                              1,
                              n00b_option_set(size_t, 2));
    n00b_plane_draw_text(plane, 0, 0, text);
    n00b_canvas_add_plane(canvas, plane);

    n00b_canvas_render(canvas);
    first_count = n00b_stream_backend_get_style_pool_count(canvas->backend_ctx);
    assert(first_count > 0);

    n00b_canvas_invalidate(canvas);
    n00b_canvas_render(canvas);
    second_count = n00b_stream_backend_get_style_pool_count(canvas->backend_ctx);
    assert(second_count == first_count);

    n00b_plane_destroy(plane);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] stream backend style pool does not accumulate across frames\n");
}

static void
test_canvas_nested_planes(void)
{
    n00b_canvas_t *c = n00b_new_kargs(n00b_canvas_t, canvas, .vtable = &n00b_renderer_stream);
    n00b_stream_backend_set_size(c->backend_ctx, 10, 20);
    n00b_canvas_resize(c, 10, 20);

    // Parent at (2,1).
    n00b_plane_t *parent = n00b_new_kargs(n00b_plane_t, plane);
    parent->width = 15;
    parent->height = 8;
    n00b_plane_move(parent, 2, 1);
    n00b_plane_draw_glyph(parent, 0, 0, 'P');

    // Child at (3,2) relative to parent -> absolute (5,3).
    n00b_plane_t *child = n00b_new_kargs(n00b_plane_t, plane);
    child->width = 5;
    child->height = 3;
    n00b_plane_draw_glyph(child, 0, 0, 'C');
    n00b_plane_add_child(parent, child, 3, 2);

    n00b_canvas_add_plane(c, parent);
    n00b_canvas_render(c);

    n00b_string_t *buf = n00b_stream_backend_get_buffer(c->backend_ctx);
    assert(buf->data != nullptr);
    // Both 'P' and 'C' should appear.
    assert(n00b_unicode_str_contains(buf, r"P"));
    assert(n00b_unicode_str_contains(buf, r"C"));

    n00b_plane_destroy(child);
    n00b_plane_destroy(parent);
    n00b_canvas_destroy(c);
    printf("  [PASS] canvas nested planes\n");
}

static void
test_canvas_nested_z_order(void)
{
    n00b_canvas_t *c = n00b_new_kargs(n00b_canvas_t, canvas, .vtable = &n00b_renderer_stream);
    n00b_stream_backend_set_size(c->backend_ctx, 5, 10);
    n00b_canvas_resize(c, 5, 10);

    // Parent with child that overlaps at same position.
    n00b_plane_t *parent = n00b_new_kargs(n00b_plane_t, plane);
    parent->width = 10;
    parent->height = 5;
    n00b_plane_draw_glyph(parent, 0, 0, 'P');

    // Child at (0,0), z=1 higher, overwrites parent.
    n00b_plane_t *child = n00b_new_kargs(n00b_plane_t, plane, .z = 1);
    child->width = 10;
    child->height = 5;
    n00b_plane_draw_glyph(child, 0, 0, 'C');
    n00b_plane_add_child(parent, child, 0, 0);

    n00b_canvas_add_plane(c, parent);
    n00b_canvas_render(c);

    n00b_string_t *buf = n00b_stream_backend_get_buffer(c->backend_ctx);
    // Child (z=1) overwrites parent (z=0) at position (0,0).
    assert(buf->data[0] == 'C');

    n00b_plane_destroy(child);
    n00b_plane_destroy(parent);
    n00b_canvas_destroy(c);
    printf("  [PASS] canvas nested z-order\n");
}

static void
test_canvas_child_clipping(void)
{
    n00b_canvas_t *c = n00b_new_kargs(n00b_canvas_t, canvas, .vtable = &n00b_renderer_stream);
    n00b_stream_backend_set_size(c->backend_ctx, 10, 20);
    n00b_canvas_resize(c, 10, 20);

    // Parent plane, positioned at (0,0).
    n00b_plane_t *parent = n00b_new_kargs(n00b_plane_t, plane);
    parent->width = 10;
    parent->height = 5;

    // Child plane positioned at (8,0) relative to parent.
    // This means the child extends from absolute col 8 to col 17.
    // But parent only goes to col 9 -- child should be clipped.
    n00b_plane_t *child = n00b_new_kargs(n00b_plane_t, plane);
    child->width = 10;
    child->height = 5;
    n00b_plane_draw_glyph(child, 0, 0, 'X');
    n00b_plane_add_child(parent, child, 8, 0);

    n00b_canvas_add_plane(c, parent);
    n00b_canvas_render(c);

    n00b_string_t *buf = n00b_stream_backend_get_buffer(c->backend_ctx);
    // 'X' at child relative (0,0) maps to absolute (8,0).
    // Parent clip is (0,0)-(10,5), so (8,0) is in bounds.
    assert(buf->data != nullptr);

    n00b_plane_destroy(child);
    n00b_plane_destroy(parent);
    n00b_canvas_destroy(c);
    printf("  [PASS] canvas child clipping\n");
}

static void
test_composite_flatten_nested(void)
{
    n00b_plane_t *parent = n00b_new_kargs(n00b_plane_t, plane, .z = 0);
    parent->width = 20;
    parent->height = 10;
    n00b_plane_move(parent, 5, 5);

    n00b_plane_t *child1 = n00b_new_kargs(n00b_plane_t, plane, .z = 1);
    child1->width = 5;
    child1->height = 3;
    n00b_plane_add_child(parent, child1, 2, 1);

    n00b_plane_t *child2 = n00b_new_kargs(n00b_plane_t, plane, .z = 2);
    child2->width = 5;
    child2->height = 3;
    n00b_plane_add_child(parent, child2, 8, 4);

    n00b_plane_t *planes[] = { parent };

    n00b_array_t(n00b_composite_entry_t) entries =
        n00b_composite_flatten(planes, 1, 1, 1);

    assert(n00b_array_len(entries) == 3); // parent + 2 children.

    // Sorted by z: parent(0), child1(0+1+1=2), child2(0+1+2=3).
    // Children get parent's abs_z + 1 as base, plus their own z offset.
    assert(entries.data[0].abs_z == 0);
    assert(entries.data[0].plane == parent);
    assert(entries.data[0].abs_x == 5);
    assert(entries.data[0].abs_y == 5);

    assert(entries.data[1].abs_z == 2);  // parent(0) + 1 + child1.z(1)
    assert(entries.data[1].plane == child1);
    assert(entries.data[1].abs_x == 7);  // 5 + 2
    assert(entries.data[1].abs_y == 6);  // 5 + 1

    assert(entries.data[2].abs_z == 3);  // parent(0) + 1 + child2.z(2)
    assert(entries.data[2].plane == child2);
    assert(entries.data[2].abs_x == 13); // 5 + 8
    assert(entries.data[2].abs_y == 9);  // 5 + 4

    n00b_array_free(entries);
    n00b_plane_destroy(child1);
    n00b_plane_destroy(child2);
    n00b_plane_destroy(parent);
    printf("  [PASS] composite flatten nested with absolute coords\n");
}

static void
test_composite_commands_quantize_partial_cells(void)
{
    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane);
    plane->width = 2;
    plane->height = 2;
    n00b_plane_move(plane, 0, 5);
    n00b_plane_fill_rect(plane, 0, 0, 2, 2, .cp = '#');

    n00b_plane_t *planes[] = { plane };
    n00b_array_t(n00b_composite_entry_t) entries =
        n00b_composite_flatten(planes, 1, 2, 2);
    n00b_composite_style_pool_t style_pool = {};

    n00b_rcell_t frame[24];
    memset(frame, 0, sizeof(frame));
    n00b_text_style_t default_style = {0};

    n00b_composite_commands_to_grid(entries.data,
                                    (n00b_isize_t)entries.len,
                                    frame,
                                    6,
                                    4,
                                    2,
                                    2,
                                    &default_style,
                                    N00B_RCAP_NONE,
                                    &style_pool);

    assert(frame[2 * 4].grapheme[0] == '#');
    assert(frame[3 * 4].grapheme[0] == '#');

    n00b_composite_style_pool_destroy(&style_pool);
    n00b_array_free(entries);
    n00b_plane_destroy(plane);
    printf("  [PASS] composite commands quantize partial cells\n");
}

static void
test_canvas_layout_move_updates_descendant_render_origin(void)
{
    n00b_canvas_t *c = n00b_new_kargs(n00b_canvas_t, canvas,
                                      .vtable = &n00b_renderer_stream);
    n00b_stream_backend_set_size(c->backend_ctx, 6, 12);
    n00b_canvas_resize(c, 6, 12);

    n00b_plane_t *parent = n00b_new_kargs(n00b_plane_t, plane);
    n00b_plane_t *child = n00b_new_kargs(n00b_plane_t, plane);

    n00b_plane_draw_glyph(child, 0, 0, 'X');
    n00b_plane_add_child(parent, child, 0, 0);

    n00b_widget_layout(parent,
                       (n00b_rect_t){
                           .x = 0,
                           .y = 0,
                           .width = 8,
                           .height = 4,
                       });
    n00b_widget_layout(child,
                       (n00b_rect_t){
                           .x = 2,
                           .y = 1,
                           .width = 1,
                           .height = 1,
                       });
    n00b_plane_move(parent, 4, 2);

    n00b_plane_t *planes[] = { parent };
    n00b_array_t(n00b_composite_entry_t) entries =
        n00b_composite_flatten(planes, 1, 1, 1);

    assert(entries.len == 2);
    assert(entries.data[1].plane == child);
    assert(entries.data[1].abs_x == 6);
    assert(entries.data[1].abs_y == 3);

    n00b_canvas_add_plane(c, parent);
    n00b_canvas_render(c);

    n00b_string_t *buf = n00b_stream_backend_get_buffer(c->backend_ctx);
    assert(buf->data != nullptr);
    assert(n00b_unicode_str_contains(buf, r"X"));

    n00b_array_free(entries);
    n00b_plane_destroy(child);
    n00b_plane_destroy(parent);
    n00b_canvas_destroy(c);
    printf("  [PASS] canvas layout move updates descendant render origin\n");
}

static void
test_canvas_descendant_z_can_overlay_later_sibling(void)
{
    n00b_canvas_t *c = n00b_new_kargs(n00b_canvas_t, canvas,
                                      .vtable = &n00b_renderer_stream);
    n00b_stream_backend_set_size(c->backend_ctx, 5, 10);
    n00b_canvas_resize(c, 5, 10);

    n00b_plane_t *parent = n00b_new_kargs(n00b_plane_t, plane);
    parent->width = 10;
    parent->height = 5;

    n00b_plane_t *high_branch = n00b_new_kargs(n00b_plane_t, plane);
    high_branch->width = 10;
    high_branch->height = 5;
    n00b_plane_t *high_wrap = n00b_new_kargs(n00b_plane_t, plane);
    high_wrap->width = 10;
    high_wrap->height = 5;
    n00b_plane_t *high_leaf = n00b_new_kargs(n00b_plane_t, plane, .z = 10);
    high_leaf->width = 10;
    high_leaf->height = 5;
    n00b_plane_draw_glyph(high_leaf, 0, 0, 'H');
    n00b_plane_add_child(high_wrap, high_leaf, 0, 0);
    n00b_plane_add_child(high_branch, high_wrap, 0, 0);

    n00b_plane_t *low_branch = n00b_new_kargs(n00b_plane_t, plane, .z = 1);
    low_branch->width = 10;
    low_branch->height = 5;
    n00b_plane_draw_glyph(low_branch, 0, 0, 'L');

    n00b_plane_add_child(parent, high_branch, 0, 0);
    n00b_plane_add_child(parent, low_branch, 0, 0);

    n00b_canvas_add_plane(c, parent);
    n00b_canvas_render(c);

    n00b_string_t *buf = n00b_stream_backend_get_buffer(c->backend_ctx);
    assert(buf->data[0] == 'H');

    n00b_plane_destroy(high_leaf);
    n00b_plane_destroy(high_wrap);
    n00b_plane_destroy(high_branch);
    n00b_plane_destroy(low_branch);
    n00b_plane_destroy(parent);
    n00b_canvas_destroy(c);
    printf("  [PASS] canvas descendant z can overlay later sibling\n");
}

// ====================================================================
// Main
// ====================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running render canvas tests...\n");

    test_canvas_init_caller_owned_storage();
    test_canvas_new_destroy();
    test_canvas_add_remove_plane();
    test_canvas_render_empty();
    test_canvas_render_single_plane();
    test_canvas_z_order();
    test_canvas_plane_offset();
    test_canvas_invalidate();
    test_canvas_refreshes_fallback_metrics_when_font_metrics_cap_absent();
    test_composite_flatten();
    test_canvas_widget_state_styling();
    test_composite_honors_string_style_ranges();
    test_composite_equal_cells_ignore_style_pointer_identity();
    test_stream_backend_style_pool_does_not_accumulate_across_frames();
    test_canvas_nested_planes();
    test_canvas_nested_z_order();
    test_canvas_child_clipping();
    test_composite_flatten_nested();
    test_composite_commands_quantize_partial_cells();
    test_canvas_layout_move_updates_descendant_render_origin();
    test_canvas_descendant_z_can_overlay_later_sibling();

    printf("All render canvas tests passed.\n");
    n00b_shutdown();
    return 0;
}
