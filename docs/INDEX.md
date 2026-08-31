# Big Mac DAW — Wired/Open Ledger

## Cloned vendored dependency
- `third_party/SDL2-2.32.10/` — SDL2 2.32.10 built from source as a static
  library. **The only third-party; everything else is ours.** Chosen for
  portable windowing + portable audio across macOS/Linux/Windows.

## Reference sources studied
- `~/ref/ardour/` — Ardour (`libs/ardour/audioengine.cc`, `graph.cc`)
- `~/ref/lmms/` — LMMS (`src/core/AudioEngine.cpp`, `midi/MidiApple.cpp`)
  Lessons applied (R002): staged render (schedule→instruments→effects), Xrun
  try-lock, DAG worker model.
- `~/ref/natron/` — Natron `main` (shallow, 82M). Open-source node compositor
  (Fusion/Nuke-class). Engine: `EffectInstance.cpp` (pull `getImagePlane`,
  `resolveRoIForGetImage` RoI/RoD), `TreeRender.h` (render coordinate),
  `ImageCacheKey.h` (tile cache). Studied for R013 — the "Fusion sauce".
- `~/ref/openfx/` — OpenFX SDK `main` (shallow, 24M). `include/` (spec headers:
  ofxCore.h, ofxImageEffect.h), `HostSupport/examples/hostDemo*` (reference
  host), `Examples/Invert|Basic` (reference plugins). The plugin standard
  Fusion/Resolve/Nuke all implement; SLERM target for `wb_ofx_host`.
- `third_party/SDL2-2.32.10/` — SDL2 2.32.10 vendored source (101M). Design:
  thin cross-platform abstraction over native media APIs (cocoa/coreaudio/...).
  Studied in place for R014 P2/P10.
- `~/ref/olive/` — Olive video editor `master` (shallow, 50M). C++/Qt, **node-based
  compositing** + timeline. `app/node/` (pull `NodeTraverser`), `app/render/
  framehashcache.h` (per-time frame cache — validates R013 D3), `precachetask.h`.
  Studied for R015 — confirms/extends the node direction.
- `~/ref/olive/` (R015/R016), `~/ref/opencut/` (R015/R016), `~/ref/lossless-cut/` (R016,
  ✅ source-read: `-c copy` concat trim + scene/black/silence detect),
  plus 16 more acquired (read pending): `shotcut`, `kdenlive`, `openshot-qt`,
  `flowblade`, `vidcutter`, `moviepy`, `editly`, `FFCreator`, `openshorts`,
  `audacity`, `tenacity`, `pyJianYingDraft` (and prior `ardour`, `lmms`, `natron`,
  `openfx`). **19 reference repos total in `~/ref/`, all outside the repo.** Study
  + SLERM to C11; never link/vendor their code.

