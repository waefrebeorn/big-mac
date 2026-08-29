/* wb_text_templates.c — text animation templates for titles/lower thirds.
 *
 * R077: Pre-built text animation templates (lower thirds, titles, kinetic).
 *
 * Templates:
 *   0: Lower third (slide up from bottom)
 *   1: Title card (fade in + scale)
 *   2: Kinetic typography (per-word stagger)
 *   3: News ticker (scroll left)
 *   4: Caption pop (word-by-word)
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

#define MAX_TEXT_TEMPLATES 16
#define MAX_TEXT_LEN 256
#define MAX_WORDS 32

typedef enum {
    TEMPLATE_LOWER_THIRD = 0,
    TEMPLATE_TITLE_CARD,
    TEMPLATE_KINETIC,
    TEMPLATE_TICKER,
    TEMPLATE_CAPTION_POP
} text_template_t;

typedef struct {
    char           text[MAX_TEXT_LEN];
    text_template_t type;
    float          duration;
    float          delay;
    /* Output parameters computed per-frame */
    float          x, y;
    float          scale;
    float          opacity;
    float          char_offsets[MAX_WORDS];  /* Per-character offset for kinetic */
    int            num_chars;
} text_template_inst;

typedef struct {
    text_template_inst templates[MAX_TEXT_TEMPLATES];
    int                num_templates;
    float              time;
} wb_text_templates_inst;

void *wb_text_templates_create(void) {
    return calloc(1, sizeof(wb_text_templates_inst));
}

void wb_text_templates_destroy(void *inst) { free(inst); }

/* Add a text template. Returns index or -1. */
int wb_text_template_add(void *inst, const char *text, text_template_t type,
                           float duration, float delay) {
    wb_text_templates_inst *tt = (wb_text_templates_inst *)inst;
    if (!tt || tt->num_templates >= MAX_TEXT_TEMPLATES) return -1;

    int idx = tt->num_templates++;
    text_template_inst *t = &tt->templates[idx];
    strncpy(t->text, text, MAX_TEXT_LEN - 1);
    t->type = type;
    t->duration = duration > 0.1f ? duration : 1.0f;
    t->delay = delay;
    t->num_chars = (int)strlen(text);

    return idx;
}

/* Easing functions */
static float ease_out_cubic(float t) {
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

static float ease_out_back(float t) {
    float c1 = 1.70158f;
    float c3 = c1 + 1.0f;
    float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

static float ease_out_expo(float t) {
    return t >= 1.0f ? 1.0f : 1.0f - powf(2.0f, -10.0f * t);
}

/* Update all templates for current time. */
void wb_text_templates_update(void *inst, float time) {
    wb_text_templates_inst *tt = (wb_text_templates_inst *)inst;
    if (!tt) return;

    for (int i = 0; i < tt->num_templates; i++) {
        text_template_inst *t = &tt->templates[i];

        float local_t = (time - t->delay) / t->duration;
        if (local_t < 0) local_t = 0;
        if (local_t > 1.0f) local_t = 1.0f;

        /* Defaults */
        t->x = 0;
        t->y = 0;
        t->scale = 1.0f;
        t->opacity = 1.0f;

        switch (t->type) {
        case TEMPLATE_LOWER_THIRD:
            /* Slide up from bottom */
            t->y = (1.0f - ease_out_cubic(local_t)) * 200.0f;
            t->opacity = ease_out_cubic(local_t < 0.2f ? local_t / 0.2f : 1.0f);
            break;

        case TEMPLATE_TITLE_CARD:
            /* Fade in + slight scale */
            t->opacity = ease_out_expo(local_t < 0.3f ? local_t / 0.3f : 1.0f);
            t->scale = 0.8f + ease_out_back(local_t) * 0.2f;
            break;

        case TEMPLATE_KINETIC:
            /* Per-character stagger handled in render */
            t->opacity = 1.0f;
            break;

        case TEMPLATE_TICKER:
            /* Scroll from right to left */
            t->x = (1.0f - local_t) * 2000.0f - 1000.0f;
            t->opacity = local_t < 0.1f ? local_t / 0.1f : 1.0f;
            break;

        case TEMPLATE_CAPTION_POP:
            /* Word-by-word pop */
            t->scale = 1.0f + 0.1f * sinf(local_t * 6.28f) * (1.0f - local_t);
            t->opacity = ease_out_cubic(local_t < 0.15f ? local_t / 0.15f : 1.0f);
            break;
        }
    }
}

/* Get template render parameters. */
const text_template_inst* wb_text_template_get(void *inst, int idx) {
    wb_text_templates_inst *tt = (wb_text_templates_inst *)inst;
    if (!tt || idx < 0 || idx >= tt->num_templates) return NULL;
    return &tt->templates[idx];
}

int wb_text_template_count(void *inst) {
    wb_text_templates_inst *tt = (wb_text_templates_inst *)inst;
    return tt ? tt->num_templates : 0;
}
