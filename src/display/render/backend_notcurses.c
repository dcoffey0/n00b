/*
 * Notcurses renderer backend with optional FreeType pixel rendering.
 *
 * When FreeType is available and the terminal supports pixel blitting
 * (Sixel/Kitty), text is rasterized into RGBA buffers and blitted via
 * ncvisual.  Otherwise, cells are rendered through notcurses' standard
 * character APIs (ncplane_putstr_yx, ncplane_set_channels, etc.).
 *
 * This file is compiled through ncc as a separate static library,
 * gated on `conf_notcurses` in meson.build.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <notcurses/notcurses.h>

#include "n00b.h"
#include "core/alloc.h"
#include "display/render/backend.h"
#include "display/render/cell.h"
#include "display/render/draw_cmd.h"
#include "display/event.h"
#include "text/strings/theme.h"
#include "text/strings/text_style.h"
#include "text/strings/string_style.h"
#include "display/render/composite.h"
#include "core/string.h"
#include "internal/display/diagnostics.h"
#include "internal/display/terminal_input.h"

#if N00B_HAVE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

// ====================================================================
// System font search paths (used when FreeType is available)
// ====================================================================

#if N00B_HAVE_FREETYPE
static const char *system_font_paths[] = {
#ifdef __APPLE__
    "/System/Library/Fonts/SFNSMono.ttf",
    "/System/Library/Fonts/Menlo.ttc",
    "/System/Library/Fonts/Monaco.ttf",
    "/System/Library/Fonts/Supplemental/Courier New.ttf",
#else
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
    "/usr/share/fonts/truetype/freefont/FreeMono.ttf",
    "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
#endif
    nullptr
};

static const char *serif_font_paths[] = {
#ifdef __APPLE__
    "/System/Library/Fonts/Supplemental/Times New Roman.ttf",
    "/Library/Fonts/Georgia.ttf",
    "/System/Library/Fonts/Supplemental/Palatino.ttc",
#else
    "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
    "/usr/share/fonts/TTF/DejaVuSerif.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf",
    "/usr/share/fonts/truetype/freefont/FreeSerif.ttf",
#endif
    nullptr
};

static const char *sans_font_paths[] = {
#ifdef __APPLE__
    "/System/Library/Fonts/Helvetica.ttc",
    "/System/Library/Fonts/HelveticaNeue.ttc",
    "/Library/Fonts/Arial.ttf",
#else
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
#endif
    nullptr
};
#endif

// ====================================================================
// Glyph cache (FreeType pixel rendering)
// ====================================================================

#if N00B_HAVE_FREETYPE

#define GLYPH_CACHE_SIZE 4096  // Must be power of 2.
#define GLYPH_CACHE_MASK (GLYPH_CACHE_SIZE - 1)

typedef struct {
    uint32_t codepoint;
    uint32_t fg;
    uint32_t bg;
    uint16_t pixel_h;
    uint8_t  font_hint;
    bool     bold;
    bool     occupied;

    // Cached glyph metrics.
    int      advance_x;   // Horizontal advance in pixels.
    int      bitmap_left;
    int      bitmap_top;
    unsigned int bmp_rows;
    unsigned int bmp_width;
    unsigned int bmp_pitch;

    // Cached rendered bitmap (FT_RENDER_MODE_NORMAL, 8-bit alpha).
    uint8_t *alpha_buf;   // bmp_rows * bmp_pitch bytes, or nullptr.
} glyph_cache_entry_t;

typedef struct {
    glyph_cache_entry_t entries[GLYPH_CACHE_SIZE];
} glyph_cache_t;

static uint32_t
glyph_cache_hash(uint32_t cp, uint32_t fg, uint32_t bg,
                 bool bold, uint16_t pixel_h, uint8_t font_hint)
{
    // FNV-1a inspired.
    uint32_t h = 2166136261u;
    h ^= cp;       h *= 16777619u;
    h ^= fg;       h *= 16777619u;
    h ^= bg;       h *= 16777619u;
    h ^= (uint32_t)bold;       h *= 16777619u;
    h ^= (uint32_t)pixel_h;   h *= 16777619u;
    h ^= (uint32_t)font_hint; h *= 16777619u;
    return h & GLYPH_CACHE_MASK;
}

static glyph_cache_entry_t *
glyph_cache_lookup(glyph_cache_t *cache, uint32_t cp,
                   uint32_t fg, uint32_t bg,
                   bool bold, uint16_t pixel_h, uint8_t font_hint)
{
    uint32_t idx = glyph_cache_hash(cp, fg, bg, bold, pixel_h, font_hint);

    for (int probe = 0; probe < 16; probe++) {
        glyph_cache_entry_t *e = &cache->entries[(idx + probe) & GLYPH_CACHE_MASK];
        if (!e->occupied) {
            return NULL;
        }
        if (e->codepoint == cp && e->fg == fg && e->bg == bg
            && e->bold == bold && e->pixel_h == pixel_h
            && e->font_hint == font_hint) {
            return e;
        }
    }
    return NULL;
}

static glyph_cache_entry_t *
glyph_cache_insert(glyph_cache_t *cache, uint32_t cp,
                   uint32_t fg, uint32_t bg,
                   bool bold, uint16_t pixel_h, uint8_t font_hint)
{
    uint32_t idx = glyph_cache_hash(cp, fg, bg, bold, pixel_h, font_hint);

    for (int probe = 0; probe < 16; probe++) {
        glyph_cache_entry_t *e = &cache->entries[(idx + probe) & GLYPH_CACHE_MASK];
        if (!e->occupied) {
            e->codepoint = cp;
            e->fg        = fg;
            e->bg        = bg;
            e->bold      = bold;
            e->pixel_h   = pixel_h;
            e->font_hint = font_hint;
            e->occupied  = true;
            return e;
        }
        // Already present?
        if (e->codepoint == cp && e->fg == fg && e->bg == bg
            && e->bold == bold && e->pixel_h == pixel_h
            && e->font_hint == font_hint) {
            return e;
        }
    }
    // Table too full — evict first slot (simple, rare).
    glyph_cache_entry_t *e = &cache->entries[idx];
    n00b_free(e->alpha_buf);
    e->codepoint  = cp;
    e->fg         = fg;
    e->bg         = bg;
    e->bold       = bold;
    e->pixel_h    = pixel_h;
    e->font_hint  = font_hint;
    e->alpha_buf  = NULL;
    return e;
}

static void
glyph_cache_destroy(glyph_cache_t *cache)
{
    if (!cache) {
        return;
    }
    for (int i = 0; i < GLYPH_CACHE_SIZE; i++) {
        n00b_free(cache->entries[i].alpha_buf);
    }
    n00b_free(cache);
}

#endif /* N00B_HAVE_FREETYPE */

// ====================================================================
// Context
// ====================================================================

typedef struct {
    struct notcurses *nc;
    struct ncplane   *stdplane;
    bool              has_pixel;

#if N00B_HAVE_FREETYPE
    bool              has_freetype;
    FT_Library        ft_lib;
#define NC_NUM_FONT_HINTS 4
    FT_Face           ft_faces[NC_NUM_FONT_HINTS]; // [DEFAULT]=mono [MONO]=mono [SERIF] [SANS]
    glyph_cache_t    *glyph_cache;
#endif

    // Pixel geometry (cached from last query).
    unsigned int      cell_pixel_w;
    unsigned int      cell_pixel_h;

    // Per-entry child planes for pixel rendering.
    struct ncplane  **entry_planes;
    unsigned int      entry_planes_count;  // allocated slots
    unsigned int      entry_planes_used;   // slots used this frame
    unsigned int      cache_cell_pixel_w;  // geometry used by plane_cache
    unsigned int      cache_cell_pixel_h;

    // Persistent compositing grid for cell fallback rendering.
    n00b_rcell_t     *comp_grid;
    n00b_isize_t      comp_grid_rows;
    n00b_isize_t      comp_grid_cols;
    n00b_composite_style_pool_t style_pool;

    // Plane blit cache: skip re-blitting planes whose content hasn't changed.
    // Open-addressing hash table keyed on n00b_plane_t pointer.
#define NC_PLANE_CACHE_SIZE 256
#define NC_PLANE_CACHE_MASK (NC_PLANE_CACHE_SIZE - 1)
    struct {
        n00b_plane_t    *plane;       // key (nullptr = empty slot)
        struct ncplane  *ncp;         // cached ncplane
        uint32_t         render_gen;  // generation when last rendered
        int32_t          abs_x;       // position when last rendered
        int32_t          abs_y;
        int32_t          cell_x;      // ncplane cell-space placement/size
        int32_t          cell_y;
        int32_t          cell_w;
        int32_t          cell_h;
        int32_t          pxoff_x;     // ncvisual pixel offsets within cell
        int32_t          pxoff_y;
        int32_t          width;       // content geometry fingerprint
        int32_t          height;
        int32_t          scroll_x;
        int32_t          scroll_y;
        n00b_box_props_t *box;
        n00b_widget_state_t state;
    } plane_cache[NC_PLANE_CACHE_SIZE];

    // Resize callback.
    void            (*resize_cb)(n00b_isize_t, n00b_isize_t, void *);
    void             *resize_user;

    // Last known dimensions for resize detection.
    unsigned int      last_rows;
    unsigned int      last_cols;

    // Mouse drag detection state.
    n00b_terminal_input_state_t input_state;
} nc_ctx_t;

// ====================================================================
// FreeType helpers
// ====================================================================

#if N00B_HAVE_FREETYPE

// Try to load a face from a null-terminated path list.
static FT_Face
ft_load_from_paths(FT_Library lib, const char **paths)
{
    FT_Face      face = nullptr;
    const char **p;
    for (p = paths; *p; p++) {
        FILE *f = fopen(*p, "rb");
        if (f) {
            fclose(f);
            if (FT_New_Face(lib, *p, 0, &face) == 0) {
                return face;
            }
        }
    }
    return nullptr;
}

static bool
ft_init(nc_ctx_t *ctx)
{
    if (FT_Init_FreeType(&ctx->ft_lib) != 0) {
        return false;
    }

    // Load monospace face (required — if this fails, no FreeType).
    FT_Face mono = ft_load_from_paths(ctx->ft_lib, system_font_paths);
    if (!mono) {
        FT_Done_FreeType(ctx->ft_lib);
        ctx->ft_lib = nullptr;
        return false;
    }

    ctx->ft_faces[N00B_FONT_DEFAULT] = mono;
    ctx->ft_faces[N00B_FONT_MONO]    = mono;

    // Load serif and sans faces; fall back to mono if unavailable.
    FT_Face serif = ft_load_from_paths(ctx->ft_lib, serif_font_paths);
    ctx->ft_faces[N00B_FONT_SERIF] = serif ? serif : mono;

    FT_Face sans = ft_load_from_paths(ctx->ft_lib, sans_font_paths);
    ctx->ft_faces[N00B_FONT_SANS] = sans ? sans : mono;

    ctx->has_freetype = true;
    return true;
}

static void
ft_destroy(nc_ctx_t *ctx)
{
    // Destroy each unique face exactly once.
    FT_Face seen[NC_NUM_FONT_HINTS] = {0};
    int nseen = 0;

    for (int i = 0; i < NC_NUM_FONT_HINTS; i++) {
        FT_Face face = ctx->ft_faces[i];
        if (!face) {
            continue;
        }
        bool dup = false;
        for (int j = 0; j < nseen; j++) {
            if (seen[j] == face) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            seen[nseen++] = face;
            FT_Done_Face(face);
        }
        ctx->ft_faces[i] = nullptr;
    }

    if (ctx->ft_lib) {
        FT_Done_FreeType(ctx->ft_lib);
        ctx->ft_lib = nullptr;
    }
    ctx->has_freetype = false;
}

