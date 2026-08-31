/* wb_chord_ai.c - AI chord progression generator (Markov-chain style).
 *
 * Generates chord progressions using a 7-state Markov chain over diatonic
 * scale degrees, with mood and complexity as shaping parameters.
 * Pure C11, zero third-party. Uses xorshift32 for reproducibility.
 *
 * API: wb_chord_ai_create / destroy / generate / generate_variation /
 *      set_complexity / set_mood / get_tension.
 */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "wbus.h"

/* ---- chord types ---- */
#define WB_CHORD_TYPES   6
#define WB_CHORD_MAJ     0   /* major       */
#define WB_CHORD_MIN     1   /* minor       */
#define WB_CHORD_MIN7    2   /* minor 7     */
#define WB_CHORD_MAJ7    3   /* major 7     */
#define WB_CHORD_DOM7    4   /* dominant 7  */
#define WB_CHORD_DIM     5   /* diminished  */

/* Tension weight per chord type (used by get_tension). Higher = more tense. */
static const float chord_tension[WB_CHORD_TYPES] = {
    0.0f, 0.2f, 0.35f, 0.5f, 0.5f, 0.8f
};

/* Scale interval tables (semitones from root). Mode 0 = major, 1 = minor. */
static const int scale_intervals[2][7] = {
    {0, 2, 4, 5, 7, 9, 11},  /* major */
    {0, 2, 3, 5, 7, 8, 10},  /* natural minor */
};

/* Diatonic triad quality per degree, indexed by [mode][degree].
 * 0=major, 1=minor, 2=diminished. Maps to a base chord type. */
static const int diag_quality[2][7] = {
    /* major:  I   ii  iii IV  V   vi  vii° */
    {0, 1, 1, 0, 0, 1, 2},
    /* minor:  i   ii° III iv  v   VI  VII  */
    {1, 2, 0, 1, 1, 0, 0},
};

/* Mood multipliers per destination degree for the Markov transition matrix.
 * Shapes which harmonic destinations are favored for each mood. */
static const float mood_mult[4][7] = {
    /* happy (0): stable major, strong V and IV */
    {1.2f, 0.8f, 0.8f, 1.1f, 1.3f, 1.0f, 0.6f},
    /* sad (1): minor submediant/mediant */
    {0.8f, 1.2f, 1.0f, 0.9f, 0.9f, 1.3f, 0.7f},
    /* tense (2): dominant + diminished */
    {0.7f, 0.8f, 0.9f, 0.8f, 1.4f, 0.9f, 1.3f},
    /* peaceful (3): tonic/subdominant stillness */
    {1.4f, 0.7f, 0.7f, 1.0f, 0.8f, 1.2f, 0.5f},
};

#define WB_AI_MAX_CHORDS 64

typedef struct wb_chord_ai {
    uint32_t seed;        /* PRNG state (0 treated as seed=1) */
    float complexity;     /* 0..1 : 0 = structured, 1 = free-form */
    int  mood;            /* 0..3 : happy/sad/tense/peaceful */
    /* last generated progression (for variation) */
    int  last_num_chords;
    int  last_key;
    int  last_mode;
    int  last_roots[WB_AI_MAX_CHORDS];
    int  last_types[WB_AI_MAX_CHORDS];
    float last_tension;
} wb_chord_ai;

/* ---- PRNG: xorshift32 ---- */
static uint32_t xor32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static float randf(uint32_t *state) {
    return (float)(xor32(state) & 0xFFFFFF) / (float)0x1000000f;
}

static int randi(uint32_t *state, int n) {
    return n > 0 ? (int)(xor32(state) % (uint32_t)n) : 0;
}

/* ---- Markov transition matrix construction ---- */
/* Base weights reflecting common Western voice-leading. Row = from degree. */
static const float base_trans[7][7] = {
    /* to:  I    ii   iii  IV   V    vi   vii */
    { 1.0f, 2.0f, 2.0f, 5.0f, 8.0f, 6.0f, 1.0f },  /* from I   */
    { 2.0f, 0.5f, 3.0f, 1.0f, 10.0f, 1.0f, 0.5f }, /* from ii  */
    { 2.0f, 0.5f, 0.5f, 3.0f, 2.0f, 8.0f, 0.5f },  /* from iii */
    { 6.0f, 3.0f, 0.5f, 0.5f, 5.0f, 1.0f, 0.5f },  /* from IV  */
    { 10.0f, 2.0f, 3.0f, 1.0f, 0.5f, 2.0f, 3.0f }, /* from V   */
    { 2.0f, 3.0f, 0.5f, 6.0f, 4.0f, 0.5f, 1.0f },  /* from vi  */
    { 8.0f, 0.5f, 0.5f, 1.0f, 3.0f, 0.5f, 0.5f },  /* from vii */
};

