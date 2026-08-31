/* wb_session_view.c — Ableton-style session view clip launcher.
 * Pure C11, zero third-party. Manages a 2D grid of clip slots with
 * launch modes (trigger/gate/toggle), quantize, scene launch, and
 * arrangement recording.
 *
 * The module is self-contained: slot state lives in internal parallel
 * arrays (not in wb_session, which stays layout-stable). The session
 * pointer is used for context only.
 */

#include <stdlib.h>
#include <string.h>
#include "wbus.h"

#define WB_SV_MAX_TRACKS 128
#define WB_SV_MAX_SCENES 256

/* Slot state flags. */
#define WB_SLOT_EXISTS   0x01
#define WB_SLOT_PLAYING  0x02
#define WB_SLOT_STOPPING 0x08

typedef struct {
    int      clip_ref;    /* clip index on track, -1 = empty */
    uint8_t  state;       /* WB_SLOT_* bitmask */
    uint8_t  launch_mode; /* WB_LAUNCH_* */
    uint8_t  quantize;    /* WB_QUANT_* */
    uint8_t  reserved;
} wb_session_slot;

struct wb_session_view {
    wb_session *session;
    wb_session_slot *slots; /* [track * WB_SV_MAX_SCENES + scene] */
    int playing_scene[WB_SV_MAX_TRACKS];       /* which scene is playing per track */
    uint8_t track_launch_mode[WB_SV_MAX_TRACKS];
    uint8_t track_quantize[WB_SV_MAX_TRACKS];
    int record_enabled;
    wb_arrangement_entry *arr_log;
    uint32_t arr_log_count;
    uint32_t arr_log_cap;
    double session_time;
};

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static wb_session_slot *slot_ptr(wb_session_view *sv, int track, int scene) {
    if (!sv || !sv->slots) return NULL;
    if (track < 0 || track >= WB_SV_MAX_TRACKS) return NULL;
    if (scene < 0 || scene >= WB_SV_MAX_SCENES) return NULL;
    return &sv->slots[track * (int)WB_SV_MAX_SCENES + scene];
}

static wb_session_slot *slot_ptr_c(const wb_session_view *sv, int track, int scene) {
    if (!sv || !sv->slots) return NULL;
    if (track < 0 || track >= WB_SV_MAX_TRACKS) return NULL;
    if (scene < 0 || scene >= WB_SV_MAX_SCENES) return NULL;
    return (wb_session_slot *)&sv->slots[track * (int)WB_SV_MAX_SCENES + scene];
}

