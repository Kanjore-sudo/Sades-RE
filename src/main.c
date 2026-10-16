/*
 * SADES Engine — a reimplementation
 * Copyright (c) 2026 Kalizkal
 * MIT License — see LICENSE
 */

/*
 * Front-end for the SADES VM.
 *
 * Responsibilities:
 *   - Parse the command line and dispatch sub-commands.
 *   - Own the SDL window and the compositor: background, character
 *     layer and text overlay.
 *   - Provide the callbacks the VM needs (load graphic, draw text,
 *     wait for input, clear layers).
 *   - Render the engine logo from TITLE.SYS at startup.
 *
 * Graphics are classified by size:
 *     w >= 400, h >= 300          full-screen background
 *     w == 320, h == 200          SD panel, stacked vertically
 *     100 <= w, h <= 399          portrait, positioned from the header
 *     w == 8, h == 6, id 500..999 blink/tint command, not drawn
 *     anything else               tile, drawn at (0, 0)
 *
 * Environment variables:
 *     SADES_DX, SADES_DY         base offset for portraits
 *     SADES_CK_R, SADES_CK_G, SADES_CK_B
 *                                optional color key for transparency
 *     SADES_ANIM_MS              frame delay (default 64)
 *     SADES_ANIM                 set to 0 to disable animation delays
 *     SADES_DELAY_MULT           ms per DELAY tick (default 32)
 *     SADES_MODE<N>_R, _G, _B    override text color for mode N
 *     SADES_FONT                 path to a Japanese font
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <SDL2/SDL.h>

#include "glz.h"
#include "m00.h"
#include "sce.h"
#include "display.h"
#include "font.h"

/* ====================================================================== */
/* 1. Globals                                                             */
/* ====================================================================== */

/* Framebuffer ---------------------------------------------------------- */
static uint8_t *g_fb   = NULL;
static int      g_fb_w = 640;
static int      g_fb_h = 400;

/* Engine state --------------------------------------------------------- */
static char g_glz_dir[512] = {0};   /* directory of the loaded .SCE */
static int  g_bg_hook      = 0;     /* dump CALL_IND targets when set */

/* User-tunable offsets ------------------------------------------------- */
static int g_off_dx = 0;
static int g_off_dy = 0;

/* Background palette and its original. Each sprite keeps its own. */
static uint8_t g_bg_palette[16][3] = {
    {0,0,0},{17,17,17},{34,34,34},{51,51,51},
    {68,68,68},{85,85,85},{102,102,102},{119,119,119},
    {136,136,136},{153,153,153},{170,170,170},{187,187,187},
    {204,204,204},{221,221,221},{238,238,238},{255,255,255},
};
static uint8_t g_bg_orig_palette[16][3] = {
    {0,0,0},{17,17,17},{34,34,34},{51,51,51},
    {68,68,68},{85,85,85},{102,102,102},{119,119,119},
    {136,136,136},{153,153,153},{170,170,170},{187,187,187},
    {204,204,204},{221,221,221},{238,238,238},{255,255,255},
};
static int g_gdc_palette_ready = 0;

/* Animation ------------------------------------------------------------ */
static int g_anim_ms     = 64;
static int g_anim_enable = 1;

/* No global tint state: palettes are per-sprite. */

/* Color key (optional transparency) ------------------------------------ */
static int g_ck_r = -1, g_ck_g = -1, g_ck_b = -1;

/* Background layer: indices, cleared on PRINT_CLEAR(2). */
static uint8_t *g_bg_idx = NULL;

/* ====================================================================== */
/* 2. Utilities                                                           */
/* ====================================================================== */

static void usage(const char *p) {
    fprintf(stderr,
        "usage:\n"
        "  %s glz   <in.glz> <out.png> [--pal <pal.glz>]\n"
        "  %s info  <in.glz>\n"
        "  %s title <TITLE.SYS> <out.png>\n"
        "  %s sce   <in.sce> <in.m00> [--trace] [--bg-hook] [--explore]\n"
        "  %s show  <in.glz>\n"
        "  %s play  <in.sce> <in.m00> [--title <glz>] [--trace] [--bg-hook]\n",
        p, p, p, p, p, p);
}

/* Block until a key is pressed. Returns the SDLK code, or SDLK_ESCAPE
 * if the window was closed. */
static int sdl_wait_key_raw(void) {
    int k;
    /* Drain anything pending first: we want a fresh keydown. */
    while ((k = display_poll()) >= 0) {
        if (k == SDLK_ESCAPE) return k;
        SDL_Delay(1);
    }
    for (;;) {
        k = display_poll();
        if (k == SDLK_ESCAPE) return k;
        if (k >= 0) break;
        SDL_Delay(16);
    }
    /*
     * Save the key before draining the queue. Reusing `k` in the
     * drain loop condition clobbers it with -1 once the queue is
     * empty, which silently discards the actual keypress.
     */
    int saved = k;
    while ((k = display_poll()) >= 0) SDL_Delay(1);
    return saved;
}

/*
 * VM wait callback. Maps number keys to a menu option index, ESC to
 * "abort the VM", and any key (when n_opts <= 1) to option 0.
 */
static int sdl_wait_key_vm(VM *vm, int n_opts) {
    (void)vm;
    /* DROP and non-menu waits call this with n_opts == 1. Any key is
     * accepted in that case. For n_opts >= 2, require 1-9 or KP 1-9. */
    for (;;) {
        int k = sdl_wait_key_raw();
        if (k == SDLK_ESCAPE) {
            printf("[ESC] aborting VM\n");
            fflush(stdout);
            return -1;
        }
        if (n_opts <= 1) {
            fprintf(stderr, "[wait] key=0x%X n_opts=1 (any key OK)\n", k);
            return 0;
        }
        if (k >= SDLK_1 && k <= SDLK_9) {
            int idx = (k - SDLK_1) % n_opts;
            fprintf(stderr, "[wait] key=0x%X n_opts=%d -> option %d\n",
                    k, n_opts, idx);
            return idx;
        }
        if (k >= SDLK_KP_1 && k <= SDLK_KP_9) {
            int idx = (k - SDLK_KP_1) % n_opts;
            fprintf(stderr, "[wait] key=0x%X (KP) n_opts=%d -> option %d\n",
                    k, n_opts, idx);
            return idx;
        }
        fprintf(stderr, "[wait] ignoring key=0x%X (n_opts=%d)\n", k, n_opts);
    }
}

