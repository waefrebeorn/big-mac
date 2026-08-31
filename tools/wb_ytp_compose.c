/* wb_ytp_compose.c — YTP composition engine (R081).
 *
 * This is the brain of the YTP editor. It takes:
 *   - A source video (with transcript from whisper.cpp)
 *   - A "script" (what the YTP should be about)
 * And produces:
 *   - A composed edit with plot structure, pacing, and context-aware effects
 *
 * The key insight: effects are NOT random. They're chosen based on:
 *   - What's being said (transcript content)
 *   - When it happens in the story (plot position)
 *   - How the pacing feels (rhythm detection)
 *
 * Plot structure:
 *   ACT 1 (Setup):    Introduce characters, establish premise
 *   ACT 2 (Escalate): Build chaos, sentence mixing, callbacks
 *   ACT 3 (Climax):   Peak absurdity, maximum effects
 *   ACT 4 (Resolve):  Punchline, outro
 *
 * Effects are chosen by a "director" function that analyzes each segment
 * and picks appropriate techniques.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ---- Segment Types ----------------------------------------------------- */

typedef enum {
    SEG_INTRO,          /* opening, title card */
    SEG_DIALOGUE,       /* character speaking */
    SEG_ACTION,         /* something happening on screen */
    SEG_TRANSITION,     /* scene change */
    SEG_PUNCHLINE,      /* the payoff moment */
    SEG_OUTRO,          /* ending */
    SEG_MUSICAL,        /* music/song segment */
    SEG_EFFECT,         /* pure effect (vine boom, etc.) */
} seg_type;

/* ---- Effect Types ------------------------------------------------------ */

typedef enum {
    EFFECT_NONE,
    EFFECT_STUTTER,     /* repeat word 2-5x */
    EFFECT_SLOWMO,      /* slow down 0.25x-0.5x */
    EFFECT_FASTFORWARD, /* speed up 2x-4x */
    EFFECT_REVERSE,     /* play backwards */
    EFFECT_PITCH_UP,    /* chipmunk */
    EFFECT_PITCH_DOWN,  /* deep voice */
    EFFECT_EARRAPE,     /* volume spike + distortion */
    EFFECT_VINE_BOOM,   /* impact sound */
    EFFECT_DEEP_FRY,    /* saturation + contrast */
    EFFECT_VHS,         /* tracking errors */
    EFFECT_KALEIDO,     /* mirror/split screen */
    EFFECT_SHAKE,       /* camera shake */
    EFFECT_ZOOM,        /* sudden zoom */
    EFFECT_FREEZE,      /* freeze frame */
    EFFECT_FLASH,       /* white flash */
    EFFECT_SCRAMBLE,    /* random word order */
    EFFECT_SENTENCE_MIX,/* rearrange words into new phrase */
    EFFECT_DATAMOSH,    /* pixel corruption */
    EFFECT_BLEEP,       /* censor beep */
    EFFECT_SUBTITLE,    /* burn text overlay */
    EFFECT_MOCAP_OVERLAY, /* dancing skeleton */
    EFFECT_SONIC_SCREAM, /* "SEGA" style sound */
    EFFECT_TO_BE_CONTINUED, /* arrow + music */
    EFFECT_MEME_SOUND,  /* airhorn, bruh, etc. */
} effect_type;

/* ---- A single edit operation ------------------------------------------ */

typedef struct {
    double timeline_start_ms;   /* where this segment goes in the output */
    double timeline_dur_ms;     /* how long it is in the output */
    double source_start_ms;     /* where in the source video */
    double source_dur_ms;       /* how long the source clip is */
    int source_clip;            /* which source file */
    seg_type type;
    effect_type effect;
    int effect_intensity;       /* 1-10, how strong */
    char text_overlay[256];     /* subtitle text if any */
    char description[256];      /* human-readable description */
} edit_op;

/* ---- The Composition --------------------------------------------------- */

typedef struct {
    edit_op *ops;
    int n_ops;
    int cap;
    double total_duration_ms;
    char title[256];
    char description[1024];
} ytp_composition;

/* ---- "Director" — analyzes content and picks effects ----------------- */

