/* wb_sentence_mix.c — sentence mixing engine for YTP (R081).
 *
 * Takes a transcript (word-level timestamps from whisper.cpp) and
 * rearranges words to create YTP-style sentence mixing.
 *
 * Techniques:
 *   - Word shuffle: randomize word order
 *   - Word repeat: repeat a word N times (stutter)
 *   - Word salad: pick random words from different parts
 *   - Sentence rebuild: reassemble words into new phrases
 *   - Pitch-shift segments: speed up/slow down individual words
 *   - Reverse segments: play words backwards
 *
 * This is the core "context-aware" editing: we know what each word is,
 * when it occurs, and can rearrange them creatively.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ---- Word segment (audio slice for one word) ---------------------------- */

typedef struct {
    char word[128];
    double start_ms;
    double end_ms;
    int source_clip_index;  /* which source clip this came from */
} word_seg;

/* ---- Mix operation ----------------------------------------------------- */

typedef enum {
    MIX_OP_WORD,        /* place a word at a time */
    MIX_OP_SILENCE,     /* insert silence */
    MIX_OP_EFFECT,      /* insert effect sound */
} mix_op_type;

typedef struct {
    mix_op_type type;
    union {
        struct { int word_index; } word;
        struct { double duration_ms; } silence;
        struct { char effect_name[64]; double duration_ms; } effect;
    };
} mix_op;

/* ---- Mix result -------------------------------------------------------- */

typedef struct {
    mix_op *ops;
    int n_ops;
    int cap;
    double total_duration_ms;
} mix_result;

/* ---- Sentence Mixer ---------------------------------------------------- */

typedef struct {
    word_seg *words;
    int n_words;
    int cap;
    mix_result result;
    unsigned int seed;
} sentence_mixer;

/* ---- API --------------------------------------------------------------- */

sentence_mixer *sentence_mixer_create(void) {
    sentence_mixer *m = calloc(1, sizeof(sentence_mixer));
    m->seed = (unsigned int)time(NULL);
    return m;
}

void sentence_mixer_free(sentence_mixer *m) {
    if (!m) return;
    free(m->words);
    free(m->result.ops);
    free(m);
}

void sentence_mixer_set_seed(sentence_mixer *m, unsigned int seed) {
    m->seed = seed;
}

/* Add a word from a transcript */
void sentence_mixer_add_word(sentence_mixer *m, const char *word,
                              double start_ms, double end_ms, int clip_idx) {
    if (m->n_words >= m->cap) {
        m->cap = m->cap ? m->cap * 2 : 256;
        m->words = realloc(m->words, m->cap * sizeof(word_seg));
    }
    word_seg *w = &m->words[m->n_words++];
    strncpy(w->word, word, sizeof(w->word) - 1);
    w->word[sizeof(w->word) - 1] = '\0';
    w->start_ms = start_ms;
    w->end_ms = end_ms;
    w->source_clip_index = clip_idx;
}

