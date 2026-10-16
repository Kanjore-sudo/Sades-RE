/*
 * SADES Engine — a reimplementation
 * Copyright (c) 2026 Kalizkal
 * MIT License — see LICENSE
 */

#ifndef SADES_FONT_H
#define SADES_FONT_H

#include <stdint.h>

/* Initialize FreeType and load a font face.
 *
 * Font resolution strategy (in order):
 *   1. If `path` is non-NULL and non-empty, use it directly.
 *   2. $SADES_FONT environment variable.
 *   3. ./fonts/ relative to the current working directory (bundled font).
 *   4. A list of common system paths: Android, Linux, macOS, Windows.
 *
 * `pixel_size` is the nominal character height in pixels. SADES uses 16,
 * which matches the PC-98 8x16 text cell (Japanese glyphs occupy 2 cells).
 *
 * Returns 0 on success, -1 on error. On error, no FreeType state is left
 * allocated; calling font_quit() afterwards is safe. */
int  font_init(const char *path, int pixel_size);

/* Draw a UTF-8 string at pixel position (x, y) on an RGB24 framebuffer
 * of size (fw x fh). Foreground color is (r, g, b).
 *
 * Layout rules:
 *   - Space advances by half a cell (cell = pixel_size).
 *   - Tab advances by one cell.
 *   - U+25BD (▽, "inverted white triangle") is not present in Noto Sans
 *     CJK. We draw it manually; it is used as a "wait for input" marker.
 *
 * Returns the total horizontal advance in pixels. */
int  font_draw(uint8_t *fb, int fw, int fh,
               int x, int y, const char *utf8,
               uint8_t r, uint8_t g, uint8_t b);

/* Release FreeType resources. Safe to call multiple times. */
void font_quit(void);

#endif /* SADES_FONT_H */