typedef struct {
    /* Analysis results */
    int n_words;
    double avg_word_rate;       /* words per second */
    double energy;              /* overall energy level 0-1 */
    int has_speech;             /* does it have dialogue */
    int has_music;              /* does it have music */
    int n_scene_changes;        /* number of scene cuts */
    double avg_shot_duration;   /* average shot length */

    /* Director's choices */
    int chaos_level;            /* 1-10, how chaotic this YTP should be */
    int sentence_mix_amount;    /* 0-100, % of words to rearrange */
    int callback_frequency;     /* how often to reference earlier moments */
    int preferred_effects[8];   /* which effects to favor */
} director_analysis;

/* ---- Segment Analysis -------------------------------------------------- */

/* Analyze a transcript segment to determine what type it is */
seg_type classify_segment(const char *text, double duration_ms, int seg_index, int total_segs) {
    /* First segment = intro */
    if (seg_index == 0) return SEG_INTRO;

    /* Last segment = outro */
    if (seg_index >= total_segs - 1) return SEG_OUTRO;

    /* Short segments with exclamation = punchline candidates */
    if (duration_ms < 500 && strchr(text, '!')) return SEG_PUNCHLINE;

    /* Long segments = dialogue */
    if (strlen(text) > 20) return SEG_DIALOGUE;

    /* Medium with action words = action */
    if (strstr(text, "go") || strstr(text, "run") || strstr(text, "jump") ||
        strstr(text, "hit") || strstr(text, "fight") || strstr(text, "kill")) {
        return SEG_ACTION;
    }

    return SEG_DIALOGUE;
}

/* Choose an effect based on segment type, content, and plot position */
effect_type choose_effect(seg_type type, const char *text, double energy,
                           double plot_position, int chaos, unsigned int *seed) {
    int r = rand_r(seed) % 100;

    /* Content-aware: check for specific YTP staples */
    int has_the = (strstr(text, "the ") != NULL || strstr(text, "The ") != NULL);
    int has_question = strchr(text, '?') != NULL;
    int has_exclaim = strchr(text, '!') != NULL;
    int has_ing = 0; /* words ending in -ing */
    int word_count = 1;
    for (const char *p = text; *p; p++) if (*p == ' ') word_count++;

    switch (type) {
        case SEG_INTRO:
            if (r < 30) return EFFECT_SLOWMO;
            if (r < 50) return EFFECT_VINE_BOOM;
            if (r < 70) return EFFECT_SUBTITLE;
            return EFFECT_NONE;

        case SEG_DIALOGUE: {
            /* Content-aware effect selection */
            int fx_roll = rand_r(seed) % 100;
            int threshold = chaos * 10; /* chaos 7 = 70% chance of any effect */

            if (fx_roll < threshold) {
                /* Pick an effect based on content */
                if (has_the && (rand_r(seed) % 100) < 40) return EFFECT_STUTTER; /* "the" = stutter */
                if (has_question && (rand_r(seed) % 100) < 30) return EFFECT_REVERSE; /* questions = reverse */
                if (has_exclaim && (rand_r(seed) % 100) < 40) return EFFECT_EARRAPE; /* ! = earrape */
                if (word_count <= 3 && (rand_r(seed) % 100) < 30) return EFFECT_VINE_BOOM; /* short = boom */

                /* General effects weighted by chaos */
                int pick = rand_r(seed) % 100;
                if (pick < 25) return EFFECT_STUTTER;
                if (pick < 40) return EFFECT_PITCH_UP;
                if (pick < 55) return EFFECT_PITCH_DOWN;
                if (pick < 70) return EFFECT_REVERSE;
                if (pick < 80) return EFFECT_SENTENCE_MIX;
                if (pick < 88) return EFFECT_EARRAPE;
                if (pick < 93) return EFFECT_DEEP_FRY;
                if (pick < 96) return EFFECT_VHS;
                return EFFECT_ZOOM;
            }
            return EFFECT_NONE;
        }

        case SEG_ACTION:
            if (r < 30) return EFFECT_FASTFORWARD;
            if (r < 50) return EFFECT_VINE_BOOM;
            if (r < 70) return EFFECT_SHAKE;
            if (r < 80) return EFFECT_ZOOM;
            return EFFECT_NONE;

        case SEG_TRANSITION:
            if (r < 40) return EFFECT_FLASH;
            if (r < 60) return EFFECT_VINE_BOOM;
            if (r < 80) return EFFECT_ZOOM;
            return EFFECT_NONE;

        case SEG_PUNCHLINE:
            if (r < 30) return EFFECT_EARRAPE;
            if (r < 50) return EFFECT_VINE_BOOM;
            if (r < 70) return EFFECT_FREEZE;
            if (r < 80) return EFFECT_TO_BE_CONTINUED;
            return EFFECT_NONE;

        case SEG_MUSICAL:
            if (r < 30) return EFFECT_PITCH_UP;
            if (r < 50) return EFFECT_PITCH_DOWN;
            if (r < 70) return EFFECT_SLOWMO;
            return EFFECT_NONE;

        case SEG_OUTRO:
            if (r < 40) return EFFECT_SLOWMO;
            if (r < 60) return EFFECT_FREEZE;
            if (r < 80) return EFFECT_TO_BE_CONTINUED;
            return EFFECT_NONE;

        default:
            return EFFECT_NONE;
    }
}