/* Build the transition matrix into `trans[7][7]`, normalized per row to
 * sum to 1.0. Complexity (0..1) blends the structured base matrix toward a
 * uniform distribution (complexity=1 = fully random). Mood scales columns. */
static void build_trans(float out[7][7], float complexity, int mood) {
    int i, j;
    for (i = 0; i < 7; i++) {
        float row[7];
        float sum = 0.0f;
        for (j = 0; j < 7; j++) {
            /* structured weight from base (mood-modulated) */
            float w = base_trans[i][j] * mood_mult[mood][j];
            /* blend toward uniform as complexity rises */
            w = w * (1.0f - complexity) + complexity * 1.0f;
            row[j] = w;
            sum += w;
        }
        if (sum <= 0.0f) sum = 1.0f;
        for (j = 0; j < 7; j++)
            out[i][j] = row[j] / sum;
    }
}

/* Sample a degree from a transition row using cumulative distribution. */
static int sample_degree(const float row[7], uint32_t *state) {
    float r = randf(state);
    float acc = 0.0f;
    for (int i = 0; i < 6; i++) {
        acc += row[i];
        if (r < acc) return i;
    }
    return 6;  /* last degree */
}

/* Map a diatonic degree + quality + mood + complexity into a chord type.
 * Higher complexity / tense mood => more 7th and altered tones. */
static int pick_chord_type(uint32_t *state, int quality, float complexity, int mood) {
    /* quality: 0=major, 1=minor, 2=diminished */
    int r = randf(state);

    /* Probability of upgrading to a 7th chord scales with complexity. */
    float seventh_chance = 0.35f + complexity * 0.45f;  /* 0.35 .. 0.8 */

    switch (quality) {
    case 0: /* major -> MAI / MAJ7 / DOM7 */
        if (r < (1.0f - seventh_chance)) return WB_CHORD_MAJ;
        if (mood == 0 /* happy */)   return WB_CHORD_MAJ7;
        if (mood == 2 /* tense */)   return WB_CHORD_DOM7;
        return randf(state) < 0.5f ? WB_CHORD_MAJ7 : WB_CHORD_DOM7;
    case 1: /* minor -> MIN / MIN7 */
        if (r < (1.0f - seventh_chance)) return WB_CHORD_MIN;
        return WB_CHORD_MIN7;
    case 2: /* diminished -> DIM / (tense -> DIM with weight) */
        if (mood == 2) return WB_CHORD_DIM;
        if (randf(state) < 0.5f) return WB_CHORD_DIM;
        /* fall back to minor sometimes so progressions stay singable */
        return WB_CHORD_MIN;
    default:
        return WB_CHORD_MAJ;
    }
}

/* ---- public API ---- */

wb_chord_ai *wb_chord_ai_create(uint32_t sr) {
    (void)sr;
    wb_chord_ai *ai = (wb_chord_ai *)calloc(1, sizeof(*ai));
    if (!ai) return NULL;
    ai->seed = 1;
    ai->complexity = 0.5f;
    ai->mood = 0;               /* happy */
    ai->last_num_chords = 0;
    ai->last_key = 0;
    ai->last_mode = 0;
    ai->last_tension = 0.0f;
    return ai;
}

void wb_chord_ai_destroy(wb_chord_ai *ai) {
    free(ai);
}

void wb_chord_ai_set_complexity(wb_chord_ai *ai, float complexity) {
    if (!ai) return;
    if (complexity < 0.0f) complexity = 0.0f;
    if (complexity > 1.0f) complexity = 1.0f;
    ai->complexity = complexity;
}

float wb_chord_ai_get_complexity(const wb_chord_ai *ai) {
    return ai ? ai->complexity : 0.0f;
}

void wb_chord_ai_set_mood(wb_chord_ai *ai, int mood) {
    if (!ai) return;
    if (mood < 0) mood = 0;
    if (mood > 3) mood = 3;
    ai->mood = mood;
}

