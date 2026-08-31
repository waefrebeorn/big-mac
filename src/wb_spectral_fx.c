/* wb_spectral_fx.c — spectral effects (Ableton-style).
 * Pure C11, zero third-party. Uses wb_fft for FFT/IFFT.
 * Three modes:
 *   0 = Resonator  — boost bins at harmonic frequencies
 *   1 = Blur       — average adjacent bins (spectral smearing)
 *   2 = Time       — freeze spectrum, feed back into reverb tail
 * Frame size 2048, 4x overlap (hop 512). */
#include "wbus/wbus_spectral_fx.h"
#include "wbus/wbus_fft.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define SF_FFT_N    2048
#define SF_HOP      512
#define SF_MAX_FRZN 4

typedef struct {
    float amount;       /* 0..1 effect depth */
    float frequency;    /* resonator base freq / blur center / time param */
    float decay;        /* resonator harmonic decay / blur width / time feedback */
    float mix;          /* 0..1 dry/wet */
    int   type;         /* 0=resonator, 1=blur, 2=time */
    uint32_t sr;

    wb_fft_plan *plan;
    double accum[SF_FFT_N];         /* overlap-add synthesis accumulator */
    double input_buf[SF_FFT_N];     /* frame assembly buffer */
    int    input_pos;               /* write position in input_buf */
    double window[SF_FFT_N];        /* Hann window */

    /* Time-freeze state */
    double frozen_mag[SF_MAX_FRZN][SF_FFT_N / 2 + 1];
    double frozen_phase[SF_MAX_FRZN][SF_FFT_N / 2 + 1];
    int    frozen_count;
    int    freeze_active;

    /* Scratch */
    double re[SF_FFT_N];
    double im[SF_FFT_N];
    double mag[SF_FFT_N / 2 + 1];
    double phase[SF_FFT_N / 2 + 1];
} wb_spectral_fx;

/* ---- Hann window ---- */
static void make_hann(double *w, int n) {
    for (int i = 0; i < n; i++)
        w[i] = 0.5 * (1.0 - cos(2.0 * M_PI * (double)i / (double)(n - 1)));
}

/* ---- param IDs ---- */
/* 0=amount, 1=frequency, 2=decay, 3=mix */

/* ---- extract magnitude/phase ---- */
static void spectrum_extract(const double *re, const double *im,
                             double *mag, double *phase, int n) {
    int bins = n / 2 + 1;
    for (int i = 0; i < bins; i++) {
        mag[i] = sqrt(re[i] * re[i] + im[i] * im[i]);
        phase[i] = atan2(im[i], re[i]);
    }
}

/* ---- Resonator: boost bins at harmonic frequencies ---- */
static void process_resonator(wb_spectral_fx *sf) {
    int bins = SF_FFT_N / 2 + 1;
    double base_bin = (double)sf->frequency * (double)SF_FFT_N / (double)sf->sr;
    double boost = 1.0 + (double)sf->amount * 8.0; /* up to 9x */
    double harm_decay = (double)sf->decay;

    for (int h = 1; h <= 32; h++) {
        double hb = base_bin * (double)h;
        if (hb >= (double)bins) break;
        double amp = boost * pow(harm_decay, (double)(h - 1));
        if (amp < 1.01) break;

        int center = (int)round(hb);
        int width = 1 + (int)(2.0 * (1.0 - (double)sf->amount * 0.5));
        for (int b = center - width; b <= center + width; b++) {
            if (b <= 0 || b >= bins) continue;
            double dist = fabs((double)b - hb) / (double)(width > 0 ? width : 1);
            if (dist > 1.0) continue;
            double gain = 1.0 + (amp - 1.0) * (1.0 - dist) * (1.0 - dist);
            sf->re[b] *= gain;
            sf->im[b] *= gain;
            /* conjugate symmetric bin */
            int mb = SF_FFT_N - b;
            if (mb > 0 && mb < SF_FFT_N && mb != b) {
                sf->re[mb] *= gain;
                sf->im[mb] *= gain;
            }
        }
    }
}

