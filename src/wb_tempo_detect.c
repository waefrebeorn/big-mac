/* wb_tempo_detect.c — autocorrelation-based BPM detection from an audio buffer.
 * Pure C11, zero third-party. Classic onset-energy -> differentiate ->
 * autocorrelate -> pick peak in 60..200 BPM (Ableton-style "Detect BPM").
 *
 * Algorithm:
 *   1. Compute energy envelope, downsample to ~100 Hz control rate.
 *   2. Half-wave-rectified first difference => onset strength function.
 *   3. Subtract local mean so periodic accents dominate.
 *   4. Autocorrelate; scan lags mapping to 60..200 BPM.
 *   5. Log-normal tempo prior (centered 120 BPM) breaks octave ties.
 *   6. Octave-error correction: prefer double-BPM when half-lag correlates
 *      comparably (Gouyon/Klapuri heuristic).
 *   7. Confidence = normalized peak autocorrelation score (0..1). */

#include "wbus.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ~100 Hz control rate: hop = SR/100. For SR=44100 => hop=441. */
#define TD_HOP(sr) ((sr) / 100)
#define TD_MIN_BPM 60.0
#define TD_MAX_BPM 200.0
#define TD_PRIOR_CENTER 120.0
#define TD_PRIOR_SIGMA 1.0   /* log2 half-width of the tempo prior */
#define TD_OCTAVE_THRESH 0.9 /* half-lag corr >= this * win-lag corr => 2x */

static float g_last_confidence = 0.0f;

float wb_tempo_detect_confidence(void) { return g_last_confidence; }

