#ifndef WUBUS_WBUS_PLUGIN_H
#define WUBUS_WBUS_PLUGIN_H

/* Big Mac DAW — "wbus" plugin ABI.
 * A C11 plugin contract modeled on CLAP's proven design: a plugin exposes
 * opaque descriptor + instance, processes blocks of audio in place, and
 * exchanges events through a single per-process event queue. Built-in DSP
 * implements this ABI; the design leaves the door open for external plugins.
 *
 * The realtime contract: process() must be allocation-free, lock-free, and
 * fast. Everything else (param queries, latency report, state) is non-RT.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wb_plugin wb_plugin;
typedef struct wb_plugin_host wb_plugin_host;

typedef enum {
    WB_EVENT_NOTE_ON = 0,   /* note, note_id */
    WB_EVENT_NOTE_OFF,      /* note, note_id */
    WB_EVENT_PARAM,         /* param_id, value (0..1) */
    WB_EVENT_CONTROL,       /* cc: control_id, value 0..127 */
    WB_EVENT_PITCHBEND,     /* value -1..1 */
    WB_EVENT_TRANSPORT,     /* transport bits + sample pos */
} wb_event_type;

typedef struct wb_event {
    uint32_t type;
    uint32_t sample_offset; /* within the block */
    int32_t  note;          /* MIDI note */
    int32_t  note_id;       /* for note-on/note-off pairing */
    uint32_t param_id;
    float    value;         /* 0..1 for params, -1..1 for pitchbend, 0..127 for cc */
    uint64_t transport_ts;  /* absolute sample position (transport event) */
    int32_t  transport_playing;
} wb_event;

/* one block of audio passed to process() */
typedef struct wb_audio_block {
    uint32_t  frames;
    uint8_t   channels;
    float   **inputs;   /* per-channel input arrays (already allocated) */
    float   **outputs;  /* per-channel output arrays */
    const wb_event *events;  /* events for this block */
    uint32_t event_count;
    uint32_t sample_rate;
} wb_audio_block;

/* parameter description (for UI automation / control) */
typedef struct wb_param {
    uint32_t id;
    char     name[32];
    float    min, max, default_value;
    char     units[16];
} wb_param;

/* ---- the plugin interface -------------------------------------------- */
typedef struct wb_plugin_vtable {
    const char *(*id)(const wb_plugin *p);
    const char *(*name)(const wb_plugin *p);
    uint32_t    (*param_count)(const wb_plugin *p);
    void        (*param_info)(const wb_plugin *p, uint32_t i, wb_param *out);
    void       *(*create)(const wb_plugin *p, uint32_t sample_rate);
    void        (*destroy)(const wb_plugin *p, void *inst);
    /* non-RT: get/set a parameter */
    float       (*get_param)(const wb_plugin *p, void *inst, uint32_t param_id);
    void        (*set_param)(const wb_plugin *p, void *inst, uint32_t param_id, float value);
    /* RT: process one block. Return 0 on success. */
    int         (*process)(const wb_plugin *p, void *inst, wb_audio_block *block);
} wb_plugin_vtable;

typedef struct wb_plugin {
    const wb_plugin_vtable *vt;
} wb_plugin;

/* ---- host interface available to plugins (minimal for now) ------------ */
typedef struct wb_plugin_host {
    void *userdata;
} wb_plugin_host;

/* ---- registry --------------------------------------------------------- */
/* Register a built-in plugin type. Returns 1 on success. */
int wb_dsp_register(const wb_plugin *p);
/* Look up a registered plugin by id string. */
const wb_plugin *wb_dsp_find(const char *id);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_PLUGIN_H */
