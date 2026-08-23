/* wb_transport.c — sample-accurate timeline renderer.
 * Reads the session's clips and schedules note-on/off events into the
 * instrument voices at exact sample positions. This is where the "song"
 * actually gets played from MIDI clips.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "wbus.h"

/* Schedule notes that fall within [block_start, block_start+n) for a track's
 * MIDI clips into the track's instrument voice. Called from the render path.
 *
 * Implemented as a thin helper; the engine calls it per block per track. */
/* G89: swing — delay in samples for a note at timeline position 'pos'.
 * Roger Linn MPC spec: every OTHER 16th note (odd step index) is delayed
 * by swing * sixteenth; even steps play on the grid. */
double wb_swing_offset(double bpm, double swing, double pos) {
    if (swing <= 0.0 || bpm <= 0.0) return 0.0;
    if (swing > 0.6) swing = 0.6;
    double sixteenth = (60.0 / bpm / 4.0) * (double)WB_SAMPLE_RATE;
    if (sixteenth <= 0.0) return 0.0;
    long step = (long)(pos / sixteenth + 1e-9);
    if (step < 0) step = 0;
    if (step % 2 == 1) return swing * sixteenth;
    return 0.0;
}

/* Swing-aware variant: bpm + session swing are passed in by the engine
 * (which reads them from the SESSION — the UI's source of truth). */
void wb_transport_schedule_notes_sw(wb_track *track, double block_start, uint32_t n,
                                    void (*note_on)(void*, int, int),
                                    void *voice, double bpm, double swing) {
    if (!track || track->kind != 0) return;
    for (uint32_t c = 0; c < track->clip_count; c++) {
        wb_clip *clip = &track->clips[c];
        if (!clip || clip->type != 0) continue;
        /* R030: take-lanes — only the active lane is heard (comping) */
        if (clip->lane != track->active_lane) continue;
        for (uint32_t k = 0; k < clip->note_count; k++) {
            wb_note *nt = &clip->notes[k];
            double off = wb_swing_offset(bpm, swing, clip->start + nt->start);
            double abs_start = clip->start + nt->start + off;
            double abs_end = clip->start + nt->start + nt->dur + off;
            /* note-on inside this block */
            if (abs_start >= block_start && abs_start < block_start + n) {
                if (note_on && voice)
                    note_on(voice, nt->pitch, nt->vel);
            }
            /* note-off: handled by duration; for our synth, we send a vel-0
             * note-off when the note ends inside this block. */
            if (abs_end >= block_start && abs_end < block_start + n) {
                if (note_on && voice)
                    note_on(voice, nt->pitch, 0);
            }
        }
    }
}

/* Straight (no swing) scheduler — kept for compatibility; delegates with
 * swing = 0 so behavior is unchanged for existing callers. */
void wb_transport_schedule_notes(wb_track *track, double block_start, uint32_t n,
                                 void (*note_on)(void*, int, int),
                                 void *voice) {
    wb_transport_schedule_notes_sw(track, block_start, n, note_on, voice, 120.0, 0.0);
}

/* R037: schedule notes for a LAUNCHED clip, played from its own looping
 * clock (launch_pos) independent of the arrangement transport. The clip loops
 * over its length; notes whose clip-local time falls in the current window
 * [launch_pos, launch_pos+n) — including the wrapped copy when the window
 * straddles the loop point — are triggered. */
void wb_transport_schedule_launched(wb_clip *clip, double launch_pos, double len,
                                    uint32_t n,
                                    void (*note_on)(void*, int, int),
                                    void *voice) {
    if (!clip || clip->type != 0 || len <= 0) return;
    for (uint32_t k = 0; k < clip->note_count; k++) {
        wb_note *nt = &clip->notes[k];
        double ns = fmod(nt->start, len);   /* clip-local, looped */
        if (ns < 0) ns += len;
        double ne = ns + nt->dur;
        /* check both the primary window and the wrapped window (lp-len..) */
        for (int w = 0; w < 2; w++) {
            double base = (w == 0) ? launch_pos : launch_pos - len;
            if (ns >= base && ns < base + n) {
                if (note_on && voice) note_on(voice, nt->pitch, nt->vel);
            }
            if (ne >= base && ne < base + n) {
                if (note_on && voice) note_on(voice, nt->pitch, 0);
            }
        }
    }
}
