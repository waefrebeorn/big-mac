#ifndef WUBUS_WBUS_MIDI_H
#define WUBUS_WBUS_MIDI_H

/* Big Mac DAW — portable MIDI input layer.
 * Abstraction over platform MIDI (CoreMIDI on macOS; the platform's own C
 * API, same treatment as CoreAudio/SDL). Enables plugging in controllers:
 * Ableton Launchpad, M-Audio keys, drum pads, etc.
 *
 * Pattern mirrors wb_backend: portable interface + platform implementation.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wb_midi wb_midi;

/* A MIDI event received from a controller. */
typedef struct wb_midi_event {
    uint8_t status;   /* 0x80 noteoff, 0x90 noteon, 0xB0 CC, 0xE0 bend */
    uint8_t data1;    /* note/CC number */
    uint8_t data2;    /* velocity/value */
} wb_midi_event;

/* Enumerate available MIDI input devices.
 * Fills `names` with up to `max` device names (caller-provided buffers of
 * `buf_size` bytes). Returns the number found. */
int wb_midi_enumerate(char (*names)[64], int max, int *out_count);

/* Open a MIDI input device by exact name (e.g. "Launchpad MK2").
 * Returns a handle, or NULL on failure. `on_event` is called from a MIDI
 * thread for each incoming event; `userdata` is passed through. */
wb_midi *wb_midi_open(const char *name,
                      void (*on_event)(wb_midi_event ev, void *userdata),
                      void *userdata);

/* Open the first device whose name CONTAINS `substr` (case-insensitive).
 * Convenience for autodetect ("Launchpad", "M-Audio", "AKAI"...). */
wb_midi *wb_midi_open_contains(const char *substr,
                               void (*on_event)(wb_midi_event ev, void *userdata),
                               void *userdata);

void wb_midi_close(wb_midi *m);

/* Open the first MIDI output destination whose name CONTAINS `substr`
 * (case-insensitive). Uses the same client as the input. Returns 0 ok, -1
 * if no matching destination is found. Call before wb_midi_send. */
int wb_midi_open_output(wb_midi *m, const char *substr);

/* ---- MIDI output (controller LED / feedback / sysex) ------------------- */
/* Create a headless MIDI handle with NO device attached (for testing /
 * inspection only). All wb_midi_send / wb_midi_send_sysex calls will fail to
 * reach hardware but can be captured via wb_midi_capture(). Returns NULL on
 * alloc failure. Free with wb_midi_close(). */
wb_midi *wb_midi_create_null(void);

/* Enable a capture buffer: every sent byte (short + sysex) is appended here
 * so headless tests can assert exact wire bytes. Pass buf=NULL to disable. */
int wb_midi_capture(wb_midi *m, uint8_t *buf, int cap);
int wb_midi_capture_len(wb_midi *m);
/* Send a short (3-byte) MIDI message on the given output destination.
 * `status` is the raw status byte (e.g. 0x90 note-on), data1/data2 follow.
 * Returns 0 on success, -1 on error. Not RT-safe (CoreMIDI send). */
int wb_midi_send(wb_midi *m, uint8_t status, uint8_t data1, uint8_t data2);

/* Send a raw SysEx (or any) byte stream on the output destination. Used for
 * Launchpad Mk2 RGB LED control. `data` must be a complete message beginning
 * with 0xF0 and ending with 0xF7. Returns 0 on success, -1 on error. */
int wb_midi_send_sysex(wb_midi *m, const uint8_t *data, int len);

/* ---- Launchpad Mk2 (our own driver, C11, class-compliant) ------------- */
#define WB_LP_MK2_COLS 8
#define WB_LP_MK2_ROWS 8

/* Grid (row,col) -> Mk2 MIDI note. Mk2 layout is 11 + col + row*10
 * (NOT the classic Launchpad's row*16+col). Returns -1 if out of bounds. */
int  wb_lp_mk2_note(int row, int col);

/* Top-row button index (0..7) -> Mk2 MIDI note (91..98). */
int  wb_lp_mk2_top_note(int idx);

/* State colors we encode on the grid (R006 §4: color = meaning). */
typedef enum {
    WB_LP_OFF=0, WB_LP_WHITE=1, WB_LP_GREEN=2, WB_LP_AMBER=3,
    WB_LP_BLUE=4, WB_LP_RED=5, WB_LP_DIM=6, WB_LP_CYAN=7
} wb_lp_color;

/* Set a pad to a named state color (maps to an RGB triple, each 0..63). */
int  wb_lp_mk2_led(wb_midi *m, int row, int col, wb_lp_color c);
/* Set a pad / top button to an explicit RGB color (each channel 0..63). */
int  wb_lp_mk2_led_rgb(wb_midi *m, int row, int col, uint8_t r, uint8_t g, uint8_t b);
int  wb_lp_mk2_top_rgb(wb_midi *m, int idx, uint8_t r, uint8_t g, uint8_t b);
/* Clear the whole grid (RGB all-off SysEx). */
int  wb_lp_mk2_clear(wb_midi *m);
/* Map a named state color to its RGB triple (r,g,b each 0..63). */
void wb_lp_color_rgb(wb_lp_color c, uint8_t *r, uint8_t *g, uint8_t *b);

/* Scale helpers (R006 §3: scale lock, owned) ----------------------- */
/* scale_type: 0=major 1=natural minor 2=dorian 3=mixolydian 4=chromatic
 * (chromatic treats every note as in-scale). Returns 1 if `note` is in the
 * scale rooted at `scale_root` (0..11), else 0. */
int  wb_scale_contains(int scale_root, int scale_type, int note);
/* Snap a raw MIDI note to the nearest in-scale note (returns a MIDI note). */
int  wb_scale_snap(int scale_root, int scale_type, int note);
/* G81: chord tones from root + scale. mode: 0=off(none),1=triad,2=7th,3=9th.
 * Fills `out` with chord tones (root + extensions), returns count (0..8). */
int  wb_chord_tones(int scale_root, int scale_type, int mode, int out[8]);

/* ---- Launchpad LED feedback (classic LP — kept for backward compat) --- */
/* A Launchpad shows its 8x8 grid as MIDI notes; sending a note-on sets the
 * LED color (velocity = color on the classic Launchpad). These helpers map
 * grid (row,col) to note and send the color. row/col are 0..7.
 * NOTE: the Mk2 uses a DIFFERENT layout + RGB SysEx — use wb_lp_mk2_* for it. */
int wb_launchpad_classic_led(wb_midi *m, int row, int col, uint8_t color);
int wb_launchpad_classic_note(int row, int col);      /* pure grid→note mapping */
int wb_launchpad_classic_clear(wb_midi *m);   /* turn all grid LEDs off */

/* Mk2 inverse: MIDI note → grid (row,col). Returns 0 on success, -1 if note
 * is not a Mk2 grid or top-row note. Fills *row,*col (0..7). */
int wb_lp_mk2_row_col_from_note(int note, int *row, int *col);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_MIDI_H */
