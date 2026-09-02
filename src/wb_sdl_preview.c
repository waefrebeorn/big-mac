/* wb_sdl_preview.c — SDL2 window for real-time video preview
 * R090: Actually displays pixels on screen via SDL.
 * Uses existing wb_video_frame_to_texture + wb_video_blit_scaled from wb_video.c.
 * Can run standalone or be called from wb_daw.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <SDL.h>
#include "wbus/wbus_compositor.h"

#define PREV_W 960
#define PREV_H 540

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    int running;
    wb_node *root_node;
    double fps;
    double current_time;
    double duration;
    int loop;
    int playing;
    int w, h;
} sdl_preview;

/* Global for signal handling */
static sdl_preview *g_prev = NULL;

int wb_sdl_preview_init(sdl_preview *p, wb_node *root, int w, int h, double fps) {
    if (!p || !root) return -1;
    memset(p, 0, sizeof(*p));
    p->root_node = root;
    p->w = w > 0 ? w : PREV_W;
    p->h = h > 0 ? h : PREV_H;
    p->fps = fps > 0 ? fps : 30.0;
    p->duration = 10.0;
    p->loop = 1;
    p->playing = 0;
    p->running = 1;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return -1;
    }

    p->window = SDL_CreateWindow("Big Mac Video Preview",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        p->w, p->h, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!p->window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return -1;
    }

    p->renderer = SDL_CreateRenderer(p->window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!p->renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return -1;
    }

    g_prev = p;
    return 0;
}

void wb_sdl_preview_run(sdl_preview *p) {
    if (!p || !p->window) return;

    uint32_t frame_delay = (uint32_t)(1000.0 / p->fps);
    uint32_t last_time = SDL_GetTicks();
    uint32_t accum = 0;

    while (p->running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_QUIT:
                    p->running = 0;
                    break;
                case SDL_KEYDOWN:
                    switch (e.key.keysym.sym) {
                        case SDLK_ESCAPE:
                        case SDLK_q:
                            p->running = 0;
                            break;
                        case SDLK_SPACE:
                            p->playing = !p->playing;
                            break;
                        case SDLK_LEFT:
                            p->current_time -= 1.0;
                            if (p->current_time < 0) p->current_time = 0;
                            break;
                        case SDLK_RIGHT:
                            p->current_time += 1.0;
                            break;
                        case SDLK_HOME:
                            p->current_time = 0.0;
                            break;
                        case SDLK_l:
                            p->loop = !p->loop;
                            break;
                    }
                    break;
            }
        }

        /* Render frame */
        SDL_SetRenderDrawColor(p->renderer, 20, 20, 30, 255);
        SDL_RenderClear(p->renderer);

        if (p->root_node) {
            wb_frame *f = p->root_node->pull(p->root_node, p->current_time,
                                              0, 0, p->w, p->h, 0);
            if (f && f->px && f->w > 0 && f->h > 0) {
                /* Convert to RGBA bytes */
                int px_count = f->w * f->h;
                uint8_t *rgba = (uint8_t *)malloc(px_count * 4);
                if (rgba) {
                    for (int i = 0; i < px_count; i++) {
                        rgba[i*4+0] = (uint8_t)(f->px[i].r < 0 ? 0 : (f->px[i].r > 255 ? 255 : f->px[i].r));
                        rgba[i*4+1] = (uint8_t)(f->px[i].g < 0 ? 0 : (f->px[i].g > 255 ? 255 : f->px[i].g));
                        rgba[i*4+2] = (uint8_t)(f->px[i].b < 0 ? 0 : (f->px[i].b > 255 ? 255 : f->px[i].b));
                        rgba[i*4+3] = (uint8_t)(f->px[i].a < 0 ? 0 : (f->px[i].a > 255 ? 255 : f->px[i].a));
                    }
                    SDL_Texture *tex = SDL_CreateTexture(p->renderer,
                        SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING,
                        f->w, f->h);
                    if (tex) {
                        SDL_UpdateTexture(tex, NULL, rgba, f->w * 4);
                        SDL_Rect dst = { 0, 0, p->w, p->h };
                        SDL_RenderCopy(p->renderer, tex, NULL, &dst);
                        SDL_DestroyTexture(tex);
                    }
                    free(rgba);
                }
                wb_frame_free(f);
            }
        }

        /* Draw transport overlay */
        /* Progress bar at bottom */
        if (p->duration > 0) {
            float progress = (float)(p->current_time / p->duration);
            if (progress > 1.0f) progress = 1.0f;
            SDL_Rect bar_bg = { 10, p->h - 30, p->w - 20, 10 };
            SDL_SetRenderDrawColor(p->renderer, 60, 60, 80, 255);
            SDL_RenderFillRect(p->renderer, &bar_bg);
            SDL_Rect bar_fg = { 10, p->h - 30, (int)((p->w - 20) * progress), 10 };
            SDL_SetRenderDrawColor(p->renderer, 0, 180, 255, 255);
            SDL_RenderFillRect(p->renderer, &bar_fg);
        }

        SDL_RenderPresent(p->renderer);

        /* Advance time */
        if (p->playing) {
            uint32_t now = SDL_GetTicks();
            uint32_t delta = now - last_time;
            last_time = now;
            accum += delta;
            if (accum >= frame_delay) {
                p->current_time += (double)accum / 1000.0;
                accum = 0;
                if (p->current_time >= p->duration) {
                    if (p->loop) {
                        p->current_time = 0.0;
                    } else {
                        p->current_time = p->duration;
                        p->playing = 0;
                    }
                }
            }
        }

        SDL_Delay(1);
    }
}

void wb_sdl_preview_destroy(sdl_preview *p) {
    if (!p) return;
    if (p->renderer) SDL_DestroyRenderer(p->renderer);
    if (p->window) SDL_DestroyWindow(p->window);
    g_prev = NULL;
}

/* Standalone main — run with: build/wb_sdl_preview <node_type> */
#ifdef BUILD_STANDALONE
int main(int argc, char **argv) {
    /* Create a simple test pattern source node */
    wb_node *root = wb_node_source_color(0.2f, 0.5f, 0.9f, 1.0f, 640, 480);
    if (!root) {
        fprintf(stderr, "failed to create source node\n");
        return 1;
    }

    sdl_preview prev;
    if (wb_sdl_preview_init(&prev, root, 960, 540, 30.0) != 0) {
        fprintf(stderr, "preview init failed\n");
        return 1;
    }

    wb_sdl_preview_run(&prev);
    wb_sdl_preview_destroy(&prev);

    if (root->free) root->free(root);
    SDL_Quit();
    return 0;
}
#endif
