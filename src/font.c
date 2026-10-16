/*
 * SADES Engine — a reimplementation
 * Copyright (c) 2026 Kalizkal
 * MIT License — see LICENSE
 */

#include "font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H

/* --------------------------------------------------------------------- */
/* Module state                                                          */
/* --------------------------------------------------------------------- */

static FT_Library g_lib  = NULL;
static FT_Face    g_face = NULL;
static int        g_px   = 16;

/* Number of faces to probe in a .ttc collection before falling back to
 * treating the file as a single-face font. */
#define FONT_MAX_FACES 8

/*
 * Candidate paths, tried in order when neither the `path` argument nor
 * $SADES_FONT is usable. System fonts come first; the project-local
 * `fonts/` directory is the last resort so bundled fonts don't shadow
 * a system one.
 */
static const char *kFontCandidates[] = {
    /* Linux / *BSD distro packages */
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/TTF/NotoSansCJK-Regular.ttc",
    /* Android */
    "/system/fonts/NotoSansCJK-Regular.ttc",
    "/system/fonts/NotoSansCJKjp-Regular.otf",
    /* macOS (Hiragino Kaku Gothic W3, UTF-8 path) */
    "/System/Library/Fonts/\xe3\x83\x92\xe3\x83\xa9\xe3\x82\xae\xe3\x83\x8e\xe8\xa7\x92\xe3\x82\xb4\xe3\x82\xb7\xe3\x83\x83\xe3\x82\xaf W3.ttc",
    "/Library/Fonts/NotoSansCJK-Regular.ttc",
    /* Windows */
    "C:/Windows/Fonts/msgothic.ttc",
    "C:/Windows/Fonts/meiryo.ttc",
    /* Project-local (bundled) */
    "fonts/NotoSansCJK-Regular.ttc",
    "fonts/SourceHanSans-Regular.otf",
    "fonts/mplus-1m-regular.ttf",
    NULL
};

/* --------------------------------------------------------------------- */
/* Helpers                                                               */
/* --------------------------------------------------------------------- */

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* Returns a pointer that outlives the call: either into the env var
 * (static storage) or into kFontCandidates. NULL if nothing suitable
 * was found. */
static const char *resolve_font_path(void) {
    const char *env = getenv("SADES_FONT");
    if (env && env[0]) {
        if (file_exists(env)) return env;
        fprintf(stderr, "[font] SADES_FONT set but file missing: %s\n", env);
    }
    for (int i = 0; kFontCandidates[i]; i++) {
        if (file_exists(kFontCandidates[i])) return kFontCandidates[i];
    }
    return NULL;
}

/* Decode one UTF-8 codepoint. Returns bytes consumed (1..4), or 0 on
 * NUL. Invalid bytes consume 1 so the outer loop can't spin. */
static int utf8_next(const char *s, uint32_t *cp) {
    const unsigned char c = (unsigned char)s[0];
    if (!c) return 0;
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0) {
        *cp = ((c & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0) {
        *cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0) {
        *cp = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12)
            | ((s[2] & 0x3F) << 6)  |  (s[3] & 0x3F);
        return 4;
    }
    return 1; /* invalid byte, consume it to avoid an infinite loop */
}

/*
 * Draw a downward-pointing white triangle (U+25BD) by hand.
 *
 * Noto Sans CJK doesn't have this glyph, and the SADES scripts use it
 * as a "wait for input" marker, so it needs to render. Shape: 9 rows,
 * tapering from 13 px at the top to a single pixel. Fits in one
 * 16-px cell.
 */
static void draw_triangle_down(uint8_t *fb, int fw, int fh,
                               int x, int y, int cell_px,
                               uint8_t r, uint8_t g, uint8_t b) {
    const int cx   = x + cell_px / 2;
    const int y0   = y + 4;
    const int y1   = y + 12;
    const int span = y1 - y0;
    const int half_top = 6;

    for (int row = 0; row <= span; row++) {
        const int fy = y0 + row;
        if (fy < 0 || fy >= fh) continue;
        int half = half_top - (row * half_top) / span;
        if (half < 0) half = 0;
        for (int bx = cx - half; bx <= cx + half; bx++) {
            if (bx < 0 || bx >= fw) continue;
            uint8_t *dst = &fb[(fy * fw + bx) * 3];
            dst[0] = r;
            dst[1] = g;
            dst[2] = b;
        }
    }
}