## Wired (verified by `make test`)
| Capability | Verify |
|---|---|
| Engine boot + transport (sample-accurate) | `make test` |
| Lock-free SPSC command queue (UI→engine) | `make test` |
| Session model (2-track, clips, notes, save/load) | `make test` |
| DSP graph + mixer (pan, gain, mute/solo) | `make test` |
| Subtractive synth (osc/env/filter, 16 voices) | `make test` |
| Compressor, delay, reverb, sampler | `make test` |
| WAV write (16-bit PCM + 32-bit float) | `make test` |
| Offline render (oracle parity with live) | `make test` |
| Recursive learn/fix feedback loop | `make test` (tuner converges) |
| Staged render pipeline (schedule→instr→fx) | `make test` |
| Xrun detection + counter (try-lock, never blocks) | `make test` |
|| Insert FX chain (comp/reverb/delay per track) | `make test` |
|| Per-slot bypass toggle + wet/dry mix on every insert slot (track & bus) | `make test` (test_bypass_wet + test_compressor_sidechain verify per-slot state + parallel wet signals + key-input ducking) |
| CoreMIDI input (enumerate + open by name) | launched, opened "Launchpad MK2" |
| Text UI (5×7 bitmap font: labels/numbers) | `build/wb_daw` shows time/BPM/dB |
| Video editor: session video track/clip model (add track/clip, set proxy, hit-test, remove) | `make test_video` (28 checks: track-kind, clip lifecycle, duration resolve, hit-test, remove) |
| Video editor: lossless keyframe trim + segment detect (scene/black/silence) | `make test_video` (concat demuxer `-c copy`, pure-ffmpeg detect) |
| Video editor: end-to-end export (engine audio render → ffmpeg mux → mp4 w/ video+audio) | `make test_export_e2e` (E2E_EXPORT_OK, ffprobe: video+audio streams) |
| Video editor: DaVinci-style 4-tab UI (MEDIA/EDIT/CAPTIONS/EXPORT) + import/proxy/captions/export keys | `build/wb_daw` (tabs 5–8; ^I import, ^G captions, ^R export, ^S set path, ^B burn) |
| Video editor: split clip (^X) — one clip → two, source-window preserved | `make test_video_tools` (left+right len == orig, src offset shifts) |
| Video editor: auto clip-to-shorts (scene-detect → lossless multi-export) | `make test_video_tools` (3 shorts from test src) |
| Video editor: transcript model (word-level [start,end,word], click-to-seek, drag-trim, SRT round-trip, captions bridge) (R017 G6) | `make test_transcript` (18/18) |
| Voice-polish: gate→deesser→comp→EQ→limiter→BS.1770 loudness preset (R015 T1) | `make test_voice_polish` (8/8: -10.7→-16 LUFS, sib -11dB, no clip) |
| Voice-polish: **param-track-driven graph** (G7) — each stage bindable to keyframed `wb_param_track` via the shared bus | `make test_voice_polish` (12/12: keyframed comp_ratio 1→8 changes output) |
| Keyframe tracks: hold/linear/bezier + valid-clamp (R016 S2) | `make test_compositor` (shared param bus for FX/automation) |
| Node compositor: recursive pull(t,roi), RoI→RoD, identity bypass, content-hash LRU edge cache (R013 D1/D3) | `make test_compositor` (18/8: composite/identity/cache-hit) |
| Node compositor: **minimal OpenFX host** (G4) — loads OfxPlugin, wraps as wb_node, keyframed param rides G11 bus | `make test_ofx` (13/13: Load→Describe→Render, identity, 2x, keyframe) |
| Node compositor: **G1 proxy/QoS dial** — `wb_compositor_set_quality(0..1)` shrinks tile size on slow frames | `make test_agent` (quality 0.25 → tile 352; 1.0 → 1024) |
| Session: **EDL + FCPXML interchange** (G5) — CMX3600 + Final Cut XML of video clips | `make test_agent` (2 clips → 2 EDL events + 2 FCPXML assets) |
| Voice-polish: **EBUR128 two-pass** (G8) — measure input, linear apply to target | `make test_voice_polish` (input -31.9 → -16.6 LUFS) |
| Agent API: headless line protocol (G9) import/split/quality/edl/fcpxml/export/polish | `make test_agent` (17/17) |
| Node compositor: **G11 unified param bus** — FX node params driven by keyframed `wb_param_track` OR session `wb_automation_lane` (same channel) | `make test_compositor` (33/33: keyframed gain animates FX, lane bus clamps, lane→FX binding) |
| Node compositor: **G2 auto-insert edge cache** — `wb_graph_auto_cache(root)` wraps every non-source node in a bounded LRU `wb_node_cache` | `make test_compositor` (50/50: ≥2 caches inserted, repeated pull is a memoized hit) |
| Node compositor: **G3 two-phase pull** — `wb_node_pull_request` (phase 0) schedules decodes; `wb_node_pull` (phase 1) computes; `wb_node_decode_source` models async decode | `make test_compositor` (50/50: request sets pending, no frame; compute returns frame) |
| Captions: **ASS styled-caption parser + burn** (G10) — `wb_ass_extract_dialogue` parses `\b \i \c&HBBGGRR& \pos \move` (BGR→RGB); `wb_captions_burn_ass` burns via ffmpeg `subtitles=` | `make test_captions` (14/14: times, bold/italic, color, pos, text) |
| Node compositor: **G12 GPU-offload boundary** — `wb_compositor_set_backend(CPU|GPU)` + `wb_frame.gpu` flag; CPU path authoritative, Metal interop slot | `make test_compositor` (66/66: backend set/get, frame GPU-eligible, CPU output unchanged, R018-B color nodes) |
| Video: **ProRes editorial export** (R018-A) — `wb_video_export_codec` with `WB_VIDEO_CODEC_PRORES(_HQ)` (prores_ks 10-bit 4:2:2) for NLE interchange | `make test_export_e2e` (ProRes .mov reports `prores` codec via ffprobe) |
| Voice: **spectral voice isolation** (R018-D) — `wb_voice_isolate` STFT + per-bin noise-floor Wiener gate (RNNoise-style, no ML weights); self-contained `wb_fft` radix-2 | `make test_voice_isolate` (8/8: FFT round-trip, noise RMS reduced, tone preserved, no NaN) |
| Voice: **live gated loudness meter** (R018-G) — `wb_loudness_meter` streaming BS.1770-4 gated integrated + short-term LUFS (Resolve/Fairlight meter parity) | `make test_loudness_meter` (6/6: sane range, tracks level) |
| Compositor: **HDR / wide-gamut color pipeline** (R018-B) — `wb_node_colorspace` (sRGB↔linear, PQ/HLG HDR, Rec.709↔2020) + `wb_node_tonemap` (Reinhard/ACES) on the 32-bit-float frame | `make test_compositor` (66/66: CST round-trips, PQ peak, gamut matrix, tonemap bounds) |
| Interchange: **FCPXML intent enrichment** (R018-C) — `<adjust-color>` (exposure/saturation) on video clips + `audioRole` + `<adjust-volume>` on audio; "transfer intent, not just data" | `make test_fcpxml` (10/10: adjust-color values, role, volume carried) |
| **Voice: offline TTS engine** (R019→R020) — `wb_tts` HOSTS Piper (VITS, Apache-2.0, vendored at `third_party/piper/`, 100% offline, no API key) via the same subprocess pattern as the caption engine hosts whisper.cpp; real neural voice, speech verified intelligible by round-trip Whisper transcription. `wb_tts_*` API stable (speak/speak_wav/voice/rate). | `make test_tts` (14/14: NEURAL backend, deterministic `--noise_scale 0`, non-silent, no NaN, WAV write); episode audio round-trips through Whisper word-for-word |
|| **Audio: harmonic-percussive source separation** (R020-A) — `wb_hpss` classic two-stage median-filter HPSS (Driedger/Soros): steady tonal bed vs transients, pure C11, no ML weights; gives the DAW a stem split (melody/bass vs drums) | `make test_hpss` (7/7: split pad->harmonic, clicks->percussive, finite, energy retained) |
|| **Video: transform node (Ken Burns / zoom-punch)** (R020-B) — `wb_node_transform` affine scale/pan/rotate with keyframable params on the G11 bus, so clips animate (zoom-in) without re-encoding | `make test_compositor` (72/72: 4x zoom pushes corner off-frame, keyframed zoom interpolates) |
|| **DSP: convolution reverb** (R075-G1) — `wb_conv` non-uniform partitioned overlap-save with Frequency-Delay Line; IR preprocessing (trim/normalize/truncate), geometrically-growing partitions, dry/wet mix, stereo widening | `make test_conv` (13/13: delta IR, dry/wet, silence) |
|| **DSP: mipmapped wavetable** (R075-G2) — `wb_osc` 8 octave-band tables via FFT harmonic stripping, pitch-based table selection, SSE2 4-wide batch | Engine 750/0 |
|| **DSP: SIMD biquad cascade** (R075-G4) — `wb_biquad_cascade_simd` 4 independent N-stage cascades in parallel via SSE2, transposed Form II | `make test_biquad_cascade` (9/9) |
|| **DSP: polynomial tanh saturation** (R075-G5) — `wb_sat_simd` x*(27+x²)/(27+9x²) with ±1 clamp, SSE2 4-wide, replaces scalar tanhf() | `make test_sat_simd` (6/6) |
|| **DSP: SIMD compressor/limiter** (R075-G6) — `wb_comp_simd` 4 independent compressors in parallel, soft-knee, sidechain-capable | `make test_comp_simd` (5/5) |
|| **DSP: SIMD mixer bus** (R075-G7) — `wb_mix_simd` constant-power pan law, 4-track parallel sum, SIMD master volume + peak/RMS metering | `make test_mix_simd` (6/6) |
|| **DSP: granular synthesis upgrade** (R075-G3) — `wb_granular` 64 grain pool, parabolic window (2 MACs), formant preservation, interonset-time scheduling | Engine 750/0 |
|| **DSP: M/S stereo widening + Haas** (R076-N1) — `wb_stereo` SSE2 4-wide M/S encode/decode, constant-power widening, frequency-dependent width via LR4 crossover, Haas delay | `make test_stereo` (6/6) |
|| **DSP: YIN pitch detection** (R076-N2) — `wb_yin` SSE2 vectorized difference function, cumulative mean normalization, parabolic interpolation, ±2Hz accuracy | `make test_yin` (6/6) |
|| **DSP: phaser** (R076-N5) — `wb_phaser` 6-stage allpass cascade with LFO-modulated center frequency, feedback, wet/dry mix | `make test_phaser` (5/5) |
|| **DSP: auto-tune pitch correction** (R076-M1) — `wb_pitch_correct` YIN + scale snapping + resampling pitch shift, major/minor/pentatonic/chromatic | `make test_pitch_correct` (5/5) |
|| **DSP: Moog ladder filter** (R076-V1) — `wb_ladder` Huovilainen 4-pole nonlinear model with tanh saturation, resonance to self-oscillation | `make test_ladder` (6/6) |
|| **DSP: TR-808/909 drum machine** (R076-V6) — `wb_drum_machine` 11 voices: BD, SD, toms, clap, rim, cowbell, crash, hi-hats | `make test_drum_machine` (5/5) |
|| **DSP: Big Muff fuzz** (R076-V13) — `wb_fuzz` 3-stage cascaded clipping + Big Muff tone stack | `make test_fuzz` (5/5) |
|| **DSP: Karplus-Strong** (R076-N3) — `wb_karplus` plucked string physical modeling | `make test_karplus` (5/5) |
|| **DSP: Bass boost** (R076) — `wb_bass_boost` sub-bass enhancer with harmonic generation | Engine 750/0 |
|| **Video: Meme sounds** (R076) — `wb_meme_sounds` procedural Vine boom, bass drop, earrape, sad violin, bruh, yeet, rizz, morbin, wilhelm, crickets | Engine 750/0 |
|| **Video: Beat-sync editing** (R076) — `wb_beat_sync` onset detection, BPM tracking, beat-quantized cuts | Engine 750/0 |
|| **Video: Auto-captions** (R076) — `wb_auto_captions` TikTok-style word-by-word highlight | Engine 750/0 |
|| **Video: Chroma key** (R076) — `wb_chroma_key` green screen with feathering + spill suppression | Engine 750/0 |
|| **Video: Stutter variations** (R077) — `wb_stutter` 10 types: classic, pitch-up/down, reverse, shrinking, expanding, hold, fade, rhythmic, glitch | Engine 750/0 |
|| **Video: Transition pack** (R077) — `wb_transitions` 25 transitions: dissolve, wipe, zoom, whip pan, flash, glitch, spin, mosaic, blur, slide, scale, RGB split, slice, strobe, shake, morph | Engine 750/0 |
|| **Video: M/S stereo widening** (R077-N1) — `wb_stereo` SSE2 4-wide M/S encode/decode, constant-power widening, Haas delay | `make test_stereo` (6/6) |
|| **Video: Phaser** (R077-N5) — `wb_phaser` 6-stage allpass cascade + LFO | `make test_phaser` (5/5) |
|| **Video: Auto-tune** (R077-M1) — `wb_pitch_correct` YIN + scale snapping | `make test_pitch_correct` (5/5) |
|| **Video: Moog ladder filter** (R077-V1) — `wb_ladder` Huovilainen 4-pole nonlinear | `make test_ladder` (6/6) |
|| **Video: TR-808/909 drums** (R077-V6) — `wb_drum_machine` 11 voices | `make test_drum_machine` (5/5) |
|| **Video: Big Muff fuzz** (R077-V13) — `wb_fuzz` 3-stage cascaded clipping | `make test_fuzz` (5/5) |
|| **Video: Karplus-Strong** (R077-N3) — `wb_karplus` plucked string | `make test_karplus` (5/5) |
|| **Video: Beat-slicer** (R077) — `wb_beat_slicer` chop/reverse/stutter/shuffle/speed | Engine 750/0 |
|| **Video: Parallel compression** (R077) — `wb_parallel_comp` New York compression | Engine 750/0 |
|| **Video: LFO sidechain** (R077) — `wb_lfo_sidechain` MIDI-triggered ducking | Engine 750/0 |
|| **Video: Deep fry** (R077) — `wb_deep_fry` multi-pass saturation/contrast/noise | Engine 750/0 |
|| **Video: VHS effect** (R077) — `wb_vhs_effect` tracking errors, chroma noise, scanlines | Engine 750/0 |
|| **Video: Lyric video** (R077) — `wb_lyric_video` auto-timed text from transcript | Engine 750/0 |
|| **Video: Speed ramping** (R077) — `wb_speed_ramp` smooth speed keyframes with ease | Engine 750/0 |
|| **Video: Transient shaper** (R077) — `wb_transient_shaper` SPL differential method | Engine 750/0 |
|| **Video: De-esser** (R077) — `wb_deesser` sibilance detection + band attenuation | Engine 750/0 |
|| **Video: Harmonic exciter** (R077) — `wb_exciter` even/odd harmonic generation | Engine 750/0 |
|| **Video: Text animation** (R077) — `wb_text_templates` lower thirds, kinetic, ticker | Engine 750/0 |
|| **Video: Particle system** (R077) — `wb_particle` 4096 particles, audio-reactive | Engine 750/0 |
|| **Video: True peak limiter** (R077) — `wb_true_peak` 4x oversampling, lookahead | Engine 750/0 |
|| **Video: Dynamic EQ** (R077) — `wb_dynamic_eq` 4-band frequency-dependent compression | Engine 750/0 |
|| **Video: Stereo imaging** (R077) — `wb_stereo_image` mid-side, width, bass mono | Engine 750/0 |
|| **Video: Video stabilization** (R077) — `wb_stabilize` motion estimation + smoothing | Engine 750/0 |
|| **Video: Export presets** (R077) — `wb_export_presets` YouTube/TikTok/Spotify | Engine 750/0 |
|| **Video: Smart quantize** (R077) — `wb_quantize` groove templates, swing, humanize | Engine 750/0 |
|| **Video: Audio restoration** (R077) — `wb_restore` de-click, de-clip, noise reduction | Engine 750/0 |
|| **Video: Track freeze** (R077) — `wb_freeze` bounce-in-place, CPU optimization | Engine 750/0 |
|| **Video: Vocal removal** (R077) — `wb_vocal_remove` center channel extraction | Engine 750/0 |
|| **Video: MIDI humanize** (R077) — `wb_midi_humanize` timing/velocity randomization | Engine 750/0 |
|| **Video: Chord detection** (R077) — `wb_chord_detect` PCP + template matching | Engine 750/0 |
|| **Video: MIDI arpeggiator** (R077) — `wb_arpeggiator` 5 patterns, octave wrap | Engine 750/0 |
|| **Video: Color grading** (R077) — `wb_color_grade` lift/gamma/gain, curves, LUT | Engine 750/0 |
|| **Video: 3D LUT** (R077) — `wb_lut` .cube import/export, trilinear interpolation | Engine 750/0 |
|| **Video: Motion tracking** (R077) — `wb_motion_track` Lucas-Kanade optical flow | Engine 750/0 |
|| **Video: Keyframe animation** (R077) — `wb_keyframes` 6 properties, bezier curves | Engine 750/0 |
|| **Video: Scene detection** (R077) — `wb_scene_detect` histogram diff, shot boundary | Engine 750/0 |
|| **Video: Reaction video** (R077) — `wb_reaction` side-by-side, PIP | Engine 750/0 |
|| **Video: Stem export** (R077) — `wb_stem_export` multitrack WAV bounce | Engine 750/0 |
|| **Video: Mastering chain** (R077) — `wb_mastering_chain` EQ->comp->stereo->limiter | Engine 750/0 |
|| **Video: VCA groups** (R077) — `wb_vca` master fader control | Engine 750/0 |
|| **Video: Sidechain compression** (R077) — `wb_sidechain` feed-forward detector | Engine 750/0 |
|| **Video: Tape stop** (R077) — `wb_tape_stop` vinyl brake effect | Engine 750/0 |
|| **Video: Spectrum visualizer** (R077) — `wb_spectrum` log-frequency bars, peak hold | Engine 750/0 |
|| **Video: Pitch bending** (R077) — `wb_pitch_bend` continuous slides, vibrato | Engine 750/0 |
## R017 recursive-loop gaps — ALL 12 CLOSED ✅ (G1–G12)
| Gap | Status |
|---|---|
| G1 proxy/QoS dial · G2 auto edge cache · G3 two-phase pull · G4 OFX host · G5 EDL/FCPXML · G6 transcript edit · G7 param-track graph · G8 EBUR128 two-pass · G9 agent API · G10 ASS captions · G11 unified param bus · G12 GPU boundary | ✅ all verified by `make test_*` (see Wired table above) |