/*
 * DELAY callback. Ticks are scaled by SADES_DELAY_MULT (default 32 ms)
 * and capped at 3000 ms so malformed bytecode can't hang the process.
 */
static void sdl_delay_vm(VM *vm, uint16_t ticks) {
    (void)vm;
    static int mult = -1;
    if (mult < 0) {
        const char *e = getenv("SADES_DELAY_MULT");
        mult = e ? atoi(e) : 32;
        if (mult <= 0) mult = 6;
    }
    uint32_t ms = (uint32_t)ticks * (uint32_t)mult;
    if (ms > 3000) ms = 3000;
    SDL_Delay(ms);
}

/* ====================================================================== */
/* 3. Character layer                                                     */
/* ====================================================================== */

typedef struct {
    int      px, py;
    int      w, h;
    uint8_t *indices;
    uint8_t  orig_palette[16][3];
    uint8_t  cur_palette[16][3];
    int      glz_id;
    int      hidden;
} char_item_t;

static char_item_t g_chars[64];
static int         g_char_count = 0;

/* Two sprites within CHAR_POS_THRESHOLD pixels of each other share a
 * slot, so replacing a portrait doesn't accumulate garbage. */
#define CHAR_POS_THRESHOLD 30

static void clear_chars(void) {
    for (int i = 0; i < g_char_count; i++) {
        free(g_chars[i].indices);
        g_chars[i].indices = NULL;
    }
    g_char_count = 0;
}

static int char_slot_put(int px, int py, int w, int h,
                         uint8_t *indices,
                         const uint8_t palette[16][3],
                         int glz_id) {
    for (int i = 0; i < g_char_count; i++) {
        char_item_t *c = &g_chars[i];
        int dx = px - c->px; if (dx < 0) dx = -dx;
        int dy = py - c->py; if (dy < 0) dy = -dy;
        /*
         * Replace only when the new sprite has the same dimensions
         * and sits at the same position. This is what "same character,
         * different expression" looks like to the engine. A different-
         * shaped sprite that happens to share coordinates must be a
         * separate layer, so the draw order stays in script order.
         */
        if (dx < CHAR_POS_THRESHOLD && dy < CHAR_POS_THRESHOLD
         && w == c->w && h == c->h) {
            if (g_anim_enable && g_anim_ms > 0 && g_fb) {
                display_show_rgb(g_fb, g_fb_w, g_fb_h);
                SDL_Delay(g_anim_ms);
            }
            free(c->indices);
            c->indices = indices;
            memcpy(c->orig_palette, palette, sizeof(c->orig_palette));
            memcpy(c->cur_palette,  palette, sizeof(c->cur_palette));
            c->px     = px;
            c->py     = py;
            c->w      = w;
            c->h      = h;
            c->glz_id = glz_id;
            c->hidden = 0;
            fprintf(stderr, "[char_slot] REPLACE %d @(%d,%d) %dx%d id=%d\n",
                    i, px, py, w, h, glz_id);
            return i;
        }
    }
    if (g_char_count < 64) {
        char_item_t *c = &g_chars[g_char_count++];
        c->px     = px;
        c->py     = py;
        c->w      = w;
        c->h      = h;
        c->indices = indices;
        memcpy(c->orig_palette, palette, sizeof(c->orig_palette));
        memcpy(c->cur_palette,  palette, sizeof(c->cur_palette));
        c->glz_id = glz_id;
        c->hidden = 0;
        fprintf(stderr, "[char_slot] ADD %d @(%d,%d) %dx%d id=%d (total=%d)\n",
                g_char_count - 1, px, py, w, h, glz_id, g_char_count);
        return g_char_count - 1;
    }
    free(indices);
    return -1;
}

/* ====================================================================== */
/* 4. Text layer                                                          */
/* ====================================================================== */

#define MAX_TEXTS 1024
typedef struct {
    int     px, py;
    uint8_t r, g, b;
    char    text[256];
} text_item_t;

static text_item_t g_page_texts[MAX_TEXTS];
static int         g_page_text_count = 0;

/* Draw one line with a 1-pixel offset black shadow, which is how the
 * original engine renders text against busy backgrounds. */
static void draw_text_line(int px, int py, const char *text,
                           uint8_t r, uint8_t g, uint8_t b) {
    font_draw(g_fb, g_fb_w, g_fb_h, px + 1, py + 1, text, 0, 0, 0);
    font_draw(g_fb, g_fb_w, g_fb_h, px,     py,     text, r, g, b);
}

/* ====================================================================== */
/* 5. Compositor                                                          */
/* ====================================================================== */

/* Build a frame from bg + chars + texts and show it.
 * Called after every state change (GUARD, CALL, PRINT_CLEAR). */
