# GUI WORKFLOW CHECKLIST — what we have / what we DON'T have
Machine-audited 2026-08-23 via per-view screenshots + control inventory.
RULE: a feature is `wired` ONLY when it has (1) real engine call behind it,
(2) visible clickable control OR documented key, (3) a test or verified
screenshot. Everything else is OPEN — do not assume done.

## Cross-cutting gaps (affect EVERY view — highest priority)
- [ ] UNDO/REDO has NO UI at all (engine wb_undo exists; agent-only). No buttons, no Cmd+Z.
- [ ] No SAVE/OPEN dialog affordance beyond single hardcoded path (Ctrl+S/O to /tmp/bigmac_proj.wbus).
- [ ] No snap-to-grid toggle anywhere.
- [ ] No loop brace drawn on the timeline ruler (loop exists in engine only).
- [ ] No metronome/click.
- [ ] No preferences/settings surface (audio device, buffer size, sample rate).
- [ ] Status line shows one message then never clears.

## ARRANGE view
Have: tracks w/ mute/solo buttons, clips, automation lane overlay, markers,
comp marquee, overview minimap, playhead scrub-on-click.
- [ ] Track RENAME (no double-click / no field)
- [ ] Track DELETE and REORDER (drag)
- [ ] Per-track REC-ARM button
- [ ] Track height resize; collapse lanes
- [ ] Clip drag to move (mouse), clip context menu (right-click menu is minimal)
- [ ] Loop brace I/O draggable on ruler
- [ ] Tempo track / BPM automation lane
- [ ] Time-signature changes mid-song
- [ ] Zoom +/- buttons (wheel-only today)

## PAD view
Have: 4x8 grid auditioning via engine note.
- [ ] Velocity control (fixed 100 today) — no velocity gesture
- [ ] Pad labels/names, color mapping
- [ ] Record pad hits into arrangement (REC arm captures notes)
- [ ] Choke groups

## STEP view
Have: 16x8 toggle grid, live playhead firing, CLEAR/COMMIT to clip.
- [ ] Per-step velocity/accent
- [ ] Pattern length > 16
- [ ] Swing
- [ ] Multiple patterns / pattern bank A-D + chaining
- [ ] Pattern copy/duplicate
- [ ] Per-row (pitch) labeling

## SESSION view
Have: slot launch, scene launch column, transport-independent looping playback.
- [ ] Record session performance INTO the arrangement
- [ ] Follow actions
- [ ] Scene tempo
- [ ] Slot naming/colors in-grid
- [ ] Stop-button per track (Ableton convention)

## MEDIA view
Have: import ONE video (hardcoded demo paths!), proxy gen, duration probe.
- [ ] FILE BROWSER / open dialog (import is hardcoded paths — real blocker)
- [ ] Import AUDIO files into audio tracks from GUI
- [ ] Media BIN: list of imported assets (only last import remembered)
- [ ] Drag-and-drop import zone
- [ ] Source in/out point setting before placing on timeline
- [ ] Waveform/ thumbnail previews of sources

## EDIT view
Have: trim t/e, split x, delete d, slip y, roll m, ripple , ., FREEZE button,
in/out readout, transcript-cut integration.
- [ ] Multi-clip selection (marquee exists on ARRANGE only)
- [ ] Mouse drag clip edges to trim (keyboard-only today)
- [ ] Razor tool / click-to-split position
- [ ] Drag clips between tracks
- [ ] Snapping edits to grid/markers
- [ ] UNDO for edit ops (see cross-cutting)

## CAPTIONS view
Have: whisper generate, word list click-seek/select/delete, burn, DELIVER, CHAP.
- [ ] SRT IMPORT (button)
- [ ] SRT EXPORT (button)
- [ ] Inline text editing of words (delete-only today)
- [ ] Caption style: font size, position, color
- [ ] Search in transcript
- [ ] Burn toggle persisted as a real checkbox control

## EXPORT view
Have: render (^R), set path (^S), codec toggle button, delivery presets via CAPTIONS.
- [ ] Output filename FIELD (path set via key only)
- [ ] Resolution/fps/bitrate controls
- [ ] Export RANGE (in/out or selection) — whole song only
- [ ] Render PROGRESS bar + CANCEL (export blocks the UI thread!)
- [ ] Audio-only export (WAV/MP3) from GUI
- [ ] Post-render file reveal / success state

## Workspace ribbon (bottom-left AUDIO..PERFORM)
Have: tier switching, tier-specific views on EDIT tab.
- [ ] CGI: add-primitive buttons (box/sphere/etc.) — engine has cgi commands, NO buttons
- [ ] CGI: render-to-video button in-view
- [ ] AGI: task submit field in-view (demo tasks seeded, no input)
- [ ] PERFORM: deck add/remove, crossfader visible control check
- [ ] FUSION: node param edit controls (view is read-only)

## Priority order to close (proposed)
P0: undo/redo UI, file browser/import-any-media, export progress+cancel+range
P1: track rename/delete/reorder/rec-arm, clip mouse-drag + edge-trim,
    SRT import/export + inline text edit
P2: snap toggle + loop brace on ruler, media bin, step velocity/patterns,
    session-record-to-arrangement
P3: caption styling, CGI/AGI in-view authoring buttons, prefs surface