// Decode one UTF-8 codepoint, advance *str.
static uint32_t
utf8_decode(const char **str)
{
    const unsigned char *s = (const unsigned char *)*str;
    uint32_t cp = 0;

    if (s[0] < 0x80) {
        cp   = s[0];
        *str += 1;
    }
    else if ((s[0] & 0xE0) == 0xC0) {
        cp   = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        *str += 2;
    }
    else if ((s[0] & 0xF0) == 0xE0) {
        cp   = ((uint32_t)(s[0] & 0x0F) << 12)
             | ((uint32_t)(s[1] & 0x3F) << 6)
             | (s[2] & 0x3F);
        *str += 3;
    }
    else if ((s[0] & 0xF8) == 0xF0) {
        cp   = ((uint32_t)(s[0] & 0x07) << 18)
             | ((uint32_t)(s[1] & 0x3F) << 12)
             | ((uint32_t)(s[2] & 0x3F) << 6)
             | (s[3] & 0x3F);
        *str += 4;
    }
    else {
        *str += 1;
    }

    return cp;
}

/*
 * Ensure a glyph is in the cache, rasterizing via FreeType if needed.
 * Returns the cache entry (never NULL on success).
 */
static glyph_cache_entry_t *
ft_ensure_glyph(nc_ctx_t *ctx, FT_Face face, uint32_t cp,
                int pixel_height, uint32_t fg, uint32_t bg,
                bool bold, uint8_t font_hint)
{
    glyph_cache_entry_t *e = glyph_cache_lookup(
        ctx->glyph_cache, cp, fg, bg, bold,
        (uint16_t)pixel_height, font_hint);
    if (e) {
        return e;
    }

    // Cache miss — rasterize with FreeType.
    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)pixel_height);

    FT_UInt glyph_idx = FT_Get_Char_Index(face, cp);
    if (FT_Load_Glyph(face, glyph_idx, FT_LOAD_DEFAULT) != 0) {
        return NULL;
    }

    FT_GlyphSlot slot = face->glyph;
    int advance = (int)(slot->advance.x >> 6);

    if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0) {
        // Can't render but have advance — insert metrics-only entry.
        e = glyph_cache_insert(ctx->glyph_cache, cp, fg, bg, bold,
                               (uint16_t)pixel_height, font_hint);
        e->advance_x  = advance;
        e->bitmap_left = 0;
        e->bitmap_top  = 0;
        e->bmp_rows   = 0;
        e->bmp_width  = 0;
        e->bmp_pitch  = 0;
        e->alpha_buf  = NULL;
        return e;
    }

    FT_Bitmap *bmp = &slot->bitmap;

    e = glyph_cache_insert(ctx->glyph_cache, cp, fg, bg, bold,
                           (uint16_t)pixel_height, font_hint);
    e->advance_x  = advance;
    e->bitmap_left = slot->bitmap_left;
    e->bitmap_top  = slot->bitmap_top;
    e->bmp_rows   = bmp->rows;
    e->bmp_width  = bmp->width;
    e->bmp_pitch  = (unsigned)bmp->pitch;

    size_t buf_size = (size_t)(bmp->rows * (unsigned)bmp->pitch);
    if (buf_size > 0) {
        e->alpha_buf = n00b_alloc_array(uint8_t, buf_size);
        if (e->alpha_buf) {
            memcpy(e->alpha_buf, bmp->buffer, buf_size);
        }
    } else {
        e->alpha_buf = NULL;
    }

    return e;
}

/*
 * Rasterize a UTF-8 string into an RGBA buffer using FreeType.
 *
 * Returns a heap-allocated RGBA buffer (caller frees with n00b_free()).
 * *out_width and *out_height receive the image dimensions.
 * fg/bg are 0x00RRGGBB packed colors.
 */
static uint8_t *
ft_render_text(nc_ctx_t    *ctx,
               FT_Face      face,
               const char  *text,
               int          pixel_height,
               uint32_t     fg_color,
               uint32_t     bg_color,
               bool         bold,
               int         *out_width,
               int         *out_height)
{
    if (!face) {
        face = ctx->ft_faces[0];
    }
    if (!face || !text || pixel_height <= 0 || !ctx->glyph_cache) {
        return nullptr;
    }

    // Determine font_hint from the face pointer.
    uint8_t font_hint = 0;
    for (int i = 0; i < NC_NUM_FONT_HINTS; i++) {
        if (ctx->ft_faces[i] == face) {
            font_hint = (uint8_t)i;
            break;
        }
    }

    // Single pass: lookup each glyph from cache (rasterizes on miss),
    // accumulate metrics.
    int total_width = 0;
    int max_ascent  = 0;
    int max_descent = 0;

    const char *p = text;
    while (*p) {
        uint32_t cp = utf8_decode(&p);
        if (cp == 0) break;

        glyph_cache_entry_t *ge = ft_ensure_glyph(
            ctx, face, cp, pixel_height, fg_color, bg_color,
            bold, font_hint);
        if (!ge) continue;

        total_width += ge->advance_x;
        int glyph_top = ge->bitmap_top;
        int glyph_bottom = ge->bitmap_top - (int)ge->bmp_rows;
        if (glyph_top > max_ascent) max_ascent = glyph_top;
        if (-glyph_bottom > max_descent) max_descent = -glyph_bottom;
    }

    if (total_width <= 0) {
        return nullptr;
    }

    int height   = max_ascent + max_descent;
    if (height < pixel_height) {
        height = pixel_height;
    }
    if (height < 1) {
        height = 1;
    }

    int baseline = max_ascent;
    if (baseline < 0) {
        baseline = 0;
    }
    if (baseline >= height) {
        baseline = height - 1;
    }

    // Allocate RGBA buffer.
    uint8_t *rgba = n00b_alloc_array(uint8_t, (size_t)(total_width * height * 4));
    if (!rgba) {
        return nullptr;
    }

    // Fill background.
    uint8_t bg_r = (bg_color >> 16) & 0xFF;
    uint8_t bg_g = (bg_color >> 8) & 0xFF;
    uint8_t bg_b = bg_color & 0xFF;

    for (int i = 0; i < total_width * height; i++) {
        rgba[i * 4 + 0] = bg_r;
        rgba[i * 4 + 1] = bg_g;
        rgba[i * 4 + 2] = bg_b;
        rgba[i * 4 + 3] = 255;
    }

    uint8_t fg_r = (fg_color >> 16) & 0xFF;
    uint8_t fg_g = (fg_color >> 8) & 0xFF;
    uint8_t fg_b = fg_color & 0xFF;

    // Compose pass: blit cached alpha bitmaps into the RGBA buffer.
    int pen_x = 0;
    p = text;
    while (*p) {
        uint32_t cp = utf8_decode(&p);
        if (cp == 0) break;

        glyph_cache_entry_t *ge = ft_ensure_glyph(
            ctx, face, cp, pixel_height, fg_color, bg_color,
            bold, font_hint);
        if (!ge) continue;

        if (!ge->alpha_buf || ge->bmp_rows == 0) {
            pen_x += ge->advance_x;
            continue;
        }

        int x0 = pen_x + ge->bitmap_left;
        int y0 = baseline - ge->bitmap_top;

        for (unsigned int by = 0; by < ge->bmp_rows; by++) {
            for (unsigned int bx = 0; bx < ge->bmp_width; bx++) {
                int dx = x0 + (int)bx;
                int dy = y0 + (int)by;

                if (dx < 0 || dx >= total_width || dy < 0 || dy >= height) {
                    continue;
                }

                uint8_t alpha = ge->alpha_buf[by * ge->bmp_pitch + bx];
                if (alpha == 0) continue;

                int idx = (dy * total_width + dx) * 4;
                float a = (float)alpha / 255.0f;

                rgba[idx + 0] = (uint8_t)(fg_r * a + rgba[idx + 0] * (1.0f - a));
                rgba[idx + 1] = (uint8_t)(fg_g * a + rgba[idx + 1] * (1.0f - a));
                rgba[idx + 2] = (uint8_t)(fg_b * a + rgba[idx + 2] * (1.0f - a));
                rgba[idx + 3] = 255;
            }
        }

        // Bold synthesis: re-blit shifted right by 1 pixel at half alpha.
        if (bold) {
            for (unsigned int by = 0; by < ge->bmp_rows; by++) {
                for (unsigned int bx = 0; bx < ge->bmp_width; bx++) {
                    int dx = x0 + (int)bx + 1;
                    int dy = y0 + (int)by;

                    if (dx < 0 || dx >= total_width || dy < 0 || dy >= height) {
                        continue;
                    }

                    uint8_t alpha = ge->alpha_buf[by * ge->bmp_pitch + bx];
                    if (alpha == 0) continue;

                    int idx = (dy * total_width + dx) * 4;
                    float a = (float)alpha / 255.0f * 0.5f;

                    rgba[idx + 0] = (uint8_t)(fg_r * a + rgba[idx + 0] * (1.0f - a));
                    rgba[idx + 1] = (uint8_t)(fg_g * a + rgba[idx + 1] * (1.0f - a));
                    rgba[idx + 2] = (uint8_t)(fg_b * a + rgba[idx + 2] * (1.0f - a));
                }
            }
        }

        pen_x += ge->advance_x;
    }

    *out_width  = total_width;
    *out_height = height;
    return rgba;
}

/*
 * Draw a horizontal line (underline / strikethrough) into an RGBA buffer.
 */
static void
ft_draw_hline(uint8_t *rgba, int img_w, int img_h,
              int y, int x_start, int x_end,
              uint8_t r, uint8_t g, uint8_t b)
{
    if (y < 0 || y >= img_h) {
        return;
    }
    if (x_start < 0) {
        x_start = 0;
    }
    if (x_end > img_w) {
        x_end = img_w;
    }

    for (int x = x_start; x < x_end; x++) {
        int idx = (y * img_w + x) * 4;
        rgba[idx + 0] = r;
        rgba[idx + 1] = g;
        rgba[idx + 2] = b;
        rgba[idx + 3] = 255;
    }
}

#endif /* N00B_HAVE_FREETYPE */

// ====================================================================
// Entry plane management for pixel rendering
// ====================================================================

/*
 * Destroy all entry planes from the previous frame.
 * ncvisual_blit(NCBLIT_PIXEL) creates internal child planes on each
 * blit target, so we must destroy the targets between frames to
 * avoid accumulating stale blit children.
 */
static void
destroy_entry_planes(nc_ctx_t *ctx)
{
    // Destroy all cached ncplanes (the plane cache is the source of truth).
    for (unsigned int i = 0; i < NC_PLANE_CACHE_SIZE; i++) {
        if (ctx->plane_cache[i].ncp) {
            ncplane_family_destroy(ctx->plane_cache[i].ncp);
            ctx->plane_cache[i].ncp   = nullptr;
            ctx->plane_cache[i].plane = nullptr;
        }
    }
    // Clear the entry_planes array (pointers were aliases into cache).
    if (ctx->entry_planes) {
        for (unsigned int i = 0; i < ctx->entry_planes_used; i++) {
            ctx->entry_planes[i] = nullptr;
        }
    }
    ctx->entry_planes_used = 0;
}

/*
 * Ensure we have at least `count` slots in the entry_planes array.
 * Does NOT create ncplanes — those are created per-entry during render.
 * Grows the array if needed; never shrinks.
 */
static void
ensure_entry_plane_slots(nc_ctx_t *ctx, unsigned int count)
{
    if (count <= ctx->entry_planes_count) {
        return;
    }
    struct ncplane **new_arr = n00b_alloc_array(struct ncplane *, count);
    if (!new_arr) {
        return;
    }
    if (ctx->entry_planes) {
        memcpy(new_arr,
               ctx->entry_planes,
               ctx->entry_planes_count * sizeof(struct ncplane *));
        n00b_free(ctx->entry_planes);
    }
    // n00b_alloc_array already zeroed the new slots.
    ctx->entry_planes       = new_arr;
    ctx->entry_planes_count = count;
}