static void recomposite(void) {
    if (!g_fb) return;

    if (g_bg_idx) {
        for (int i = 0; i < g_fb_w * g_fb_h; i++) {
            const uint8_t *src = g_bg_palette[g_bg_idx[i]];
            g_fb[i*3+0] = src[0];
            g_fb[i*3+1] = src[1];
            g_fb[i*3+2] = src[2];
        }
    } else {
        memset(g_fb, 0, (size_t)g_fb_w * g_fb_h * 3);
    }

    /* Character layer: each sprite resolves its own indices. */
    for (int i = 0; i < g_char_count; i++) {
        char_item_t *c = &g_chars[i];
        if (c->hidden) continue;
        for (int y = 0; y < c->h; y++) {
            int dy = c->py + y;
            if (dy < 0 || dy >= g_fb_h) continue;
            for (int x = 0; x < c->w; x++) {
                int dx = c->px + x;
                if (dx < 0 || dx >= g_fb_w) continue;
                uint8_t idx = c->indices[y * c->w + x];
                const uint8_t *src = c->cur_palette[idx];
                if (g_ck_r >= 0 && g_ck_g >= 0 && g_ck_b >= 0
                 && src[0] == g_ck_r && src[1] == g_ck_g && src[2] == g_ck_b) {
                    continue;
                }
                memcpy(&g_fb[(dy * g_fb_w + dx) * 3], src, 3);
            }
        }
    }

    /* Text overlay. */
    for (int i = 0; i < g_page_text_count; i++) {
        text_item_t *t = &g_page_texts[i];
        draw_text_line(t->px, t->py, t->text, t->r, t->g, t->b);
    }

    display_show_rgb(g_fb, g_fb_w, g_fb_h);
}

/* ====================================================================== */
/* 6. VM callbacks                                                        */
/* ====================================================================== */

/*
 * 8x6 markers (IDs 500..999).
 *
 *   bucket = id / 100, target = id % 100
 *
 *   5xx  restore original palettes (bg + all sprites)
 *   6xx  global tint: every palette <- marker palette
 *   7xx  same as 6xx
 *   8xx  same as 6xx
 *   9xx  hide the target sprite
 */
static int handle_blink_marker(GLZ *g, int glz_id) {
    int bucket = glz_id / 100;
    int target = glz_id % 100;

    if (bucket == 5) {
        for (int i = 0; i < g_char_count; i++) {
            memcpy(g_chars[i].cur_palette, g_chars[i].orig_palette,
                   sizeof(g_chars[i].cur_palette));
            g_chars[i].hidden = 0;
        }
        memcpy(g_bg_palette, g_bg_orig_palette, sizeof(g_bg_palette));
        fprintf(stderr, "[marker] RESTORE ALL\n");
    } else if (bucket == 6 || bucket == 7 || bucket == 8) {
        for (int i = 0; i < g_char_count; i++) {
            memcpy(g_chars[i].cur_palette, g->palette,
                   sizeof(g_chars[i].cur_palette));
            g_chars[i].hidden = 0;
        }
        memcpy(g_bg_palette, g->palette, sizeof(g_bg_palette));
        fprintf(stderr, "[marker] TINT ALL (bucket %d)\n", bucket);
    } else if (bucket == 9) {
        for (int i = 0; i < g_char_count; i++) {
            if (g_chars[i].glz_id == target) {
                g_chars[i].hidden = 1;
                fprintf(stderr, "[marker] HIDE sprite %d\n", target);
                break;
            }
        }
    }

    glz_free(g);
    recomposite();
    if (g_anim_enable && g_anim_ms > 0) SDL_Delay(g_anim_ms);
    return 0;
}


/*
 * .XSC files carry no palette. Some games ship a default .PAL next to
 * the .SCE; load it once so the first sprite renders with proper
 * colors. No-op for games with embedded palettes.
 */
static int g_pal_loaded = 0;
static void try_load_default_pal(const char *glz_dir) {
    if (g_pal_loaded) return;
    char pal_path[1024];
    /* Try p00001.pal first (Yukonis), then palette.pal, then game.pal. */
    const char *names[] = {"p00001.pal", "palette.pal", "game.pal", NULL};
    for (int i = 0; names[i]; i++) {
        snprintf(pal_path, sizeof(pal_path), "%s/%s", glz_dir, names[i]);
        FILE *f = fopen(pal_path, "rb");
        if (!f) continue;
        uint8_t buf[0x40];
        size_t got = fread(buf, 1, 0x40, f);
        fclose(f);
        if (got < 0x40) continue;
        glz_set_external_palette_rgb32(buf);
        for (int i = 0; i < 16; i++) {
            g_bg_palette[i][0] = buf[i*4 + 0];
            g_bg_palette[i][1] = buf[i*4 + 1];
            g_bg_palette[i][2] = buf[i*4 + 2];
            g_bg_orig_palette[i][0] = g_bg_palette[i][0];
            g_bg_orig_palette[i][1] = g_bg_palette[i][1];
            g_bg_orig_palette[i][2] = g_bg_palette[i][2];
        }
        fprintf(stderr, "[pal] loaded default palette from %s (RGB32)\n", pal_path);
        g_pal_loaded = 1;
        return;
    }
}

