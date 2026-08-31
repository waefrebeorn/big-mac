/* wbus_text_edit.h — text-based video editing via transcript (Descript/Resolve style).
 *
 * Edit the video timeline by editing transcript text. Each word in the transcript
 * maps to a time span on the video track; deleting/reordering words performs the
 * corresponding edit on the underlying video clips.
 *
 * This is the "Descript" model: the transcript IS the timeline UI.
 */

#ifndef WUBUS_WBUS_TEXT_EDIT_H
#define WUBUS_WBUS_TEXT_EDIT_H

#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
#include <stddef.h>

#include "wbus_transcript.h"
struct wb_session;

/* Text-edit context: binds a transcript to a session's video track. */
typedef struct wb_text_edit {
    struct wb_session *session;
    int                track;       /* video track index (-1 = not set) */
    wb_transcript     *transcript;  /* owned by text_edit (init creates it) */
} wb_text_edit;

/* Initialize a text-edit context. Creates an internal empty transcript.
 * The caller should set the video track with wb_text_edit_set_track() before
 * performing edits that modify clips. Returns 0 on success, -1 on error. */
int wb_text_edit_init(wb_text_edit *te, struct wb_session *session);

/* Set the video track to edit. Returns 0 on success, -1 on bad track. */
int wb_text_edit_set_track(wb_text_edit *te, int track);

/* Set the transcript to use (takes ownership). Pass NULL to create empty. */
void wb_text_edit_set_transcript(wb_text_edit *te, wb_transcript *tr);

/* Get the transcript (for populating with words). */
wb_transcript *wb_text_edit_get_transcript(wb_text_edit *te);

/* Free internal resources (transcript). Does NOT destroy the session. */
void wb_text_edit_free(wb_text_edit *te);

/* Delete words [start_word, end_word) from the transcript AND ripple-cut their
 * time span out of the track's video clips. Returns number of words removed,
 * or -1 on error. */
int wb_text_edit_delete_words(wb_text_edit *te, int start_word, int end_word);

/* Reorder: move the word (and its corresponding video segment) at word_index
 * to new_position in the word list. The video clip timeline is reordered
 * accordingly. Returns 0 on success, -1 on error. */
int wb_text_edit_reorder_words(wb_text_edit *te, int word_index, int new_position);

/* Insert a silence pause of duration_sec at the boundary before word_index
 * (i.e., after word[word_index-1]). Shifts all later words and video clips
 * right by duration_sec. Returns 0 on success, -1 on error. */
int wb_text_edit_insert_pause(wb_text_edit *te, int word_index, float duration_sec);

/* Dead air detection: find silent gaps between consecutive words longer than
 * threshold_sec and remove them (ripple the later words/clips left). Returns
 * the number of gaps removed, or -1 on error. */
int wb_text_edit_dead_air(wb_text_edit *te, float threshold_sec);

/* Filler word removal: detect "um", "uh", "like", "you know" and remove them
 * from the transcript AND cut their time span from the video clips. Returns
 * the number of filler words removed, or -1 on error. */
int wb_text_edit_um_ah_remove(wb_text_edit *te);

/* Get the current word count of the transcript. */
int wb_text_edit_get_word_count(const wb_text_edit *te);

/* Get the total duration of the transcript in seconds (last word's end). */
double wb_text_edit_get_total_duration(const wb_text_edit *te);

#ifdef __cplusplus
}
#endif
#endif /* WUBUS_WBUS_TEXT_EDIT_H */