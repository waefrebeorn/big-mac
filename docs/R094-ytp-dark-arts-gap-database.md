# R094 — YTP/YTPMV Dark Arts & Old-School MIDI/Automation Gap Database

## Part 1: YTP Poopisms — Complete Taxonomy

### Tier 1: Canonical Poopisms (Official YTP Wiki)

| # | Poopism | Description | Big Mac Status |
|---|---------|-------------|----------------|
| 1 | **Stutter Loop** | Repeat a short clip N times | ✅ Audio only (wb_ytp.c) |
| 2 | **Stutter Loop Plus** | Different video effect per loop iteration | ❌ MISSING (video) |
| 3 | **Stutter Loop Minus** | Remove video, keep audio (implied sex) | ❌ MISSING |
| 4 | **Buzzing Stutter Loop** | Stutter + buzzing noise (fake crash) | ❌ MISSING |
| 5 | **Scrambling/Random Chop** | Chop into frames, random order | ❌ MISSING |
| 6 | **Sex-O-Phone** | Sax music + visual FX for "tension" | ❌ MISSING |
| 7 | **Dance Rave** | Looping dance footage + techno/dubstep | ❌ MISSING |
| 8 | **Reverse** | Play clip backwards | ✅ wb_reverse() |
| 9 | **Sentence Mixing** | Rearrange words/syllables | ✅ wb_sentence_mix() |
| 10 | **Meme Replacements** | Replace word with meme audio | ✅ insert_soundbite() |
| 11 | **Stare Down / Mysterious Zoom** | Freeze face + slow zoom in | ❌ MISSING |
| 12 | **Bleep Censors** | Tone replaces swear word | ❌ MISSING |
| 13 | **Ear-Rape** | Volume blast to catch off-guard | ✅ wb_earrape() |
| 14 | **Panning** | Move clip around screen | ❌ MISSING (animated) |
| 15 | **Tech Text** | Split-second text commentary | ❌ MISSING |
| 16 | **Paint Jobs** | MS Paint edited images | ❌ MISSING |
| 17 | **SpaDinner** | Famous quotes as soundbites | ✅ insert_soundbite() |

### Tier 2: Advanced / Dark Arts Poopisms

| # | Technique | Description | Big Mac Status |
|---|-----------|-------------|----------------|
| 18 | **Datamosh** | Remove I-frames → melting effect | ✅ wb_datamosh() |
| 19 | **Deep Fry** | Contrast+saturation+noise+compress | ✅ wb_effect_deep_fry() |
| 20 | **Remux Chain** | Multiple re-encode → artifact buildup | ❌ MISSING |
| 21 | **VHS Degradation** | Tracking noise, chroma shift, bleed | ✅ wb_effect_vhs() |
| 22 | **RGB Glitch** | Channel separation + displacement | ✅ wb_effect_rgb_glitch() |
| 23 | **Strobe / Flash** | Rapid white/black frame alternation | ❌ MISSING |
| 24 | **Color Inversion Flash** | Inverted colors for 1-2 frames | ❌ MISSING |
| 25 | **Frame Freeze / Hold** | Freeze on frame for N seconds | ❌ MISSING |
| 26 | **Sudden Zoom / Zoom Punch** | Quick zoom in, hold, snap back | ❌ MISSING |
| 27 | **Screen Shake (Impact)** | Violent brief shake on hit | ❌ MISSING |
| 28 | **Impact Frame** | White/black frame on impact | ❌ MISSING |
| 29 | **Vine Boom** | Low thump + noise burst | ✅ wb_ytp_vine_boom() |
| 30 | **Cookie Cutter** | Mask video into shape (circle, star) | ❌ MISSING |
| 31 | **Mirror / Kaleidoscope** | Split-screen mirror, radial symmetry | ❌ MISSING |
| 32 | **Swirl** | Radial rotation distortion | ❌ MISSING |
| 33 | **Spherize / Bulge** | Pinch/bulge distortion | ❌ MISSING |
| 34 | **Wave Displacement** | Sin/cos pixel displacement | ❌ MISSING |
| 35 | **Mesh Warp** | Control-point grid distortion | ✅ wb_mesh_warp.c |
| 36 | **Flip / Spin** | Animated flip, continuous rotation | ❌ MISSING |
| 37 | **Picture-in-Picture** | Small video inset, animated pos | ❌ MISSING |
| 38 | **Ken Burns** | Slow pan/zoom across still image | ❌ MISSING |
| 39 | **CRT / Scanlines** | Horizontal scanlines, barrel curve | ❌ MISSING |
| 40 | **Posterize** | Reduce color levels | ✅ wb_effect_posterize() |
| 41 | **Chromatic Aberration** | RGB channel offset | ✅ wb_chromatic_aberration() |
| 42 | **Vignette** | Dark corners | ✅ wb_effect_vignette() |
| 43 | **Camera Shake** | Random offset per frame | ✅ wb_camera_shake() |
| 44 | **Speed Ramp** | Gradual speed change | ✅ wb_speed_ramp.c |
| 45 | **Tape Stop** | Pitch + speed drop to zero | ✅ wb_tape_stop.c |

