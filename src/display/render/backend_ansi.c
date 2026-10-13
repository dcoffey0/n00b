/*
 * ANSI terminal renderer backend.
 *
 * Full-featured terminal backend with:
 * - Differential rendering (only emit changed cells)
 * - SGR escape sequences for colors and decorations
 * - Cursor position tracking to minimize cursor movement
 * - Output buffering for efficiency
 */

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif
#else
#include <unistd.h>
#include <sys/ioctl.h>
#include <poll.h>
#endif
#include "n00b.h"
#include "core/alloc.h"
#include "display/render/backend.h"
#include "display/event.h"
#include "conduit/write.h"
#include "conduit/signal.h"
#include "text/strings/text_style.h"
#include "display/render/composite.h"
#include "internal/display/diagnostics.h"
#include "internal/display/ansi_sgr.h"
#include "internal/display/terminal_input.h"

// -------------------------------------------------------------------
// ANSI context
// -------------------------------------------------------------------

typedef struct {
    char                                   *buf;
    size_t                                  buf_size;
    size_t                                  buf_used;
    int                                     fd;
    n00b_conduit_topic_t(n00b_buffer_t *)  *output;
    n00b_isize_t                            rows;
    n00b_isize_t                            cols;
    n00b_isize_t                            cursor_row;
    n00b_isize_t                            cursor_col;
    bool                                    cursor_visible;
    n00b_terminal_input_state_t             input_state;
#ifndef _WIN32
    n00b_conduit_signal_inbox_t            *sigwinch_inbox;
#endif

    // Persistent compositing grid.
    n00b_rcell_t                           *comp_grid;
    n00b_isize_t                            comp_grid_rows;
    n00b_isize_t                            comp_grid_cols;
    n00b_composite_style_pool_t             style_pool;
} ansi_ctx_t;

#define ANSI_INITIAL_BUF 16384

static const char ansi_base64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// -------------------------------------------------------------------
// Output buffer helpers
// -------------------------------------------------------------------

static void
ansi_ensure(ansi_ctx_t *ctx, size_t needed)
{
    if (ctx->buf_used + needed < ctx->buf_size) {
        return;
    }

    while (ctx->buf_used + needed >= ctx->buf_size) {
        ctx->buf_size *= 2;
    }

    char *new_buf = n00b_alloc_array_with_opts(char, ctx->buf_size, &(n00b_alloc_opts_t){.no_scan = true});
    memcpy(new_buf, ctx->buf, ctx->buf_used);
    n00b_free(ctx->buf);
    ctx->buf = new_buf;
}

static void
ansi_emit(ansi_ctx_t *ctx, const char *data, size_t len)
{
    ansi_ensure(ctx, len);
    memcpy(ctx->buf + ctx->buf_used, data, len);
    ctx->buf_used += len;
}

static void
ansi_emit_str(ansi_ctx_t *ctx, const char *str)
{
    ansi_emit(ctx, str, strlen(str));
}

static void
ansi_emit_base64(ansi_ctx_t *ctx, const uint8_t *data, size_t len)
{
    size_t i;

    for (i = 0; i + 3 <= len; i += 3) {
        char chunk[4];
        uint32_t block = ((uint32_t)data[i] << 16)
                       | ((uint32_t)data[i + 1] << 8)
                       | (uint32_t)data[i + 2];

        chunk[0] = ansi_base64[(block >> 18) & 0x3f];
        chunk[1] = ansi_base64[(block >> 12) & 0x3f];
        chunk[2] = ansi_base64[(block >> 6) & 0x3f];
        chunk[3] = ansi_base64[block & 0x3f];
        ansi_emit(ctx, chunk, sizeof(chunk));
    }

    if (i < len) {
        char chunk[4];
        uint32_t block = (uint32_t)data[i] << 16;

        if (i + 1 < len) {
            block |= (uint32_t)data[i + 1] << 8;
        }

        chunk[0] = ansi_base64[(block >> 18) & 0x3f];
        chunk[1] = ansi_base64[(block >> 12) & 0x3f];
        chunk[2] = (i + 1 < len) ? ansi_base64[(block >> 6) & 0x3f] : '=';
        chunk[3] = '=';
        ansi_emit(ctx, chunk, sizeof(chunk));
    }
}

