/*
 * SADES Engine — a reimplementation
 * Copyright (c) 2026 Kalizkal
 * MIT License — see LICENSE
 */

#include "glz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------- */
/* Palette state (module-level)                                          */
/* --------------------------------------------------------------------- */

static uint8_t g_external_palette[16][3];
static uint8_t g_active_palette[16][3];
static int     g_active_pal_valid = 0;
static int     g_use_external     = 0;

void glz_set_external_palette(const uint8_t raw48[48]) {
    /* B, R, G nibbles, high nibble of each byte. Scaled to 0..255
     * by multiplying the nibble by 17. */
    for (int i = 0; i < 16; i++) {
        uint8_t b = (raw48[i * 3 + 0] & 0x0F) * 17;
        uint8_t r = (raw48[i * 3 + 1] & 0x0F) * 17;
        uint8_t g = (raw48[i * 3 + 2] & 0x0F) * 17;
        g_external_palette[i][0] = r;
        g_external_palette[i][1] = g;
        g_external_palette[i][2] = b;
    }
    g_use_external = 1;
}

void glz_set_external_palette_rgb32(const uint8_t raw64[64]) {
    /* 16 entries of (R, G, B, padding). Values are literal 0..255. */
    for (int i = 0; i < 16; i++) {
        g_external_palette[i][0] = raw64[i * 4 + 0];
        g_external_palette[i][1] = raw64[i * 4 + 1];
        g_external_palette[i][2] = raw64[i * 4 + 2];
    }
    g_use_external = 1;
}

/* --------------------------------------------------------------------- */
/* Header parsing                                                        */
/* --------------------------------------------------------------------- */

/* Container format detected from the first bytes of the file. */
typedef enum {
    FMT_GLZ,      /* 6-byte FF FF FF FF FF FF magic, optional palette */
    FMT_XSC,      /* 4-byte header 03 03 03 03 / 01 03 03 03, no palette */
    FMT_INVALID,  /* unknown format */
} glz_format_t;

static glz_format_t detect_format(const uint8_t *data, long sz) {
    if (sz >= 6 && data[0] == 0xFF && data[1] == 0xFF && data[2] == 0xFF
     && data[3] == 0xFF && data[4] == 0xFF && data[5] == 0xFF) {
        return FMT_GLZ;
    }
    /* .XSC doesn't have a fixed magic. Validate structurally: the
     * first four bytes are codec IDs in 1..4, and the sum of the four
     * plane sizes at 0x0A must equal filesize - 19 (18-byte header
     * plus the EOF byte). Holds for all 56 sprites in the sample. */
    if (sz >= 0x13 &&
        data[0] >= 1 && data[0] <= 4 && data[1] >= 1 && data[1] <= 4 &&
        data[2] >= 1 && data[2] <= 4 && data[3] >= 1 && data[3] <= 4) {
        uint32_t sum = 0;
        for (int i = 0; i < 4; i++)
            sum += (uint32_t)(data[0x0A + i*2] | (data[0x0B + i*2] << 8));
        if (sum + 19 == (uint32_t)sz) return FMT_XSC;
    }
    return FMT_INVALID;
}

/* --------------------------------------------------------------------- */
/* LZ77 decode table                                                     */
/* --------------------------------------------------------------------- */

/* Precomputed back-reference distance table for the LZ77 decoder.
 * The distances depend on the bitmap height, which is why the table
 * is rebuilt for every file. */
static void build_table(uint16_t rows, uint16_t tbl[256]) {
    int cap = rows < 0xC7 ? rows : 0xC7;
    for (int i = 0; i < cap; i++) tbl[i] = i + 1;
    int idx = cap;
    int ax  = rows;
    int dx  = cap + 1;
    while (idx < 256) {
        if (ax >= 0x3F00) {
            while (idx < 256) tbl[idx++] = dx++;
            break;
        }
        tbl[idx++] = ax;
        ax = (ax + rows) & 0xFFFF;
    }
}

/* --------------------------------------------------------------------- */
/* Main decoder                                                          */
/* --------------------------------------------------------------------- */