### Tier 3: Obscure / Esoteric Poopisms

| # | Technique | Description | Big Mac Status |
|---|-----------|-------------|----------------|
| 46 | **Suckr** | Mouth "sucks" objects (scale anim) | ❌ MISSING |
| 47 | **Blow-up Punch** | Character punches camera (zoom+shake) | ❌ MISSING |
| 48 | **Horror Poop** | Dark color grade + bass + strobe | ❌ MISSING |
| 49 | **MLG Montage** | Snoop + "DO IT" + datamosh | ❌ MISSING |
| 50 | **Saponite / Soapbro** | Smooth zoom + reverb + slow-mo | ❌ MISSING |
| 51 | **Tennis Rally** | Back-and-forth clip exchange | ❌ MISSING |
| 52 | **Infinite Loop** | Seamless loop (last frame = first) | ❌ MISSING |
| 53 | **Subversion Poop** | Subvert viewer expectations | ❌ MISSING |
| 54 | **Anti-Poop** | Deliberately boring → sudden chaos | ❌ MISSING |
| 55 | **Source Abuse** | Single source pushed to absurdity | ❌ MISSING |
| 56 | **Collab Chaos** | 10+ editors, each adds layer | ❌ MISSING |
| 57 | **Recursion Poop** | Video inside itself (Droste) | ❌ MISSING |
| 58 | **Steganography** | Hidden frames, subliminal | ❌ MISSING |
| 59 | **Compression Torture** | Re-encode 100x at lowest quality | ❌ MISSING |
| 60 | **AVS Plugin Abuse** | Stacked AVS filters (2000s style) | ❌ MISSING |

---

## Part 2: YTPMV Production Pipeline

### Core YTPMV Techniques

| # | Technique | Description | Big Mac Status |
|---|-----------|-------------|----------------|
| Y1 | **Phoneme Extraction** | Chop audio into phonemes/syllables | ❌ MISSING |
| Y2 | **Phoneme Catalog** | Database: pitch, duration, timbre | ❌ MISSING |
| Y3 | **Pitch-to-Note Map** | Map phoneme to musical note | ❌ MISSING |
| Y4 | **Formant-Preserving Shift** | Pitch shift without chipmunk | ❌ MISSING |
| Y5 | **Stutter-Bass** | Loop phrase → pitch down → kick | ❌ MISSING |
| Y6 | **Beat Sequencer Grid** | 16th-note grid for phonemes | ❌ MISSING |
| Y7 | **BPM Quantize** | Snap phoneme placement to beat | ❌ MISSING |
| Y8 | **Lip-Sync Match** | Mouth shape → vowel phoneme | ❌ MISSING |
| Y9 | **Vocal Parallel** | Overlay voice on unrelated visual | ❌ MISSING |
| Y10 | **Sidechain Pump** | Audio duck on kick hit | ✅ (wb_sidechain.c) |
| Y11 | **Glitch Retrigger** | Stutter on beat drop | ❌ MISSING |
| Y12 | **Tape-Stop on Beat** | Pitch/speed drop on transition | ✅ (wb_tape_stop.c) |
| Y13 | **Color-Coded Motifs** | Visual color = audio motif | ❌ MISSING |
| Y14 | **Call-and-Response** | Vocal syllable ↔ SFX alternation | ❌ MISSING |
| Y15 | **YTPMV Renderer** | Composite final with beat-sync FX | ❌ MISSING |

### YTPMV Sub-Genres

| # | Sub-Genre | Description |
|---|-----------|-------------|
| YS1 | **Classic YTPMV** | Phonemes → melody, tight quantize |
| YS2 | **Plunderphonics YTPMV** | Musical sources, not speech |
| YS3 | **Breakcore YTPMV** | 140-180 BPM, amen breaks |
| YS4 | **Nightcore YTPMV** | 1.25-1.5x speed, uplifting |
| YS5 | **Horror YTPMV** | Minor key, bass drops, strobe |
| YS6 | **OTOMAD** | Japanese style, Nico Nico Douga |
| YS7 | **MAD** | Anime source, frame-accurate |
| YS8 | **SoundClown** | Modern successor, TikTok era |

