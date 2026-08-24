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

/* R073 hop 4: one-pole crossover split for the tonality limit. */
static void band_split(const float *in, uint32_t n, double corner_hz,
                       uint32_t sr, float *low, float *high) {
    /* butterworth-ish 2nd order via cascaded one-poles at half the corner */
    double rc = 1.0 / (2.0 * M_PI * corner_hz);
    double a = rc / (rc + 1.0 / sr);            /* one-pole LP coeff */
    float l1 = 0, l2 = 0;
    for (uint32_t i = 0; i < n; i++) {
        l1 += (float)(a * ((double)in[i] - l1));
        l2 += (float)(a * ((double)l1 - l2));
        low[i]  = l2;
        high[i] = in[i] - l2;
    }
}

/* WSOLA core: stretch mono input by `rate` into a grown buffer.
 * Returns output length, or 0 on error. *outp must be freed. */
static uint32_t wsola(const float *in, uint32_t nin, double rate,
                      double pitch, const uint32_t *trans, uint32_t ntrans,
                      float **outp) {
    /* R073-G26b: transient guard — a candidate window straddling a transient
     * would double or skip it; such candidates are skipped by the search. */
    if (!in || !outp || nin < TS_FRAME * 2 || rate <= 0.0) return 0;
    /* nominal read hop: consume rate× fewer/more source samples per hop */
    uint32_t read_hop = (uint32_t)(TS_HOP * rate);
    if (read_hop < 8) read_hop = 8;
    /* R073: search must span at least half a synthesis hop so the similarity
     * search can always reach phase alignment for any tone period up to
     * TS_HOP — a window of read_hop/4 left periodic signals misaligned,
     * producing constant beating (measured 392Hz vs 440Hz on a 220Hz tone). */
    uint32_t search = TS_HOP / 2;
    uint32_t max_out = (size_t)((double)nin / rate) + TS_FRAME + TS_HOP;
    float *out = calloc(max_out, sizeof(float));
    if (!out) return 0;

    uint32_t out_pos = 0;                        /* write cursor */
    uint32_t src_pos = 0;                        /* nominal read cursor */
    /* R073: natural-continuation anchor — the input segment that follows the
     * previously consumed frame. Candidates correlate against THIS, not the
     * crossfaded output (which smears two positions and biases the search). */
    float anchor[TS_HOP];
    int have_anchor = 0;
    /* normalized cross-correlation anchor: last TS_HOP of written output */
    while (src_pos + TS_FRAME + search < nin &&
           out_pos + TS_FRAME < max_out) {
        /* find offset delta in [−search, +search] maximizing similarity to
         * the previously written overlap region (the natural continuation) */
        /* R073 (WSOLA per Verhelst/Roelands '93): cross-correlate each
         * candidate's overlap segment against the tail of the previously
         * written output — full-resolution dot product, not a stride-8
         * difference. Maximize correlation; this is what makes sustained
         * tones stay phase-coherent across the splice. */
        int best_d = 0;
        float best_e = -1e30f;
        /* R073-G26b (MDPI TSM review §4.3): when this frame's window contains
         * a transient, lock to nominal (d=0) so the attack is neither doubled
         * by a shifted copy nor skipped; the search then runs normally. */
        int locked = 0;
        for (uint32_t t = 0; t < ntrans && !locked; t++)
            if (trans[t] >= src_pos && trans[t] < src_pos + TS_FRAME)
                locked = 1;
        for (int d = locked ? 0 : -(int)search;
             d <= (locked ? 0 : (int)search); d += 2) {
            long q = (long)src_pos + d;
            if (q < 0 || q + TS_FRAME >= (long)nin) continue;
            float e = 0;
            if (!ntrans && have_anchor) {
                for (uint32_t k = 0; k < TS_HOP; k++) {
                    double cp = (double)q + (double)k * pitch;
                    uint32_t ci = (uint32_t)cp;
                    if (ci + 1 >= (long)nin) { e = -1e30f; break; }
                    float f2 = (float)(cp - (double)ci);
                    float cv = in[ci]*(1.0f-f2) + in[ci+1]*f2;
                    e += cv * anchor[k];
                }
            } else if (have_anchor) {
                int straddles = 0;
                for (uint32_t t = 0; t < ntrans; t++)
                    if (trans[t] > q - TS_FRAME && trans[t] < q + 2*TS_HOP)
                        { straddles = 1; break; }
                if (straddles) continue;
                for (uint32_t k = 0; k < TS_HOP; k++)
                    e += in[q + k] * anchor[k];
            } else {
                /* no written tail yet: prefer highest energy for the first
                 * frame so we start on strong material */
                for (uint32_t k = 0; k < TS_FRAME; k += 4)
                    e -= fabsf(in[q + k]);
            }
            if (e > best_e) { best_e = e; best_d = d; }
        }
        uint32_t p = (uint32_t)((long)src_pos + best_d);
        if (p + TS_FRAME >= nin) break;
        /* overlap-add with linear crossfade over the first TS_HOP samples.
         * R073 hop 3: simultaneous pitch — frame samples are read at step
         * `pitch`, so pitch shifts in the SAME pass as time (no chained
         * stretch+resample, no compounding artifacts). */
        for (uint32_t k = 0; k < TS_FRAME; k++) {
            float w_in  = k < TS_HOP && out_pos + k > 0
                          ? (float)k / TS_HOP : 1.0f;
            float w_old = 1.0f - w_in;
            /* R073 hop 6: power-COLA normalization — a linear crossfade dips
             * energy mid-splice (w²+(1-w)² < 1); dividing by the summed
             * weight energy keeps loudness flat across every splice. */
            float wnorm = sqrtf(w_in * w_in + w_old * w_old);
            if (wnorm < 1e-6f) wnorm = 1.0f;
            double sp = (double)p + (double)k * pitch;
            uint32_t s0 = (uint32_t)sp;
            if (s0 + 1 >= nin || out_pos + k >= max_out) break;
            float f = (float)(sp - (double)s0);
            float v = in[s0] * (1.0f - f) + in[s0 + 1] * f;
            out[out_pos + k] =
                (out[out_pos + k] * w_old + v * w_in) / wnorm;
        }
        /* save the ideal next-overlap segment from the INPUT for the next
         * search (this is what makes WSOLA 'waveform similarity' work) */
        for (uint32_t k = 0; k < TS_HOP; k++) {
            double ap2 = (double)p + (double)(TS_HOP + k) * pitch;
            uint32_t ai = (uint32_t)ap2;
            anchor[k] = ai + 1 < nin
                ? in[ai] * (1.0f - (float)(ap2 - ai)) + in[ai+1] * (float)(ap2 - ai)
                : 0.0f;
        }
        have_anchor = 1;
        out_pos += TS_HOP;
        src_pos += read_hop;
    }
    *outp = out;
    return out_pos + TS_FRAME;
}

