/* wb_text_edit.c — text-based video editing via transcript (Descript/Resolve style).
 * Pure C11. Edit the video timeline by editing transcript text.
 */

#include "wbus/wbus_text_edit.h"
#include "wbus/wbus_video.h"
#include "wbus.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

/* ---- internals ------------------------------------------------------------ */

/* Case-insensitive string compare (returns 0 if equal). */
static int str_icmp(const char *a, const char *b) {
    if (!a || !b) return (a == b) ? 0 : -1;
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* Strip leading/trailing punctuation from a word copy. Returns newly allocated. */
static char *strip_punct(const char *word) {
    if (!word) return NULL;
    size_t len = strlen(word);
    size_t start = 0, end = len;
    while (start < end && ispunct((unsigned char)word[start])) start++;
    while (end > start && ispunct((unsigned char)word[end - 1])) end--;
    if (start >= end) {
        char *p = malloc(1); if (p) p[0] = '\0'; return p;
    }
    size_t n = end - start;
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, word + start, n);
    out[n] = '\0';
    return out;
}

/* Check if a word is a filler (um, uh, like, you know, etc.). */
static int is_filler(const char *word) {
    if (!word) return 0;
    char *stripped = strip_punct(word);
    if (!stripped) return 0;
    int filler = (str_icmp(stripped, "um") == 0 ||
                  str_icmp(stripped, "uh") == 0 ||
                  str_icmp(stripped, "ah") == 0 ||
                  str_icmp(stripped, "er") == 0 ||
                  str_icmp(stripped, "like") == 0 ||
                  str_icmp(stripped, "you") == 0 ||  /* "you know" handled separately */
                  str_icmp(stripped, "know") == 0 ||
                  str_icmp(stripped, "basically") == 0 ||
                  str_icmp(stripped, "literally") == 0 ||
                  str_icmp(stripped, "actually") == 0 ||
                  str_icmp(stripped, "so") == 0 ||
                  str_icmp(stripped, "well") == 0);
    free(stripped);
    return filler;
}

/* Find the video track index in the session (first video track). Returns -1 if none. */
static int find_video_track(const wb_session *s) {
    if (!s) return -1;
    for (uint32_t i = 0; i < s->track_count; i++) {
        if (s->tracks[i].kind == WB_TRACK_KIND_VIDEO) return (int)i;
    }
    return -1;
}

/* Forward declarations needed by session functions. */
extern int wb_session_transcript_cut(wb_session *s, int track,
                                     wb_transcript *tr, int w0, int w1);
extern int wb_session_ripple_delete_video_clip(wb_session *s, int track, int clip);
extern int wb_session_split_video_clip(wb_session *s, int track, int clip, double split_pos);

/* ---- public API ---------------------------------------------------------- */

int wb_text_edit_init(wb_text_edit *te, wb_session *session) {
    if (!te || !session) return -1;
    memset(te, 0, sizeof(*te));
    te->session = session;
    te->track = -1;
    te->transcript = wb_transcript_create();
    if (!te->transcript) return -1;
    /* Auto-detect first video track. */
    te->track = find_video_track(session);
    return 0;
}

int wb_text_edit_set_track(wb_text_edit *te, int track) {
    if (!te || !te->session) return -1;
    if (track < 0 || track >= (int)te->session->track_count) return -1;
    if (te->session->tracks[track].kind != WB_TRACK_KIND_VIDEO) return -1;
    te->track = track;
    return 0;
}

void wb_text_edit_set_transcript(wb_text_edit *te, wb_transcript *tr) {
    if (!te) return;
    if (te->transcript) wb_transcript_free(te->transcript);
    te->transcript = tr ? tr : wb_transcript_create();
}

wb_transcript *wb_text_edit_get_transcript(wb_text_edit *te) {
    return te ? te->transcript : NULL;
}

void wb_text_edit_free(wb_text_edit *te) {
    if (!te) return;
    if (te->transcript) {
        wb_transcript_free(te->transcript);
        te->transcript = NULL;
    }
    te->session = NULL;
    te->track = -1;
}

/* Delete words and ripple-cut their time span from video clips. */
int wb_text_edit_delete_words(wb_text_edit *te, int start_word, int end_word) {
    if (!te || !te->transcript) return -1;
    int count = wb_transcript_count(te->transcript);
    if (start_word < 0 || end_word <= start_word || end_word > count) return -1;

    /* If we have a valid video track, cut the media first. */
    if (te->track >= 0 && te->session) {
        if (wb_session_transcript_cut(te->session, te->track,
                                      te->transcript, start_word, end_word) != 0)
            return -1;
        /* transcript_cut already removes the words from the transcript */
        return end_word - start_word;
    }

    /* No video track: just remove words from transcript. */
    return wb_transcript_remove_range(te->transcript, start_word, end_word);
}