---

## Part 3: Old-School MIDI & Automation Styles

### MIDI Techniques (90s-2000s DAW era)

| # | Technique | Description | Big Mac Status |
|---|-----------|-------------|----------------|
| M1 | **MIDI Note Grid** | Piano roll with 16th-note snap | ✅ (wb_midi.c) |
| M2 | **MIDI CC Automation** | Mod wheel, expression, breath | ✅ (wb_midi_remote.c) |
| M3 | **MIDI LFO** | Low-freq oscillation → any param | ✅ (wb_lfo_sidechain.c) |
| M4 | **MIDI Pitch Bend** | ±2 semitone bend wheel | ✅ (wb_pitch_bend.c) |
| M5 | **MIDI Aftertouch** | Pressure per note | ❌ MISSING |
| M6 | **MIDI Poly Aftertouch** | Pressure per key independently | ❌ MISSING |
| M7 | **MIDI NRPN/RPN** | Non-registered param numbers | ❌ MISSING |
| M8 | **MIDI SysEx** | System exclusive (device config) | ❌ MISSING |
| M9 | **MIDI Clock Sync** | BPM sync to external clock | ✅ (wb_beat_sync.c) |
| M10 | **MIDI MMC** | Machine control (play/stop/rec) | ❌ MISSING |
| M11 | **MTC (MIDI Timecode)** | SMPTE sync via MIDI | ❌ MISSING |
| M12 | **MIDI Chase** | Catch up to playback position | ❌ MISSING |
| M13 | **MIDI Quantize** | Snap recorded notes to grid | ✅ (wb_quantize.c) |
| M14 | **MIDI Velocity Curve** | Remap velocity response | ❌ MISSING |
| M15 | **MIDI Arpeggiator** | Auto-chord from held notes | ✅ (wb_arpeggiator.c) |
| M16 | **MIDI Step Sequencer** | 16-step trigger pattern | ❌ MISSING |
| M17 | **MIDI Euclidean** | Distribute N hits over M steps | ❌ MISSING |
| M18 | **MIDI Probability** | % chance per note fires | ❌ MISSING |
| M19 | **MIDI Ratchet** | Rapid note repeat (1/32) | ❌ MISSING |
| M20 | **MIDI Strum** | Slight delay per note in chord | ❌ MISSING |

### Old-School Automation Styles

| # | Style | Description | Era | Big Mac Status |
|---|-------|-------------|-----|----------------|
| A1 | **Linear Fade** | Straight-line volume/pan change | 1990s | ✅ (wb_automation.c) |
| A2 | **Exponential Fade** | Natural log curve fade | 1990s | ❌ MISSING |
| A3 | **Logarithmic Fade** | Reverse-log curve | 1990s | ❌ MISSING |
| A4 | **S-Curve Fade** | Smooth ease-in-out | 1990s | ❌ MISSING |
| A5 | **Step Automation** | Instant value change (no interp) | 1990s | ❌ MISSING |
| A6 | **Hold Automation** | Value holds until next point | 1990s | ❌ MISSING |
| A7 | **Bezier Curve** | Cubic bezier keyframe interp | 2000s | ❌ MISSING |
| A8 | **Ease In / Ease Out** | Acceleration/deceleration | 2000s | ❌ MISSING |
| A9 | **Overshoot** | Go past target then settle | 2000s | ❌ MISSING |
| A10 | **Elastic** | Spring-like bounce | 2010s | ❌ MISSING |
| A11 | **Bounce** | Ball-bounce decay | 2010s | ❌ MISSING |
| A12 | **Smooth (Hermite)** | Catmull-Rom spline | 2000s | ❌ MISSING |
| A13 | **Tension/Continuity/Bias** | TCB spline control | 1990s | ❌ MISSING |
| A14 | **Expression Automation** | Math formula drives param | 2000s | ✅ (wb_expressions.c) |
| A15 | **Audio-Driven Automation** | Envelope follower → param | 2000s | ✅ (wb_audio_reactive.c) |
| A16 | **LFO Automation** | Sine/triangle/saw → param | 1990s | ✅ (wb_lfo_sidechain.c) |
| A17 | **Random Automation** | Random value per beat | 2000s | ❌ MISSING |
| A18 | **MIDI Learn** | MIDI CC → any param | 1990s | ✅ (wb_midi_remote.c) |
| A19 | **Touch/Latch/Write** | DAW automation modes | 1990s | ❌ MISSING |
| A20 | **Trim Mode** | Offset all automation by delta | 2000s | ❌ MISSING |
| A21 | **Relative Automation** | Multiply instead of replace | 2000s | ❌ MISSING |
| A22 | **Snapshot Automation** | Instant recall of all params | 1990s | ❌ MISSING |
| A23 | **Morph Automation** | Interpolate between snapshots | 2000s | ❌ MISSING |
| A24 | **Modulation Matrix** | Source → Dest with depth | 2000s | ✅ (wb_mod_matrix.c) |
| A25 | **Envelope Generator** | ADSR → any param | 1990s | ✅ (wb_env.c) |

