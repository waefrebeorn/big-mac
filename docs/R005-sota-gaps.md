# SOTA DAW Gap Research — recursive loop input

## What Big Mac has (verified)
- Engine: pull-based, staged (schedule→instruments→effects→buses→mix), RT-safe cmd queue, double-buffered, Xrun detection
- Instruments: FM synth, drum machine, audio-clip player
- Effects: comp, delay, reverb, chorus, EQ, sampler
- CLAP host bridge (graph integration, 12/12)
- Transport: play/stop/record, bpm, time sig, loop
- Session: tracks, clips, notes, inserts, automation lanes, undo/redo, route=bus/group
- MIDI recording, launchpad LED feedback, project file workflow (.wbus)
- Per-slot bypass/wet, aux send array (send[WB_MAX_TRACKS])
- Build: 0 warnings, 114/114 selftest, 12/12 CLAP

## SOTA benchmarks (online research, 2026)

### Reaper (routing)
- Every track = audio+MIDI+bus+send+return simultaneously
- Routing matrix (rows=sources, cols=destinations), unlimited sends
- Sidechain: drag route button onto target plugin → sends as 3/4 channel
- Any track can host instruments AND effects
- **Gap**: Big Mac has route (bus) + send[] but no sidechain plugin-key routing, no MIDI-to-track routing, no matrix UI

### Bitwig (modulation)
- The Grid: 235 modules, any signal → any port, 4x oversampled poly stereo
- Unified Modulation System: assign/edit modulations, every parameter modulatable
- All signals stereo (audio AND control), pre-cords for common connections
- Song position as sample-accurate signal, note expression (velocity/pressure/slide)
- **Gap**: Big Mac has NO modulation system, no per-parameter modulation routing, no note expression

### VST landscape
- VST3 SDK now MIT-licensed (2024+). VST2 still proprietary. CLAP is open (free-audio/clap). LV2 fading, VST3 + CLAP rising on Linux.
- Good hosts: Carla, Element, OpenDaw (glenwrhodes — full open-source DAW)
- Plugin dev frameworks: JUCE (dominant), iPlug2, DPF (DISTRHO — builds VST2/3, LV2, CLAP, AU), NIH-plug (Rust), rust-vst
- **Gap**: Big Mac has NO VST hosting at all. CLAP bridge exists but no real plugin loading/scanned plugins.

### OpenDaw (glenwrhodes/OpenDaw)
- Full open-source DAW for Windows/macOS/Linux. Real reference for "complete DAW" scope.

## Pinned gaps (priority order for recursive loop)

### P0 — VST/VST3 hosting (the big one)
- VST3 SDK integration (MIT now — no licensing barrier)
- Plugin scanner (scan directories, discover .vst3 bundles)
- Plugin instance lifecycle (create/destroy, process, setParam/getParam)
- Plugin UI embedding (VST3 editor windows)
- Parameter automation (VST3 parameters → wbus automation lanes)
- Fallback chain: VST3 → CLAP (existing bridge) → built-ins

### P1 — Sidechain / proper sends
- Per-track send to specific plugin input channel (not just bus→bus)
- Sidechain key input on compressor (external signal as compression trigger)
- Send levels per destination, pre/post fader choice

### P2 — Modulation system (Bitwig-style, scaled down)
- Per-parameter modulation: LFO, envelope, step sequencer → any parameter
- Modulation matrix (source → target parameter)
- Built-in modulators: LFO, envelope, MIDI CC, note expression

### P3 — UI completeness
- Mixer view (faders, pans, sends, inserts, routing)
- Track headers (name, color, mute/solo, arm, volume/pan readouts)
- Plugin window embedding
- Piano roll / MIDI editor
- proper arrangement navigation (zoom, scroll, selection)

### P4 — MIDI effects / processing
- Arpeggiator, quantizer, note repeat, velocity scaling
- MIDI FX chain per track (before instrument)

### P5 — Audio effects depth
- More built-in FX: multiband comp, vocoder, gate, chorus/flanger variants, saturation
- Parallel processing chains (send → FX return track)

## Recursive loop protocol
1. Research online (web_search) for current SOTA feature X
2. Compare vs Big Mac state (read relevant source)
3. Design minimal implementation that closes gap
4. Implement + verify with selftest/render
5. Commit, update INDEX.md ledger
6. Repeat with next gap
