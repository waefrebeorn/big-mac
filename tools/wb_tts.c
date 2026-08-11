/*
 * wb_tts.c — Big Mac TEXT-TO-SPEECH (pure articulatory, no neural nets)
 *
 * The proper TTS pipeline, taking the ecosystem's audio research
 * (wuburvc/knowledge/WUBU_AUDIO_RESEARCH.md) and CMUdict (public domain)
 * as the reference:
 *
 *   text -> words -> phones (CMUdict lookup + rule fallback)
 *        -> articulation gestures (full ARPABET table: tongue/lips/
 *           velum/voicing/turbulence)
 *        -> prosody (declarative/question intonation, stress, rhythm)
 *        -> render through the character throat (the 15 presets)
 *
 * Consonant handling per the research: voiceless consonants (s/sh/f/p/t/k)
 * get glottis OFF + turbulence noise (the "protect voiceless consonants"
 * rule); voiced consonants get glottis ON + constriction; nasals open the
 * velum. Vowels carry pitch/intonation.
 *
 * Usage: wb_tts "<text>" <character> <out.wav> [f0]
 */
#include "wb_tract.h"
#include "wb_glottis.h"
#include "wb_wav.h"
#include "wb_aiff.h"
#include "wb_measure.h"
#include "wb_mlp.h"
#include "data/tts_dict.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#define SR 44100
#define BLOCK 1024

/* ---------------- character presets ---------------- */
typedef struct {
    const char *name;
    double f0, tenseness, jitter, shimmer, vib_depth, vib_rate;
    int tract_n;
    double ti, td, lips;   /* neutral articulation */
} wb_char_t;

static const wb_char_t CHARS[] = {
    { "mickey", 320, 0.85, 0.004, 0.01, 0.004, 6.5, 36, 14.0, 0.8, 0.80 },
    { "bugs",   175, 0.70, 0.006, 0.02, 0.003, 5.5, 44, 14.0, 0.9, 0.85 },
    { "homer",  105, 0.45, 0.012, 0.04, 0.002, 4.5, 44, 16.0, 1.2, 0.60 },
    { "donald", 250, 0.90, 0.008, 0.02, 0.005, 7.0, 36, 14.0, 0.6, 0.35 },
    { "spongebob",300,0.80, 0.006, 0.02, 0.004, 6.0, 36, 14.0, 0.8, 0.85 },
    { "daffy",  210, 0.75, 0.007, 0.02, 0.004, 6.0, 44, 14.0, 0.7, 0.70 },
    { "cartman",330, 0.80, 0.005, 0.02, 0.003, 5.0, 28, 14.0, 0.8, 0.80 },
    { "stewie", 240, 0.70, 0.005, 0.02, 0.003, 5.0, 36, 14.0, 0.8, 0.75 },
    { "bart",   290, 0.65, 0.009, 0.03, 0.003, 5.5, 28, 14.0, 0.8, 0.80 },
    { "scooby", 130, 0.55, 0.010, 0.03, 0.002, 4.5, 44, 15.0, 1.0, 0.70 },
    { "betty",  380, 0.85, 0.004, 0.01, 0.005, 7.0, 36, 14.0, 0.7, 0.80 },
    { "popeye", 120, 0.60, 0.014, 0.05, 0.002, 4.0, 44, 16.0, 1.1, 0.65 },
    { "shaggy", 150, 0.50, 0.010, 0.03, 0.002, 4.5, 44, 15.0, 1.0, 0.75 },
    { "yoda",   160, 0.55, 0.008, 0.03, 0.003, 5.0, 44, 15.0, 1.0, 0.70 },
    { "bender", 140, 0.60, 0.010, 0.03, 0.002, 4.0, 44, 15.0, 1.0, 0.60 },
};
#define N_CHARS (sizeof(CHARS)/sizeof(CHARS[0]))

static const wb_char_t *find_char(const char *name) {
    for (size_t i = 0; i < N_CHARS; i++)
        if (!strcmp(name, CHARS[i].name)) return &CHARS[i];
    return NULL;
}

/* ---------------- emotion presets (acoustic correlates, Scherer + the
 * 38-study systematic review: F0 mean/range, rate, intensity, voice
 * quality) ---------------- */
typedef struct {
    const char *name;
    double f0_shift;      /* multiply base f0 */
    double f0_range;      /* intonation excursion multiplier */
    double intensity;     /* glottis intensity */
    double tenseness;     /* 0 breathy .. 1 pressed */
    double jitter;
    double shimmer;
    double vib_depth;     /* trembling (fear) / warmth (happy) */
    double vib_rate;
} wb_emotion_t;

static const wb_emotion_t EMOTIONS[] = {
    { "neutral", 1.00, 1.00, 0.80, 0.65, 0.005, 0.01, 0.002, 5.0 },
    { "happy",   1.15, 1.60, 0.88, 0.60, 0.006, 0.02, 0.004, 6.0 },
    { "sad",     0.85, 0.55, 0.55, 0.45, 0.010, 0.04, 0.002, 4.0 },
    { "angry",   1.20, 1.80, 0.98, 0.92, 0.012, 0.05, 0.003, 5.5 },
    { "fearful", 1.30, 1.90, 0.90, 0.85, 0.018, 0.06, 0.010, 7.0 },
    { "surprised",1.35, 2.00, 0.95, 0.80, 0.008, 0.03, 0.006, 6.5 },
};

static const wb_emotion_t *find_emotion(const char *name) {
    for (size_t i = 0; i < sizeof(EMOTIONS)/sizeof(EMOTIONS[0]); i++)
        if (!strcmp(name, EMOTIONS[i].name)) return &EMOTIONS[i];
    return NULL;
}

