/* wb_time_sig.c — time signature map (changes over the timeline).
 * Stores a sorted array of time signature changes in wb_session. The
 * session's base time_sig_num/den applies before the first change.
 *
 * Bar length in beats = num * (4 / den). E.g. 4/4 -> 4 beats, 3/4 -> 3,
 * 7/8 -> 3.5, 6/8 -> 3. A "beat" for conversion purposes is the quarter
 * note (the standard DAW pulse), so samples<->beats uses the quarter-note
 * duration at the relevant time signature.
 */

#include <stdlib.h>
#include <string.h>
#include "wbus.h"

/* ---- helpers ------------------------------------------------------------ */

/* Is den a valid power-of-two denominator (1,2,4,8,16,32)? */
static int is_valid_den(int den) {
    return den > 0 && (den & (den - 1)) == 0 && den <= 32;
}

/* Is (num, den) a valid time signature? */
static int is_valid_time_sig(int num, int den) {
    return num >= 1 && num <= 32 && is_valid_den(den);
}

/* Quarter-note duration in samples at the given time signature and sample rate.
 * A quarter note = 60/bpm seconds, independent of the time sig's denominator.
 * The denominator only affects how many quarter notes make a beat-group. */
static double quarter_note_samples(double bpm, uint32_t sr) {
    return (60.0 / bpm) * (double)sr;
}

/* ---- validation + add --------------------------------------------------- */

int wb_session_add_time_sig_change(wb_session *s, double pos_samples, int num, int den) {
    if (!s) return -1;
    if (!is_valid_time_sig(num, den)) return -1;
    if (pos_samples < 0) return -1;
    if (s->time_sig_change_count >= WB_MAX_TIME_SIG_CHANGES) return -1;

    /* Find insertion point (keep sorted by pos). */
    uint32_t i = 0;
    while (i < s->time_sig_change_count &&
           s->time_sig_changes[i].pos < pos_samples) {
        i++;
    }
    /* Shift right to make room. */
    for (uint32_t j = s->time_sig_change_count; j > i; j--) {
        s->time_sig_changes[j] = s->time_sig_changes[j - 1];
    }
    s->time_sig_changes[i].pos = pos_samples;
    s->time_sig_changes[i].num = num;
    s->time_sig_changes[i].den = den;
    s->time_sig_change_count++;
    return (int)i;
}

int wb_session_remove_time_sig_change(wb_session *s, int index) {
    if (!s) return -1;
    if (index < 0 || (uint32_t)index >= s->time_sig_change_count) return -1;
    for (uint32_t j = (uint32_t)index; j + 1 < s->time_sig_change_count; j++) {
        s->time_sig_changes[j] = s->time_sig_changes[j + 1];
    }
    s->time_sig_change_count--;
    return 0;
}

/* ---- lookup ------------------------------------------------------------- */