static void
ansi_sgr_emit_adapter(void *vctx, const char *data, size_t len)
{
    ansi_ctx_t *ctx = vctx;
    ansi_emit(ctx, data, len);
}

static void
ansi_emit_int(ansi_ctx_t *ctx, int val)
{
    char tmp[16];
    int len = snprintf(tmp, sizeof(tmp), "%d", val);
    ansi_emit(ctx, tmp, len);
}

// -------------------------------------------------------------------
// SGR escape generation
// -------------------------------------------------------------------


// -------------------------------------------------------------------
// Cursor movement
// -------------------------------------------------------------------

static void
ansi_move_cursor(ansi_ctx_t *ctx, n00b_isize_t row, n00b_isize_t col)
{
    if (ctx->cursor_row == row && ctx->cursor_col == col) {
        return;
    }

    // CSI row;col H (1-indexed).
    ansi_emit_str(ctx, "\033[");
    ansi_emit_int(ctx, (int)(row + 1));
    ansi_emit_str(ctx, ";");
    ansi_emit_int(ctx, (int)(col + 1));
    ansi_emit_str(ctx, "H");

    ctx->cursor_row = row;
    ctx->cursor_col = col;
}

// -------------------------------------------------------------------
// Vtable implementation
// -------------------------------------------------------------------