// ====================================================================
// Pixel rendering helpers (reused by per-entry renderer)
// ====================================================================

#if N00B_HAVE_FREETYPE

/*
 * Per-frame pixel metrics, computed once and passed to each entry.
 */
typedef struct {
    int pixel_h;    // cell height in pixels
    int pixel_w;    // cell width in pixels
    int baseline;   // baseline offset for text rendering
} pixel_metrics_t;

// Compare two cell styles for run-grouping purposes.
static uint32_t style_bg_color(const n00b_text_style_t *style, uint32_t fallback);
static uint32_t style_fg_color(const n00b_text_style_t *style, uint32_t fallback);

static bool
styles_match(const n00b_text_style_t *a, const n00b_text_style_t *b)
{
    uint32_t a_fg = 0;
    uint32_t a_bg = 0;
    uint32_t b_fg = 0;
    uint32_t b_bg = 0;

    if (a == b) {
        return true;
    }
    if (!a || !b) {
        return false;
    }

    a_fg = style_fg_color(a, 0);
    a_bg = style_bg_color(a, 0);
    b_fg = style_fg_color(b, 0);
    b_bg = style_bg_color(b, 0);

    return a_fg              == b_fg
        && a_bg              == b_bg
        && a->bold           == b->bold
        && a->italic         == b->italic
        && a->underline      == b->underline
        && a->double_underline == b->double_underline
        && a->strikethrough  == b->strikethrough
        && a->reverse        == b->reverse
        && a->dim            == b->dim
        && a->font_hint      == b->font_hint
        && a->font_size      == b->font_size;
}

static bool
style_resolve_color(const n00b_text_style_t *style, bool foreground, uint32_t *out)
{
    if (!style || !out) {
        return false;
    }

    n00b_color_t rgb = foreground ? style->fg_rgb : style->bg_rgb;
    int8_t       ix  = foreground ? style->fg_palette_ix : style->bg_palette_ix;

    if (n00b_color_is_set(rgb)) {
        *out = (uint32_t)n00b_color_rgb(rgb);
        return true;
    }

    if (ix >= 0 && ix < N00B_PAL_SIZE) {
        n00b_color_t resolved = n00b_theme_resolve_color(ix);

        if (n00b_color_is_set(resolved)) {
            *out = (uint32_t)n00b_color_rgb(resolved);
            return true;
        }
    }

    return false;
}

// Extract style properties into local variables.
static void
extract_style(const n00b_text_style_t *style,
              uint32_t *fg, uint32_t *bg, bool *bold,
              bool *uline, bool *strike, uint8_t *hint, int16_t *font_size)
{
    *fg        = 0xFFFFFF;
    *bg        = 0x000000;
    *bold      = false;
    *uline     = false;
    *strike    = false;
    *hint      = 0;
    *font_size = 0;

    if (style) {
        (void)style_resolve_color(style, true, fg);
        (void)style_resolve_color(style, false, bg);
        *bold      = (style->bold == N00B_TRI_YES);
        *uline     = (style->underline == N00B_TRI_YES);
        *strike    = (style->strikethrough == N00B_TRI_YES);
        *hint      = (uint8_t)(style->font_hint < NC_NUM_FONT_HINTS
                                   ? style->font_hint : 0);
        *font_size = style->font_size;
    }
}

// Resolve the face for a style's font hint, with fallback to default.
static FT_Face
resolve_style_face(nc_ctx_t *ctx, uint8_t hint)
{
    if (hint >= NC_NUM_FONT_HINTS) {
        hint = N00B_FONT_DEFAULT;
    }

    FT_Face face = ctx->ft_faces[hint];
    if (!face) {
        face = ctx->ft_faces[0];
    }

    return face;
}

// Style font size follows the existing backend convention:
// scale the base cell pixel height by font_size/14.
// If unset, fall back to the backend's cell pixel height.
static int
style_pixel_height(nc_ctx_t *ctx, int16_t font_size)
{
    int pixel_h = (int)ctx->cell_pixel_h;

    if (font_size > 0) {
        pixel_h = pixel_h * (int)font_size / 14;
    }

    if (pixel_h < 1) {
        pixel_h = 1;
    }

    return pixel_h;
}

// Apply a pixel size to a face and return that request size while
// exposing derived ascent/descent/line height for layout consistency.
static int
fit_face_pixel_size(FT_Face face,
                    int     target_line_h,
                    int    *out_ascent,
                    int    *out_descent,
                    int    *out_line_h)
{
    if (target_line_h < 1) {
        target_line_h = 1;
    }

    if (!face) {
        if (out_ascent) {
            *out_ascent = target_line_h;
        }
        if (out_descent) {
            *out_descent = 0;
        }
        if (out_line_h) {
            *out_line_h = target_line_h;
        }
        return target_line_h;
    }

    int req     = target_line_h;
    int ascent  = 0;
    int descent = 0;
    int line_h  = target_line_h;

    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)req);

    ascent  = (int)(face->size->metrics.ascender >> 6);
    descent = (int)(-face->size->metrics.descender >> 6);
    line_h  = ascent + descent;

    if (line_h < 1) {
        line_h = req;
    }

    if (out_ascent) {
        *out_ascent = ascent;
    }
    if (out_descent) {
        *out_descent = descent;
    }
    if (out_line_h) {
        *out_line_h = line_h;
    }

    return req;
}

// Maximum bytes for a style run's text buffer.
#define RUN_TEXT_MAX 1024

// Extract a packed 0x00RRGGBB color from a style, with a fallback default.
static uint32_t
style_bg_color(const n00b_text_style_t *style, uint32_t fallback)
{
    uint32_t resolved = fallback;

    if (style_resolve_color(style, false, &resolved)) {
        return resolved;
    }
    return fallback;
}

static uint32_t
style_fg_color(const n00b_text_style_t *style, uint32_t fallback)
{
    uint32_t resolved = fallback;

    if (style_resolve_color(style, true, &resolved)) {
        return resolved;
    }
    return fallback;
}

static size_t
text_next_style_boundary(n00b_string_t *text, size_t byte_pos)
{
    size_t boundary;

    if (!text) {
        return 0;
    }

    boundary = text->u8_bytes;

    auto info_opt = n00b_str_get_style_info(text);
    if (!n00b_option_is_set(info_opt)) {
        return boundary;
    }

    n00b_string_style_info_t *info = n00b_option_get(info_opt);

    for (int64_t i = 0; i < info->num_styles; i++) {
        n00b_style_record_t *rec = &info->styles[i];

        if (rec->start > byte_pos && rec->start < boundary) {
            boundary = rec->start;
        }

        if (n00b_option_is_set(rec->end)) {
            size_t end = n00b_option_get(rec->end);

            if (end > byte_pos && end < boundary) {
                boundary = end;
            }
        }
    }

    return boundary;
}

static n00b_text_style_t *
text_resolve_effective_style(n00b_string_t      *text,
                             n00b_text_style_t  *fallback_style,
                             size_t              byte_pos)
{
    auto info_opt = n00b_str_get_style_info(text);

    if (!n00b_option_is_set(info_opt)) {
        return fallback_style;
    }

    n00b_text_style_t *resolved = n00b_str_resolve_style_at(text, byte_pos);
    n00b_text_style_t *merged;

    if (!fallback_style) {
        return resolved;
    }

    merged = n00b_str_style_merge(fallback_style, resolved);
    n00b_free(resolved);

    return merged;
}

// Fill a rectangle in an RGBA buffer with a solid color.
// stride = buffer width in pixels, buf_h = buffer height in pixels.
static void
rgba_fill_rect(uint8_t *rgba, int stride, int buf_h,
               int x, int y, int w, int h,
               uint32_t color)
{
    // Clamp to buffer bounds.
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > stride) { w = stride - x; }
    if (y + h > buf_h)  { h = buf_h - y; }
    if (w <= 0 || h <= 0) { return; }

    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;

    for (int py = y; py < y + h; py++) {
        for (int px = x; px < x + w; px++) {
            int idx = (py * stride + px) * 4;
            rgba[idx + 0] = r;
            rgba[idx + 1] = g;
            rgba[idx + 2] = b;
            rgba[idx + 3] = 255;
        }
    }
}

// Composite a source RGBA image onto a destination RGBA image at (dst_x, dst_y).
// Copies opaquely (no alpha blend — source overwrites dest).
static void
rgba_blit(uint8_t *dst, int dst_stride,
          const uint8_t *src, int src_w, int src_h,
          int dst_x, int dst_y,
          int clip_w, int clip_h)
{
    for (int sy = 0; sy < src_h; sy++) {
        int dy = dst_y + sy;
        if (dy < 0) continue;
        if (dy >= clip_h) break;
        for (int sx = 0; sx < src_w; sx++) {
            int dx = dst_x + sx;
            if (dx < 0) continue;
            if (dx >= clip_w) break;

            int si = (sy * src_w + sx) * 4;
            int di = (dy * dst_stride + dx) * 4;
            dst[di + 0] = src[si + 0];
            dst[di + 1] = src[si + 1];
            dst[di + 2] = src[si + 2];
            dst[di + 3] = 255;
        }
    }
}

// ====================================================================
// Border rendering into RGBA buffer
// ====================================================================

/*
 * Encode a single Unicode codepoint into a UTF-8 buffer.
 * Returns the number of bytes written.
 */
