/* wb_arrange_ai.c — AI arrangement assistant.
 *
 * Auto-arranges clips into song structures (verse/chorus/bridge/etc) based
 * on a named style. Assigns clips to sections by duration and a derived
 * "energy" heuristic so longer/higher-energy clips land on chorus/drop-type
 * sections. Pure C11, zero third-party. Uses xorshift32 for reproducibility.
 *
 * API: wb_arrange_ai_create / destroy / arrange / get_section_name / set_tempo.
 */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "wbus.h"

/* ---- section ids ---- */
#define WB_ARR_INTRO     0
#define WB_ARR_VERSE     1
#define WB_ARR_CHORUS    2
#define WB_ARR_BRIDGE    3
#define WB_ARR_SOLO      4
#define WB_ARR_OUTRO     5
#define WB_ARR_BUILD     6
#define WB_ARR_DROP      7
#define WB_ARR_BREAKDOWN 8
#define WB_ARR_HEAD      9
#define WB_ARR_NUM_SECTIONS 10

static const char *section_names[WB_ARR_NUM_SECTIONS] = {
    "intro", "verse", "chorus", "bridge", "solo", "outro",
    "build", "drop", "breakdown", "head"
};

/* ---- style definitions ---- */
typedef struct {
    const char *name;
    const int  *sections;   /* section id array */
    int         num_sections;
} wb_style;

/* Pop: intro-verse-chorus-verse-chorus-bridge-chorus-outro */
static const int pop_sections[] = {
    WB_ARR_INTRO, WB_ARR_VERSE, WB_ARR_CHORUS,
    WB_ARR_VERSE, WB_ARR_CHORUS, WB_ARR_BRIDGE,
    WB_ARR_CHORUS, WB_ARR_OUTRO
};

/* Rock: intro-verse-chorus-verse-chorus-solo-chorus-outro */
static const int rock_sections[] = {
    WB_ARR_INTRO, WB_ARR_VERSE, WB_ARR_CHORUS,
    WB_ARR_VERSE, WB_ARR_CHORUS, WB_ARR_SOLO,
    WB_ARR_CHORUS, WB_ARR_OUTRO
};

/* EDM: build-drop-build-drop-breakdown-drop-outro */
static const int edm_sections[] = {
    WB_ARR_BUILD, WB_ARR_DROP, WB_ARR_BUILD, WB_ARR_DROP,
    WB_ARR_BREAKDOWN, WB_ARR_DROP, WB_ARR_OUTRO
};

/* Hiphop: intro-hook-verse-hook-verse-hook-bridge-hook-outro
 * "hook" maps to chorus in our section vocabulary */
static const int hiphop_sections[] = {
    WB_ARR_INTRO, WB_ARR_CHORUS, WB_ARR_VERSE, WB_ARR_CHORUS,
    WB_ARR_VERSE, WB_ARR_CHORUS, WB_ARR_BRIDGE, WB_ARR_CHORUS, WB_ARR_OUTRO
};

/* Jazz: head-solo-solo-solo-head */
static const int jazz_sections[] = {
    WB_ARR_HEAD, WB_ARR_SOLO, WB_ARR_SOLO, WB_ARR_SOLO, WB_ARR_HEAD
};

static const wb_style styles[] = {
    { "pop",    pop_sections,    (int)(sizeof(pop_sections)/sizeof(pop_sections[0])) },
    { "rock",   rock_sections,   (int)(sizeof(rock_sections)/sizeof(rock_sections[0])) },
    { "edm",    edm_sections,    (int)(sizeof(edm_sections)/sizeof(edm_sections[0])) },
    { "hiphop", hiphop_sections, (int)(sizeof(hiphop_sections)/sizeof(hiphop_sections[0])) },
    { "jazz",   jazz_sections,   (int)(sizeof(jazz_sections)/sizeof(jazz_sections[0])) },
};
#define WB_NUM_STYLES (sizeof(styles)/sizeof(styles[0]))

/* ---- arrangement state ---- */
#define WB_ARR_MAX_CLIPS 256

typedef struct {
    float    bpm;                        /* current tempo */
    uint32_t seed;                       /* PRNG state */
    int      clip_energy[WB_ARR_MAX_CLIPS]; /* derived energy per clip (sorted index) */
} wb_arrange_ai;

/* ---- PRNG: xorshift32 ---- */
static uint32_t xor32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static int randi(uint32_t *state, int n) {
    return n > 0 ? (int)(xor32(state) % (uint32_t)n) : 0;
}

/* ---- section energy weight ---- */
/* Higher = more energetic section. Used to match high-energy clips to
 * high-energy sections (chorus, drop, etc). */
static float section_energy_weight(int section_id) {
    switch (section_id) {
    case WB_ARR_INTRO:     return 0.3f;
    case WB_ARR_VERSE:     return 0.4f;
    case WB_ARR_CHORUS:    return 0.85f;
    case WB_ARR_BRIDGE:    return 0.6f;
    case WB_ARR_SOLO:      return 0.8f;
    case WB_ARR_OUTRO:     return 0.25f;
    case WB_ARR_BUILD:     return 0.65f;
    case WB_ARR_DROP:      return 1.0f;
    case WB_ARR_BREAKDOWN: return 0.35f;
    case WB_ARR_HEAD:      return 0.7f;
    default:               return 0.5f;
    }
}

/* ---- clip energy derivation ---- */
/* Energy heuristic: normalize duration against the longest clip, then add
 * a small deterministic per-index jitter so clips of equal duration still
 * get a stable ordering. Returns 0..1000 scale for integer comparison. */
