/* wb_quantize.c — smart quantize and groove templates.
 *
 * R077: MIDI quantization that preserves human feel.
 *
 * Smart quantize:
 *   - Strength parameter (0=raw, 100=full grid)
 *   - Swing offset (shift even 8th notes)
 *   - Tolerance window (only quantize notes near grid)
 *
 * Groove templates:
 *   - Extract timing offsets from a reference performance
 *   - Apply offsets to other MIDI clips
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

#define MAX_GROOVE_POINTS 128

typedef struct {
    float    grid_pos;      /* Position in beats */
    float    offset;        /* Timing offset in beats (-0.5 to 0.5) */
} groove_point_t;

typedef struct {
    /* Smart quantize parameters */
    float    strength;      /* 0..1 (0=raw, 1=full quantize) */
    float    swing;         /* 0..0.5 (0=straight, 0.5=heavy swing) */
    float    tolerance;     /* Window size in beats (0.1=strict) */
    int      grid_division; /* 1=quarter, 2=eighth, 4=sixteenth, 3=triplet */

    /* Groove template */
    groove_point_t groove[MAX_GROOVE_POINTS];
    int      num_groove_points;
    int      use_groove;
} wb_quantize_inst;

void *wb_quantize_create(void) {
    wb_quantize_inst *q = (wb_quantize_inst *)calloc(1, sizeof(*q));
    if (!q) return NULL;
    q->strength = 0.8f;
    q->swing = 0.0f;
    q->tolerance = 0.25f;
    q->grid_division = 4;  /* 16th notes */
    q->use_groove = 0;
    return q;
}

void wb_quantize_destroy(void *inst) { free(inst); }

void wb_quantize_set(void *inst, int param, float v) {
    wb_quantize_inst *q = (wb_quantize_inst *)inst;
    if (!q) return;
    switch (param) {
    case 0: q->strength = v < 0 ? 0 : (v > 1 ? 1 : v); break;
    case 1: q->swing = v < 0 ? 0 : (v > 0.5f ? 0.5f : v); break;
    case 2: q->tolerance = v < 0.01f ? 0.01f : (v > 0.5f ? 0.5f : v); break;
    case 3: q->grid_division = (int)v > 0 ? (int)v : 1; break;
    case 4: q->use_groove = (int)v; break;
    default: break;
    }
}

/* Quantize a single note position.
 * Returns the quantized position in beats. */
float wb_quantize_note(wb_quantize_inst *q, float pos_beats) {
    if (!q) return pos_beats;

    float grid_size = 1.0f / (float)q->grid_division;

    /* Find nearest grid point */
    float grid_pos = roundf(pos_beats / grid_size) * grid_size;

    /* Apply swing to even 8th notes */
    if (q->swing > 0 && q->grid_division >= 2) {
        float eighth_pos = pos_beats * 2.0f;
        int is_even = ((int)(eighth_pos + 0.5f)) % 2 == 0;
        if (is_even) {
            grid_pos += q->swing * grid_size;
        }
    }

    /* Apply groove template offset */
    if (q->use_groove && q->num_groove_points > 0) {
        /* Find nearest groove point */
        float nearest_offset = 0;
        float min_dist = 1.0f;
        for (int i = 0; i < q->num_groove_points; i++) {
            float dist = fabsf(grid_pos - q->groove[i].grid_pos);
            if (dist < min_dist) {
                min_dist = dist;
                nearest_offset = q->groove[i].offset;
            }
        }
        grid_pos += nearest_offset * grid_size;
    }

    /* Check tolerance */
    float diff = grid_pos - pos_beats;
    if (fabsf(diff) > q->tolerance * grid_size) {
        return pos_beats;  /* Outside tolerance, don't quantize */
    }

    /* Apply strength */
    return pos_beats + diff * q->strength;
}

/* Extract groove template from a reference MIDI clip.
 * note_positions: array of note positions in beats
 * num_notes: number of notes */
int wb_quantize_extract_groove(wb_quantize_inst *q,
                                const float *note_positions,
                                int num_notes) {
    if (!q || !note_positions || num_notes < 2) return 0;

    float grid_size = 1.0f / (float)q->grid_division;
    q->num_groove_points = 0;

    for (int i = 0; i < num_notes && q->num_groove_points < MAX_GROOVE_POINTS; i++) {
        float pos = note_positions[i];
        float grid_pos = roundf(pos / grid_size) * grid_size;
        float offset = (pos - grid_pos) / grid_size;

        /* Only store significant offsets */
        if (fabsf(offset) > 0.05f) {
            q->groove[q->num_groove_points].grid_pos = grid_pos;
            q->groove[q->num_groove_points].offset = offset;
            q->num_groove_points++;
        }
    }

    return q->num_groove_points;
}

/* Humanize: add subtle timing and velocity variations.
 * timing_amount: 0..1 (max timing variation)
 * velocity_amount: 0..1 (max velocity variation) */
typedef struct {
    float    timing;        /* Timing offset in beats */
    int      velocity;      /* Velocity offset */
} humanize_result_t;

humanize_result_t wb_quantize_humanize(wb_quantize_inst *q,
                                         float base_pos, int base_vel,
                                         float timing_amount,
                                         float velocity_amount) {
    humanize_result_t result = {0, 0};
    if (!q) return result;

    /* Random timing variation */
    float rand_val = (float)(rand() % 1000) / 1000.0f - 0.5f;
    result.timing = rand_val * timing_amount * 0.1f;  /* ±5% of a beat max */

    /* Random velocity variation */
    int vel_rand = (rand() % 200) - 100;
    result.velocity = (int)(vel_rand * velocity_amount);

    return result;
}
