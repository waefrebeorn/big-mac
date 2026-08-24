/* wb_cgi_bands.c — R073 hop 61: frequency-band audio-reactive CGI.
 *
 * Extends the visualizer workflow: each object can react to its own
 * frequency band (bass/mid/high) via a real FFT of windowed windows.
 * Per window: FFT the mono downmix, sum magnitude in [lo,hi) Hz, normalize,
 * bake scale keys. Pure C11, uses the existing wb_fft module.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus/wbus.h"
#include "wbus/wbus_fft.h"
#include "wbus/wbus_anim.h"

int wb_cgi_band_pulse(wb_anim *a, int obj,
                      const wb_sample *audio, uint32_t frames,
                      uint32_t chn, double dur_secs,
                      float lo_hz, float hi_hz,
                      float base, float amount) {
    if (!a || !audio || frames == 0 || chn == 0 || dur_secs <= 0)
        return -1;
    if (!(lo_hz < hi_hz)) return -1;

    const int N = 1024;                        /* FFT size per window */
    wb_fft_plan *plan = wb_fft_create(N);
    if (!plan) return -1;

    const int wins = 16;                       /* keys across the duration */
    uint32_t hop = frames / (uint32_t)wins;
    if (hop < (uint32_t)N) hop = N;

    float sr = (float)WB_SAMPLE_RATE;
    double *re = malloc(N * sizeof(double));
    double *im = malloc(N * sizeof(double));
    if (!re || !im) { free(re); free(im); wb_fft_destroy(plan); return -1; }

    int wrote = 0;
    for (int w2 = 0; w2 < wins; w2++) {
        uint32_t s0 = (uint32_t)((double)w2 / wins * frames);
        if (s0 + N > frames) break;

        /* mono downmix + Hann window */
        for (int i = 0; i < N; i++) {
            float m = 0;
            for (uint32_t c2 = 0; c2 < chn; c2++)
                m += audio[(s0 + i) * chn + c2];
            m /= chn;
            double win = 0.5 * (1.0 - cos(2.0 * M_PI * i / (N - 1)));
            re[i] = m * win;
            im[i] = 0.0;
        }
        wb_fft_run(plan, re, im, 0);

        /* band energy in [lo_hz, hi_hz) */
        int bin_lo = (int)(lo_hz / sr * N);
        int bin_hi = (int)(hi_hz / sr * N);
        if (bin_hi > N/2) bin_hi = N/2;
        double mag = 0;
        for (int b = bin_lo; b < bin_hi && b < N/2; b++) {
            mag += sqrt(re[b]*re[b] + im[b]*im[b]);
        }
        mag /= (bin_hi - bin_lo > 0 ? bin_hi - bin_lo : 1);

        /* running normalization is handled by the caller comparing keys —
         * here use a soft compression so loud bands don't dominate */
        double norm = mag / (mag + 1e-3f);     /* 0..1-ish */
        float scale = base + amount * (float)(norm * 2.0);

        double t = (double)w2 * dur_secs / wins;
        if (wb_anim_key(a, obj, t,
                        0, 0, -2,
                        0.0f, (float)(norm * 6.283), 0.0f,
                        scale) == 0)
            wrote++;
    }
    free(re); free(im);
    wb_fft_destroy(plan);
    return wrote;
}

/* ---- R073 hop 62: one-call visualizer builder ------------------------------ */
/* Builds a 3-sphere scene (bass/mid/high) and bakes each object's band
 * response as keys. Returns 0 or -1. Meshes owned by the anim's caller? No:
 * this helper keeps them alive in static storage tied to the anim via
 * caller responsibility — see PITFALL: anim stores mesh pointers, so the
 * meshes are intentionally leaked into `*meshes_out` for the caller to free
 * after wb_anim_free. */
int wb_cgi_visualizer_build(wb_anim *a,
                            const wb_sample *audio, uint32_t frames,
                            uint32_t chn, double dur_secs,
                            float base, float amount,
                            wb_mesh **meshes_out /*[3]*/) {
    if (!a || !audio || !meshes_out || dur_secs <= 0) return -1;
    struct band { float lo, hi; uint8_t r,g,b; } bd[3] = {
        { 40.0f, 250.0f, 255, 80, 60 },    /* bass  */
        { 250.0f, 2000.0f, 80, 255, 120 }, /* mid   */
        { 2000.0f, 9000.0f, 90, 160, 255 } /* high  */
    };
    for (int i = 0; i < 3; i++) {
        wb_mesh *m = wb_mesh_sphere(5 + i, 8, 12,
                                    bd[i].r, bd[i].g, bd[i].b);
        if (!m) return -1;
        int obj = wb_anim_add_object(a, m, bd[i].r, bd[i].g, bd[i].b);
        int wrote = wb_cgi_band_pulse(a, obj, audio, frames, chn,
                                      dur_secs,
                                      bd[i].lo, bd[i].hi,
                                      base, amount);
        if (wrote < 0) { wb_mesh_free(m); return -1; }
        meshes_out[i] = m;   /* caller frees after wb_anim_free */
    }
    return 0;
}