static int
utf8_encode_cp(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/*
 * Render a single border character via FreeType and composite it
 * onto the entry RGBA buffer at pixel position (px, py).
 */
static void
nc_render_border_char(nc_ctx_t *ctx, uint8_t *rgba,
                      int buf_w, int buf_h,
                      int px, int py, int cell_w, int cell_h,
                      n00b_codepoint_t cp,
                      uint32_t fg, uint32_t bg,
                      const pixel_metrics_t *pm)
{
    if (cp == 0) {
        return;
    }

    char text[8];
    int  n = utf8_encode_cp(cp, text);
    text[n] = '\0';

    int box_w = 0, box_h = 0;
    uint8_t *box = ft_render_text(ctx, ctx->ft_faces[0], text,
                                   pm->pixel_h, fg, bg, false,
                                   &box_w, &box_h);
    if (!box) {
        // Fill the cell with bg color as fallback.
        if (px >= 0 && py >= 0 && px + cell_w <= buf_w && py + cell_h <= buf_h) {
            rgba_fill_rect(rgba, buf_w, buf_h, px, py, cell_w, cell_h, bg);
        }
        return;
    }

    // Center the rendered glyph within the cell.
    int y_off = (cell_h - box_h) / 2;
    int x_off = (cell_w - box_w) / 2;

    rgba_blit(rgba, buf_w, box, box_w, box_h,
              px + x_off, py + y_off, buf_w, buf_h);
    n00b_free(box);
}

/*
 * Render box borders into an entry's RGBA buffer.
 *
 * Border characters occupy one cell on each active edge.
 * Padding area (between border and content) is filled with fill_style bg.
 */
static void
nc_render_border_to_rgba(nc_ctx_t              *ctx,
                         uint8_t               *rgba,
                         int                    buf_w,
                         int                    buf_h,
                         const n00b_entry_info_t *info,
                         const pixel_metrics_t  *pm)
{
    const n00b_box_props_t *box = info->box;
    if (!box || !box->border_theme) {
        return;
    }

    const n00b_border_theme_t *theme = box->border_theme;
    n00b_border_set_t borders = box->borders;
    int cpw = pm->pixel_w;
    int cph = pm->pixel_h;

    uint32_t fg = style_fg_color(info->border_style, 0xFFFFFF);
    uint32_t bg = style_bg_color(info->fill_style, 0x000000);

    int has_top    = (borders & N00B_BORDER_TOP) ? 1 : 0;
    int has_bottom = (borders & N00B_BORDER_BOTTOM) ? 1 : 0;
    int has_left   = (borders & N00B_BORDER_LEFT) ? 1 : 0;
    int has_right  = (borders & N00B_BORDER_RIGHT) ? 1 : 0;

    int inner_cols = (buf_w - (has_left + has_right) * cpw) / cpw;
    int inner_rows = (buf_h - (has_top + has_bottom) * cph) / cph;

    // Top border row.
    if (has_top) {
        // Top-left corner.
        if (has_left) {
            nc_render_border_char(ctx, rgba, buf_w, buf_h,
                                  0, 0, cpw, cph,
                                  theme->upper_left, fg, bg, pm);
        }
        // Top horizontal edges.
        for (int c = 0; c < inner_cols; c++) {
            nc_render_border_char(ctx, rgba, buf_w, buf_h,
                                  (has_left + c) * cpw, 0, cpw, cph,
                                  theme->horizontal, fg, bg, pm);
        }
        // Top-right corner.
        if (has_right) {
            nc_render_border_char(ctx, rgba, buf_w, buf_h,
                                  (has_left + inner_cols) * cpw, 0, cpw, cph,
                                  theme->upper_right, fg, bg, pm);
        }
    }

    // Bottom border row.
    if (has_bottom) {
        int by = (has_top + inner_rows) * cph;
        if (has_left) {
            nc_render_border_char(ctx, rgba, buf_w, buf_h,
                                  0, by, cpw, cph,
                                  theme->lower_left, fg, bg, pm);
        }
        for (int c = 0; c < inner_cols; c++) {
            nc_render_border_char(ctx, rgba, buf_w, buf_h,
                                  (has_left + c) * cpw, by, cpw, cph,
                                  theme->horizontal, fg, bg, pm);
        }
        if (has_right) {
            nc_render_border_char(ctx, rgba, buf_w, buf_h,
                                  (has_left + inner_cols) * cpw, by, cpw, cph,
                                  theme->lower_right, fg, bg, pm);
        }
    }

    // Left border column.
    if (has_left) {
        for (int r = 0; r < inner_rows; r++) {
            nc_render_border_char(ctx, rgba, buf_w, buf_h,
                                  0, (has_top + r) * cph, cpw, cph,
                                  theme->vertical, fg, bg, pm);
        }
    }

    // Right border column.
    if (has_right) {
        int rx = (has_left + inner_cols) * cpw;
        for (int r = 0; r < inner_rows; r++) {
            nc_render_border_char(ctx, rgba, buf_w, buf_h,
                                  rx, (has_top + r) * cph, cpw, cph,
                                  theme->vertical, fg, bg, pm);
        }
    }
}

// ====================================================================
// Per-entry pixel rendering
// ====================================================================

/*
 * Render one composite entry into an RGBA buffer and blit it to an
 * ncplane via ncvisual.  All coordinates are in pixels.
 *
 * If `reuse_ncp` is non-null, the existing ncplane is resized/moved
 * and re-blitted into instead of creating a new one.  This avoids
 * kitty pixel protocol artifacts from destroying ncplanes mid-frame.
 *
 * Returns the ncplane for this entry (caller tracks for cleanup).
 */
static struct ncplane *
nc_render_entry(nc_ctx_t                     *ctx,
                const n00b_composite_entry_t *entry,
                n00b_text_style_t            *default_style,
                const pixel_metrics_t        *pm,
                struct ncplane               *reuse_ncp)
{
    n00b_plane_t *plane = entry->plane;

    if (!(plane->flags & N00B_PLANE_VISIBLE)) {
        return nullptr;
    }

    // Skip planes with no visual content — don't waste a sprixel on
    // an empty background plane.  The stdplane's default bg handles it.
    if (plane->draw_list.count == 0 && !plane->box) {
        return nullptr;
    }

    int cpw = pm->pixel_w;
    int cph = pm->pixel_h;

    // Get entry info (pixel geometry, styles).
    n00b_entry_info_t info;
    n00b_composite_entry_info(entry, &info, cpw, cph);

    int outer_w = (int)info.outer_cols;
    int outer_h = (int)info.outer_rows;
    if (outer_w <= 0 || outer_h <= 0) {
        return nullptr;
    }

    // Skip if entirely outside clip rect.
    if (info.outer_x + outer_w <= entry->clip_x
        || info.outer_y + outer_h <= entry->clip_y
        || info.outer_x >= entry->clip_x + entry->clip_w
        || info.outer_y >= entry->clip_y + entry->clip_h) {
        return nullptr;
    }

    // Allocate RGBA buffer for the full outer box.
    size_t rgba_size = (size_t)(outer_w * outer_h * 4);
    uint8_t *rgba = n00b_alloc_array(uint8_t, rgba_size);
    if (!rgba) {
        return nullptr;
    }

    // Fill entire buffer with background color.
    uint32_t bg_color = style_bg_color(info.fill_style,
                            style_bg_color(default_style, 0x000000));
    rgba_fill_rect(rgba, outer_w, outer_h, 0, 0, outer_w, outer_h, bg_color);

    // Render border decorations.
    nc_render_border_to_rgba(ctx, rgba, outer_w, outer_h, &info, pm);

    // Fill padding area with fill background (between border and content).
    // The fill_style bg may differ from the default bg.
    uint32_t fill_bg = style_bg_color(info.fill_style, bg_color);
    int inl = (int)info.inset_left;
    int inr = (int)info.inset_right;
    int int_top = (int)info.inset_top;
    int inb = (int)info.inset_bot;

    // Compute border thickness (1 cell if border present, else 0).
    int bdr_top = (info.box && (info.box->borders & N00B_BORDER_TOP)) ? cph : 0;
    int bdr_bot = (info.box && (info.box->borders & N00B_BORDER_BOTTOM)) ? cph : 0;
    int bdr_left = (info.box && (info.box->borders & N00B_BORDER_LEFT)) ? cpw : 0;
    int bdr_right = (info.box && (info.box->borders & N00B_BORDER_RIGHT)) ? cpw : 0;

    // Top padding strip (between top border and content).
    if (int_top > bdr_top) {
        rgba_fill_rect(rgba, outer_w, outer_h,
                       bdr_left, bdr_top,
                       outer_w - bdr_left - bdr_right, int_top - bdr_top,
                       fill_bg);
    }
    // Bottom padding strip.
    if (inb > bdr_bot) {
        rgba_fill_rect(rgba, outer_w, outer_h,
                       bdr_left, outer_h - inb,
                       outer_w - bdr_left - bdr_right, inb - bdr_bot,
                       fill_bg);
    }
    // Left padding strip.
    if (inl > bdr_left) {
        rgba_fill_rect(rgba, outer_w, outer_h,
                       bdr_left, int_top,
                       inl - bdr_left, outer_h - int_top - inb,
                       fill_bg);
    }
    // Right padding strip.
    if (inr > bdr_right) {
        rgba_fill_rect(rgba, outer_w, outer_h,
                       outer_w - inr, int_top,
                       inr - bdr_right, outer_h - int_top - inb,
                       fill_bg);
    }

    // Render content from draw commands.
    // Walk the plane's draw_list, rendering each command into the
    // RGBA buffer at the appropriate pixel position.
    // Draw command coordinates are in cell units; scale to pixels.
    int content_ox = inl;
    int content_oy = int_top;

    for (n00b_isize_t c = 0; c < plane->draw_list.count; c++) {
        const n00b_draw_cmd_t *cmd = &plane->draw_list.cmds[c];

        switch (cmd->selector) {
        case typehash(n00b_draw_text_t): {
            n00b_draw_text_t d = n00b_variant_get(*cmd, n00b_draw_text_t);
            n00b_string_t *text = d.string;
            bool           has_string_style;
            size_t         run_start = 0;
            int            run_px;
            if (!text || text->u8_bytes == 0) {
                break;
            }

            n00b_text_style_t *base_style = d.style
                                              ? d.style
                                              : info.text_style;
            has_string_style = n00b_option_is_set(n00b_str_get_style_info(text));

            // Pixel position within the RGBA buffer.
            // Draw commands are already in pixel coordinates.
            int px = content_ox + d.x - plane->scroll_x;
            int py = content_oy + d.y - plane->scroll_y;
            run_px = px;

            while (run_start < text->u8_bytes) {
                size_t run_end = has_string_style
                                   ? text_next_style_boundary(text, run_start)
                                   : text->u8_bytes;
                n00b_text_style_t *run_style = has_string_style
                                                 ? text_resolve_effective_style(text,
                                                                                base_style,
                                                                                run_start)
                                                 : base_style;
                n00b_string_t *run_text = run_start == 0 && run_end == text->u8_bytes
                                            ? text
                                            : n00b_string_from_raw(text->data + run_start,
                                                                   (int64_t)(run_end - run_start));
                uint32_t text_bg = style_bg_color(run_style,
                                       style_bg_color(info.text_style, fill_bg));
                uint32_t fg, bg;
                bool     is_bold, is_uline, is_strike;
                uint8_t  hint;
                int16_t  font_size;

                if (run_end <= run_start) {
                    break;
                }

                extract_style(run_style, &fg, &bg, &is_bold,
                               &is_uline, &is_strike, &hint, &font_size);

                FT_Face face = resolve_style_face(ctx, hint);

                int target_h = style_pixel_height(ctx, font_size);
                int max_h = pm->pixel_h * 4;
                if (target_h > max_h) {
                    target_h = max_h;
                }

                int ascent_px = target_h;
                int descent_px = 0;
                int line_h_px = target_h;
                int render_ph = fit_face_pixel_size(face,
                                                    target_h,
                                                    &ascent_px,
                                                    &descent_px,
                                                    &line_h_px);

                int box_w = 0, box_h = 0;
                uint8_t *box = ft_render_text(ctx, face, run_text->data, render_ph,
                                               fg, text_bg, is_bold, &box_w, &box_h);
                if (!box) {
                    if (has_string_style) {
                        n00b_free(run_style);
                    }
                    break;
                }

                // Vertically center within the style's effective line height.
                int draw_h = line_h_px > 0 ? line_h_px : cph;
                int y_off = (draw_h > box_h) ? (draw_h - box_h) / 2 : 0;

                // Fill background behind the text area.
                if (box_w > 0 && draw_h > 0 && run_px >= 0 && py >= 0) {
                    rgba_fill_rect(rgba, outer_w, outer_h,
                                   run_px, py, box_w, draw_h, text_bg);
                }

                rgba_blit(rgba, outer_w, box, box_w, box_h,
                          run_px, py + y_off, outer_w, outer_h);
                n00b_free(box);

                if (is_uline || is_strike) {
                    int dec_end = run_px + box_w;
                    if (dec_end > outer_w) dec_end = outer_w;

                    uint8_t fg_r = (fg >> 16) & 0xFF;
                    uint8_t fg_g = (fg >> 8) & 0xFF;
                    uint8_t fg_b = fg & 0xFF;

                    if (is_uline) {
                        ft_draw_hline(rgba, outer_w, outer_h,
                                      py + draw_h - 2,
                                      run_px, dec_end, fg_r, fg_g, fg_b);
                    }
                    if (is_strike) {
                        ft_draw_hline(rgba, outer_w, outer_h,
                                      py + draw_h / 2,
                                      run_px, dec_end, fg_r, fg_g, fg_b);
                    }
                }

                run_px += box_w;
                run_start = run_end;

                if (has_string_style) {
                    n00b_free(run_style);
                }
            }
            break;
        }

        case typehash(n00b_draw_glyph_t): {
            n00b_draw_glyph_t d = n00b_variant_get(*cmd, n00b_draw_glyph_t);
            const n00b_text_style_t *glyph_style = d.style
                                                      ? d.style
                                                      : info.text_style;

            // Draw commands are already in pixel coordinates.
            int px = content_ox + d.x - plane->scroll_x;
            int py = content_oy + d.y - plane->scroll_y;

            // Encode codepoint to UTF-8 for FreeType.
            char cp_buf[5];
            int cp_len = 0;
            n00b_codepoint_t gcp = d.cp;

            if (gcp < 0x80) {
                cp_buf[0] = (char)gcp;
                cp_len = 1;
            }
            else if (gcp < 0x800) {
                cp_buf[0] = (char)(0xC0 | (gcp >> 6));
                cp_buf[1] = (char)(0x80 | (gcp & 0x3F));
                cp_len = 2;
            }
            else if (gcp < 0x10000) {
                cp_buf[0] = (char)(0xE0 | (gcp >> 12));
                cp_buf[1] = (char)(0x80 | ((gcp >> 6) & 0x3F));
                cp_buf[2] = (char)(0x80 | (gcp & 0x3F));
                cp_len = 3;
            }
            else {
                cp_buf[0] = (char)(0xF0 | (gcp >> 18));
                cp_buf[1] = (char)(0x80 | ((gcp >> 12) & 0x3F));
                cp_buf[2] = (char)(0x80 | ((gcp >> 6) & 0x3F));
                cp_buf[3] = (char)(0x80 | (gcp & 0x3F));
                cp_len = 4;
            }
            cp_buf[cp_len] = '\0';

            uint32_t fg, bg;
            bool     is_bold, is_uline, is_strike;
            uint8_t  hint;
            int16_t  font_size;
            extract_style(glyph_style, &fg, &bg, &is_bold,
                           &is_uline, &is_strike, &hint, &font_size);

            FT_Face face = resolve_style_face(ctx, hint);

            int target_h = style_pixel_height(ctx, font_size);
            int max_h = pm->pixel_h * 4;
            if (target_h > max_h) {
                target_h = max_h;
            }

            int ascent_px = target_h;
            int descent_px = 0;
            int line_h_px = target_h;
            int render_ph = fit_face_pixel_size(face,
                                                target_h,
                                                &ascent_px,
                                                &descent_px,
                                                &line_h_px);

            uint32_t glyph_bg = style_bg_color(glyph_style,
                                     style_bg_color(info.text_style, fill_bg));

            int box_w = 0, box_h = 0;
            uint8_t *box = ft_render_text(ctx, face, cp_buf, render_ph,
                                           fg, glyph_bg, is_bold, &box_w, &box_h);
            if (box) {
                int draw_h = line_h_px > 0 ? line_h_px : cph;
                int y_off = (draw_h > box_h) ? (draw_h - box_h) / 2 : 0;
                rgba_blit(rgba, outer_w, box, box_w, box_h,
                          px, py + y_off, outer_w, outer_h);
                n00b_free(box);
            }
            break;
        }

        case typehash(n00b_draw_fill_rect_t): {
            n00b_draw_fill_rect_t d = n00b_variant_get(*cmd, n00b_draw_fill_rect_t);
            const n00b_text_style_t *fill_style = d.style
                                                     ? d.style
                                                     : info.fill_style;

            // Draw commands are already in pixel coordinates.
            int px = content_ox + d.x - plane->scroll_x;
            int py = content_oy + d.y - plane->scroll_y;
            int pw = d.w;
            int ph = d.h;

            uint32_t rect_bg = style_bg_color(fill_style,
                                   style_bg_color(info.fill_style, fill_bg));

            if (pw > 0 && ph > 0) {
                rgba_fill_rect(rgba, outer_w, outer_h, px, py, pw, ph, rect_bg);
            }

            // If the fill codepoint is not a space, render it tiled.
            if (d.cp != ' ' && d.cp != 0) {
                // Encode fill codepoint.
                char fill_buf[5];
                int fill_len = 0;
                n00b_codepoint_t fcp = d.cp;

                if (fcp < 0x80) {
                    fill_buf[0] = (char)fcp;
                    fill_len = 1;
                }
                else if (fcp < 0x800) {
                    fill_buf[0] = (char)(0xC0 | (fcp >> 6));
                    fill_buf[1] = (char)(0x80 | (fcp & 0x3F));
                    fill_len = 2;
                }
                else if (fcp < 0x10000) {
                    fill_buf[0] = (char)(0xE0 | (fcp >> 12));
                    fill_buf[1] = (char)(0x80 | ((fcp >> 6) & 0x3F));
                    fill_buf[2] = (char)(0x80 | (fcp & 0x3F));
                    fill_len = 3;
                }
                else {
                    fill_buf[0] = (char)(0xF0 | (fcp >> 18));
                    fill_buf[1] = (char)(0x80 | ((fcp >> 12) & 0x3F));
                    fill_buf[2] = (char)(0x80 | ((fcp >> 6) & 0x3F));
                    fill_buf[3] = (char)(0x80 | (fcp & 0x3F));
                    fill_len = 4;
                }
                fill_buf[fill_len] = '\0';

                uint32_t fg, dummy_bg;
                bool     is_bold, is_uline, is_strike;
                uint8_t  hint;
                int16_t  font_size;
                extract_style(fill_style, &fg, &dummy_bg, &is_bold,
                               &is_uline, &is_strike, &hint, &font_size);

                FT_Face face = resolve_style_face(ctx, hint);

                int target_h = style_pixel_height(ctx, font_size);
                int max_h = pm->pixel_h * 4;
                if (target_h > max_h) {
                    target_h = max_h;
                }

                int render_ph = fit_face_pixel_size(face,
                                                    target_h,
                                                    nullptr,
                                                    nullptr,
                                                    nullptr);

                // Render a single glyph, tile it across the rect.
                int gbox_w = 0, gbox_h = 0;
                uint8_t *gbox = ft_render_text(ctx, face, fill_buf,
                                                render_ph, fg, rect_bg,
                                                is_bold, &gbox_w, &gbox_h);
                if (gbox && gbox_w > 0 && gbox_h > 0) {
                    for (int ty = py; ty < py + ph; ty += gbox_h) {
                        for (int tx = px; tx < px + pw; tx += gbox_w) {
                            rgba_blit(rgba, outer_w, gbox, gbox_w, gbox_h,
                                      tx, ty, outer_w, outer_h);
                        }
                    }
                    n00b_free(gbox);
                }
            }
            break;
        }
        }
    }

    // Position/size in cell coordinates. Preserve sub-cell pixel offsets so
    // visual placement stays aligned with pixel-space hit-testing/layout.
    int plane_pxoff_y = info.outer_y % cph;
    int plane_pxoff_x = info.outer_x % cpw;
    if (plane_pxoff_y < 0) plane_pxoff_y += cph;
    if (plane_pxoff_x < 0) plane_pxoff_x += cpw;

    int plane_cell_y = (info.outer_y - plane_pxoff_y) / cph;
    int plane_cell_x = (info.outer_x - plane_pxoff_x) / cpw;
    int plane_cell_h = (outer_h + plane_pxoff_y + cph - 1) / cph;
    int plane_cell_w = (outer_w + plane_pxoff_x + cpw - 1) / cpw;
    if (plane_cell_h < 1) plane_cell_h = 1;
    if (plane_cell_w < 1) plane_cell_w = 1;

    // Always destroy+recreate the visual plane on cache miss.
    if (reuse_ncp) {
        ncplane_family_destroy(reuse_ncp);
    }

    // Log content summary.
    if (n00b_display_diag_would_log(N00B_DISPLAY_DIAG_TRACE)) {
        // Collect sample text from draw commands.
        char sample[128] = {0};
        int slen = 0;
        int text_cmds = 0;
        int glyph_cmds = 0;
        int fill_cmds = 0;

        for (n00b_isize_t c = 0; c < plane->draw_list.count; c++) {
            const n00b_draw_cmd_t *cmd = &plane->draw_list.cmds[c];
            switch (cmd->selector) {
            case typehash(n00b_draw_text_t): {
                n00b_draw_text_t d = n00b_variant_get(*cmd, n00b_draw_text_t);
                text_cmds++;
                if (slen < 60 && d.string && d.string->u8_bytes > 0) {
                    int tlen = (int)d.string->u8_bytes;
                    if (tlen > 60 - slen) tlen = 60 - slen;
                    memcpy(sample + slen, d.string->data, tlen);
                    slen += tlen;
                }
                break;
            }
            case typehash(n00b_draw_glyph_t):
                glyph_cmds++;
                break;
            case typehash(n00b_draw_fill_rect_t):
                fill_cmds++;
                break;
            }
        }
        sample[slen] = '\0';

        n00b_display_diag_log(N00B_DISPLAY_DIAG_TRACE,
                               "backend_notcurses",
                               "entry z=%d outer=%dx%d vp=%ux%u draw_cmds=%u (text=%d glyph=%d fill=%d) content_ofs=%d,%d text=\"%s\"",
                               entry->abs_z,
                               outer_w,
                               outer_h,
                               (unsigned)(plane->width / cpw),
                               (unsigned)(plane->height / cph),
                               (unsigned)plane->draw_list.count,
                               text_cmds,
                               glyph_cmds,
                               fill_cmds,
                               content_ox,
                               content_oy,
                               sample);

        // Dump each draw command's coordinates.
        for (n00b_isize_t c = 0; c < plane->draw_list.count; c++) {
            const n00b_draw_cmd_t *cmd = &plane->draw_list.cmds[c];
            if (n00b_variant_is_type(*cmd, n00b_draw_text_t)) {
                n00b_draw_text_t d = n00b_variant_get(*cmd, n00b_draw_text_t);
                if (d.string) {
                    n00b_display_diag_log(N00B_DISPLAY_DIAG_TRACE,
                                           "backend_notcurses",
                                           "cmd[%d] TEXT x=%d y=%d style=%p '%.*s'",
                                           (int)c,
                                           d.x,
                                           d.y,
                                           (void *)d.style,
                                           (int)d.string->u8_bytes,
                                           d.string->data);
                }
            }
        }
    }

    struct ncvisual *visual =
        ncvisual_from_rgba(rgba, outer_h, outer_w * 4, outer_w);
    n00b_free(rgba);

    if (!visual) {
        return nullptr;
    }

    struct ncvisual_options vopts = {
        .n       = ctx->stdplane,
        .scaling = NCSCALE_NONE,
        .y       = plane_cell_y,
        .x       = plane_cell_x,
        .blitter = NCBLIT_PIXEL,
        .pxoffy  = (unsigned)plane_pxoff_y,
        .pxoffx  = (unsigned)plane_pxoff_x,
        // Ensure ncvisual_blit returns the real drawable child plane that
        // we can cache/move/destroy directly in subsequent frames.
        .flags   = NCVISUAL_OPTION_CHILDPLANE,
    };

    struct ncplane *blit_plane = ncvisual_blit(ctx->nc, visual, &vopts);

    n00b_display_diag_log(N00B_DISPLAY_DIAG_TRACE,
                           "backend_notcurses",
                           "blit %s @cell(%d,%d) %dx%d cells",
                           blit_plane ? "OK" : "FAIL",
                           plane_cell_x,
                           plane_cell_y,
                           plane_cell_w,
                           plane_cell_h);

    ncvisual_destroy(visual);
    return blit_plane;
}

#endif /* N00B_HAVE_FREETYPE */

// ====================================================================
// Resize detection
// ====================================================================

static void
check_resize(nc_ctx_t *ctx)
{
    if (!ctx->resize_cb) {
        return;
    }

    unsigned int rows = 0, cols = 0;
    ncplane_dim_yx(ctx->stdplane, &rows, &cols);

    if (rows != ctx->last_rows || cols != ctx->last_cols) {
        ctx->last_rows = rows;
        ctx->last_cols = cols;
        ctx->resize_cb((n00b_isize_t)rows, (n00b_isize_t)cols,
                        ctx->resize_user);
    }
}

// ====================================================================
// Vtable implementation
// ====================================================================

static void *
nc_init(n00b_conduit_topic_t(n00b_buffer_t *) *output)
{
    (void)output; // notcurses writes directly to the terminal.

    nc_ctx_t *ctx = n00b_alloc(nc_ctx_t);
    if (!ctx) {
        return nullptr;
    }

    // The n00b runtime owns fatal-signal handlers and their alternate
    // signal stack.  If notcurses also installs fatal handlers it
    // allocates its own sigaltstack; on notcurses_stop() it tries to
    // remove it, but the stack may still be active on the calling
    // thread, causing "couldn't remove alternate signal stack
    // (invalid argument)".
    //
    // NCOPTION_NO_QUIT_SIGHANDLERS prevents this — notcurses skips
    // SIGINT/SIGTERM/etc. and never touches sigaltstack.  We leave
    // SIGWINCH to notcurses so it gets proper resize events and can
    // run its terminal probe without interference.
    struct notcurses_options opts = {
        .flags = NCOPTION_SUPPRESS_BANNERS
               | NCOPTION_NO_QUIT_SIGHANDLERS,
    };

    ctx->nc = notcurses_init(&opts, nullptr);
    if (!ctx->nc) {
        n00b_display_diag_log(N00B_DISPLAY_DIAG_ERROR,
                               "backend_notcurses",
                               "notcurses_init failed");
        n00b_free(ctx);
        return nullptr;
    }

    ctx->stdplane = notcurses_stdplane(ctx->nc);

    // Disable line signals so Ctrl+C/Ctrl+Z come through as key
    // events instead of generating SIGINT/SIGTSTP.
    notcurses_linesigs_disable(ctx->nc);

    // Enable mouse events (button + drag, no bare motion).
    // NCMICE_ALL_EVENTS causes a flood of motion events on every
    // pixel movement which overwhelms the event loop.
    notcurses_mice_enable(ctx->nc, NCMICE_DRAG_EVENT);

    // Cache dimensions.
    unsigned int rows = 0, cols = 0;
    ncplane_dim_yx(ctx->stdplane, &rows, &cols);
    ctx->last_rows = rows;
    ctx->last_cols = cols;

    // Query pixel geometry.
    unsigned int pix_y = 0, pix_x = 0;
    ncplane_pixel_geom(ctx->stdplane, nullptr, nullptr,
                       &pix_y, &pix_x, nullptr, nullptr);
    ctx->cell_pixel_h = pix_y > 0 ? pix_y : 1;
    ctx->cell_pixel_w = pix_x > 0 ? pix_x : 1;
    n00b_terminal_input_reset(&ctx->input_state);

    // Detect pixel support.
    ctx->has_pixel = (notcurses_check_pixel_support(ctx->nc) > 0);

#if N00B_HAVE_FREETYPE
    ft_init(ctx);
    if (ctx->has_freetype) {
        ctx->glyph_cache = n00b_alloc(glyph_cache_t);
    }
#endif

    n00b_display_diag_log(N00B_DISPLAY_DIAG_INFO,
                           "backend_notcurses",
                           "init cells=%ux%u cell_pixel=%ux%u has_pixel=%d has_freetype=%d",
                           cols,
                           rows,
                           pix_x,
                           pix_y,
                           ctx->has_pixel,
#if N00B_HAVE_FREETYPE
                           ctx->has_freetype
#else
                           0
#endif
                           );

    return ctx;
}

static void
nc_destroy(void *vctx)
{
    nc_ctx_t *ctx = vctx;
    if (!ctx) {
        return;
    }

    if (ctx->comp_grid) {
        n00b_free(ctx->comp_grid);
        ctx->comp_grid = nullptr;
    }
    n00b_composite_style_pool_destroy(&ctx->style_pool);

#if N00B_HAVE_FREETYPE
    glyph_cache_destroy(ctx->glyph_cache);
    ctx->glyph_cache = NULL;
    ft_destroy(ctx);
#endif

    destroy_entry_planes(ctx);
    n00b_free(ctx->entry_planes);
    ctx->entry_planes       = nullptr;
    ctx->entry_planes_count = 0;
    n00b_display_diag_log(N00B_DISPLAY_DIAG_TRACE,
                           "backend_notcurses",
                           "destroy");

    if (ctx->nc) {
        notcurses_stop(ctx->nc);
    }
    n00b_free(ctx);
}

static n00b_render_cap_t
nc_capabilities(void *vctx)
{
    nc_ctx_t *ctx = vctx;

    n00b_render_cap_t caps =
        N00B_RCAP_COLOR_24BIT
      | N00B_RCAP_COLOR_256
      | N00B_RCAP_COLOR_BASIC
      | N00B_RCAP_BOLD
      | N00B_RCAP_ITALIC
      | N00B_RCAP_UNDERLINE
      | N00B_RCAP_STRIKETHROUGH
      | N00B_RCAP_DIM
      | N00B_RCAP_CURSOR_MOVE
      | N00B_RCAP_ALT_SCREEN
      | N00B_RCAP_MANAGES_TTY
      | N00B_RCAP_UNICODE
      | N00B_RCAP_WIDE_CHARS
      | N00B_RCAP_DIFF_RENDER
      | N00B_RCAP_MOUSE;

    if (ctx->has_pixel) {
        caps |= N00B_RCAP_PIXEL_COORDS | N00B_RCAP_FONT_METRICS;
    }

    return caps;
}

static n00b_render_size_t
nc_get_size(void *vctx)
{
    nc_ctx_t *ctx = vctx;

    unsigned int rows = 0, cols = 0;
    ncplane_dim_yx(ctx->stdplane, &rows, &cols);

    // Refresh pixel geometry.
    unsigned int pix_y = 0, pix_x = 0;
    unsigned int total_py = 0, total_px = 0;
    ncplane_pixel_geom(ctx->stdplane, &total_py, &total_px,
                       &pix_y, &pix_x, nullptr, nullptr);
    if (pix_y == 0) {
        pix_y = ctx->cell_pixel_h > 0 ? ctx->cell_pixel_h : 1;
    }
    if (pix_x == 0) {
        pix_x = ctx->cell_pixel_w > 0 ? ctx->cell_pixel_w : 1;
    }
    ctx->cell_pixel_h = pix_y;
    ctx->cell_pixel_w = pix_x;

    if (total_py == 0) {
        total_py = rows * pix_y;
    }
    if (total_px == 0) {
        total_px = cols * pix_x;
    }

    return (n00b_render_size_t){
        .cols         = (n00b_isize_t)cols,
        .rows         = (n00b_isize_t)rows,
        .pixel_w      = (n00b_isize_t)total_px,
        .pixel_h      = (n00b_isize_t)total_py,
        .cell_pixel_w = (n00b_isize_t)pix_x,
        .cell_pixel_h = (n00b_isize_t)pix_y,
    };
}

static void
nc_style_to_notcurses(const n00b_text_style_t *style,
                      uint64_t                *channels_out,
                      unsigned                *styles_out)
{
    uint64_t channels = 0;
    unsigned styles   = NCSTYLE_NONE;

    ncchannels_set_fg_default(&channels);
    ncchannels_set_bg_default(&channels);
    ncchannels_set_fg_alpha(&channels, NCALPHA_OPAQUE);
    ncchannels_set_bg_alpha(&channels, NCALPHA_OPAQUE);

    if (style) {
        uint32_t fg = 0;
        uint32_t bg = 0;

        if (style_resolve_color(style, true, &fg)) {
            ncchannels_set_fg_rgb(&channels, fg);
        }
        if (style_resolve_color(style, false, &bg)) {
            ncchannels_set_bg_rgb(&channels, bg);
        }
        if (style->bold == N00B_TRI_YES) {
            styles |= NCSTYLE_BOLD;
        }
        if (style->italic == N00B_TRI_YES) {
            styles |= NCSTYLE_ITALIC;
        }
        if (style->underline == N00B_TRI_YES
            || style->double_underline == N00B_TRI_YES) {
            styles |= NCSTYLE_UNDERLINE;
        }
        if (style->strikethrough == N00B_TRI_YES) {
            styles |= NCSTYLE_STRUCK;
        }
    }

    *channels_out = channels;
    *styles_out   = styles;
}

static void
nc_render_frame(void         *vctx,
                n00b_rcell_t *cells,
                n00b_isize_t  rows,
                n00b_isize_t  cols,
                n00b_rcell_t *prev_cells)
{
    nc_ctx_t *ctx = vctx;
    if (!ctx || !ctx->stdplane || !cells || rows <= 0 || cols <= 0) {
        return;
    }

    // Full repaint for now; this path is used when pixel rendering is
    // unavailable and keeps behavior deterministic across terminals.
    ncplane_erase(ctx->stdplane);

    uint64_t last_channels = 0;
    unsigned last_styles   = NCSTYLE_NONE;
    bool     have_style    = false;

    for (n00b_isize_t r = 0; r < rows; r++) {
        for (n00b_isize_t c = 0; c < cols; c++) {
            n00b_rcell_t *cell = &cells[r * cols + c];

            if (cell->flags & N00B_CELL_WIDE_CONT) {
                continue;
            }

            uint64_t channels = 0;
            unsigned styles   = NCSTYLE_NONE;
            nc_style_to_notcurses(cell->style, &channels, &styles);

            if (!have_style || channels != last_channels) {
                ncplane_set_channels(ctx->stdplane, channels);
                last_channels = channels;
            }
            if (!have_style || styles != last_styles) {
                ncplane_set_styles(ctx->stdplane, styles);
                last_styles = styles;
            }
            have_style = true;

            if ((cell->flags & N00B_CELL_OCCUPIED) && cell->grapheme_len > 0) {
                ncplane_putstr_yx(ctx->stdplane, (int)r, (int)c, cell->grapheme);
            }
            else {
                ncplane_putchar_yx(ctx->stdplane, (int)r, (int)c, ' ');
            }
        }
    }

    (void)prev_cells;
}

static void
nc_flush(void *vctx)
{
    nc_ctx_t *ctx = vctx;
    int rc = notcurses_render(ctx->nc);
    n00b_display_diag_log(N00B_DISPLAY_DIAG_TRACE,
                           "backend_notcurses",
                           "flush render=%d",
                           rc);
    check_resize(ctx);
}

static void
nc_cursor_set_visible(void *vctx, bool visible)
{
    nc_ctx_t *ctx = vctx;
    if (visible) {
        notcurses_cursor_enable(ctx->nc, 0, 0);
    }
    else {
        notcurses_cursor_disable(ctx->nc);
    }
}

static void
nc_cursor_move(void *vctx, n00b_isize_t row, n00b_isize_t col)
{
    nc_ctx_t *ctx = vctx;
    ncplane_cursor_move_yx(ctx->stdplane, (int)row, (int)col);
}

static void
nc_alt_screen_enter(void *vctx)
{
    (void)vctx;
    // notcurses manages the alternate screen internally.
    // If we initialized with NCOPTION_NO_ALTERNATE_SCREEN,
    // re-enter alt screen here.  notcurses 3.x doesn't expose
    // a toggle API, so this is a no-op — the caller should
    // re-init if alt screen behavior is needed.
}

static void
nc_alt_screen_leave(void *vctx)
{
    (void)vctx;
    // See nc_alt_screen_enter.
}

static void
nc_on_resize(void  *vctx,
             void (*cb)(n00b_isize_t, n00b_isize_t, void *),
             void  *user_ctx)
{
    nc_ctx_t *ctx = vctx;
    ctx->resize_cb   = cb;
    ctx->resize_user = user_ctx;
}

static void
nc_prepare_gui(void *vctx, n00b_plane_t **planes, n00b_isize_t n)
{
    // No GUI extensions for terminal backends.
    (void)vctx;
    (void)planes;
    (void)n;
}

// ====================================================================
// Input polling
// ====================================================================

static bool
nc_poll_event(void *vctx, int32_t timeout_ms, n00b_event_t *out)
{
    nc_ctx_t *ctx = vctx;
    out->type = N00B_EVENT_NONE;

    // Use poll() + non-blocking get, matching slop's ctui pattern.
    int nc_fd = notcurses_inputready_fd(ctx->nc);
    struct pollfd pfd = { .fd = nc_fd, .events = POLLIN };
    // eintr: an interrupted wait reports no event, as a timeout does, and the
    // caller polls again.
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0) {
        return false;
    }

    ncinput ni;
    memset(&ni, 0, sizeof(ni));

    uint32_t r = notcurses_get_nblock(ctx->nc, &ni);
    if (r == (uint32_t)-1 || r == 0) {
        return false;
    }

    n00b_display_diag_log(N00B_DISPLAY_DIAG_TRACE,
                           "backend_notcurses",
                           "poll id=0x%x evtype=%d ctrl=%d shift=%d alt=%d eff_text[0]=0x%x",
                           ni.id,
                           ni.evtype,
                           ni.ctrl,
                           ni.shift,
                           ni.alt,
                           (unsigned)ni.eff_text[0]);

    // Resize event.
    if (ni.id == NCKEY_RESIZE) {
        out->type = N00B_EVENT_RESIZE;
        unsigned int rows = 0, cols = 0;
        ncplane_dim_yx(ctx->stdplane, &rows, &cols);
        out->resize.rows = (n00b_isize_t)rows;
        out->resize.cols = (n00b_isize_t)cols;
        ctx->last_rows = rows;
        ctx->last_cols = cols;
        return true;
    }

    n00b_terminal_ncinput_view_t view = {
        .id        = ni.id,
        .evtype    = (uint32_t)ni.evtype,
        .x         = ni.x,
        .y         = ni.y,
        .xpx       = ni.xpx,
        .ypx       = ni.ypx,
        .shift     = ni.shift,
        .ctrl      = ni.ctrl,
        .alt       = ni.alt,
        .eff_text0 = (uint32_t)ni.eff_text[0],
    };

    return n00b_terminal_translate_notcurses(&view,
                                              &ctx->input_state,
                                              ctx->cell_pixel_w,
                                              ctx->cell_pixel_h,
                                              out);
}