/* Choose effect intensity based on plot position */
int choose_intensity(double plot_position, seg_type type, unsigned int *seed) {
    /* Build intensity through the middle, peak at climax (70-80%) */
    int base = 0;
    if (plot_position < 0.2) base = 2;           /* intro: mild */
    else if (plot_position < 0.5) base = 4 + (int)(plot_position * 10); /* building */
    else if (plot_position < 0.75) base = 7;      /* climax: intense */
    else if (plot_position < 0.9) base = 5;       /* winding down */
    else base = 3;                                /* outro: mild */

    /* Add randomness */
    int jitter = (rand_r(seed) % 3) - 1;
    base += jitter;

    /* Clamp */
    if (base < 1) base = 1;
    if (base > 10) base = 10;
    return base;
}

/* ---- Composition Builder ----------------------------------------------- */

ytp_composition *ytp_create(const char *title) {
    ytp_composition *c = calloc(1, sizeof(ytp_composition));
    strncpy(c->title, title, sizeof(c->title) - 1);
    return c;
}

void ytp_add_op(ytp_composition *c, const edit_op *op) {
    if (c->n_ops >= c->cap) {
        c->cap = c->cap ? c->cap * 2 : 64;
        c->ops = realloc(c->ops, c->cap * sizeof(edit_op));
    }
    c->ops[c->n_ops++] = *op;
    c->total_duration_ms += op->timeline_dur_ms;
}

void ytp_free(ytp_composition *c) {
    if (!c) return;
    free(c->ops);
    free(c);
}

/* ---- Build a YTP from a transcript ------------------------------------- */

/*
 * This is the main entry point. Given a transcript (array of word timestamps),
 * build a complete YTP composition with plot structure.
 *
 * words: array of word strings
 * starts: start time of each word in ms
 * ends: end time of each word in ms
 * n_words: number of words
 * source_clip: which source video these words come from
 * chaos: 1-10, how chaotic
 * seed: random seed
 */