/* --------------------------------------------------------------------- */
/* Public API                                                            */
/* --------------------------------------------------------------------- */

int font_init(const char *path, int pixel_size) {
    if (g_face) {
        fprintf(stderr, "[font] already initialized, ignoring call\n");
        return 0;
    }

    if (FT_Init_FreeType(&g_lib) != 0) {
        fprintf(stderr, "[font] FT_Init_FreeType failed\n");
        g_lib = NULL;
        return -1;
    }

    const char *resolved = (path && path[0]) ? path : resolve_font_path();
    if (!resolved) {
        fprintf(stderr,
            "[font] no suitable Japanese font found.\n"
            "       Set SADES_FONT=/path/to/font or place one in ./fonts/\n");
        FT_Done_FreeType(g_lib);
        g_lib = NULL;
        return -1;
    }

    /* .ttc files are collections. Probe a few face indices, then fall
     * back to treating the file as a single-face font. */
    FT_Error err = -1;
    int face_index = 0;
    for (int i = 0; i < FONT_MAX_FACES; i++) {
        err = FT_New_Face(g_lib, resolved, i, &g_face);
        if (err == 0) { face_index = i; break; }
    }
    if (err != 0) {
        /* Plain single-face font (.ttf, .otf). */
        err = FT_New_Face(g_lib, resolved, 0, &g_face);
        if (err != 0) {
            fprintf(stderr, "[font] FT_New_Face(%s): error %d\n",
                    resolved, err);
            FT_Done_FreeType(g_lib);
            g_lib = NULL;
            return -1;
        }
    }

    g_px = pixel_size;
    FT_Set_Pixel_Sizes(g_face, 0, g_px);
    fprintf(stderr, "[font] loaded '%s' (face=%d, height=%dpx)\n",
            resolved, face_index, g_px);
    return 0;
}

int font_draw(uint8_t *fb, int fw, int fh,
              int x, int y, const char *utf8,
              uint8_t r, uint8_t g, uint8_t b) {
    if (!g_face) return 0;

    const int start_x = x;
    const char *p = utf8;

    while (*p) {
        uint32_t cp;
        int n = utf8_next(p, &cp);
        if (n == 0) break;
        p += n;

        if (cp == ' ' || cp == '\t') {
            x += (cp == '\t') ? g_px : (g_px / 2);
            continue;
        }

        if (cp == 0x25BD) { /* ▽ */
            draw_triangle_down(fb, fw, fh, x, y, g_px, r, g, b);
            x += g_px;
            continue;
        }

        if (FT_Load_Char(g_face, cp, FT_LOAD_RENDER) != 0) {
            /* Missing glyph. Skip it but still advance, so a stray
             * codepoint doesn't collapse the rest of the line. */
            x += g_px;
            continue;
        }

        FT_GlyphSlot slot = g_face->glyph;

        /* Use the font's own metric when meaningful; otherwise assume
         * half a cell for ASCII, a full cell for CJK. */
        int advance = slot->advance.x >> 6;
        if (advance < 4) advance = g_px / 2;

        const int x_off = slot->bitmap_left;
        const int y_off = g_px - slot->bitmap_top - 1;

        for (int by = 0; by < (int)slot->bitmap.rows; by++) {
            const int fy = y + y_off + by;
            if (fy < 0 || fy >= fh) continue;
            for (int bx = 0; bx < (int)slot->bitmap.width; bx++) {
                const int fx = x + x_off + bx;
                if (fx < 0 || fx >= fw) continue;
                const unsigned char a =
                    slot->bitmap.buffer[by * slot->bitmap.pitch + bx];
                if (a == 0) continue;
                uint8_t *dst = &fb[(fy * fw + fx) * 3];
                dst[0] = (uint8_t)((r * a + dst[0] * (255 - a)) / 255);
                dst[1] = (uint8_t)((g * a + dst[1] * (255 - a)) / 255);
                dst[2] = (uint8_t)((b * a + dst[2] * (255 - a)) / 255);
            }
        }
        x += advance;
    }
    return x - start_x;
}

void font_quit(void) {
    if (g_face) { FT_Done_Face(g_face);  g_face = NULL; }
    if (g_lib)  { FT_Done_FreeType(g_lib); g_lib = NULL; }
}