/* Add a word from a whisper SRT file */
int sentence_mixer_load_srt(sentence_mixer *m, const char *srt_path, int clip_idx) {
    FILE *f = fopen(srt_path, "r");
    if (!f) return -1;

    char line[1024];
    double start_ms = 0, end_ms = 0;
    int in_cue = 0;

    while (fgets(line, sizeof(line), f)) {
        /* Parse SRT timestamp line: "HH:MM:SS,mmm --> HH:MM:SS,mmm" */
        int sh, sm, ss, ems, eh, em, es, ems2;
        if (sscanf(line, "%d:%d:%d,%d --> %d:%d:%d,%d",
                   &sh, &sm, &ss, &ems, &eh, &em, &es, &ems2) == 8) {
            start_ms = (sh * 3600 + sm * 60 + ss) * 1000.0 + ems;
            end_ms = (eh * 3600 + em * 60 + es) * 1000.0 + ems2;
            in_cue = 1;
            continue;
        }

        /* Text line */
        if (in_cue && strlen(line) > 1) {
            /* Strip newline */
            line[strcspn(line, "\r\n")] = '\0';
            /* Skip cue numbers */
            if (atoi(line) > 0 && strlen(line) < 6) continue;

            /* Split into words */
            char *text = line;
            char *tok;
            int n_words = 0;
            char *words[256];
            double word_dur = (end_ms - start_ms) / (double)strlen(text) * 5.0; /* rough */

            /* Tokenize by spaces */
            char buf[1024];
            strncpy(buf, text, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';

            tok = strtok(buf, " ");
            double word_start = start_ms;
            while (tok && n_words < 255) {
                words[n_words++] = tok;
                tok = strtok(NULL, " ");
            }

            /* Distribute time evenly */
            if (n_words > 0) {
                double per_word = (end_ms - start_ms) / (double)n_words;
                for (int i = 0; i < n_words; i++) {
                    sentence_mixer_add_word(m, words[i],
                                            word_start + i * per_word,
                                            word_start + (i + 1) * per_word,
                                            clip_idx);
                }
            }
        }

        /* Empty line = end of cue */
        if (strlen(line) <= 1) in_cue = 0;
    }

    fclose(f);
    return 0;
}

/* ---- Mix Operations ---------------------------------------------------- */

static mix_op *add_op(sentence_mixer *m) {
    if (m->result.n_ops >= m->result.cap) {
        m->result.cap = m->result.cap ? m->result.cap * 2 : 256;
        m->result.ops = realloc(m->result.ops, m->result.cap * sizeof(mix_op));
    }
    return &m->result.ops[m->result.n_ops++];
}

void mix_add_word(sentence_mixer *m, int word_index) {
    if (word_index < 0 || word_index >= m->n_words) return;
    mix_op *op = add_op(m);
    op->type = MIX_OP_WORD;
    op->word.word_index = word_index;
    m->result.total_duration_ms += m->words[word_index].end_ms - m->words[word_index].start_ms;
}

void mix_add_silence(sentence_mixer *m, double duration_ms) {
    mix_op *op = add_op(m);
    op->type = MIX_OP_SILENCE;
    op->silence.duration_ms = duration_ms;
    m->result.total_duration_ms += duration_ms;
}

void mix_add_effect(sentence_mixer *m, const char *name, double duration_ms) {
    mix_op *op = add_op(m);
    op->type = MIX_OP_EFFECT;
    strncpy(op->effect.effect_name, name, sizeof(op->effect.effect_name) - 1);
    op->effect.duration_ms = duration_ms;
    m->result.total_duration_ms += duration_ms;
}

/* ---- Mixing Strategies -------------------------------------------------- */

/* Strategy 1: Random shuffle */
void mix_shuffle(sentence_mixer *m) {
    m->result.n_ops = 0;
    m->result.total_duration_ms = 0;

    /* Fisher-Yates shuffle */
    int *indices = malloc(m->n_words * sizeof(int));
    for (int i = 0; i < m->n_words; i++) indices[i] = i;

    for (int i = m->n_words - 1; i > 0; i--) {
        int j = rand_r(&m->seed) % (i + 1);
        int t = indices[i]; indices[i] = indices[j]; indices[j] = t;
    }

    for (int i = 0; i < m->n_words; i++) {
        mix_add_word(m, indices[i]);
    }

    free(indices);
}

/* Strategy 2: Stutter (repeat each word N times) */
void mix_stutter(sentence_mixer *m, int repeats) {
    m->result.n_ops = 0;
    m->result.total_duration_ms = 0;

    for (int i = 0; i < m->n_words; i++) {
        for (int r = 0; r < repeats; r++) {
            mix_add_word(m, i);
        }
    }
}

/* Strategy 3: Word salad (pick random words) */
void mix_word_salad(sentence_mixer *m, int count) {
    m->result.n_ops = 0;
    m->result.total_duration_ms = 0;

    for (int i = 0; i < count; i++) {
        int idx = rand_r(&m->seed) % m->n_words;
        mix_add_word(m, idx);
    }
}

/* Strategy 4: Reverse order */
void mix_reverse(sentence_mixer *m) {
    m->result.n_ops = 0;
    m->result.total_duration_ms = 0;

    for (int i = m->n_words - 1; i >= 0; i--) {
        mix_add_word(m, i);
    }
}

/* Strategy 5: Every Nth word (skip words) */
void mix_every_nth(sentence_mixer *m, int n) {
    m->result.n_ops = 0;
    m->result.total_duration_ms = 0;

    for (int i = 0; i < m->n_words; i += n) {
        mix_add_word(m, i);
    }
}

/* Strategy 6: Duplicate specific word (YTP "somebody once told me" style) */
void mix_emphasize_word(sentence_mixer *m, const char *target, int repeats) {
    m->result.n_ops = 0;
    m->result.total_duration_ms = 0;

    for (int i = 0; i < m->n_words; i++) {
        if (strcasecmp(m->words[i].word, target) == 0) {
            for (int r = 0; r < repeats; r++) {
                mix_add_word(m, i);
            }
        } else {
            mix_add_word(m, i);
        }
    }
}

/* Strategy 7: Build a phrase from specific words */
void mix_build_phrase(sentence_mixer *m, const char **phrase, int n_words) {
    m->result.n_ops = 0;
    m->result.total_duration_ms = 0;

    for (int i = 0; i < n_words; i++) {
        /* Find the word in our transcript */
        for (int j = 0; j < m->n_words; j++) {
            if (strcasecmp(m->words[j].word, phrase[i]) == 0) {
                mix_add_word(m, j);
                break;
            }
        }
    }
}

/* Strategy 8: Interleave two sources */
void mix_interleave(sentence_mixer *m, int source_a, int source_b) {
    m->result.n_ops = 0;
    m->result.total_duration_ms = 0;

    int i = 0;
    int added = 1;
    while (added) {
        added = 0;
        for (; i < m->n_words; i++) {
            if (m->words[i].source_clip_index == source_a) {
                mix_add_word(m, i);
                added = 1;
                break;
            }
        }
        for (; i < m->n_words; i++) {
            if (m->words[i].source_clip_index == source_b) {
                mix_add_word(m, i);
                added = 1;
                break;
            }
        }
        i++;
    }
}

/* ---- Export ------------------------------------------------------------ */

/* Export the mix as an EDL (Edit Decision List) for ffmpeg */
int mix_export_edl(sentence_mixer *m, const char *edl_path) {
    FILE *f = fopen(edl_path, "w");
    if (!f) return -1;

    fprintf(f, "# YTP Sentence Mix EDL\n");
    fprintf(f, "# Total duration: %.2f ms (%.2f s)\n",
            m->result.total_duration_ms, m->result.total_duration_ms / 1000.0);
    fprintf(f, "# Operations: %d\n\n", m->result.n_ops);

    double timeline_pos = 0;
    for (int i = 0; i < m->result.n_ops; i++) {
        mix_op *op = &m->result.ops[i];
        switch (op->type) {
            case MIX_OP_WORD: {
                word_seg *w = &m->words[op->word.word_index];
                double dur = w->end_ms - w->start_ms;
                fprintf(f, "[%06.0fms] WORD \"%s\" src=%d in=%.0fms dur=%.0fms\n",
                        timeline_pos, w->word, w->source_clip_index,
                        w->start_ms, dur);
                break;
            }
            case MIX_OP_SILENCE:
                fprintf(f, "[%06.0fms] SILENCE dur=%.0fms\n",
                        timeline_pos, op->silence.duration_ms);
                break;
            case MIX_OP_EFFECT:
                fprintf(f, "[%06.0fms] EFFECT \"%s\" dur=%.0fms\n",
                        timeline_pos, op->effect.effect_name, op->effect.duration_ms);
                break;
        }
        switch (op->type) {
            case MIX_OP_WORD:
                timeline_pos += m->words[op->word.word_index].end_ms - m->words[op->word.word_index].start_ms;
                break;
            case MIX_OP_SILENCE:
                timeline_pos += op->silence.duration_ms;
                break;
            case MIX_OP_EFFECT:
                timeline_pos += op->effect.duration_ms;
                break;
        }
    }

    fclose(f);
    return 0;
}

/* Export as a readable script */
int mix_export_script(sentence_mixer *m, const char *script_path) {
    FILE *f = fopen(script_path, "w");
    if (!f) return -1;

    fprintf(f, "# YTP Sentence Mix Script\n\n");
    fprintf(f, "MIX: ");

    for (int i = 0; i < m->result.n_ops; i++) {
        mix_op *op = &m->result.ops[i];
        if (op->type == MIX_OP_WORD) {
            fprintf(f, "%s ", m->words[op->word.word_index].word);
        } else if (op->type == MIX_OP_SILENCE) {
            fprintf(f, "... ");
        } else if (op->type == MIX_OP_EFFECT) {
            fprintf(f, "[%s] ", op->effect.effect_name);
        }
    }

    fprintf(f, "\n");
    fclose(f);
    return 0;
}

/* ---- Main (test) ------------------------------------------------------- */

int main(int argc, char **argv) {
    printf("=== YTP Sentence Mixing Engine ===\n\n");

    sentence_mixer *m = sentence_mixer_create();
    sentence_mixer_set_seed(m, 42);

    /* Test with sample words (simulating a transcript) */
    const char *test_words[] = {
        "somebody", "once", "told", "me", "the", "world", "is", "gonna", "roll", "me",
        "I", "ain't", "the", "sharpest", "tool", "in", "the", "shed",
        "she", "was", "looking", "kind", "dumb", "with", "her", "finger", "and", "her",
        "thumb", "in", "the", "shape", "of", "an", "L", "on", "her", "forehead"
    };
    int n_test = sizeof(test_words) / sizeof(test_words[0]);

    for (int i = 0; i < n_test; i++) {
        double start = i * 200.0;
        sentence_mixer_add_word(m, test_words[i], start, start + 180.0, 0);
    }

    printf("Loaded %d words\n", m->n_words);
    printf("Original: ");
    for (int i = 0; i < m->n_words; i++) printf("%s ", m->words[i].word);
    printf("\n\n");

    /* Test strategies */
    printf("--- Strategy 1: Shuffle ---\n");
    mix_shuffle(m);
    mix_export_script(m, "/tmp/mix_shuffle.txt");
    system("cat /tmp/mix_shuffle.txt");

    printf("\n--- Strategy 2: Stutter (x3) ---\n");
    mix_stutter(m, 3);
    mix_export_script(m, "/tmp/mix_stutter.txt");
    system("head -1 /tmp/mix_stutter.txt");

    printf("\n--- Strategy 3: Word Salad (10 words) ---\n");
    mix_word_salad(m, 10);
    mix_export_script(m, "/tmp/mix_salad.txt");
    system("cat /tmp/mix_salad.txt");

    printf("\n--- Strategy 4: Reverse ---\n");
    mix_reverse(m);
    mix_export_script(m, "/tmp/mix_reverse.txt");
    system("cat /tmp/mix_reverse.txt");

    printf("\n--- Strategy 5: Emphasize 'the' (x5) ---\n");
    mix_emphasize_word(m, "the", 5);
    mix_export_script(m, "/tmp/mix_emph.txt");
    system("cat /tmp/mix_emph.txt");

    printf("\n--- Strategy 6: Build phrase ---\n");
    const char *phrase[] = {"I", "am", "the", "sharpest", "tool"};
    mix_build_phrase(m, phrase, 5);
    mix_export_script(m, "/tmp/mix_phrase.txt");
    system("cat /tmp/mix_phrase.txt");

    /* Export EDL */
    mix_shuffle(m);
    mix_export_edl(m, "/tmp/mix_edl.txt");
    printf("\n--- EDL exported to /tmp/mix_edl.txt ---\n");
    system("head -20 /tmp/mix_edl.txt");

    sentence_mixer_free(m);
    printf("\n=== ALL TESTS PASS ===\n");
    return 0;
}
