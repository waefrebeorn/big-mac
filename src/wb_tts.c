/* wb_tts.c — legitimate, dependency-free Text-to-Speech for Big Mac.
 *
 * v1 backend = PHONETIC: a pure-C11 formant / articulatory synthesizer
 * (Klatt-style). text -> words -> phonemes (dictionary + letter-to-sound) ->
 * per-phoneme glottal/aspiration source through 3 formant resonators ->
 * 22.05 kHz mono float PCM. No model file, no third-party runtime. Runs
 * instantly on a dual-core iMac; trivially fast on better hardware.
 *
 * The NEURAL (ggml VITS) backend is an architected slot (see docs/R019): it
 * reuses the same wb_tts_speak() API once a .bin/.gguf VITS model is vendored.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include "wbus/wbus_tts.h"
#include "wbus/wb_internal.h"   /* wb_wav_write_pcm16 */

#define WB_TTS_SR 22050

/* ---- phoneme model ---------------------------------------------------- */
typedef enum { P_VOWEL, P_VOICED, P_UNVOICED, P_PAUSE } pclass;
typedef struct {
    const char *sym;     /* ARPABET-ish symbol */
    pclass      cls;
    float f1, f2, f3;     /* formant center freqs (Hz); 0 = n/a */
    float bw1, bw2, bw3;  /* formant bandwidths (Hz) */
    float dur;            /* base duration (s) at rate 1.0 */
    float gain;           /* relative amplitude */
} phoneme;

/* Vowels: F1/F2/F3 tuned to rough cardinal values. Consonants: approximate
 * constriction formants + whether they are noise-excited. */
static const phoneme PHON[] = {
    /* vowels */
    {"AA", P_VOWEL, 730,1090,2440, 80,90,120, 0.090, 1.00},
    {"AE", P_VOWEL, 660,1720,2410, 80,90,120, 0.080, 1.00},
    {"AH", P_VOWEL, 660,1310,2330, 80,90,120, 0.085, 1.00},
    {"AO", P_VOWEL, 570, 840,2410, 80,90,120, 0.090, 1.00},
    {"AW", P_VOWEL, 590,1450,2550, 90,90,120, 0.095, 0.95},
    {"AY", P_VOWEL, 670,1660,2330, 80,90,120, 0.095, 0.95},
    {"EH", P_VOWEL, 530,1840,2480, 80,90,120, 0.075, 1.00},
    {"ER", P_VOWEL, 490,1350,1690, 80,90,120, 0.080, 0.95},
    {"EY", P_VOWEL, 550,1700,2480, 80,90,120, 0.095, 0.95},
    {"IH", P_VOWEL, 400,1900,2550, 80,90,120, 0.065, 1.00},
    {"IY", P_VOWEL, 310,2320,3000, 80,90,120, 0.075, 1.00},
    {"OW", P_VOWEL, 570, 840,2410, 80,90,120, 0.095, 0.95},
    {"OY", P_VOWEL, 500,1450,2500, 90,90,120, 0.095, 0.95},
    {"UH", P_VOWEL, 440,1020,2240, 80,90,120, 0.070, 1.00},
    {"UW", P_VOWEL, 310, 870,2250, 80,90,120, 0.075, 1.00},
    /* voiced consonants */
    {"B",  P_VOICED, 400, 800, 900, 100,100,150, 0.060, 0.55},
    {"D",  P_VOICED, 300,1700,2600, 120,120,200, 0.060, 0.55},
    {"G",  P_VOICED, 300,2600,3200, 120,150,200, 0.070, 0.55},
    {"JH", P_VOICED, 400,1100,2600, 120,120,200, 0.070, 0.55},
    {"L",  P_VOICED, 360,1300,2800, 100,100,150, 0.070, 0.70},
    {"M",  P_VOICED, 250,1000,2200, 100,100,150, 0.070, 0.65},
    {"N",  P_VOICED, 250,1700,2600, 100,100,150, 0.070, 0.65},
    {"NG", P_VOICED, 250,2000,2900, 100,120,150, 0.075, 0.60},
    {"R",  P_VOICED, 490,1350,1690, 100,100,150, 0.070, 0.70},
    {"V",  P_VOICED, 250,1100,2400, 100,120,200, 0.060, 0.55},
    {"W",  P_VOICED, 300, 800,2300, 100,100,150, 0.060, 0.60},
    {"Y",  P_VOICED, 300,1700,2300, 100,100,150, 0.060, 0.60},
    {"Z",  P_VOICED, 250,1400,2800, 100,120,200, 0.065, 0.55},
    {"ZH", P_VOICED, 400,1100,2600, 120,120,200, 0.070, 0.55},
    {"DH", P_VOICED, 250,1200,2400, 100,120,200, 0.060, 0.55},
    /* unvoiced consonants */
    {"CH", P_UNVOICED, 400,1100,2600, 140,140,220, 0.070, 0.55},
    {"F",  P_UNVOICED, 250,1100,2400, 120,140,200, 0.060, 0.55},
    {"K",  P_UNVOICED, 300,2600,3200, 140,160,220, 0.060, 0.55},
    {"P",  P_UNVOICED, 400, 800, 900, 140,140,200, 0.050, 0.50},
    {"S",  P_UNVOICED, 250,1400,2800, 120,140,200, 0.065, 0.55},
    {"SH", P_UNVOICED, 400,1100,2600, 140,140,220, 0.070, 0.55},
    {"T",  P_UNVOICED, 300,1700,2600, 140,140,220, 0.050, 0.50},
    {"TH", P_UNVOICED, 250,1200,2400, 120,140,200, 0.060, 0.55},
    {"HH", P_UNVOICED, 600,1500,2500, 120,140,200, 0.065, 0.55},
    /* pause (inter-word / punctuation) */
    {"_",   P_PAUSE, 0,0,0, 0,0,0, 0.120, 0.0},
    {NULL, P_PAUSE, 0,0,0, 0,0,0, 0, 0}
};