### Old-School Keyframe Interpolation Types

| # | Interp Type | Math | Used In |
|---|-------------|------|---------|
| K1 | **Constant** | No interpolation | Everywhere |
| K2 | **Linear** | y = mx + b | Everywhere |
| K3 | **Bezier** | Cubic bezier | After Effects, Vegas |
| K4 | **Hermite** | Catmull-Rom | 3ds Max, game engines |
| K5 | **Step/Hold** | f(t) = floor | Logic, Pro Tools |
| K6 | **Ease-In** | t² | Flash, After Effects |
| K7 | **Ease-Out** | 1-(1-t)² | Flash, After Effects |
| K8 | **Ease-In-Out** | 3t²-2t³ (smoothstep) | Everywhere |
| K9 | **Elastic** | Spring ODE | After Effects |
| K10 | **Bounce** | Gravity simulation | After Effects |
| K11 | **Back** | Overshoot cubic | After Effects |
| K12 | **Exponential** | 2^(10(t-1)) | Vegas, Premiere |
| K13 | **Logarithmic** | log(1+9t)/log(10) | Vegas, Premiere |
| K14 | **S-Curve** | Smoothstep variant | Everywhere |
| K15 | **TCB** | Tension/Continuity/Bias | 3ds Max, Maya |

---

## Part 4: Priority Build List

### Immediate (YTP Core)
1. Video stutter loop (repeat video segment N times)
2. Stutter loop plus (different FX per iteration)
3. Cookie cutter mask shapes (circle, triangle, star)
4. Mirror / kaleidoscope effect
5. Swirl / spherize geometric warp
6. Zoom punch + impact frame
7. Frame freeze / hold
8. Scramble / random chop

### High (YTPMV Pipeline)
9. Phoneme extractor (whisper → syllable segmentation)
10. Phoneme catalog database
11. Pitch-to-note mapper (chromatic scale)
12. Beat sequencer grid (16th note piano roll)
13. Stutter-bass generator
14. YTPMV renderer (beat-synced composite)

### Medium (Old-School MIDI/Automation)
15. MIDI aftertouch (channel + poly)
16. MIDI step sequencer
17. MIDI euclidean sequencer
18. MIDI probability/ratchet
19. All keyframe interpolation types (K1-K15)
20. All fade curves (exponential, log, S-curve)
21. Automation modes (touch/latch/write/trim)
22. Snapshot automation + morph

### Lower (Obscure/Advanced)
23. Recursion poop (Droste effect)
24. Compression torture (re-encode chain)
25. Steganography (hidden frames)
26. CRT / scanlines effect
27. Picture-in-Picture
28. Ken Burns effect

---

## Part 5: Technical Notes

### YTPMV MIDI Workflow
```
Source Audio → Whisper STT → Word Timestamps → Phoneme Segmentation
    → Pitch Detection → Note Mapping (C4=261.63Hz, etc.)
    → MIDI Note Assignment → Piano Roll Sequencer
    → Trigger Original Audio Chunks at Pitch-Shifted Rate
    → Mix with Drum Pattern → Master → Render
```

### Keyframe Interpolation Math
```
Linear:       y = t
Smoothstep:   y = 3t² - 2t³
Ease-In:      y = t²
Ease-Out:     y = 1 - (1-t)²
Exponential:  y = 2^(10(t-1))
Logarithmic:  y = log(1+9t)/log(10)
Elastic:      y = 2^(-10t) * sin((t-0.075)*2π/0.3) + 1
Bounce:       simulate gravity with decay
TCB:          Hermite with tension/continuity/bias params
```

### Old-School DAW Automation Modes
```
Touch:    Write while held, return to previous on release
Latch:    Write while held, stay at last value on release
Write:    Overwrite everything during playback
Read:     Play back automation, don't record
Trim:     Offset existing automation by delta
Relative: Multiply existing automation by delta
```