/* Public API: stretch/pitch an interleaved stereo clip into a new buffer.
 * rate: time factor (>1 faster). semitones: pitch shift after stretching.
 * Returns output frame count, *outp owned by caller. 0 on error. */
uint32_t wb_timestretch_tr(const wb_sample *in, uint32_t frames,
                           uint32_t chn, double rate, double semitones,
                           const uint32_t *trans, uint32_t ntrans,
                           wb_sample **outp) {
    if (!in || !outp || frames == 0 || chn == 0 ||
        rate <= 0.01 || rate > 16.0) return 0;
    if (fabs(semitones) > 24.0) return 0;

    float *mono = mono_of(in, frames, chn);
    if (!mono) return 0;

    /* R073 hop 3: simultaneous pitch+time — one WSOLA pass with a per-sample
     * read step of 2^(st/12). Time ratio comes from rate, pitch from the
     * step; no chained stretch->resample so artifacts don't compound.
     * R073 hop 4: tonality limit — above TONALITY_HZ the shift fades out
     * (cymbals/breath keep their natural character), via a 2-band split:
     * only the low band is pitched; the high band rides a pitch-free pass. */
    double resamp = pow(2.0, semitones / 12.0);
    float *stretched = NULL;
    uint32_t ns;
    if (fabs(semitones) >= 0.001) {
        enum { TONALITY_HZ = 5000 };
        float *low = malloc((size_t)frames * sizeof(float));
        float *high = malloc((size_t)frames * sizeof(float));
        if (!low || !high) { free(low); free(high); free(mono); return 0; }
        band_split(mono, frames, TONALITY_HZ, WB_SAMPLE_RATE, low, high);
        float *slow = NULL, *shigh = NULL;
        uint32_t n1 = wsola(low, frames, rate, resamp,
                            trans, ntrans, &slow);
        uint32_t n2 = wsola(high, frames, rate, 1.0,
                            trans, ntrans, &shigh);
        free(low); free(high);
        free(mono);
        if (!n1 || !n2) { free(slow); free(shigh); return 0; }
        ns = n1 < n2 ? n1 : n2;
        stretched = malloc((size_t)ns * sizeof(float));
        if (!stretched) { free(slow); free(shigh); return 0; }
        for (uint32_t i = 0; i < ns; i++)
            stretched[i] = slow[i] + shigh[i];
        free(slow); free(shigh);
    } else {
        ns = wsola(mono, frames, rate, 1.0, trans, ntrans, &stretched);
        free(mono);
        if (!ns) return 0;
    }

    /* output is dual-mono at native rate */
    wb_sample *out = calloc((size_t)ns * 2, sizeof(wb_sample));
    if (!out) { free(stretched); return 0; }
    for (uint32_t i = 0; i < ns; i++) {
        out[i*2] = stretched[i];
        out[i*2+1] = stretched[i];
    }
    free(stretched);
    *outp = out;
    return ns;
}

/* Compat: stretch without explicit transients (detector-free callers). */
uint32_t wb_timestretch(const wb_sample *in, uint32_t frames, uint32_t chn,
                        double rate, double semitones, wb_sample **outp) {
    return wb_timestretch_tr(in, frames, chn, rate, semitones,
                             NULL, 0, outp);
}