static int on_load_glz(VM *vm, int glz_id, int arg_x, int arg_y) {
    (void)vm;
    if (glz_id <= 0 || glz_id > 99999) return -1;

    /* Try .GLZ first, then .XSC. */
    char path[1024];
    snprintf(path, sizeof(path), "%s/P%05d.GLZ", g_glz_dir, glz_id);
    if (access(path, R_OK) != 0) {
        snprintf(path, sizeof(path), "%s/p%05d.glz", g_glz_dir, glz_id);
        if (access(path, R_OK) != 0) {
            snprintf(path, sizeof(path), "%s/P%05d.XSC", g_glz_dir, glz_id);
            if (access(path, R_OK) != 0) {
                snprintf(path, sizeof(path), "%s/p%05d.xsc", g_glz_dir, glz_id);
            }
        }
    }

    /* Load the game's default .PAL if it has one. */
    try_load_default_pal(g_glz_dir);

    /* Pre-read the header to get the pad fields before the full load. */
    int arg_px = (arg_x == 0xFFFF) ? 0 : arg_x;
    int arg_py = (arg_y == 0xFFFF) ? 0 : arg_y;
    int is_xsc_local = 0;
    {
        FILE *f = fopen(path, "rb");
        if (f) {
            uint8_t h[16];
            if (fread(h, 1, 16, f) == 16) {
                /* First byte: 0xFF means .GLZ, 1..4 means .XSC. */
                int is_glz = (h[0] == 0xFF && h[1] == 0xFF && h[2] == 0xFF);
                if (is_glz) {
                    /* .GLZ: pad_x at 0x08, pad_y at 0x06. */
                    if (arg_x == 0xFFFF) arg_px = h[0x08] | (h[0x09] << 8);
                    if (arg_y == 0xFFFF) arg_py = h[0x06] | (h[0x07] << 8);
                } else {
                    /* .XSC: single pad at 0x04, raw VRAM byte offset. */
                    is_xsc_local = 1;
                    if (arg_x == 0xFFFF)
                        arg_px = h[0x04] | (h[0x05] << 8);
                    if (arg_y == 0xFFFF) arg_py = 0;
                }
            }
            fclose(f);
        }
    }

    fprintf(stderr, "[GUARD] P%05d @ (%d,%d)",
            glz_id, arg_px, arg_py);
    if (!g_fb) { fprintf(stderr, " (batch)\n"); return 0; }

    GLZ g;
    if (glz_load(path, &g) != 0) {
        fprintf(stderr, " LOAD_FAIL\n");
        return -1;
    }
    fprintf(stderr, " size=%dx%d\n", g.width, g.height);

    int is_marker = (g.width == 8 && g.height == 6
                     && glz_id >= 500 && glz_id < 1000);

    if (is_marker) {
        return handle_blink_marker(&g, glz_id);
    }

    int is_bg       = (g.width >= 400 && g.height >= 300);
    int is_sd_panel = (!is_bg && g.width == 320 && g.height == 200);
    int is_portrait = (!is_bg && !is_sd_panel
                       && g.width >= 100 && g.height >= 100);

    int px = arg_px, py = arg_py;

    if (is_portrait && is_xsc_local) {
        /* .XSC: pad is a VRAM byte offset. Convert to screen coords
         * using the standard 640x400 layout (80 bytes per row). */
        int pad = g.pad_y;
        px = g_off_dx + (pad % 80) * 8;
        py = g_off_dy + (pad / 80);
        fprintf(stderr, "XSC %dx%d id=%d pad=%d -> pos=(%d,%d)\n",
                g.width, g.height, glz_id, pad, px, py);
    } else if (is_portrait) {
        /* .GLZ: pad_y is X in 8-pixel cells, pad_x is Y in pixels. */
        px = g_off_dx + g.pad_y * 8;
        py = g_off_dy + g.pad_x;
        fprintf(stderr, "GLZ %dx%d id=%d pad=(%d,%d) -> pos=(%d,%d)\n",
                g.width, g.height, glz_id, g.pad_x, g.pad_y, px, py);
    } else if (is_sd_panel) {
        /*
         * SD panels come in through arg_x as a vertical slot (0 for
         * the top, 200 for the bottom). The other argument is unused,
         * and both panels stack in the left column without DX/DY.
         */
        px = arg_py;
        py = arg_px;
        fprintf(stderr, "GLZ SD %dx%d arg=(%d,%d) -> (%d,%d)\n",
                g.width, g.height, arg_px, arg_py, px, py);
    }

    if (is_bg) {
        /*
         * A background overwrites only its own rectangle, not the
         * entire character layer. Erase any sprites that overlap it
         * and leave the rest alone, so a portrait drawn to the right
         * of a smaller background survives.
         */
        int bg_x0 = arg_px, bg_y0 = arg_py;
        int bg_x1 = bg_x0 + g.width;
        int bg_y1 = bg_y0 + g.height;
        for (int i = 0; i < g_char_count; i++) {
            char_item_t *c = &g_chars[i];
            int cx0 = c->px, cy0 = c->py;
            int cx1 = cx0 + c->w, cy1 = cy0 + c->h;
            if (cx1 > bg_x0 && cx0 < bg_x1
             && cy1 > bg_y0 && cy0 < bg_y1) {
                free(c->indices);
                c->indices = NULL;
                c->hidden = 1;
            }
        }
        if (g_bg_idx) {
            if (g.pal_flag) {
                memcpy(g_bg_palette,      g.palette, sizeof(g_bg_palette));
                memcpy(g_bg_orig_palette, g.palette, sizeof(g_bg_orig_palette));
            }
            memset(g_bg_idx, 0, (size_t)g_fb_w * g_fb_h);
            for (int y = 0; y < g.height; y++) {
                int dy = py + y;
                if (dy < 0 || dy >= g_fb_h) continue;
                for (int x = 0; x < g.width; x++) {
                    int dx = px + x;
                    if (dx < 0 || dx >= g_fb_w) continue;
                    g_bg_idx[dy * g_fb_w + dx] =
                        g.indices[y * g.width + x];
                }
            }
        }
    } else {
        uint8_t *copy = malloc((size_t)g.width * g.height);
        if (copy) {
            memcpy(copy, g.indices, (size_t)g.width * g.height);
            char_slot_put(px, py, g.width, g.height, copy,
                          g.palette, glz_id);
        }
    }

    glz_free(&g);
    recomposite();
    return 0;
}

/* Batch-mode menu picker: cycles through options so headless runs cover
 * every branch. Used by the `sce` sub-command. */
static uint32_t g_menu_seq = 0;
static uint32_t g_menu_pc[65536];
static int      g_explore  = 0;

