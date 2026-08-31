/* wb_warp.c — R078: Ableton-style warp markers for elastic audio.
 *
 * Warp markers remap the source audio timeline to a musical (beat) timeline.
 * Between adjacent markers, linear time-stretching is applied via wb_timestretch
 * so audio conforms to the beat grid without pitch shift.
 *
 *   marker[i] = (src_sample_i, dst_beat_i)
 *
 * A virtual marker at (0, 0) is always implicit — audio before the first
 * explicit marker is stretched from (0,0) to that marker. Markers are kept
 * sorted by src_sample. With zero explicit markers the mapping is 1:1
 * (pass-through).
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <wbus/wbus.h>

#ifndef WB_WARP_MAX_MARKERS
#define WB_WARP_MAX_MARKERS 256
#endif

/* A single warp marker: maps a source sample position to a beat position. */
typedef struct {
    double src_sample;  /* position in source audio (samples)  */
    double dst_beat;    /* corresponding musical position (beats) */
} wb_warp_marker;

/* Warp marker set + cached source audio.
 * Opaque struct — users interact via the wb_warp_* API. */
struct wb_warp {
    uint32_t           sr;          /* sample rate                    */
    wb_sample          *src;        /* interleaved source audio       */
    uint32_t           src_frames;  /* source length in frames        */
    uint32_t           src_chn;     /* source channel count           */

    wb_warp_marker      markers[WB_WARP_MAX_MARKERS];
    int                 marker_count;
};

/* ---- creation / destruction -------------------------------------------- */

wb_warp *wb_warp_create(uint32_t sr) {
    if (sr == 0) sr = WB_SAMPLE_RATE;
    wb_warp *w = calloc(1, sizeof(wb_warp));
    if (!w) return NULL;
    w->sr = sr;
    return w;
}

void wb_warp_destroy(wb_warp *w) {
    if (!w) return;
    free(w->src);
    free(w);
}

/* ---- source audio ------------------------------------------------------- */

int wb_warp_set_source(wb_warp *w, const wb_sample *audio,
                       uint32_t frames, uint32_t channels) {
    if (!w || !audio || frames == 0 || channels == 0)
        return -1;
    free(w->src);
    size_t n = (size_t)frames * channels;
    w->src = malloc(n * sizeof(wb_sample));
    if (!w->src) { w->src_frames = 0; w->src_chn = 0; return -1; }
    memcpy(w->src, audio, n * sizeof(wb_sample));
    w->src_frames = frames;
    w->src_chn = channels;
    return 0;
}

/* ---- marker management -------------------------------------------------- */

/* Insert a marker keeping the array sorted by src_sample.
 * Returns the new count, or -1 on error / duplicate / full. */
static int insert_marker(wb_warp *w, double src_sample, double dst_beat) {
    if (w->marker_count >= WB_WARP_MAX_MARKERS) return -1;
    if (src_sample < 0.0) src_sample = 0.0;

    /* find insertion point (sorted by src_sample) */
    int pos = 0;
    while (pos < w->marker_count &&
           w->markers[pos].src_sample < src_sample)
        pos++;

    /* reject exact duplicate src_sample */
    if (pos < w->marker_count &&
        w->markers[pos].src_sample == src_sample)
        return -1;

    /* shift right */
    for (int i = w->marker_count; i > pos; i--)
        w->markers[i] = w->markers[i - 1];

    w->markers[pos].src_sample = src_sample;
    w->markers[pos].dst_beat   = dst_beat;
    w->marker_count++;
    return w->marker_count;
}

int wb_warp_add_marker(wb_warp *w, double src_sample, double dst_beat) {
    if (!w) return -1;
    return insert_marker(w, src_sample, dst_beat);
}

int wb_warp_remove_marker(wb_warp *w, int index) {
    if (!w || index < 0 || index >= w->marker_count)
        return -1;
    for (int i = index; i < w->marker_count - 1; i++)
        w->markers[i] = w->markers[i + 1];
    w->marker_count--;
    return 0;
}

int wb_warp_clear_markers(wb_warp *w) {
    if (!w) return -1;
    w->marker_count = 0;
    return 0;
}

int wb_warp_marker_count(const wb_warp *w) {
    return w ? w->marker_count : 0;
}

/* ---- coordinate mapping ------------------------------------------------- */

/* Given a source sample position, find the segment [i, i+1] it falls in.
 * Returns the index i of the LEFT marker (or -1 if before first, or
 * marker_count-1 if after last — caller clamps). The implicit (0,0) marker
 * is handled transparently. */
