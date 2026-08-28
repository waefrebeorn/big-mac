/* wb_text_animate.c — text animation system for kinetic typography.
 *
 * R077: Animated text overlays for music videos, memes, lyric videos.
 *
 * Animation types:
 *   0: Fade in/out
 *   1: Slide from left
 *   2: Slide from right
 *   3: Slide from bottom
 *   4: Bounce
 *   5: Typewriter (character by character)
 *   6: Pop (scale up with bounce)
 *   7: Wave (per-character sine offset)
 *   8: Glitch (random position jitter)
 *   9: Karaoke (fill color progress)
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

#define MAX_TEXT_ANIM 32

typedef enum {
    ANIM_FADE = 0,
    ANIM_SLIDE_LEFT,
    ANIM_SLIDE_RIGHT,
    ANIM_SLIDE_UP,
    ANIM_BOUNCE,
    ANIM_TYPEWRITER,
    ANIM_POP,
    ANIM_WAVE,
    ANIM_GLITCH,
    ANIM_KARAOKE
} text_anim_type_t;

typedef struct {
    char           text[256];
    text_anim_type_t type;
    float          start_time;
    float          duration;
    float          delay;
    /* Output parameters (computed per-frame) */
    float          x_offset;
    float          y_offset;
    float          opacity;
    float          scale;
    float          rotation;
    int            chars_visible;  /* For typewriter */
    float          fill_progress;  /* For karaoke */
} text_anim_t;

typedef struct {
    uint32_t sr;
    text_anim_t anims[MAX_TEXT_ANIM];
    int      num_anims;
} wb_text_animate_inst;

void *wb_text_animate_create(uint32_t sr) {
    wb_text_animate_inst *ta = (wb_text_animate_inst *)calloc(1, sizeof(*ta));
    if (!ta) return NULL;
    ta->sr = sr;
    return ta;
}

void wb_text_animate_destroy(void *inst) { free(inst); }

/* Add a text animation. Returns index or -1. */
int wb_text_animate_add(void *inst, const char *text, text_anim_type_t type,
                         float start, float duration) {
    wb_text_animate_inst *ta = (wb_text_animate_inst *)inst;
    if (!ta || ta->num_anims >= MAX_TEXT_ANIM) return -1;

    int idx = ta->num_anims++;
    strncpy(ta->anims[idx].text, text, 255);
    ta->anims[idx].type = type;
    ta->anims[idx].start_time = start;
    ta->anims[idx].duration = duration > 0.1f ? duration : 0.5f;
    return idx;
}

/* Easing functions */
static float ease_out_bounce(float t) {
    if (t < 1.0f / 2.75f) {
        return 7.5625f * t * t;
    } else if (t < 2.0f / 2.75f) {
        t -= 1.5f / 2.75f;
        return 7.5625f * t * t + 0.75f;
    } else if (t < 2.5f / 2.75f) {
        t -= 2.25f / 2.75f;
        return 7.5625f * t * t + 0.9375f;
    } else {
        t -= 2.625f / 2.75f;
        return 7.5625f * t * t + 0.984375f;
    }
}

static float ease_out_back(float t) {
    float c1 = 1.70158f;
    float c3 = c1 + 1.0f;
    return 1.0f + c3 * powf(t - 1.0f, 3.0f) + c1 * powf(t - 1.0f, 2.0f);
}

static float ease_in_out_cubic(float t) {
    return t < 0.5f ? 4.0f * t * t * t : 1.0f - powf(-2.0f * t + 2.0f, 3.0f) / 2.0f;
}

/* Update all animations for current time. */
void wb_text_animate_update(wb_text_animate_inst *ta, float time_sec) {
    if (!ta) return;

    for (int i = 0; i < ta->num_anims; i++) {
        text_anim_t *anim = &ta->anims[i];

        /* Time relative to this animation */
        float t = (time_sec - anim->start_time) / anim->duration;

        /* Reset defaults */
        anim->x_offset = 0;
        anim->y_offset = 0;
        anim->opacity = 1.0f;
        anim->scale = 1.0f;
        anim->rotation = 0;
        anim->chars_visible = (int)strlen(anim->text);
        anim->fill_progress = 1.0f;

        if (t < 0 || t > 1.0f) {
            /* Before or after animation */
            if (t < 0) {
                anim->opacity = 0;
                anim->scale = 0.5f;
            }
            continue;
        }

        switch (anim->type) {
        case ANIM_FADE:
            anim->opacity = ease_in_out_cubic(t);
            break;

        case ANIM_SLIDE_LEFT:
            anim->x_offset = (1.0f - ease_out_back(t)) * -200.0f;
            anim->opacity = t < 0.1f ? t / 0.1f : 1.0f;
            break;

        case ANIM_SLIDE_RIGHT:
            anim->x_offset = (1.0f - ease_out_back(t)) * 200.0f;
            anim->opacity = t < 0.1f ? t / 0.1f : 1.0f;
            break;

        case ANIM_SLIDE_UP:
            anim->y_offset = (1.0f - ease_out_bounce(t)) * 100.0f;
            anim->opacity = t < 0.1f ? t / 0.1f : 1.0f;
            break;

        case ANIM_BOUNCE:
            anim->y_offset = -ease_out_bounce(t) * 20.0f;
            anim->scale = 0.5f + ease_out_bounce(t) * 0.5f;
            break;

        case ANIM_TYPEWRITER:
            anim->chars_visible = (int)(t * (float)strlen(anim->text));
            break;

        case ANIM_POP:
            anim->scale = ease_out_back(t) * 1.2f;
            if (anim->scale > 1.0f) anim->scale = 1.0f;
            anim->opacity = t < 0.2f ? t / 0.2f : 1.0f;
            break;

        case ANIM_WAVE:
            anim->y_offset = sinf(t * 6.28f * 2) * 10.0f;
            break;

        case ANIM_GLITCH:
            if (t < 0.8f) {
                anim->x_offset = ((rand() % 20) - 10) * (1.0f - t);
                anim->opacity = (rand() % 100) < 90 ? 1.0f : 0.5f;
            }
            break;

        case ANIM_KARAOKE:
            anim->fill_progress = ease_in_out_cubic(t);
            break;

        default:
            break;
        }
    }
}

/* Get animation by index. */
const text_anim_t* wb_text_animate_get(wb_text_animate_inst *ta, int idx) {
    if (!ta || idx < 0 || idx >= ta->num_anims) return NULL;
    return &ta->anims[idx];
}

int wb_text_animate_count(wb_text_animate_inst *ta) {
    return ta ? ta->num_anims : 0;
}
