/* wb_ytpmv_lipsync.c — C11 lip-sync/viseme engine for YTPMV (R131b).
 *
 * Maps ARPABET-style phonemes to 15 Oculus-standard viseme IDs, generates
 * viseme timelines from a wb_phoneme_db, and produces ffmpeg filter strings
 * for mouth-shape selection in YTPMV renders.
 *
 * Oculus 15-viseme standard:
 *   SIL(0) PP(1) FF(2) TH(3) DD(4) KK(5) CH(6) SS(7)
 *   NN(8) RR(9) AA(10) EE(11) IH(12) OH(13) OO(14)
 *
 * Opaque style: the engine state is an opaque struct; callers interact
 * only through the create/destroy/accessor API.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "wbus/wbus_compositor.h"

/* ---- Opaque lipsync engine state ---- */
#define WB_LIPSYNC_MAX_FRAMES 512
#define WB_LIPSYNC_MAX_PHONEMES 64
#define WB_LIPSYNC_MAX_FFTEXT 256

struct wb_lipsync_engine {
    /* Phoneme input */
    wb_phoneme_type phonemes[WB_LIPSYNC_MAX_PHONEMES];
    float           start_times[WB_LIPSYNC_MAX_PHONEMES];
    float           end_times[WB_LIPSYNC_MAX_PHONEMES];
    int             n_phonemes;

    /* Viseme timeline */
    struct {
        float start_time, end_time;
        int   viseme_id;       /* WB_VISEME_* */
        float blend;           /* 0..1 transition blend */
        float energy;          /* audio energy at this segment */
    } frames[WB_LIPSYNC_MAX_FRAMES];
    int n_frames;

    /* FFmpeg filter text buffer */
    char ffmpeg_filter[WB_LIPSYNC_MAX_FFTEXT];

    /* Config */
    float blend_speed;         /* how fast visemes transition (0..1 per frame) */
    float sample_rate;
    float fps;
};

/* ---- Phoneme-to-viseme mapping (ARPABET → Oculus 15) ---- */
static int phoneme_to_viseme_id(wb_phoneme_type p)
{
    switch (p) {
    case PHON_VOWEL_A:   return WB_VISEME_AA;
    case PHON_VOWEL_E:   return WB_VISEME_EE;
    case PHON_VOWEL_I:   return WB_VISEME_IH;
    case PHON_VOWEL_O:   return WB_VISEME_OH;
    case PHON_VOWEL_U:   return WB_VISEME_OO;

    case PHON_CONSONANT_P:
    case PHON_CONSONANT_B:
    case PHON_CONSONANT_M:   return WB_VISEME_PP;

    case PHON_CONSONANT_F:
    case PHON_CONSONANT_V:   return WB_VISEME_FF;

    /* TH maps to PHON_CONSONANT_T in our phoneme set (closest match) */
    case PHON_CONSONANT_T:   return WB_VISEME_TH;

    case PHON_CONSONANT_D:
    case PHON_CONSONANT_L:   return WB_VISEME_DD;

    case PHON_CONSONANT_K:
    case PHON_CONSONANT_G:
    case PHON_CONSONANT_H:   return WB_VISEME_KK;

    case PHON_CONSONANT_N:   return WB_VISEME_NN;
    case PHON_CONSONANT_R:   return WB_VISEME_RR;
    case PHON_CONSONANT_S:
    case PHON_CONSONANT_Z:   return WB_VISEME_SS;

    case PHON_CONSONANT_W:   return WB_VISEME_OO;

    case PHON_SILENCE:       return WB_VISEME_SIL;
    case PHON_UNKNOWN:
    default:                 return WB_VISEME_SIL;
    }
}

/* ---- Viseme name lookup ---- */
static const char *viseme_id_to_name(int id)
{
    static const char *names[15] = {
        "SIL", "PP", "FF", "TH", "DD",
        "KK", "CH", "SS", "NN", "RR",
        "AA", "EE", "IH", "OH", "OO"
    };
    if (id < 0 || id >= 15) return "???";
    return names[id];
}

/* ---- Mouth shape descriptor for ffmpeg ---- */
static const char *viseme_mouth_shape(int id)
{
    /* Returns a short descriptor used in ffmpeg drawtext or overlay selection */
    static const char *shapes[15] = {
        "rest",     /* SIL */
        "closed",   /* PP */
        "fv",       /* FF */
        "th",       /* TH */
        "dt",       /* DD */
        "kg",       /* KK */
        "ch",       /* CH */
        "sz",       /* SS */
        "nn",       /* NN */
        "rr",       /* RR */
        "open",     /* AA */
        "smile",    /* EE */
        "relaxed",  /* IH */
        "round",    /* OH */
        "pucker"    /* OO */
    };
    if (id < 0 || id >= 15) return "rest";
    return shapes[id];
}

