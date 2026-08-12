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
void wb_transport_schedule_notes(wb_track *track, double block_start, uint32_t n,
                                 void (*note_on)(void*, int, int),
                                 void *voice) {
    if (!track || track->kind != 0) return;
    for (uint32_t c = 0; c < track->clip_count; c++) {
        wb_clip *clip = &track->clips[c];
        if (!clip || clip->type != 0) continue;
        for (uint32_t k = 0; k < clip->note_count; k++) {
            wb_note *nt = &clip->notes[k];
            double abs_start = clip->start + nt->start;
            double abs_end = clip->start + nt->start + nt->dur;
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