static void *
ansi_init(n00b_conduit_topic_t(n00b_buffer_t *) *output)
{
    ansi_ctx_t *ctx = n00b_alloc_with_opts(ansi_ctx_t, &(n00b_alloc_opts_t){.no_scan = true});
    ctx->fd         = STDOUT_FILENO;
    ctx->output     = output;
    ctx->buf_size   = ANSI_INITIAL_BUF;
    ctx->buf        = n00b_alloc_array_with_opts(char, ANSI_INITIAL_BUF, &(n00b_alloc_opts_t){.no_scan = true});
    ctx->buf_used   = 0;
    ctx->cursor_visible = true;
    n00b_terminal_input_reset(&ctx->input_state);

#ifdef _WIN32
    ctx->cols = 80;
    ctx->rows = 25;
#else
    // Try to get terminal size.
    struct winsize ws;
    if (ioctl(ctx->fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        ctx->cols = ws.ws_col;
        ctx->rows = ws.ws_row;
    }
    else {
        ctx->cols = 80;
        ctx->rows = 25;
    }
#endif

#ifndef _WIN32
    {
        // Subscribe to SIGWINCH via the conduit signal system.
        if (!output) {
            n00b_display_diag_log(N00B_DISPLAY_DIAG_TRACE,
                                   "backend_ansi",
                                   "init: output topic is null; skipping SIGWINCH");
        }
        else {
            n00b_conduit_topic_base_t *base = (n00b_conduit_topic_base_t *)output;
            n00b_conduit_t *c = base->conduit;
            n00b_display_diag_log(N00B_DISPLAY_DIAG_TRACE,
                                   "backend_ansi",
                                   "init: output=%p conduit=%p",
                                   (void *)output,
                                   (void *)c);
            if (c) {
                n00b_result_t(n00b_conduit_topic_base_t *) sr =
                    n00b_conduit_signal_topic(c, SIGWINCH);
                if (n00b_result_is_ok(sr)) {
                    ctx->sigwinch_inbox = n00b_conduit_signal_inbox_new(c);
                    n00b_conduit_signal_subscribe(
                        n00b_result_get(sr), ctx->sigwinch_inbox,
                        .operations = N00B_CONDUIT_SIGNAL_ALL);
                    n00b_display_diag_log(N00B_DISPLAY_DIAG_TRACE,
                                           "backend_ansi",
                                           "init: SIGWINCH subscribed inbox=%p",
                                           (void *)ctx->sigwinch_inbox);
                }
                else {
                    n00b_display_diag_log(N00B_DISPLAY_DIAG_ERROR,
                                           "backend_ansi",
                                           "init: failed to fetch SIGWINCH signal topic");
                }
            }
            else {
                n00b_display_diag_log(N00B_DISPLAY_DIAG_TRACE,
                                       "backend_ansi",
                                       "init: conduit is null");
            }
        }
    }
#endif

    return ctx;
}

static void
ansi_destroy(void *vctx)
{
    ansi_ctx_t *ctx = vctx;
    if (ctx) {
#ifndef _WIN32
        if (ctx->sigwinch_inbox) {
            n00b_conduit_topic_base_t *base =
                (n00b_conduit_topic_base_t *)ctx->output;
            if (base && base->conduit) {
                n00b_conduit_signal_unwatch(base->conduit, SIGWINCH);
            }
        }
#endif
        if (ctx->comp_grid) {
            n00b_free(ctx->comp_grid);
        }
        n00b_composite_style_pool_destroy(&ctx->style_pool);
        n00b_free(ctx->buf);
        n00b_free(ctx);
    }
}

static n00b_render_cap_t
ansi_capabilities(void *vctx)
{
    (void)vctx;
    return N00B_RCAP_COLOR_BASIC
         | N00B_RCAP_COLOR_256
         | N00B_RCAP_COLOR_24BIT
         | N00B_RCAP_BOLD
         | N00B_RCAP_ITALIC
         | N00B_RCAP_UNDERLINE
         | N00B_RCAP_STRIKETHROUGH
         | N00B_RCAP_DIM
         | N00B_RCAP_CURSOR_MOVE
         | N00B_RCAP_ALT_SCREEN
         | N00B_RCAP_UNICODE
         | N00B_RCAP_WIDE_CHARS
         | N00B_RCAP_DIFF_RENDER
         | N00B_RCAP_MOUSE;
}

static n00b_render_size_t
ansi_get_size(void *vctx)
{
    ansi_ctx_t *ctx = vctx;

#ifndef _WIN32
    // Re-query terminal size.
    struct winsize ws;
    if (ioctl(ctx->fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        ctx->cols = ws.ws_col;
        ctx->rows = ws.ws_row;
    }
#endif

    return (n00b_render_size_t){
        .rows = ctx->rows,
        .cols = ctx->cols,
    };
}

static void
ansi_render_frame(void *vctx, n00b_rcell_t *cells,
                  n00b_isize_t rows, n00b_isize_t cols,
                  n00b_rcell_t *prev_cells)
{
    ansi_ctx_t *ctx = vctx;
    ctx->buf_used = 0;

    // Invalidate tracked cursor so the first cell always gets an
    // explicit CUP.  Without this, if the terminal's real cursor
    // isn't at (0,0) (e.g. after alt-screen + mouse-enable sequences),
    // the backend skips the move and row 0 renders at the wrong spot.
    ctx->cursor_row = -1;
    ctx->cursor_col = -1;

    const n00b_text_style_t *last_style = nullptr;

    for (n00b_isize_t r = 0; r < rows; r++) {
        for (n00b_isize_t c = 0; c < cols; c++) {
            n00b_rcell_t *cell = &cells[r * cols + c];

            if (cell->flags & N00B_CELL_WIDE_CONT) {
                continue;
            }

            // Diff render: skip unchanged cells.
            if (prev_cells) {
                n00b_rcell_t *prev = &prev_cells[r * cols + c];
                if (n00b_rcell_equal(cell, prev)) {
                    continue;
                }
            }

            // Move cursor to this cell.
            ansi_move_cursor(ctx, r, c);

            // Emit style if changed.
            if (cell->style != last_style) {
                n00b_display_ansi_emit_style(cell->style, ansi_sgr_emit_adapter, ctx);
                last_style = cell->style;
            }

            // Emit grapheme.
            if (cell->flags & N00B_CELL_OCCUPIED) {
                ansi_emit(ctx, cell->grapheme, cell->grapheme_len);
            }
            else {
                ansi_emit(ctx, " ", 1);
            }

            ctx->cursor_col += cell->display_width ? cell->display_width : 1;
        }
    }

    // Reset style at end.
    n00b_display_ansi_emit_reset(ansi_sgr_emit_adapter, ctx);
}

static void
ansi_flush(void *vctx)
{
    ansi_ctx_t *ctx = vctx;

    if (ctx->buf_used == 0) {
        return;
    }

    if (ctx->output) {
        n00b_buffer_t *buf = n00b_buffer_from_bytes(ctx->buf, (int64_t)ctx->buf_used);
        n00b_write(n00b_buffer_t *, ctx->output, buf);
    }

    ctx->buf_used = 0;
}

static bool
ansi_clipboard_copy(void *vctx, const char *utf8, size_t len)
{
    ansi_ctx_t *ctx = vctx;

    if (!ctx || !utf8) {
        return false;
    }

    ansi_emit_str(ctx, "\033]52;c;");
    ansi_emit_base64(ctx, (const uint8_t *)utf8, len);
    ansi_emit_str(ctx, "\a");
    return true;
}

// -------------------------------------------------------------------
// Plane-based rendering
// -------------------------------------------------------------------

static void
ansi_render_planes(void                         *vctx,
                   const n00b_composite_entry_t *entries,
                   n00b_isize_t                  count,
                   n00b_isize_t                  total_rows,
                   n00b_isize_t                  total_cols,
                   n00b_text_style_t            *default_style,
                   n00b_render_cap_t             caps)
{
    ansi_ctx_t *ctx = vctx;

    // Reuse persistent grid; reallocate only on size change.
    if (total_rows != ctx->comp_grid_rows
        || total_cols != ctx->comp_grid_cols) {
        if (ctx->comp_grid) {
            n00b_free(ctx->comp_grid);
        }
        size_t total = (size_t)total_rows * total_cols;
        ctx->comp_grid = n00b_alloc_array(n00b_rcell_t, total);
        ctx->comp_grid_rows = total_rows;
        ctx->comp_grid_cols = total_cols;
    }

    n00b_composite_style_pool_clear(&ctx->style_pool);
    n00b_composite_commands_to_grid(entries, count, ctx->comp_grid,
                                     total_rows, total_cols,
                                     1, 1,
                                     default_style, caps,
                                     &ctx->style_pool);

    ansi_render_frame(vctx, ctx->comp_grid, total_rows, total_cols, nullptr);
}

static void
ansi_cursor_set_visible(void *vctx, bool visible)
{
    ansi_ctx_t *ctx = vctx;
    if (visible) {
        ansi_emit_str(ctx, "\033[?25h");
    }
    else {
        ansi_emit_str(ctx, "\033[?25l");
    }
    ctx->cursor_visible = visible;
    ansi_flush(vctx);
}

static void
ansi_cursor_move(void *vctx, n00b_isize_t row, n00b_isize_t col)
{
    ansi_ctx_t *ctx = vctx;
    ansi_move_cursor(ctx, row, col);
    ansi_flush(vctx);
}

static void
ansi_alt_screen_enter(void *vctx)
{
    ansi_ctx_t *ctx = vctx;
    ansi_emit_str(ctx, "\033[?1049h");
    ansi_flush(vctx);
}

static void
ansi_alt_screen_leave(void *vctx)
{
    ansi_ctx_t *ctx = vctx;
    ansi_emit_str(ctx, "\033[?1049l");
    ansi_flush(vctx);
}

// -------------------------------------------------------------------
// Event polling
// -------------------------------------------------------------------

#ifndef _WIN32

// Try to read one byte from stdin (non-blocking).
// Returns the byte, or -1 if nothing available.
static int
ansi_try_read_byte(void)
{
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n == 1) {
        return (int)c;
    }
    return -1;
}

// Read one byte, waiting up to timeout_ms.  Uses poll() for sub-sequence
// reads (CSI/SS3 parsing) where we don't need SIGWINCH responsiveness.
static int
ansi_read_byte(int timeout_ms)
{
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
    // eintr: EINTR ends the wait as its timeout would, so a signal between the
    // bytes of an escape sequence splits it.
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0) {
        return -1;
    }
    return ansi_try_read_byte();
}

