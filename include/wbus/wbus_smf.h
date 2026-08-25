/* wbus_smf.h — R074 hop 115 (G-SF061): Standard MIDI File loader.
 * Parses type-0/1 .mid into wb_note arrays (seconds). Pure C11.
 */
#ifndef WUBUS_SMF_H
#define WUBUS_SMF_H

#include <stdint.h>
#include <stddef.h>
#include "wbus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wb_smf wb_smf;

/* Load and parse a .mid file. Returns NULL on any error. */
wb_smf *wb_smf_load(const char *path);

/* Number of note events parsed (note-on/off paired). */
int      wb_smf_note_count(const wb_smf *s);

/* Parsed notes, ordered by track then time (seconds-based start/dur). */
const wb_note *wb_smf_notes(const wb_smf *s);

/* Length of the longest note (seconds). */
double   wb_smf_duration(const wb_smf *s);

/* Tempo from the first set-tempo meta event (default 120). */
/* double wb_smf_bpm — folded into load; see duration. */

void     wb_smf_free(wb_smf *s);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_SMF_H */