static void compute_clip_energies(wb_arrange_ai *a, int num_clips,
                                  const int *clip_durations) {
    if (num_clips <= 0) return;
    if (num_clips > WB_ARR_MAX_CLIPS) num_clips = WB_ARR_MAX_CLIPS;

    /* find max duration */
    int max_dur = 1;
    for (int i = 0; i < num_clips; i++) {
        if (clip_durations[i] > max_dur) max_dur = clip_durations[i];
    }

    for (int i = 0; i < num_clips; i++) {
        float norm = (float)clip_durations[i] / (float)max_dur;
        /* small deterministic jitter from PRNG so ties break consistently */
        float jitter = (float)(randi(&a->seed, 100)) / 1000.0f; /* 0..0.099 */
        a->clip_energy[i] = (int)((norm + jitter) * 1000.0f);
    }
}

/* ---- public API ---- */

void *wb_arrange_ai_create(void) {
    wb_arrange_ai *a = (wb_arrange_ai *)calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->bpm = 120.0f;
    a->seed = 1;
    return a;
}

void wb_arrange_ai_destroy(void *handle) {
    wb_arrange_ai *a = (wb_arrange_ai *)handle;
    free(a);
}

void wb_arrange_ai_set_tempo(void *handle, float bpm) {
    wb_arrange_ai *a = (wb_arrange_ai *)handle;
    if (!a) return;
    if (bpm <= 0.0f) bpm = 120.0f;
    if (bpm > 999.0f) bpm = 999.0f;
    a->bpm = bpm;
}

/* Find the style by name. Returns NULL if not found. */
static const wb_style *find_style(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < WB_NUM_STYLES; i++) {
        if (strcmp(name, styles[i].name) == 0)
            return &styles[i];
    }
    return NULL;
}

/* Arrange clips into a song structure.
 *
 * a              : handle from wb_arrange_ai_create()
 * style          : one of "pop", "rock", "edm", "hiphop", "jazz"
 * num_clips      : number of clips
 * clip_durations : array of clip durations (in arbitrary consistent units)
 * out_order      : output array of size num_clips — clip indices in arranged order
 * out_sections   : output array of size num_clips — section id per position
 *
 * Returns 0 on success, -1 on error (bad style, NULL args, etc).
 *
 * Algorithm:
 *   1. Look up the style's section template.
 *   2. Compute per-clip energy from duration + deterministic jitter.
 *   3. Tile the section template to cover num_clips (repeat the pattern).
 *   4. Build a target energy per position from the section's weight.
 *   5. Greedily assign highest-energy clips to highest-energy sections,
 *      with tie-breaking by PRNG for variety.
 */
int wb_arrange_ai_arrange(void *handle, const char *style, int num_clips,
                          int *clip_durations, int *out_order, int *out_sections) {
    wb_arrange_ai *a = (wb_arrange_ai *)handle;
    if (!a || !style || num_clips <= 0 || !clip_durations || !out_order || !out_sections)
        return -1;

    const wb_style *s = find_style(style);
    if (!s) return -1;

    if (num_clips > WB_ARR_MAX_CLIPS) num_clips = WB_ARR_MAX_CLIPS;

    /* 1. compute clip energies */
    compute_clip_energies(a, num_clips, clip_durations);

    /* 2. build the section sequence: tile the template to cover num_clips */
    int sections_seq[WB_ARR_MAX_CLIPS];
    for (int i = 0; i < num_clips; i++) {
        sections_seq[i] = s->sections[i % s->num_sections];
    }

    /* 3. compute target energy per position from section weight + small jitter */
    int pos_energy[WB_ARR_MAX_CLIPS];
    for (int i = 0; i < num_clips; i++) {
        float w = section_energy_weight(sections_seq[i]);
        float jitter = (float)(randi(&a->seed, 50)) / 1000.0f; /* 0..0.049 */
        pos_energy[i] = (int)((w + jitter) * 1000.0f);
    }

    /* 4. Greedy assignment: sort positions by target energy (descending),
    *     sort clips by energy (descending), assign highest clip to highest
    *     position. Use stable matching with index tiebreak. */
    /* clip indices sorted by energy desc */
    int clip_idx[WB_ARR_MAX_CLIPS];
    for (int i = 0; i < num_clips; i++) clip_idx[i] = i;
    /* insertion sort (small N) */
    for (int i = 1; i < num_clips; i++) {
        int key = clip_idx[i];
        int key_e = a->clip_energy[key];
        int j = i - 1;
        while (j >= 0 && a->clip_energy[clip_idx[j]] < key_e) {
            clip_idx[j + 1] = clip_idx[j];
            j--;
        }
        clip_idx[j + 1] = key;
    }

    /* position indices sorted by target energy desc */
    int pos_idx[WB_ARR_MAX_CLIPS];
    for (int i = 0; i < num_clips; i++) pos_idx[i] = i;
    for (int i = 1; i < num_clips; i++) {
        int key = pos_idx[i];
        int key_e = pos_energy[key];
        int j = i - 1;
        while (j >= 0 && pos_energy[pos_idx[j]] < key_e) {
            pos_idx[j + 1] = pos_idx[j];
            j--;
        }
        pos_idx[j + 1] = key;
    }

    /* assign: highest-energy clip -> highest-energy position */
    for (int rank = 0; rank < num_clips; rank++) {
        int p = pos_idx[rank];
        int c = clip_idx[rank];
        out_order[p] = c;
        out_sections[p] = sections_seq[p];
    }

    return 0;
}

/* Get the human-readable name for a section id.
 * Returns 0 on success, -1 on bad id. */
int wb_arrange_ai_get_section_name(int section_id, char *name_out, int cap) {
    if (section_id < 0 || section_id >= WB_ARR_NUM_SECTIONS) return -1;
    if (!name_out || cap <= 0) return -1;
    strncpy(name_out, section_names[section_id], (size_t)(cap - 1));
    name_out[cap - 1] = '\0';
    return 0;
}