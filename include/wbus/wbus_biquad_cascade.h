#ifndef WBUS_WBUS_BIQUAD_CASCADE_H
#define WBUS_WBUS_BIQUAD_CASCADE_H

#include <emmintrin.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Biquad cascade via partial fraction expansion (G4 [R075]).
 * Two paths:
 * 1. Scalar PFE: converts N-stage cascade to parallel first-order sections
 * 2. SIMD: processes 4 independent N-stage cascades in parallel */

typedef struct {
    float b0, b1, b2;
    float a1, a2;
} biquad_section_t;

#define WB_BIQUAD_CASCADE_MAX_STAGES 8
#define WB_BIQUAD_CASCADE_MAX_SECTIONS 16

/* ---- Scalar PFE path ---- */
typedef struct {
    float gain;
    float pole;
    float state;
} wb_firstorder_section;

typedef struct wb_biquad_cascade {
    int n_sections;
    wb_firstorder_section sections[WB_BIQUAD_CASCADE_MAX_SECTIONS];
} wb_biquad_cascade;

int wb_biquad_cascade_init(wb_biquad_cascade *c, const biquad_section_t *biquads, int n_biquads);
float wb_biquad_cascade_process_scalar(wb_biquad_cascade *c, float x);
void wb_biquad_cascade_reset(wb_biquad_cascade *c);

/* ---- SIMD path: 4 independent cascades in parallel ---- */
typedef struct wb_biquad_cascade4 {
    int n_stages;
    /* SoA: each __m128 holds 4 values (one per cascade instance) */
    __m128 b0[WB_BIQUAD_CASCADE_MAX_STAGES];
    __m128 b1[WB_BIQUAD_CASCADE_MAX_STAGES];
    __m128 b2[WB_BIQUAD_CASCADE_MAX_STAGES];
    __m128 a1[WB_BIQUAD_CASCADE_MAX_STAGES];
    __m128 a2[WB_BIQUAD_CASCADE_MAX_STAGES];
    __m128 s1[WB_BIQUAD_CASCADE_MAX_STAGES];
    __m128 s2[WB_BIQUAD_CASCADE_MAX_STAGES];
} wb_biquad_cascade4;

int wb_biquad_cascade4_init(wb_biquad_cascade4 *c4, const biquad_section_t *biquads, int n_biquads);
__m128 wb_biquad_cascade4_process(wb_biquad_cascade4 *c4, __m128 x);
void wb_biquad_cascade4_reset(wb_biquad_cascade4 *c4);

#ifdef __cplusplus
}
#endif

#endif /* WBUS_WBUS_BIQUAD_CASCADE_H */
