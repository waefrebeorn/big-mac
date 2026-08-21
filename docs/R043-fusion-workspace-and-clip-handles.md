# R043 — Fusion-style Workspace Ribbon + Clip-Edit Handle Research

**Date:** 2026-08-21 · **Status:** implemented + verified (all gates green)
**Combos:** DAW (AUDIO) + NLE (VIDEO) + FUSION + 3D-CGI (future) + AGI (future)
**Directive (user):** "coherence with DaVinci Resolve Fusion-style bottom menu tabbing
to allow more flexibility easy to switch audio and video — this is a combo daw and
video workstation … C11 porting … fix the broken code … no monolithic files … opaque
structs + minimal includes + C11 only … no god headers … keep every module
self-contained."

---

## What shipped this turn (verified by real gates + screenshots)

### 1. Deep R042 video-engine crash — ROOT-CAUSED and FIXED
Two distinct broken-code surfaces were found and fixed:

- **R042-A (realtime use-after-free):** `wb_engine_set_session()` freed `e->rtracks`
  WITHOUT holding `process_lock`, while the CoreAudio render callback reads it under
  a *trylock*. Importing a `type==2` video clip while the transport was live →
  non-deterministic crash (the "deep R042 video-clip crash"). Fixed by serializing the
  entire destroy+rebuild under `process_lock`: render observes either the fully-built
  OLD array or the fully-built NEW one. Launch-state for removed tracks is also cleared.
- **R042-B (duration resolution gap):** `wb_session_add_video_clip()` left `duration=-1`
  (R041 had stopped probing it to avoid the libavformat decoder crash). That broke
  `s->length` growth → `wb_video_export`'s audio render failed (`s->length>0`
  required), and split/EDL/FCPXML/agent span math got garbage. Fixed by resolving
  duration via the **ffprobe SHELL helper** (`wb_video_proxy_duration`, child-process
  isolated — NOT the crashing in-process decoder). Cascades to fix 3 previously-failing
  gates.

### 2. Fusion-style workspace ribbon (R043) — self-contained C11 module
- New module `src/wb_workspace.c` + `include/wbus/wbus_workspace.h` (opaque struct,
  C11, minimal includes, no god header, no DAW/UI knowledge). 5 tiers:
  `AUDIO / VIDEO / FUSION / 3D-CGI / AGI`. Each tier = one active workspace; the
  engine/render path queries `wb_workspace_audio_active()` etc.
- `3D-CGI` and `AGI` start LOCKED (capability not yet present); press **`W`** to
  unlock/relock them (proves the combo architecture is wireable end-to-end).
- UI (`tools/wb_daw.c`): bottom ribbon drawn + hit-tested from ONE source of truth
  (the button registry). Clicking a tier switches the active workspace AND flips the
  top tab-band to that tier's home view (AUDIO→ARRANGE, VIDEO→MEDIA, FUSION→EDIT,
  etc.). Change routed through `ws_on_change` callback.
- New gate `test_workspace` (23 checks, 0 failures).

### 3. Verification (real output, not assumptions)
- All gates green: selftest **216/0**, video **28/0**, agent **17/0**,
  e2e-export **E2E_EXPORT_OK**, compositor **72/0**, workspace **23/0**,
  voice_polish 15, captions 14, transcript 18, ofx 13, launchpad_mk2 59, tts 14,
  hpss 7, voice_isolate 8. `wb_render --demo` → AUDIO PRESENT.
- Screenshots of AUDIO (view 0) and VIDEO (view 4) both render the bottom ribbon with
  AUDIO/VIDEO/FUSION/3D-CGI/AGI tiers and a `WORKSPACE: AUDIO` readout — crash-free.

---

## Research loop (recursive 7-hop, TEXT-ONLY subagent) — clip-handle + tier UX

18 web_search queries across 20+ domains (ableton.com, bitwig.com, avid.com,
apple.com, presonus.com, blackmagicdesign.com, reaper.blog/reapertips, macprovideo,
pixflow, steinberg, uaudio, protoolstraining, tourbox, + reddit/facebook/forum threads).
Key converging findings:

1. **Ableton** — fade handles live at the *edges* of audio clips (visible when the
   track lane is tall enough); every clip has a fade by default. Clip edges drag to
   trim (anchor on the opposite edge). Source: ableton.com/en/manual/arrangement-view.
2. **Studio One** — "Audio Events" have edge handles; clip-gain edits shift the event;
   per-event inspector. Source: presonus.com support + S1 v-history.
3. **Logic Pro** — region fade handles appear at borders; Marquee-delete auto-adds
   fades; Drag mode controls handling. Source: apple.com Logic docs + groups threads.
4. **Bitwig** — clip right/left edge drag scales content; ALT+drag scales multiple
   clips; slide content by mousing the top half of the waveform. Source: bitwig.com
   user guide (clips chapter).
5. **Pro Tools** — clip gain line + trim handles; convert clip-gain ↔ volume
   automation; Touch/Latch automation modes. Source: protoolstraining + avid.
6. **Reaper** — item fade in/out handles, loop, fade-snap-to-grid; rich themes with
   fader-cap value display. Source: reapertips + reaper.blog.
