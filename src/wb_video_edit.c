/* wb_video_edit.c — video editing commands & auto-assembly (R077 Phase 4).
 *
 * Agent-driven video editing: cut, trim, transition, effect commands,
 * scene detection (frame difference + histogram), auto-assembly (rough cut),
 * silence detection, beat-sync editing, and template-based editing.
 *
 * Works with wb_agent.c, wb_compositor.c, wb_vfx.c, wb_char2d.c.
 * Pure C11, no third party.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

/* ===================================================================
 * Scene Detection
 * =================================================================== */

typedef struct {
    float timestamp;     /* seconds */
    float confidence;    /* 0..1, how different this frame is */
} wb_scene_cut;

/* Detect scene cuts by comparing consecutive frame histograms.
 * frame_data: array of RGBA frames, each w*h*4 bytes
 * Returns number of detected cuts, writes cuts[] array.
 */
int wb_detect_scenes(const uint8_t **frames, int n_frames, int w, int h,
                      wb_scene_cut *cuts, int max_cuts, float threshold) {
    if (n_frames < 2 || !frames || !cuts) return 0;

    int n_cuts = 0;
    /* Histogram: 16 bins per channel = 4096 total bins */
    #define HIST_BINS 16
    #define HIST_SIZE (HIST_BINS * HIST_BINS * HIST_BINS)

    uint32_t prev_hist[HIST_SIZE];
    memset(prev_hist, 0, sizeof(prev_hist));

    /* Build histogram for first frame */
    int pixels = w * h;
    for (int p = 0; p < pixels; p++) {
        int idx = p * 4;
        int r = frames[0][idx+0] * (HIST_BINS-1) / 255;
        int g = frames[0][idx+1] * (HIST_BINS-1) / 255;
        int b = frames[0][idx+2] * (HIST_BINS-1) / 255;
        prev_hist[r * HIST_BINS * HIST_BINS + g * HIST_BINS + b]++;
    }

    for (int f = 1; f < n_frames && n_cuts < max_cuts; f++) {
        uint32_t hist[HIST_SIZE];
        memset(hist, 0, sizeof(hist));

        for (int p = 0; p < pixels; p++) {
            int idx = p * 4;
            int r = frames[f][idx+0] * (HIST_BINS-1) / 255;
            int g = frames[f][idx+1] * (HIST_BINS-1) / 255;
            int b = frames[f][idx+2] * (HIST_BINS-1) / 255;
            hist[r * HIST_BINS * HIST_BINS + g * HIST_BINS + b]++;
        }

        /* Chi-squared difference between histograms */
        float diff = 0;
        for (int i = 0; i < HIST_SIZE; i++) {
            float a = (float)hist[i];
            float b = (float)prev_hist[i];
            float sum = a + b;
            if (sum > 0) {
                diff += (a - b) * (a - b) / sum;
            }
        }
        diff /= (float)(pixels * 2);

        /* Normalize to 0..1 range (empirical: diff > 0.5 = likely cut) */
        float confidence = fminf(1.0f, diff * 2.0f);

        if (confidence > threshold) {
            cuts[n_cuts].timestamp = (float)f;  /* frame index as proxy */
            cuts[n_cuts].confidence = confidence;
            n_cuts++;
        }

        memcpy(prev_hist, hist, sizeof(prev_hist));
    }

    return n_cuts;
}

/* ===================================================================
 * Silence Detection
 * =================================================================== */

typedef struct {
    float start, end;    /* seconds */
} wb_silence;

/* Detect silent regions in audio.
 * samples: interleaved audio (n_channels * n_frames samples)
 * Returns number of silent regions found.
 */
int wb_detect_silence(const float *samples, int n_frames, int n_channels,
                       float sample_rate, float threshold_db,
                       wb_silence *regions, int max_regions) {
    if (!samples || !regions || n_frames < 1) return 0;

    float threshold_linear = powf(10.0f, threshold_db / 20.0f);  /* dB to linear */
    int window_size = (int)(sample_rate * 0.05f);  /* 50ms windows */
    if (window_size < 1) window_size = 1;

    int n_regions = 0;
    int in_silence = 0;
    int silence_start = 0;

    for (int i = 0; i < n_frames; i += window_size) {
        /* Compute RMS in this window */
        float sum_sq = 0;
        int count = 0;
        int end = i + window_size;
        if (end > n_frames) end = n_frames;

        for (int j = i; j < end; j++) {
            float s = samples[j * n_channels];  /* first channel */
            sum_sq += s * s;
            count++;
        }
        float rms = (count > 0) ? sqrtf(sum_sq / count) : 0;

        if (rms < threshold_linear) {
            if (!in_silence) {
                silence_start = i;
                in_silence = 1;
            }
        } else {
            if (in_silence) {
                float duration = (float)(i - silence_start) / sample_rate;
                if (duration > 0.3f && n_regions < max_regions) {  /* min 300ms */
                    regions[n_regions].start = (float)silence_start / sample_rate;
                    regions[n_regions].end = (float)i / sample_rate;
                    n_regions++;
                }
                in_silence = 0;
            }
        }
    }

    /* Close trailing silence */
    if (in_silence && n_regions < max_regions) {
        float duration = (float)(n_frames - silence_start) / sample_rate;
        if (duration > 0.3f) {
            regions[n_regions].start = (float)silence_start / sample_rate;
            regions[n_regions].end = (float)n_frames / sample_rate;
            n_regions++;
        }
    }

    return n_regions;
}