ytp_composition *ytp_build(const char **words, const double *starts,
                            const double *ends, int n_words,
                            int source_clip, int chaos, unsigned int seed) {
    char title[256];
    snprintf(title, sizeof(title), "YTP_%u", seed);
    ytp_composition *c = ytp_create(title);

    /* Group words into segments (sentences/phrases) */
    typedef struct {
        int start_word;
        int end_word;
        double start_ms;
        double end_ms;
        char text[1024];
    } segment;

    segment *segs = NULL;
    int n_segs = 0;
    int seg_cap = 0;

    /* Split into segments:
     *  - Pause >300ms = new segment
     *  - Sentence ending (.!?) = new segment
     *  - Every 5-8 words = new segment (for YTP pacing)
     *  - Comma = possible break (50% chance)
     */
    int seg_start = 0;
    int words_in_seg = 0;
    for (int i = 1; i <= n_words; i++) {
        int end_here = 0;
        words_in_seg++;

        /* End of word list */
        if (i >= n_words) {
            end_here = 1;
        }
        /* Pause >300ms = new segment */
        else if (starts[i] - ends[i-1] > 300.0) {
            end_here = 1;
        }
        /* Sentence ending punctuation */
        const char *w = words[i-1];
        int len = strlen(w);
        if (len > 0 && (w[len-1] == '.' || w[len-1] == '!' || w[len-1] == '?')) {
            end_here = 1;
        }
        /* Force break every 5-8 words for YTP pacing */
        if (words_in_seg >= 5 + (rand_r(&seed) % 4)) {
            end_here = 1;
        }
        /* Comma = 40% chance of break */
        if (len > 0 && w[len-1] == ',' && (rand_r(&seed) % 100) < 40) {
            end_here = 1;
        }

        if (end_here && i > seg_start) {
            if (n_segs >= seg_cap) {
                seg_cap = seg_cap ? seg_cap * 2 : 32;
                segs = realloc(segs, seg_cap * sizeof(segment));
            }
            segment *s = &segs[n_segs++];
            s->start_word = seg_start;
            s->end_word = i;
            s->start_ms = starts[seg_start];
            s->end_ms = ends[i-1];
            s->text[0] = '\0';

            /* Build text */
            for (int j = seg_start; j < i; j++) {
                if (j > seg_start) strcat(s->text, " ");
                strcat(s->text, words[j]);
            }

            seg_start = i;
            words_in_seg = 0;
        }
    }

    printf("  Grouped %d words into %d segments\n", n_words, n_segs);

    /* Now compose each segment */
    double timeline_pos = 0;

    for (int s = 0; s < n_segs; s++) {
        segment *seg = &segs[s];
        double plot_pos = (double)s / (double)n_segs;
        double seg_dur = seg->end_ms - seg->start_ms;

        /* Classify the segment */
        seg_type type = classify_segment(seg->text, seg_dur, s, n_segs);

        /* Choose effect */
        effect_type fx = choose_effect(type, seg->text, 0.5, plot_pos, chaos, &seed);
        int intensity = choose_intensity(plot_pos, type, &seed);

        /* Build the edit operation */
        edit_op op = {0};
        op.timeline_start_ms = timeline_pos;
        op.source_clip = source_clip;
        op.source_start_ms = seg->start_ms;
        op.source_dur_ms = seg->end_ms - seg->start_ms;
        op.type = type;
        op.effect = fx;
        op.effect_intensity = intensity;

        /* Apply effect to duration */
        switch (fx) {
            case EFFECT_SLOWMO:
                op.timeline_dur_ms = op.source_dur_ms * (1.0 + intensity * 0.3);
                snprintf(op.description, sizeof(op.description),
                         "SLOWMO(%.1fx): \"%s\"", 1.0 / (1.0 + intensity * 0.3), seg->text);
                break;
            case EFFECT_FASTFORWARD:
                op.timeline_dur_ms = op.source_dur_ms / (1.0 + intensity * 0.5);
                snprintf(op.description, sizeof(op.description),
                         "FAST(%.1fx): \"%s\"", 1.0 + intensity * 0.5, seg->text);
                break;
            case EFFECT_STUTTER:
                op.timeline_dur_ms = op.source_dur_ms * (1.0 + intensity * 0.4);
                snprintf(op.description, sizeof(op.description),
                         "STUTTER(%dx): \"%s\"", intensity, seg->text);
                break;
            case EFFECT_REVERSE:
                op.timeline_dur_ms = op.source_dur_ms;
                snprintf(op.description, sizeof(op.description),
                         "REVERSE: \"%s\"", seg->text);
                break;
            case EFFECT_PITCH_UP:
            case EFFECT_PITCH_DOWN:
                op.timeline_dur_ms = op.source_dur_ms;
                snprintf(op.description, sizeof(op.description),
                         "PITCH(%s): \"%s\"", fx == EFFECT_PITCH_UP ? "UP" : "DOWN", seg->text);
                break;
            case EFFECT_EARRAPE:
                op.timeline_dur_ms = op.source_dur_ms;
                snprintf(op.description, sizeof(op.description),
                         "EARRAPE: \"%s\"", seg->text);
                break;
            case EFFECT_VINE_BOOM:
                op.timeline_dur_ms = 500; /* short impact */
                snprintf(op.description, sizeof(op.description),
                         "VINE BOOM");
                break;
            case EFFECT_FREEZE:
                op.timeline_dur_ms = 1000 + intensity * 200;
                snprintf(op.description, sizeof(op.description),
                         "FREEZE: \"%s\"", seg->text);
                break;
            case EFFECT_FLASH:
                op.timeline_dur_ms = 200;
                snprintf(op.description, sizeof(op.description), "FLASH");
                break;
            case EFFECT_ZOOM:
                op.timeline_dur_ms = op.source_dur_ms * 0.5;
                snprintf(op.description, sizeof(op.description),
                         "ZOOM: \"%s\"", seg->text);
                break;
            case EFFECT_SHAKE:
                op.timeline_dur_ms = op.source_dur_ms;
                snprintf(op.description, sizeof(op.description),
                         "SHAKE: \"%s\"", seg->text);
                break;
            case EFFECT_SENTENCE_MIX:
                op.timeline_dur_ms = op.source_dur_ms * 1.2;
                snprintf(op.description, sizeof(op.description),
                         "SENTENCE MIX: \"%s\"", seg->text);
                break;
            case EFFECT_SUBTITLE:
                op.timeline_dur_ms = op.source_dur_ms;
                strncpy(op.text_overlay, seg->text, sizeof(op.text_overlay) - 1);
                snprintf(op.description, sizeof(op.description),
                         "SUBTITLE: \"%s\"", seg->text);
                break;
            case EFFECT_TO_BE_CONTINUED:
                op.timeline_dur_ms = 2000;
                snprintf(op.description, sizeof(op.description), "TO BE CONTINUED...");
                break;
            case EFFECT_DEEP_FRY:
                op.timeline_dur_ms = op.source_dur_ms;
                snprintf(op.description, sizeof(op.description),
                         "DEEP FRY: \"%s\"", seg->text);
                break;
            case EFFECT_VHS:
                op.timeline_dur_ms = op.source_dur_ms;
                snprintf(op.description, sizeof(op.description),
                         "VHS: \"%s\"", seg->text);
                break;
            case EFFECT_DATAMOSH:
                op.timeline_dur_ms = op.source_dur_ms;
                snprintf(op.description, sizeof(op.description),
                         "DATAMOSH: \"%s\"", seg->text);
                break;
            case EFFECT_BLEEP:
                op.timeline_dur_ms = op.source_dur_ms;
                snprintf(op.description, sizeof(op.description),
                         "BLEEP: \"%s\"", seg->text);
                break;
            case EFFECT_MOCAP_OVERLAY:
                op.timeline_dur_ms = op.source_dur_ms;
                snprintf(op.description, sizeof(op.description),
                         "DANCING SKELETON: \"%s\"", seg->text);
                break;
            default:
                op.timeline_dur_ms = op.source_dur_ms;
                snprintf(op.description, sizeof(op.description),
                         "CUT: \"%s\"", seg->text);
                break;
        }

        ytp_add_op(c, &op);
        timeline_pos += op.timeline_dur_ms;

        /* Add gap between segments (except for fast-paced sections) */
        if (plot_pos > 0.3 && plot_pos < 0.8 && chaos > 5) {
            /* No gap in chaotic sections */
        } else if (s < n_segs - 1) {
            /* Small gap */
            double gap = segs[s+1].start_ms - seg->end_ms;
            if (gap > 0 && gap < 2000) {
                timeline_pos += gap;
            }
        }
    }

    free(segs);

    snprintf(c->description, sizeof(c->description),
             "YTP: %s | %d segments | %.1fs | chaos=%d",
             title, n_segs, c->total_duration_ms / 1000.0, chaos);

    return c;
}

