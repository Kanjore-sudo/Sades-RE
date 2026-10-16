/*
 * SADES Engine — a reimplementation
 * Copyright (c) 2026 Kalizkal
 * MIT License — see LICENSE
 */

#ifndef SADES_GLZ_H
#define SADES_GLZ_H

#include <stdint.h>
#include <stddef.h>

/* Decoder for the SADES .GLZ and .XSC graphic formats.
 *
 * Both formats share the same LZ77-based compression and 4-plane
 * bitplane layout; they differ only in the header:
 *
 *   .GLZ header (0x10 bytes):
 *     0x00  6-byte magic: FF FF FF FF FF FF
 *     0x06  pad_y (uint16 LE)   -- see below
 *     0x08  pad_x (uint16 LE)   -- see below
 *     0x0A  bpr   (uint16 LE)   -- bytes per row, /8
 *     0x0C  rows  (uint16 LE)   -- height in pixels
 *     0x0E  pal_flag (uint16 LE, 0 = no palette, else 16-color palette)
 *     0x10  48-byte BGR-nibble palette (only if pal_flag != 0)
 *
 *   .XSC header (0x0C bytes):
 *     0x00  2-byte version: 03 03 or 01 03
 *     0x02  2-byte constant: 00 00
 *     0x04  bpr   (uint16 LE)
 *     0x06  rows  (uint16 LE)
 *     0x08  compressed stream begins here
 *     .XSC carries no palette; it always uses the active palette
 *     (inherited from the last .GLZ loaded, or from a .PAL file).
 *
 * pad_x / pad_y semantics (validated empirically against Erion Quest):
 *   - The "pad" fields describe where the sprite anchor sits inside
 *     the bitmap, not a screen offset.
 *   - The VM's GUARD opcode passes (arg_x, arg_y) = (0xFFFF, 0xFFFF)
 *     for centered portraits, causing the engine to use pad_x as Y and
 *     pad_y*8 as X.
 *   - A value of 0xFFFF in either field means "not supplied".
 *
 * The decoder produces an RGB24 buffer of size (bpr*8) * rows * 3.
 * Palette resolution order for pal_flag == 0:
 *     1. glz_set_external_palette() (if called)
 *     2. active palette inherited from a previous glz_load() with pal_flag != 0
 *     3. grayscale fallback (16-step ramp)
 */

typedef struct {
    uint16_t bpr;               /* bytes per row (from header) */
    uint16_t rows;              /* height in pixels */
    uint16_t pal_flag;          /* non-zero if palette was embedded */
    uint16_t pad_x, pad_y;      /* anchor metadata */
    uint8_t  palette[16][3];    /* resolved RGB888 palette (always valid) */
    uint8_t *rgb;               /* width*height*3, caller owns via glz_free */
    uint8_t *indices;           /* width*height, values 0..15 */
    int      width;             /* bpr * 8 */
    int      height;            /* == rows */
} GLZ;

/* Load and decode a .GLZ or .XSC file.
 * Returns 0 on success, -1 on error. On success, `g->rgb` must be freed
 * via glz_free(). On error, `g` is left in a consistent empty state. */
int  glz_load(const char *path, GLZ *g);

/* Release the RGB buffer. Safe to call multiple times. */
void glz_free(GLZ *g);

/* Provide an external 48-byte BGR-nibble palette. Subsequent glz_load()
 * calls will use it for files with pal_flag == 0 (unless an active
 * palette is already in effect). */
void glz_set_external_palette(const uint8_t raw48[48]);

/* Provide an external 64-byte RGB32 palette (as used by SADES v1.1
 * / Yukonis .PAL files): 16 entries of (R, G, B, 0) with literal
 * 0..255 values. Byte 3 of each entry is ignored. */
void glz_set_external_palette_rgb32(const uint8_t raw64[64]);

/* Write an RGB24 framebuffer to a PNG file.
 * Returns 0 on success, -1 on error. */
int  write_png(const char *path, int w, int h, const uint8_t *rgb);

#endif /* SADES_GLZ_H */