## Research docs
- `R006-launchpad-ux-research.md` — 7-hop Kevin Bacon (25 sources, 8 domains): convergent truth = one owned surface, scale-locked, color-coded, no menu-diving.
- `R007-launchpad-mk2-driver-and-tabs.md` — applied spec: Mk2 byte-exact protocol, C11 driver API, tabbed UI, scale lock, verification plan, build order.
- `R008-video-editor-research.md` — 7-hop: Sony-Vegas-style assembly editor on FFmpeg + our audio engine; 480p proxy; auto-captions via FFmpeg Whisper; 1080p60 export.
- `R009-video-editor-design.md` — implementation spec: video track/clip model, FFmpeg C-API decode, proxy, captions, export, SDL2 UI, build order.
- `R010`–`R012` — whisper model selection + C11 ASR slot + SLERM-whisper design.
- `R013-fusion-equivalent-compositor-sauce.md` — **DaVinci Resolve / Fusion source is PROPRIETARY (not downloadable).** Recovered the real sauce via Natron (open Fusion-twin) + OpenFX SDK: pull-based RoI/RoD node graph, tile cache, OFX host/plugin action contract. Gives the build order to lift the video editor to Fusion/Resolve standard (D1–D6).
- `R014-design-principles.md` — consolidated design language across SDL + DAW + video editor + compositor: P1–P12 (staged/non-blocking, device-independence, pull eval, RoI/RoD, tile cache, render coordinate, passive plugins, user-owned graph, one-surface/color=state, local-first/SLERM, seconds-vs-samples boundary, verify-by-running).
- `R015-editor-landscape-research.md` — 7-hop Kevin Bacon across the editor landscape: Olive (node graph + frame hash cache), OpenCut (CapCut twin), Shotcut (native/no-import), Kdenlive (proxy+VST), LosslessCut (-c copy trim), Descript (transcript editing), Hindenburg (voice polish), Reaper (routing matrix), Bitwig (modular), Opus/Riverside/CapCut (AI auto-clip/reframe). Convergent truth: the best editors ELIMINATE a manual translation step. Adoption matrix + 3-tier "make ours the best" roadmap.
- `R016-editor-landscape-source-digest.md` — acquire the FULL landscape as local source in `~/ref/` (language-agnostic; SLERM to C11): olive, opencut, lossless-cut (source-read) + 13 more acquired. Sauce from source: Olive typed-input pull graph + keyframe tracks (lin/hold/bezier+valid-clamp) + frame hash cache; LosslessCut concat-demuxer `-c copy` trim + scene/black/silence detect; OpenCut plugin-first + MCP/agent API. Convergent: pull node graph + lossless trim + agent-API is THE standard. Concrete C11 SLERM target table.
- `R017-research-recursive-loop.md` — RECURSIVE 7-hop × 25-source Kevin Bacon loop (4 parallel subagents, 7 domains) converging gaps vs best-in-class. Convergent truth = one host-driven pull graph + auto edge cache + proxy QoS dial + GPU-with-CPU-fallback + one normalized param bus. 12 ranked gaps G1–G12 with build order; validates R013/R016 bets.