static const phoneme *ph_find(const char *sym) {
    for (int i = 0; PHON[i].sym; i++)
        if (strcmp(PHON[i].sym, sym) == 0) return &PHON[i];
    return NULL;
}

/* ---- mini pronunciation dictionary (covers common narration words) ----- */
typedef struct { const char *w; const char *ph; } entry;
static const entry DICT[] = {
    {"hello",   "HH EH L OW"},
    {"world",   "W ER L D"},
    {"the",     "DH AH"},
    {"a",       "AH"},
    {"an",      "AE N"},
    {"and",     "AE N D"},
    {"big",     "B IH G"},
    {"mac",     "M AE K"},
    {"editor",  "EH D IH T ER"},
    {"podcast", "P AA D K AE S T"},
    {"episode", "EH P IH S OW D"},
    {"recursive","R IH K ER S IH V"},
    {"edit",    "EH D IH T"},
    {"software","S AA F T W EH R"},
    {"that",    "DH AE T"},
    {"improves","IH M P R UW V Z"},
    {"itself",  "IH T S EH L F"},
    {"about",   "AH B AW T"},
    {"story",   "S T AO R IY"},
    {"comes",   "K AH M Z"},
    {"from",    "F R AH M"},
    {"digital", "D IH J IH T AH L"},
    {"audio",   "AO D IY OW"},
    {"video",   "V IH D IY OW"},
    {"workstation","W ER K S T EY SH AH N"},
    {"built",   "B IH L T"},
    {"pure",    "P Y U R"},
    {"with",    "W IH DH"},
    {"zero",    "Z IH R OW"},
    {"third",   "TH ER D"},
    {"party",   "P AA R T IY"},
    {"dependency","D IH P EH N D AH N S IY"},
    {"constraint","K AH N S T R EY N T"},
    {"became",  "B IH K EY M"},
    {"superpower","S UW P ER P AW ER"},
    {"today",   "T AH D EY"},
    {"loop",    "L UW P"},
    {"best",    "B EH S T"},
    {"one",     "W AH N"},
    {"step",    "S T EH P"},
    {"time",    "T AY M"},
    {"for",     "F AO R"},
    {"this",    "DH IH S"},
    {"is",      "IH Z"},
    {"to",      "T UW"},
    {"of",      "AH V"},
    {"you",     "Y UW"},
    {"we",      "W IY"},
    {"our",     "AW R"},
    {"are",     "AA R"},
    {"be",      "B IY"},
    {"can",     "K AE N"},
    {"it",      "IH T"},
    {"in",      "IH N"},
    {"on",      "AA N"},
    {"at",      "AE T"},
    {"no",      "N OW"},
    {"not",     "N AA T"},
    {"has",     "H AE Z"},
    {"was",     "W AA Z"},
    {"by",      "B AY"},
    {"or",      "AO R"},
    {"as",      "AE Z"},
    {"your",    "Y AO R"},
    {"all",     "AO L"},
    {"new",     "N UW"},
    {"more",    "M AO R"},
    {"most",    "M OW S T"},
    {"what",    "W AH T"},
    {"who",     "HH UW"},
    {"why",     "W AY"},
    {"how",     "HH AW"},
    {"when",    "W EH N"},
    {"where",   "W EH R"},
    {"which",   "W IH CH"},
    {"will",    "W IH L"},
    {"would",   "W UH D"},
    {"could",   "K UH D"},
    {"should",  "SH UH D"},
    {"there",   "DH EH R"},
    {"their",   "DH EH R"},
    {"they",    "DH EY"},
    {"said",    "S EH D"},
    {"make",    "M EY K"},
    {"made",    "M EY D"},
    {"use",     "Y UW Z"},
    {"used",    "Y UW S T"},
    {"using",   "Y UW Z IH N G"},
    {"voice",   "V OY S"},
    {"sound",   "S AW N D"},
    {"right",   "R AY T"},
    {"engine",  "EH N J AH N"},
    {"system",  "S IH S T AH M"},
    {"model",   "M AA D AH L"},
    {"data",    "D AE T AH"},
    {"open",    "OW P AH N"},
    {"source",  "S AO R S"},
    {"code",    "K OW D"},
    {"free",    "F R IY"},
    {"real",    "R IY L"},
    {"first",   "F ER S T"},
    {"next",    "N EH K S T"},
    {"last",    "L AE S T"},
    {"only",    "OW N L IY"},
    {"out",     "AW T"},
    {"up",      "AH P"},
    {"down",    "D AW N"},
    {"off",     "AO F"},
    {"over",    "OW V ER"},
    {"now",     "N AW"},
    {"then",    "DH EH N"},
    {"than",    "DH AE N"},
    {"them",    "DH EH M"},
    {"these",   "DH IY Z"},
    {"those",   "DH OW Z"},
    {"two",     "T UW"},
    {"three",   "TH R IY"},
    {"four",    "F AO R"},
    {"five",    "F AY V"},
    {"six",     "S IH K S"},
    {"seven",   "S EH V AH N"},
    {"eight",   "EY T"},
    {"nine",    "N AY N"},
    {"ten",     "T EH N"},
    {"zero",    "Z IH R OW"},
    {"human",   "HH Y UW M AH N"},
    {"machine", "M AH SH IY N"},
    {"language","L AE N G G W IH J"},
    {"learning","L ER N IH N G"},
    {"research","R IY S ER CH"},
    {"agent",   "EY J AH N T"},
    {"host",    "HH OW S T"},
    {"show",    "SH OW"},
    {"stand",   "S T AE N D"},
    {"small",   "S M AO L"},
    {"large",   "L AA R J"},
    {"great",   "G R EY T"},
    {"good",    "G UH D"},
    {"bad",     "B AE D"},
    {"high",    "H AY"},
    {"low",     "L OW"},
    {"fast",    "F AE S T"},
    {"slow",    "S L OW"},
    {"run",     "R AH N"},
    {"runs",    "R AH N Z"},
    {"running", "R AH N IH N G"},
    {"test",    "T EH S T"},
    {"tests",   "T EH S T S"},
    {"build",   "B IH L D"},
    {"builds",  "B IH L D Z"},
    {"builders","B IH L D ER Z"},
    {NULL, NULL}
};