int wb_session_get_time_sig_at(wb_session *s, double pos_samples, int *num_out, int *den_out) {
    if (!s) return -1;
    /* Binary search: find the last change with pos <= pos_samples. */
    int lo = 0, hi = (int)s->time_sig_change_count - 1;
    int found = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (s->time_sig_changes[mid].pos <= pos_samples) {
            found = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (found >= 0) {
        if (num_out) *num_out = s->time_sig_changes[found].num;
        if (den_out) *den_out = s->time_sig_changes[found].den;
    } else {
        if (num_out) *num_out = s->time_sig_num;
        if (den_out) *den_out = s->time_sig_den;
    }
    return 0;
}

int wb_session_time_sig_change_count(const wb_session *s) {
    if (!s) return 0;
    return (int)s->time_sig_change_count;
}

int wb_session_get_time_sig_change(const wb_session *s, int index,
                                   double *pos_out, int *num_out, int *den_out) {
    if (!s || index < 0 || (uint32_t)index >= s->time_sig_change_count) return -1;
    if (pos_out) *pos_out = s->time_sig_changes[index].pos;
    if (num_out) *num_out = s->time_sig_changes[index].num;
    if (den_out) *den_out = s->time_sig_changes[index].den;
    return 0;
}

/* ---- bar / beat conversions -------------------------------------------- */

/* Bar length in beats (quarter notes) for a time sig. */
static double bar_len_beats(int num, int den) {
    return (double)num * (4.0 / (double)den);
}

/* Bar length in samples for a time sig at a given bpm and sample rate. */
static double bar_len_samples(int num, int den, double bpm, uint32_t sr) {
    return bar_len_beats(num, den) * quarter_note_samples(bpm, sr);
}

double wb_session_get_bar_start(const wb_session *s, int bar_number) {
    if (!s || bar_number <= 0) return 0.0;
    uint32_t sr = WB_SAMPLE_RATE;
    double bpm = s->bpm > 0 ? s->bpm : 120.0;

    /* Walk through time sig regions, subtracting bars, until we reach the
     * region containing bar_number. */
    double pos = 0.0;
    int bars_consumed = 0;
    int cur_num = s->time_sig_num;
    int cur_den = s->time_sig_den;

    /* Iterate through changes; each change defines a region starting at its
     * position. We need to figure out how many bars fit between consecutive
     * change points, or from 0 to the first change. */
    for (uint32_t i = 0; i <= s->time_sig_change_count; i++) {
        /* Determine the end of the current region (next change or infinity). */
        double region_end;
        int next_num, next_den;
        if (i < s->time_sig_change_count) {
            region_end = s->time_sig_changes[i].pos;
            next_num = s->time_sig_changes[i].num;
            next_den = s->time_sig_changes[i].den;
        } else {
            region_end = 1e30; /* effectively infinite */
            next_num = cur_num;
            next_den = cur_den;
        }

        double bls = bar_len_samples(cur_num, cur_den, bpm, sr);
        double region_len = region_end - pos;
        int bars_in_region = (int)(region_len / bls);

        if (bars_consumed + bars_in_region >= bar_number) {
            /* The desired bar is within this region. */
            int offset = bar_number - bars_consumed;
            return pos + offset * bls;
        }
        /* Skip this region entirely. */
        bars_consumed += bars_in_region;
        pos = region_end;
        cur_num = next_num;
        cur_den = next_den;
    }
    /* Fell off the end — compute from the last known position. */
    double bls = bar_len_samples(cur_num, cur_den, bpm, sr);
    int offset = bar_number - bars_consumed;
    return pos + offset * bls;
}

int wb_session_get_bar_at(const wb_session *s, double pos_samples) {
    if (!s || pos_samples <= 0.0) return 0;
    uint32_t sr = WB_SAMPLE_RATE;
    double bpm = s->bpm > 0 ? s->bpm : 120.0;

    double pos = 0.0;
    int bar_count = 0;
    int cur_num = s->time_sig_num;
    int cur_den = s->time_sig_den;

    for (uint32_t i = 0; i <= s->time_sig_change_count; i++) {
        double region_end;
        int next_num, next_den;
        if (i < s->time_sig_change_count) {
            region_end = s->time_sig_changes[i].pos;
            next_num = s->time_sig_changes[i].num;
            next_den = s->time_sig_changes[i].den;
        } else {
            region_end = 1e30;
            next_num = cur_num;
            next_den = cur_den;
        }

        if (pos_samples < region_end) {
            /* The position is within this region. */
            double bls = bar_len_samples(cur_num, cur_den, bpm, sr);
            int bars_here = (int)((pos_samples - pos) / bls);
            return bar_count + bars_here;
        }
        /* Position is past this region — count all bars in it. */
        double bls = bar_len_samples(cur_num, cur_den, bpm, sr);
        double region_len = region_end - pos;
        bar_count += (int)(region_len / bls);
        pos = region_end;
        cur_num = next_num;
        cur_den = next_den;
    }
    /* Past all changes. */
    double bls = bar_len_samples(cur_num, cur_den, bpm, sr);
    int bars_here = (int)((pos_samples - pos) / bls);
    return bar_count + bars_here;
}

double wb_session_samples_to_beats(const wb_session *s, double samples) {
    if (!s) return 0.0;
    uint32_t sr = WB_SAMPLE_RATE;
    double bpm = s->bpm > 0 ? s->bpm : 120.0;
    /* Quarter-note duration in samples is independent of time sig. */
    double qn = quarter_note_samples(bpm, sr);
    return samples / qn;
}

double wb_session_beats_to_samples(const wb_session *s, double beats) {
    if (!s) return 0.0;
    uint32_t sr = WB_SAMPLE_RATE;
    double bpm = s->bpm > 0 ? s->bpm : 120.0;
    double qn = quarter_note_samples(bpm, sr);
    return beats * qn;
}