static int find_segment(const wb_warp *w, double src) {
    if (w->marker_count == 0) return -1;
    int i = 0;
    while (i < w->marker_count - 1 &&
           w->markers[i + 1].src_sample <= src)
        i++;
    return i;
}

/* Map a source sample position to warped beat position.
 * With no markers this is identity (src_sample / sr). */
double wb_warp_src_to_dst(const wb_warp *w, double src_sample) {
    if (!w) return 0.0;
    if (w->marker_count == 0)
        return src_sample / (double)w->sr;

    /* clamp to source bounds if source is set */
    if (src_sample <= 0.0) {
        /* before first marker: extrapolate from (0,0) to marker[0] */
        const wb_warp_marker *m0 = &w->markers[0];
        if (m0->src_sample <= 0.0) return m0->dst_beat;
        return (src_sample / m0->src_sample) * m0->dst_beat;
    }
    if (w->src && src_sample >= (double)w->src_frames) {
        /* after last marker: extrapolate using last segment slope */
        const wb_warp_marker *ml = &w->markers[w->marker_count - 1];
        double src_lo = 0.0, dst_lo = 0.0;
        if (w->marker_count >= 2) {
            const wb_warp_marker *ml2 = &w->markers[w->marker_count - 2];
            src_lo = ml2->src_sample;
            dst_lo = ml2->dst_beat;
        }
        double src_span = ml->src_sample - src_lo;
        double dst_span = ml->dst_beat  - dst_lo;
        double slope = (src_span > 0.0) ? dst_span / src_span : 1.0;
        return ml->dst_beat + (src_sample - ml->src_sample) * slope;
    }

    /* find segment */
    int i = find_segment(w, src_sample);
    if (i < 0) i = 0;

    /* left and right markers of the segment */
    double ls, ld, rs, rd;
    if (i == 0) {
        ls = 0.0; ld = 0.0;
    } else {
        ls = w->markers[i - 1].src_sample;
        ld = w->markers[i - 1].dst_beat;
    }
    rs = w->markers[i].src_sample;
    rd = w->markers[i].dst_beat;

    /* if this is the last marker and src_sample > rs, extrapolate */
    if (i == w->marker_count - 1 && src_sample > rs) {
        double src_span = rs - ls;
        double dst_span = rd - ld;
        double slope = (src_span > 0.0) ? dst_span / src_span : 1.0;
        return rd + (src_sample - rs) * slope;
    }

    /* interpolate within segment */
    double src_span = rs - ls;
    if (src_span <= 0.0) return rd;
    double t = (src_sample - ls) / src_span;
    return ld + t * (rd - ld);
}

/* Map a beat position back to source sample position.
 * Inverse of src_to_dst via binary search on the beat axis. */
double wb_warp_dst_to_src(const wb_warp *w, double beat) {
    if (!w) return 0.0;
    if (w->marker_count == 0)
        return beat * (double)w->sr;

    /* before first marker: extrapolate from (0,0) */
    if (beat <= 0.0 || beat <= w->markers[0].dst_beat) {
        const wb_warp_marker *m0 = &w->markers[0];
        if (m0->dst_beat == 0.0) return m0->src_sample;
        return (beat / m0->dst_beat) * m0->src_sample;
    }

    /* after last marker: extrapolate */
    const wb_warp_marker *ml = &w->markers[w->marker_count - 1];
    if (beat >= ml->dst_beat) {
        double src_lo = 0.0, dst_lo = 0.0;
        if (w->marker_count >= 2) {
            const wb_warp_marker *ml2 = &w->markers[w->marker_count - 2];
            src_lo = ml2->src_sample;
            dst_lo = ml2->dst_beat;
        }
        double src_span = ml->src_sample - src_lo;
        double dst_span = ml->dst_beat  - dst_lo;
        double slope = (dst_span > 0.0) ? src_span / dst_span : 1.0;
        return ml->src_sample + (beat - ml->dst_beat) * slope;
    }

    /* find segment by beat */
    int i = 0;
    while (i < w->marker_count - 1 &&
           w->markers[i + 1].dst_beat <= beat)
        i++;

    double ls, ld, rs, rd;
    if (i == 0) { ls = 0.0; ld = 0.0; }
    else { ls = w->markers[i - 1].src_sample; ld = w->markers[i - 1].dst_beat; }
    rs = w->markers[i].src_sample;
    rd = w->markers[i].dst_beat;

    double dst_span = rd - ld;
    if (dst_span <= 0.0) return rs;
    double t = (beat - ld) / dst_span;
    return ls + t * (rs - ls);
}

/* ---- auto-warp ---------------------------------------------------------- */

/* Auto-place warp markers from detected beat positions.
 * beat_positions[] are source-sample positions of each beat.
 * Maps beat i to beat i (1 beat spacing) so audio conforms to those beats.
 * Returns the number of markers placed, or -1 on error. */