/* ---------------- FNV-1a (must match gen_tts_dict.py) ---------------- */
static unsigned int fnv1a(const char *s) {
    unsigned int h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

/* ---------------- phone table: ARPABET -> articulation ---------------- */
typedef struct {
    const char *ph;    /* ARPABET, without stress digit */
    double ti;         /* tongue index */
    double td;         /* tongue diameter */
    double lips;
    double velum;      /* nasal coupling 0..1 */
    int voiced;        /* glottis on */
    double turb;       /* turbulence (fricative) 0..1 */
    double dur;        /* relative duration */
    double ti2, td2;   /* diphthong glide end-target (0 = none, gap G82) */
    double fric_fc;    /* fricative spectral center Hz (R012-A1, 0=flat) */
    double fric_bw;    /* fricative bandwidth Hz */
    double round;      /* R013 mouth: lip rounding 0..1 (pursed tube -> lower F2/F3) */
    double npos;       /* R013 mouth: turbulence source position in 44-scale
                          sections (0 = default/alveolar) */
} wb_phone_t;

/* Reference formant targets: F1/F2 for vowels (Peterson & Barney-ish),
 * mapped to the tract via the retrieval table at runtime. */
static const wb_phone_t PHONES[] = {
    /* vowels — ti/td tuned to the KL tract (14 = mid, lower = front).
     * td is now the REAL tongue constriction (R013 fix: tongue reaches tube),
     * so vowels use td 2.0-3.0 (open) — old 0.6-1.3 choked the tract. */
    { "AA", 20.0, 3.0, 0.90, 0.0, 1, 0.00, 1.0 },  /* father  */
    { "AE", 11.0, 2.6, 0.90, 0.0, 1, 0.00, 1.0 },  /* cat     */
    { "AH", 16.0, 3.0, 0.80, 0.0, 1, 0.00, 0.8 },  /* but     */
    { "AO", 18.0, 2.6, 0.60, 0.0, 1, 0.00, 1.0, 0.6 },  /* thought (rounded) */
    { "AW", 16.0, 2.8, 0.50, 0.0, 1, 0.00, 1.2, 18.5, 2.6, 0.6 },  /* cow     a->ʊ */
    { "AY", 13.0, 2.6, 0.85, 0.0, 1, 0.00, 1.2, 11.0, 2.4 },  /* hide    a->ɪ */
    { "EH", 10.0, 2.6, 0.90, 0.0, 1, 0.00, 0.9, 0, 0 },
    { "ER", 14.0, 2.2, 0.80, 0.0, 1, 0.00, 1.0, 0, 0 },  /* bird    */
    { "EY", 11.0, 2.6, 0.85, 0.0, 1, 0.00, 1.1, 10.0, 2.4 },  /* bait    e->ɪ */
    { "IH", 11.0, 2.2, 0.90, 0.0, 1, 0.00, 0.7 },  /* bit     */
    { "IY", 9.0, 2.4, 0.85, 0.0, 1, 0.00, 1.0 },  /* beet    */
    { "OW", 18.0, 2.6, 0.55, 0.0, 1, 0.00, 1.1, 18.5, 2.6, 0.9 },  /* boat    o->ʊ */
    { "OY", 18.0, 2.6, 0.60, 0.0, 1, 0.00, 1.2, 11.0, 2.4, 0.9 },  /* boy     ɔ->ɪ */
    { "UH", 17.0, 2.2, 0.50, 0.0, 1, 0.00, 0.8, 0.85 },  /* book    */
    { "UW", 18.5, 2.4, 0.30, 0.0, 1, 0.00, 1.0, 1.0 },  /* boot    */

    /* consonants */
    { "B", 14.0, 0.2, 0.10, 0.0, 1, 0.00, 0.4, 0, 0, 500, 400, 0, 39 },   /* stop labial (burst at lips) */
    { "CH", 29.0, 1.6, 0.60, 0.0, 0, 0.80, 0.5, 0, 0, 3200, 1600, 1.0, 29.5 },  /* affricate (closure -> open release) */
    { "D", 15.0, 0.2, 0.80, 0.0, 1, 0.00, 0.4, 0, 0, 4500, 1800, 0, 32 },   /* stop alveolar (burst at teeth) */
    { "DH", 30.0, 2.1, 0.70, 0.0, 1, 0.60, 0.5, 0, 0, 2500, 1500, 0, 31.5 },  /* th-voiced (dental) */
    { "F", 36.0, 2.1, 0.22, 0.0, 0, 0.55, 0.4, 0, 0, 2500, 1400, 0, 37.5 },   /* labiodental (constrict+noise at lip/teeth) */
    { "G", 17.0, 0.2, 0.80, 0.0, 1, 0.00, 0.4, 0, 0, 2000, 1500, 0, 26 },   /* stop velar (burst back) */
    { "HH", 16.0, 2.6, 0.90, 0.0, 0, 0.50, 0.4, 0, 0, 0, 0 },  /* h (open aspirate) */
    { "JH", 29.0, 1.6, 0.60, 0.0, 1, 0.80, 0.5, 0, 0, 0, 0, 1.0, 29.5 },  /* affricate voiced */
    { "K", 17.0, 0.2, 0.80, 0.0, 0, 0.00, 0.4, 0, 0, 2000, 1500, 0, 26 },   /* stop velar (burst back) */
    { "L", 14.0, 2.0, 0.80, 0.0, 1, 0.00, 0.6 },   /* liquid (open) */
    { "M", 14.5, 0.5, 0.10, 0.7, 1, 0.00, 0.5, 0, 0, 0, 0, 0, 39 },   /* nasal bilabial (velum) */
    { "N", 15.0, 0.5, 0.80, 0.7, 1, 0.00, 0.5, 0, 0, 0, 0, 0, 32 },   /* nasal alveolar */
    { "NG", 17.0, 0.5, 0.80, 0.7, 1, 0.00, 0.5, 0, 0, 0, 0, 0, 26 },  /* nasal velar */
    { "P", 14.0, 0.2, 0.10, 0.0, 0, 0.00, 0.4, 0, 0, 500, 400, 0, 39 },   /* stop labial (burst at lips) */
    { "R", 13.5, 2.0, 0.70, 0.0, 1, 0.00, 0.6, 0.5 },   /* liquid (open, lip-rounded) */
    { "S", 31.0, 2.1, 0.80, 0.0, 0, 0.65, 0.4, 0, 0, 6000, 2500, 0, 32.5 },  /* hard sibilant (constrict at teeth) */
    { "SH", 29.0, 2.1, 0.60, 0.0, 0, 0.65, 0.4, 0, 0, 3200, 1600, 1.0, 29.5 }, /* soft sibilant (constrict, rounded, big front cavity) */
    { "T", 15.0, 0.2, 0.80, 0.0, 0, 0.00, 0.4, 0, 0, 4500, 1500, 0, 32 },   /* stop alveolar (burst at teeth) */
    { "TH", 30.0, 2.1, 0.70, 0.0, 0, 0.60, 0.4, 0, 0, 3200, 1600, 0, 31.5 },  /* dental */
    { "V", 36.0, 2.1, 0.22, 0.0, 1, 0.55, 0.4, 0, 0, 2500, 1400, 0, 37.5 },   /* labiodental (constrict+noise at lip/teeth) */
    { "W", 19.0, 2.4, 0.35, 0.0, 1, 0.00, 0.5, 0, 0, 0, 0, 1.0 },   /* glide (rounded, open) */
    { "Y", 13.0, 2.4, 0.85, 0.0, 1, 0.00, 0.5, 0, 0, 0, 0 },   /* glide (open) */
    { "Z", 31.0, 2.1, 0.80, 0.0, 1, 0.65, 0.4, 0, 0, 6000, 2500, 0, 32.5 },  /* hard sibilant (constrict at teeth) */
    { "ZH", 29.0, 2.1, 0.60, 0.0, 1, 0.65, 0.4, 0, 0, 3200, 1600, 1.0, 29.5 }, /* soft sibilant (constrict, rounded, big front cavity) */

    /* ---------- R015 extended inventory (IPA-style, render via -p) ---------- */
    /* rounded-front + unrounded vowels */
    { "YY",  10.0, 2.4, 0.40, 0.0, 1, 0.00, 0.9, 0, 0, 0, 0, 1.0 },  /* y  close front rounded */
    { "OE",  10.0, 2.6, 0.45, 0.0, 1, 0.00, 1.0, 0, 0, 0, 0, 1.0 },  /* ø  close-mid front rounded */
    { "OEH", 11.0, 2.7, 0.50, 0.0, 1, 0.00, 1.0, 0, 0, 0, 0, 0.9 },  /* œ  open-mid front rounded */
    { "UUX", 18.5, 2.4, 0.85, 0.0, 1, 0.00, 0.9, 0, 0, 0, 0 },  /* ɯ  close back UNROUNDED (Japanese u) */
    { "SCH", 15.0, 2.6, 0.75, 0.0, 1, 0.00, 0.7 },  /* ə  schwa (reduced) */
    { "VAX", 16.0, 3.0, 0.85, 0.0, 1, 0.00, 0.8 },  /* ɐ  near-open central */
    /* nasal vowels */
    { "ENN", 10.0, 2.6, 0.85, 0.6, 1, 0.00, 1.0 },  /* ɛ̃  nasalized e */
    { "ANN", 20.0, 3.0, 0.85, 0.6, 1, 0.00, 1.0 },  /* ɑ̃  nasalized a */
    { "ONN", 18.0, 2.6, 0.60, 0.6, 1, 0.00, 1.0, 0, 0, 0, 0, 0.6 },  /* ɔ̃  nasalized o */
    /* palatal + uvular stops */
    { "CY", 12.0, 0.2, 0.80, 0.0, 0, 0.00, 0.4, 0, 0, 3000, 1500, 0, 28 },  /* c  voiceless palatal stop */
    { "JY", 12.0, 0.2, 0.80, 0.0, 1, 0.00, 0.4, 0, 0, 3000, 1500, 0, 28 },  /* ɟ  voiced palatal stop */
    { "QV", 22.0, 0.2, 0.80, 0.0, 0, 0.00, 0.4, 0, 0, 1500, 1000, 0, 23 },  /* q  voiceless uvular stop */
    { "GV", 22.0, 0.2, 0.80, 0.0, 1, 0.00, 0.4, 0, 0, 1500, 1000, 0, 23 },  /* ɢ  voiced uvular stop */
    { "QX", 24.0, 0.2, 0.90, 0.0, 0, 0.00, 0.3, 0, 0, 0, 0, 0, 2 },  /* ʔ  glottal stop */
    /* more fricatives */
    { "PHI", 14.0, 2.1, 0.30, 0.0, 0, 0.50, 0.4, 0, 0, 1500, 1000, 0, 39 },  /* ɸ  voiceless bilabial fricative */
    { "BTA", 14.0, 2.1, 0.30, 0.0, 1, 0.50, 0.4, 0, 0, 1500, 1000, 0, 39 },  /* β  voiced bilabial fricative */
    { "CJ",  12.0, 2.1, 0.80, 0.0, 1, 0.60, 0.4, 0, 0, 3500, 1500, 0, 28 },  /* ʝ  voiced palatal fricative */
    { "RHO", 22.0, 2.1, 0.70, 0.0, 1, 0.50, 0.4, 0, 0, 1200, 900, 0, 23 },  /* ʁ  voiced uvular fricative */
    { "KHI", 22.0, 2.1, 0.70, 0.0, 0, 0.50, 0.4, 0, 0, 1200, 900, 0, 23 },  /* χ  voiceless uvular fricative */
    /* lateral fricatives */
    { "LLL", 15.0, 1.8, 0.80, 0.0, 0, 0.60, 0.4, 0, 0, 4000, 1500, 0, 32 },  /* ɬ  voiceless lateral fricative */
    { "LLZ", 15.0, 1.8, 0.80, 0.0, 1, 0.60, 0.4, 0, 0, 4000, 1500, 0, 32 },  /* ɮ  voiced lateral fricative */
    /* retroflex series */
    { "RT", 14.0, 0.2, 0.80, 0.0, 0, 0.00, 0.4, 0, 0, 4000, 1500, 0, 31 },  /* ʈ  voiceless retroflex stop */
    { "RD", 14.0, 0.2, 0.80, 0.0, 1, 0.00, 0.4, 0, 0, 4000, 1500, 0, 31 },  /* ɖ  voiced retroflex stop */
    { "RS", 14.0, 2.1, 0.60, 0.0, 0, 0.60, 0.4, 0, 0, 3500, 1500, 0, 30 },  /* ʂ  voiceless retroflex fricative */
    { "RZ", 14.0, 2.1, 0.60, 0.0, 1, 0.60, 0.4, 0, 0, 3500, 1500, 0, 30 },  /* ʐ  voiced retroflex fricative */
    /* affricates */
    { "TS", 15.0, 1.6, 0.80, 0.0, 0, 0.80, 0.5, 0, 0, 6000, 2000, 0, 32 },  /* ts */
    { "DZ", 15.0, 1.6, 0.80, 0.0, 1, 0.80, 0.5, 0, 0, 6000, 2000, 0, 32 },  /* dz */
    { "PF", 14.0, 1.6, 0.30, 0.0, 0, 0.70, 0.5, 0, 0, 2500, 1200, 0, 38 },  /* pf */
    /* approximants */
    { "VV", 14.0, 2.4, 0.40, 0.0, 1, 0.00, 0.5 },  /* ʋ  labiodental approximant */
    { "RY", 14.0, 2.0, 0.70, 0.0, 1, 0.00, 0.6, 0, 0, 0, 0, 0.5 },  /* ɹ  alveolar approximant */
    { "JW", 12.0, 2.4, 0.40, 0.0, 1, 0.00, 0.5, 0, 0, 0, 0, 1.0 },  /* ɥ  labial-palatal approximant */
    { "GW", 20.0, 2.4, 0.80, 0.0, 1, 0.00, 0.5 },  /* ɰ  velar approximant */
};

static const wb_phone_t *find_phone(const char *ph, int *stress) {
    *stress = 0;
    /* strip stress digit (0-2) */
    char p[4];
    size_t n = strlen(ph);
    if (n >= 1 && ph[n-1] >= '0' && ph[n-1] <= '2') {
        *stress = ph[n-1] - '0';
        n--;
    }
    if (n > 3) n = 3;
    memcpy(p, ph, n); p[n] = 0;
    for (size_t i = 0; i < sizeof(PHONES)/sizeof(PHONES[0]); i++) {
        if (!strcmp(PHONES[i].ph, p)) return &PHONES[i];
    }
    return NULL;
}

/* ---------------- CMUdict lookup + fallback ---------------- */
static const char *lookup_word(const char *word) {
    unsigned int h = fnv1a(word);
    int lo = 0, hi = WB_DICT_N - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (WB_DICT[mid].hash == h) return WB_DICT[mid].phones;
        if (WB_DICT[mid].hash < h) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

/* ---------------- the 7 tone contours (WordVoice taxonomy) ---------------- */
/* tone_shape returns the f0 multiplier at fraction u (0..1) of the word. */
static double tone_shape(int tone, double u) {
    switch (tone) {
    case 0: return 1.00;                    /* flat */
    case 1: return 1.00 + 0.15 * u;         /* rise */
    case 2: return 1.00 + 0.30 * u;         /* strong_rise */
    case 3: return 1.15 - 0.15 * u;         /* fall */
    case 4: return 1.30 - 0.30 * u;         /* strong_fall */
    case 5: return 1.00 + 0.30 * sin(M_PI * u);       /* peak */
    case 6: return 1.15 - 0.30 * sin(M_PI * u);       /* valley */
    default: return 1.00;
    }
}

/* Tokyo-style binary H/L pitch accent: ONE tonic (high) mora per word.
 * Rule (Tokyo dialect): if the accent is on the first mora -> HꜜL L L…;
 * if on a later mora -> L H…Hꜜ L L… (first low, accented up to it high,
 * then a downstep to low). High = +step, low = -step. */
static double pa_high(int accent, int i) {
    if (accent == 0) return (i == 0) ? 1.0 : 0.0;      /* Hꜜ L L L */
    return (i >= 1 && i <= accent) ? 1.0 : 0.0;         /* L H..Hꜜ L L */
}

/* ---------------- render ---------------- */
/* biquad bandpass state (R012-A1 fricative spectral shaping) */
typedef struct {
    double b0, b1, b2, a1, a2;   /* coefficients */
    double z1, z2;               /* state */
} wb_biquad_t;

typedef struct {
    wb_tract_t *tract;
    wb_glottis_t *glottis;
    const wb_char_t *ch;
    double *out;
    int nsamp;
    double f0_phrase_start, f0_phrase_end;  /* intonation contour */
    wb_biquad_t fric_filt;   /* fricative spectral shaper (R012-A1) */
    wb_biquad_t nasal_notch; /* nasal antiformant zero (P0) */
    int sine_mode;           /* formant-track sine render (P2) */
    double sine_ph1, sine_ph2, sine_ph3;  /* sine oscillator phases */
} wb_tts_t;

/* Vowel formant targets (Peterson & Barney) for the sine-track mode. */
static const struct { const char *ph; double f1, f2, f3; } WB_FORMANTS[] = {
    { "AA", 730, 1090, 2440 }, { "AE", 660, 1720, 2410 }, { "AH", 640, 1190, 2390 },
    { "AO", 570,  840, 2410 }, { "AW", 640, 1190, 2390 }, { "AY", 600, 1800, 2500 },
    { "EH", 530, 1840, 2480 }, { "ER", 490, 1350, 1690 }, { "EY", 440, 2150, 2760 },
    { "IH", 390, 1990, 2550 }, { "IY", 270, 2290, 3010 }, { "OW", 570,  840, 2410 },
    { "OY", 470, 1050, 2500 }, { "UH", 440, 1020, 2240 }, { "UW", 300,  870, 2240 },
};

/* Formant track for a phone: vowels get their Peterson-Barney targets.
 * Consonants have an F2 "locus" (Delattre & Liberman 1952 — the F2 frequency
 * the transition starts/ends at, the primary place cue) and the render glides
 * from that locus into the vowel's F2. Loci approximate classic values:
 * labial /b p/ ~700, alveolar /d t n/ ~1800, velar /g k ŋ/ ~2500. */
static double consonant_locus(const wb_phone_t *ph) {
    switch (ph->ph[0]) {
    case 'B': case 'P': case 'M': case 'W': return 700.0;
    case 'D': case 'T': case 'N': return 1800.0;
    case 'G': case 'K': return 2500.0;
    case 'L': case 'R': return 1300.0;
    case 'S': case 'Z': return 4000.0;
    case 'F': case 'V': case 'TH': case 'DH': return 1700.0;
    case 'Y': return 2200.0;
    default: return 1600.0;
    }
}
static void phone_formants(const wb_phone_t *ph, const wb_phone_t *prev,
                           const wb_phone_t *next, double *f1, double *f2, double *f3) {
    for (size_t k = 0; k < sizeof(WB_FORMANTS)/sizeof(WB_FORMANTS[0]); k++)
        if (!strcmp(WB_FORMANTS[k].ph, ph->ph)) { *f1=WB_FORMANTS[k].f1; *f2=WB_FORMANTS[k].f2; *f3=WB_FORMANTS[k].f3; return; }
    /* consonant: NG is a two-letter phone; check it before the single-letter switch */
    if (strcmp(ph->ph, "NG") == 0) { *f1=300; *f2=2500; *f3=2400; return; }
    if (strcmp(ph->ph, "SH") == 0 || strcmp(ph->ph, "ZH") == 0 ||
        strcmp(ph->ph, "CH") == 0 || strcmp(ph->ph, "JH") == 0) { *f1=300; *f2=2500; *f3=3000; return; }
    *f1 = 300.0;
    *f2 = consonant_locus(ph);
    *f3 = 2500.0;
}

/* Nasal antiformant (P0): the murmur of /m/ /n/ /ŋ/ has a spectral ZERO
 * (the antiformant) that is the key place cue — /m/ ~750-1250 Hz, /n/
 * ~2500 Hz, /ŋ/ ~1800 Hz. We notch the tract output during nasal phones. */
static double nasal_antiformant_fc(const wb_phone_t *ph) {
    if (ph->velum < 0.5) return 0.0;              /* not a nasal */
    if (ph->ph[0] == 'M') return 1000.0;          /* /m/ labial */
    if (ph->ph[0] == 'N') return 2500.0;          /* /n/ alveolar */
    if (strcmp(ph->ph, "NG") == 0) return 1800.0; /* /ŋ/ velar */
    return 0.0;
}

/* Coarticulation smoothing (R010 gap A1/A2/A3 — VTL-style):
 * the articulators don't snap between phone targets; they GLIDE with
 * overlap. At the start of a phone we're still arriving from the previous
 * phone's gesture (carryover); near the end we're already anticipating the
 * next phone's gesture (anticipatory). And vowels never fully reach their
 * target before the next consonant pulls away (undershoot). We blend the
 * (ti, td, lips, velum) targets with a cosine overlap window. */
static void blend_targets(const wb_phone_t *prev, const wb_phone_t *cur,
                          const wb_phone_t *next, double t,   /* 0..1 in phone */
                          double *ti, double *td, double *lips, double *velum,
                          double *round) {
    double carry = 0, anti = 0;
    /* first 30% dominated by carryover from prev, last 30% anticipation
     * of next; middle is pure current target (with undershoot toward the
     * surrounding consonant positions for vowels) */
    if (t < 0.30) carry = 1.0 - t / 0.30;
    if (t > 0.70) anti = (t - 0.70) / 0.30;
    double w_cur = 1.0 - 0.6 * (carry + anti);   /* current never fully dominates */
    w_cur = w_cur < 0.15 ? 0.15 : w_cur;

    const wb_phone_t *p = prev ? prev : cur;
    const wb_phone_t *n = next ? next : cur;
    double cti = cur->ti, ctd = cur->td;
    /* diphthong glide (gap G82): the vowel's target moves from its start
     * target toward ti2/td2 across the phone (monophthongs have ti2=0) */
    if (cur->ti2 != 0.0 && t > 0.2) {
        double g = (t - 0.2) / 0.8;   /* glide across the phone body */
        if (g > 1.0) g = 1.0;
        cti = cur->ti + (cur->ti2 - cur->ti) * g;
        ctd = cur->td + (cur->td2 - cur->td) * g;
    }
    *ti    = p->ti    * carry + cti           * w_cur + n->ti    * anti;
    *td    = p->td    * carry + ctd           * w_cur + n->td    * anti;
    *lips  = p->lips  * carry + cur->lips     * w_cur + n->lips  * anti;
    *velum = p->velum * carry + cur->velum    * w_cur + n->velum * anti;
    *round = p->round * carry + cur->round    * w_cur + n->round * anti;
}

/* ---------------- biquad bandpass filter for fricative shaping (R012-A1)
 * Shapes the turbulence noise spectrum to the fricative's front-cavity
 * resonance (Birkholz 2006): /s/ ~6000Hz, /ʃ/ ~3200Hz, /f/ ~2500Hz.
 * Direct-form-II biquad, state kept per phone. */
static void wb_biquad_bandpass(wb_biquad_t *f, double fc, double bw, int sr) {
    double q = fc / bw;
    if (q < 0.5) q = 0.5;
    if (q > 20) q = 20;
    double w0 = 2.0 * M_PI * fc / sr;
    double alpha = sin(w0) / (2.0 * q);
    double cosw = cos(w0);
    double a0 = 1.0 + alpha;
    f->b0 = alpha / a0;
    f->b1 = 0.0;
    f->b2 = -alpha / a0;
    f->a1 = -2.0 * cosw / a0;
    f->a2 = (1.0 - alpha) / a0;
    f->z1 = f->z2 = 0.0;
}

static double wb_biquad_run(wb_biquad_t *f, double x) {
    double y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

/* Biquad notch (band-reject) — used for the nasal antiformant zero. */
static void wb_biquad_notch(wb_biquad_t *f, double fc, double bw, int sr) {
    double q = fc / bw;
    if (q < 0.5) q = 0.5;
    if (q > 20) q = 20;
    double w0 = 2.0 * M_PI * fc / sr;
    double alpha = sin(w0) / (2.0 * q);
    double cosw = cos(w0);
    double a0 = 1.0 + alpha;
    f->b0 = 1.0 / a0;
    f->b1 = -2.0 * cosw / a0;
    f->b2 = 1.0 / a0;
    f->a1 = -2.0 * cosw / a0;
    f->a2 = (1.0 - alpha) / a0;
    f->z1 = f->z2 = 0.0;
}

static void tts_render(wb_tts_t *T, double t0, double dur,
                       const wb_phone_t *ph, int stress,
                       const wb_phone_t *prev, const wb_phone_t *next,
                       double f0_start, double f0_end, double energy) {
    (void)stress;   /* (stress used for amplitude in the caller) */
    int s0 = (int)(t0 * SR), s1 = (int)((t0 + dur) * SR);
    if (s1 > T->nsamp) s1 = T->nsamp;
    /* stop detection: closed tract (td small), no frication, not nasal.
     * Stops get a closure phase then a release burst (gap D39) + aspiration.
     * Affricates CH/JH are stops that release into a fricative instead of a
     * burst, so they share the closure phase. */
    int is_stop = (ph->td < 0.3 && ph->turb < 0.05 && ph->velum < 0.05);
    int is_voiceless_stop = is_stop && !ph->voiced;
    int is_affricate = !is_stop && (strcmp(ph->ph, "CH") == 0 || strcmp(ph->ph, "JH") == 0);
    int has_closure = is_stop || is_affricate;
    /* init the fricative spectral filter for this phone */
    if (ph->fric_fc > 0) wb_biquad_bandpass(&T->fric_filt, ph->fric_fc, ph->fric_bw, SR);
    else { T->fric_filt.a1 = T->fric_filt.a2 = T->fric_filt.z1 = T->fric_filt.z2 = 0; }
    double nf_fc = nasal_antiformant_fc(ph);   /* P0: nasal spectral zero */
    if (nf_fc > 0) wb_biquad_notch(&T->nasal_notch, nf_fc, 400.0, SR);
    int dur_samp = s1 - s0;
    /* Stop closure phase: a real stop holds a closed tract in silence
     * (voiceless /p t k/) or a low "voice bar" (voiced /b d g/) for the
     * first ~55% of its duration, THEN releases. The old code fired the
     * burst at onset with no closure, which made stops sound mushy and
     * smear into the following vowel. Affricates get a shorter (~40%)
     * closure before the fricative release. */
    int closure_frac = is_affricate ? 40 : 55;
    int closure_samples = has_closure ? (dur_samp * closure_frac / 100) : 0;
    int release_start = closure_samples;
    for (int j = s0; j < s1; j++) {
        double t = (double)(j - s0) / (s1 - s0);   /* 0..1 within phone */
        int jj = j - s0;  /* sample into phone */
        /* f0 glide within the phone (intonation) */
        double f0 = f0_start + (f0_end - f0_start) * t;
        /* articulation: coarticulated blend of prev/cur/next targets */
        double ti, td, lips, velum, round;
        blend_targets(prev, ph, next, t, &ti, &td, &lips, &velum, &round);
        wb_tract_set_rest_diameter(T->tract, ti, td);
        wb_tract_set_lips(T->tract, lips);
        wb_tract_set_lip_rounding(T->tract, round);
        wb_tract_set_velum(T->tract, velum);
        /* R013 mouth: place the frication source at this phone's constriction
         * (labiodental /f v/ at the lips, sibilants at the teeth, stops at
         * their place). Scaled from 44-scale to this character's tract.
         * npos 0 = default alveolar (preserves Pink Trombone behaviour). */
        double npos = ph->npos > 0 ? ph->npos * T->ch->tract_n / 44.0 : -1.0;
        wb_tract_set_noise_pos(T->tract, npos);
        double noise = (double)((j * 2654435761u) >> 24) / 128.0 - 1.0;
        int phonate;
        double turb;
        if (has_closure) {
            if (jj < release_start) {
                /* CLOSURE: tract closed, no frication. Voiced stops keep a
                 * low-amplitude voice bar; voiceless stops are silent. */
                turb = 0.0;
                if (ph->voiced) {
                    wb_glottis_set_frequency(T->glottis, f0);
                    wb_glottis_set_intensity(T->glottis, 0.25);
                    phonate = 1;
                } else {
                    wb_glottis_set_intensity(T->glottis, 0.0);
                    phonate = 0;
                }
            } else if (is_affricate) {
                /* AFFRICATE release: the closure bursts into its homorganic
                 * fricative (CH -> ʃ, JH -> ʒ), not a stop burst. */
                wb_glottis_set_frequency(T->glottis, f0);
                wb_glottis_set_intensity(T->glottis, ph->voiced ? 0.6 : 0.0);
                phonate = ph->voiced;
                turb = noise * 0.3 * ph->turb;
                if (ph->fric_fc > 0)
                    turb = wb_biquad_run(&T->fric_filt, turb);
            } else {
                /* RELEASE: place-shaped burst (Dorman 1977: burst spectrum
                 * cues place — P low ~500Hz, T/K high ~4.5/2kHz via fric_fc),
                 * then a light aspiration tail for voiceless stops. */
                int rel = jj - release_start;
                double rel_t = (double)rel / SR;
                wb_glottis_set_intensity(T->glottis, 0.0);
                phonate = 0;
                double burst_env = exp(-rel_t / 0.008);   /* 8ms decay */
                turb = noise * (is_voiceless_stop ? 0.8 : 0.4) * burst_env;
                if (is_voiceless_stop && rel_t > 0.015)
                    turb = noise * 0.3 * 0.25;   /* light aspiration tail */
                if (ph->fric_fc > 0)
                    turb = wb_biquad_run(&T->fric_filt, turb);
            }
        } else {
            /* vowel or continuant: glottis on if voiced, frication if turb */
            phonate = ph->voiced;
            if (phonate) {
                wb_glottis_set_frequency(T->glottis, f0);
                wb_glottis_set_intensity(T->glottis, 0.8);
            } else {
                wb_glottis_set_intensity(T->glottis, 0.0);
            }
            turb = noise * 0.3 * ph->turb;
            if (ph->fric_fc > 0)
                turb = wb_biquad_run(&T->fric_filt, turb);
        }
        int m = j % BLOCK;
        double lam1 = (double)m / BLOCK, lam2 = (m + 0.5) / BLOCK;
        double gl = wb_glottis_run_step(T->glottis, lam1, noise * 0.3);
        double vocal = wb_tract_run_step(T->tract, gl, turb, lam1)
                     + wb_tract_run_step(T->tract, gl, turb, lam2);
        if (nf_fc > 0) vocal = wb_biquad_run(&T->nasal_notch, vocal);   /* nasal antiformant */
        /* phonation onset/offset transients (gap F80): smooth 10ms ramps
         * so voiced segments fade in/out instead of switching abruptly */
        double env = 1.0;
        int n_env = (int)(0.010 * SR);
        if (ph->voiced) {
            if (jj < n_env) env = (double)jj / n_env;                    /* onset */
            if (s1 - j - 1 < n_env) env = (double)(s1 - j - 1) / n_env;  /* offset */
        }
        T->out[j] += vocal * 0.125 * env * energy;
        if (m == BLOCK - 1) {
            wb_glottis_finish_block(T->glottis, phonate, (double)BLOCK / SR);
            wb_tract_finish_block(T->tract, (double)BLOCK / SR);
        }
    }
}

/* ---------------- P2 formant-track sine render (the "cheat") ----------------
 * Three sine oscillators track F1/F2/F3 (Remez sine-wave speech — the formant
 * TRACKS alone carry intelligibility). Ultra-light: a few sin() per sample,
 * no waveguide, ideal for the 1-core real-time voice-changer path. */
static void sine_render_phone(wb_tts_t *T, double t0, double dur,
                              const wb_phone_t *ph, const wb_phone_t *prev,
                              const wb_phone_t *next,
                              double f0_start, double f0_end, double energy) {
    int s0 = (int)(t0 * SR), s1 = (int)((t0 + dur) * SR);
    if (s1 > T->nsamp) s1 = T->nsamp;
    if (s1 <= s0) return;
    (void)f0_start; (void)f0_end;
    /* formant sets for prev / cur / next so the F2 locus glides into the
     * vowel's F2 (coarticulated, smooth — like blend_targets for the tract) */
    double pf1,pf2,pf3, cf1,cf2,cf3, nf1,nf2,nf3;
    phone_formants(prev ? prev : ph, prev, ph, &pf1,&pf2,&pf3);
    phone_formants(ph, prev, next, &cf1,&cf2,&cf3);
    phone_formants(next ? next : ph, ph, next, &nf1,&nf2,&nf3);
    double step = 1.0 / (double)SR;
    for (int j = s0; j < s1; j++) {
        double t = (double)(j - s0) / (double)(s1 - s0);
        double carry = 0, anti = 0;
        if (t < 0.30) carry = 1.0 - t / 0.30;
        if (t > 0.70) anti = (t - 0.70) / 0.30;
        double w = 1.0 - 0.6 * (carry + anti); if (w < 0.15) w = 0.15;
        double f1 = pf1*carry + cf1*w + nf1*anti;
        double f2 = pf2*carry + cf2*w + nf2*anti;
        double f3 = pf3*carry + cf3*w + nf3*anti;
        T->sine_ph1 += 2 * M_PI * f1 * step;
        T->sine_ph2 += 2 * M_PI * f2 * step;
        T->sine_ph3 += 2 * M_PI * f3 * step;
        double v = 0.5 * sin(T->sine_ph1) + 0.3 * sin(T->sine_ph2) + 0.2 * sin(T->sine_ph3);
        double amp = ph->voiced ? 1.0 : 0.12;   /* voiceless quieter */
        int jj = j - s0;
        int n_env = (int)(0.010 * SR);
        double env = 1.0;
        if (ph->voiced) {
            if (jj < n_env) env = (double)jj / n_env;
            if (s1 - j - 1 < n_env) env = (double)(s1 - j - 1) / n_env;
        }
        T->out[j] += v * amp * env * 0.25 * energy;
    }
}

/* ---------------- Klatt-style duration rules (gap B) ----------------
 * Base durations in seconds per phone class, then context modifiers:
 * stress (stressed ~1.3x), phrase-final lengthening, prepausal, consonant
 * cluster shortening, voiced-vowel lengthening. Base rate ~14 phones/sec
 * (real speech) instead of our old ~5/sec — the measured 1.7x slowness. */
static double phone_duration(const wb_phone_t *ph, int stress,
                             int is_phrase_final, int prev_consonant,
                             int next_consonant) {
    double d;
    /* classify: vowels = voiced, non-nasal (velum closed), no frication.
     * This correctly catches /IY/ (td 0.6) and all vowels. */
    int is_vowel = (ph->voiced && ph->turb < 0.05 && ph->velum < 0.05);
    if (is_vowel) {
        d = 0.085;                    /* vowel base (lengthened) */
        if (stress > 0) d *= 1.35;    /* stressed vowel longer */
    } else {
        d = 0.050;                    /* consonant base (shortened) */
    }
    /* Klatt context rules */
    if (is_phrase_final) d *= 1.30;           /* phrase-final lengthening */
    if (prev_consonant && next_consonant) d *= 0.75;  /* cluster shortening */
    if (is_vowel && ph->voiced) d *= 1.05;    /* voiced vowel slightly longer */
    /* reduce function words / schwa */
    if (stress == 0 && !is_vowel) d *= 0.80;  /* unstressed consonants shorter */
    if (d < 0.030) d = 0.030;
    if (d > 0.18) d = 0.18;
    return d;
}

/* pitch declination + downstep (gap C): F0 target at position u in phrase
 * (0..1), with phrase-level fall. */
static double phrase_f0(double u, double base) {
    double decl = 1.0 - 0.20 * u;              /* declination */
    return base * decl;
}

int main(int argc, char **argv) {
    /* -pa: Japanese-style binary H/L pitch accent + mora-isochronous timing
     * (Tokyo dialect: every word has ONE tonic mora). -sine: formant-track
     * "sine-wave speech" mode (Remez) — three oscillators track F1/F2/F3.
     * Filter both out so the positional parsing below is unchanged. */
    int g_pitch_accent = 0, g_sine_mode = 0, g_phone_mode = 0, g_whisper = 0, g_fry = 0;
    int g_tone = 0;   /* 1-4 Mandarin lexical tone applied per word */
    double g_breathiness = 0.0;
    {
        int w = 1;
        for (int a = 1; a < argc; a++) {
            if (!strcmp(argv[a], "-pa")) { g_pitch_accent = 1; continue; }
            if (!strcmp(argv[a], "-sine")) { g_sine_mode = 1; continue; }
            if (!strcmp(argv[a], "-p")) { g_phone_mode = 1; continue; }
            if (!strcmp(argv[a], "-whisper")) { g_whisper = 1; continue; }
            if (!strcmp(argv[a], "-fry")) { g_fry = 1; continue; }
            if (!strcmp(argv[a], "-breathy") && a + 1 < argc) { g_breathiness = atof(argv[++a]); continue; }
            if (!strcmp(argv[a], "-tone") && a + 1 < argc) { g_tone = atoi(argv[++a]); continue; }
            argv[w++] = argv[a];
        }
        argc = w;
    }
    if (argc < 4) {
        fprintf(stderr, "usage: wb_tts \\\\\\\"<text>\\\\\\\" <character> <out.wav> [f0] [emotion] [planner.mlp] [-pa] [-sine] [-p]\\n");
        fprintf(stderr, "       wb_tts -f <file> <character> <out.wav> [f0] [emotion] [planner.mlp] [-pa] [-sine] [-p]\\n");
        fprintf(stderr, "  emotions: neutral happy sad angry fearful surprised\\n");
        fprintf(stderr, "  -pa: Tokyo-style binary H/L pitch accent + mora timing (Japanese prosody)\\n");
        fprintf(stderr, "  -sine: formant-track mode — 3 sine oscillators track F1/F2/F3 (sine-wave speech, ultra-light)\\n");
        fprintf(stderr, "  -p: PHONE mode — each whitespace token is rendered as a phone code directly\\n");
        fprintf(stderr, "      (e.g. \\\\\\\"-p \\\\\\\"IY S IY K S\\\\\\\"\\\\\\\"), bypassing the dictionary. Lets you speak the full\\n");
        fprintf(stderr, "      inventory: rounded front vowels, nasal vowels, retroflex, uvular, lateral, etc.\\n");
        return 1;
    }
    /* read text from a file (API path, gap I97) */
    static char file_text[4096];
    const char *text;
    char *a2 = argv[2], *a3 = argv[3];
    char *a4 = argc > 4 ? argv[4] : NULL;
    char *a5 = argc > 5 ? argv[5] : NULL;
    char *a6 = argc > 6 ? argv[6] : NULL;
    if (!strcmp(argv[1], "-f") && argc >= 4) {
        FILE *tf = fopen(argv[2], "r");
        if (!tf) { fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
        size_t got = fread(file_text, 1, sizeof(file_text)-1, tf);
        file_text[got] = 0;
        fclose(tf);
        text = file_text;
        a2 = argv[3];               /* char */
        a3 = argc > 4 ? argv[4] : NULL;   /* out */
        a4 = argc > 5 ? argv[5] : NULL;   /* f0 */
        a5 = argc > 6 ? argv[6] : NULL;   /* emotion */
        a6 = argc > 7 ? argv[7] : NULL;   /* planner */
    } else {
        text = argv[1];
    }
    const wb_char_t *ch = find_char(a2);
    if (!ch) { fprintf(stderr, "unknown character %s\n", a2); return 1; }
    const char *out_path = a3;
    double base_f0 = a4 ? atof(a4) : ch->f0;
    const wb_emotion_t *em = &EMOTIONS[0];  /* neutral */
    if (a5) {
        const wb_emotion_t *e = find_emotion(a5);
        if (!e) { fprintf(stderr, "unknown emotion %s\n", a5); return 1; }
        em = e;
    }
    /* optional MLP planner (the WordVoice bound-token, non-neural) */
    const char *planner_path = a6;
    wb_mlp_t planner;
    int use_planner = 0;
    if (planner_path && wb_mlp_load(planner_path, &planner) == 0) {
        use_planner = 1;
        printf("using MLP prosody planner: %s\n", planner_path);
    }
    /* emotion applies to the character's voice */
    base_f0 *= em->f0_shift;
    double tempo = 0;  /* (unused; kept for readability) */
    (void)tempo;
    printf("tts: \"%s\" as %s (%s)\n", text, ch->name, em->name);

    /* ---------------- tokenize: words + punctuation ---------------- */
    char words[512][32];
    char punct[512];
    int nwords = 0;
    char tmp[2048];
    snprintf(tmp, sizeof(tmp), "%s", text);
    char *tok = strtok(tmp, " \t\n");
    while (tok && nwords < 511) {
        /* separate trailing punctuation */
        size_t len = strlen(tok);
        char w[32]; snprintf(w, sizeof(w), "%s", tok);
        punct[nwords] = 0;
        if (len > 1 && strchr(".,!?;:", w[len-1])) {
            w[len-1] = 0;
            punct[nwords] = tok[len-1];
        }
        /* lowercase (except in -p phone mode: phone codes are uppercase) */
        if (!g_phone_mode)
            for (size_t i = 0; w[i]; i++) w[i] = (char)tolower((unsigned char)w[i]);
        snprintf(words[nwords], 32, "%s", w);
        nwords++;
        tok = strtok(NULL, " \t\n");
    }

    /* ---------------- build phone sequence with prosody ---------------- */
    typedef struct {
        const wb_phone_t *ph;
        int stress;
        double dur;       /* seconds */
        double f0_start, f0_end;
        double energy;    /* amplitude gain for this phone (gap C28-33) */
    } wb_event_t;
    wb_event_t ev[2048];
    int nev = 0;

    /* phrase-level intonation: declarative falls, question rises */
    int is_question = 0;
    if (nwords > 0 && (punct[nwords-1] == '?' )) is_question = 1;
    if (nwords > 0 && punct[nwords-1] == 0 && strchr(text, '?')) is_question = 1;

    /* (tempo declared in main; unused now that phone_duration is explicit) */
    double phrase_dur = 0;

    /* first pass: estimate phrase duration */
    for (int wi = 0; wi < nwords; wi++) {
        const char *ph_str = g_phone_mode ? words[wi] : lookup_word(words[wi]);
        if (!ph_str) { phrase_dur += 0.25; continue; }
        char buf[64]; snprintf(buf, sizeof(buf), "%s", ph_str);
        char *p = strtok(buf, " ");
        while (p) {
            int st;
            const wb_phone_t *ph = find_phone(p, &st);
            if (ph) phrase_dur += phone_duration(ph, st, wi == nwords-1, 0, 0);
            p = strtok(NULL, " ");
        }
        phrase_dur += 0.06;  /* word gap (shorter than before) */
    }
    if (phrase_dur < 0.5) phrase_dur = 0.5;

    /* second pass: assign f0 contour (declarative: fall; question: rise) */
    double t_abs = 0.15;  /* lead-in */
    long j_global = 0;    /* sample-phase counter for microvariation */
    for (int wi = 0; wi < nwords; wi++) {
        const char *ph_str = g_phone_mode ? words[wi] : lookup_word(words[wi]);
        if (!ph_str) {
            /* fallback: spell the word (letter-by-letter via dict) */
            for (size_t k = 0; k < strlen(words[wi]) && nev < 2047; k++) {
                char letter[2] = { words[wi][k], 0 };
                const char *lph = lookup_word(letter);
                if (!lph) continue;
                char buf[16]; snprintf(buf, sizeof(buf), "%s", lph);
                char *p = strtok(buf, " ");
                while (p && nev < 2047) {
                    int st;
                    const wb_phone_t *ph = find_phone(p, &st);
                    if (ph) {
                        double f0s = base_f0, f0e = base_f0;
                        if (is_question) { f0s = base_f0 * 0.95; f0e = base_f0 * 1.35; }
                        else { f0s = base_f0 * 1.05; f0e = base_f0 * 0.85; }
                        ev[nev].ph = ph; ev[nev].stress = 0;
                        ev[nev].dur = phone_duration(ph, 0, wi == nwords-1, 0, 0);
                        ev[nev].f0_start = f0s; ev[nev].f0_end = f0e;
                        ev[nev].energy = 0.9;
                        t_abs += ev[nev].dur;
                        nev++;
                    }
                    p = strtok(NULL, " ");
                }
            }
            t_abs += 0.08;
            continue;
        }
        char buf[128]; snprintf(buf, sizeof(buf), "%s", ph_str);
        char *p = strtok(buf, " ");
        /* word-level pitch: stressed syllable gets a bump */
        int phone_idx = 0, nphones = 0;
        char *phones[16];
        while (p && nphones < 16) { phones[nphones++] = p; p = strtok(NULL, " "); }

        /* ---- MLP planner: predict word prosody (bound-token, non-neural) */
        double p_dur = 1.0, p_energy = em->intensity, p_pitch = 0;
        int p_tone = 0, p_boundary = 0;
        if (use_planner) {
            double feat[WB_MLP_IN], o[WB_MLP_OUT];
            feat[0] = (double)strlen(words[wi]);        /* word length */
            feat[1] = nphones / 2.0;                    /* syllables-ish */
            feat[2] = 0;                                 /* stress (filled per phone below) */
            feat[3] = (double)wi;                        /* position */
            feat[4] = wi == 0 ? 1.0 : 0.0;              /* is_first */
            feat[5] = wi == nwords - 1 ? 1.0 : 0.0;    /* is_last */
            feat[6] = 1.0;                              /* has_vowel */
            feat[7] = punct[wi] == ',' ? 1 : (punct[wi] ? 2 : 0);  /* punct */
            feat[8] = is_question ? 1.0 : 0.0;          /* is_question */
            feat[9] = 0;                                 /* prev_stress */
            feat[10] = (double)nphones;                 /* n_phones */
            feat[11] = 0.5;                             /* freq hint */
            wb_mlp_forward(&planner, feat, o);
            p_dur = o[0] * 2.0;
            p_energy = o[1] * em->intensity;
            p_pitch = o[2];
            int tmaxi = 0; double tm = o[3];
            for (int k = 4; k <= 9; k++) if (o[k] > tm) { tm = o[k]; tmaxi = k - 3; }
            p_tone = tmaxi;
            int bmaxi = 0; double bm = o[10];
            for (int k = 11; k <= 14; k++) if (o[k] > bm) { bm = o[k]; bmaxi = k - 10; }
            p_boundary = bmaxi;
        }

        /* R017 -tone N: cycling Mandarin lexical tone (1-4) per word.
         * T1 high-level(55), T2 rising(35), T3 dipping(214), T4 falling(51).
         * Mapped to tone_shape: flat / rise / valley / strong-fall. */
        if (g_tone) {
            int t = (wi % 4) + 1;
            p_tone = (t == 1) ? 0 : (t == 2) ? 1 : (t == 3) ? 6 : 4;
            p_pitch = (t == 1) ? 0.6 : 0.0;   /* T1 rides high */
        }

        /* Japanese pitch-accent: ONE tonic (high) mora per word — the first
         * stressed phone, else the last mora (common unaccented-last pattern). */
        int pa_accent = nphones - 1;
        if (g_pitch_accent) {
            for (int k = 0; k < nphones; k++) {
                int s2;
                if (find_phone(phones[k], &s2) && s2 > 0) { pa_accent = k; break; }
            }
        }

        for (int pi = 0; pi < nphones; pi++) {
            int st;
            const wb_phone_t *ph = find_phone(phones[pi], &st);
            if (!ph) continue;
            /* prosody: stress bump + phrase contour + declination */
            double f0s = base_f0, f0e = base_f0;
            double u_phrase = (double)wi / (nwords > 1 ? nwords - 1 : 1);
            if (g_pitch_accent) {
                /* binary H/L step: constant within the mora, no glide */
                double step = base_f0 * (pa_high(pa_accent, phone_idx) ? 1.16 : 0.88);
                f0s = f0e = step;
            } else {
            double local = (double)phone_idx / (nphones > 1 ? nphones - 1 : 1);
            /* apply the planner's tone shape (WordVoice 7-tone taxonomy),
             * scaled by the planner's pitch offset */
            double shape_s = tone_shape(p_tone, (double)phone_idx / (nphones > 0 ? nphones : 1));
            double shape_e = tone_shape(p_tone, (double)(phone_idx + 1) / (nphones > 0 ? nphones : 1));
            double pitch_mult = 1.0 + p_pitch * 0.25;   /* pitch offset -> semitones-ish */
            /* pitch declination: phrase-level fall + stress bump + microvariation */
            double decl = phrase_f0(u_phrase, 1.0);
            double micro = 1.0 + 0.02 * sin(2 * M_PI * 3.0 * (double)j_global + (double)wi);  /* 3Hz microvib */
            if (is_question) {
                f0s = base_f0 * decl * (0.92 + 0.10 * local) * shape_s * pitch_mult * micro;
                f0e = base_f0 * decl * (1.10 + 0.30 * local * em->f0_range) * shape_e * pitch_mult * micro;
                if (st > 0) { f0s *= 1.08; f0e *= 1.08; }
            } else {
                double mid = 0.35;
                double pct = local < mid ? local / mid : 1.0 - (local - mid) / (1 - mid);
                f0s = base_f0 * decl * (0.95 + 0.15 * pct * em->f0_range) * shape_s * pitch_mult * micro;
                f0e = base_f0 * decl * (0.95 + 0.15 * pct * em->f0_range - 0.10 * em->f0_range) * shape_e * pitch_mult * micro;
                if (st > 0) { f0s *= 1.10; f0e *= 1.05; }
            }
            }
            /* Klatt duration: context + stress + phrase-final + planner dur */
            int prev_cons = pi > 0;
            int next_cons = pi + 1 < nphones;
            int is_final = (wi == nwords - 1 && pi == nphones - 1);
            double dur = phone_duration(ph, st, is_final, prev_cons && next_cons, 0);
            dur *= p_dur;   /* planner duration multiplier */
            if (g_pitch_accent) {
                /* mora-isochronous timing (Japanese): a vowel is ~1 mora,
                 * a consonant ~0.5 mora; morae run at a steady rate. */
                int is_v = (ph->voiced && ph->turb < 0.05 && ph->velum < 0.05);
                dur = (is_v ? 0.095 : 0.048) * p_dur;
            }
            ev[nev].ph = ph; ev[nev].stress = st;
            ev[nev].dur = dur;
            ev[nev].f0_start = f0s; ev[nev].f0_end = f0e;
            /* amplitude variation (gap C28-33): stressed words louder,
             * first/last words emphasized, gentle declination, micro */
            double amp = 0.85 + 0.20 * (st > 0) + 0.10 * (wi == 0 || wi == nwords-1);
            amp *= (1.0 - 0.10 * u_phrase);            /* amplitude declination */
            amp *= (1.0 + 0.05 * sin(2 * M_PI * 2.0 * (double)wi));  /* microvar */
            if (amp > 1.2) amp = 1.2;
            if (amp < 0.5) amp = 0.5;
            ev[nev].energy = amp;
            t_abs += ev[nev].dur;
            nev++;
            phone_idx++;
            j_global++;
        }
        /* planner boundary: b0 no pause .. b4 long pause (gap C: prosody) */
        double gap = 0.06;
        if (use_planner) gap = 0.03 + 0.10 * p_boundary * p_boundary;
        else if (punct[wi] == ',' ) gap = 0.15;
        else if (punct[wi]) gap = 0.28;
        t_abs += gap;  /* word gap */
    }
    double total = t_abs + 0.3;

    /* ---------------- render ---------------- */
    int nsamp = (int)(total * SR);
    double *out = calloc((size_t)nsamp, sizeof(double));
    wb_tract_t *tract = wb_tract_new(ch->tract_n);
    wb_glottis_t *g = wb_glottis_new();
    wb_glottis_set_tenseness(g, ch->tenseness * em->tenseness / 0.65);
    wb_glottis_set_jitter(g, ch->jitter + em->jitter);
    wb_glottis_set_shimmer(g, ch->shimmer + em->shimmer);
    wb_glottis_set_vibrato(g, ch->vib_depth + em->vib_depth, em->vib_rate);
    wb_glottis_set_intensity(g, em->intensity);
    /* R017 source knobs */
    if (g_whisper) wb_glottis_set_whisper(g, 1);
    if (g_fry) { wb_glottis_set_fry(g, 1); wb_glottis_set_frequency(g, ch->f0 * 0.5); }
    if (g_breathiness > 0) wb_glottis_set_breathiness(g, g_breathiness);
    /* planner energy: scale output amplitude per word is complex mid-render;
     * we apply it as a gentle global loudness from the planner's average.
     * (Kept simple: em->intensity already carries the emotion loudness.) */

    wb_tts_t T = { tract, g, ch, out, nsamp, base_f0, base_f0 };
    T.sine_mode = g_sine_mode;

    double t0 = 0.15;
    for (int i = 0; i < nev; i++) {
        const wb_phone_t *prev = i > 0 ? ev[i-1].ph : ev[i].ph;
        const wb_phone_t *next = i + 1 < nev ? ev[i+1].ph : ev[i].ph;
        if (g_sine_mode)
            sine_render_phone(&T, t0, ev[i].dur, ev[i].ph, prev, next,
                              ev[i].f0_start, ev[i].f0_end, ev[i].energy);
        else
            tts_render(&T, t0, ev[i].dur, ev[i].ph, ev[i].stress, prev, next,
                       ev[i].f0_start, ev[i].f0_end, ev[i].energy);
        t0 += ev[i].dur;
    }

    /* loudness normalization (gap I): boost output to a speech-typical RMS
     * (~-20 dBFS) so it's comparable to real TTS, not 4x too quiet */
    {
        double rms = 0;
        for (int i = 0; i < nsamp; i++) rms += out[i] * out[i];
        rms = sqrt(rms / (nsamp > 0 ? nsamp : 1));
        double target = 0.10;   /* ~ -20 dBFS */
        double gain = rms > 1e-6 ? target / rms : 1.0;
        if (gain > 4.0) gain = 4.0;   /* limit so we don't clip silence */
        double peak = 0;
        for (int i = 0; i < nsamp; i++) {
            out[i] *= gain;
            double v = fabs(out[i]);
            if (v > peak) peak = v;
        }
        if (peak > 0.99) {  /* re-normalize if we clipped */
            for (int i = 0; i < nsamp; i++) out[i] *= 0.99 / peak;
        }
        printf("  loudness: rms %.3f -> %.3f (gain %.2fx)\n", rms, target, gain);
    }

    wb_wav_write(out_path, out, (size_t)nsamp, SR);
    char aiff_path[512];
    snprintf(aiff_path, sizeof(aiff_path), "%s", out_path);
    char *dot = strrchr(aiff_path, '.');
    if (dot) strcpy(dot, ".aiff");
    wb_aiff_write(aiff_path, out, (size_t)nsamp, SR);
    /* verify: measure F0 + quality */
    wb_f0_measure_t f0m = wb_measure_f0(out, (size_t)nsamp, SR);
    wb_quality_measure_t q = wb_measure_quality(out, (size_t)nsamp, SR);
    printf("tts: \"%s\" as %s -> %.2fs, %d phones\n", text, ch->name,
           (double)nsamp / SR, nev);
    printf("  measured f0=%.0fHz voiced=%.0f%% jitter=%.2f%% shimmer=%.2f%% hnr=%.1fdB\n",
           f0m.f0_mean, f0m.voiced_fraction * 100, q.jitter_pct, q.shimmer_pct, q.hnr_db);
    printf("  wrote %s (+ .aiff)\n", out_path);

    free(out);
    wb_glottis_free(g);
    wb_tract_free(tract);
    return 0;
}