static int batch_wait_fn(VM *vm, int n_opts) {
    if (n_opts <= 1) return 0;
    if (!g_explore)  return 0;
    uint32_t key   = (uint32_t)(vm->pc) & 0xFFFF;
    uint32_t visit = g_menu_pc[key]++;
    uint32_t rot   = g_menu_seq++;
    return (int)((visit + rot) % (uint32_t)n_opts);
}

static void on_print_clear(VM *vm, uint16_t flags) {
    (void)vm;
    fprintf(stderr, "[PRINT_CLEAR] flags=%u text=%d vram=%d\n",
            flags, flags & 1, (flags >> 1) & 1);
    if (flags & 0x1) g_page_text_count = 0;
    if (flags & 0x2) {
        g_page_text_count = 0;
        clear_chars();
        if (g_bg_idx) memset(g_bg_idx, 0, (size_t)g_fb_w * g_fb_h);
    }
    recomposite();
}

/* RC MODE field is a PC-98 text color index, not a "mode":
 *   0=black 1=blue 2=red 3=magenta 4=green 5=cyan 6=yellow 7=white
 * The blink bit lives in the text attribute (bit 7), not the MODE. */
static const uint8_t kModeRgb[8][3] = {
    {0x00, 0x00, 0x00},  /* 0: black   (in-game narration) */
    {0x40, 0x80, 0xFF},  /* 1: blue    (HIT in combat) */
    {0xFF, 0x40, 0x40},  /* 2: red     (MISS in combat) */
    {0xFF, 0x40, 0xFF},  /* 3: magenta */
    {0x00, 0xFF, 0x40},  /* 4: green   (credits) */
    {0x40, 0xFF, 0xFF},  /* 5: cyan */
    {0xFF, 0xFF, 0x40},  /* 6: yellow */
    {0xFF, 0xFF, 0xFF},  /* 7: white   (SD layout) */
};

static void on_call(VM *vm, int id, int sub, uint16_t off, const char *text) {
    (void)sub;
    (void)off;
    if (id < 0 || id >= 256) return;

    uint8_t cx   = vm->rc[id].x;
    uint8_t cy   = vm->rc[id].y;
    uint8_t mode = vm->rc[id].mode;
    int px = cx * 8;
    int py = cy * 16;

    uint8_t r, g, b;
    if (mode < 8) {
        r = kModeRgb[mode][0];
        g = kModeRgb[mode][1];
        b = kModeRgb[mode][2];
    } else {
        r = g = b = 0xFF;
    }

    /* Per-mode color override for debugging. */
    {
        char key[32], *e;
        snprintf(key, sizeof(key), "SADES_MODE%d_R", mode);
        e = getenv(key); if (e) r = (uint8_t)atoi(e);
        snprintf(key, sizeof(key), "SADES_MODE%d_G", mode);
        e = getenv(key); if (e) g = (uint8_t)atoi(e);
        snprintf(key, sizeof(key), "SADES_MODE%d_B", mode);
        e = getenv(key); if (e) b = (uint8_t)atoi(e);
    }

    /*
     * Reuse an existing slot at the same position. This matters for
     * the combat display, where the same line cycles through states
     * like 【1】??? -> 【1】HIT without stacking up duplicates.
     */
    int dup = 0;
    for (int i = 0; i < g_page_text_count; i++) {
        text_item_t *t = &g_page_texts[i];
        if (t->px == px && t->py == py) {
            snprintf(t->text, sizeof(t->text), "%s", text);
            t->r = r; t->g = g; t->b = b;
            dup = 1;
            break;
        }
    }
    if (!dup && g_page_text_count < MAX_TEXTS) {
        text_item_t *t = &g_page_texts[g_page_text_count++];
        t->px = px;
        t->py = py;
        t->r = r; t->g = g; t->b = b;
        snprintf(t->text, sizeof(t->text), "%s", text);
    }

    recomposite();
    fprintf(stderr,
            "  CALL id=%-3d RC=(%d,%d,m=%d) px=(%d,%d) rgb=(%d,%d,%d) [texts=%d] %s\n",
            id, cx, cy, mode, px, py, r, g, b, g_page_text_count, text);
}

static void on_call_ind(VM *vm, uint16_t arg, uint16_t target) {
    (void)vm;
    if (!g_bg_hook) return;
    fprintf(stderr, "  [CALL_IND] arg=0x%04X target=0x%04X\n", arg, target);
}

/* ====================================================================== */
/* 7. TITLE.SYS (engine logo)                                             */
/* ====================================================================== */

/*
 * TITLE.SYS is a 68-byte header followed by a 1bpp planar bitmap.
 *
 * Header layout:
 *   0x00  4-byte magic: 00 00 04 08
 *   0x04  word BE: nibbles for background (index 2)
 *   0x06  word BE: nibbles for foreground (indices 1 and 3)
 *   0x08  "COMMAND.X\0" companion filename
 *   0x40  width  (BE)
 *   0x42  height (BE)
 *
 * The bitmap uses two colors, bit 1 = foreground, bit 0 = background.
 * Bytes per row is ceil(W / 8), not W / 8 — widths that aren't a
 * multiple of 8 (like the Yukonis no Mori logo at 300) need the
 * rounded-up value, otherwise every row is offset by one byte.
 *
 * Palette nibbles are packed in G, R, B order. Each nibble becomes
 * 0..255 by multiplying by 17.
 */
