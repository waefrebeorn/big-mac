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
    double tempo;         /* phones/sec (5.2 = neutral) */
    double intensity;     /* glottis intensity */
    double tenseness;     /* 0 breathy .. 1 pressed */
    double jitter;
    double shimmer;
    double vib_depth;     /* trembling (fear) / warmth (happy) */
    double vib_rate;
} wb_emotion_t;

static const wb_emotion_t EMOTIONS[] = {
    { "neutral", 1.00, 1.00, 5.2, 0.80, 0.65, 0.005, 0.01, 0.002, 5.0 },
    { "happy",   1.15, 1.60, 6.0, 0.88, 0.60, 0.006, 0.02, 0.004, 6.0 },
    { "sad",     0.85, 0.55, 3.9, 0.55, 0.45, 0.010, 0.04, 0.002, 4.0 },
    { "angry",   1.20, 1.80, 6.4, 0.98, 0.92, 0.012, 0.05, 0.003, 5.5 },
    { "fearful", 1.30, 1.90, 6.2, 0.90, 0.85, 0.018, 0.06, 0.010, 7.0 },
    { "surprised",1.35, 2.00, 5.8, 0.95, 0.80, 0.008, 0.03, 0.006, 6.5 },
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
} wb_phone_t;

/* Reference formant targets: F1/F2 for vowels (Peterson & Barney-ish),
 * mapped to the tract via the retrieval table at runtime. */
