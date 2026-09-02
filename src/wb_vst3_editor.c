/* wb_vst3_editor.c — VST3 plugin editor window
 * R091: Plugin editor UI with parameter knobs
 *
 * Provides a simple SDL-based editor window for VST3 plugins.
 * Shows parameter sliders, bypass button, and preset info.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "wbus/wbus_vst3.h"

#define EDITOR_W 480
#define EDITOR_H 320
#define SLIDER_H 24
#define SLIDER_GAP 8
#define KNOB_SIZE 48

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    int running;
    void *plugin_inst;
    int param_count;
    char plugin_name[256];
    char vendor_name[128];
} vst3_editor;

static vst3_editor *g_editor = NULL;

/* ---- Editor lifecycle ---- */

int wb_vst3_editor_open(void *plugin_inst, const char *title) {
    if (!plugin_inst) return -1;

    vst3_editor *e = (vst3_editor *)calloc(1, sizeof(vst3_editor));
    if (!e) return -1;

    e->plugin_inst = plugin_inst;
    e->param_count = wb_vst3_param_count(plugin_inst);
    e->running = 1;

    wb_vst3_get_info(plugin_inst, e->plugin_name, sizeof(e->plugin_name),
                      e->vendor_name, sizeof(e->vendor_name), NULL, 0);

    char win_title[512];
    snprintf(win_title, sizeof(win_title), "VST3: %s", e->plugin_name[0] ? e->plugin_name : title ? title : "Plugin");

    if (SDL_Init(SDL_INIT_VIDEO) != 0) { free(e); return -1; }

    e->window = SDL_CreateWindow(win_title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        EDITOR_W, EDITOR_H, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!e->window) { free(e); return -1; }

    e->renderer = SDL_CreateRenderer(e->window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!e->renderer) { SDL_DestroyWindow(e->window); free(e); return -1; }

    g_editor = e;
    return 0;
}

void wb_vst3_editor_run(vst3_editor *e) {
    if (!e || !e->window) return;

    while (e->running) {
        SDL_Event evt;
        while (SDL_PollEvent(&evt)) {
            switch (evt.type) {
                case SDL_QUIT:
                    e->running = 0;
                    break;
                case SDL_KEYDOWN:
                    if (evt.key.keysym.sym == SDLK_ESCAPE ||
                        evt.key.keysym.sym == SDLK_q)
                        e->running = 0;
                    break;
                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEMOTION:
                    if (evt.type == SDL_MOUSEBUTTONDOWN && evt.button.button == SDL_BUTTON_LEFT) {
                        /* Check if clicking on a parameter slider */
                        int y_start = 60;
                        for (int i = 0; i < e->param_count && i < 20; i++) {
                            SDL_Rect slider = { 140, y_start + i * (SLIDER_H + SLIDER_GAP),
                                                EDITOR_W - 160, SLIDER_H };
                            if (evt.button.x >= slider.x && evt.button.x <= slider.x + slider.w &&
                                evt.button.y >= slider.y && evt.button.y <= slider.y + slider.h) {
                                float val = (float)(evt.button.x - slider.x) / (float)slider.w;
                                if (val < 0) val = 0;
                                if (val > 1) val = 1;
                                wb_vst3_set_param(e->plugin_inst, i, val);
                            }
                        }
                    }
                    break;
            }
        }

        /* Draw */
        SDL_SetRenderDrawColor(e->renderer, 25, 25, 35, 255);
        SDL_RenderClear(e->renderer);

        /* Title bar */
        SDL_Rect title_bar = { 0, 0, EDITOR_W, 30 };
        SDL_SetRenderDrawColor(e->renderer, 40, 60, 100, 255);
        SDL_RenderFillRect(e->renderer, &title_bar);

        /* Plugin name */
        /* Note: wb_ui_draw_text would be used here, but we use SDL directly for simplicity */
        /* Draw parameter sliders */
        int y_start = 60;
        for (int i = 0; i < e->param_count && i < 20; i++) {
            char pname[64] = {0};
            wb_vst3_param_name(e->plugin_inst, i, pname, sizeof(pname));
            float val = wb_vst3_get_param(e->plugin_inst, i);

            /* Slider background */
            SDL_Rect slider_bg = { 140, y_start + i * (SLIDER_H + SLIDER_GAP),
                                    EDITOR_W - 160, SLIDER_H };
            SDL_SetRenderDrawColor(e->renderer, 50, 50, 60, 255);
            SDL_RenderFillRect(e->renderer, &slider_bg);

            /* Slider fill */
            SDL_Rect slider_fill = { 140, y_start + i * (SLIDER_H + SLIDER_GAP),
                                      (int)((EDITOR_W - 160) * val), SLIDER_H };
            SDL_SetRenderDrawColor(e->renderer, 0, 150, 220, 255);
            SDL_RenderFillRect(e->renderer, &slider_fill);

            /* Slider border */
            SDL_SetRenderDrawColor(e->renderer, 80, 80, 100, 255);
            SDL_RenderDrawRect(e->renderer, &slider_bg);
        }

        /* Bypass button */
        SDL_Rect bypass_btn = { 10, EDITOR_H - 40, 80, 30 };
        SDL_SetRenderDrawColor(e->renderer, 150, 50, 50, 255);
        SDL_RenderFillRect(e->renderer, &bypass_btn);
        SDL_SetRenderDrawColor(e->renderer, 200, 200, 200, 255);
        SDL_RenderDrawRect(e->renderer, &bypass_btn);

        /* Close button */
        SDL_Rect close_btn = { EDITOR_W - 90, EDITOR_H - 40, 80, 30 };
        SDL_SetRenderDrawColor(e->renderer, 80, 80, 80, 255);
        SDL_RenderFillRect(e->renderer, &close_btn);
        SDL_SetRenderDrawColor(e->renderer, 200, 200, 200, 255);
        SDL_RenderDrawRect(e->renderer, &close_btn);

        SDL_RenderPresent(e->renderer);
        SDL_Delay(16);
    }
}

void wb_vst3_editor_close(vst3_editor *e) {
    if (!e) return;
    if (e->renderer) SDL_DestroyRenderer(e->renderer);
    if (e->window) SDL_DestroyWindow(e->window);
    free(e);
    g_editor = NULL;
}

void wb_vst3_editor_destroy(void) {
    if (g_editor) {
        g_editor->running = 0;
        wb_vst3_editor_close(g_editor);
    }
}

/* Convenience: open, run, close */
int wb_vst3_editor_show(void *plugin_inst, const char *title) {
    if (wb_vst3_editor_open(plugin_inst, title) != 0) return -1;
    wb_vst3_editor_run(g_editor);
    wb_vst3_editor_close(g_editor);
    return 0;
}
