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
[G01] P0 WIRED(file browser) — File browser / OS open dialog for import. Import is hardcoded paths.
      Every NLE starts here (Premiere Cmd+I opens a browser).
[G02] P0 WIRED(audio import wb_import) — Audio-file import into audio tracks via GUI (WAV/AIFF/MP3).
[G03] P1 — Drag-and-drop import zone (Finder → timeline).  **WIRED R073: SDL_DROPFILE -> browser_import (video/audio/SRT)**
[G04] P1 — Media bin: persistent list of imported assets with thumbnails,  **WIRED R073: wb_session_add_bin_entry + save/load "bin" records (lane C)**
      durations, search/rename (Kdenlive "media bin", FCP "browser").
[G05] P2 — Audio recording input path: arm track, choose input device, record
      mic/instrument to arrangement (all DAWs core).
[G06] P2 — MIDI hardware recording: capture controller notes into clips,
      then quantize (Avid MIDI guide workflow).
[G07] P3 — Screen/camera capture ingest.

## B. ORGANIZE loop
[G08] P0 WIRED(undo/redo UI buttons + auto-checkpoints) — Undo/redo UI + history panel (Premiere History panel; trimming
      actions tracked per edit).
[G09] P1 WIRED(track rename/delete/reorder/rec-arm in gutter) — Track management: rename, delete, reorder (drag), rec-arm button,
      height resize (audit-confirmed missing).
[G10] P1 — Snap toggle + snap-to-markers/grid (FL scale/snap highlighting).  **WIRED R073: snap_pos() grid+marker quantize wired into clip move + scrub-seek (scale-highlight covered by G80)**
[G11] P2 — Project templates + recent-files list + new-project dialog
      (Reaper templates, Cakewalk dialog threads).
[G12] P2 — Autosave + crash recovery copies (Premiere auto-save folder/.bak).
[G13] P2 — Clip colors/naming in-grid (Ableton convention; SESSION audit found
      bars colored but slots blank).

## C. ROUGH-CUT loop
[G14] P0 WIRED(mouse clip move + edge trim, model helpers) — Mouse clip operations on ARRANGE: drag move between tracks/positions,
      edge-drag trims, marquee multi-select + group ops (FCP/Premiere baseline).
[G15] P1 WIRED(trim mode: T enters, frame nudge, JKL shuttle) — Trim MODE: JKL dynamic trimming while playing, frame-step nudge,
      extend-edit-to-playhead, ripple-trim-to-playhead variants
      (Premiere dynamic trimming doc; we have J/K/L shuttle only).
[G16] P1 WIRED(razor tool R + click splits all under x) — Razor at playhead / blade-all-tracks shortcut (Cmd-B class).
[G17] P1 — Slide edit (move content, neighbors adjust) distinct from slip
      (Derek Lieu trick list: slide vs slip are separate tools everywhere).  **WIRED R073: wb_session_slide_video_clip (Premiere Slide semantics, span-preserving), 8 gate checks**