/*
 * Two counter readers, one per codec family.
 *
 * Codec 1 and 2 use a single 8-bit counter. A stored value of 0
 * means 256, because the decoder decrements before testing.
 *
 * Codec 3 and 4 use a big-endian 16-bit counter with bit 7 of the
 * first byte as a flag. A stored value of 0 means 65536.
 */
static size_t rle_read_counter_1b(const uint8_t *stream, size_t slen, size_t *si)
{
    if (*si >= slen) return 1;
    uint8_t b = stream[(*si)++];
    return b ? (size_t)b : 256u;
}

static size_t rle_read_counter_2b(const uint8_t *stream, size_t slen, size_t *si)
{
    if (*si >= slen) return 1;
    uint8_t b1 = stream[(*si)++];
    if (b1 & 0x80) {
        if (*si >= slen) return 1;
        uint8_t b2 = stream[(*si)++];
        size_t v = ((size_t)(b1 & 0x7F) << 8) | (size_t)b2;
        return v ? v : 65536u;
    }
    return b1 ? (size_t)b1 : 256u;
}

/*
 * .XSC planes use byte-level RLE, not LZ77.
 *
 * Stream layout: a one-byte marker (0x01 or 0x03) that must be
 * skipped, then pairs (DH, DL). If DH != DL, DH is a literal byte
 * and the reader shifts DH := DL and reads a new DL. If DH == DL,
 * the reader consumes a run counter and emits DH that many times.
 */
static size_t decode_rle_xsc(const uint8_t *stream, size_t slen,
                              uint8_t *out, size_t out_size,
                              int one_byte)
{
    if (slen < 3) return 0;
    size_t si = 1;
    uint8_t DH = stream[si++];
    uint8_t DL = stream[si++];
    size_t counter = (DH == DL)
        ? (one_byte ? rle_read_counter_1b(stream, slen, &si)
                    : rle_read_counter_2b(stream, slen, &si))
        : 1;
    size_t written = 0;
    while (written < out_size) {
        out[written++] = DH;
        if (DH == DL) {
            counter--;
            if (counter == 0) {
                if (si + 2 > slen) break;
                DH = stream[si++];
                DL = stream[si++];
                counter = (DH == DL)
                    ? (one_byte ? rle_read_counter_1b(stream, slen, &si)
                                : rle_read_counter_2b(stream, slen, &si))
                    : 1;
            }
        } else {
            DH = DL;
            if (si >= slen) break;
            DL = stream[si++];
            counter = (DH == DL)
                ? (one_byte ? rle_read_counter_1b(stream, slen, &si)
                            : rle_read_counter_2b(stream, slen, &si))
                : 1;
        }
    }
    return written;
}