static int load_title_rgb(const char *path,
                          uint8_t **out_rgb, int *out_w, int *out_h) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 68) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }

    uint8_t *data = malloc((size_t)sz);
    if (!data) { fclose(f); return -1; }
    if (fread(data, 1, (size_t)sz, f) != (size_t)sz) {
        free(data); fclose(f); return -1;
    }
    fclose(f);

    int W = (data[0x40] << 8) | data[0x41];
    int H = (data[0x42] << 8) | data[0x43];
    if (W <= 0 || H <= 0 || (size_t)(68 + ((W + 7) / 8) * H) > (size_t)sz) {
        free(data);
        return -1;
    }
    int bpr = (W + 7) / 8;

    uint8_t *rgb = malloc((size_t)W * H * 3);
    if (!rgb) { free(data); return -1; }

    uint16_t w_fg = (data[0x06] << 8) | data[0x07];
    uint16_t w_bg = (data[0x04] << 8) | data[0x05];
    uint8_t fg = ((w_fg >>  2)       & 0x0F) * 17;
    uint8_t fr = ((w_fg <<  1) >> 8  & 0x0F) * 17;
    uint8_t fb = ((w_fg >> 12)       & 0x0F) * 17;
    uint8_t bg = ((w_bg >>  2)       & 0x0F) * 17;
    uint8_t br = ((w_bg <<  1) >> 8  & 0x0F) * 17;
    uint8_t bb = ((w_bg >> 12)       & 0x0F) * 17;
    fprintf(stderr, "[title] palette fg=#%02X%02X%02X bg=#%02X%02X%02X\n",
            fr, fg, fb, br, bg, bb);

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int byte = data[68 + y * bpr + (x >> 3)];
            int bit  = (byte >> (7 - (x & 7))) & 1;
            uint8_t *p = &rgb[(y * W + x) * 3];
            if (bit) { p[0]=fr; p[1]=fg; p[2]=fb; }
            else     { p[0]=br; p[1]=bg; p[2]=bb; }
        }
    }
    free(data);
    *out_rgb = rgb;
    *out_w   = W;
    *out_h   = H;
    return 0;
}

/* ====================================================================== */
/* 8. Sub-commands                                                        */
/* ====================================================================== */

static int do_title(const char *path, const char *out_png) {
    uint8_t *rgb;
    int W, H;
    if (load_title_rgb(path, &rgb, &W, &H) != 0) {
        fprintf(stderr, "[title] failed to load %s\n", path);
        return 1;
    }
    int rc = write_png(out_png, W, H, rgb);
    printf("TITLE %s: %dx%d -> %s\n", path, W, H, out_png);
    free(rgb);
    return rc;
}

static int do_show(const char *glz_path) {
    GLZ g;
    if (glz_load(glz_path, &g) != 0) {
        fprintf(stderr, "[show] glz_load failed\n");
        return 1;
    }
    printf("show: %s  %dx%d\n", glz_path, g.width, g.height);
    if (display_init("SADES - show", g.width, g.height) != 0) {
        glz_free(&g);
        return 1;
    }
    display_show_rgb(g.rgb, g.width, g.height);
    for (;;) {
        int k = display_poll();
        if (k == SDLK_ESCAPE || k == SDLK_q) break;
        SDL_Delay(20);
    }
    display_quit();
    font_quit();
    glz_free(&g);
    return 0;
}

/* Full-screen blit used by the title splash. */
static void blit_glz_full(const GLZ *g) {
    for (int y = 0; y < g_fb_h; y++) {
        int sy = (y * g->height) / g_fb_h;
        for (int x = 0; x < g_fb_w; x++) {
            int sx = (x * g->width) / g_fb_w;
            memcpy(&g_fb[(y * g_fb_w + x) * 3],
                   &g->rgb[(sy * g->width + sx) * 3], 3);
        }
    }
}

/* Load TITLE.SYS, center it on black and wait for a key.
 * Returns 1 if shown, 0 if the file was missing or malformed. */
static int show_engine_logo(void) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/TITLE.SYS", g_glz_dir);

    uint8_t *rgb;
    int W, H;
    if (load_title_rgb(path, &rgb, &W, &H) != 0) {
        fprintf(stderr, "[LOGO] %s not found, skipping\n", path);
        return 0;
    }

    memset(g_fb, 0, (size_t)g_fb_w * g_fb_h * 3);
    int off_x = (g_fb_w - W) / 2;
    int off_y = (g_fb_h - H) / 2;
    for (int y = 0; y < H; y++) {
        int dy = off_y + y;
        if (dy < 0 || dy >= g_fb_h) continue;
        for (int x = 0; x < W; x++) {
            int dx = off_x + x;
            if (dx < 0 || dx >= g_fb_w) continue;
            memcpy(&g_fb[(dy * g_fb_w + dx) * 3], &rgb[(y * W + x) * 3], 3);
        }
    }
    free(rgb);
    fprintf(stderr, "[LOGO] %s %dx%d\n", path, W, H);
    display_show_rgb(g_fb, g_fb_w, g_fb_h);
    sdl_wait_key_raw();
    return 1;
}

/* Load the game title screen (P00100 or P00099) into g_fb and g_bg_layer. */
static const char *load_game_title(const char *explicit_path) {
    const char *tp = explicit_path;
    char buf[1024];
    if (!tp) {
        snprintf(buf, sizeof(buf), "%s/P00100.GLZ", g_glz_dir);
        if (access(buf, R_OK) == 0) {
            tp = buf;
        } else {
            snprintf(buf, sizeof(buf), "%s/P00099.GLZ", g_glz_dir);
            if (access(buf, R_OK) == 0) tp = buf;
        }
    }
    if (!tp) {
        /* Fallback: solid dark purple, matches the typical palette. */
        for (int i = 0; i < g_fb_w * g_fb_h * 3; i += 3) {
            g_fb[i] = 0x20; g_fb[i + 1] = 0x00; g_fb[i + 2] = 0x40;
        }
        return NULL;
    }
    GLZ tt;
    if (glz_load(tp, &tt) == 0) {
        blit_glz_full(&tt);
        if (g_bg_idx) {
            for (int y = 0; y < g_fb_h; y++) {
                int sy = (y * tt.height) / g_fb_h;
                for (int x = 0; x < g_fb_w; x++) {
                    int sx = (x * tt.width) / g_fb_w;
                    g_bg_idx[y * g_fb_w + x] = tt.indices[sy * tt.width + sx];
                }
            }
            if (tt.pal_flag) {
                memcpy(g_bg_palette,      tt.palette, sizeof(g_bg_palette));
                memcpy(g_bg_orig_palette, tt.palette, sizeof(g_bg_orig_palette));
                g_gdc_palette_ready = 1;
            }
        }
        glz_free(&tt);
    }
    return tp;
}