/* ===================================================================
 * Beat Detection (onset-based)
 * =================================================================== */

typedef struct {
    float timestamp;     /* seconds */
    float strength;      /* onset strength 0..1 */
} wb_beat;

/* Simple beat detection via spectral flux onset detection.
 * Uses energy difference between consecutive windows.
 */
int wb_detect_beats(const float *samples, int n_frames, int n_channels,
                     float sample_rate, wb_beat *beats, int max_beats) {
    if (!samples || !beats || n_frames < 1) return 0;

    int window_size = (int)(sample_rate * 0.02f);  /* 20ms */
    if (window_size < 64) window_size = 64;
    int hop = window_size / 2;

    int n_beats = 0;
    float prev_energy = 0;
    float avg_flux = 0;
    int flux_count = 0;

    /* First pass: compute average flux for threshold */
    for (int i = 0; i + window_size < n_frames; i += hop) {
        float energy = 0;
        for (int j = i; j < i + window_size; j++) {
            float s = samples[j * n_channels];
            energy += s * s;
        }
        energy /= window_size;
        float flux = energy - prev_energy;
        if (flux > 0) { avg_flux += flux; flux_count++; }
        prev_energy = energy;
    }
    avg_flux = (flux_count > 0) ? avg_flux / flux_count : 0.001f;
    float threshold = avg_flux * 1.5f;

    /* Second pass: find onsets */
    prev_energy = 0;
    int last_beat_frame = 0;
    int min_beat_gap = (int)(sample_rate * 0.2f);  /* min 200ms between beats */

    for (int i = 0; i + window_size < n_frames && n_beats < max_beats; i += hop) {
        float energy = 0;
        for (int j = i; j < i + window_size; j++) {
            float s = samples[j * n_channels];
            energy += s * s;
        }
        energy /= window_size;
        float flux = energy - prev_energy;

        if (flux > threshold && (i - last_beat_frame) > min_beat_gap) {
            beats[n_beats].timestamp = (float)i / sample_rate;
            beats[n_beats].strength = fminf(1.0f, flux / (avg_flux * 3.0f));
            n_beats++;
            last_beat_frame = i;
        }
        prev_energy = energy;
    }

    return n_beats;
}

/* ===================================================================
 * Auto-Assembly (Rough Cut)
 * =================================================================== */

typedef struct {
    float start_time;    /* in source */
    float end_time;
    float score;         /* how interesting this segment is */
} wb_auto_segment;

/* Score segments by motion + audio energy.
 * Higher score = more interesting = keep in rough cut.
 */
int wb_auto_assemble(const float *audio_energy, int n_energy_frames,
                      const float *motion_scores, int n_motion_frames,
                      float total_duration, float min_segment_sec,
                      float max_segment_sec, float target_duration,
                      wb_auto_segment *segments, int max_segments) {
    if (!segments) return 0;

    /* Score each second of footage */
    int n_seconds = (int)total_duration;
    if (n_seconds < 1) return 0;

    float *scores = calloc(n_seconds, sizeof(float));
    if (!scores) return 0;

    for (int s = 0; s < n_seconds; s++) {
        /* Average audio energy in this second */
        float audio_score = 0;
        if (audio_energy && s < n_energy_frames) {
            audio_score = audio_energy[s];
        }
        /* Average motion in this second */
        float motion_score = 0;
        if (motion_scores && s < n_motion_frames) {
            motion_score = motion_scores[s];
        }
        /* Combined: prefer segments with both audio and motion */
        scores[s] = audio_score * 0.5f + motion_score * 0.5f;
    }

    /* Greedy: pick highest-scoring segments until target duration reached */
    int n_segments = 0;
    float total_selected = 0;
    int *used = calloc(n_seconds, sizeof(int));

    while (total_selected < target_duration && n_segments < max_segments) {
        /* Find best unused segment */
        int best_start = -1;
        float best_score = -1;

        for (int s = 0; s < n_seconds; s++) {
            if (used[s]) continue;
            if (scores[s] > best_score) {
                best_score = scores[s];
                best_start = s;
            }
        }

        if (best_start < 0 || best_score <= 0) break;

        /* Determine segment length based on score */
        float seg_len = min_segment_sec;
        if (best_score > 0.7f) seg_len = max_segment_sec;
        else if (best_score > 0.4f) seg_len = (min_segment_sec + max_segment_sec) * 0.5f;

        /* Don't exceed target */
        if (total_selected + seg_len > target_duration) {
            seg_len = target_duration - total_selected;
        }
        if (seg_len < min_segment_sec) break;

        segments[n_segments].start_time = (float)best_start;
        segments[n_segments].end_time = (float)best_start + seg_len;
        segments[n_segments].score = best_score;
        n_segments++;
        total_selected += seg_len;

        /* Mark seconds as used */
        for (int s = best_start; s < best_start + (int)seg_len && s < n_seconds; s++) {
            used[s] = 1;
        }
    }

    free(scores);
    free(used);
    return n_segments;
}

