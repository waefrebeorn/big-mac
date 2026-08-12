#ifndef WUBUS_WB_RECORDER_H
#define WUBUS_WB_RECORDER_H

/* Internal: MIDI record-into-clip capture. Owned by wb_engine (one recorder
 * per armed track/clip). The MIDI callback pushes RT-safe; the render thread
 * flushes into the authored clip.
 */

#include "wbus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wb_recorder wb_recorder;

wb_recorder *wb_recorder_create(wb_track *tr, int clip_idx);
void  wb_recorder_destroy(wb_recorder *r);
void  wb_recorder_set_overdub(wb_recorder *r, int overdub);

/* RT-safe: called from the MIDI thread (or wb_engine_note). Returns 0 ok. */
int   wb_recorder_midi_event(wb_recorder *r, int pitch, int vel, double song_pos);

/* Non-RT: called once per render block. Returns # of note-offs finalized. */
int   wb_recorder_flush(wb_recorder *r, double song_pos_end);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WB_RECORDER_H */