// ====================================================================
// Vtable
// ====================================================================

// -------------------------------------------------------------------
// Plane-based rendering (direct pixel path)
// -------------------------------------------------------------------

static void
nc_render_planes(void                         *vctx,
                 const n00b_composite_entry_t *entries,
                 n00b_isize_t                  count,
                 n00b_isize_t                  total_rows,
                 n00b_isize_t                  total_cols,
                 n00b_text_style_t            *default_style,
                 n00b_render_cap_t             caps)
{
    nc_ctx_t *ctx = vctx;
    bool use_pixel = false;
#if N00B_HAVE_FREETYPE
    use_pixel = ctx->has_freetype && ctx->has_pixel;
#endif

    if (!use_pixel) {
        n00b_isize_t cell_px_w = ctx->cell_pixel_w > 0
                               ? (n00b_isize_t)ctx->cell_pixel_w
                               : 1;
        n00b_isize_t cell_px_h = ctx->cell_pixel_h > 0
                               ? (n00b_isize_t)ctx->cell_pixel_h
                               : 1;
        n00b_isize_t cell_rows = total_rows / cell_px_h;
        n00b_isize_t cell_cols = total_cols / cell_px_w;

        if (cell_rows <= 0 || cell_cols <= 0) {
            return;
        }

        if (cell_rows != ctx->comp_grid_rows
            || cell_cols != ctx->comp_grid_cols) {
            size_t total = (size_t)cell_rows * (size_t)cell_cols;
            n00b_rcell_t *new_grid = n00b_alloc_array(n00b_rcell_t, total);

            if (ctx->comp_grid) {
                n00b_free(ctx->comp_grid);
            }
            ctx->comp_grid      = new_grid;
            ctx->comp_grid_rows = cell_rows;
            ctx->comp_grid_cols = cell_cols;
        }

        n00b_composite_style_pool_clear(&ctx->style_pool);
        n00b_composite_commands_to_grid(entries,
                                         count,
                                         ctx->comp_grid,
                                         cell_rows,
                                         cell_cols,
                                         (int32_t)cell_px_w,
                                         (int32_t)cell_px_h,
                                         default_style,
                                         caps,
                                         &ctx->style_pool);
        nc_render_frame(ctx,
                        ctx->comp_grid,
                        cell_rows,
                        cell_cols,
                        nullptr);

        n00b_display_diag_log(N00B_DISPLAY_DIAG_TRACE,
                               "backend_notcurses",
                               "render_planes cell fallback rows=%d cols=%d (no pixel/freetype)",
                               (int)cell_rows,
                               (int)cell_cols);
        return;
    }

#if N00B_HAVE_FREETYPE

    int pixel_h = (int)ctx->cell_pixel_h;
    int pixel_w = (int)ctx->cell_pixel_w;
    if (pixel_h <= 0 || pixel_w <= 0 || !ctx->ft_faces[0]) {
        return;
    }

    if (ctx->cache_cell_pixel_w != (unsigned)pixel_w
        || ctx->cache_cell_pixel_h != (unsigned)pixel_h) {
        destroy_entry_planes(ctx);
        ctx->cache_cell_pixel_w = (unsigned)pixel_w;
        ctx->cache_cell_pixel_h = (unsigned)pixel_h;
    }

    // Compute font metrics once per frame.
    FT_UInt req_size = (FT_UInt)pixel_h;
    FT_Set_Pixel_Sizes(ctx->ft_faces[0], 0, req_size);

    int ascent  = (int)(ctx->ft_faces[0]->size->metrics.ascender >> 6);
    int descent = (int)(-ctx->ft_faces[0]->size->metrics.descender >> 6);
    int line_h  = ascent + descent;

    if (line_h > pixel_h) {
        req_size = (FT_UInt)((long)req_size * pixel_h / line_h);
        if (req_size < 1) {
            req_size = 1;
        }
        FT_Set_Pixel_Sizes(ctx->ft_faces[0], 0, req_size);
        ascent  = (int)(ctx->ft_faces[0]->size->metrics.ascender >> 6);
        descent = (int)(-ctx->ft_faces[0]->size->metrics.descender >> 6);
        line_h  = ascent + descent;
    }

    pixel_metrics_t pm = {
        .pixel_h  = pixel_h,
        .pixel_w  = pixel_w,
        .baseline = ascent + ((pixel_h > line_h) ? (pixel_h - line_h) / 2 : 0),
    };

    n00b_display_diag_log(N00B_DISPLAY_DIAG_TRACE,
                           "backend_notcurses",
                           "render_planes entries=%u cell_px=%dx%d",
                           (unsigned)count,
                           pixel_w,
                           pixel_h);

    // Destroy orphaned cached ncplanes (planes no longer in this frame's
    // entry list).  Build a "seen" set by marking cache slots, then destroy
    // any cached ncplane not referenced by the current composite list.
    // Per-plane cache with ncplane reuse.  On cache miss, re-blit into
    // the existing ncplane (resize if needed) instead of destroying and
    // recreating it.  This avoids kitty pixel protocol artifacts from
    // mixing fresh and cached sprixel ncplanes in the same frame.
    bool seen[NC_PLANE_CACHE_SIZE] = {0};

    ensure_entry_plane_slots(ctx, (unsigned)count);
    unsigned int used = 0;

    // If any entry would miss cache this frame, rebuild all entries together.
    // Mixing stale cached sprixels with freshly blitted sprixels has caused
    // persistent vertical drift/artifacts on some terminals.
    bool force_rebuild = false;
    for (n00b_isize_t i = 0; i < count; i++) {
        const n00b_composite_entry_t *entry = &entries[i];
        n00b_plane_t *plane = entry->plane;

        if (!(plane->flags & N00B_PLANE_VISIBLE)) {
            continue;
        }

        int cache_slot = -1;
        for (unsigned int slot = 0; slot < NC_PLANE_CACHE_SIZE; slot++) {
            if (ctx->plane_cache[slot].plane == plane) {
                cache_slot = (int)slot;
                break;
            }
        }

        n00b_entry_info_t hit_info;
        n00b_composite_entry_info(entry, &hit_info, pixel_w, pixel_h);
        int expected_pxoff_y = hit_info.outer_y % pixel_h;
        int expected_pxoff_x = hit_info.outer_x % pixel_w;
        if (expected_pxoff_y < 0) expected_pxoff_y += pixel_h;
        if (expected_pxoff_x < 0) expected_pxoff_x += pixel_w;
        int expected_cell_y = (hit_info.outer_y - expected_pxoff_y) / pixel_h;
        int expected_cell_x = (hit_info.outer_x - expected_pxoff_x) / pixel_w;
        int expected_cell_h = ((int)hit_info.outer_rows + expected_pxoff_y + pixel_h - 1) / pixel_h;
        int expected_cell_w = ((int)hit_info.outer_cols + expected_pxoff_x + pixel_w - 1) / pixel_w;
        if (expected_cell_h < 1) expected_cell_h = 1;
        if (expected_cell_w < 1) expected_cell_w = 1;

        bool cache_hit = cache_slot >= 0
            && ctx->plane_cache[cache_slot].ncp != nullptr
            && ctx->plane_cache[cache_slot].render_gen == plane->render_gen
            && ctx->plane_cache[cache_slot].abs_x == entry->abs_x
            && ctx->plane_cache[cache_slot].abs_y == entry->abs_y
            && ctx->plane_cache[cache_slot].cell_x == expected_cell_x
            && ctx->plane_cache[cache_slot].cell_y == expected_cell_y
            && ctx->plane_cache[cache_slot].cell_w == expected_cell_w
            && ctx->plane_cache[cache_slot].cell_h == expected_cell_h
            && ctx->plane_cache[cache_slot].pxoff_x == expected_pxoff_x
            && ctx->plane_cache[cache_slot].pxoff_y == expected_pxoff_y
            && ctx->plane_cache[cache_slot].width == plane->width
            && ctx->plane_cache[cache_slot].height == plane->height
            && ctx->plane_cache[cache_slot].scroll_x == plane->scroll_x
            && ctx->plane_cache[cache_slot].scroll_y == plane->scroll_y
            && ctx->plane_cache[cache_slot].box == plane->box
            && ctx->plane_cache[cache_slot].state == plane->widget_state;

        if (!cache_hit) {
            force_rebuild = true;
            break;
        }
    }

    if (force_rebuild) {
        destroy_entry_planes(ctx);
    }

    for (n00b_isize_t i = 0; i < count; i++) {
        const n00b_composite_entry_t *entry = &entries[i];
        n00b_plane_t *plane = entry->plane;

        if (!(plane->flags & N00B_PLANE_VISIBLE)) {
            continue;
        }

        // Look up in plane cache. Use full-table scan for robustness:
        // the previous limited-probe hash path could miss insert/find and
        // leave stale ncplanes alive.
        int cache_slot = -1;
        int free_slot = -1;

        // Compute expected ncplane cell-space bounds for this entry.
        n00b_entry_info_t hit_info;
        n00b_composite_entry_info(entry, &hit_info, pixel_w, pixel_h);
        int expected_pxoff_y = hit_info.outer_y % pixel_h;
        int expected_pxoff_x = hit_info.outer_x % pixel_w;
        if (expected_pxoff_y < 0) expected_pxoff_y += pixel_h;
        if (expected_pxoff_x < 0) expected_pxoff_x += pixel_w;

        int expected_cell_y = (hit_info.outer_y - expected_pxoff_y) / pixel_h;
        int expected_cell_x = (hit_info.outer_x - expected_pxoff_x) / pixel_w;
        int expected_cell_h = ((int)hit_info.outer_rows + expected_pxoff_y + pixel_h - 1) / pixel_h;
        int expected_cell_w = ((int)hit_info.outer_cols + expected_pxoff_x + pixel_w - 1) / pixel_w;
        if (expected_cell_h < 1) expected_cell_h = 1;
        if (expected_cell_w < 1) expected_cell_w = 1;

        for (unsigned int slot = 0; slot < NC_PLANE_CACHE_SIZE; slot++) {
            if (ctx->plane_cache[slot].plane == plane) {
                cache_slot = (int)slot;
                seen[slot] = true;
                break;
            }
            if (free_slot < 0 && ctx->plane_cache[slot].plane == nullptr) {
                free_slot = (int)slot;
            }
        }

        // Cache hit — content and position unchanged.
        if (cache_slot >= 0
            && ctx->plane_cache[cache_slot].ncp != nullptr
            && ctx->plane_cache[cache_slot].render_gen == plane->render_gen
            && ctx->plane_cache[cache_slot].abs_x == entry->abs_x
            && ctx->plane_cache[cache_slot].abs_y == entry->abs_y
            && ctx->plane_cache[cache_slot].cell_x == expected_cell_x
            && ctx->plane_cache[cache_slot].cell_y == expected_cell_y
            && ctx->plane_cache[cache_slot].cell_w == expected_cell_w
            && ctx->plane_cache[cache_slot].cell_h == expected_cell_h
            && ctx->plane_cache[cache_slot].pxoff_x == expected_pxoff_x
            && ctx->plane_cache[cache_slot].pxoff_y == expected_pxoff_y
            && ctx->plane_cache[cache_slot].width == plane->width
            && ctx->plane_cache[cache_slot].height == plane->height
            && ctx->plane_cache[cache_slot].scroll_x == plane->scroll_x
            && ctx->plane_cache[cache_slot].scroll_y == plane->scroll_y
            && ctx->plane_cache[cache_slot].box == plane->box
            && ctx->plane_cache[cache_slot].state == plane->widget_state) {
            ctx->entry_planes[used++] = ctx->plane_cache[cache_slot].ncp;
            continue;
        }

        // Stale or new: get the existing ncplane for reuse (if any).
        struct ncplane *reuse_ncp = nullptr;
        if (cache_slot >= 0) {
            reuse_ncp = ctx->plane_cache[cache_slot].ncp;
            ctx->plane_cache[cache_slot].ncp = nullptr;
        }

        // Render the RGBA buffer (glyph cache makes this fast).
        struct ncplane *ncp = nc_render_entry(ctx, entry,
                                               default_style, &pm,
                                               reuse_ncp);
        if (ncp) {
            ctx->entry_planes[used++] = ncp;

            // Update cache.
            if (cache_slot >= 0) {
                ctx->plane_cache[cache_slot].ncp        = ncp;
                ctx->plane_cache[cache_slot].render_gen  = plane->render_gen;
                ctx->plane_cache[cache_slot].abs_x       = entry->abs_x;
                ctx->plane_cache[cache_slot].abs_y       = entry->abs_y;
                ctx->plane_cache[cache_slot].cell_x      = expected_cell_x;
                ctx->plane_cache[cache_slot].cell_y      = expected_cell_y;
                ctx->plane_cache[cache_slot].cell_w      = expected_cell_w;
                ctx->plane_cache[cache_slot].cell_h      = expected_cell_h;
                ctx->plane_cache[cache_slot].pxoff_x     = expected_pxoff_x;
                ctx->plane_cache[cache_slot].pxoff_y     = expected_pxoff_y;
                ctx->plane_cache[cache_slot].width       = plane->width;
                ctx->plane_cache[cache_slot].height      = plane->height;
                ctx->plane_cache[cache_slot].scroll_x    = plane->scroll_x;
                ctx->plane_cache[cache_slot].scroll_y    = plane->scroll_y;
                ctx->plane_cache[cache_slot].box         = plane->box;
                ctx->plane_cache[cache_slot].state       = plane->widget_state;
            }
            else {
                int insert_slot = free_slot;
                if (insert_slot < 0) {
                    // Cache saturated: evict an unseen slot (not referenced
                    // by this frame so far). Fallback to slot 0 if needed.
                    for (unsigned int slot = 0; slot < NC_PLANE_CACHE_SIZE; slot++) {
                        if (!seen[slot]) {
                            insert_slot = (int)slot;
                            break;
                        }
                    }
                    if (insert_slot < 0) {
                        insert_slot = 0;
                    }
                    if (ctx->plane_cache[insert_slot].ncp) {
                        ncplane_family_destroy(ctx->plane_cache[insert_slot].ncp);
                    }
                    ctx->plane_cache[insert_slot].plane = nullptr;
                    ctx->plane_cache[insert_slot].ncp   = nullptr;
                }

                ctx->plane_cache[insert_slot].plane      = plane;
                ctx->plane_cache[insert_slot].ncp         = ncp;
                ctx->plane_cache[insert_slot].render_gen  = plane->render_gen;
                ctx->plane_cache[insert_slot].abs_x       = entry->abs_x;
                ctx->plane_cache[insert_slot].abs_y       = entry->abs_y;
                ctx->plane_cache[insert_slot].cell_x      = expected_cell_x;
                ctx->plane_cache[insert_slot].cell_y      = expected_cell_y;
                ctx->plane_cache[insert_slot].cell_w      = expected_cell_w;
                ctx->plane_cache[insert_slot].cell_h      = expected_cell_h;
                ctx->plane_cache[insert_slot].pxoff_x     = expected_pxoff_x;
                ctx->plane_cache[insert_slot].pxoff_y     = expected_pxoff_y;
                ctx->plane_cache[insert_slot].width       = plane->width;
                ctx->plane_cache[insert_slot].height      = plane->height;
                ctx->plane_cache[insert_slot].scroll_x    = plane->scroll_x;
                ctx->plane_cache[insert_slot].scroll_y    = plane->scroll_y;
                ctx->plane_cache[insert_slot].box         = plane->box;
                ctx->plane_cache[insert_slot].state       = plane->widget_state;
                seen[insert_slot] = true;
            }
        }
        else if (reuse_ncp) {
            // Render failed — destroy the old ncplane we pulled out.
            ncplane_family_destroy(reuse_ncp);
        }
    }
    ctx->entry_planes_used = used;

    // Destroy cached ncplanes for planes that are no longer in the frame.
    for (unsigned int s = 0; s < NC_PLANE_CACHE_SIZE; s++) {
        if (ctx->plane_cache[s].plane && !seen[s]) {
            if (ctx->plane_cache[s].ncp) {
                ncplane_family_destroy(ctx->plane_cache[s].ncp);
            }
            ctx->plane_cache[s].plane = nullptr;
            ctx->plane_cache[s].ncp   = nullptr;
        }
    }

    // Reassert z-order every frame. Cache hits keep existing ncplanes in
    // whatever stack position they previously had, while rerendered planes
    // are recreated at the top; this can cause focus/interaction jitter when
    // adjacent planes overlap after cell rounding. Moving every live plane to
    // top in composite order yields a stable bottom->top stack.
    for (unsigned int i = 0; i < used; i++) {
        struct ncplane *ncp = ctx->entry_planes[i];
        if (ncp) {
            ncplane_move_family_top(ncp);
        }
    }
#endif
}