static const wb_phone_t PHONES[] = {
    /* vowels — ti/td tuned to the KL tract (14 = mid, lower = front) */
    { "AA", 20.0, 1.1, 0.90, 0.0, 1, 0.00, 1.0 },  /* father  */
    { "AE", 15.5, 0.8, 0.90, 0.0, 1, 0.00, 1.0 },  /* cat     */
    { "AH", 16.5, 1.2, 0.80, 0.0, 1, 0.00, 0.8 },  /* but     */
    { "AO", 17.5, 1.0, 0.60, 0.0, 1, 0.00, 1.0 },  /* thought */
    { "AW", 16.0, 1.2, 0.50, 0.0, 1, 0.00, 1.2 },  /* cow     */
    { "AY", 15.5, 1.0, 0.85, 0.0, 1, 0.00, 1.2 },  /* hide    */
    { "EH", 15.0, 0.8, 0.90, 0.0, 1, 0.00, 0.9 },  /* bet     */
    { "ER", 13.5, 0.6, 0.80, 0.0, 1, 0.00, 1.0 },  /* bird    */
    { "EY", 14.5, 0.7, 0.85, 0.0, 1, 0.00, 1.1 },  /* bait    */
    { "IH", 14.0, 0.8, 0.90, 0.0, 1, 0.00, 0.7 },  /* bit     */
    { "IY", 13.0, 0.6, 0.85, 0.0, 1, 0.00, 1.0 },  /* beet    */
    { "OW", 18.0, 1.1, 0.55, 0.0, 1, 0.00, 1.1 },  /* boat    */
    { "OY", 16.0, 1.0, 0.60, 0.0, 1, 0.00, 1.2 },  /* boy     */
    { "UH", 17.0, 1.2, 0.50, 0.0, 1, 0.00, 0.8 },  /* book    */
    { "UW", 18.5, 1.3, 0.30, 0.0, 1, 0.00, 1.0 },  /* boot    */

    /* consonants */
    { "B", 14.0, 0.2, 0.10, 0.0, 1, 0.00, 0.4 },   /* stop, voiced */
    { "CH", 14.0, 0.3, 0.60, 0.0, 0, 0.80, 0.5 },  /* affricate */
    { "D", 15.0, 0.2, 0.80, 0.0, 1, 0.00, 0.4 },   /* stop, voiced */
    { "DH", 15.5, 0.3, 0.70, 0.0, 1, 0.60, 0.5 },  /* th-voiced */
    { "F", 14.0, 0.9, 0.15, 0.0, 0, 0.90, 0.5 },   /* fricative */
    { "G", 17.0, 0.2, 0.80, 0.0, 1, 0.00, 0.4 },   /* stop, voiced */
    { "HH", 16.0, 1.4, 0.90, 0.0, 0, 0.50, 0.4 },  /* h */
    { "JH", 14.0, 0.3, 0.60, 0.0, 1, 0.80, 0.5 },  /* affricate voiced */
    { "K", 17.0, 0.2, 0.80, 0.0, 0, 0.00, 0.4 },   /* stop */
    { "L", 14.0, 0.6, 0.80, 0.0, 1, 0.00, 0.6 },   /* liquid */
    { "M", 14.5, 0.5, 0.10, 1.0, 1, 0.00, 0.5 },   /* nasal */
    { "N", 15.0, 0.5, 0.80, 1.0, 1, 0.00, 0.5 },   /* nasal */
    { "NG", 17.0, 0.5, 0.80, 1.0, 1, 0.00, 0.5 },  /* nasal */
    { "P", 14.0, 0.2, 0.10, 0.0, 0, 0.00, 0.4 },   /* stop */
    { "R", 13.5, 0.7, 0.70, 0.0, 1, 0.00, 0.6 },   /* liquid */
    { "S", 15.0, 0.35, 0.80, 0.0, 0, 0.95, 0.5 },  /* sibilant */
    { "SH", 14.0, 0.35, 0.60, 0.0, 0, 0.95, 0.5 }, /* sibilant */
    { "T", 15.0, 0.2, 0.80, 0.0, 0, 0.00, 0.4 },   /* stop */
    { "TH", 15.5, 0.3, 0.70, 0.0, 0, 0.90, 0.5 },  /* th-unvoiced */
    { "V", 14.0, 0.9, 0.15, 0.0, 1, 0.80, 0.5 },   /* fricative */
    { "W", 19.0, 1.2, 0.35, 0.0, 1, 0.00, 0.5 },   /* glide */
    { "Y", 13.0, 0.7, 0.85, 0.0, 1, 0.00, 0.5 },   /* glide */
    { "Z", 15.0, 0.35, 0.80, 0.0, 1, 0.90, 0.5 },  /* sibilant */
    { "ZH", 14.0, 0.35, 0.60, 0.0, 1, 0.90, 0.5 }, /* sibilant */
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

/* ---------------- render ---------------- */
typedef struct {
    wb_tract_t *tract;
    wb_glottis_t *glottis;
    const wb_char_t *ch;
    double *out;
    int nsamp;
    double f0_phrase_start, f0_phrase_end;  /* intonation contour */
} wb_tts_t;

static void tts_render(wb_tts_t *T, double t0, double dur,
                       const wb_phone_t *ph, int stress,
                       double f0_start, double f0_end) {
    int s0 = (int)(t0 * SR), s1 = (int)((t0 + dur) * SR);
    if (s1 > T->nsamp) s1 = T->nsamp;
    int voiced_prev = 0;
    for (int j = s0; j < s1; j++) {
        double t = (double)(j - s0) / (s1 - s0);   /* 0..1 within phone */
        /* f0 glide within the phone (intonation) */
        double f0 = f0_start + (f0_end - f0_start) * t;
        /* articulation: blend from neutral to phone target quickly */
        wb_tract_set_rest_diameter(T->tract, ph->ti, ph->td);
        wb_tract_set_lips(T->tract, ph->lips);
        wb_tract_set_velum(T->tract, ph->velum);
        if (ph->voiced) {
            wb_glottis_set_frequency(T->glottis, f0);
            wb_glottis_set_intensity(T->glottis, 0.8);
        } else {
            wb_glottis_set_intensity(T->glottis, 0.0);  /* voiceless */
        }
        int m = j % BLOCK;
        double lam1 = (double)m / BLOCK, lam2 = (m + 0.5) / BLOCK;
        /* turbulence noise for fricatives (the "protect voiceless
         * consonants" rule: noise source, not glottis) */
        double noise = (double)((j * 2654435761u) >> 24) / 128.0 - 1.0;
        double gl = wb_glottis_run_step(T->glottis, lam1, noise * 0.3);
        double turb = noise * 0.3 * ph->turb;
        double vocal = wb_tract_run_step(T->tract, gl, turb, lam1)
                     + wb_tract_run_step(T->tract, gl, turb, lam2);
        T->out[j] += vocal * 0.125;
        if (m == BLOCK - 1) {
            wb_glottis_finish_block(T->glottis, ph->voiced, (double)BLOCK / SR);
            wb_tract_finish_block(T->tract, (double)BLOCK / SR);
        }
        voiced_prev = ph->voiced;
    }
    (void)voiced_prev;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: wb_tts \"<text>\" <character> <out.wav> [f0] [emotion]\n");
        fprintf(stderr, "  emotions: neutral happy sad angry fearful surprised\n");
        return 1;
    }
    const char *text = argv[1];
    const wb_char_t *ch = find_char(argv[2]);
    if (!ch) { fprintf(stderr, "unknown character %s\n", argv[2]); return 1; }
    const char *out_path = argv[3];
    double base_f0 = argc > 4 ? atof(argv[4]) : ch->f0;
    const wb_emotion_t *em = &EMOTIONS[0];  /* neutral */
    if (argc > 5) {
        const wb_emotion_t *e = find_emotion(argv[5]);
        if (!e) { fprintf(stderr, "unknown emotion %s\n", argv[5]); return 1; }
        em = e;
    }
    /* optional MLP planner (the WordVoice bound-token, non-neural) */
    const char *planner_path = NULL;
    if (argc > 6) planner_path = argv[6];
    wb_mlp_t planner;
    int use_planner = 0;
    if (planner_path && wb_mlp_load(planner_path, &planner) == 0) {
        use_planner = 1;
        printf("using MLP prosody planner: %s\n", planner_path);
    }
    /* emotion applies to the character's voice */
    base_f0 *= em->f0_shift;
    double tempo = em->tempo;
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
        /* lowercase */
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
    } wb_event_t;
    wb_event_t ev[2048];
    int nev = 0;

    /* phrase-level intonation: declarative falls, question rises */
    int is_question = 0;
    if (nwords > 0 && (punct[nwords-1] == '?' )) is_question = 1;
    if (nwords > 0 && punct[nwords-1] == 0 && strchr(text, '?')) is_question = 1;

    /* (tempo declared in main) */
    double t_phrase = 0;
    int phrase_nwords = 0;
    int phrase_word_count = 0;
    double phrase_dur = 0;

    /* first pass: estimate phrase duration */
    for (int wi = 0; wi < nwords; wi++) {
        const char *ph_str = lookup_word(words[wi]);
        if (!ph_str) { phrase_dur += 0.25; continue; }
        char buf[64]; snprintf(buf, sizeof(buf), "%s", ph_str);
        char *p = strtok(buf, " ");
        while (p) {
            int st;
            const wb_phone_t *ph = find_phone(p, &st);
            if (ph) phrase_dur += ph->dur / tempo;
            p = strtok(NULL, " ");
        }
        phrase_dur += 0.08;  /* word gap */
        phrase_word_count++;
    }
    if (phrase_dur < 0.5) phrase_dur = 0.5;

    /* second pass: assign f0 contour (declarative: fall; question: rise) */
    double t_abs = 0.15;  /* lead-in */
    for (int wi = 0; wi < nwords; wi++) {
        const char *ph_str = lookup_word(words[wi]);
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
                        ev[nev].dur = ph->dur / tempo;
                        ev[nev].f0_start = f0s; ev[nev].f0_end = f0e;
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

        for (int pi = 0; pi < nphones; pi++) {
            int st;
            const wb_phone_t *ph = find_phone(phones[pi], &st);
            if (!ph) continue;
            /* prosody: stress bump + phrase contour */
            double f0s = base_f0, f0e = base_f0;
            double local = (double)phone_idx / (nphones > 1 ? nphones - 1 : 1);
            /* apply the planner's tone shape (WordVoice 7-tone taxonomy),
             * scaled by the planner's pitch offset */
            double shape_s = tone_shape(p_tone, (double)phone_idx / (nphones > 0 ? nphones : 1));
            double shape_e = tone_shape(p_tone, (double)(phone_idx + 1) / (nphones > 0 ? nphones : 1));
            double pitch_mult = 1.0 + p_pitch * 0.25;   /* pitch offset -> semitones-ish */
            if (is_question) {
                f0s = base_f0 * (0.92 + 0.10 * local) * shape_s * pitch_mult;
                f0e = base_f0 * (1.10 + 0.30 * local * em->f0_range) * shape_e * pitch_mult;
                if (st > 0) { f0s *= 1.08; f0e *= 1.08; }
            } else {
                double mid = 0.35;
                double pct = local < mid ? local / mid : 1.0 - (local - mid) / (1 - mid);
                f0s = base_f0 * (0.95 + 0.15 * pct * em->f0_range) * shape_s * pitch_mult;
                f0e = base_f0 * (0.95 + 0.15 * pct * em->f0_range - 0.10 * em->f0_range) * shape_e * pitch_mult;
                if (st > 0) { f0s *= 1.10; f0e *= 1.05; }
            }
            ev[nev].ph = ph; ev[nev].stress = st;
            ev[nev].dur = ph->dur / tempo * (st > 0 ? 1.25 : 1.0) * p_dur;
            ev[nev].f0_start = f0s; ev[nev].f0_end = f0e;
            t_abs += ev[nev].dur;
            nev++;
            phone_idx++;
        }
        /* planner boundary: b0 no pause .. b4 long pause */
        double gap = 0.08;
        if (use_planner) gap = 0.04 + 0.12 * p_boundary * p_boundary;
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
    /* planner energy: scale output amplitude per word is complex mid-render;
     * we apply it as a gentle global loudness from the planner's average.
     * (Kept simple: em->intensity already carries the emotion loudness.) */

    wb_tts_t T = { tract, g, ch, out, nsamp, base_f0, base_f0 };

    double t0 = 0.15;
    for (int i = 0; i < nev; i++) {
        tts_render(&T, t0, ev[i].dur, ev[i].ph, ev[i].stress,
                   ev[i].f0_start, ev[i].f0_end);
        t0 += ev[i].dur;
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