/* ---- Blur: average adjacent bins (spectral smearing) ---- */
static void process_blur(wb_spectral_fx *sf) {
    int bins = SF_FFT_N / 2 + 1;
    int width = 1 + (int)((double)sf->amount * 20.0);

    double *tmp_re = (double *)malloc((size_t)bins * sizeof(double));
    double *tmp_im = (double *)malloc((size_t)bins * sizeof(double));
    if (!tmp_re || !tmp_im) { free(tmp_re); free(tmp_im); return; }

    for (int i = 0; i < bins; i++) {
        tmp_re[i] = sf->re[i];
        tmp_im[i] = sf->im[i];
    }

    /* box blur across frequency bins */
    for (int i = 0; i < bins; i++) {
        double sr = 0.0, si = 0.0;
        int cnt = 0;
        int lo = i - width; if (lo < 0) lo = 0;
        int hi = i + width; if (hi >= bins) hi = bins - 1;
        for (int j = lo; j <= hi; j++) {
            sr += tmp_re[j];
            si += tmp_im[j];
            cnt++;
        }
        if (cnt > 0) {
            sf->re[i] = sr / (double)cnt;
            sf->im[i] = si / (double)cnt;
        }
    }

    /* preserve DC and Nyquist */
    sf->re[0] = tmp_re[0];
    sf->im[0] = 0.0;
    sf->re[bins - 1] = tmp_re[bins - 1];
    sf->im[bins - 1] = 0.0;

    /* rebuild conjugate symmetry */
    for (int i = bins; i < SF_FFT_N; i++) {
        sf->re[i] = sf->re[SF_FFT_N - i];
        sf->im[i] = -sf->im[SF_FFT_N - i];
    }

    free(tmp_re);
    free(tmp_im);
}

/* ---- Time: freeze spectrum, feed back into reverb tail ---- */
static void process_time(wb_spectral_fx *sf) {
    int bins = SF_FFT_N / 2 + 1;

    /* trigger freeze when amount crosses up */
    if (sf->amount > 0.55f && !sf->freeze_active) {
        sf->freeze_active = 1;
        sf->frozen_count = 0;
    }
    /* release when amount drops */
    if (sf->amount < 0.35f) {
        sf->freeze_active = 0;
    }

    if (sf->freeze_active) {
        /* capture current spectrum */
        if (sf->frozen_count < SF_MAX_FRZN) {
            int idx = sf->frozen_count;
            for (int i = 0; i < bins; i++) {
                sf->frozen_mag[idx][i] = sf->mag[i];
                sf->frozen_phase[idx][i] = sf->phase[i];
            }
            sf->frozen_count++;
        }

        /* rebuild from frozen average + feedback */
        double fb = (double)sf->decay;
        for (int i = 0; i < bins; i++) {
            double avg_mag = 0.0;
            for (int f = 0; f < sf->frozen_count; f++)
                avg_mag += sf->frozen_mag[f][i];
            avg_mag /= (double)(sf->frozen_count > 0 ? sf->frozen_count : 1);

            double m = fb * avg_mag + (1.0 - fb) * sf->mag[i];
            double ph = sf->phase[i];
            sf->re[i] = m * cos(ph);
            sf->im[i] = m * sin(ph);
        }
        /* conjugate symmetry */
        for (int i = bins; i < SF_FFT_N; i++) {
            sf->re[i] = sf->re[SF_FFT_N - i];
            sf->im[i] = -sf->im[SF_FFT_N - i];
        }
    }
}

/* ---- create / destroy ---- */
void *wb_spectral_fx_create(uint32_t sr) {
    wb_spectral_fx *sf = (wb_spectral_fx *)calloc(1, sizeof(wb_spectral_fx));
    if (!sf) return NULL;
    sf->sr = sr > 0 ? sr : 44100;
    sf->plan = wb_fft_create(SF_FFT_N);
    if (!sf->plan) { free(sf); return NULL; }
    make_hann(sf->window, SF_FFT_N);
    sf->type = 0;
    sf->amount = 0.5f;
    sf->frequency = 440.0f;
    sf->decay = 0.5f;
    sf->mix = 0.5f;
    return sf;
}

