/*
 * SADES Engine — a reimplementation
 * Copyright (c) 2026 Kalizkal
 * MIT License — see LICENSE
 */

#include "display.h"

#include <SDL2/SDL.h>
#include <stdio.h>

/* --------------------------------------------------------------------- */
/* Module state                                                          */
/* --------------------------------------------------------------------- */

static SDL_Window   *g_win = NULL;
static SDL_Renderer *g_ren = NULL;
static SDL_Texture  *g_tex = NULL;
static int           g_w   = 0;
static int           g_h   = 0;

/* --------------------------------------------------------------------- */
/* Public API                                                            */
/* --------------------------------------------------------------------- */

int display_init(const char *title, int w, int h) {
    if (g_win) {
        fprintf(stderr, "[display] already initialized\n");
        return -1;
    }

    /*
     * Force the software renderer. Termux:X11 has no GPU surface, so
     * EGL and ZINK both fail at startup; setting this hint before
     * SDL_Init is the only reliable way to skip that path. On desktop
     * it's a no-op and at 640x400 the software blit is plenty fast.
     *
     * The other hints disable things that don't play well with a
     * single fixed-size window: batching, the WM compositor bypass,
     * and auto-minimize on focus loss.
     */
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    SDL_SetHint(SDL_HINT_RENDER_BATCHING, "0");
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[display] SDL_Init: %s\n", SDL_GetError());
        return -1;
    }

    {
        const char *drv = SDL_GetCurrentVideoDriver();
        fprintf(stderr, "[display] video driver = %s\n",
                drv ? drv : "(null)");
    }

    g_w = w;
    g_h = h;

    g_win = SDL_CreateWindow(title,
                             SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED,
                             w, h,
                             SDL_WINDOW_SHOWN);
    if (!g_win) {
        fprintf(stderr, "[display] SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return -1;
    }

    /* Some window managers (Termux:X11 is one) create the window
     * off-screen and never map it unless we ask explicitly. */
    SDL_ShowWindow(g_win);
    SDL_RaiseWindow(g_win);

    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
    if (!g_ren) {
        fprintf(stderr, "[display] SDL_CreateRenderer(SOFTWARE): %s\n",
                SDL_GetError());
        SDL_DestroyWindow(g_win);
        g_win = NULL;
        SDL_Quit();
        return -1;
    }
    {
        SDL_RendererInfo ri;
        if (SDL_GetRendererInfo(g_ren, &ri) == 0) {
            fprintf(stderr, "[display] renderer = %s\n", ri.name);
        }
    }

    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGB24,
                              SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!g_tex) {
        fprintf(stderr, "[display] SDL_CreateTexture: %s\n",
                SDL_GetError());
        SDL_DestroyRenderer(g_ren);
        SDL_DestroyWindow(g_win);
        g_ren = NULL;
        g_win = NULL;
        SDL_Quit();
        return -1;
    }

    /* Push one black frame so the WM actually maps the window. Without
     * this, the first real frame can be delayed by a few hundred ms on
     * some systems. */
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
    SDL_RenderClear(g_ren);
    SDL_RenderPresent(g_ren);
    return 0;
}

void display_show_rgb(const uint8_t *rgb, int w, int h) {
    if (!g_tex || !rgb) return;

    SDL_UpdateTexture(g_tex, NULL, rgb, w * 3);
    SDL_RenderClear(g_ren);

    if (w == g_w && h == g_h) {
        SDL_RenderCopy(g_ren, g_tex, NULL, NULL);
    } else {
        SDL_Rect dst = {0, 0, g_w, g_h};
        SDL_RenderCopy(g_ren, g_tex, NULL, &dst);
    }
    SDL_RenderPresent(g_ren);
}

int display_poll(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) return SDLK_ESCAPE;
        if (e.type == SDL_KEYDOWN) {
            return e.key.keysym.sym;
        }
    }
    return -1;
}

void display_quit(void) {
    if (g_tex) { SDL_DestroyTexture(g_tex);   g_tex = NULL; }
    if (g_ren) { SDL_DestroyRenderer(g_ren);  g_ren = NULL; }
    if (g_win) { SDL_DestroyWindow(g_win);    g_win = NULL; }
    g_w = g_h = 0;
    SDL_Quit();
}