/* ---- Opaque API implementation ---- */

wb_lipsync_engine *wb_lipsync_engine_create(float sample_rate, float fps)
{
    wb_lipsync_engine *eng = (wb_lipsync_engine *)calloc(1, sizeof(wb_lipsync_engine));
    if (!eng) return NULL;
    eng->blend_speed = 0.3f;
    eng->sample_rate = sample_rate > 0 ? sample_rate : 48000.0f;
    eng->fps = fps > 0 ? fps : 30.0f;
    eng->n_phonemes = 0;
    eng->n_frames = 0;
    eng->ffmpeg_filter[0] = '\0';
    return eng;
}

void wb_lipsync_engine_destroy(wb_lipsync_engine *eng)
{
    if (eng) free(eng);
}

/* Load phonemes from a phoneme database into the engine */
int wb_lipsync_load_phonemes(wb_lipsync_engine *eng,
                              const wb_phoneme_db *db)
{
    if (!eng || !db) return -1;
    int n = db->count < WB_LIPSYNC_MAX_PHONEMES ? db->count : WB_LIPSYNC_MAX_PHONEMES;
    eng->n_phonemes = n;
    for (int i = 0; i < n; i++) {
        eng->phonemes[i] = db->phonemes[i].type;
        eng->start_times[i] = db->phonemes[i].start_time;
        eng->end_times[i] = db->phonemes[i].end_time;
    }
    return n;
}

/* Add a single phoneme manually */
int wb_lipsync_add_phoneme(wb_lipsync_engine *eng, wb_phoneme_type type,
                            float start, float end)
{
    if (!eng || eng->n_phonemes >= WB_LIPSYNC_MAX_PHONEMES) return -1;
    int idx = eng->n_phonemes++;
    eng->phonemes[idx] = type;
    eng->start_times[idx] = start;
    eng->end_times[idx] = end;
    return idx;
}

/* Generate the viseme timeline from loaded phonemes */
int wb_lipsync_generate_timeline(wb_lipsync_engine *eng)
{
    if (!eng || eng->n_phonemes == 0) return -1;

    int n = 0;
    for (int i = 0; i < eng->n_phonemes && n < WB_LIPSYNC_MAX_FRAMES; i++) {
        int vid = phoneme_to_viseme_id(eng->phonemes[i]);

        /* Skip consecutive duplicate visemes (merge) */
        if (n > 0 && eng->frames[n - 1].viseme_id == vid) {
            /* Extend the previous frame's end time */
            eng->frames[n - 1].end_time = eng->end_times[i];
            continue;
        }

        eng->frames[n].start_time = eng->start_times[i];
        eng->frames[n].end_time   = eng->end_times[i];
        eng->frames[n].viseme_id  = vid;
        eng->frames[n].blend      = 0.0f;
        eng->frames[n].energy     = 0.5f; /* default mid energy */
        n++;
    }
    eng->n_frames = n;

    /* Compute blend values: frames with short duration get higher blend */
    for (int i = 0; i < n; i++) {
        float dur = eng->frames[i].end_time - eng->frames[i].start_time;
        if (dur <= 0.0f) dur = 1.0f / eng->fps;
        /* Short phonemes → fast blend (high value) */
        eng->frames[i].blend = (dur < 0.1f) ? 0.8f :
                               (dur < 0.2f) ? 0.5f : 0.2f;
    }

    return n;
}

/* Query the active viseme at a given time */
int wb_lipsync_viseme_at(const wb_lipsync_engine *eng, float time_sec)
{
    if (!eng || eng->n_frames == 0) return WB_VISEME_SIL;
    for (int i = 0; i < eng->n_frames; i++) {
        if (time_sec >= eng->frames[i].start_time && time_sec < eng->frames[i].end_time)
            return eng->frames[i].viseme_id;
    }
    /* If past last frame, return last viseme */
    if (eng->n_frames > 0 && time_sec >= eng->frames[eng->n_frames - 1].end_time)
        return eng->frames[eng->n_frames - 1].viseme_id;
    return WB_VISEME_SIL;
}

/* Get the number of viseme frames in the timeline */
int wb_lipsync_frame_count(const wb_lipsync_engine *eng)
{
    return eng ? eng->n_frames : 0;
}