7. **DaVinci Resolve** — divided into **pages**, each a dedicated workspace/toolset
   (MEDIA/CUT/EDIT/COLOR/FUSION/FAIRIGHT/DELIVER). Fairlight is the audio page inside
   the *same* app sharing the timeline. "Nodes Are Easier Than Layers" — the Fusion
   page's MediaIn node represents the clip from the edit timeline. Source:
   blackmagicdesign.com/products/davinciresolve (+ reddit fairlight→timeline moves).
8. **Mixer fader feedback** — DAWs show a live dB (or value) readout on drag, often a
   tooltip near the cap; automation write/arm modes (Touch/Latch/Read/Write) gate
   whether fader moves are captured. Source: protoolstraining automation modes,
   reapertips fader themes.

### Convergent truth
A clip is "real" only when it exposes **four distinct, draggable affordances** —
*trim (edge), fade-in/out (corner), loop, and a content/offset handle* — each drawn
from the same geometry the hit-tester uses (one source of truth, like our button
registry). And a combo DAW+NLE stays coherent by giving audio and video their own
**dedicated, switchable workspaces that share ONE session/timeline** (Resolve's page
model) — exactly what the R043 ribbon does, extended with FUSION/3D-CGI/AGI tiers.

---

## Gap → Action table (G1..G7) — the next loop

| ID | Gap (open) | Why it matters | Build order | Status |
|----|-----------|----------------|------------|--------|
| G1 | Arrangement clips have **no resize/trim/fade handles** drawn | clips read as static blocks, not real editable events (research 1–6) | draw trim edges + fade corners in `draw_arrangement`; hit-test 4 handle types; drag = split/trim/fade | open |
| G2 | **Fade in/out** not yet a clip property/param | fades are table-stakes (Ableton/Logic/Pro Tools all default-on) | add `clip_fade_in/out` (sec) to `wb_clip`; apply in `stage_instruments` audio path + draw | open |
| G3 | **Loop region** handle on clips / arrangement | repeat is a core musical action (Bitwig/Reaper) | clip `loop` flag + loop-bar handle; render honors it | open |
| G4 | **Mixer fader readout + automation arm** | fader feels dead without a live dB value + write mode (research 8) | draw dB value near cap on drag; `automation_write` per track | open |
| G5 | **Content/offset slide** handle (Bitwig-style) | slide audio inside the clip boundary without moving the clip | top-half drag → `start_in_source` shift; draw + hit-test | open |
| G6 | **FUSION page** node graph view (we have `wb_compositor`) | the FUSION tier is wired but has no dedicated view yet | tab 5 (EDIT) → host the compositor node editor when FUSION active | open |
| G7 | **3D-CGI / AGI** tiers: real views | tiers unlockable but empty; user wants low-poly CGI + AGI control surface | stub a `wb_cgi` module (opaque, C11) + AGI command bridge; wire to ribbon | open |
| G8 | **Crossfade needs real overlap + material past the edge** | cosmetic crossfades that ignore this feel fake (Bitwig: only renders when clips overlap AND have audio extending past their edges) | G2/G1 must drag the fade *past* the boundary onto the other clip; draw the overlap region, not a fake gradient | open |
| G9 | **Pre-fade preserves the edit point** | fading in earlier audio *before* the clip while keeping the clip's true start at full amp is a distinct operation (Bitwig pre-fade) | G2 handle model must separate *clip-edge fade* from *content-before-edge fade-in* | open |

---

## Wired vs Open ledger (this repo)
- **wired:** R038 shell, R039 SESSION colors/scenes, R040 overview, R041 crash
  hardening, R042 clip-gain-on-waveform, R042-A realtime lock fix, R042-B duration
  resolve, R043 workspace module + ribbon + gate, **R043-G1/G2 clip-edit handles
  (trim + fade-in/out) wired via a self-contained `wb_clip_edit` side-table**,
  **R043-G3 clip LOOP (repeats buffer across timeline via side-table `loop` flag)
  + R043-G5 content-slide (start_in_source offset, slides waveform inside clip)** —
  both verified end-to-end through the engine render path (selftest loop=6 peaks,
  slide head 0.000→0.300). See `src/wb_clip_edit.c` + `include/wbus/wbus_clip_edit.h`.
- **R043-G4: mixer fader is now DRAGGABLE** (it was draw-only before) with
  dB-quantized steps (0.5 dB grid) + a hard 0 dB unity anchor, plus an
  automation-WRITE arm ('A' button per strip) that captures fader moves into a
  `volume` automation lane via wb_automation_recorder — verified end-to-end
  (selftest: 0 dB anchor snaps exactly; armed capture attenuates the rendered
  output). Reuses the existing automation model (no new subsystem).
- **open:** G6 (Fusion view), G7 (CGI/AGI views), G8/G9 (crossfade overlap + pre-fade).

## Build order for next loop
G1 → G2 → G3 (the "clips feel real" cluster, cheapest high-leverage) → G4 (fader
feedback) → G6 (Fusion view) → G5/G7 (content slide, CGI/AGI stubs).