int glz_load(const char *path, GLZ *g) {
    memset(g, 0, sizeof(*g));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0x10) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }

    uint8_t *data = malloc((size_t)sz);
    if (!data) { fclose(f); return -1; }
    if (fread(data, 1, (size_t)sz, f) != (size_t)sz) {
        free(data); fclose(f); return -1;
    }
    fclose(f);

    glz_format_t fmt = detect_format(data, sz);
    size_t pos;

    if (fmt == FMT_GLZ) {
        if (sz < 0x10) { free(data); return -1; }
        g->pad_y    = data[0x06] | (data[0x07] << 8);
        g->pad_x    = data[0x08] | (data[0x09] << 8);
        g->bpr      = data[0x0A] | (data[0x0B] << 8);
        g->rows     = data[0x0C] | (data[0x0D] << 8);
        g->pal_flag = data[0x0E] | (data[0x0F] << 8);
        pos = 0x10;

        if (g->pal_flag) {
            if (sz < (long)(pos + 48)) { free(data); return -1; }
            for (int i = 0; i < 16; i++) {
                /* B, R, G nibble-high. */
                uint8_t b = (data[pos + i*3 + 0] & 0x0F) * 17;
                uint8_t r = (data[pos + i*3 + 1] & 0x0F) * 17;
                uint8_t gg = (data[pos + i*3 + 2] & 0x0F) * 17;
                g->palette[i][0] = r;
                g->palette[i][1] = gg;
                g->palette[i][2] = b;
            }
            pos += 48;
            memcpy(g_active_palette, g->palette, sizeof(g_active_palette));
            g_active_pal_valid = 1;
            fprintf(stderr, "[glz] %s: embedded palette (active updated)\n", path);
        }
    } else { /* FMT_XSC */
        /*
         * pad at 0x04 is a raw VRAM byte offset. On a 640x400 screen
         * each plane row is 80 bytes, so the sprite sits at:
         *     y = pad / 80
         *     x = (pad % 80) * 8
         * There is no clamp: large values (like 22727) are valid and
         * produce sprites near the bottom of the screen.
         */
        if (sz < 0x13) { free(data); return -1; }
        g->pad_y    = data[0x04] | (data[0x05] << 8);
        g->bpr      = data[0x06] | (data[0x07] << 8);
        g->rows     = data[0x08] | (data[0x09] << 8);
        fprintf(stderr, "[glz] %s: XSC pad=%u bpr=%u rows=%u\n",
                path, g->pad_y, g->bpr, g->rows);
        g->pal_flag = 0;
        g->pad_x    = 0;
        pos = 0x12;  /* skip header + size table, start at plane streams */
    }

    /* Resolve palette for pal_flag == 0. */
    if (g->pal_flag == 0) {
        if (g_use_external) {
            memcpy(g->palette, g_external_palette, sizeof(g_external_palette));
            fprintf(stderr, "[glz] %s: external palette\n", path);
        } else if (g_active_pal_valid) {
            memcpy(g->palette, g_active_palette, sizeof(g_active_palette));
            fprintf(stderr, "[glz] %s: inherited active palette\n", path);
        } else {
            for (int i = 0; i < 16; i++) {
                uint8_t v = i * 17;
                g->palette[i][0] = v;
                g->palette[i][1] = v;
                g->palette[i][2] = v;
            }
            fprintf(stderr, "[glz] %s: grayscale fallback\n", path);
        }
    }

    /* -- Bitplane decode ---------------------------------------------------
     * GLZ: 4 bitplanes per column (B, R, G, E) in ONE continuous LZ77
     *      stream. Stream starts at 0x10 (or 0x40 if pal_flag).
     * XSC: 4 INDEPENDENT LZ77 streams, one per bitplane. Each stream
     *      decodes bpr*rows bytes. Stream sizes are at 0x0A..0x11,
     *      streams start at 0x12 (concatenated).
     * Same LZ77 core: 16 KB ring buffer, distance table from rows.
     * Final RGB index for pixel (x, y):
     *   idx = (E << 3) | (G << 2) | (R << 1) | B */
    uint16_t tbl[256];
    build_table(g->rows, tbl);

    const int MAXCOLS = 300;
    const size_t plane_size = (size_t)g->rows * MAXCOLS;
    uint8_t *planes[4];
    for (int i = 0; i < 4; i++) {
        planes[i] = calloc(plane_size, 1);
        if (!planes[i]) {
            for (int j = 0; j < i; j++) free(planes[j]);
            free(data);
            return -1;
        }
    }

    int ncols = 0;

    /* Codec lookup per plane. Codec 1/3 use an L1 layout, codec 2/4
     * use dense, so the compose loop needs this list. */
    uint8_t xsc_codecs[4] = {0, 0, 0, 0};

    if (fmt == FMT_XSC) {
        /* XSC: 4 independent streams, one per plane, each with fresh
         * LZ state and its own end-of-stream bound. Validated against
         * 56 Yukonis files (all decode to bpr * rows bytes/plane). */
        /* Per-plane codec IDs. */
        for (int i = 0; i < 4; i++) xsc_codecs[i] = data[i];

        uint16_t xsc_sizes[4];
        for (int i = 0; i < 4; i++)
            xsc_sizes[i] = data[0x0A + i*2] | (data[0x0B + i*2] << 8);

        size_t stream_off = 0x12;
        for (int pi = 0; pi < 4; pi++) {
            size_t slen = xsc_sizes[pi];
            size_t stream_end = stream_off + slen;
            size_t want = (size_t)g->bpr * g->rows;
            int codec = xsc_codecs[pi];
            size_t written = 0;
            if (codec >= 1 && codec <= 4) {
                written = decode_rle_xsc(data + stream_off, slen,
                                         planes[pi], want,
                                         (codec == 1 || codec == 2));
            } else {
                fprintf(stderr, "[glz]   plane %d: unknown codec %d\n",
                        pi, codec);
            }
            fprintf(stderr, "[glz]   plane %d: codec %d, %zu/%zu bytes\n",
                    pi, codec, written, want);
            stream_off = stream_end;
        }
        ncols = g->bpr;
    } else {
        /* GLZ: single continuous stream. */
        uint8_t ring[0x4000];
        memset(ring, 0, sizeof(ring));

        uint8_t ah = 1, dh = 1, dl = 0;
        int si = 0, bp = 0;
        size_t sp = pos;
        int ended = 0;

        for (int col = 0; col < MAXCOLS && !ended; col++) {
            for (int pi = 0; pi < 4 && !ended; pi++) {
                uint8_t *plane = planes[pi];
                for (int row = 0; row < g->rows && !ended; row++) {
                    uint8_t al;
                    if ((--ah) == 0) {
                        if ((--dh) == 0) {
                            if (sp >= (size_t)sz) { ended = 1; break; }
                            dl = data[sp++];
                            dh = 8;
                        }
                        int carry = (dl >> 7) & 1;
                        dl = (dl << 1) & 0xFF;
                        if (carry == 0) {
                            if (sp >= (size_t)sz) { ended = 1; break; }
                            al = data[sp++];
                            ah = 1;
                        } else {
                            if (sp + 1 >= (size_t)sz) { ended = 1; break; }
                            uint8_t lo = data[sp++];
                            uint8_t hi = data[sp++];
                            si = (bp - tbl[lo]) & 0x3FFF;
                            al = ring[si];
                            si = (si + 1) & 0x3FFF;
                            ah = hi;
                        }
                    } else {
                        al = ring[si];
                        si = (si + 1) & 0x3FFF;
                    }
                    plane[row * MAXCOLS + col] = al;
                    ring[bp] = al;
                    bp = (bp + 1) & 0x3FFF;
                }
            }
            if (sp > (size_t)sz) ended = 1;
            if (!ended) ncols = col + 1;
        }
    }

    /* -- Compose RGB24 ----------------------------------------------------- */
    const int W = ncols * 8;
    const int H = g->rows;
    g->width  = W;
    g->height = H;
    g->rgb = malloc((size_t)W * H * 3);
    if (!g->rgb) {
        for (int i = 0; i < 4; i++) free(planes[i]);
        free(data);
        return -1;
    }
    g->indices = malloc((size_t)W * H);
    if (!g->indices) {
        free(g->rgb); g->rgb = NULL;
        for (int i = 0; i < 4; i++) free(planes[i]);
        free(data);
        return -1;
    }

    /*
     * Layout depends on the codec of each plane.
     *
     * Codec 1 and 3 (L1): two pages, column-major.
     *     page_size = (bpr * rows) / 2
     *     stride    = rows / 2
     *     bi = (y & 1) * page_size + (x >> 3) * stride + (y >> 1)
     *
     * Codec 2 and 4 (dense): row-major with stride = bpr.
     *     bi = y * bpr + (x >> 3)
     */
    int is_xsc = (fmt == FMT_XSC);
    size_t xsc_page_size = ((size_t)g->bpr * g->rows) / 2;
    int    xsc_stride    = g->rows / 2;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int c   = x >> 3;
            int bit = 7 - (x & 7);
            int vals[4] = {0, 0, 0, 0};
            for (int pi = 0; pi < 4; pi++) {
                int bi;
                if (!is_xsc) {
                    bi = y * MAXCOLS + c;               /* GLZ */
                } else if (xsc_codecs[pi] == 1
                        || xsc_codecs[pi] == 3) {
                    bi = (int)((y & 1) * xsc_page_size  /* L1 */
                               + c * xsc_stride + (y >> 1));
                } else {
                    bi = y * g->bpr + c;                /* dense */
                }
                if (bi >= 0 && bi < (int)plane_size)
                    vals[pi] = (planes[pi][bi] >> bit) & 1;
            }
            int b  = vals[0];
            int r  = vals[1];
            int gg = vals[2];
            int e  = vals[3];
            int idx = (e << 3) | (gg << 2) | (r << 1) | b;
            g->indices[y * W + x] = (uint8_t)idx;
            uint8_t *p = &g->rgb[(y * W + x) * 3];
            p[0] = g->palette[idx][0];
            p[1] = g->palette[idx][1];
            p[2] = g->palette[idx][2];
        }
    }

    for (int i = 0; i < 4; i++) free(planes[i]);
    free(data);
    return 0;
}