// -------------------------------------------------------------------
// Font metrics (FreeType-based)
// -------------------------------------------------------------------

#if N00B_HAVE_FREETYPE

// Measure the pixel width of a UTF-8 string using FreeType glyph advances.
static int32_t
nc_fm_text_width(void *vctx, n00b_string_t *text, n00b_text_style_t *style)
{
    nc_ctx_t *ctx = vctx;

    if (!text || text->u8_bytes == 0) {
        return 0;
    }

    uint8_t hint = style ? (uint8_t)style->font_hint : N00B_FONT_DEFAULT;
    int16_t font_size = style ? style->font_size : 0;

    FT_Face face = resolve_style_face(ctx, hint);
    if (!face) {
        return 0;
    }

    int target_h = style_pixel_height(ctx, font_size);
    (void)fit_face_pixel_size(face, target_h, nullptr, nullptr, nullptr);

    int total_width = 0;
    const char *p = text->data;
    const char *end = text->data + text->u8_bytes;

    while (p < end && *p) {
        uint32_t cp = utf8_decode(&p);
        if (cp == 0) {
            break;
        }

        FT_UInt glyph_idx = FT_Get_Char_Index(face, cp);
        if (FT_Load_Glyph(face, glyph_idx, FT_LOAD_DEFAULT) != 0) {
            continue;
        }

        total_width += (int)(face->glyph->advance.x >> 6);
    }

    return (int32_t)total_width;
}