[G18] P2 — Replace edit / match frame (source monitor ↔ timeline roundtrip).
[G19] P2 — Three-point editing (set in/out in source, place at playhead).
[G20] P2 — Multicam: sync angles by waveform/timecode, switch angles live
      (FCP multicam workflow; pain point #1 in r/editors threads).
[G21] P2 — Auto audio/video sync of separate recordings by waveform
      ("syncing separate audio is a nightmare" — top pain point).
[G22] P3 — Magnetic-timelime-style rippling default OR swap-clips drag.

## D. REFINE loop
[G23] P1 — Visible fade handles on clip edges + curve handle + adjacent
      crossfade drag (engine HAS crossfades; NO handles — Ableton fade-controls doc).  **WIRED (verified R073): grips + fade lines + curve glyph already drawn, drag + right-click curve cycle live, crossfade overlap tested**
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
[G30] P0 WIRED(aux sends SEND A/B -> bus tracks) — Sends/returns FX buses with pre/post fader sends (universally cited
      as THE mixing workflow; we have route bus only, no send knobs).
      [WIRED Wave1-B: wb_track.send_level/send_target[2] + stage_bus send pass
      into kind-2 buses; mixer strip SEND A/B -/+ buttons, % readout, target
      cycle; test_sends.]
[G31] P1 — FX chain rack UI per track: add/remove/reorder inserts with param
      panels (inserts array exists; no visible chain editor).
[G32] P1 — Real-time LUFS/true-peak meter on master (Youlean class; delivery  **WIRED R073: wb_lufs on live master path, wb_engine_get_master_lufs, mixer LUFS/TP readouts
      normalize exists offline but no live meter).
[G33] P2 — Track freeze/bounce-in-place for CPU (Ardour/Logic freeze; user
      directive list already flags FREEZE button — still open).
[G34] P2 — Plugin sandboxing / crash isolation (Bitwig crash protection; a bad
      VST3 kills our whole process today).
[G35] P2 — MIDI learn: map hardware knobs to any param (universal expectation).
[G36] P3 — Score/notation view (Cubase 14 Dorico-engine score editor trend).
[G37] P3 — Surround/spatial monitoring paths (5.1/Atmos trend; stereo-only today).

## F. DELIVER loop
[G38] P0 WIRED(render queue wb_export_job pthread + progress + cancel) — Render queue: background export with progress bar, cancel, multiple
      outputs (Media Encoder model; Resolve users demand separate queue; our
      export blocks the UI thread).
[G39] P1 WIRED(export range IN/OUT) — Export range: selection / in-out / whole (audit-confirmed missing).
[G40] P1 WIRED(resolution row 480p/720p/1080p) — Export settings UI: resolution/fps/bitrate/audio format fields
      (audit-confirmed static text only).
[G41] P1 WIRED(STEMS export per-track WAVs) — Stems export: selected tracks → individual files, same start
      (Logic stems workflow; collaboration requirement).
[G42] P2 — Platform preset targets: YouTube -14 LUFS, Netflix -27 LKFS/-2dBTP,
      broadcast -23 (loudness spec docs; we normalize to one target only).
[G43] P2 — Watch-folder auto-export (AME watch folders).
[G44] P2 — Text/title overlay tool: basic lower thirds/titles with font controls
      (titles = table stakes in every NLE incl. free ones; we have captions only).
[G45] P3 — GIF/social aspect-ratio export presets (9:16, 1:1 crops).

## G. INTEROP / RECOVER loop
[G46] P1 — SRT import/export buttons (captions roundtrip; audit-confirmed).  **WIRED R073: .srt in scan/browser, SRT IN/OUT buttons, write->parse roundtrip test
[G47] P2 — OTIO import/export alongside our shadow-bin (OTIO is becoming the
      editorial common language — Linux Foundation/ASWF; AAF adapter ecosystem).
[G48] P2 — DAWproject-format awareness for cross-DAW exchange (Bitwig/PreSonus).
[G49] P2 — Accessibility: screen-reader labels/names on all controls (OSARA
      proves pro-audio accessibility is real demand), full-keyboard operation.
[G50] P3 — Customizable keyboard shortcuts + macros (Studio One macros,
      InDesign-style remapping; NNGroup accelerator best practice).
[G51] P3 — Preferences surface: audio device, buffer size, latency display
      (Sweetwater buffer-size guidance; currently fixed).

## H. Deep-research additions (3 parallel agents, 2026-08-23 — merged)
Sources: Adobe Helpx autosave/crash-recovery, Reaper render queue + region
wildcards, Resolve Live Save/DRP backups, BMD forums (background render,
keyframe easing), OSARA/MSAA-UIA, WCAG 2.1 AA / Section 508 / EN 301 549 /
VPAT, Netflix Partner Help (-27 LKFS dialogue-gated), FCC CALM Act (ATSC A/85),
IMF ST 2067, FL Studio mixer routing/piano-roll tools docs, Bitwig Note FX
(Operators/Humanize/Note Repeats), Ableton Live 11-12 MIDI Tools/follow actions/
Scale Awareness, Cubase Chord Track/Expression Maps, Roger Linn swing spec
(Attack Magazine), Shotcut crossfade forum complaints, Vegas trimmer,
Premiere metadata panel, packafoma Premiere wishlist.

### Delivery loop additions
[G52] P1 WIRED(export delivery presets YOUTUBE/NETFLIX/BROADCAST/PODCAST) — Export presets: user-saved + platform presets (Resolve/ME ship them).
[G53] P2 — Batch render matrix: per-region/marker wildcard naming, multiple
      simultaneous outputs per job (Reaper $region wildcards; Resolve multi-codec).
[G54] P2 — Watch-folder auto-render (AME model; pairs with G43).
[G55] P1 WIRED(named loudness profiles EBU/A85/NETFLIX/YT/PODCAST) — Named loudness profiles: EBU R128 -23/-1dBTP, ATSC A/85 -24 LKFS,
      Netflix -27 LKFS dialogue-gated LRA 4-18 -2dBTP, streaming -14/-16 —
      selectable target + true-peak ceiling, not one hardcoded -14.
      Rejected deliveries are an explicit failure mode (Netflix spec).
[G56] P3 — IMF (SMPTE ST 2067) packaging awareness — document as roadmap.

### Recover loop additions
[G57] P0 WIRED(autosave 120s to /tmp/bigmac_autosave keep-5) — Autosave to dated Auto-Save folder on an interval (Premiere default;
      Kdenlive autosave credited with rescuing projects). We save ONLY on Ctrl+S.
[G58] P2 — Crash recovery on relaunch from latest autosave (Premiere behavior).
[G59] P2 — Versioned project backups/timeline snapshots (Resolve Live Save/DRP).

### Accessibility additions
[G60] P2 — Screen-reader labels via accessibility API (OSARA proves full-DAW
      access is real demand; macOS path is the AX tree).
[G61] P2 — Full keyboard operability audit vs Section 508 / EN 301 549;
      publish a VPAT-style conformance statement.
[G62] P3 — Focus visibility (WCAG 2.4.7) + reduced-motion/flash safety (2.3.1);
      we already have WCAG-AA contrast (R029) — extend to focus/motion.

### NLE timeline additions
[G63] P1 — Draggable transitions between clips with dynamic re-linking when
      clips move (Shotcut's #1 complaint is non-dynamic crossfades breaking).
[G64] P2 WIRED(crossfade curve types linear/equal-power/smoothstep) — Crossfade curve types: equal-power / constant-power / +3dB choice
      (engine has equal-gain linear only; Resolve offers all three).
[G65] P1 WIRED(two-up precision trim display) — Two-up precision trim display (FCP Precision Editor) — trims are
      blind today; show outgoing/incoming frames at the edit point.
[G66] P2 WIRED(drop modes OVERWRITE/INSERT cycle O) — Insert/overwrite/append/connect drop modes (FCP Q/W/E; Premiere
      toggle) — keyboard-only placement is currently ambiguous.
[G67] P2 — Basic color correction: Lift/Gamma/Gain wheels + waveform scope
      (even exposure fixes matter; Kdenlive outclasses minimal editors here).
[G68] P2 — Metadata columns/sorting + clip color labels in the bin (editors
      "spend as much time looking for clips as editing").
[G69] P1 WIRED(multiple timelines wb_project container) — Multiple timelines/sequences per project (every NLE has many; one
      timeline limits any real edit).
[G70] P2 — Relink/offline-media handling — hardcoded absolute paths break the  **WIRED R073: video offline flag + wb_session_update_offline + find_by_basename relink scan (lane C)**
      moment a project moves (r/editors relink threads).
[G71] P2 — Render cache/pre-render previews for smooth scrubbing (Resolve smart
      cache); thumbnail/waveform disk cache for big projects (R066 wavcache is
      in-RAM only).
[G72] P3 — Speed ramps / retiming curves (FCP Retime Editor class).
[G73] P3 — Default-duration batch transitions across all cuts (Shotcut FR 552).

### Music-production additions
[G74] P1 WIRED(per-send pre/post-fader switch) — Per-send pre/post-fader switch (Pro Tools standard) — refines G30.
      [WIRED Wave1-B: wb_track.send_pre[2] per-send toggle; post = tap after
      fader gain (default), pre = raw buffer; mixer PRE button per send;
      test_sends asserts pre > post when fader < 1.]
[G75] P2 — Sidechain routing UI (engine HAS sidechain DSP; no routing UI).  **WIRED R073: mixer insert rows = clickable key-source cycle buttons -> wb_engine_set_insert_sidechain**
[G76] P2 — FX chain rack: drag-reorder inserts, bypass, save/load chains
      (Cubase insert-chain presets).
[G77] P2 — Copy-paste channel-strip settings between strips (Reaper/Logic).
[G78] P2 — Peak-hold + LUFS/true-peak meter on master (refines G32; VU misses  **WIRED R073: wb_lufs K-weighted meter + peak hold, RBJ high-shelf fix, silence-flush test**
      streaming targets entirely).
[G79] P3 — Pre/post-fader meter point option (Pro Tools gain-staging modes).
[G80] P2 — Scale highlight + scale-lock ("draw in-key notes only" — Ableton 12  **WIRED R073: scale_lock snaps step_click pitches via engine wb_scale_snap; LOCK toggle live**
      Scale Awareness, Cubase Scale Assistant).
[G81] P2 — Chord/scale stamping into piano roll (FL chord tools).  **WIRED R073: chord stamp fills following steps with diatonic tones via engine wb_chord_tones**
[G82] P3 — Chord track with harmonic transformation (Cubase Chord Track).
[G83] P3 — MIDI transformations: arpeggiate/strum/humanize/randomize
      (Live 12 MIDI Tools, Bitwig Note FX).
[G84] P3 — Articulation management hiding raw keyswitches (Cubase Expression
      Maps / Logic Articulation Sets).
[G85] P3 — Ghost notes from other clips while editing (FL ghost channels).
[G86] P3 — Multi-CC lanes: pitch/mod/aftertouch editors beyond velocity
      (FL graph editor Note/Velocity/Pitch/Mod X/Y).
[G87] P1 — Per-step velocity (FL per-step Velocity/Pitch/Shift) — refines  **WIRED R073: per-step velocity — shift-drag edit, commit uses step_vel, playback fires with it**
      the STEP checklist item.
[G88] P2 — Per-step probability/chance (Logic Step Sequencer chance; Bitwig  **WIRED R073: per-step probability — playback rolls against step_prob each trigger**
      Operators) — static binary grids sound robotic.
[G89] P1 WIRED(swing % MPC spec odd-16th delay, SWING control) — Swing/shuffle % MPC-spec 50-75% (Roger Linn canonical swing).
      [WIRED Wave1-B: session->swing 0..0.6 fraction of a 16th delayed on odd
      steps; wb_swing_offset helper applied in wb_transport_schedule_notes_sw
      and STEP perf_tick; toolbar SWING -/+ + % readout; test_swing.]
[G90] P2 — Pattern chaining/song mode (MPC chaining; FL patterns-as-clips).
[G91] P3 — Step-fill utilities (every 2nd/4th, random fill).
[G92] P3 — Note repeats/retrigs with accents (Bitwig Note Repeats).
[G93] P1 WIRED(capture-quantize rolling note log -> 16th-grid clip) — Capture-quantize: record what you JUST played without pre-arming
      (Live capture MIDI) — jamming becomes material.
[G94] P1 WIRED(session-record-to-arrangement wb_launchrec) — Record session-launcher performance INTO the arrangement
      (Ableton §7.5; Bitwig Record-to-Arranger) — closes the launcher loop.
[G95] P2 — Chance-weighted follow actions incl. Jump/Other/Fill/Legato
      (Live 11+ two-action chance model).

### Agent convergent truths (merged)
- Delivery agent: mature tools converge on a decoupled cancellable observable
  render queue + saved presets; continuous autosave/backups behind a prefs UI;
  accessibility as API + WCAG basics; open interchange accepted as lossy-but-
  documented; loudness parameterized by NAMED STANDARD with true-peak ceilings.
- NLE agent: the value is the surrounding SYSTEM (media organization, proxies,
  non-blocking render, transitions, precise visual feedback), not the timeline.
- DAW agent: four ideas everywhere Big Mac lacks — routing as first-class UI;
  probability + humanization; theory-aware editing; nonlinear capture into
  linear time.

### Revised build order (supersedes the waves above)
Wave 1 (loops broken at both ends): G01 import browser, G02 audio import,
G08 undo UI, G57 autosave, G38 render queue+cancel, G30 sends, G94
session-record-to-arrangement, G89 swing.
Wave 2: G14 mouse clip ops, G15+G65 trim mode + precision display, G23 fade
handles, G55 loudness profiles, G41 stems, G69 multiple timelines.
Wave 3+: everything else by loop order; P3s last.