static int do_play(const char *sce_path, const char *m00_path,
                   const char *title_path, int trace) {
    /* The GLZ directory is the directory of the .SCE file. */
    char sce_copy[1024];
    snprintf(sce_copy, sizeof(sce_copy), "%s", sce_path);
    snprintf(g_glz_dir, sizeof(g_glz_dir), "%s", dirname(sce_copy));

    if (font_init(NULL, 16) != 0) {
        fprintf(stderr, "[play] font_init failed\n");
        /* Not fatal: we can still show graphics, just no text. */
    }

    /* Environment overrides. */
    { const char *e = getenv("SADES_DX");         if (e) g_off_dx          = atoi(e); }
    { const char *e = getenv("SADES_DY");         if (e) g_off_dy          = atoi(e); }
    { const char *e = getenv("SADES_CK_R");       if (e) g_ck_r            = atoi(e); }
    { const char *e = getenv("SADES_CK_G");       if (e) g_ck_g            = atoi(e); }
    { const char *e = getenv("SADES_CK_B");       if (e) g_ck_b            = atoi(e); }
    { const char *e = getenv("SADES_ANIM_MS");    if (e) g_anim_ms         = atoi(e); }
    { const char *e = getenv("SADES_ANIM");       if (e) g_anim_enable     = atoi(e); }
    /* SADES_FORCE_TINT / SADES_TINT_TIMEOUT are no longer used. */

    printf("[offset] dx=%d dy=%d anim_ms=%d anim=%d\n",
           g_off_dx, g_off_dy, g_anim_ms, g_anim_enable);
    printf("=== SADES play ===\nSCE:  %s\nM00:  %s\nGLZ:  %s\n\n",
           sce_path, m00_path, g_glz_dir);

    g_fb = calloc((size_t)g_fb_w * g_fb_h * 3, 1);
    if (!g_fb) return 1;
    g_bg_idx = calloc((size_t)g_fb_w * g_fb_h, 1);
    if (!g_bg_idx) { free(g_fb); g_fb = NULL; return 1; }

    if (display_init("SADES", g_fb_w, g_fb_h) != 0) {
        free(g_bg_idx); g_bg_idx = NULL;
        free(g_fb); g_fb = NULL;
        return 1;
    }
    SDL_ShowCursor(SDL_DISABLE);

    /* ---- Splash sequence ---------------------------------------------
     *   1. Draw the game title (P00100 / P00099) and stash it as the bg.
     *   2. Show TITLE.SYS over it (if present) and wait for a key.
     *   3. Restore the title from the background layer and wait for a key.
     *   4. Enter the VM.
     */
    const char *loaded_title = load_game_title(title_path);
    if (loaded_title) printf("[TITULO] %s\n", loaded_title);

    show_engine_logo();

    /* Restore the game title; the logo overwrote g_fb with black. */
    if (g_bg_idx) {
        for (int i = 0; i < g_fb_w * g_fb_h; i++) {
            const uint8_t *src = g_bg_palette[g_bg_idx[i]];
            g_fb[i*3+0] = src[0];
            g_fb[i*3+1] = src[1];
            g_fb[i*3+2] = src[2];
        }
    }
    display_show_rgb(g_fb, g_fb_w, g_fb_h);
    printf("Press any key to start...\n");
    fflush(stdout);
    sdl_wait_key_raw();

    /* ---- Load data files -------------------------------------------- */
    SCE sce;
    M00 m00;
    if (sce_load(sce_path, &sce) != 0) {
        fprintf(stderr, "[play] sce_load failed\n");
        goto fail;
    }
    if (m00_load(m00_path, &m00) != 0) {
        fprintf(stderr, "[play] m00_load failed\n");
        sce_free(&sce);
        goto fail;
    }
    if (sce.nslots == 0 || sce.bc_start >= sce.size) {
        fprintf(stderr, "[play] .SCE has no valid slots\n");
        sce_free(&sce);
        m00_free(&m00);
        goto fail;
    }

    /* ---- Run ---------------------------------------------------------- */
    VM vm;
    vm_init(&vm, &sce, &m00);
    vm.trace         = trace;
    vm.wait_on_drop  = 1;
    vm.wait_fn       = sdl_wait_key_vm;
    vm.delay_fn      = sdl_delay_vm;
    vm.call_fn       = on_call;
    vm.call_ind_fn   = on_call_ind;
    vm.load_glz_fn   = on_load_glz;
    vm.print_clear_fn = on_print_clear;

    printf("\n=== %s ===\nnslots=%u  bc_start=0x%zX\n",
           sce_path, sce.nslots, sce.bc_start);
    fflush(stdout);

    vm_run(&vm, 0);

    printf("\n--- %d ops, %d CALLs, %d wait_keys, %d unknown ---\n",
           vm.steps, vm.call_count, vm.wait_count, vm.unknown_ops);
    printf("Press any key to exit...\n");
    sdl_wait_key_raw();

    sce_free(&sce);
    m00_free(&m00);

    display_quit();
    free(g_bg_idx); g_bg_idx = NULL;
    free(g_fb);     g_fb     = NULL;
    return 0;

fail:
    display_quit();
    free(g_bg_idx); g_bg_idx = NULL;
    free(g_fb);     g_fb     = NULL;
    return 1;
}

