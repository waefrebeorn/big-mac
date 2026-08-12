/* wb_esynth.h — espeak-ng-style additive formant synthesis (R015 fresh start) */
#ifndef WB_ESYNTH_H
#define WB_ESYNTH_H

/* one formant peak: center freq, height, left/right bandwidths (Hz) */
typedef struct {
    double freq;
    double height;
    double left;
    double right;
} wb_esynth_peak_t;

/* per-phone acoustic specification: f0, amplitude, and the formant peaks */
typedef struct {
    double f0;
    double amplitude;
    int npeaks;
    wb_esynth_peak_t peaks[8];
} wb_esynth_phone_t;

typedef struct wb_esynth wb_esynth_t;

wb_esynth_t *wb_esynth_new(int sample_rate);
void wb_esynth_free(wb_esynth_t *s);
/* additive-harmonic render of one phone into out (nsamp samples, added) */
void wb_esynth_phone(wb_esynth_t *s, double *out, int nsamp,
                     const wb_esynth_phone_t *ph);

/* R015 REMAKE: render a WHOLE PHRASE as one continuous formant trajectory.
 * frames[i] is the peak target for phone i (0..n-1), frame_dur[i] its
 * duration in seconds. The peaks are interpolated continuously between
 * consecutive frames (espeak-ng AdvanceParameters), so formants GLIDE from
 * phone to phone — the connectedness that makes it speech, not blips.
 * Returns the total samples rendered into out (0 on error). */
int wb_esynth_phrase(wb_esynth_t *s, double *out, int out_cap,
                     const wb_esynth_phone_t *frames, const double *frame_dur,
                     int n);

#endif
