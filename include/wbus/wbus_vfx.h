/* wbus/wbus_vfx.h — visual effects API (R077 Phase 2).
 *
 * Blend modes, 3D LUT loader, color correction,
 * transitions, camera effects, meme/YTP effects.
 */

#ifndef WBUS_VFX_H
#define WBUS_VFX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Blend modes (W3C Compositing spec) */
typedef enum {
    WB_BLEND_NORMAL = 0,
    WB_BLEND_MULTIPLY,
    WB_BLEND_SCREEN,
    WB_BLEND_OVERLAY,
    WB_BLEND_DARKEN,
    WB_BLEND_LIGHTEN,
    WB_BLEND_COLOR_DODGE,
    WB_BLEND_COLOR_BURN,
    WB_BLEND_HARD_LIGHT,
    WB_BLEND_SOFT_LIGHT,
    WB_BLEND_DIFFERENCE,
    WB_BLEND_EXCLUSION,
    WB_BLEND_ADD,
    WB_BLEND_SUBTRACT,
    WB_BLEND_COUNT
} wb_blend_mode;

void wb_blend_pixels(uint8_t *dst, const uint8_t *src, int count, wb_blend_mode mode);

/* 3D LUT */
typedef struct wb_lut3d wb_lut3d;
int  wb_lut3d_load(wb_lut3d *lut, const char *path);
void wb_lut3d_apply(const wb_lut3d *lut, uint8_t *rgba, int count);

/* Color correction */
typedef struct {
    float lift, gamma, gain, contrast, saturation, temperature, hue;
} wb_color_params;

void wb_color_correct(uint8_t *rgba, int count, const wb_color_params *p);

/* Transitions */
void wb_transition_dissolve(uint8_t *out, const uint8_t *a, const uint8_t *b, int count, float t);
void wb_transition_wipe(uint8_t *out, const uint8_t *a, const uint8_t *b, int w, int h, float t);
void wb_transition_flash(uint8_t *out, const uint8_t *a, const uint8_t *b, int count, float t);

/* Camera effects */
typedef struct wb_camera_shake wb_camera_shake;
void wb_camera_shake_init(wb_camera_shake *s);
void wb_camera_shake_trigger(wb_camera_shake *s, float amount);
void wb_camera_shake_update(wb_camera_shake *s, float dt);
void wb_camera_shake_offset(wb_camera_shake *s, float *ox, float *oy);

void wb_chromatic_aberration(uint8_t *rgba, int w, int h, float amount);

/* Meme/YTP effects */
void wb_effect_deep_fry(uint8_t *rgba, int w, int h, float intensity);
void wb_effect_vhs(uint8_t *rgba, int w, int h, float time, float intensity);
void wb_effect_rgb_glitch(uint8_t *rgba, int w, int h, float intensity);
void wb_effect_posterize(uint8_t *rgba, int count, int levels);
void wb_effect_vignette(uint8_t *rgba, int w, int h, float strength);

#ifdef __cplusplus
}
#endif

#endif /* WBUS_VFX_H */