/* Reorder a word (and its video segment) to a new position. */
int wb_text_edit_reorder_words(wb_text_edit *te, int word_index, int new_position) {
    if (!te || !te->transcript) return -1;
    int count = wb_transcript_count(te->transcript);
    if (word_index < 0 || word_index >= count) return -1;
    if (new_position < 0 || new_position >= count) return -1;
    if (word_index == new_position) return 0;

    /* Get mutable access to the word array via the helper. */
    wb_word *words = NULL;
    /* We need mutable access — use wb_transcript_word_mut for each word. */
    /* First, copy the word to move. */
    const wb_word *src = wb_transcript_word(te->transcript, word_index);
    if (!src) return -1;
    wb_word tmp = *src;
    tmp.word = malloc(strlen(src->word) + 1);
    if (!tmp.word) return -1;
    strcpy(tmp.word, src->word);

    /* Remove the word at word_index from the transcript. */
    if (wb_transcript_remove_range(te->transcript, word_index, word_index + 1) <= 0) {
        free(tmp.word);
        return -1;
    }

    /* Insert at new_position (which is now valid since we removed one word). */
    /* If word_index < new_position, the removal shifted the target left by 1,
     * so the correct insert position is new_position (already accounts for removal).
     * If word_index > new_position, the removal didn't affect the target. */
    int insert_at = (word_index < new_position) ? new_position - 1 : new_position;
    if (insert_at < 0) insert_at = 0;
    if (insert_at > wb_transcript_count(te->transcript))
        insert_at = wb_transcript_count(te->transcript);

    /* Grow the transcript by 1 word at insert_at. We do this by adding a
     * placeholder at the end and shifting. Simpler: add at end, then shift
     * elements to make room. */
    wb_transcript_add(te->transcript, tmp.start_ms, tmp.end_ms, tmp.word);
    free(tmp.word);
    if (insert_at == wb_transcript_count(te->transcript) - 1) {
        /* Already at the right position. */
        goto done;
    }
    /* Move the last word to insert_at by shifting everything in between. */
    {
        int n = wb_transcript_count(te->transcript);
        wb_word last = *wb_transcript_word(te->transcript, n - 1);
        /* Shift [insert_at, n-2] right by 1 (using remove + add pattern).
         * Simpler: use word_mut to do it in-place. */
        words = malloc(n * sizeof(wb_word));
        if (!words) return -1;
        /* Copy all words to temp array. */
        for (int i = 0; i < n; i++) {
            const wb_word *w = wb_transcript_word(te->transcript, i);
            words[i] = *w;
            words[i].word = malloc(strlen(w->word) + 1);
            strcpy(words[i].word, w->word);
        }
        /* Free original words and rebuild. */
        wb_transcript_free(te->transcript);
        te->transcript = wb_transcript_create();
        /* Re-add in new order: [0..insert_at), then last, then [insert_at..n-1). */
        for (int i = 0; i < insert_at; i++)
            wb_transcript_add(te->transcript, words[i].start_ms, words[i].end_ms, words[i].word);
        wb_transcript_add(te->transcript, last.start_ms, last.end_ms, last.word);
        for (int i = insert_at; i < n - 1; i++)
            wb_transcript_add(te->transcript, words[i].start_ms, words[i].end_ms, words[i].word);
        /* Cleanup temp. */
        for (int i = 0; i < n; i++) free(words[i].word);
        free(words);
        words = NULL;
    }

done:
    /* Recompute word timings so they remain contiguous (each word gets equal
     * share of the total duration). The total timeline length stays the same;
     * we just reassign which word owns which time slot. */
    count = wb_transcript_count(te->transcript);
    double total_ms = wb_transcript_duration_ms(te->transcript);
    if (count > 0 && total_ms > 0) {
        double slot = total_ms / count;
        for (int i = 0; i < count; i++) {
            wb_word *w = wb_transcript_word_mut(te->transcript, i);
            if (w) {
                w->start_ms = i * slot;
                w->end_ms = (i + 1) * slot;
            }
        }
    }

    (void)te->session;  /* would reorder clips here if track is set */
    return 0;
}