/* ===================================================================
 * Beat-Sync Editing
 * =================================================================== */

/* Given a list of beats and a list of scene cuts, snap cuts to nearest beat.
 * Returns number of cuts that were snapped.
 */
int wb_sync_cuts_to_beats(wb_scene_cut *cuts, int n_cuts,
                           const wb_beat *beats, int n_beats,
                           float max_offset_sec) {
    if (!cuts || !beats || n_cuts < 1 || n_beats < 1) return 0;

    int snapped = 0;
    for (int c = 0; c < n_cuts; c++) {
        float best_dist = max_offset_sec;
        int best_beat = -1;

        for (int b = 0; b < n_beats; b++) {
            float dist = fabsf(cuts[c].timestamp - beats[b].timestamp);
            if (dist < best_dist) {
                best_dist = dist;
                best_beat = b;
            }
        }

        if (best_beat >= 0) {
            cuts[c].timestamp = beats[best_beat].timestamp;
            snapped++;
        }
    }

    return snapped;
}

/* ===================================================================
 * Template-Based Editing
 * =================================================================== */

typedef struct {
    char name[64];
    float duration;          /* total target duration */
    int has_intro;           /* add intro card */
    int has_outro;           /* add outro card */
    float transition_sec;    /* transition duration */
    int transition_type;     /* 0=dissolve, 1=wipe, 2=flash */
    int auto_sync_beats;     /* snap cuts to beats */
    int remove_silence;      /* auto-remove silent sections */
    float target_clip_len;   /* preferred clip length in seconds */
} wb_edit_template;

void wb_edit_template_init(wb_edit_template *t) {
    strncpy(t->name, "Default", 63);
    t->name[63] = '\0';
    t->duration = 60.0f;
    t->has_intro = 1;
    t->has_outro = 1;
    t->transition_sec = 0.5f;
    t->transition_type = 0;
    t->auto_sync_beats = 1;
    t->remove_silence = 1;
    t->target_clip_len = 5.0f;
}

/* Apply template to generate an edit decision list (EDL).
 * Returns number of edits in the list.
 */
int wb_template_apply(const wb_edit_template *t,
                       const wb_scene_cut *cuts, int n_cuts,
                       wb_auto_segment *edl, int max_edl) {
    if (!t || !edl) return 0;

    int n_edl = 0;
    float total = 0;

    /* Intro */
    if (t->has_intro && n_edl < max_edl) {
        edl[n_edl].start_time = 0;
        edl[n_edl].end_time = 3.0f;  /* 3 sec intro */
        edl[n_edl].score = 1.0f;
        n_edl++;
        total += 3.0f;
    }

    /* Main content: use scene cuts as clip boundaries */
    for (int i = 0; i < n_cuts - 1 && n_edl < max_edl; i++) {
        float clip_start = cuts[i].timestamp;
        float clip_end = cuts[i+1].timestamp;
        float clip_len = clip_end - clip_start;

        /* Skip clips that are too short */
        if (clip_len < 0.5f) continue;

        /* Trim to target length if much longer */
        if (clip_len > t->target_clip_len * 2.0f) {
            clip_end = clip_start + t->target_clip_len;
            clip_len = t->target_clip_len;
        }

        if (total + clip_len > t->duration) break;

        edl[n_edl].start_time = clip_start;
        edl[n_edl].end_time = clip_end;
        edl[n_edl].score = cuts[i].confidence;
        n_edl++;
        total += clip_len;
    }

    /* Outro */
    if (t->has_outro && n_edl < max_edl && total + 3.0f <= t->duration) {
        edl[n_edl].start_time = total;
        edl[n_edl].end_time = total + 3.0f;
        edl[n_edl].score = 1.0f;
        n_edl++;
    }

    return n_edl;
}
