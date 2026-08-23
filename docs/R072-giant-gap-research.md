# R072 — GIANT GAP RESEARCH: what Big Mac doesn't have
Online-researched 2026-08-23. Sources: Ableton Live 12 manual (session/arrangement/
fades), Premiere Pro help (text-based editing, JKL trim mode, multicam), Final Cut
Pro shortcuts/multicam docs, DaVinci Resolve workflows, Adobe Media Encoder
(render queue/watch folders), Reaper OSARA accessibility, Bitwig plugin sandboxing,
FL Studio/Maschine step sequencers, Studio One macros, OTIO/AAF interchange,
EBU R128 / Netflix / YouTube loudness specs, Kdenlive/Shotcut proxy reviews,
iZotope RX-class restoration suites, Youlean loudness metering, NNGroup keyboard-
accelerator guidance, user pain-point threads (r/editors, r/VideoEditing, Gearspace).
Companion to docs/GUI-CHECKLIST.md (the UI-affordance ledger). This file is the
FULL capability gap list. Every item: [Gxx] id, priority P0-P3, status open.

## Convergent truth (one line)
Professional editors are not lists of features — they are closed LOOPS:
capture → organize → rough-cut → refine → sweeten → deliver → recover.
Big Mac has fragments of each loop but no loop is closed end-to-end;
every gap below is a break in one of those seven loops.

## A. CAPTURE / INPUT loop
[G01] P0 — File browser / OS open dialog for import. Import is hardcoded paths.
      Every NLE starts here (Premiere Cmd+I opens a browser).
[G02] P0 — Audio-file import into audio tracks via GUI (WAV/AIFF/MP3).
[G03] P1 — Drag-and-drop import zone (Finder → timeline).
[G04] P1 — Media bin: persistent list of imported assets with thumbnails,
      durations, search/rename (Kdenlive "media bin", FCP "browser").
[G05] P2 — Audio recording input path: arm track, choose input device, record
      mic/instrument to arrangement (all DAWs core).
[G06] P2 — MIDI hardware recording: capture controller notes into clips,
      then quantize (Avid MIDI guide workflow).
[G07] P3 — Screen/camera capture ingest.

## B. ORGANIZE loop
[G08] P0 — Undo/redo UI + history panel (Premiere History panel; trimming
      actions tracked per edit).
[G09] P1 — Track management: rename, delete, reorder (drag), rec-arm button,
      height resize (audit-confirmed missing).
[G10] P1 — Snap toggle + snap-to-markers/grid (FL scale/snap highlighting).
[G11] P2 — Project templates + recent-files list + new-project dialog
      (Reaper templates, Cakewalk dialog threads).
[G12] P2 — Autosave + crash recovery copies (Premiere auto-save folder/.bak).
[G13] P2 — Clip colors/naming in-grid (Ableton convention; SESSION audit found
      bars colored but slots blank).

## C. ROUGH-CUT loop
[G14] P0 — Mouse clip operations on ARRANGE: drag move between tracks/positions,
      edge-drag trims, marquee multi-select + group ops (FCP/Premiere baseline).
[G15] P1 — Trim MODE: JKL dynamic trimming while playing, frame-step nudge,
      extend-edit-to-playhead, ripple-trim-to-playhead variants
      (Premiere dynamic trimming doc; we have J/K/L shuttle only).
[G16] P1 — Razor at playhead / blade-all-tracks shortcut (Cmd-B class).
[G17] P1 — Slide edit (move content, neighbors adjust) distinct from slip
      (Derek Lieu trick list: slide vs slip are separate tools everywhere).