static int32_t
nc_fm_line_height(void *vctx, n00b_text_style_t *style)
{
    nc_ctx_t *ctx = vctx;
    uint8_t hint = style ? (uint8_t)style->font_hint : N00B_FONT_DEFAULT;
    int16_t font_size = style ? style->font_size : 0;

    FT_Face face = resolve_style_face(ctx, hint);
    int target_h = style_pixel_height(ctx, font_size);
    int line_h = target_h;

    (void)fit_face_pixel_size(face, target_h, nullptr, nullptr, &line_h);

    return (int32_t)line_h;
}

static int32_t
nc_fm_ascent(void *vctx, n00b_text_style_t *style)
{
    nc_ctx_t *ctx = vctx;
    uint8_t hint = style ? (uint8_t)style->font_hint : N00B_FONT_DEFAULT;
    int16_t font_size = style ? style->font_size : 0;

    FT_Face face = resolve_style_face(ctx, hint);
    if (!face) {
        return (int32_t)ctx->cell_pixel_h;
    }

    int target_h = style_pixel_height(ctx, font_size);
    int ascent = target_h;
    (void)fit_face_pixel_size(face, target_h, &ascent, nullptr, nullptr);

    return (int32_t)ascent;
}

static n00b_font_metrics_provider_t
nc_get_font_metrics(void *vctx)
{
    return (n00b_font_metrics_provider_t){
        .text_width  = nc_fm_text_width,
        .line_height = nc_fm_line_height,
        .ascent      = nc_fm_ascent,
        .ctx         = vctx,
    };
}