int wb_warp_auto_warp(wb_warp *w, const double *beat_positions, int num_beats) {
    if (!w || !beat_positions || num_beats <= 0) return -1;

    wb_warp_clear_markers(w);
    for (int i = 0; i < num_beats; i++) {
        if (insert_marker(w, beat_positions[i], (double)i) < 0)
            return -1;  /* full or duplicate */
    }
    return w->marker_count;
}

/* ---- processing --------------------------------------------------------- */

/* Render warped audio.
 * beat_start, beat_end: the beat range to render.
 * out: output buffer (interleaved stereo, 2 channels), must hold
 *      frames * 2 samples.
 * frames: number of output frames to render.
 *
 * Strategy: for each output frame at beat position b, find the source
 * sample via dst_to_src, then read the source at that position using
 * windowed-sinc interpolation for quality. This is a per-sample resampling
 * approach — simpler and more flexible than segmenting + timestretch,
 * and handles arbitrary warp curves correctly. */
void wb_warp_process(wb_warp *w, double beat_start, double beat_end,
                     wb_sample *out, uint32_t frames) {
    if (!w || !out || frames == 0) return;
    if (!w->src || w->src_frames == 0) {
        memset(out, 0, (size_t)frames * 2 * sizeof(wb_sample));
        return;
    }

    double beat_step = (beat_end - beat_start) / (double)frames;
    uint32_t src_max = w->src_frames;
    uint32_t chn   = w->src_chn;

    for (uint32_t f = 0; f < frames; f++) {
        double beat = beat_start + (double)f * beat_step;
        double src_pos = wb_warp_dst_to_src(w, beat);

        /* windowed-sinc interpolation at src_pos */
        double sample_l = 0.0, sample_r = 0.0;

        if (src_pos >= 0.0 && src_pos < (double)(src_max - 1)) {
            /* identity path: no markers → direct copy */
            if (w->marker_count == 0) {
                uint32_t i0 = (uint32_t)src_pos;
                double frac = src_pos - (double)i0;
                if (chn >= 2) {
                    sample_l = w->src[i0 * chn] * (1.0 - frac) +
                               w->src[(i0 + 1) * chn] * frac;
                    sample_r = w->src[i0 * chn + 1] * (1.0 - frac) +
                               w->src[(i0 + 1) * chn + 1] * frac;
                } else {
                    sample_l = w->src[i0] * (1.0 - frac) +
                               w->src[i0 + 1] * frac;
                    sample_r = sample_l;
                }
            } else {
                /* windowed-sinc for quality */
                sample_l = 0.0;
                sample_r = 0.0;
                int i0 = (int)floor(src_pos);
                double frac = src_pos - floor(src_pos);

                /* 8-tap windowed sinc */
                static float sinc_tbl[512][16];
                static int sinc_ready = 0;
                if (!sinc_ready) {
                    for (int p = 0; p < 512; p++) {
                        double t = (double)p / 512.0;
                        for (int k = -8; k < 8; k++) {
                            double x = (double)k + t;
                            double s = fabs(x) < 1e-9 ? 1.0 : sin(M_PI * x) / (M_PI * x);
                            double win = 0.42 + 0.5 * cos(M_PI * x / 8.0)
                                             + 0.08 * cos(2.0 * M_PI * x / 8.0);
                            sinc_tbl[p][k + 8] = (float)(s * win);
                        }
                    }
                    sinc_ready = 1;
                }

                int ph = (int)(frac * 512.0);
                if (ph >= 512) ph = 511;
                const float *tap = sinc_tbl[ph];

                double acc_l = 0.0, acc_r = 0.0;
                for (int k = -8; k < 8; k++) {
                    long idx = (long)i0 + k;
                    if (idx < 0 || idx >= (long)src_max) continue;
                    float coeff = tap[k + 8];
                    if (chn >= 2) {
                        acc_l += w->src[idx * chn] * coeff;
                        acc_r += w->src[idx * chn + 1] * coeff;
                    } else {
                        acc_l += w->src[idx] * coeff;
                    }
                }
                sample_l = acc_l;
                sample_r = (chn >= 2) ? acc_r : acc_l;
            }
        }

        /* clamp to valid range */
        if (sample_l > 1.0) sample_l = 1.0;
        if (sample_l < -1.0) sample_l = -1.0;
        if (sample_r > 1.0) sample_r = 1.0;
        if (sample_r < -1.0) sample_r = -1.0;

        out[f * 2]     = (wb_sample)sample_l;
        out[f * 2 + 1] = (wb_sample)sample_r;
    }
}