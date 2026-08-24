/* wbus_clip_edit.h — R043 (G1/G2/G8/G9): direct-manipulation clip-edit state.
 *
 * Self-contained, opaque side-table holding per-clip edit handles
 * (fade in/out, content offset, pre-fade) WITHOUT bloating the core wb_clip
 * struct. This keeps wb_clip layout-stable (critical: it is memcpy'd and
 * serialized) and honors the "every module self-contained, no god header"
 * doctrine. The engine render path and the UI both consult this table by
 * (track, clip) index; missing entries are treated as neutral (no fade,
 * zero offset) so the table is purely additive.
 *
 * C11 only. No <string.h> abuse, no UI/render knowledge. */

#ifndef WBUS_CLIP_EDIT_H
#define WBUS_CLIP_EDIT_H

#ifdef __cplusplus
extern "C" {
#endif

/* One clip's edit handles. All fades in SECONDS; offset/length in SAMPLES. */
typedef struct wb_clip_edit {
    float  fade_in;        /* linear fade-in ramp at clip head (Ableton/Logic) */
    float  fade_out;       /* linear fade-out ramp at clip tail */
    double start_in_source;/* sample offset into the owned buffer where playback
                              begins (Bitwig "content slide" inside the clip) */
    float  pre_fade_in;    /* G9: fade in material BEFORE the edit point while
                              keeping the clip's true start at full amp (0=off) */
    int    loop;           /* G3: 1 = clip repeats its loop region */
    double loop_len;       /* G3: loop region length in SAMPLES (0 = full clip) */
    int    curve;          /* G64: fade/crossfade curve shape:
                              0 = linear (equal-gain, classic), 1 = equal-power
                              (sqrt gain law, constant loudness when paired with
                              a complementary fade), 2 = smoothstep (S-curve,
                              3t^2-2t^3) */
    int    color;          /* G13: clip color slot 0..7 (0 = default palette);
                              Ableton-style in-grid labeling. Side-table only —
                              wb_clip layout is frozen (.wbus/memcpy). */
} wb_clip_edit;

/* Opaque table. Keyed by (track, clip); grows as needed. */
typedef struct wb_clip_edit_table wb_clip_edit_table;

/* Create an empty edit table. Returns NULL on alloc failure. */
wb_clip_edit_table *wb_clip_edit_create(void);

/* Free the table and all entries. */
void wb_clip_edit_destroy(wb_clip_edit_table *t);

/* Get the (mutable) edit state for clip (track, clip). Creates a neutral
 * entry on first access. Never returns NULL for a valid table. */
wb_clip_edit *wb_clip_edit_get(wb_clip_edit_table *t, int track, int clip);

/* Clear the edit state for one clip back to neutral (no fade, zero offset). */
void wb_clip_edit_clear(wb_clip_edit_table *t, int track, int clip);

/* G14: migrate a clip's edit entry from (src_track, src_clip) to
 * (dst_track, dst_clip) after the clip moved in the session model (e.g.
 * drag between tracks). The source entry is cleared; fades/offset/curve
 * travel with the clip. Missing entries are created as needed. */
void wb_clip_edit_move(wb_clip_edit_table *t, int src_track, int src_clip,
                       int dst_track, int dst_clip);

/* Multiplicative fade envelope at sample position `f` (samples from clip
 * start) within a clip of `length` samples. Returns 1.0 when no fade applies.
 * Pure — safe to call from the realtime render callback. */
float wb_clip_edit_env(const wb_clip_edit *e, double f, double length,
                       double sample_rate);

#ifdef __cplusplus
}
#endif
#endif /* WBUS_CLIP_EDIT_H */