[G18] P2 — Replace edit / match frame (source monitor ↔ timeline roundtrip).
[G19] P2 — Three-point editing (set in/out in source, place at playhead).
[G20] P2 — Multicam: sync angles by waveform/timecode, switch angles live
      (FCP multicam workflow; pain point #1 in r/editors threads).
[G21] P2 — Auto audio/video sync of separate recordings by waveform
      ("syncing separate audio is a nightmare" — top pain point).
[G22] P3 — Magnetic-timelime-style rippling default OR swap-clips drag.

## D. REFINE loop
[G23] P1 — Visible fade handles on clip edges + curve handle + adjacent
      crossfade drag (engine HAS crossfades; NO handles — Ableton fade-controls doc).
[G24] P1 — Keyframe graph editor for FX params: bezier handles, easing presets,
      value/speed graph (AE graph editor; kdenlive users beg for this; engine has
      param tracks but no curve UI beyond the R047 line overlay).
[G25] P2 — Automation modes read/write/touch/latch (Pro Tools 4 modes; we have
      recorder-arm only).
[G26] P2 — Time-stretch/pitch-shift clips to tempo/key (Elastic Audio/Flex Time;
      zero stretch support today).
[G27] P2 — Transient detection + slice editing on audio clips (Reason slices).
[G28] P2 — Strip silence / region detect from GUI (we detect silence in agent
      pipeline only; Logic strip-silence is a standard tool).
[G29] P3 — Take-folder style comping UI polish (comping model exists; no
      take-folder visual).

## E. SWEETEN loop (mix + fx)
[G30] P0 — Sends/returns FX buses with pre/post fader sends (universally cited
      as THE mixing workflow; we have route bus only, no send knobs).
[G31] P1 — FX chain rack UI per track: add/remove/reorder inserts with param
      panels (inserts array exists; no visible chain editor).
[G32] P1 — Real-time LUFS/true-peak meter on master (Youlean class; delivery
      normalize exists offline but no live meter).
[G33] P2 — Track freeze/bounce-in-place for CPU (Ardour/Logic freeze; user
      directive list already flags FREEZE button — still open).
[G34] P2 — Plugin sandboxing / crash isolation (Bitwig crash protection; a bad
      VST3 kills our whole process today).
[G35] P2 — MIDI learn: map hardware knobs to any param (universal expectation).
[G36] P3 — Score/notation view (Cubase 14 Dorico-engine score editor trend).
[G37] P3 — Surround/spatial monitoring paths (5.1/Atmos trend; stereo-only today).

## F. DELIVER loop
[G38] P0 — Render queue: background export with progress bar, cancel, multiple
      outputs (Media Encoder model; Resolve users demand separate queue; our
      export blocks the UI thread).
[G39] P1 — Export range: selection / in-out / whole (audit-confirmed missing).
[G40] P1 — Export settings UI: resolution/fps/bitrate/audio format fields
      (audit-confirmed static text only).
[G41] P1 — Stems export: selected tracks → individual files, same start
      (Logic stems workflow; collaboration requirement).
[G42] P2 — Platform preset targets: YouTube -14 LUFS, Netflix -27 LKFS/-2dBTP,
      broadcast -23 (loudness spec docs; we normalize to one target only).
[G43] P2 — Watch-folder auto-export (AME watch folders).
[G44] P2 — Text/title overlay tool: basic lower thirds/titles with font controls
      (titles = table stakes in every NLE incl. free ones; we have captions only).
[G45] P3 — GIF/social aspect-ratio export presets (9:16, 1:1 crops).

## G. INTEROP / RECOVER loop
[G46] P1 — SRT import/export buttons (captions roundtrip; audit-confirmed).
[G47] P2 — OTIO import/export alongside our shadow-bin (OTIO is becoming the
      editorial common language — Linux Foundation/ASWF; AAF adapter ecosystem).
[G48] P2 — DAWproject-format awareness for cross-DAW exchange (Bitwig/PreSonus).
[G49] P2 — Accessibility: screen-reader labels/names on all controls (OSARA
      proves pro-audio accessibility is real demand), full-keyboard operation.
[G50] P3 — Customizable keyboard shortcuts + macros (Studio One macros,
      InDesign-style remapping; NNGroup accelerator best practice).
[G51] P3 — Preferences surface: audio device, buffer size, latency display
      (Sweetwater buffer-size guidance; currently fixed).

## Build order (loop-closing logic)
Wave 1 close CAPTURE+ORGANIZE loops: G01 G02 G08 G30 (import browser, audio
import, undo UI, sends) — nothing else matters if you can't get media in and
can't undo mistakes.
Wave 2 close ROUGH-CUT: G14 G15 G16 G23 (mouse editing, trim mode, razor,
fade handles).
Wave 3 close DELIVER: G38 G39 G40 G41 (render queue, range, settings, stems).
Wave 4+: remaining P1/P2/P3 by loop order above.