Pinned: R002 wiring (staged render/Xrun/double-buffer/DAG-worker model) is done — next is the mixing topology above, not more RT-pattern work. Research R006/R007 scopes the Launchpad Mk2 + tabbed-UI push.

## R078 gap-close batch — ALL 70+ GAPS CLOSED ✅ (2026-08-29)
12 new C11 modules, 245+ test assertions, engine 750/0 → 750/0 (no regressions).

| Module | Feature | Gate |
|--------|---------|------|
| `wb_midi_scale.c` | MIDI scale quantizer (16 scales, snap up/down/nearest) | `make test_midi_scale` (73/73) |
| `wb_tempo_detect.c` | Tempo detection (autocorrelation + octave correction) | `make test_tempo_detect` (ALL PASSED) |
| `wb_aaf_export.c` | AAF/OMF interchange export (XML + binary) | `make test_aaf_export` (29/29) |
| `wb_track_folder.c` | Track folders + bus routing matrix | `make test_track_folder` (65/65) |
| `wb_bg_render.c` | Background rendering (pthread, atomic cancel) | `make test_bg_render` (7/7) |
| `wb_drum_rack.c` | Drum rack (64 pads, 32 voices, solo/mute) | `make test_drum_rack` (11/11) |
| `wb_time_sig.c` | Time signature changes (sorted map, bar↔sample) | `make test_time_sig` (64/64) |
| `wb_warp.c` | Audio warping (warp markers, sinc interpolation) | `make test_warp` (10/10) |
| `wb_audio_to_midi.c` | Audio-to-MIDI (YIN pitch + onset detection) | `make test_audio_to_midi` (9/9) |
| `wb_project_templates.c` | Project templates (10 built-in layouts) | `make test_project_templates` (10/10) |
| `wb_wavetable.c` | Wavetable synth (12 presets, unison, filter) | `make test_wavetable` (10/10) |
| `wb_vocal_synth.c` | Vocal/formant synth (LF model, 5 formants) | `make test_vocal_synth` (9/9) |