/* ---- letter-to-sound fallback (very small, English-ish) ---------------- */
/* Produce a rough phoneme string for an unknown lowercase word. Returns a
 * malloc'd string (space-separated ARPABET) or NULL. */
static char *lts_phonemes(const char *w) {
    size_t n = strlen(w);
    if (n == 0) return NULL;
    char *out = malloc(n * 4 + 8);
    if (!out) return NULL;
    out[0] = 0;
    /* naive vowel map + default consonant = its own letter uppercased */
    for (size_t i = 0; i < n; i++) {
        char c = w[i];
        const char *ph = NULL;
        switch (c) {
            case 'a': ph = (i+1<n && w[i+1]=='i') ? "AY" : "AE"; break;
            case 'e': ph = "EH"; break;
            case 'i': ph = (i+1<n && (w[i+1]=='o'||w[i+1]=='e')) ? "IY" : "IH"; break;
            case 'o': ph = "OW"; break;
            case 'u': ph = "UW"; break;
            case 'y': ph = "IY"; break;
            default: {
                char buf[2] = { (char)toupper(c), 0 };
                ph = buf;
            }
        }
        strcat(out, ph);
        if (i + 1 < n) strcat(out, " ");
    }
    return out;
}

/* ---- voice / engine state --------------------------------------------- */
struct wb_tts {
    wb_tts_backend backend;
    int   sr;
    float pitch;     /* base F0 in Hz */
    float rate;      /* 1.0 normal */
    int   voice;     /* 0 = default */
};

