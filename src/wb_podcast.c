/* wb_podcast.c — podcast production workflow tools.
 *
 * Voice isolation (bandpass 300Hz–4kHz + spectral gating),
 * noise gate, loudness normalization (LUFS target), and
 * chapter detection from silence gaps.
 * Pure C11, zero third-party. Reuses wb_biquad for the bandpass.
 */

#include "wbus/wbus_dsp.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- opaque state ------------------------------------------------------ */

typedef struct {
    /* bandpass filter pair: highpass@300Hz + lowpass@4kHz */
    wb_biquad hp;   /* highpass, removes rumble below 300 Hz */
    wb_biquad lp;   /* lowpass, removes hiss above 4 kHz */

    /* spectral-gating state: running noise-floor estimate per "band".
     * We track a simple short-term RMS as the noise profile and
     * attenuate bins that fall near it. Simplified time-domain
     * approach: track envelope of the residual after bandpass,
     * suppress when it tracks the noise floor. */
    float   noise_floor;      /* smoothed noise estimate (RMS) */
    float   env_follower;     /* envelope follower for gate */
    float   voice_strength;   /* 0.0 = bypass, 1.0 = max isolation */

    /* loudness normalization state */
    double  l_sq_sum;         /* mean-square accumulator (K-weighted approx) */
    int     l_count;          /* samples in current measurement window */
    double  l_integ_sum;      /* integrated loudness sum (all blocks) */
    int     l_integ_blocks;   /* number of completed blocks */
    int     l_block_cap;      /* samples per 400ms BS.1770 gate block */
    double  l_short_term;     /* last short-term LUFS estimate */
    int     l_has_short;      /* 1 once first block closes */

    /* K-weighting filter (simplified: pre-emphasis high-shelf + HP) */
    wb_biquad k_pre;          /* high-shelf ~1.5 kHz, +4 dB */
    wb_biquad k_hp;           /* high-pass ~60 Hz (RLB) */

    uint32_t sr;
} wb_podcast_state;

/* wb_podcast is an opaque wrapper pointer */
typedef struct wb_podcast {
    wb_podcast_state s;
} wb_podcast;

/* ---- allocation -------------------------------------------------------- */

struct wb_podcast *wb_podcast_alloc(void) {
    struct wb_podcast *pc = (struct wb_podcast *)calloc(1, sizeof(struct wb_podcast));
    return pc;
}

void wb_podcast_free(struct wb_podcast *pc) {
    free(pc);
}

/* ---- helpers ----------------------------------------------------------- */

static inline float db_to_lin(float db) { return powf(10.0f, db / 20.0f); }

/* One-pole smoothing */
static inline float smooth(float prev, float in, float coef) {
    return prev + coef * (in - prev);
}

/* ---- K-weighting filter setup (BS.1770-4 approximation) ---------------- */
static void kweight_init(wb_biquad *pre, wb_biquad *hp, float sr) {
    /* RLB high-pass: 60 Hz, Q=0.707 */
    wb_biquad_init(hp, sr);
    wb_biquad_set(hp, 1 /*highpass*/, 60.0f, 0.707f, 0.0f);

    /* Pre-emphasis high-shelf: +4 dB @ 1500 Hz */
    wb_biquad_init(pre, sr);
    /* Approximate shelf with a peaking filter at 1500 Hz, Q=1.0, +4dB */
    wb_biquad_set(pre, 2 /*bandpass*/, 1500.0f, 1.0f, 4.0f);
}

/* Measure short-term LUFS over a block using K-weighting.
 * Returns LUFS value (typically -70..0). */
static double measure_block_lufs(wb_podcast_state *st, const float *in, int n) {
    double sq_sum = 0.0;
    for (int i = 0; i < n; i++) {
        float x = in[i];
        /* K-weight: RLB HP + pre-emphasis */
        float y = wb_biquad_process(&st->k_hp, x);
        y = wb_biquad_process(&st->k_pre, y);
        sq_sum += (double)y * (double)y;
    }
    double ms = sq_sum / (double)n;
    if (ms <= 1e-20) return -70.0;
    /* LUFS = -0.691 + 10*log10(mean_square) */
    return -0.691 + 10.0 * log10(ms);
}

/* ---- API implementation ------------------------------------------------ */