void glz_free(GLZ *g) {
    if (!g) return;
    free(g->rgb);      g->rgb = NULL;
    free(g->indices);  g->indices = NULL;
}

/* --------------------------------------------------------------------- */
/* PNG writer (uncompressed, stored deflate blocks)                      */
/* --------------------------------------------------------------------- */

static uint32_t g_crc_table[256];
static int      g_crc_ready = 0;

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc_table[i] = c;
    }
    g_crc_ready = 1;
}

static uint32_t adler32_buf(const uint8_t *buf, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) {
        a = (a + buf[i]) % 65521;
        b = (b + a)    % 65521;
    }
    return (b << 16) | a;
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8)  & 0xFF;
    p[3] =  v        & 0xFF;
}

static void write_chunk(FILE *f, const char *type,
                        const uint8_t *data, size_t len) {
    uint8_t hdr[8];
    put_be32(hdr, (uint32_t)len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len) fwrite(data, 1, len, f);

    if (!g_crc_ready) crc_init();
    uint32_t c = 0xFFFFFFFFu;
    const uint8_t *tp = (const uint8_t *)type;
    for (int i = 0; i < 4; i++) c = g_crc_table[(c ^ tp[i]) & 0xFF] ^ (c >> 8);
    for (size_t i = 0; i < len; i++) c = g_crc_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    c ^= 0xFFFFFFFFu;

    uint8_t crcbuf[4];
    put_be32(crcbuf, c);
    fwrite(crcbuf, 1, 4, f);
}

