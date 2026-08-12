/*
 * wb_esynth.c — espeak-ng's proven additive-formant synthesis, faithfully
 * ported to pure C11 (R015 fresh start). No third-party, every byte ours.
 *
 * The engine (from espeak-ng wavegen.c PeaksToHarmspect + WavegenFill):
 *   - Each formant is a PEAK {freq, height, left_BW, right_BW} (F1..F5 here).
 *   - Every harmonic's amplitude is built by summing a peak-shape window
 *     contribution from each nearby formant peak:
 *         htab[h] += pk_shape[(distance_from_peak)/bandwidth] * peak.height
 *   - Plus a bass boost (stronger low harmonics) and tone adjustment.
 *   - The waveform = sum over harmonics of sin(harmonic_phase) * harmonic_amp,
 *     scaled by a per-phone amplitude.
 *
 * The per-phone peaks are supplied by the caller (wb_esynth_phone_t): vowels
 * use Peterson-Barney formants with per-vowel amplitudes; consonants use
 * their formant loci. This is the control that produced intelligible speech
 * in espeak-ng and which my earlier invented renderers lacked.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "wb_esynth.h"

/* ---- peak-shape window (espeak-ng pk_shape1): 255 at center -> 0 at edge ---- */
#define PEAK_SHAPE_W 160
static const unsigned char PK_SHAPE[PEAK_SHAPE_W + 1] = {
    255,254,254,254,254,254,253,253,252,251,251,250,249,248,247,246,
    245,244,242,241,239,238,236,234,233,231,229,227,225,223,220,218,
    216,213,211,209,207,205,203,201,199,197,195,193,191,189,187,185,
    183,180,178,176,173,171,169,166,164,161,159,156,154,151,148,146,
    143,140,138,135,132,129,126,123,120,118,115,112,108,105,102,99,
    96,95,93,91,90,88,86,85,83,82,80,79,77,76,74,73,
    72,70,69,68,67,66,64,63,62,61,60,59,58,57,56,55,
    55,54,53,52,52,51,50,50,49,48,48,47,47,46,46,46,
    45,45,45,44,44,44,44,44,44,44,43,43,43,43,44,43,
    42,42,41,40,40,39,38,38,37,36,36,35,35,34,33,33,
    32,32,31,30,30,29,29,28,28,27,26,26,25,25,24,24,
    23,23,22,22,21,21,20,20,19,19,18,18,18,17,17,16,
    16,15,15,15,14,14,13,13,13,12,12,11,11,11,10,10,
    10,9,9,9,8,8,8,7,7,7,7,6,6,6,5,5,
    5,5,4,4,4,4,4,3,3,3,3,2,2,2,2,2,
    2,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,
    0
};

struct wb_esynth {
    int sr;                 /* sample rate */
    double phase[64];       /* per-harmonic phase accumulators */
};

wb_esynth_t *wb_esynth_new(int sample_rate) {
    wb_esynth_t *s = calloc(1, sizeof(*s));
    if (s) s->sr = sample_rate;
    return s;
}
void wb_esynth_free(wb_esynth_t *s) { free(s); }

/* PeaksToHarmspect: build per-harmonic amplitudes from the formant peaks.
 * peaks[0..npeaks-1] are formants; returns the highest harmonic with energy. */
static int wb_peaks_to_harmspect(const wb_esynth_peak_t *peaks, int npeaks,
                                 double f0, int sr, double htab[64]) {
    int hmax = 0;
    for (int h = 0; h < 64; h++) htab[h] = 0.0;
    /* each formant peak contributes a pk_shape bump to harmonics near it */
    for (int pk = 0; pk < npeaks; pk++) {
        double fp = peaks[pk].freq;
        if (peaks[pk].height == 0 || fp <= 0) continue;
        double fhi = fp + peaks[pk].right;   /* peak right edge */
        double flo = fp - peaks[pk].left;    /* peak left edge */
        int hlo = (int)(flo / f0); if (hlo < 1) hlo = 1;
        int hhi = (int)(fhi / f0);
        for (int h = hlo; h <= hhi && h < 64; h++) {
            double f = h * f0;
            double d;
            if (f < fp) d = (fp - f) / (peaks[pk].left > 1 ? peaks[pk].left : 1);
            else        d = (f - fp) / (peaks[pk].right > 1 ? peaks[pk].right : 1);
            /* R015: espeak-ng indexes the peak shape by (freq_diff/bw)*256
             * (freqs are Hz<<16, bw>>8), so a peak is ~0.625*bw wide, NOT
             * bw*160. Without the *256 my peaks smeared across the whole
             * spectrum, making every vowel too bright. */
            int ix = (int)(d * 256.0);
            if (ix > PEAK_SHAPE_W) ix = PEAK_SHAPE_W;
            htab[h] += PK_SHAPE[ix] / 255.0 * peaks[pk].height;
            if (h > hmax) hmax = h;
        }
    }
    /* bass boost: strengthen low harmonics (espeak-ng: y = peaks[1].height*10,
     * decreasing to zero by 1000Hz). This is what makes low vowels /u/ /a/
     * dark — without enough of it, the high formants push the centroid up. */
    if (npeaks > 1) {
        double y = peaks[1].height * 0.6;      /* F1-height-scaled bass */
        double step = y / (1000.0 / f0);        /* decay to ~0 by 1000Hz */
        int h = 1;
        while (y > 0.0 && h < 64) { htab[h] += y; y -= step; h++; }
    }
    return hmax;
}

/* Render one phone: additive harmonic synthesis driven by per-phone peaks.
 * out must be nsamp samples. */
void wb_esynth_phone(wb_esynth_t *s, double *out, int nsamp,
                     const wb_esynth_phone_t *ph) {
    int sr = s->sr;
    double htab[64];
    int hmax = wb_peaks_to_harmspect(ph->peaks, ph->npeaks, ph->f0, sr, htab);
    if (hmax > 60) hmax = 60;
    /* fundamental phase advances each sample; harmonics use h * waveph */
    double waveph = 0;
    for (int i = 0; i < nsamp; i++) {
        double total = 0;
        double theta = waveph;
        for (int h = 1; h <= hmax; h++) {
            total += sin(theta) * htab[h];
            theta += waveph;
        }
        /* amplitude envelope: smooth on/off */
        int nen = (int)(0.01 * sr);
        double env = 1.0;
        if (i < nen) env = (double)i / nen;
        if (nsamp - i - 1 < nen) env = (double)(nsamp - i - 1) / nen;
        out[i] += total * ph->amplitude * env;
        waveph += 2.0 * M_PI * ph->f0 / sr;
        if (waveph > 2.0 * M_PI) waveph -= 2.0 * M_PI;
    }
}
