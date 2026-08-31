/* wbus_transcript.h — editable transcript model for the video editor (R015 G6).
 *
 * A transcript is an ordered list of WORDS, each with a [start_ms, end_ms]
 * timestamp. This is the data model Descript / Hindenburg use: the timeline
 * IS the transcript — click a word to seek, drag a word-range to select/trim.
 *
 * SRT from whisper gives SEGMENT-level timestamps. We distribute those across
 * the segment's words (word i spans start + i/n of the segment) so each word
 * is individually seekable/trimmable. This is honest, functional word-level
 * timing good enough for click-to-seek and drag-to-trim (R017 G6).
 */

#ifndef WUBUS_WBUS_TRANSCRIPT_H
#define WUBUS_WBUS_TRANSCRIPT_H


#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
#include <stddef.h>

typedef struct {
    double start_ms;   /* inclusive */
    double end_ms;     /* exclusive */
    char  *word;       /* owned, NUL-terminated */
} wb_word;

typedef struct wb_transcript wb_transcript;

wb_transcript *wb_transcript_create(void);
void            wb_transcript_free(wb_transcript *t);

/* Number of words. */
int  wb_transcript_count(const wb_transcript *t);
/* Get word i (read-only). */
const wb_word *wb_transcript_word(const wb_transcript *t, int i);

/* Append a word (copies the string). */
void wb_transcript_add(wb_transcript *t, double start_ms, double end_ms,
                        const char *word);

/* Find the word index whose span contains time `ms` (binary search).
 * Returns -1 if before the first or after the last word. */
int wb_transcript_word_at(const wb_transcript *t, double ms);

/* Total transcript duration (ms) = last word's end. */
double wb_transcript_duration_ms(const wb_transcript *t);

/* Edit: replace a word's text (keeps its timing). */
void wb_transcript_set_word(wb_transcript *t, int i, const char *word);

/* Edit: get mutable pointer to word i (for internal use by text editing).
 * Returns NULL on bad index. Use with care — caller must not break ordering. */
wb_word *wb_transcript_word_mut(wb_transcript *t, int i);

/* Edit: shift all words from index `start_idx` onward by `delta_ms`.
 * Positive delta shifts right (later), negative shifts left (earlier).
 * Returns 0 on success, -1 on error. */
int wb_transcript_shift_from(wb_transcript *t, int start_idx, double delta_ms);

/* Edit: delete the word RANGE [i0, i1) (Descript-style text editing).
 * Words are removed; surrounding timing is untouched (the timeline gap is
 * real: playback of that span is what the editor cuts). Returns the number
 * of words removed, or -1 on bad range. */
int wb_transcript_remove_range(wb_transcript *t, int i0, int i1);

/* Export: write an SRT from the transcript (one cue per word, or merge
 *相邻的同段? keep per-word for max granularity). Returns 0 on success. */
int wb_transcript_write_srt(const wb_transcript *t, const char *srt_path);

/* Parse an SRT file into a transcript (segment timing distributed to words).
 * Returns 0 on success, -1 on error. */
int wb_transcript_parse_srt(wb_transcript *t, const char *srt_path);

/* Build a transcript directly from a whisper SRT file (convenience). */
wb_transcript *wb_transcript_from_srt(const char *srt_path);


#ifdef __cplusplus
}
#endif
#endif /* WUBUS_WBUS_TRANSCRIPT_H */