/* ====================================================================== */
/* 9. main()                                                              */
/* ====================================================================== */

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 1; }

    const char *cmd = argv[1];

    if (strcmp(cmd, "glz") == 0) {
        if (argc < 4) { usage(argv[0]); return 1; }

        /* Optional external palette: --pal <file>
         * Accepts either a .GLZ (palette at 0x10 if pal_flag set)
         * or a raw .PAL (64 bytes: 16 RGB triples + padding). */
        if (argc >= 6 && strcmp(argv[4], "--pal") == 0) {
            FILE *f = fopen(argv[5], "rb");
            if (!f) {
                fprintf(stderr, "[glz] cannot open %s\n", argv[5]);
                return 1;
            }
            uint8_t buf[0x40];
            size_t got = fread(buf, 1, 0x40, f);
            fclose(f);
            if (got < 0x40) {
                fprintf(stderr, "[glz] %s too small (%zu bytes)\n",
                        argv[5], got);
                return 1;
            }

            /* Heuristic: if bytes 0..5 are all 0xFF, it's a .GLZ
             * (use its embedded palette at 0x10). Otherwise treat
             * as raw .PAL where the palette starts at byte 0. */
            int is_glz = (buf[0] == 0xFF && buf[1] == 0xFF && buf[2] == 0xFF
                       && buf[3] == 0xFF && buf[4] == 0xFF && buf[5] == 0xFF);
            const uint8_t *pal_src = is_glz ? (buf + 0x10) : buf;

            if (is_glz) {
                uint16_t pf = buf[0x0E] | (buf[0x0F] << 8);
                if (!pf) {
                    fprintf(stderr, "[glz] %s has no embedded palette\n",
                            argv[5]);
                    return 1;
                }
                glz_set_external_palette(pal_src);
            } else {
                /* raw .PAL = 64 bytes RGB32 (16 x R,G,B,pad). */
                glz_set_external_palette_rgb32(buf);
            }
            fprintf(stderr, "[glz] external palette from %s (%s)\n",
                    argv[5], is_glz ? "GLZ" : "raw PAL (RGB32)");
        }

        GLZ g;
        if (glz_load(argv[2], &g) != 0) {
            fprintf(stderr, "[glz] load failed\n");
            return 1;
        }
        printf("OK %s: %dx%d  pal=%s\n",
               argv[2], g.width, g.height,
               g.pal_flag ? "embedded" : "inherited/gray");
        int rc = write_png(argv[3], g.width, g.height, g.rgb);
        glz_free(&g);
        return rc;
    }

    if (strcmp(cmd, "info") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        GLZ g;
        if (glz_load(argv[2], &g) != 0) {
            fprintf(stderr, "[info] load failed\n");
            return 1;
        }
        printf("%s: bpr=%u rows=%u pal_flag=%u -> %dx%d\n",
               argv[2], g.bpr, g.rows, g.pal_flag, g.width, g.height);
        if (g.pal_flag) {
            for (int i = 0; i < 16; i++) {
                printf("  [%2d] #%02X%02X%02X\n", i,
                       g.palette[i][0], g.palette[i][1], g.palette[i][2]);
            }
        }
        glz_free(&g);
        return 0;
    }

    if (strcmp(cmd, "title") == 0) {
        if (argc < 4) { usage(argv[0]); return 1; }
        return do_title(argv[2], argv[3]);
    }

    if (strcmp(cmd, "sce") == 0) {
        if (argc < 4) { usage(argv[0]); return 1; }
        SCE sce; M00 m00;
        if (sce_load(argv[2], &sce) != 0) {
            fprintf(stderr, "[sce] load failed\n");
            return 1;
        }
        if (m00_load(argv[3], &m00) != 0) {
            fprintf(stderr, "[sce] M00 load failed\n");
            sce_free(&sce);
            return 1;
        }
        if (sce.nslots == 0 || sce.bc_start >= sce.size) {
            fprintf(stderr, "[sce] no valid slots\n");
            sce_free(&sce); m00_free(&m00);
            return 1;
        }

        int trace = 0;
        for (int i = 4; i < argc; i++) {
            if (strcmp(argv[i], "--trace")   == 0) trace      = 1;
            if (strcmp(argv[i], "--bg-hook") == 0) g_bg_hook  = 1;
            if (strcmp(argv[i], "--explore") == 0) g_explore  = 1;
        }

        printf("=== %s ===\nnslots=%u  bc_start=0x%zX  bytes_bytecode=%zu\n",
               argv[2], sce.nslots, sce.bc_start, sce.size - sce.bc_start);

        VM vm;
        vm_init(&vm, &sce, &m00);
        vm.trace          = trace;
        vm.wait_on_drop   = 1;
        vm.wait_fn        = batch_wait_fn;
        vm.call_ind_fn    = on_call_ind;
        vm.load_glz_fn    = on_load_glz;
        vm.print_clear_fn = on_print_clear;

        vm_run(&vm, 0);

        printf("\n--- %d ops, %d CALLs, %d wait_keys, %d unknown ---\n",
               vm.steps, vm.call_count, vm.wait_count, vm.unknown_ops);

        sce_free(&sce);
        m00_free(&m00);
        return 0;
    }

    if (strcmp(cmd, "show") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        return do_show(argv[2]);
    }

    if (strcmp(cmd, "play") == 0) {
        if (argc < 4) { usage(argv[0]); return 1; }
        const char *title = NULL;
        int trace = 0;
        for (int i = 4; i < argc; i++) {
            if (strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
                title = argv[i + 1];
            }
            if (strcmp(argv[i], "--trace")   == 0) trace     = 1;
            if (strcmp(argv[i], "--bg-hook") == 0) g_bg_hook = 1;
        }
        return do_play(argv[2], argv[3], title, trace);
    }

    usage(argv[0]);
    return 1;
}