/*
 * Native X11 renderer backend (Linux/Unix GUI window).
 *
 * This backend provides a real windowed GUI path outside of terminal
 * emulators. It renders the composited cell grid into an X11 window and
 * translates X11 keyboard/mouse/resize events into n00b events.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include "n00b.h"
#include "core/alloc.h"
#include "display/event.h"
#include "display/render/backend.h"
#include "display/render/composite.h"
#include "internal/display/x11_backend_contracts.h"
#include "text/strings/theme.h"

#define X11_DEFAULT_COLS 80
#define X11_DEFAULT_ROWS 25
#define X11_DEFAULT_CELL_W 9
#define X11_DEFAULT_CELL_H 16
#define X11_EVENT_QUEUE_CAP 128

typedef struct {
    Display        *dpy;
    int             screen;
    Window          window;
    GC              gc;
    XFontStruct    *font;
    Colormap        colormap;
    Visual         *visual;
    Atom            wm_delete;
    Atom            clipboard_atom;
    Atom            targets_atom;
    Atom            utf8_string_atom;
    Atom            text_atom;

    bool            running;

    n00b_isize_t    rows;
    n00b_isize_t    cols;
    n00b_isize_t    pixel_w;
    n00b_isize_t    pixel_h;
    n00b_isize_t    cell_w;
    n00b_isize_t    cell_h;

    n00b_rcell_t   *comp_grid;
    n00b_isize_t    comp_grid_rows;
    n00b_isize_t    comp_grid_cols;
    n00b_composite_style_pool_t style_pool;

    n00b_event_t    event_queue[X11_EVENT_QUEUE_CAP];
    uint32_t        eq_head;
    uint32_t        eq_tail;
    char           *clipboard;
    size_t          clipboard_len;

    n00b_x11_pending_state_t pending;

    void          (*resize_cb)(n00b_isize_t, n00b_isize_t, void *);
    void           *resize_user_ctx;
} x11_ctx_t;

static inline bool
x11_queue_empty(const x11_ctx_t *ctx)
{
    return ctx->eq_head == ctx->eq_tail;
}

static void
x11_enqueue_event(x11_ctx_t *ctx, const n00b_event_t *ev)
{
    uint32_t next = (ctx->eq_head + 1u) % X11_EVENT_QUEUE_CAP;
    if (next == ctx->eq_tail) {
        ctx->eq_tail = (ctx->eq_tail + 1u) % X11_EVENT_QUEUE_CAP;
    }

    ctx->event_queue[ctx->eq_head] = *ev;
    ctx->eq_head = next;
}

static bool
x11_dequeue_event(x11_ctx_t *ctx, n00b_event_t *out)
{
    if (x11_queue_empty(ctx)) {
        return false;
    }

    *out = ctx->event_queue[ctx->eq_tail];
    ctx->eq_tail = (ctx->eq_tail + 1u) % X11_EVENT_QUEUE_CAP;
    return true;
}

static void
x11_free_clipboard(x11_ctx_t *ctx)
{
    if (!ctx || !ctx->clipboard) {
        return;
    }

    n00b_free(ctx->clipboard);
    ctx->clipboard = nullptr;
    ctx->clipboard_len = 0;
}

static bool
x11_clipboard_is_ascii(const x11_ctx_t *ctx)
{
    if (!ctx || !ctx->clipboard) {
        return true;
    }

    for (size_t i = 0; i < ctx->clipboard_len; i++) {
        if (((unsigned char)ctx->clipboard[i]) > 0x7fu) {
            return false;
        }
    }

    return true;
}

static void
x11_send_selection_notify(x11_ctx_t *ctx,
                          const XSelectionRequestEvent *request,
                          Atom property)
{
    XEvent reply = {};

    if (!ctx || !request) {
        return;
    }

    reply.xselection.type      = SelectionNotify;
    reply.xselection.display   = request->display;
    reply.xselection.requestor = request->requestor;
    reply.xselection.selection = request->selection;
    reply.xselection.target    = request->target;
    reply.xselection.property  = property;
    reply.xselection.time      = request->time;

    XSendEvent(ctx->dpy, request->requestor, False, 0, &reply);
    XFlush(ctx->dpy);
}

static void
x11_handle_selection_request(x11_ctx_t *ctx,
                             const XSelectionRequestEvent *request)
{
    Atom property;
    Atom reply_property = None;

    if (!ctx || !request) {
        return;
    }

    property = request->property ? request->property : request->target;

    if (!ctx->clipboard
        || (request->selection != ctx->clipboard_atom
            && request->selection != XA_PRIMARY)) {
        x11_send_selection_notify(ctx, request, None);
        return;
    }

    if (request->target == ctx->targets_atom) {
        Atom targets[4];
        int  count = 0;

        targets[count++] = ctx->targets_atom;
        targets[count++] = ctx->utf8_string_atom;
        targets[count++] = ctx->text_atom;
        if (x11_clipboard_is_ascii(ctx)) {
            targets[count++] = XA_STRING;
        }

        XChangeProperty(ctx->dpy,
                        request->requestor,
                        property,
                        XA_ATOM,
                        32,
                        PropModeReplace,
                        (unsigned char *)targets,
                        count);
        reply_property = property;
    }
    else if (request->target == ctx->utf8_string_atom
             || request->target == ctx->text_atom) {
        XChangeProperty(ctx->dpy,
                        request->requestor,
                        property,
                        request->target,
                        8,
                        PropModeReplace,
                        (const unsigned char *)ctx->clipboard,
                        (int)ctx->clipboard_len);
        reply_property = property;
    }
    else if (request->target == XA_STRING && x11_clipboard_is_ascii(ctx)) {
        XChangeProperty(ctx->dpy,
                        request->requestor,
                        property,
                        XA_STRING,
                        8,
                        PropModeReplace,
                        (const unsigned char *)ctx->clipboard,
                        (int)ctx->clipboard_len);
        reply_property = property;
    }

    x11_send_selection_notify(ctx, request, reply_property);
}

static n00b_key_mod_t
x11_translate_mods(unsigned int state)
{
    n00b_key_mod_t mods = N00B_MOD_NONE;
    if (state & ShiftMask) {
        mods |= N00B_MOD_SHIFT;
    }
    if (state & ControlMask) {
        mods |= N00B_MOD_CTRL;
    }
    if (state & Mod1Mask) {
        mods |= N00B_MOD_ALT;
    }
    return mods;
}

static uint32_t
x11_translate_keysym(KeySym keysym)
{
    switch (keysym) {
    case XK_Up:
        return N00B_KEY_UP;
    case XK_Down:
        return N00B_KEY_DOWN;
    case XK_Left:
        return N00B_KEY_LEFT;
    case XK_Right:
        return N00B_KEY_RIGHT;
    case XK_Home:
        return N00B_KEY_HOME;
    case XK_End:
        return N00B_KEY_END;
    case XK_Page_Up:
        return N00B_KEY_PAGE_UP;
    case XK_Page_Down:
        return N00B_KEY_PAGE_DOWN;
    case XK_Insert:
        return N00B_KEY_INSERT;
    case XK_Delete:
        return N00B_KEY_DELETE;
    case XK_F1:
        return N00B_KEY_F1;
    case XK_F2:
        return N00B_KEY_F2;
    case XK_F3:
        return N00B_KEY_F3;
    case XK_F4:
        return N00B_KEY_F4;
    case XK_F5:
        return N00B_KEY_F5;
    case XK_F6:
        return N00B_KEY_F6;
    case XK_F7:
        return N00B_KEY_F7;
    case XK_F8:
        return N00B_KEY_F8;
    case XK_F9:
        return N00B_KEY_F9;
    case XK_F10:
        return N00B_KEY_F10;
    case XK_F11:
        return N00B_KEY_F11;
    case XK_F12:
        return N00B_KEY_F12;
    default:
        return N00B_KEY_NONE;
    }
}

static void
x11_handle_keypress(x11_ctx_t *ctx, XEvent *xev)
{
    char   buf[32] = {0};
    KeySym keysym = NoSymbol;

    int nbytes = XLookupString(&xev->xkey, buf, (int)sizeof(buf), &keysym, NULL);

    n00b_event_t ev = {
        .type = N00B_EVENT_KEY,
        .key = {
            .key  = N00B_KEY_NONE,
            .mods = x11_translate_mods(xev->xkey.state),
        },
    };

    if (keysym == XK_ISO_Left_Tab) {
        ev.key.key = N00B_KEY_TAB;
        ev.key.mods = (n00b_key_mod_t)(ev.key.mods | N00B_MOD_SHIFT);
        x11_enqueue_event(ctx, &ev);
        return;
    }

    if (keysym == XK_Return || keysym == XK_KP_Enter) {
        ev.key.key = N00B_KEY_ENTER;
        x11_enqueue_event(ctx, &ev);
        return;
    }

    if (keysym == XK_Tab) {
        ev.key.key = N00B_KEY_TAB;
        x11_enqueue_event(ctx, &ev);
        return;
    }

    if (keysym == XK_Escape) {
        ev.key.key = N00B_KEY_ESCAPE;
        x11_enqueue_event(ctx, &ev);
        return;
    }

    if (keysym == XK_BackSpace) {
        ev.key.key = N00B_KEY_BACKSPACE;
        x11_enqueue_event(ctx, &ev);
        return;
    }

    uint32_t mapped = x11_translate_keysym(keysym);
    if (mapped != N00B_KEY_NONE) {
        ev.key.key = mapped;
        x11_enqueue_event(ctx, &ev);
        return;
    }

    if (nbytes > 0) {
        if (n00b_x11_translate_lookup_bytes(buf,
                                            nbytes,
                                            ev.key.mods,
                                            &ev)) {
            x11_enqueue_event(ctx, &ev);
        }
    }
}

static n00b_mouse_button_t
x11_translate_button(unsigned int button)
{
    switch (button) {
    case Button1:
        return N00B_MOUSE_LEFT;
    case Button2:
        return N00B_MOUSE_MIDDLE;
    case Button3:
        return N00B_MOUSE_RIGHT;
    case Button4:
        return N00B_MOUSE_SCROLL_UP;
    case Button5:
        return N00B_MOUSE_SCROLL_DOWN;
    default:
        return N00B_MOUSE_NONE;
    }
}

static void
x11_enqueue_mouse(x11_ctx_t           *ctx,
                  int                  px,
                  int                  py,
                  n00b_mouse_button_t  button,
                  n00b_mouse_action_t  action,
                  unsigned int         state)
{
    n00b_event_t ev = {
        .type = N00B_EVENT_MOUSE,
        .mouse = {
            // Mouse routing expects pixel coordinates.
            .x      = (int32_t)px,
            .y      = (int32_t)py,
            .button = button,
            .action = action,
            .mods   = x11_translate_mods(state),
        },
    };
    x11_enqueue_event(ctx, &ev);
}

static void
x11_handle_resize(x11_ctx_t *ctx, int width, int height)
{
    if (width < 1) {
        width = 1;
    }
    if (height < 1) {
        height = 1;
    }

    ctx->pixel_w = (n00b_isize_t)width;
    ctx->pixel_h = (n00b_isize_t)height;

    n00b_isize_t new_cols = (n00b_isize_t)(width / (int)ctx->cell_w);
    n00b_isize_t new_rows = (n00b_isize_t)(height / (int)ctx->cell_h);

    if (new_cols < 1) {
        new_cols = 1;
    }
    if (new_rows < 1) {
        new_rows = 1;
    }

    if (new_cols != ctx->cols || new_rows != ctx->rows) {
        ctx->cols = new_cols;
        ctx->rows = new_rows;
        ctx->pending.has_pending_resize = true;
        ctx->pending.pending_resize_rows = new_rows;
        ctx->pending.pending_resize_cols = new_cols;

        if (ctx->resize_cb) {
            ctx->resize_cb(new_rows, new_cols, ctx->resize_user_ctx);
        }
    }
}

static void
x11_pump_events(x11_ctx_t *ctx)
{
    while (XPending(ctx->dpy) > 0) {
        XEvent xev;
        XNextEvent(ctx->dpy, &xev);

        switch (xev.type) {
        case KeyPress:
            x11_handle_keypress(ctx, &xev);
            break;

        case ButtonPress: {
            n00b_mouse_button_t button = x11_translate_button(xev.xbutton.button);
            if (button != N00B_MOUSE_NONE) {
                x11_enqueue_mouse(ctx,
                                  xev.xbutton.x,
                                  xev.xbutton.y,
                                  button,
                                  N00B_MOUSE_PRESS,
                                  xev.xbutton.state);
            }
            break;
        }

        case ButtonRelease: {
            n00b_mouse_button_t button = x11_translate_button(xev.xbutton.button);
            if (button == N00B_MOUSE_LEFT
                || button == N00B_MOUSE_MIDDLE
                || button == N00B_MOUSE_RIGHT) {
                x11_enqueue_mouse(ctx,
                                  xev.xbutton.x,
                                  xev.xbutton.y,
                                  button,
                                  N00B_MOUSE_RELEASE,
                                  xev.xbutton.state);
            }
            break;
        }

        case MotionNotify: {
            n00b_mouse_button_t button = N00B_MOUSE_NONE;
            n00b_mouse_action_t action = N00B_MOUSE_MOVE;
            n00b_x11_translate_motion_state(xev.xmotion.state, &button, &action);
            x11_enqueue_mouse(ctx,
                              xev.xmotion.x,
                              xev.xmotion.y,
                              button,
                              action,
                              xev.xmotion.state);
            break;
        }

        case Expose:
            n00b_x11_note_expose(&ctx->pending, xev.xexpose.count);
            break;

        case ConfigureNotify:
            x11_handle_resize(ctx, xev.xconfigure.width, xev.xconfigure.height);
            break;

        case SelectionRequest:
            x11_handle_selection_request(ctx, &xev.xselectionrequest);
            break;

        case ClientMessage:
            if ((Atom)xev.xclient.data.l[0] == ctx->wm_delete) {
                // Synthesize Ctrl+C so event loop exits cleanly.
                n00b_event_t ev = {
                    .type = N00B_EVENT_KEY,
                    .key = {
                        .key  = (uint32_t)'c',
                        .mods = N00B_MOD_CTRL,
                    },
                };
                x11_enqueue_event(ctx, &ev);
                ctx->running = false;
            }
            break;

        default:
            break;
        }
    }
}

static unsigned long
x11_scale_channel_to_mask(uint8_t channel, unsigned long mask)
{
    if (mask == 0ul) {
        return 0ul;
    }

    int shift = 0;
    while (((mask >> shift) & 1ul) == 0ul && shift < 32) {
        shift++;
    }

    int bits = 0;
    while (((mask >> (shift + bits)) & 1ul) == 1ul && bits < 32) {
        bits++;
    }

    if (bits <= 0) {
        return 0ul;
    }

    unsigned long max = (1ul << bits) - 1ul;
    unsigned long scaled = ((unsigned long)channel * max + 127ul) / 255ul;
    return (scaled << shift) & mask;
}

static unsigned long
x11_rgb_to_pixel(const x11_ctx_t *ctx, uint32_t rgb)
{
    if (!ctx || !ctx->visual || !ctx->dpy) {
        return 0ul;
    }

    if (ctx->visual->class == TrueColor || ctx->visual->class == DirectColor) {
        uint8_t r = (uint8_t)((rgb >> 16) & 0xFFu);
        uint8_t g = (uint8_t)((rgb >> 8) & 0xFFu);
        uint8_t b = (uint8_t)(rgb & 0xFFu);

        return x11_scale_channel_to_mask(r, ctx->visual->red_mask)
             | x11_scale_channel_to_mask(g, ctx->visual->green_mask)
             | x11_scale_channel_to_mask(b, ctx->visual->blue_mask);
    }

    // Fallback for non-TrueColor visuals.
    uint8_t r = (uint8_t)((rgb >> 16) & 0xFFu);
    uint8_t g = (uint8_t)((rgb >> 8) & 0xFFu);
    uint8_t b = (uint8_t)(rgb & 0xFFu);
    int luminance = (int)r * 30 + (int)g * 59 + (int)b * 11;
    return luminance >= 128 * 100
        ? WhitePixel(ctx->dpy, ctx->screen)
        : BlackPixel(ctx->dpy, ctx->screen);
}

static uint32_t
x11_style_fg_rgb(const n00b_text_style_t *style)
{
    if (style && n00b_color_is_set(style->fg_rgb)) {
        return (uint32_t)n00b_color_rgb(style->fg_rgb);
    }
    if (style
        && style->fg_palette_ix >= 0
        && style->fg_palette_ix < N00B_PAL_SIZE) {
        n00b_color_t resolved = n00b_theme_resolve_color(style->fg_palette_ix);

        if (n00b_color_is_set(resolved)) {
            return (uint32_t)n00b_color_rgb(resolved);
        }
    }
    return 0xFFFFFFu;
}

static uint32_t
x11_style_bg_rgb(const n00b_text_style_t *style)
{
    if (style && n00b_color_is_set(style->bg_rgb)) {
        return (uint32_t)n00b_color_rgb(style->bg_rgb);
    }
    if (style
        && style->bg_palette_ix >= 0
        && style->bg_palette_ix < N00B_PAL_SIZE) {
        n00b_color_t resolved = n00b_theme_resolve_color(style->bg_palette_ix);

        if (n00b_color_is_set(resolved)) {
            return (uint32_t)n00b_color_rgb(resolved);
        }
    }
    return 0x000000u;
}

static uint32_t
x11_dim_rgb(uint32_t rgb)
{
    uint32_t r = ((rgb >> 16) & 0xFFu) / 2u;
    uint32_t g = ((rgb >> 8) & 0xFFu) / 2u;
    uint32_t b = (rgb & 0xFFu) / 2u;
    return (r << 16) | (g << 8) | b;
}

static XFontStruct *
x11_load_font(Display *dpy)
{
    const char *candidates[] = {
        "9x15",
        "fixed",
        "8x13",
        "6x13",
        NULL,
    };

    for (size_t i = 0; candidates[i] != NULL; i++) {
        XFontStruct *font = XLoadQueryFont(dpy, candidates[i]);
        if (!font) {
            continue;
        }

        if (font->fid == None) {
            XFreeFont(dpy, font);
            continue;
        }

        return font;
    }

    return NULL;
}

static void *
x11_init(n00b_conduit_topic_t(n00b_buffer_t *) *output)
{
    (void)output;

    x11_ctx_t *ctx = n00b_alloc(x11_ctx_t);
    if (!ctx) {
        return nullptr;
    }

    ctx->dpy = XOpenDisplay(NULL);
    if (!ctx->dpy) {
        fprintf(stderr,
                "n00b: x11 backend unavailable (cannot open DISPLAY).\n");
        n00b_free(ctx);
        return nullptr;
    }

    ctx->screen = DefaultScreen(ctx->dpy);
    ctx->colormap = DefaultColormap(ctx->dpy, ctx->screen);
    ctx->visual = DefaultVisual(ctx->dpy, ctx->screen);
    ctx->running = true;

    ctx->font = x11_load_font(ctx->dpy);

    ctx->cell_w = X11_DEFAULT_CELL_W;
    ctx->cell_h = X11_DEFAULT_CELL_H;
    if (ctx->font) {
        if (ctx->font->max_bounds.width > 0) {
            ctx->cell_w = (n00b_isize_t)ctx->font->max_bounds.width;
        }

        int font_h = ctx->font->ascent + ctx->font->descent;
        if (font_h > 0) {
            ctx->cell_h = (n00b_isize_t)font_h;
        }
    }

    if (ctx->cell_w < 1) {
        ctx->cell_w = X11_DEFAULT_CELL_W;
    }
    if (ctx->cell_h < 1) {
        ctx->cell_h = X11_DEFAULT_CELL_H;
    }

    ctx->rows = X11_DEFAULT_ROWS;
    ctx->cols = X11_DEFAULT_COLS;
    ctx->pixel_w = ctx->cols * ctx->cell_w;
    ctx->pixel_h = ctx->rows * ctx->cell_h;

    ctx->window = XCreateSimpleWindow(ctx->dpy,
                                      RootWindow(ctx->dpy, ctx->screen),
                                      100,
                                      100,
                                      (unsigned int)ctx->pixel_w,
                                      (unsigned int)ctx->pixel_h,
                                      0,
                                      BlackPixel(ctx->dpy, ctx->screen),
                                      BlackPixel(ctx->dpy, ctx->screen));

    XStoreName(ctx->dpy, ctx->window, "n00b (x11)");

    long input_mask = ExposureMask
                    | KeyPressMask
                    | ButtonPressMask
                    | ButtonReleaseMask
                    | PointerMotionMask
                    | StructureNotifyMask;
    XSelectInput(ctx->dpy, ctx->window, input_mask);

    ctx->wm_delete = XInternAtom(ctx->dpy, "WM_DELETE_WINDOW", False);
    ctx->clipboard_atom = XInternAtom(ctx->dpy, "CLIPBOARD", False);
    ctx->targets_atom = XInternAtom(ctx->dpy, "TARGETS", False);
    ctx->utf8_string_atom = XInternAtom(ctx->dpy, "UTF8_STRING", False);
    ctx->text_atom = XInternAtom(ctx->dpy, "TEXT", False);
    XSetWMProtocols(ctx->dpy, ctx->window, &ctx->wm_delete, 1);

    XGCValues gcv = {};
    ctx->gc = XCreateGC(ctx->dpy, ctx->window, 0, &gcv);
    if (ctx->font && ctx->font->fid != None && ctx->gc) {
        XSetFont(ctx->dpy, ctx->gc, ctx->font->fid);
    }

    XMapWindow(ctx->dpy, ctx->window);
    XFlush(ctx->dpy);

    return ctx;
}

static void
x11_destroy(void *vctx)
{
    x11_ctx_t *ctx = vctx;
    if (!ctx) {
        return;
    }

    if (ctx->comp_grid) {
        n00b_free(ctx->comp_grid);
    }
    x11_free_clipboard(ctx);
    n00b_composite_style_pool_destroy(&ctx->style_pool);

    if (ctx->dpy) {
        if (ctx->gc) {
            XFreeGC(ctx->dpy, ctx->gc);
        }

        if (ctx->font) {
            XFreeFont(ctx->dpy, ctx->font);
        }

        if (ctx->window) {
            XDestroyWindow(ctx->dpy, ctx->window);
        }

        XCloseDisplay(ctx->dpy);
    }

    n00b_free(ctx);
}

static n00b_render_cap_t
x11_capabilities(void *vctx)
{
    (void)vctx;
    return N00B_RCAP_MANAGES_TTY
         | N00B_RCAP_COLOR_24BIT
         | N00B_RCAP_MOUSE
         | N00B_RCAP_CURSOR_MOVE
         | N00B_RCAP_PIXEL_COORDS;
}

static n00b_render_size_t
x11_get_size(void *vctx)
{
    x11_ctx_t *ctx = vctx;
    if (!ctx) {
        return (n00b_render_size_t){0};
    }

    return (n00b_render_size_t){
        .rows = ctx->rows,
        .cols = ctx->cols,
        .pixel_w = ctx->pixel_w,
        .pixel_h = ctx->pixel_h,
        .cell_pixel_w = ctx->cell_w,
        .cell_pixel_h = ctx->cell_h,
    };
}

static void
x11_render_frame(void         *vctx,
                 n00b_rcell_t *cells,
                 n00b_isize_t  rows,
                 n00b_isize_t  cols,
                 n00b_rcell_t *prev_cells)
{
    (void)prev_cells;

    x11_ctx_t *ctx = vctx;
    if (!ctx || !ctx->dpy || !ctx->window || !ctx->gc || !cells) {
        return;
    }

    if (rows < 1 || cols < 1) {
        return;
    }

    ctx->rows = rows;
    ctx->cols = cols;

    unsigned long black = BlackPixel(ctx->dpy, ctx->screen);
    XSetForeground(ctx->dpy, ctx->gc, black);
    XFillRectangle(ctx->dpy,
                   ctx->window,
                   ctx->gc,
                   0,
                   0,
                   (unsigned int)ctx->pixel_w,
                   (unsigned int)ctx->pixel_h);

    int baseline_offset = ctx->font ? ctx->font->ascent : (int)(ctx->cell_h - 2);

    for (n00b_isize_t r = 0; r < rows; r++) {
        for (n00b_isize_t c = 0; c < cols; c++) {
            n00b_rcell_t *cell = &cells[r * cols + c];

            int x = (int)(c * ctx->cell_w);
            int y = (int)(r * ctx->cell_h);

            const n00b_text_style_t *style = cell->style;

            uint32_t fg_rgb = x11_style_fg_rgb(style);
            uint32_t bg_rgb = x11_style_bg_rgb(style);

            if (style && style->reverse == N00B_TRI_YES) {
                uint32_t tmp = fg_rgb;
                fg_rgb = bg_rgb;
                bg_rgb = tmp;
            }

            if (style && style->dim == N00B_TRI_YES) {
                fg_rgb = x11_dim_rgb(fg_rgb);
            }

            unsigned long bg = x11_rgb_to_pixel(ctx, bg_rgb);
            unsigned long fg = x11_rgb_to_pixel(ctx, fg_rgb);

            XSetForeground(ctx->dpy, ctx->gc, bg);
            XFillRectangle(ctx->dpy,
                           ctx->window,
                           ctx->gc,
                           x,
                           y,
                           (unsigned int)ctx->cell_w,
                           (unsigned int)ctx->cell_h);

            if ((cell->flags & N00B_CELL_OCCUPIED)
                && !(cell->flags & N00B_CELL_WIDE_CONT)
                && cell->grapheme_len > 0) {
                char text[17];
                uint8_t n = cell->grapheme_len;
                if (n > 16u) {
                    n = 16u;
                }

                memcpy(text, cell->grapheme, n);
                text[n] = '\0';

                XSetForeground(ctx->dpy, ctx->gc, fg);
                XDrawString(ctx->dpy,
                            ctx->window,
                            ctx->gc,
                            x,
                            y + baseline_offset,
                            text,
                            (int)n);
            }
        }
    }
}

static void
x11_flush(void *vctx)
{
    x11_ctx_t *ctx = vctx;
    if (!ctx || !ctx->dpy) {
        return;
    }
    XFlush(ctx->dpy);
}

static bool
x11_clipboard_copy(void *vctx, const char *utf8, size_t len)
{
    x11_ctx_t *ctx = vctx;
    char      *copy;

    if (!ctx || !ctx->dpy || !ctx->window || !utf8) {
        return false;
    }

    copy = n00b_alloc_array_with_opts(char,
                                      len + 1,
                                      &(n00b_alloc_opts_t){.no_scan = true});
    if (len > 0) {
        memcpy(copy, utf8, len);
    }
    copy[len] = '\0';

    x11_free_clipboard(ctx);
    ctx->clipboard = copy;
    ctx->clipboard_len = len;

    XSetSelectionOwner(ctx->dpy, ctx->clipboard_atom, ctx->window, CurrentTime);
    XSetSelectionOwner(ctx->dpy, XA_PRIMARY, ctx->window, CurrentTime);
    XFlush(ctx->dpy);

    return XGetSelectionOwner(ctx->dpy, ctx->clipboard_atom) == ctx->window;
}

static void
x11_render_planes(void                         *vctx,
                  const n00b_composite_entry_t *entries,
                  n00b_isize_t                  count,
                  n00b_isize_t                  total_rows,
                  n00b_isize_t                  total_cols,
                  n00b_text_style_t            *default_style,
                  n00b_render_cap_t             caps)
{
    x11_ctx_t *ctx = vctx;
    if (!ctx) {
        return;
    }

    n00b_isize_t cell_rows = total_rows / ctx->cell_h;
    n00b_isize_t cell_cols = total_cols / ctx->cell_w;

    if (cell_rows < 1) {
        cell_rows = 1;
    }
    if (cell_cols < 1) {
        cell_cols = 1;
    }

    if (cell_rows != ctx->comp_grid_rows || cell_cols != ctx->comp_grid_cols) {
        size_t total = (size_t)cell_rows * (size_t)cell_cols;

        if (ctx->comp_grid) {
            n00b_free(ctx->comp_grid);
        }
        ctx->comp_grid = n00b_alloc_array(n00b_rcell_t, total);
        ctx->comp_grid_rows = cell_rows;
        ctx->comp_grid_cols = cell_cols;
    }

    if (!ctx->comp_grid) {
        return;
    }

    ctx->rows = cell_rows;
    ctx->cols = cell_cols;

    n00b_composite_style_pool_clear(&ctx->style_pool);
    n00b_composite_commands_to_grid(entries,
                                     count,
                                     ctx->comp_grid,
                                     cell_rows,
                                     cell_cols,
                                     ctx->cell_w,
                                     ctx->cell_h,
                                     default_style,
                                     caps,
                                     &ctx->style_pool);

    x11_render_frame(vctx, ctx->comp_grid, cell_rows, cell_cols, nullptr);
}

static void
x11_cursor_set_visible(void *vctx, bool visible)
{
    (void)vctx;
    (void)visible;
}

static void
x11_cursor_move(void *vctx, n00b_isize_t row, n00b_isize_t col)
{
    (void)vctx;
    (void)row;
    (void)col;
}

static void
x11_on_resize(void *vctx,
              void (*cb)(n00b_isize_t, n00b_isize_t, void *),
              void *user_ctx)
{
    x11_ctx_t *ctx = vctx;
    if (!ctx) {
        return;
    }

    ctx->resize_cb = cb;
    ctx->resize_user_ctx = user_ctx;
}

static void
x11_prepare_gui(void *vctx, n00b_plane_t **planes, n00b_isize_t n)
{
    (void)vctx;
    (void)planes;
    (void)n;
}

static bool
x11_poll_event(void *vctx, int32_t timeout_ms, n00b_event_t *out)
{
    x11_ctx_t *ctx = vctx;

    if (!ctx || !out || !ctx->dpy) {
        return false;
    }

    out->type = N00B_EVENT_NONE;

    if (n00b_x11_take_pending_event(&ctx->pending, ctx->rows, ctx->cols, out)) {
        return true;
    }

    x11_pump_events(ctx);

    if (n00b_x11_take_pending_event(&ctx->pending, ctx->rows, ctx->cols, out)) {
        return true;
    }

    if (x11_dequeue_event(ctx, out)) {
        return true;
    }

    if (timeout_ms == 0) {
        return false;
    }

    int fd = ConnectionNumber(ctx->dpy);
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);

    struct timeval tv;
    struct timeval *tvp = NULL;
    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        tvp = &tv;
    }

    // eintr: an interrupted wait reports no event, as a timeout does, and the
    // caller polls again.
    int rc = select(fd + 1, &readfds, NULL, NULL, tvp);
    if (rc <= 0) {
        return false;
    }

    x11_pump_events(ctx);

    if (n00b_x11_take_pending_event(&ctx->pending, ctx->rows, ctx->cols, out)) {
        return true;
    }

    return x11_dequeue_event(ctx, out);
}

const n00b_renderer_vtable_t n00b_renderer_x11 = {
    .name               = "x11",
    .version            = N00B_RENDERER_ABI_VERSION,
    .init               = x11_init,
    .destroy            = x11_destroy,
    .capabilities       = x11_capabilities,
    .get_size           = x11_get_size,
    .render_frame       = x11_render_frame,
    .flush              = x11_flush,
    .render_planes      = x11_render_planes,
    .clipboard_copy     = x11_clipboard_copy,
    .cursor_set_visible = x11_cursor_set_visible,
    .cursor_move        = x11_cursor_move,
    .on_resize          = x11_on_resize,
    .prepare_gui        = x11_prepare_gui,
    .poll_event         = x11_poll_event,
};