wb_tts *wb_tts_create(const char *model_path) {
    wb_tts *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->sr = WB_TTS_SR;
    t->pitch = 120.0f;
    t->rate = 1.0f;
    t->voice = 0;
    /* Neural backend is deferred; if a model path is given but not loadable
     * we still fall back to phonetic (honest, always-works path). */
    t->backend = WB_TTS_BACKEND_PHONETIC;
    (void)model_path;
    return t;
}

void wb_tts_destroy(wb_tts *t) { free(t); }

wb_tts_backend wb_tts_get_backend(wb_tts *t) { return t ? t->backend : WB_TTS_BACKEND_NONE; }
int  wb_tts_sample_rate(wb_tts *t) { return t ? t->sr : 0; }
void wb_tts_set_pitch(wb_tts *t, float hz) { if (t) t->pitch = hz; }
void wb_tts_set_rate(wb_tts *t, float r) { if (t) t->rate = r < 0.2f ? 0.2f : r; }
void wb_tts_set_voice(wb_tts *t, int i) { if (t) t->voice = i < 0 ? 0 : i; }
int  wb_tts_voice_count(wb_tts *t) { (void)t; return 2; } /* 0 default, 1 low */
const char *wb_tts_voice_name(wb_tts *t, int i) {
    (void)t;
    return i == 1 ? "low" : "default";
}

/* ---- formant resonator (2-pole bandpass) ------------------------------ */
typedef struct { double a1, a2, g, y1, y2, x1, x2; } reson;

static void reson_init(reson *r, float f, float bw, int sr) {
    double R = exp(-M_PI * bw / sr);
    double th = 2.0 * M_PI * f / sr;
    r->a1 = -2.0 * R * cos(th);
    r->a2 = R * R;
    r->g  = (1.0 - R * R) * 0.5;   /* peak-ish normalization */
    r->y1 = r->y2 = r->x1 = r->x2 = 0;
}
static double reson_run(reson *r, double x) {
    double y = r->g * (x - r->x2) - r->a1 * r->y1 - r->a2 * r->y2;
    r->x2 = r->x1; r->x1 = x;
    r->y2 = r->y1; r->y1 = y;
    return y;
}

/* Simple LCG for deterministic noise (no libc rand state surprises). */
static uint32_t lcgs = 0x9e3779b9u;
static double tt_noise_state(uint32_t *st) {
    *st = *st * 1664525u + 1013904223u;
    return ((int32_t)(*st >> 8) / 2147483648.0);
}
#define tt_noise() tt_noise_state(&lstate)

/* Synthesize one phoneme into buf (already-allocated, length n samples).
 * f0 = current fundamental. Formant freqs are interpolated by interp 0..1. */
static void synth_phoneme(const phoneme *p, float *buf, int n, int sr,
                          float f0, float amp, uint32_t seed) {
    uint32_t lstate = seed ? seed : 0x9e3779b9u;
    reson r1, r2, r3;
    int voiced_src = (p->cls == P_VOWEL || p->cls == P_VOICED);
    if (p->f1 > 0) { reson_init(&r1, p->f1, p->bw1, sr); reson_init(&r2, p->f2, p->bw2, sr); reson_init(&r3, p->f3, p->bw3, sr); }
    double phase = 0;
    double f0s = f0 / sr;
    for (int i = 0; i < n; i++) {
        double src;
        if (voiced_src) {
            /* glottal impulse train (Rosenberg-ish shaped pulse) */
            phase += f0s;
            if (phase >= 1.0) phase -= 1.0;
            /* simple asymmetric glottal pulse */
            double g = (phase < 0.4) ? (phase/0.4) : (1.0 - (phase-0.4)/0.6);
            if (g < 0) g = 0;
            double aspir = 0.04 * tt_noise();
            src = g * 0.9 + aspir;
        } else {
            /* unvoiced: broadband noise through constriction */
            src = tt_noise() * 0.8;
        }
        double out;
        if (p->f1 > 0) {
            double a = reson_run(&r1, src);
            double b = reson_run(&r2, src);
            double c = reson_run(&r3, src);
            out = (a + 0.6*b + 0.3*c) * 0.5;
        } else {
            out = src * 0.4;
        }
        buf[i] += (float)(out * amp * p->gain);
    }
}