typedef struct {
    int pending;
} ansi_input_reader_t;

static int
ansi_read_for_parser(void *vctx, int32_t timeout_ms)
{
    ansi_input_reader_t *reader = vctx;
    if (reader->pending >= 0) {
        int c = reader->pending;
        reader->pending = -1;
        return c;
    }
    return ansi_read_byte(timeout_ms);
}

static bool
ansi_check_sigwinch(ansi_ctx_t *ctx, n00b_event_t *out)
{
    if (!ctx->sigwinch_inbox) {
        return false;
    }
    if (!n00b_conduit_signal_inbox_has_messages(ctx->sigwinch_inbox)) {
        return false;
    }
    n00b_display_diag_log(N00B_DISPLAY_DIAG_TRACE,
                           "backend_ansi",
                           "sigwinch: inbox has messages");
    while (n00b_conduit_signal_inbox_has_messages(ctx->sigwinch_inbox)) {
        n00b_conduit_signal_inbox_pop(ctx->sigwinch_inbox);
    }
    struct winsize ws;
    if (ioctl(ctx->fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        ctx->rows = (n00b_isize_t)ws.ws_row;
        ctx->cols = (n00b_isize_t)ws.ws_col;
        out->type        = N00B_EVENT_RESIZE;
        out->resize.rows = ctx->rows;
        out->resize.cols = ctx->cols;
        n00b_display_diag_log(N00B_DISPLAY_DIAG_TRACE,
                               "backend_ansi",
                               "sigwinch: resize=%dx%d",
                               (int)ws.ws_col,
                               (int)ws.ws_row);
        return true;
    }
    return false;
}

static bool
ansi_poll_event(void *vctx, int32_t timeout_ms, n00b_event_t *out)
{
    ansi_ctx_t *ctx = vctx;
    out->type = N00B_EVENT_NONE;

    // Check for pending SIGWINCH.
    if (ansi_check_sigwinch(ctx, out)) {
        return true;
    }

    // Try non-blocking read from stdin.
    int c = ansi_try_read_byte();

    if (c < 0) {
        // No stdin data yet.  Wait on the inbox CV so we wake
        // immediately when SIGWINCH arrives, rather than sleeping
        // the full tick.  The CV times out after timeout_ms, giving
        // us the same polling cadence as before for keyboard input.
        if (ctx->sigwinch_inbox && timeout_ms > 0) {
            n00b_condition_wait(&ctx->sigwinch_inbox->cv,
                                .timeout_ms = timeout_ms);
        }
        else if (timeout_ms > 0) {
            // No inbox — fall back to poll() on stdin.
            struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
            // eintr: an early wake only re-checks for input.
            poll(&pfd, 1, timeout_ms);
        }

        // Re-check SIGWINCH after waking.
        if (ansi_check_sigwinch(ctx, out)) {
            return true;
        }

        // Try stdin again.
        c = ansi_try_read_byte();
        if (c < 0) {
            return false;
        }
    }

    ansi_input_reader_t reader = { .pending = c };
    return n00b_terminal_parse_ansi_event(&ctx->input_state,
                                           ansi_read_for_parser,
                                           &reader,
                                           0,
                                           out);
}
#endif // !_WIN32

// -------------------------------------------------------------------
// Public vtable
// -------------------------------------------------------------------

const n00b_renderer_vtable_t n00b_renderer_ansi = {
    .name               = "ansi",
    .version            = N00B_RENDERER_ABI_VERSION,
    .init               = ansi_init,
    .destroy            = ansi_destroy,
    .capabilities       = ansi_capabilities,
    .get_size           = ansi_get_size,
    .render_frame       = ansi_render_frame,
    .flush              = ansi_flush,
    .render_planes      = ansi_render_planes,
    .clipboard_copy     = ansi_clipboard_copy,
    .cursor_set_visible = ansi_cursor_set_visible,
    .cursor_move        = ansi_cursor_move,
    .alt_screen_enter   = ansi_alt_screen_enter,
    .alt_screen_leave   = ansi_alt_screen_leave,
#ifndef _WIN32
    .poll_event         = ansi_poll_event,
#endif
};
