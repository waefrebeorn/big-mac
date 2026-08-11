/*
 * wb_psola.h — TD-PSOLA pitch shifter (real-time voice-changer core).
 *
 * Shifts F0 by `factor` (1.0 = unchanged, 2.0 = octave up, 0.5 = octave down)
 * while preserving the spectral envelope (formants), so the voice stays
 * natural. Pure C11, self-contained.
 */
#ifndef WB_PSOLA_H
#define WB_PSOLA_H

/* Pitch-shift x[] by `factor`. Returns the output length (0 on failure).
 * `out` must hold at least `max_out` samples. Duration is preserved (there
 * are ~factor glottal pulses per input period). */
int wb_psola_pitch_shift(const double *x, int n, int sr, double factor,
                         double *out, int max_out);

#endif /* WB_PSOLA_H */