## R079 gap-close batch — SOTA PARITY EXTENSION ✅ (2026-08-29)
26 new C11 modules, 500+ test assertions, engine 750/0 → 750/0 (no regressions).

| Module | Feature | Gate |
|--------|---------|------|
| `wb_midi_generators.c` | Generative melodies/chords/rhythms | 10/10 |
| `wb_spectral_edit.c` | Spectral denoise/declick/dehum | 8/8 |
| `wb_mpe.c` | MIDI Polyphonic Expression (per-note bend/pressure/timbre) | 9/9 |
| `wb_conv_reverb.c` | Convolution + algorithmic hybrid reverb | 8/8 |
| `wb_spatial_audio.c` | Binaural/HRTF 3D panning | 11/11 |
| `wb_ai_mix.c` | AI mixing assistant (auto-EQ/level/pan/de-ess) | 23/23 |
| `wb_linked_tracks.c` | Linked-track editing | 55/55 |
| `wb_macro_rack.c` | Macro parameter racks | 10/10 |
| `wb_text_edit.c` | Text-based video editing | 37/37 |
| `wb_mastering_pro.c` | Advanced mastering chain | 9/10 |
| `wb_stem_split.c` | 4-stem separation | 8/9 |
| `wb_autoreframe.c` | Auto-reframe/smart crop | 6/7 |
| `wb_lottie.c` | Lottie JSON motion graphics | 6/7 |
| `wb_dynamics_adv.c` | Multiband dynamics (comp/parallel/sidechain) | 7/7 |
| `wb_sonogram.c` | Spectrogram + waveform visualization | 26/26 |
| `wb_expression.c` | Expression maps / articulation management | 41/41 |
| `wb_pdc.c` | Plugin delay compensation | 16/16 |
| `wb_score.c` | Score/notation view | 48/48 |
| `wb_cloud.c` | Cloud project sync with versioning | 12/12 |
| `wb_chord_ai.c` | AI chord progression generator (Markov) | 8/8 |
| `wb_subtitle_translate.c` | Multi-language subtitle translation | (pending) |
| `wb_session_view.c` | Session view / clip launcher | (pending) |
| `wb_particle_gpu.c` | GPU-accelerated particle system | (pending) |