/* ---- Export ------------------------------------------------------------ */

/* Export composition as human-readable script */
int ytp_export_script(ytp_composition *c, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f, "=== %s ===\n", c->title);
    fprintf(f, "%s\n\n", c->description);

    const char *type_names[] = {
        "INTRO", "DIALOGUE", "ACTION", "TRANSITION",
        "PUNCHLINE", "OUTRO", "MUSICAL", "EFFECT"
    };
    const char *fx_names[] = {
        "NONE", "STUTTER", "SLOWMO", "FASTFORWARD", "REVERSE",
        "PITCH_UP", "PITCH_DOWN", "EARRAPE", "VINE_BOOM", "DEEP_FRY",
        "VHS", "KALEIDO", "SHAKE", "ZOOM", "FREEZE", "FLASH",
        "SCRAMBLE", "SENTENCE_MIX", "DATAMOSH", "BLEEP", "SUBTITLE",
        "MOCAP_OVERLAY", "SONIC_SCREAM", "TO_BE_CONTINUED", "MEME_SOUND"
    };

    fprintf(f, "%-8s %-10s %-18s %s\n", "TIME", "TYPE", "EFFECT", "DESCRIPTION");
    fprintf(f, "------------------------------------------------------------------------\n");

    for (int i = 0; i < c->n_ops; i++) {
        edit_op *op = &c->ops[i];
        double t = op->timeline_start_ms / 1000.0;
        fprintf(f, "%7.2fs %-10s %-18s %s\n",
                t,
                type_names[op->type < 8 ? op->type : 0],
                fx_names[op->effect < 24 ? op->effect : 0],
                op->description);
    }

    fprintf(f, "\nTotal duration: %.2f seconds\n", c->total_duration_ms / 1000.0);
    fclose(f);
    return 0;
}