int write_png(const char *path, int w, int h, const uint8_t *rgb) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);

    uint8_t ihdr[13];
    put_be32(ihdr, (uint32_t)w);
    put_be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    write_chunk(f, "IHDR", ihdr, 13);

    /* Raw rows: 1 filter byte (0) + w*3 RGB bytes per row. */
    const size_t raw_len = (size_t)h * (1 + (size_t)w * 3);
    uint8_t *raw = malloc(raw_len);
    if (!raw) { fclose(f); return -1; }
    size_t o = 0;
    for (int y = 0; y < h; y++) {
        raw[o++] = 0;
        memcpy(raw + o, rgb + (size_t)y * w * 3, (size_t)w * 3);
        o += (size_t)w * 3;
    }

    /* zlib stream: 0x78 0x01 + stored deflate blocks + Adler-32. */
    const size_t num_blocks = (raw_len + 65534) / 65535;
    const size_t zlen = 2 + raw_len + num_blocks * 5 + 4;
    uint8_t *zbuf = malloc(zlen);
    if (!zbuf) { free(raw); fclose(f); return -1; }
    size_t zp = 0;
    zbuf[zp++] = 0x78;
    zbuf[zp++] = 0x01;

    size_t rem = raw_len, off = 0;
    while (rem > 0) {
        size_t blk = rem > 65535 ? 65535 : rem;
        int final = (blk == rem);
        zbuf[zp++] = final ? 1 : 0;
        zbuf[zp++] =  blk        & 0xFF;
        zbuf[zp++] = (blk >> 8)  & 0xFF;
        zbuf[zp++] = (~blk)      & 0xFF;
        zbuf[zp++] = ((~blk) >> 8) & 0xFF;
        memcpy(zbuf + zp, raw + off, blk);
        zp  += blk;
        off += blk;
        rem -= blk;
    }
    put_be32(zbuf + zp, adler32_buf(raw, raw_len));
    zp += 4;

    write_chunk(f, "IDAT", zbuf, zp);
    write_chunk(f, "IEND", NULL, 0);

    free(zbuf);
    free(raw);
    fclose(f);
    return 0;
}