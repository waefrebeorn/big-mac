/* wb_transcript.c — editable transcript model (R015 G6). Pure C11. */

#include "wbus/wbus_transcript.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

struct wb_transcript {
    wb_word *words;
    int   count;
    int   cap;
};

wb_transcript *wb_transcript_create(void) {
    return calloc(1, sizeof(wb_transcript));
}

void wb_transcript_free(wb_transcript *t) {
    if (!t) return;
    for (int i = 0; i < t->count; i++) free(t->words[i].word);
    free(t->words);
    free(t);
}

int wb_transcript_count(const wb_transcript *t) { return t ? t->count : 0; }

const wb_word *wb_transcript_word(const wb_transcript *t, int i) {
    if (!t || i < 0 || i >= t->count) return NULL;
    return &t->words[i];
}

static char *dup_str(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

void wb_transcript_add(wb_transcript *t, double start_ms, double end_ms,
                        const char *word) {
    if (!t || !word) return;
    if (t->count >= t->cap) {
        int ncap = t->cap ? t->cap * 2 : 64;
        wb_word *nw = realloc(t->words, ncap * sizeof(wb_word));
        if (!nw) return;
        t->words = nw; t->cap = ncap;
    }
    wb_word *w = &t->words[t->count++];
    w->start_ms = start_ms;
    w->end_ms = end_ms;
    w->word = dup_str(word);
}

int wb_transcript_word_at(const wb_transcript *t, double ms) {
    if (!t || t->count == 0) return -1;
    int lo = 0, hi = t->count - 1, best = -1;
    /* find last word with start_ms <= ms */
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (t->words[mid].start_ms <= ms) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (best >= 0 && ms < t->words[best].end_ms) return best;
    if (best >= 0 && ms >= t->words[best].end_ms) {
        /* between this word and next: return this word if within ~0.5*span */
        return best;
    }
    return best;
}

double wb_transcript_duration_ms(const wb_transcript *t) {
    if (!t || t->count == 0) return 0.0;
    return t->words[t->count - 1].end_ms;
}

void wb_transcript_set_word(wb_transcript *t, int i, const char *word) {
    if (!t || i < 0 || i >= t->count || !word) return;
    free(t->words[i].word);
    t->words[i].word = dup_str(word);
}

wb_word *wb_transcript_word_mut(wb_transcript *t, int i) {
    if (!t || i < 0 || i >= t->count) return NULL;
    return &t->words[i];
}

int wb_transcript_shift_from(wb_transcript *t, int start_idx, double delta_ms) {
    if (!t || start_idx < 0 || start_idx > t->count) return -1;
    for (int i = start_idx; i < t->count; i++) {
        t->words[i].start_ms += delta_ms;
        t->words[i].end_ms += delta_ms;
    }
    return 0;
}

int wb_transcript_remove_range(wb_transcript *t, int i0, int i1) {
    if (!t || i0 < 0 || i1 <= i0 || i1 > t->count) return -1;
    for (int i = i0; i < i1; i++) free(t->words[i].word);
    int nrem = i1 - i0;
    for (int i = i1; i < t->count; i++)
        t->words[i - nrem] = t->words[i];
    t->count -= nrem;
    return nrem;
}

int wb_transcript_write_srt(const wb_transcript *t, const char *srt_path) {
    if (!t || !srt_path) return -1;
    FILE *f = fopen(srt_path, "w");
    if (!f) return -1;
    for (int i = 0; i < t->count; i++) {
        const wb_word *w = &t->words[i];
        int sh = (int)(w->start_ms / 3600000.0);
        int sm = (int)(((int)w->start_ms % 3600000) / 60000.0);
        int ss = (int)(((int)w->start_ms % 60000) / 1000.0);
        int ssec = (int)w->start_ms % 1000;
        int eh = (int)(w->end_ms / 3600000.0);
        int em = (int)(((int)w->end_ms % 3600000) / 60000.0);
        int es = (int)(((int)w->end_ms % 60000) / 1000.0);
        int esec = (int)w->end_ms % 1000;
        fprintf(f, "%d\n", i + 1);
        fprintf(f, "%02d:%02d:%02d,%03d --> %02d:%02d:%02d,%03d\n",
                sh, sm, ss, ssec, eh, em, es, esec);
        fprintf(f, "%s\n\n", w->word);
    }
    fclose(f);
    return 0;
}

/* parse "HH:MM:SS,mmm" or "MM:SS.mmm" -> ms */
static double parse_ts(const char *s) {
    int h = 0, m = 0, sec = 0, ms = 0;
    /* try HH:MM:SS,mmm */
    if (sscanf(s, "%d:%d:%d,%d", &h, &m, &sec, &ms) == 4) {
        /* ok */
    } else if (sscanf(s, "%d:%d,%d", &m, &sec, &ms) == 3) {
        h = 0;
    } else if (sscanf(s, "%d:%d:%d.%d", &h, &m, &sec, &ms) == 4) {
        /* dot variant */
    } else {
        return -1;
    }
    return ((double)h * 3600.0 + (double)m * 60.0 + (double)sec) * 1000.0 + ms;
}

int wb_transcript_parse_srt(wb_transcript *t, const char *srt_path) {
    if (!t || !srt_path) return -1;
    FILE *f = fopen(srt_path, "r");
    if (!f) return -1;
    char line[2048];
    /* state: expecting index, then timestamp, then text lines until blank */
    int phase = 0; /* 0=index, 1=timestamp, 2=text */
    double seg_start = 0, seg_end = 0;
    char *seg_text = NULL;
    int seg_cap = 0, seg_len = 0;

    while (fgets(line, sizeof(line), f)) {
        /* strip trailing newline */
        size_t L = strlen(line);
        while (L > 0 && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = '\0';

        if (phase == 0) {
            if (L == 0) continue;            /* skip blanks before index */
            phase = 1;                        /* this line is the index (ignore) */
        } else if (phase == 1) {
            /* timestamp line: "start --> end" */
            char a[256], b[256];
            if (sscanf(line, "%255[^ ] --> %255[^ ]", a, b) == 2) {
                seg_start = parse_ts(a);
                seg_end = parse_ts(b);
                seg_len = 0;
                if (seg_cap == 0) { seg_cap = 1024; seg_text = malloc(seg_cap); }
                seg_text[0] = '\0';
                phase = 2;
            } else {
                phase = 0; /* malformed, reset */
            }
        } else { /* phase 2: text */
            if (L == 0) {
                /* end of cue: distribute timing across words */
                if (seg_text && seg_len > 0 && seg_end > seg_start) {
                    /* tokenize words */
                    char *tok = strtok(seg_text, " \t\n\r");
                    int nwords = 0;
                    char **ws = NULL; int wcap = 0;
                    while (tok) {
                        if (nwords >= wcap) {
                            wcap = wcap ? wcap*2 : 16;
                            ws = realloc(ws, wcap * sizeof(char*));
                        }
                        ws[nwords++] = tok;
                        tok = strtok(NULL, " \t\n\r");
                    }
                    double span = seg_end - seg_start;
                    for (int i = 0; i < nwords; i++) {
                        double ws_start = seg_start + (i * span) / nwords;
                        double ws_end = seg_start + ((i+1) * span) / nwords;
                        wb_transcript_add(t, ws_start, ws_end, ws[i]);
                    }
                    free(ws);
                }
                phase = 0;
            } else {
                /* accumulate text (join multi-line cues with space) */
                if (seg_len > 0 && seg_len + 1 < seg_cap) {
                    seg_text[seg_len++] = ' ';
                }
                size_t need = seg_len + strlen(line) + 1;
                if (need > (size_t)seg_cap) {
                    while ((int)need > seg_cap) seg_cap *= 2;
                    seg_text = realloc(seg_text, seg_cap);
                }
                strcat(seg_text, line);
                seg_len = (int)strlen(seg_text);
            }
        }
    }
    /* flush trailing cue */
    if (phase == 2 && seg_text && seg_len > 0 && seg_end > seg_start) {
        char *tok = strtok(seg_text, " \t\n\r");
        int nwords = 0; char **ws = NULL; int wcap = 0;
        while (tok) {
            if (nwords >= wcap) { wcap = wcap?wcap*2:16; ws = realloc(ws, wcap*sizeof(char*)); }
            ws[nwords++] = tok; tok = strtok(NULL, " \t\n\r");
        }
        double span = seg_end - seg_start;
        for (int i = 0; i < nwords; i++) {
            double ws_start = seg_start + (i*span)/nwords;
            double ws_end = seg_start + ((i+1)*span)/nwords;
            wb_transcript_add(t, ws_start, ws_end, ws[i]);
        }
        free(ws);
    }
    free(seg_text);
    fclose(f);
    return t->count > 0 ? 0 : -1;
}

wb_transcript *wb_transcript_from_srt(const char *srt_path) {
    wb_transcript *t = wb_transcript_create();
    if (!t) return NULL;
    if (wb_transcript_parse_srt(t, srt_path) != 0) {
        wb_transcript_free(t);
        return NULL;
    }
    return t;
}