int wb_chord_ai_get_mood(const wb_chord_ai *ai) {
    return ai ? ai->mood : 0;
}

int wb_chord_ai_generate(wb_chord_ai *ai, int key, int mode, int num_chords,
                         int *out_roots, int *out_types, uint32_t *seed) {
    if (!ai || !out_roots || !out_types || num_chords <= 0) return -1;
    if (key < 0) key = 0;
    if (key > 11) key = 11;
    if (mode < 0 || mode > 1) mode = 0;
    if (num_chords > WB_AI_MAX_CHORDS) num_chords = WB_AI_MAX_CHORDS;

    uint32_t state = seed ? *seed : ai->seed;
    ai->seed = state; /* persist for subsequent calls */

    float trans[7][7];
    build_trans(trans, ai->complexity, ai->mood);

    const int *scale = scale_intervals[mode];

    /* Start on the tonic (degree 0) with high probability, else random. */
    int degree;
    if (randf(&state) < 0.8f)
        degree = 0;
    else
        degree = randi(&state, 7);

    float tension_acc = 0.0f;
    float stability_acc = 0.0f;

    for (int i = 0; i < num_chords; i++) {
        int quality = diag_quality[mode][degree];
        int ctype = pick_chord_type(&state, quality, ai->complexity, ai->mood);

        out_roots[i] = key + scale[degree];
        out_types[i] = ctype;

        tension_acc += chord_tension[ctype];
        stability_acc += (1.0f - chord_tension[ctype]);

        /* Pick next degree from the Markov row. */
        degree = sample_degree(trans[degree], &state);
    }

    ai->seed = state;
    ai->last_num_chords = num_chords;
    ai->last_key = key;
    ai->last_mode = mode;
    for (int i = 0; i < num_chords; i++) {
        ai->last_roots[i] = out_roots[i];
        ai->last_types[i] = out_types[i];
    }
    ai->last_tension = (tension_acc / (float)num_chords) * 10.0f;

    if (seed) *seed = state;
    return num_chords;
}

/* Generate a variation of the most recently generated progression.
 * Same key/mode/chord-count; chords are nudged via re-rolled degrees and
 * type adjustments biased toward (but not identical to) the original. */
int wb_chord_ai_generate_variation(wb_chord_ai *ai, uint32_t seed,
                                   int *out_roots, int *out_types) {
    if (!ai || !out_roots || !out_types || ai->last_num_chords <= 0) return -1;

    uint32_t state = seed ? seed : ai->seed;
    ai->seed = state;

    float trans[7][7];
    build_trans(trans, ai->complexity, ai->mood);
    const int *scale = scale_intervals[ai->last_mode];

    /* Recover the last progression's degrees from roots, then perturb. */
    int prev_degree = 0;
    /* find the degree of the first chord */
    for (int d = 0; d < 7; d++) {
        if (ai->last_roots[0] == ai->last_key + scale[d]) {
            prev_degree = d;
            break;
        }
    }

    float tension_acc = 0.0f;
    int n = ai->last_num_chords;

    for (int i = 0; i < n; i++) {
        /* With ~25% probability, keep the original chord; otherwise take a
         * Markov step from the previous degree (fresh neighbor). */
        int degree;
        float keep_p = 0.75f;  /* keep original degree */
        if (randf(&state) < keep_p && ai->last_roots[i] >= 0) {
            /* recover original degree */
            degree = prev_degree; /* default */
            for (int d = 0; d < 7; d++) {
                if (ai->last_roots[i] == ai->last_key + scale[d]) {
                    degree = d;
                    break;
                }
            }
        } else {
            /* take a real random walk step */
            degree = sample_degree(trans[prev_degree], &state);
        }
        prev_degree = degree;

        int quality = diag_quality[ai->last_mode][degree];
        int ctype = pick_chord_type(&state, quality, ai->complexity, ai->mood);

        out_roots[i] = ai->last_key + scale[degree];
        out_types[i] = ctype;
        tension_acc += chord_tension[ctype];
    }

    ai->seed = state;
    ai->last_tension = (tension_acc / (float)n) * 10.0f;
    return n;
}

float wb_chord_ai_get_tension(const wb_chord_ai *ai) {
    return ai ? ai->last_tension : 0.0f;
}