void wb_spectral_fx_destroy(void *sf) {
    if (!sf) return;
    wb_spectral_fx *p = (wb_spectral_fx *)sf;
    if (p->plan) wb_fft_destroy(p->plan);
    free(p);
}

void wb_spectral_fx_set_type(void *sf, int type) {
    if (!sf) return;
    wb_spectral_fx *p = (wb_spectral_fx *)sf;
    if (type < 0) type = 0;
    if (type > 2) type = 2;
    p->type = type;
    p->freeze_active = 0;
    p->frozen_count = 0;
}

void wb_spectral_fx_set_param(void *sf, int param, float value) {
    if (!sf) return;
    wb_spectral_fx *p = (wb_spectral_fx *)sf;
    switch (param) {
        case 0: /* amount */
            if (value < 0.0f) value = 0.0f;
            if (value > 1.0f) value = 1.0f;
            p->amount = value;
            break;
        case 1: /* frequency */
            if (value < 20.0f) value = 20.0f;
            if (value > 20000.0f) value = 20000.0f;
            p->frequency = value;
            break;
        case 2: /* decay */
            if (value < 0.0f) value = 0.0f;
            if (value > 0.999f) value = 0.999f;
            p->decay = value;
            break;
        case 3: /* mix */
            if (value < 0.0f) value = 0.0f;
            if (value > 1.0f) value = 1.0f;
            p->mix = value;
            break;
        default: break;
    }
}

/* ---- process: overlap-add STFT with effect ---- */
void wb_spectral_fx_process(void *sf, wb_sample *out, const wb_sample *in, uint32_t frames) {
    if (!sf || !out || !in) return;
    wb_spectral_fx *p = (wb_spectral_fx *)sf;

    uint32_t in_pos = 0;
    uint32_t out_pos = 0;

    while (out_pos < frames) {
        /* Fill input_buf toward FFT_N */
        while (p->input_pos < SF_FFT_N && in_pos < frames) {
            p->input_buf[p->input_pos++] = (double)in[in_pos++];
        }

        if (p->input_pos < SF_FFT_N) {
            /* Not enough input remaining — if we have partial, zero-pad and do one last frame */
            if (p->input_pos > 0) {
                while (p->input_pos < SF_FFT_N)
                    p->input_buf[p->input_pos++] = 0.0;
            } else {
                break;
            }
        }

        /* Window + forward FFT */
        for (int i = 0; i < SF_FFT_N; i++) {
            p->re[i] = p->input_buf[i] * p->window[i];
            p->im[i] = 0.0;
        }
        wb_fft_run(p->plan, p->re, p->im, 0);
        spectrum_extract(p->re, p->im, p->mag, p->phase, SF_FFT_N);

        /* Apply selected effect */
        switch (p->type) {
            case 0: process_resonator(p); break;
            case 1: process_blur(p); break;
            case 2: process_time(p); break;
            default: break;
        }

        /* Inverse FFT */
        double tmp[SF_FFT_N];
        wb_fft_real_inverse(p->plan, p->re, p->im, tmp);

        /* Synthesis window + overlap-add into accum */
        for (int i = 0; i < SF_FFT_N; i++) {
            p->accum[i] += tmp[i] * p->window[i];
        }

        /* Output HOP samples from accum (dry/wet mix) */
        uint32_t hop = SF_HOP;
        if (out_pos + hop > frames) hop = frames - out_pos;
        double mix = (double)p->mix;
        for (uint32_t i = 0; i < hop; i++) {
            double wet = p->accum[i];
            double dry = p->input_buf[i];
            double result = dry * (1.0 - mix) + wet * mix;
            if (result > 1.0) result = 1.0;
            if (result < -1.0) result = -1.0;
            out[out_pos++] = (wb_sample)result;
        }

        /* Shift accum by HOP */
        memmove(p->accum, p->accum + SF_HOP, (SF_FFT_N - SF_HOP) * sizeof(double));
        memset(p->accum + (SF_FFT_N - SF_HOP), 0, SF_HOP * sizeof(double));

        /* Shift input_buf by HOP for overlap */
        int keep = SF_FFT_N - SF_HOP;
        memmove(p->input_buf, p->input_buf + SF_HOP, keep * sizeof(double));
        p->input_pos = keep;
    }
}