float wb_tempo_detect(const wb_sample *audio, uint32_t frames,
                      uint32_t sample_rate) {
    g_last_confidence = 0.0f;
    if (!audio || frames < (uint32_t)(sample_rate / 2) || sample_rate == 0)
        return 0.0f;

    uint32_t hop = TD_HOP(sample_rate);
    if (hop < 32) hop = 32; /* guard very low sample rates */
    uint32_t nframes = frames / hop;
    if (nframes < 32) return 0.0f;

    float *flux = (float *)calloc(nframes, sizeof(float));
    if (!flux) return 0.0f;

    /* 1+2: energy envelope (abs sample) then half-wave-rectified difference. */
    for (uint32_t f = 0; f < nframes; f++) {
        float d = 0.0f;
        const wb_sample *base = audio + (size_t)f * hop;
        for (uint32_t i = 1; i < hop; i++) {
            float dv = fabsf(base[i]) - fabsf(base[i - 1]);
            if (dv > 0.0f) d += dv;
        }
        flux[f] = d;
    }

    /* 3: subtract local mean so periodic accents dominate.
     * Also measure flux impulsiveness — the ratio of the strongest onset
     * to the mean onset. Click trains have a few large spikes on a near-zero
     * floor (high ratio); pure tones have uniform low energy (ratio ~1). */
    float mean = 0.0f;
    for (uint32_t f = 0; f < nframes; f++) mean += flux[f];
    mean /= (float)nframes;
    float maxf = 0.0f;
    for (uint32_t f = 0; f < nframes; f++) {
        flux[f] = flux[f] > mean ? flux[f] - mean : 0.0f;
        if (flux[f] > maxf) maxf = flux[f];
    }
    /* Compute mean of positive (non-zero) flux frames. */
    float flux_sum = 0.0f; int flux_nz = 0;
    for (uint32_t f = 0; f < nframes; f++) {
        if (flux[f] > 0.0f) { flux_sum += flux[f]; flux_nz++; }
    }
    float flux_mean_pos = flux_nz > 0 ? flux_sum / (float)flux_nz : 0.0f;
    /* Impulsiveness: max/mean of positive frames. Click train ~3-10, tone ~1-2.
     * Map to 0..1: (ratio-1)/2 clamped. */
    float impulsiveness = 0.0f;
    if (flux_mean_pos > 1e-12f) {
        float ratio = maxf / flux_mean_pos;
        impulsiveness = (ratio - 1.0f) / 2.0f;
        if (impulsiveness > 1.0f) impulsiveness = 1.0f;
        if (impulsiveness < 0.0f) impulsiveness = 0.0f;
    }

    double fps = (double)sample_rate / (double)hop; /* control frames/sec */

    /* 4+5: scan BPMs, autocorrelate at the matching lag, apply tempo prior. */
    /* Also accumulate mean autocorrelation for peak-prominence confidence. */
    double best_corr = -1e30, best_bpm = 0.0;
    double corr_sum = 0.0;
    int corr_count = 0;
    for (double bpm = TD_MIN_BPM; bpm <= TD_MAX_BPM; bpm += 0.5) {
        double lag_f = 60.0 * fps / bpm;
        uint32_t lag = (uint32_t)(lag_f + 0.5);
        if (lag < 2 || lag + 2 >= nframes) continue;
        double corr = 0.0;
        for (uint32_t f = 0; f + lag < nframes; f++)
            corr += flux[f] * flux[f + lag];
        corr /= (double)(nframes - lag);

        /* Log-normal tempo prior centered at 120 BPM. */
        double logratio = log2(bpm / TD_PRIOR_CENTER) / TD_PRIOR_SIGMA;
        corr *= exp(-0.5 * logratio * logratio);

        if (corr > best_corr) { best_corr = corr; best_bpm = bpm; }
        corr_sum += corr;
        corr_count++;
    }

    if (best_bpm <= 0.0 || best_corr <= 0.0) { free(flux); return 0.0f; }

    /* 6: octave-error correction. If the half-lag correlates comparably to
     * the winning lag, the true tempo is 2x (click trains match at 2x period). */
    if (best_bpm * 2.0 <= TD_MAX_BPM) {
        uint32_t half_lag = (uint32_t)(60.0 * fps / best_bpm / 2.0 + 0.5);
        if (half_lag >= 2 && half_lag + 2 < nframes) {
            double corr2 = 0.0, corr_win = 0.0;
            uint32_t win_lag = (uint32_t)(60.0 * fps / best_bpm + 0.5);
            for (uint32_t f = 0; f + half_lag < nframes; f++)
                corr2 += flux[f] * flux[f + half_lag];
            corr2 /= (double)(nframes - half_lag);
            if (win_lag >= 2 && win_lag + 2 < nframes) {
                for (uint32_t f = 0; f + win_lag < nframes; f++)
                    corr_win += flux[f] * flux[f + win_lag];
                corr_win /= (double)(nframes - win_lag);
            }
            if (corr_win > 0.0 && corr2 >= corr_win * TD_OCTAVE_THRESH)
                best_bpm *= 2.0;
        }
    }

    /* 7: confidence = peak prominence × dynamic range factor.
     * Peak prominence: ratio of best autocorrelation to mean across lags.
     * Dynamic range: how spiky the onset function is (0=flat, 1=impulsive).
     * A pure tone has a flat onset function => dyn_range ~0 => low confidence.
     * A click train has both a sharp autocorrelation peak AND spiky flux. */
    if (corr_count > 0) {
        double mean_corr = corr_sum / (double)corr_count;
        if (mean_corr > 0.0) {
            double ratio = best_corr / mean_corr; /* >= 1 for any peak */
            /* Map ratio: 1.0 -> 0.0, 2.0 -> 0.5, 4.0 -> 0.8, large -> ~1.0 */
            double prom = (ratio - 1.0) / (ratio + 1.0); /* 0..1, gentle */
            double peak_conf = prom * 2.0;                /* sharpen */
            if (peak_conf > 1.0) peak_conf = 1.0;
            if (peak_conf < 0.0) peak_conf = 0.0;
            g_last_confidence = (float)(peak_conf * impulsiveness);
        }
    }

    free(flux);
    return (float)best_bpm;
}
