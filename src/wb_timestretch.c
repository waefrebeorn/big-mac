/* wb_timestretch.c — G26: time-stretch / pitch-shift.
 *
 * WSOLA-style (Waveform-Similarity Overlap-Add) time stretching: output is
 * assembled from overlapping input frames whose hop in the source is chosen
 * by searching near the nominal position for the best match to the tail of
 * the previous output frame — preserving transient phase coherence without
 * an FFT. Pitch shift = resample after stretch. Pure C11, no third party.
 *
 *   rate > 1.0  → faster/shorter output
 *   semitones   → applied as a post-resample ratio 2^(st/12)
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <wbus/wbus.h>

#define TS_FRAME 1024     /* synthesis frame length          */
#define TS_HOP   256      /* synthesis hop (75% overlap)     */

/* Mono downmix view of interleaved input. Caller frees. */
static float *mono_of(const wb_sample *in, uint32_t n, uint32_t chn) {
    float *m = malloc((size_t)n * sizeof(float));
    if (!m) return NULL;
    if (chn <= 1)
        for (uint32_t i = 0; i < n; i++) m[i] = in[i];
    else
        for (uint32_t i = 0; i < n; i++)
            m[i] = 0.5f * (in[i*chn] + in[i*chn + 1]);
    return m;
}

/* WSOLA core: stretch mono input by `rate` into a grown buffer.
 * Returns output length, or 0 on error. *outp must be freed. */
static uint32_t wsola(const float *in, uint32_t nin, double rate,
                      float **outp) {
    if (!in || !outp || nin < TS_FRAME * 2 || rate <= 0.0) return 0;
    /* nominal read hop: consume rate× fewer/more source samples per hop */
    uint32_t read_hop = (uint32_t)(TS_HOP * rate);
    if (read_hop < 8) read_hop = 8;
    uint32_t search = read_hop / 4;              /* WSOLA search window */
    uint32_t max_out = (size_t)((double)nin / rate) + TS_FRAME + TS_HOP;
    float *out = calloc(max_out, sizeof(float));
    if (!out) return 0;

    uint32_t out_pos = 0;                        /* write cursor */
    uint32_t src_pos = 0;                        /* nominal read cursor */
    /* normalized cross-correlation anchor: last TS_HOP of written output */
    while (src_pos + TS_FRAME + search < nin &&
           out_pos + TS_FRAME < max_out) {
        /* find offset delta in [−search, +search] maximizing similarity to
         * the previously written overlap region (the natural continuation) */
        int best_d = 0;
        float best_e = -1e30f;
        for (int d = -(int)search; d <= (int)search; d += 4) {
            uint32_t p = src_pos + (uint32_t)(d > 0 ? d : 0);
            if ((int)p + d < 0) continue;
            uint32_t q = (uint32_t)((long)p + d);
            if (q + TS_FRAME >= nin) break;
            /* energy of the candidate's overlap segment vs the tail we
             * already wrote: maximize their product (similarity proxy) */
            float e = 0;
            for (uint32_t k = 0; k < TS_HOP; k += 8)
                e -= fabsf(in[q + k] -
                           (out_pos >= TS_HOP ? out[out_pos - TS_HOP + k] : 0));
            if (e > best_e) { best_e = e; best_d = d; }
        }
        uint32_t p = (uint32_t)((long)src_pos + best_d);
        if (p + TS_FRAME >= nin) break;
        /* overlap-add with linear crossfade over the first TS_HOP samples */
        for (uint32_t k = 0; k < TS_FRAME; k++) {
            float w_in  = k < TS_HOP && out_pos + k > 0
                          ? (float)k / TS_HOP : 1.0f;
            float w_old = 1.0f - w_in;
            if (out_pos + k < max_out)
                out[out_pos + k] =
                    out[out_pos + k] * w_old + in[p + k] * w_in;
        }
        out_pos += TS_HOP;
        src_pos += read_hop;
    }
    *outp = out;
    return out_pos + TS_FRAME;
}

/* Public API: stretch/pitch an interleaved stereo clip into a new buffer.
 * rate: time factor (>1 faster). semitones: pitch shift after stretching.
 * Returns output frame count, *outp owned by caller. 0 on error. */
uint32_t wb_timestretch(const wb_sample *in, uint32_t frames, uint32_t chn,
                        double rate, double semitones, wb_sample **outp) {
    if (!in || !outp || frames == 0 || chn == 0 ||
        rate <= 0.01 || rate > 16.0) return 0;
    if (fabs(semitones) > 24.0) return 0;

    float *mono = mono_of(in, frames, chn);
    if (!mono) return 0;

    /* combined factor: pitch-up also shortens unless we pre-stretch.
     * total_time_ratio = rate; resample_factor = 2^(st/12).
     * stretch by rate × resample, then resample by 1/resample. */
    double resamp = pow(2.0, semitones / 12.0);
    float *stretched = NULL;
    uint32_t ns = 0;
    if (fabs(semitones) < 0.001) {
        ns = wsola(mono, frames, rate, &stretched);
        free(mono);
        if (!ns) return 0;
        resamp = 1.0;
    } else {
        /* after resample-by-resamp the length divides by resamp, so pre-
         * stretch by 1/resamp relative to the target rate to land on it */
        ns = wsola(mono, frames, rate / resamp, &stretched);
        free(mono);
        if (!ns) return 0;
    }

    /* naive linear-interpolation resampler for the pitch component */
    uint32_t n_out = (uint32_t)((double)ns / resamp);
    wb_sample *out = calloc((size_t)n_out * 2, sizeof(wb_sample));
    if (!out) { free(stretched); return 0; }
    for (uint32_t i = 0; i < n_out; i++) {
        double sp = (double)i * resamp;
        uint32_t i0 = (uint32_t)sp;
        uint32_t i1 = i0 + 1 < ns ? i0 + 1 : i0;
        float f = (float)(sp - i0);
        float v = stretched[i0] * (1 - f) + stretched[i1] * f;
        out[i*2] = v;           /* dual-mono output (input was downmixed) */
        out[i*2+1] = v;
    }
    free(stretched);
    *outp = out;
    return n_out;
}