#endif // N00B_HAVE_FREETYPE

const n00b_renderer_vtable_t n00b_renderer_notcurses = {
    .name               = "notcurses",
    .version            = N00B_RENDERER_ABI_VERSION,
    .init               = nc_init,
    .destroy            = nc_destroy,
    .capabilities       = nc_capabilities,
    .get_size           = nc_get_size,
    .render_frame       = nc_render_frame,
    .flush              = nc_flush,
    .render_planes      = nc_render_planes,
    .cursor_set_visible = nc_cursor_set_visible,
    .cursor_move        = nc_cursor_move,
    .alt_screen_enter   = nc_alt_screen_enter,
    .alt_screen_leave   = nc_alt_screen_leave,
    .on_resize          = nc_on_resize,
    .prepare_gui        = nc_prepare_gui,
    .poll_event         = nc_poll_event,
#if N00B_HAVE_FREETYPE
    .get_font_metrics   = nc_get_font_metrics,
#endif
};

// ====================================================================
// Public query functions
// ====================================================================

bool
n00b_notcurses_has_pixel_support(void *vctx)
{
    nc_ctx_t *ctx = vctx;
    return ctx ? ctx->has_pixel : false;
}

bool
n00b_notcurses_has_freetype(void *vctx)
{
#if N00B_HAVE_FREETYPE
    nc_ctx_t *ctx = vctx;
    return ctx ? ctx->has_freetype : false;
#else
    (void)vctx;
    return false;
#endif
}
