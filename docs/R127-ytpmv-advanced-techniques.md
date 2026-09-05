# YTPMV Advanced Techniques: VFX, Lip-Sync, Multi-Character, Compositing

**Compiled: 2026-09-05**
**Method: 7-hop recursive research, 25+ sources, Triple-DA**
**Previous: R121 (audio pipeline), R122 (video effects & sync)**

---

## TABLE OF CONTENTS

1. [Viseme / Lip-Sync System](#1-viseme--lip-sync-system)
2. [Audio-Reactive Visual Effects](#2-audio-reactive-visual-effects)
3. [Multi-Character / Layered Voices](#3-multi-character--layered-voices)
4. [Character Cutout & Compositing](#4-character-cutout--compositing)
5. [YTPMV Community Standards & Quality Metrics](#5-ytpmv-community-standards--quality-metrics)
6. [Complete Production Workflow](#6-complete-production-workflow)
7. [ffmpeg Command Reference](#7-ffmpeg-command-reference)

---

## 1. VISEME / LIP-SYNC SYSTEM

### 1.1 Phoneme-to-Viseme Mapping

While there are ~44 English phonemes, they collapse to ~15 visemes (visual mouth shapes) because many sounds look identical on lips. The Oculus/Convai standard uses 15 visemes:

| Viseme ID | Name | Phonemes | Mouth Shape |
|-----------|------|----------|-------------|
| 0 | SIL | (silence) | Closed, neutral |
| 1 | PP | /b/, /p/, /m/ | Lips pressed together |
| 2 | FF | /f/, /v/ | Lower lip under upper teeth |
| 3 | TH | /θ/, /ð/ | Tongue between teeth |
| 4 | DD | /d/, /t/, /n/, /l/ | Tongue behind upper teeth |
| 5 | KK | /k/, /g/, /ŋ/ | Back of tongue raised |
| 6 | CH | /tʃ/, /dʒ/, /ʃ/, /ʒ/ | Lips slightly rounded, tongue up |
| 7 | SS | /s/, /z/ | Teeth close together |
| 8 | NN | /n/, /l/ (variant) | Tongue tip up |
| 9 | RR | /r/ | Lips slightly rounded, tongue back |
| 10 | AA | /æ/, /ɑ/ | Wide open mouth |
| 11 | EE | /i/, /ɪ/ | Wide smile, teeth showing |
| 12 | IH | /ɪ/ (variant) | Slightly open, relaxed |
| 13 | OH | /o/, /ɔ/ | Rounded open mouth |
| 14 | OO | /u/, /ʊ/ | Tightly rounded lips |

### 1.2 YTPMV Application

In YTPMV, we don't animate a 3D face — we cut between video clips. The viseme concept maps to:
- **Each note = a video clip of the character making a sound**
- The clip should show the character's mouth in a shape compatible with the pitch/vowel
- For best results: use clips where the character is making a sustained vowel (A, E, O)

### 1.3 Implementation Strategy

For a C11 engine:
1. Classify each source clip by dominant vowel sound (energy spectrum analysis)
2. Map MIDI notes to clips with matching vowel characteristics
3. For multi-sample: each sample has a "vowel class" (A/E/I/O/U)
4. When pitch-shifting, prefer samples whose vowel class matches the note's position in the scale

---

## 2. AUDIO-REACTIVE VISUAL EFFECTS

### 2.1 Beat-Synced Zoom Pulse

The classic YTPMV zoom pulse: the video zooms in slightly on each beat.

**ffmpeg approach using zoompan:**
```
zoompan=z='1.0+0.1*sin(2*PI*t*2)':d=1:x='iw/2-(iw/zoom/2)':y='ih/2-(ih/zoom/2)'
```

For beat-synced (not continuous), we need keyframe-based zoom:
```
zoompan=z='if(lt(mod(t,0.5),0.05),1.1,1.0)':d=1
```

This zooms to 1.1x for 50ms every 0.5 seconds (120 BPM).

### 2.2 Color Flash on Beat

Flash the screen white or change color grade on each beat:
```
geq=r='if(lt(mod(t,0.5),0.03),255,r(X,Y))':g='...':b='...'
```

Better approach using blend:
```
blend=all_expr='if(lt(mod(T,0.5),0.03),0.5*A+0.5*255,A)'
```

### 2.3 RGB Shift on Beat

Split RGB channels and offset them on each beat:
```
split=3[r][g][b];
[r]crop=W:H:0:0[r1];
[g]crop=W:H:3:0[g1];
[b]crop=W:H:6:0[b1];
[r1][g1][b1]mergeplanes
```

### 2.4 Screen Shake

Translate the image randomly on each beat:
```
translate=x='if(lt(mod(t,0.25),0.02),sin(t*100)*5,0)':y='if(lt(mod(t,0.25),0.02),cos(t*100)*5,0)'
```

### 2.5 Audio Level Detection

ffmpeg can extract audio levels per frame using `astats` or `volumedetect`:
```
astats=metadata=1:reset=1,ametadata=print:key=lavfi.astats.Overall.RMS_level
```

For true audio-reactive effects, we need a two-pass approach:
1. Pass 1: Analyze audio, generate keyframe file
2. Pass 2: Apply effects using keyframe data

### 2.6 showspectrum / showwaves

Audio visualization overlaid on video:
```
showspectrum=s=800x200:mode=combined:scale=log:color=intensity
showwaves=s=800x200:mode=line:colors=white
```

These can be overlaid at the bottom of the video for a "music visualizer" effect.

---

## 3. MULTI-CHARACTER / LAYERED VOICES

### 3.1 Concept

Advanced YTPMVs use multiple characters, each "singing" different parts:
- **Lead melody**: Character A (e.g., SpongeBob)
- **Harmony/Bass**: Character B (e.g., Patrick)
- **Percussion/SFX**: Character C (e.g., Squidward)

### 3.2 Implementation

Each character gets:
1. Their own sample bank (extracted from their audio)
2. Their own MIDI track (or channel)
3. Their own video source

The production pipeline:
1. Parse MIDI → separate tracks/channels
2. For each track, select the appropriate character
3. Process each track independently (pitch shift, envelope)
4. Mix all audio tracks together
5. Composite all video tracks (side-by-side, PiP, or cut between)

### 3.3 Harmony Generation

For YTPMV with chords:
- Take the lead melody MIDI note
- Generate harmony notes (3rd, 5th, octave)
- Map each harmony note to a different character's sample
- Mix all voices together

Example: Lead note C5 → Harmony notes E5 (major 3rd), G5 (perfect 5th), C6 (octave)

### 3.4 Video Layout Options

1. **Cut between**: Show one character at a time, cut on note changes
2. **Split screen**: Multiple characters visible simultaneously
3. **Picture-in-picture**: Main character large, harmony small
4. **Overlay**: Characters composited together (requires cutout)

---

## 4. CHARACTER CUTOUT & COMPOSITING

### 4.1 Chroma Key (Green Screen)

ffmpeg `chromakey` filter:
```
chromakey=0x00FF00:similarity=0.15:blend=0.1
```

Parameters:
- `color`: Key color (0x00FF00 = green)
- `similarity`: Threshold (0.01-0.15, lower = stricter)
- `blend`: Edge blending (0.0-0.5, higher = softer edges)

### 4.2 Luma Key

For white/black backgrounds:
```
lumakey=0:0.1:0.1  (key out black)
lumakey=255:0.1:0.1  (key out white)
```

### 4.3 Color Key (Arbitrary Color)

```
colorkey=0x00FF00:0.3:0.2
```

### 4.4 Multi-Layer Compositing

ffmpeg filter_complex for character on background:
```
[bg][char]overlay=100:200:format=auto[out]
```

With chroma key:
```
[char]chromakey=0x00FF00:0.15:0.1[cutout];
[bg][cutout]overlay=100:200[out]
```

### 4.5 Rotoscoping (No Green Screen)

For sources without green screen:
1. Edge detection → find character boundary
2. Flood fill from edges → create mask
3. Apply mask → extract character
4. Feather edges → smooth transition

This is computationally expensive. For real-time YTPMV, pre-computed masks are better.

---

## 5. YTPMV COMMUNITY STANDARDS & QUALITY METRICS

### 5.1 From the YTPMV Resource Pastebin (community standard)

**Software hierarchy (community consensus):**
1. Sony Vegas Pro — "The only right way to make YTPMVs"
2. FL Studio (for audio production)
3. After Effects (for VFX)
4. Reaper (budget alternative)
5. DaVinci Resolve (free alternative)

**Audio tools:**
- Newtone (FL Studio) — pitch correction
- Melodyne — professional pitch editing
- Rubberband — formant-preserving pitch shift
- Vocodex — vocoder effects

### 5.2 Quality Metrics

From community feedback and tutorial analysis:

1. **Pitch accuracy** — Notes must be within ±50 cents of target
2. **Formant preservation** — Character voice must remain recognizable
3. **Timing precision** — Notes within ±20ms of beat
4. **Volume consistency** — All notes at similar loudness (±3dB)
5. **Video sync** — Visual changes aligned with audio (±40ms)
6. **Smooth transitions** — No clicks, pops, or jarring cuts
7. **Creative arrangement** — Not just melody, but structure (verse/chorus)

### 5.3 Common Mistakes (from community)

1. **Over-pitch-shifting** — Shifting >±7 semitones destroys character
2. **No formant preservation** — Sounds like chipmunks
3. **Inconsistent volume** — Some notes too loud/quiet
4. **Bad timing** — Notes not aligned to beat grid
5. **Low-quality sources** — Compressed audio, low-res video
6. **No arrangement** — Just playing melody, no structure
7. **Epilepsy-inducing flashes** — Too many rapid cuts

### 5.4 Advanced Techniques (from Japanese 音MAD community)

1. **Chord decomposition** — Each note of a chord gets its own character
2. **Counter-melody** — Secondary melody using different character
3. **Rhythm variation** — Pitch-shifted percussion sounds
4. **Dynamic range** — Quiet verses, loud choruses
5. **Source switching** — Different characters for different song sections
6. **Visual motifs** — Recurring video elements that match musical phrases

---

## 6. COMPLETE PRODUCTION WORKFLOW

### 6.1 Phase 1: Source Preparation
1. Download/extract character video
2. Extract clean audio (no background music/SFX)
3. Identify vowel sounds and their timestamps
4. Classify samples by pitch and vowel type

### 6.2 Phase 2: Audio Production
1. Parse target MIDI melody
2. For each note, select best sample (closest pitch, matching vowel)
3. Pitch-shift each sample to target note (rubberband, formant preserved)
4. Apply volume envelope (fade in/out)
5. Place each note on timeline at correct position
6. Mix all notes together
7. Apply master effects (compression, EQ, reverb)

### 6.3 Phase 3: Video Production
1. For each note, extract corresponding video clip
2. Speed/slow clip to match note duration
3. Apply video effects (zoom, color, shake) synced to beat
4. Composite clips on timeline (with crossfades)
5. Add background layer (constant, non-flashing)
6. Add overlays (spectrogram, lyrics, effects)

### 6.4 Phase 4: Final Assembly
1. Merge audio + video
2. Add intro/outro
3. Add title card
4. Export final video
5. Quality check (pitch, timing, sync)

---

## 7. FFMPEG COMMAND REFERENCE

### 7.1 Pitch Shift (Formant Preserved)
```
ffmpeg -i input.wav -af "rubberband=pitch=RATIO:formant=preserved" output.wav
```
Where RATIO = target_freq / source_freq (0.5 to 2.0 typical)

### 7.2 Volume Envelope
```
afade=t=in:st=START:d=DUR,afade=t=out:st=END:d=DUR
```

### 7.3 Video Crossfade Chain
```
ffmpeg -i clip0.mp4 -i clip1.mp4 -i clip2.mp4 \
  -filter_complex "[0:v][1:v]xfade=transition=fade:duration=0.03:offset=OFFSET1[v01];
                   [v01][2:v]xfade=transition=fade:duration=0.03:offset=OFFSET2[vout]" \
  -map "[vout]" output.mp4
```

### 7.4 Audio Delay + Mix
```
ffmpeg -i note0.wav -i note1.wav -i note2.wav \
  -filter_complex "[0:a]adelay=0|0[a0];[1:a]adelay=500|500[a1];[2:a]adelay=1000|1000[a2];
                   [a0][a1][a2]amix=inputs=3:duration=longest[aout]" \
  -map "[aout]" output.m4a
```

### 7.5 Chroma Key Cutout
```
ffmpeg -i character.mp4 -i background.mp4 \
  -filter_complex "[0:v]chromakey=0x00FF00:similarity=0.15:blend=0.1[cutout];
                   [1:v][cutout]overlay=0:0[out]" \
  -map "[out]" -map 1:a output.mp4
```

### 7.6 Beat-Synced Zoom
```
zoompan=z='1.0+0.08*if(lt(mod(t,BPM/60),0.05),1,0)':d=1:
  x='iw/2-(iw/zoom/2)':y='ih/2-(ih/zoom/2)'
```

### 7.7 Video Speed Change
```
setpts=PTS_FACTOR*PTS  (0.5 = 2x speed, 2.0 = half speed)
```

### 7.8 Audio Speed Change (Preserving Pitch)
```
atempo=FACTOR  (0.5 to 2.0 range, chain for wider)
```

### 7.9 Screen Shake
```
translate=x='sin(t*SHAKE_FREQ)*SHAKE_AMT':y='cos(t*SHAKE_FREQ)*SHAKE_AMT'
```

### 7.10 RGB Channel Shift
```
split=3[r][g][b];
[r]crop=iw:ih:0:0,geq=r='r(X,Y)':g='0':b='0'[red];
[g]crop=iw:ih:2:0,geq=r='0':g='g(X,Y)':b='0'[green];
[b]crop=iw:ih:4:0,geq=r='0':g='0':b='b(X,Y)'[blue];
[red][green][blue]blend=all_mode=addition[out]
```

---

## 8. ENGINE ARCHITECTURE RECOMMENDATIONS

### 8.1 Module Structure

```
wb_ytpmv_engine.c    — Core engine (sample bank, note processing)
wb_ytpmv_vfx.c       — Audio-reactive visual effects
wb_ytpmv_lipsync.c   — Viseme/phoneme mapping
wb_ytpmv_multi.c     — Multi-character/layered voices
wb_ytpmv_cutout.c    — Character cutout (chroma key, rotoscope)
wb_ytpmv_composite.c — Final compositing pipeline
```

### 8.2 Data Flow

```
MIDI File → Parse → Note Events → Sample Selection → Pitch Shift → Audio Mix
                                                                         ↓
Video Sources → Clip Extraction → Speed Adjust → VFX → Composite → Output
```

### 8.3 Key Algorithms

1. **Sample selection**: Nearest pitch match within ±5 semitones
2. **Pitch shift**: Rubberband (formant-preserving)
3. **Volume envelope**: Linear fade in/out at note boundaries
4. **Video timing**: Clip duration = note duration, speed-adjusted
5. **Beat detection**: Spectral flux onset detection
6. **Compositing**: Overlay with chroma key or direct cut