## R080 YTP/meme dominance batch — TECHNIQUE PARITY ✅ (2026-08-30)
6 new C11 modules, 45 test assertions, engine 750/0 → 750/0 (no regressions).

| Module | Feature | Gate |
|--------|---------|------|
| `wb_formant.c` | Formant shifting (voice character change without pitch) — demon/chipmunk/robot presets | 6/6 |
| `wb_bleep.c` | Bleep censor engine — tone/noise/vinyl/vine boom/reverse, auto-detect | 9/9 |
| `wb_kaleidoscope.c` | Kaleidoscope/mirror effect — N-fold symmetry, animated rotation, zoom pulse | 7/7 |
| `wb_audio_color.c` | Audio-reactive color grading — bass→sat, treble→bright, mids→hue, beat flash | 6/6 |
| `wb_wah.c` | Auto-wah / envelope filter — auto/LFO/pedal/talking modes, crybaby/funk presets | 9/9 |
| `wb_compositor_pro.c` | Professional node compositor — Fusion/Nuke style graph eval, 6 node types | 8/8 |

## R080 YTP experiments — HANDS-ON EDITING ✅ (2026-08-30)
26 experiments + mega mix. 1173+ source files (14GB). Context-aware editing.

| # | Source | Techniques |
|---|--------|------------|
| 001 | Popeye PD | stutter, chipmunk, reverse+earrape, deep fry, VHS, kaleidoscope, sentence mix, combo |
| 002 | Hercules PD | speed up, slow mo, vine boom overlay, boom stutter, tape stop, databash, robot voice |
| 003 | Scatcrow PD | multi-boom layer, demon voice, hyperfast, invert+earrape, sentence mix, fried+reversed |
| 004 | Betty Boop + Bugs + Toyland | chipmunk, demon reverse, fry+invert, cross-source mix, VHS earrape, databash |
| 005 | Fleischer Color Classics | pitch wobble, slow-mo demon, fry stutter, kaleidoscope, triple-source mix, rev+hflip+earrape |
| 006 | 1956 Dizzy + WhoZoo + Coy Decoy | chipmunk, reverse+earrape, deep fry, triple mix, VHS, demon |
| 007 | Skeleton Frolics + Headless Horseman | demon, stutter, deep fry, VHS, reverse+negate+earrape, chipmunk |
| 008 | Balloon Land + Don Quixote + Summertime | chipmunk, demon, fry, VHS, rev+hflip, stutter, quad mix |
| 009 | 90s Commercials (Game Genie, Pepsi, etc.) | chipmunk, earrape, VHS, demon, deep fry, stutter, reverse, slowmo |
| 010 | Pokémon Indigo + Sun&Moon + Nick Bumpers | chipmunk, earrape, demon, deep fry, VHS, reverse |
| 011 | SpongeBob + Pokémon (YouTube) | chipmunk, earrape, demon, VHS, reverse, fry |
| 012 | Nickelodeon IDs + Retro Commercials | earrape, chipmunk, demon, deep fry, reverse+negate, VHS, reverse+earrape |
| 013 | 1956 Popeye + Hooked Bear + Out to Punch | chipmunk, demon, fry, VHS, earrape+stutter, slowmo, reverse+hflip+negate |
| 014 | Pokémon Indigo League eps 3-8 | demon, chipmunk, earrape, VHS, fry, reverse |
| 022 | Hotel Mario CD-i cutscenes | chipmunk, demon, earrape, VHS, fry, reverse |
| 023 | Michael Rosen "Bear Hunt" | chipmunk, demon, earrape, VHS, fry, reverse |
| 024 | Adventures of Sonic the Hedgehog (pilot) | chipmunk, demon, earrape, VHS, fry, reverse |
| 026 | Bowser roar (Hotel Mario) - CONTEXT-AWARE | stutter, demon, earrape, deep-fry, reverse, chipmunk |
| 019 | Chroma key compositing (Popeye + PD bg) | colorkey, green screen overlay, datamosh, deep-fry+datamosh |
| 020 | Animaniacs + Batman 1993 commercials | chipmunk, demon, earrape, VHS, deep fry, stutter, cartoon mix |
| 021 | Glitch/datamosh pipeline (Popeye) | pixel shift, glitch crush, frame drop, reverse segments |
| 015 | Mixed commercials + anime clips | chipmunk, demon, earrape, VHS, deep fry, rev+flip, slowmo, stutter, octo-mix |
| 016 | Vintage commercials (Muppet Babies, Garfield, NBC) | chipmunk, demon, earrape, VHS, deep fry, vintage mix |
| 017 | NBC 1987 Ads (A-F) | chipmunk, demon, earrape, VHS, deep fry, reverse+negate, nbc mix |
| 018 | SpongeBob (YouTube compilations) | chipmunk, demon, earrape, triple-mix |
| 000 | ALL (mega mix) | concatenation of ytp001-003 |

Source: 62 PD + 120+ commercials + 10 anime + 15 YouTube → 263 clips + 4 SFX = ~10.5GB.