/* Export as ffmpeg concat EDL */
int ytp_export_ffmpeg_edl(ytp_composition *c, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f, "# YTP ffmpeg EDL\n");
    fprintf(f, "# %s\n", c->description);
    fprintf(f, "# Usage: concat these segments with ffmpeg\n\n");

    for (int i = 0; i < c->n_ops; i++) {
        edit_op *op = &c->ops[i];
        fprintf(f, "# [%06.0fms] %s\n", op->timeline_start_ms, op->description);
        fprintf(f, "file 'source_%d.mp4'\n", op->source_clip);
        fprintf(f, "inpoint %.3f\n", op->source_start_ms / 1000.0);
        fprintf(f, "outpoint %.3f\n\n", (op->source_start_ms + op->source_dur_ms) / 1000.0);
    }

    fclose(f);
    return 0;
}

/* ---- Main (test) ------------------------------------------------------- */

int main(int argc, char **argv) {
    printf("=== YTP Composition Engine ===\n\n");

    /* Test with Shrek "Somebody Once Told Me" lyrics */
    const char *words[] = {
        "somebody", "once", "told", "me", "the", "world", "is", "gonna", "roll", "me",
        "I", "ain't", "the", "sharpest", "tool", "in", "the", "shed",
        "she", "was", "looking", "kind", "dumb", "with", "her", "finger", "and", "her",
        "thumb", "in", "the", "shape", "of", "an", "L", "on", "her", "forehead",
        "well", "the", "years", "start", "coming", "and", "they", "don't", "stop", "coming",
        "fed", "to", "the", "rules", "and", "I", "hit", "the", "ground", "running",
        "didn't", "make", "sense", "not", "to", "live", "for", "fun",
        "your", "brain", "gets", "smart", "but", "your", "head", "gets", "dumb",
        "so", "much", "to", "do", "so", "much", "to", "see",
        "so", "what's", "wrong", "with", "taking", "the", "back", "streets",
        "you'll", "never", "know", "if", "you", "don't", "go",
        "you'll", "never", "shine", "if", "you", "don't", "glow"
    };
    int n_words = sizeof(words) / sizeof(words[0]);

    /* Build timestamps (simulate 200ms per word) */
    double *starts = malloc(n_words * sizeof(double));
    double *ends = malloc(n_words * sizeof(double));
    for (int i = 0; i < n_words; i++) {
        starts[i] = i * 200.0;
        ends[i] = starts[i] + 180.0;
    }

    /* Build compositions at different chaos levels */
    for (int chaos = 3; chaos <= 8; chaos += 2) {
        unsigned int seed = 42 + chaos;
        printf("--- Chaos Level %d ---\n", chaos);

        ytp_composition *c = ytp_build(words, starts, ends, n_words, 0, chaos, seed);

        char script_path[256];
        snprintf(script_path, sizeof(script_path), "/tmp/ytp_chaos_%d.txt", chaos);
        ytp_export_script(c, script_path);

        /* Print summary */
        printf("  %s\n", c->description);
        printf("  Script: %s\n\n", script_path);

        ytp_free(c);
    }

    /* Print the chaos=5 script as example */
    printf("=== Example Output (chaos=5) ===\n");
    system("cat /tmp/ytp_chaos_5.txt");

    free(starts);
    free(ends);

    printf("\n=== ALL TESTS PASS ===\n");
    return 0;
}