/* Access a specific viseme frame (read-only) */
const wb_viseme_frame *wb_lipsync_get_frame(const wb_lipsync_engine *eng, int idx)
{
    if (!eng || idx < 0 || idx >= eng->n_frames) return NULL;
    return (const wb_viseme_frame *)&eng->frames[idx];
}

/* Generate an ffmpeg filter string for mouth shape selection.
 * Produces an expression that selects mouth image index per frame
 * based on the viseme timeline. Output goes into eng->ffmpeg_filter. */
const char *wb_lipsync_generate_ffmpeg_filter(wb_lipsync_engine *eng)
{
    if (!eng || eng->n_frames == 0) return "";

    char *buf = eng->ffmpeg_filter;
    int   rem = WB_LIPSYNC_MAX_FFTEXT;
    int   off = 0;

    off += snprintf(buf + off, rem - off,
        "select='");
    for (int i = 0; i < eng->n_frames && off < rem - 64; i++) {
        float t0 = eng->frames[i].start_time;
        float t1 = eng->frames[i].end_time;
        int   vid = eng->frames[i].viseme_id;
        if (i > 0)
            off += snprintf(buf + off, rem - off, "+");
        off += snprintf(buf + off, rem - off,
            "between(t\\,%.3f\\,%.3f)*%d", t0, t1, vid);
    }
    off += snprintf(buf + off, rem - off,
        "',setpts=PTS-STARTPTS");

    return buf;
}

/* Generate an ffmpeg overlay filter that selects from 15 mouth PNG files.
 * mouth_prefix: path prefix, e.g. "mouths/mouth_" → mouths/mouth_SIL.png etc.
 * Returns a filter string in eng->ffmpeg_filter for the overlay chain. */
const char *wb_lipsync_generate_mouth_overlay(wb_lipsync_engine *eng,
                                               const char *mouth_prefix)
{
    if (!eng || eng->n_frames == 0) return "";

    char *buf = eng->ffmpeg_filter;
    int   rem = WB_LIPSYNC_MAX_FFTEXT;
    int   off = 0;

    /* Build an overlay expression that activates during viseme frames */
    off += snprintf(buf + off, rem - off,
        "overlay=x=0:y=0:enable='");
    for (int i = 0; i < eng->n_frames && off < rem - 64; i++) {
        float t0 = eng->frames[i].start_time;
        float t1 = eng->frames[i].end_time;
        if (i > 0)
            off += snprintf(buf + off, rem - off, "+");
        off += snprintf(buf + off, rem - off,
            "between(t\\,%.3f\\,%.3f)", t0, t1);
    }
    off += snprintf(buf + off, rem - off, "'");

    (void)mouth_prefix; /* used by caller to build the full filter chain */
    return buf;
}

/* Set blend speed (0..1, higher = faster transitions) */
void wb_lipsync_set_blend_speed(wb_lipsync_engine *eng, float speed)
{
    if (!eng) return;
    eng->blend_speed = speed < 0.0f ? 0.0f : (speed > 1.0f ? 1.0f : speed);
}

/* Get the viseme name for a given ID */
const char *wb_lipsync_viseme_name(int viseme_id)
{
    return viseme_id_to_name(viseme_id);
}

/* Get the mouth shape descriptor for a given viseme ID */
const char *wb_lipsync_mouth_shape(int viseme_id)
{
    return viseme_mouth_shape(viseme_id);
}

/* Map a wb_phoneme_type to a viseme ID (public utility) */
int wb_phoneme_to_viseme(wb_phoneme_type p)
{
    return phoneme_to_viseme_id(p);
}

/* ---- Bridge to existing wb_viseme enum (for compatibility) ---- */
wb_viseme wb_lipsync_map_to_legacy_viseme(int oculus_viseme_id)
{
    switch (oculus_viseme_id) {
    case WB_VISEME_SIL: return VISEME_REST;
    case WB_VISEME_AA:  return VISEME_AH;
    case WB_VISEME_EE:  return VISEME_EE;
    case WB_VISEME_OH:  return VISEME_OH;
    case WB_VISEME_OO:  return VISEME_OO;
    case WB_VISEME_FF:  return VISEME_FV;
    case WB_VISEME_PP:  return VISEME_MBP;
    case WB_VISEME_DD:
    case WB_VISEME_NN:  return VISEME_L;
    case WB_VISEME_TH:  return VISEME_TH;
    case WB_VISEME_RR:  return VISEME_W;
    default:            return VISEME_REST;
    }
}