/* ---- top-level speak --------------------------------------------------- */
int wb_tts_speak(wb_tts *t, const char *text,
                 float **out_pcm, uint32_t *out_frames, int *out_sr) {
    if (!t || !text || !out_pcm) return -1;
    *out_pcm = NULL; *out_frames = 0; if (out_sr) *out_sr = t->sr;

    /* deterministic per-call noise seed from the text + voice/pitch, so two
     * speak() calls with identical inputs produce bit-identical output. */
    uint32_t call_seed = 0x9e3779b9u;
    for (const char *p = text; *p; p++) call_seed = call_seed * 31u + (uint32_t)(unsigned char)*p;
    call_seed ^= (uint32_t)(t->voice * 2654435761u) ^ (uint32_t)(t->pitch * 1000.0f);

    /* tokenize words + punctuation into a phoneme plan (with pauses) */
    /* first pass: count total phonemes + word gaps to size output */
    size_t tn = strlen(text);
    /* generous upper bound on samples */
    size_t cap = tn * (size_t)(0.5 * t->sr) + 4096;
    float *pcm = calloc(cap, sizeof(float));
    if (!pcm) return -1;
    uint32_t nframes = 0;

    float base_f0 = t->pitch * (t->voice == 1 ? 0.8f : 1.0f);

    char *low = malloc(tn + 1);
    if (!low) { free(pcm); return -1; }
    for (size_t i = 0; i <= tn; i++) low[i] = (char)tolower(text[i]);

    size_t i = 0;
    int word_idx = 0;
    while (i < tn) {
        /* skip spaces */
        if (isspace((unsigned char)low[i])) { i++; continue; }
        /* punctuation -> pause + optional pitch move */
        if (ispunct((unsigned char)low[i])) {
            int np = (int)(0.18 * t->sr / t->rate);
            nframes += np;
            i++;
            continue;
        }
        /* read a word */
        size_t j = i;
        while (j < tn && !isspace((unsigned char)low[j]) && !ispunct((unsigned char)low[j])) j++;
        size_t wlen = j - i;
        char wbuf[64];
        if (wlen >= sizeof(wbuf)) wlen = sizeof(wbuf) - 1;
        memcpy(wbuf, low + i, wlen); wbuf[wlen] = 0;

        /* phonemes for this word */
        char *phstr = NULL;
        for (int d = 0; DICT[d].w; d++) {
            if (strcmp(DICT[d].w, wbuf) == 0) { phstr = strdup(DICT[d].ph); break; }
        }
        if (!phstr) phstr = lts_phonemes(wbuf);

        /* tokenize phstr by spaces */
        char *save = NULL;
        char *tok = strtok_r(phstr, " ", &save);
        int nph = 0;
        const phoneme *plist[64];
        while (tok && nph < 64) {
            const phoneme *p = ph_find(tok);
            if (p) plist[nph++] = p;
            tok = strtok_r(NULL, " ", &save);
        }
        free(phstr);

        /* synthesize each phoneme with simple prosody */
        for (int p = 0; p < nph; p++) {
            const phoneme *p2 = plist[p];
            int np = (int)(p2->dur * t->sr / t->rate);
            if (np < 1) np = 1;
            if (nframes + np > cap) np = (int)(cap - nframes);
            if (np <= 0) break;
            /* pitch contour: slight rise mid-word, fall at end; tiny per-word jitter */
            float f0 = base_f0
                     + 8.0f * sinf((float)word_idx * 1.3f + (float)p * 0.5f)
                     + (p == nph-1 ? -10.0f : 0.0f);
            if (f0 < 70) f0 = 70; if (f0 > 300) f0 = 300;
            float amp = (p2->cls == P_UNVOICED) ? 0.8f : 1.0f;
            synth_phoneme(p2, pcm + nframes, np, t->sr, f0, amp,
                          call_seed ^ (uint32_t)(word_idx * 2654435761u) ^ (uint32_t)(p * 40503u));
            nframes += (uint32_t)np;
        }
        /* inter-word gap */
        int gap = (int)(0.06 * t->sr / t->rate);
        nframes += gap;
        word_idx++;
        i = j;
    }
    free(low);

    /* gentle output soft-clip to keep peaks sane */
    for (uint32_t k = 0; k < nframes; k++) {
        float v = pcm[k];
        if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
        pcm[k] = v;
    }

    *out_pcm = pcm;
    *out_frames = nframes;
    return 0;
}

int wb_tts_speak_wav(wb_tts *t, const char *text, const char *wav_path) {
    float *pcm = NULL; uint32_t n = 0; int sr = 0;
    if (wb_tts_speak(t, text, &pcm, &n, &sr) != 0) return -1;
    int rc = wb_wav_write_pcm16(wav_path, pcm, n, 1, sr);
    free(pcm);
    return rc == 0 ? 0 : -1;
}
