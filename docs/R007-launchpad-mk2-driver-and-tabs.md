# R007 — Launchpad Mk2 driver (C11, owned) + tabbed piano-roll UI

**Builds on:** R006 (7-hop convergence: ONE owned surface, scale-locked,
color-coded, no menu-diving).
**Hard constraints (doctrine):** C11 only, zero third-party libs, no stubs,
verified by building + a headless self-test. Mk2 is class-compliant → CoreMIDI
already enumerates it; we only add the Mk2 protocol layer on top of the
existing `wb_midi_coremidi.c` output port.

---

## 1. Driver corrections (the existing code is WRONG for Mk2)

Current `wb_launchpad_note()` returns `row*16+col` and `wb_launchpad_led()`
sends velocity-as-color. That is the **classic Launchpad** map. Mk2 is different:

| Item | Classic (current, WRONG for Mk2) | Mk2 (correct) |
|------|----------------------------------|---------------|
| Grid note | `row*16 + col` | `11 + col + row*10` |
| Color | velocity byte (palette) | RGB SysEx, each channel 0–63 |
| Top row | part of grid | notes `91..98` (separate) |
| Clear | note-on vel 0 | SysEx all-off OR note-on vel 0 on each pad |

**New header (add to `wbus_midi.h`):**

```c
/* ---- Launchpad Mk2 (our own driver, C11, class-compliant) ---- */
#define WB_LP_MK2_COLS 8
#define WB_LP_MK2_ROWS 8

/* Grid (row,col) -> Mk2 MIDI note. Returns 11+col+row*10, or -1 if OOB. */
int  wb_lp_mk2_note(int row, int col);

/* Top-row button index (0..7) -> Mk2 MIDI note (91..98). */
int  wb_lp_mk2_top_note(int idx);

/* Set a pad to an RGB color (r,g,b each 0..63). Uses the Mk2 RGB SysEx:
 * F0 00 20 29 02 18 0B <note> <r> <g> <b> F7 */
int  wb_lp_mk2_led_rgb(wb_midi *m, int row, int col, uint8_t r, uint8_t g, uint8_t b);
int  wb_lp_mk2_top_rgb(wb_midi *m, int idx, uint8_t r, uint8_t g, uint8_t b);

/* Convenience: clear whole grid via RGB SysEx all-off (note 0x0E then 00s). */
int  wb_lp_mk2_clear(wb_midi *m);

/* A small named-palette so we encode STATE in color (see R006 §4): */
typedef enum {
    WB_LP_OFF=0, WB_LP_WHITE=1, WB_LP_GREEN=2, WB_LP_AMBER=3,
    WB_LP_BLUE=4, WB_LP_RED=5, WB_LP_DIM=6
} wb_lp_color;
/* wb_lp_mk2_led(m,row,col,WB_LP_GREEN) maps the enum to an (r,g,b) triple. */
int  wb_lp_mk2_led(wb_midi *m, int row, int col, wb_lp_color c);
```

**SysEx send:** add `wb_midi_send_sysex(wb_midi*, const uint8_t* data, int len)`
to the CoreMIDI output path (build an `MIDIPacketList` from a raw byte buffer —
same `MIDISend` call already used by `wb_midi_send`). This is the only new
CoreMIDI primitive needed.

**RGB palette triples (0..63):** white (63,63,63), green (0,63,0),
amber (63,45,0), blue (0,20,63), red (63,0,0), dim (8,8,8), off (0,0,0).

**Keep backward compat:** rename the OLD `wb_launchpad_*` to
`wb_launchpad_classic_*` so the existing demo callback still compiles, but the
DAW's Mk2 path uses `wb_lp_mk2_*`. (The current `midi_cb` in `wb_daw.c` calls
`wb_launchpad_led` — update it to Mk2 map.)

---

## 2. Tabbed / organized menu (the "one surface" from R006)

Add a tab bar to `wb_daw.c` under the transport (or as a left rail). Tabs:

| Tab | What it shows | Input |
|-----|---------------|-------|
| `KEYS`  | existing keyboard piano roll (mouse) | mouse clicks/drag |
| `PAD`   | Launchpad Mk2 piano roll: 8×8 grid mirrors the Mk2; same `wb_clip` | Mk2 pads + mouse |
| `STEP`  | step sequencer: rows=tracks(Drum/Scale/Chromatic), cols=steps | Mk2 + mouse |
| `SESSION` | clip launcher: cols=tracks, rows=scenes, lit/dim block | Mk2 + mouse |

**State to add to `app`:**
```c
int tab;            /* 0=KEYS 1=PAD 2=STEP 3=SESSION */
int scale_root;     /* 0..11 */
int scale_type;     /* 0=major 1=minor 2=dorian 3=chromatic ... */
```

**Tab switching keys:** `Tab` cycles, or `1/2/3/4` jump. `s` to set scale (after
R006 §3). The Mk2 top row (91–98) doubles as live tab shortcuts while in PAD/STEP.

**Why tabs, not menus:** R006 source 25 — extraneous load comes from physically
separated menus. A visible tab bar keeps the mode one key away and on-screen.

---

## 3. Scale lock (R006 §3, sources 13–17)

A scale = root + interval set. Helper:
```c
/* returns 1 if `note` is in the current scale, else 0 */
int wb_scale_contains(int scale_root, int scale_type, int note);
/* snap a raw MIDI note to the nearest in-scale note */
int wb_scale_snap(int scale_root, int scale_type, int note);
```
- In `KEYS` and `PAD`, out-of-scale notes render dim/off and are snapped on input.
- On the Mk2, light in-scale pads in the scale color, out-of-scale dim — a
  walking "piano" the user can't get wrong.
- This is the FL/Ableton "scale highlighting / scale device" feature, owned.

---

## 4. Color = state (R006 §4, sources 21, 25)

| State | Mk2 color | Meaning |
|-------|-----------|---------|
| Playhead column | WHITE | where transport is |
| Loop region | AMBER | loop on |
| Clip-on pad | GREEN | note/clip present |
| Selected | BLUE | currently editing |
| Out-of-scale | DIM | not playable in key |
| Record-arm | RED | armed |

Because Mk2 is RGB, we encode this directly on the hardware — the user reads
state from the grid, not a menu.

---

## 5. One editable object (R006 §1, sources 18,20,23)

Both `KEYS` and `PAD` edit the **same** `wb_clip->notes[]`. Clicking Mk2 pad
(row,col) → `wb_session_add_note()` on the selected track at the column's time;
the on-screen roll updates next frame. No second data model, no sync bug.

---

## 6. Verification plan (doctrine: no stubs, verify by running)

- `tests/test_launchpad_mk2.c`: asserts `wb_lp_mk2_note(0,0)==11`,
  `wb_lp_mk2_note(7,7)==88`, `wb_lp_mk2_top_note(0)==91`; asserts the RGB SysEx
  byte stream for a known (row,col,color) matches the expected 11-byte sequence;
  asserts `wb_scale_contains`/`wb_scale_snap` on major/minor.
- `make` clean build; `wb_selftest` still 147→+N green.
- `--screenshot` headless render of each tab (PAD/STEP/SESSION) shows a
  non-garbage grid; visually confirm scale colors + playhead.
- Real-device smoke (manual, on the iMac with the Mk2 plugged in): run a tiny
  `tools/wb_lp_test` that clears, lights a scale, and echoes pads. Log result.

---

## 7. Build order (heavy-first, like the user asked)

1. `wb_midi_send_sysex` + `wb_lp_mk2_*` in `wb_midi_coremidi.c` (driver core).
2. `wb_scale_*` helpers (pure, testable).
3. `tests/test_launchpad_mk2.c` (verify protocol bytes — the honest gate).
4. Tab bar + `app.tab` + `draw_pad/draw_step/draw_session` in `wb_daw.c`.
5. Wire Mk2 input callback to the active tab's clip edit.
6. Color-state mapping on both screen + Mk2.
7. Screenshot + selftest + (device smoke). Commit per-step, build green each.

---

## Next-cycle target (recursion contract)

After this lands: **performance latency** — how many ms from Mk2 pad-press to
audible note through CoreAudio, and whether the lock-free queue + RT callback
keep it <10 ms. That becomes hop #1 of the next 7-hop cycle.