int wb_podcast_init(wb_podcast *pc, uint32_t sr) {
    if (!pc || sr == 0) return -1;
    memset(pc, 0, sizeof(*pc));
    wb_podcast_state *st = &pc->s;
    st->sr = sr;

    /* Bandpass: HP@300 Hz + LP@4 kHz */
    wb_biquad_init(&st->hp, (float)sr);
    wb_biquad_set(&st->hp, 1 /*highpass*/, 300.0f, 0.707f, 0.0f);

    wb_biquad_init(&st->lp, (float)sr);
    wb_biquad_set(&st->lp, 0 /*lowpass*/, 4000.0f, 0.707f, 0.0f);

    /* Noise floor / envelope */
    st->noise_floor = 0.0f;
    st->env_follower = 0.0f;
    st->voice_strength = 0.5f;  /* default moderate */

    /* Loudness measurement */
    st->l_sq_sum = 0.0;
    st->l_count = 0;
    st->l_integ_sum = 0.0;
    st->l_integ_blocks = 0;
    st->l_block_cap = (int)(0.4 * (double)sr);  /* 400ms blocks */
    st->l_short_term = -70.0;
    st->l_has_short = 0;

    /* K-weighting */
    kweight_init(&st->k_pre, &st->k_hp, (float)sr);

    return 0;
}

void wb_podcast_set_voice_isolation_strength(void *pc, float strength) {
    if (!pc) return;
    wb_podcast *p = (wb_podcast *)pc;
    if (strength < 0.0f) strength = 0.0f;
    if (strength > 1.0f) strength = 1.0f;
    p->s.voice_strength = strength;
}

int wb_podcast_process_voice(void *pc, const float *in, float *out, int n) {
    if (!pc || !in || !out || n <= 0) return -1;
    wb_podcast_state *st = &((wb_podcast *)pc)->s;
    float strength = st->voice_strength;

    if (strength <= 0.0f) {
        /* Bypass: copy input to output */
        memcpy(out, in, n * sizeof(float));
        return 0;
    }

    /* Attack/release coefficients for envelope follower */
    const float atk_coef = 1.0f - expf(-1.0f / (0.005f * st->sr));   /* 5ms */
    const float rel_coef = 1.0f - expf(-1.0f / (0.050f * st->sr));   /* 50ms */

    /* Noise-floor adaptation rate */
    const float nf_adapt = 0.999f;  /* very slow — tracks background */

    for (int i = 0; i < n; i++) {
        float x = in[i];

        /* Step 1: Bandpass 300 Hz – 4 kHz */
        float bp = wb_biquad_process(&st->hp, x);
        bp = wb_biquad_process(&st->lp, bp);

        /* Step 2: Envelope follower (rectified) */
        float rect = fabsf(bp);
        float coef = (rect > st->env_follower) ? atk_coef : rel_coef;
        st->env_follower = smooth(st->env_follower, rect, coef);

        /* Step 3: Noise floor tracking (min-tracking via slow release) */
        if (rect < st->noise_floor) {
            st->noise_floor = rect;  /* instant drop */
        } else {
            st->noise_floor = st->noise_floor * nf_adapt + rect * (1.0f - nf_adapt);
        }

        /* Step 4: Spectral-gating soft suppression.
         * Compute suppression gain: when the envelope is well above
         * the noise floor, pass at unity; when near the floor,
         * attenuate proportionally to strength. */
        float gate_ratio;
        if (st->noise_floor < 1e-10f) {
            gate_ratio = 1.0f;
        } else {
            float snr = st->env_follower / st->noise_floor;
            /* snr = 1 → at noise floor → suppress fully (with strength)
             * snr >= 3 → well above → pass */
            float threshold = 3.0f;
            if (snr >= threshold) {
                gate_ratio = 1.0f;
            } else {
                /* ramp from 0 (at snr=0) to 1 (at snr=threshold) */
                float raw = snr / threshold;
                /* Apply strength: at strength=1, full suppression below threshold;
                 * at strength=0, no suppression */
                gate_ratio = raw * strength + (1.0f - strength);
                if (gate_ratio > 1.0f) gate_ratio = 1.0f;
            }
        }

        /* Mix: blend between raw and isolated based on strength */
        float isolated = bp * gate_ratio;
        out[i] = x * (1.0f - strength) + isolated * strength;
    }

    return 0;
}

int wb_podcast_process_noise_gate(void *pc, const float *in, float *out,
                                    int n, float threshold) {
    if (!pc || !in || !out || n <= 0) return -1;
    wb_podcast_state *st = &((wb_podcast *)pc)->s;

    /* threshold is a linear amplitude (e.g. 0.01 = -40 dBFS) */
    if (threshold < 0.0f) threshold = 0.0f;

    /* Hysteresis: open threshold is 3dB above close threshold */
    float open_thresh = threshold * 1.414f;  /* +3 dB */
    float close_thresh = threshold;

    /* Attack/release */
    const float atk_coef = 1.0f - expf(-1.0f / (0.005f * st->sr));   /* 5ms */
    const float rel_coef = 1.0f - expf(-1.0f / (0.100f * st->sr));   /* 100ms */

    int gate_open = 0;
    float env = 0.0f;

    for (int i = 0; i < n; i++) {
        float x = in[i];

        /* Level detector: rectified + smoothed */
        float rect = fabsf(x);
        float coef = (rect > env) ? atk_coef : rel_coef;
        env = env + coef * (rect - env);

        /* Hysteresis gate */
        if (!gate_open && env >= open_thresh) {
            gate_open = 1;
        } else if (gate_open && env < close_thresh) {
            gate_open = 0;
        }

        /* Apply gain: full when open, attenuate when closed */
        float gain = gate_open ? 1.0f : 0.0f;
        /* Smooth the gain transition to avoid clicks */
        static float smoothed_gain = 1.0f;
        (void)st;  /* suppress unused in static context */
        smoothed_gain = smoothed_gain + 0.001f * (gain - smoothed_gain);
        out[i] = x * smoothed_gain;
    }

    return 0;
}

