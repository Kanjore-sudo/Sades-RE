/*
 * SADES Engine — a reimplementation
 * Copyright (c) 2026 Kalizkal
 * MIT License — see LICENSE
 */

#ifndef SADES_DISPLAY_H
#define SADES_DISPLAY_H

#include <stdint.h>

/*
 * Minimal SDL2 wrapper.
 *
 * The engine renders everything to one RGB24 framebuffer at 640x400,
 * the PC-98 resolution. This module owns the window, the renderer and
 * the streaming texture, and pushes complete frames to the screen.
 *
 * The software renderer is forced because Termux:X11 has no GPU access
 * from SDL. On desktop it makes no difference at this size.
 *
 * All functions are main-thread only.
 */

/* Create the window and prepare the streaming texture.
 * Returns 0 on success, -1 on error. On error, SDL is fully shut down. */
int  display_init(const char *title, int w, int h);

/* Upload an RGB24 framebuffer (w*h*3 bytes, row-major, no padding) and
 * present it. If w/h differ from the initial window size, the image is
 * scaled to fit. Safe to call at any time after display_init(). */
void display_show_rgb(const uint8_t *rgb, int w, int h);

/* Poll one pending SDL event.
 * Returns the SDLK_* code of a key-down event, SDLK_ESCAPE if the
 * window was closed, or -1 if no event is pending. Caller loops until
 * it gets -1 to drain the queue. */
int  display_poll(void);

/* Destroy the window, renderer, texture and quit SDL.
 * Safe to call multiple times. */
void display_quit(void);

#endif /* SADES_DISPLAY_H */