static int log_arr_entry(wb_session_view *sv, int track, int clip_ref, int active) {
    if (sv->arr_log_count >= sv->arr_log_cap) {
        uint32_t new_cap = sv->arr_log_cap ? sv->arr_log_cap * 2 : 256;
        wb_arrangement_entry *new_log = (wb_arrangement_entry *)realloc(
            sv->arr_log, new_cap * sizeof(wb_arrangement_entry));
        if (!new_log) return -1;
        sv->arr_log = new_log;
        sv->arr_log_cap = new_cap;
    }
    wb_arrangement_entry *e = &sv->arr_log[sv->arr_log_count++];
    e->time_samples = sv->session_time;
    e->track = track;
    e->clip_ref = clip_ref;
    e->active = active;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Default view (for wb_session* convenience API)                      */
/* ------------------------------------------------------------------ */

static wb_session_view *g_default_view = NULL;
static wb_session *g_default_session = NULL;

static wb_session_view *get_default(wb_session *session) {
    if (!g_default_view || g_default_session != session) {
        if (g_default_view) wb_session_view_destroy(g_default_view);
        g_default_view = wb_session_view_create(session);
        g_default_session = session;
    }
    return g_default_view;
}

/* ------------------------------------------------------------------ */
/* Creation / destruction                                              */
/* ------------------------------------------------------------------ */

wb_session_view *wb_session_view_create(wb_session *session) {
    wb_session_view *sv = (wb_session_view *)calloc(1, sizeof(wb_session_view));
    if (!sv) return NULL;
    sv->session = session;
    size_t total = (size_t)WB_SV_MAX_TRACKS * (size_t)WB_SV_MAX_SCENES;
    sv->slots = (wb_session_slot *)calloc(total, sizeof(wb_session_slot));
    if (!sv->slots) { free(sv); return NULL; }
    for (size_t i = 0; i < total; i++) {
        sv->slots[i].clip_ref = -1;
        sv->slots[i].state = 0;
        sv->slots[i].launch_mode = WB_LAUNCH_TRIGGER;
        sv->slots[i].quantize = WB_QUANT_FREE;
    }
    for (int t = 0; t < WB_SV_MAX_TRACKS; t++) {
        sv->playing_scene[t] = -1;
        sv->track_launch_mode[t] = WB_LAUNCH_TRIGGER;
        sv->track_quantize[t] = WB_QUANT_FREE;
    }
    return sv;
}

void wb_session_view_destroy(wb_session_view *sv) {
    if (!sv) return;
    free(sv->slots);
    free(sv->arr_log);
    free(sv);
}

/* ------------------------------------------------------------------ */
/* Slot management                                                     */
/* ------------------------------------------------------------------ */

int wb_session_create_slot(wb_session *session, int track, int scene) {
    wb_session_view *sv = get_default(session);
    return wb_session_view_create_slot(sv, track, scene);
}

int wb_session_view_create_slot(wb_session_view *sv, int track, int scene) {
    wb_session_slot *slot = slot_ptr(sv, track, scene);
    if (!slot) return -1;
    slot->state |= WB_SLOT_EXISTS;
    slot->launch_mode = sv->track_launch_mode[track];
    slot->quantize = sv->track_quantize[track];
    return 0;
}

/* ------------------------------------------------------------------ */
/* Clip launch / stop                                                  */
/* ------------------------------------------------------------------ */

int wb_session_launch_clip(wb_session *session, int track, int scene) {
    wb_session_view *sv = get_default(session);
    return wb_session_view_launch_clip(sv, track, scene);
}

int wb_session_view_launch_clip(wb_session_view *sv, int track, int scene) {
    wb_session_slot *slot = slot_ptr(sv, track, scene);
    if (!slot || !(slot->state & WB_SLOT_EXISTS)) return -1;
    if (slot->clip_ref < 0) return -1;

    switch (slot->launch_mode) {
    case WB_LAUNCH_TRIGGER:
        slot->state |= WB_SLOT_PLAYING;
        slot->state &= ~(uint8_t)WB_SLOT_STOPPING;
        sv->playing_scene[track] = scene;
        break;
    case WB_LAUNCH_GATE:
        slot->state |= WB_SLOT_PLAYING;
        slot->state &= ~(uint8_t)WB_SLOT_STOPPING;
        sv->playing_scene[track] = scene;
        break;
    case WB_LAUNCH_TOGGLE:
        if (slot->state & WB_SLOT_PLAYING) {
            slot->state &= ~(uint8_t)WB_SLOT_PLAYING;
            slot->state |= WB_SLOT_STOPPING;
            if (sv->playing_scene[track] == scene)
                sv->playing_scene[track] = -1;
        } else {
            slot->state |= WB_SLOT_PLAYING;
            slot->state &= ~(uint8_t)WB_SLOT_STOPPING;
            sv->playing_scene[track] = scene;
        }
        break;
    default:
        return -1;
    }

    if (sv->record_enabled)
        log_arr_entry(sv, track, slot->clip_ref,
                      (slot->state & WB_SLOT_PLAYING) ? 1 : 0);
    return 0;
}

int wb_session_stop_clip(wb_session *session, int track) {
    wb_session_view *sv = get_default(session);
    return wb_session_view_stop_clip(sv, track);
}

int wb_session_view_stop_clip(wb_session_view *sv, int track) {
    if (!sv || track < 0 || track >= WB_SV_MAX_TRACKS) return -1;
    int scene = sv->playing_scene[track];
    if (scene < 0) return -1;
    wb_session_slot *slot = slot_ptr(sv, track, scene);
    if (!slot) return -1;
    slot->state &= ~(uint8_t)WB_SLOT_PLAYING;
    slot->state |= WB_SLOT_STOPPING;
    int ref = slot->clip_ref;
    sv->playing_scene[track] = -1;
    if (sv->record_enabled)
        log_arr_entry(sv, track, ref, 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Scene launch / stop all                                             */
/* ------------------------------------------------------------------ */

int wb_session_launch_scene(wb_session *session, int scene) {
    wb_session_view *sv = get_default(session);
    return wb_session_view_launch_scene(sv, scene);
}

int wb_session_view_launch_scene(wb_session_view *sv, int scene) {
    if (!sv || scene < 0 || scene >= WB_SV_MAX_SCENES) return -1;
    int launched = 0;
    for (int t = 0; t < WB_SV_MAX_TRACKS; t++) {
        wb_session_slot *slot = slot_ptr(sv, t, scene);
        if (!slot || !(slot->state & WB_SLOT_EXISTS)) continue;
        if (slot->clip_ref < 0) continue;
        if (sv->playing_scene[t] >= 0 && sv->playing_scene[t] != scene)
            wb_session_view_stop_clip(sv, t);
        if (wb_session_view_launch_clip(sv, t, scene) == 0)
            launched++;
    }
    return launched > 0 ? 0 : -1;
}

int wb_session_stop_all(wb_session *session) {
    wb_session_view *sv = get_default(session);
    return wb_session_view_stop_all(sv);
}

int wb_session_view_stop_all(wb_session_view *sv) {
    if (!sv) return -1;
    for (int t = 0; t < WB_SV_MAX_TRACKS; t++) {
        if (sv->playing_scene[t] >= 0)
            wb_session_view_stop_clip(sv, t);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Launch mode / quantize setters                                      */
/* ------------------------------------------------------------------ */

int wb_session_set_clip_launch_mode(wb_session *session, int track, int mode) {
    wb_session_view *sv = get_default(session);
    return wb_session_view_set_clip_launch_mode(sv, track, mode);
}

int wb_session_view_set_clip_launch_mode(wb_session_view *sv, int track, int mode) {
    if (!sv || track < 0 || track >= WB_SV_MAX_TRACKS) return -1;
    if (mode < WB_LAUNCH_TRIGGER || mode > WB_LAUNCH_TOGGLE) return -1;
    sv->track_launch_mode[track] = (uint8_t)mode;
    for (int s = 0; s < WB_SV_MAX_SCENES; s++) {
        wb_session_slot *slot = slot_ptr(sv, track, s);
        if (slot && (slot->state & WB_SLOT_EXISTS))
            slot->launch_mode = (uint8_t)mode;
    }
    return 0;
}

int wb_session_set_clip_quantize(wb_session *session, int track, int quantize) {
    wb_session_view *sv = get_default(session);
    return wb_session_view_set_clip_quantize(sv, track, quantize);
}

int wb_session_view_set_clip_quantize(wb_session_view *sv, int track, int quantize) {
    if (!sv || track < 0 || track >= WB_SV_MAX_TRACKS) return -1;
    if (quantize < WB_QUANT_FREE || quantize > WB_QUANT_1_16) return -1;
    sv->track_quantize[track] = (uint8_t)quantize;
    for (int s = 0; s < WB_SV_MAX_SCENES; s++) {
        wb_session_slot *slot = slot_ptr(sv, track, s);
        if (slot && (slot->state & WB_SLOT_EXISTS))
            slot->quantize = (uint8_t)quantize;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Query                                                               */
/* ------------------------------------------------------------------ */

int wb_session_get_playing_clip(const wb_session *session, int track) {
    wb_session_view *sv = get_default((wb_session *)session);
    if (!sv || track < 0 || track >= WB_SV_MAX_TRACKS) return -1;
    int scene = sv->playing_scene[track];
    if (scene < 0) return -1;
    wb_session_slot *slot = slot_ptr_c(sv, track, scene);
    if (!slot) return -1;
    return slot->clip_ref;
}

int wb_session_view_get_playing_scene(const wb_session_view *sv, int track) {
    if (!sv || track < 0 || track >= WB_SV_MAX_TRACKS) return -1;
    return sv->playing_scene[track];
}

int wb_session_get_playing_scene(const wb_session *session) {
    wb_session_view *sv = get_default((wb_session *)session);
    if (!sv) return -1;
    for (int s = 0; s < WB_SV_MAX_SCENES; s++) {
        for (int t = 0; t < WB_SV_MAX_TRACKS; t++) {
            if (sv->playing_scene[t] == s) return s;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Arrangement recording                                              */
/* ------------------------------------------------------------------ */

int wb_session_record_to_arrangement(wb_session *session, int enable) {
    wb_session_view *sv = get_default(session);
    return wb_session_view_record_to_arrangement(sv, enable);
}

int wb_session_view_record_to_arrangement(wb_session_view *sv, int enable) {
    if (!sv) return -1;
    sv->record_enabled = enable ? 1 : 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Direct-access API (wb_session_view* handle)                         */
/* ------------------------------------------------------------------ */

uint32_t wb_session_view_arr_log_count(const wb_session_view *sv) {
    return sv ? sv->arr_log_count : 0;
}

const wb_arrangement_entry *wb_session_view_arr_log(const wb_session_view *sv, uint32_t idx) {
    if (!sv || idx >= sv->arr_log_count) return NULL;
    return &sv->arr_log[idx];
}

int wb_session_view_set_slot_clip(wb_session_view *sv, int track, int scene, int clip_ref) {
    wb_session_slot *slot = slot_ptr(sv, track, scene);
    if (!slot || !(slot->state & WB_SLOT_EXISTS)) return -1;
    slot->clip_ref = clip_ref;
    return 0;
}

int wb_session_view_get_slot_clip(const wb_session_view *sv, int track, int scene) {
    wb_session_slot *slot = slot_ptr_c(sv, track, scene);
    if (!slot) return -1;
    return slot->clip_ref;
}

int wb_session_view_slot_exists(const wb_session_view *sv, int track, int scene) {
    wb_session_slot *slot = slot_ptr_c(sv, track, scene);
    if (!slot) return 0;
    return (slot->state & WB_SLOT_EXISTS) ? 1 : 0;
}

int wb_session_view_slot_playing(const wb_session_view *sv, int track, int scene) {
    wb_session_slot *slot = slot_ptr_c(sv, track, scene);
    if (!slot) return 0;
    return (slot->state & WB_SLOT_PLAYING) ? 1 : 0;
}

int wb_session_view_get_slot_launch_mode(const wb_session_view *sv, int track, int scene) {
    wb_session_slot *slot = slot_ptr_c(sv, track, scene);
    if (!slot) return -1;
    return (int)slot->launch_mode;
}

int wb_session_view_get_slot_quantize(const wb_session_view *sv, int track, int scene) {
    wb_session_slot *slot = slot_ptr_c(sv, track, scene);
    if (!slot) return -1;
    return (int)slot->quantize;
}

void wb_session_view_advance_time(wb_session_view *sv, double delta_samples) {
    if (sv) sv->session_time += delta_samples;
}