/* Insert a silence pause before word_index. */
int wb_text_edit_insert_pause(wb_text_edit *te, int word_index, float duration_sec) {
    if (!te || !te->transcript || duration_sec <= 0.0f) return -1;
    int count = wb_transcript_count(te->transcript);
    if (word_index < 0 || word_index > count) return -1;

    double pause_ms = (double)duration_sec * 1000.0;

    /* Shift all words from word_index onward right by pause_ms. */
    if (wb_transcript_shift_from(te->transcript, word_index, pause_ms) != 0)
        return -1;

    /* If we have a video track, shift all video clips that start at or after
     * the word boundary's time position. */
    if (te->track >= 0 && te->session) {
        wb_track *tk = &te->session->tracks[te->track];
        double boundary_sec;
        if (word_index < count) {
            const wb_word *w = wb_transcript_word(te->transcript, word_index);
            boundary_sec = w->start_ms / 1000.0 - duration_sec;
        } else {
            boundary_sec = wb_transcript_duration_ms(te->transcript) / 1000.0 - duration_sec;
        }
        for (uint32_t c = 0; c < tk->clip_count; c++) {
            wb_clip *cl = &tk->clips[c];
            if (cl->type != 2 || !cl->video) continue;
            if (cl->start >= boundary_sec - 1e-6)
                cl->start += duration_sec;
        }
        /* Grow session length to accommodate the pause. */
        if (te->session->length > 0) {
            te->session->length += (double)duration_sec * WB_SAMPLE_RATE;
        }
    }

    return 0;
}

/* Detect and remove dead air (silent gaps between words > threshold). */
int wb_text_edit_dead_air(wb_text_edit *te, float threshold_sec) {
    if (!te || !te->transcript || threshold_sec <= 0.0f) return -1;
    int removed = 0;

    /* Iterate from the end so removals don't affect earlier indices. */
    for (int i = wb_transcript_count(te->transcript) - 1; i > 0; i--) {
        const wb_word *w0 = wb_transcript_word(te->transcript, i - 1);
        const wb_word *w1 = wb_transcript_word(te->transcript, i);
        if (!w0 || !w1) continue;
        double gap_ms = w1->start_ms - w0->end_ms;
        double gap_sec = gap_ms / 1000.0;
        if (gap_sec > (double)threshold_sec) {
            /* Shift all words from i onward left by the gap. */
            if (wb_transcript_shift_from(te->transcript, i, -gap_ms) != 0)
                return -1;

            /* Shift video clips if track is set. */
            if (te->track >= 0 && te->session) {
                wb_track *tk = &te->session->tracks[te->track];
                double gap_start = w1->start_ms / 1000.0;
                for (uint32_t c = 0; c < tk->clip_count; c++) {
                    wb_clip *cl = &tk->clips[c];
                    if (cl->type != 2 || !cl->video) continue;
                    if (cl->start >= gap_start - 1e-6)
                        cl->start -= gap_sec;
                }
                /* Shrink session length. */
                if (te->session->length > 0) {
                    double shrink = gap_sec * WB_SAMPLE_RATE;
                    if (te->session->length > shrink)
                        te->session->length -= shrink;
                    else
                        te->session->length = 0;
                }
            }
            removed++;
        }
    }
    return removed;
}

/* Remove filler words (um, uh, like, etc.) and cut their time spans. */
int wb_text_edit_um_ah_remove(wb_text_edit *te) {
    if (!te || !te->transcript) return -1;
    int removed = 0;
    int count = wb_transcript_count(te->transcript);

    /* Scan from the end so removals don't affect earlier indices. */
    for (int i = count - 1; i >= 0; i--) {
        const wb_word *w = wb_transcript_word(te->transcript, i);
        if (!w) continue;
        if (is_filler(w->word)) {
            double word_start = w->start_ms;
            double word_end = w->end_ms;
            double word_dur_ms = word_end - word_start;

            /* Remove the word from the transcript. */
            if (wb_transcript_remove_range(te->transcript, i, i + 1) <= 0)
                continue;

            /* Shift all later words left by the removed word's duration. */
            if (wb_transcript_shift_from(te->transcript, i, -word_dur_ms) != 0)
                return -1;

            /* Cut from video clips if track is set. */
            if (te->track >= 0 && te->session) {
                double sec = word_dur_ms / 1000.0;
                wb_track *tk = &te->session->tracks[te->track];
                /* Ripple-shift all clips that start after the removed word's start. */
                for (uint32_t c = 0; c < tk->clip_count; c++) {
                    wb_clip *cl = &tk->clips[c];
                    if (cl->type != 2 || !cl->video) continue;
                    if (cl->start >= word_start / 1000.0 - 1e-6)
                        cl->start -= sec;
                }
                /* Shrink session length. */
                if (te->session->length > 0) {
                    double shrink = sec * WB_SAMPLE_RATE;
                    if (te->session->length > shrink)
                        te->session->length -= shrink;
                    else
                        te->session->length = 0;
                }
            }
            removed++;
            count--;
        }
    }
    return removed;
}

int wb_text_edit_get_word_count(const wb_text_edit *te) {
    if (!te || !te->transcript) return 0;
    return wb_transcript_count(te->transcript);
}

double wb_text_edit_get_total_duration(const wb_text_edit *te) {
    if (!te || !te->transcript) return 0.0;
    return wb_transcript_duration_ms(te->transcript) / 1000.0;
}