int wb_podcast_normalize_loudness(void *pc, float *audio, int n,
                                   float target_lufs) {
    if (!pc || !audio || n <= 0) return -1;
    wb_podcast_state *st = &((wb_podcast *)pc)->s;

    /* Step 1: Measure integrated loudness of the buffer */
    /* Process in 400ms blocks with 75% overlap */
    int block_size = (int)(0.4 * (double)st->sr);
    if (block_size > n) block_size = n;
    int hop = block_size / 4;  /* 75% overlap */
    if (hop < 1) hop = 1;

    double sum_loudness = 0.0;
    int num_blocks = 0;

    /* Reset K-filter state for measurement pass */
    wb_biquad tmp_hp, tmp_pre;
    memcpy(&tmp_hp, &st->k_hp, sizeof(wb_biquad));
    memcpy(&tmp_pre, &st->k_pre, sizeof(wb_biquad));

    for (int off = 0; off + block_size <= n; off += hop) {
        double lufs = measure_block_lufs(st, audio + off, block_size);
        if (lufs > -70.0) {  /* only count non-silent blocks */
            sum_loudness += lufs;
            num_blocks++;
        }
    }

    if (num_blocks == 0) return 0;  /* silent input, nothing to do */

    double avg_lufs = sum_loudness / (double)num_blocks;

    /* Step 2: Compute gain needed to reach target */
    float gain_db = target_lufs - (float)avg_lufs;
    float gain_lin = db_to_lin(gain_db);

    /* Safety: clamp gain to prevent blowup */
    if (gain_lin > 10.0f) gain_lin = 10.0f;   /* max +20 dB */
    if (gain_lin < 0.01f) gain_lin = 0.01f;   /* min -40 dB */

    /* Step 3: Apply gain */
    for (int i = 0; i < n; i++) {
        audio[i] *= gain_lin;
    }

    return 0;
}

int wb_podcast_detect_chapters(void *pc, const float *audio, int n,
                                float sr, double *chapter_times_out,
                                int max_chapters) {
    if (!pc || !audio || n <= 0 || !chapter_times_out || max_chapters <= 0)
        return 0;
    (void)pc;  /* not needed for this operation */

    int chapter_count = 0;
    int min_gap_samples = (int)(2.0 * sr);  /* 2 seconds of silence minimum */

    /* Scan for silence regions: compute RMS in 50ms windows */
    int window_size = (int)(0.05 * sr);  /* 50ms */
    if (window_size < 1) window_size = 1;

    float silence_thresh = 0.01f;  /* -40 dBFS RMS threshold */

    int silence_start = -1;
    int in_silence = 0;

    for (int pos = 0; pos + window_size <= n; pos += window_size) {
        /* Compute RMS of this window */
        double sq_sum = 0.0;
        for (int i = pos; i < pos + window_size; i++) {
            sq_sum += (double)audio[i] * (double)audio[i];
        }
        float rms = (float)sqrt(sq_sum / (double)window_size);

        if (rms < silence_thresh) {
            if (!in_silence) {
                silence_start = pos;
                in_silence = 1;
            }
        } else {
            if (in_silence) {
                int silence_len = pos - silence_start;
                if (silence_len >= min_gap_samples) {
                    /* Chapter boundary at the END of the silence region */
                    double chapter_time = (double)pos / (double)sr;
                    if (chapter_count == 0) {
                        /* Always include t=0 as first chapter */
                        chapter_times_out[chapter_count++] = 0.0;
                    }
                    if (chapter_count < max_chapters) {
                        chapter_times_out[chapter_count++] = chapter_time;
                    }
                }
                in_silence = 0;
                if (chapter_count >= max_chapters) break;
            }
        }
    }

    /* Handle trailing silence */
    if (in_silence && chapter_count < max_chapters) {
        int silence_len = n - silence_start;
        if (silence_len >= min_gap_samples) {
            if (chapter_count == 0) {
                chapter_times_out[chapter_count++] = 0.0;
            }
            if (chapter_count < max_chapters) {
                chapter_times_out[chapter_count++] = (double)n / (double)sr;
            }
        }
    }

    /* If no chapters found, just return t=0 */
    if (chapter_count == 0) {
        chapter_times_out[0] = 0.0;
        chapter_count = 1;
    }

    return chapter_count;
}