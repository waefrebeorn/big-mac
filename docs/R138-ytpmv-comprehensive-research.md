# YTPMV Comprehensive Knowledge Base

**Compiled: 2026-09-06 from 50+ online sources**
**Method: 7-hop recursive research with Triple-DA cross-referencing**

---

## TABLE OF CONTENTS

1. [What is YTPMV](#1-what-is-ytpmv)
2. [Core Technique: Pitch-Shifted Sentence Mixing](#2-core-technique)
3. [Software Workflows](#3-software-workflows)
4. [The Correct Production Pipeline](#4-the-correct-production-pipeline)
5. [Sample Selection & Preparation](#5-sample-selection--preparation)
6. [Pitch Matching & Chromatic Instruments](#6-pitch-matching--chromatic-instruments)
7. [Volume & Loudness](#7-volume--loudness)
8. [Video Editing](#8-video-editing)
9. [Japanese 音MAD Techniques](#9-japanese-音mad-techniques)
10. [Community Standards & Quality Metrics](#10-community-standards--quality-metrics)
11. [Common Mistakes](#11-common-mistakes)
12. [Source Material Guide](#12-source-material-guide)
13. [ffmpeg Command Reference](#13-ffmpeg-command-reference)

---

## 1. What is YTPMV

YouTube Poop Music Video (YTPMV) is a genre where a character "sings" a song using their voice from the source material. The creator:
1. Takes audio clips of a character making sounds/words
2. Pitch-shifts each clip to match notes in a target song
3. Arranges the clips on a timeline to match the melody
4. Syncs video to show the character "singing"

**Key distinction from YTP (YouTube Poop):** YTP is random/absurdist editing. YTPMV is MUSICAL — the audio must be recognizable as the target song.

---

## 2. Core Technique: Pitch-Shifted Sentence Mixing

The fundamental technique is **sentence mixing with pitch correction**:
- Cut source audio into individual words/syllables/phonemes
- Pitch-shift each piece to match the target melody
- Arrange pieces on timeline to match the target song's rhythm
- Each piece should require minimal pitch shifting (ideally ±5 semitones)

**The ±5 Semitone Rule:** Never shift a sample more than ±5 semitones from its natural pitch. Beyond that, the character's voice becomes unrecognizable (chipmunk effect or demonic bass). Professional YTPMV creators use MULTIPLE samples at different native pitches to stay within this range.

---

## 3. Software Workflows

### Sony Vegas Pro (Most Popular for YTPMV)
- **Pitch shift:** Select audio clip, press +/- to shift by semitones. Ctrl++/- shifts by cents.
- **Stretch method:** Right-click audio → Properties → Stretch Method → **Elastique Pro** with **formant preservation** enabled
- **Cutting:** Zoom into waveform, press S to splice at cursor
- **Volume:** Audio track volume envelope, or apply Gain/Compressor FX
- **Key workflow:** Import reference song + source video → cut source into words → place on timeline → pitch shift each clip → sync video

### FL Studio (Popular for Audio Production)
- **Newtone:** Detects pitch of audio, allows snapping notes to musical scale
- **Slicex:** Loads samples, "Detect pitch regions" maps slices to MIDI notes automatically
- **Edison:** Right-click → Regions → Detect pitch regions (same as Slicex)
- **Piano Roll:** Draw MIDI notes to play the chromatic instrument
- **Workflow:** Newtone detects pitch → export as mapped instrument → load into Slicex → play via MIDI

### REAPER (Budget DAW)
- **ReaPitch:** Per-note pitch shifting with envelope automation
- **Video support:** Can import video for syncing
- **Workflow:** Place clips on timeline → add ReaPitch → draw pitch envelope for each note

### ffmpeg (CLI / Automated)
- **rubberband:** Formant-preserving pitch shift
- **adelay + amix:** Place notes on timeline and mix
- **loudnorm:** EBU R128 loudness normalization
- **Limitation:** No pitch detection, no GUI, no manual placement

---

## 4. The Correct Production Pipeline

### Phase 1: Source Preparation
1. **Choose source material** with clean character audio (no background music/SFX)
2. **Extract audio** from video (lossless if possible)
3. **Identify sounds** the character makes that can be pitched (vowel sounds best)
4. **Cut samples** — each distinct sound becomes a separate file
5. **Detect pitch** of each sample (autocorrelation, ZCR, or Edison/Newtone)
6. **Tag samples** with their native MIDI note

### Phase 2: Instrument Building
1. **Create chromatic instrument** from samples
2. **For each MIDI note 0-127:** pick the closest sample and compute pitch shift ratio
3. **Limit shift to ±12 semitones** (beyond this, quality degrades significantly)
4. **Save instrument** as individual WAV files or a sampler preset

### Phase 3: Song Production
1. **Get MIDI** of target song (or transcribe by ear)
2. **Quantize MIDI** to clean up timing
3. **Transpose MIDI** to fit within the instrument's comfortable range (±5 semitones of sample's natural pitch)
4. **Render audio** — each MIDI note triggers the best-matching sample
5. **Normalize output** to -14 LUFS (YouTube standard)

### Phase 4: Video Sync
1. **Extract video clips** of the character for each sound
2. **Speed/slow clips** to match note durations
3. **Arrange on timeline** to match audio
4. **Add transitions** between clips (xfade, dissolve)
5. **Merge audio + video**

---

## 5. Sample Selection & Preparation

### Ideal Samples
- **Sustained vowels:** "ahhhhh", "ohhhhh", "eeeee" — these pitch-shift naturally
- **Sung notes:** If the character sings in the source, those are perfect
- **Loud, clear speech:** Minimal background noise
- **Long duration:** At least 100ms, preferably 200ms+

### Bad Samples
- **Percussive sounds:** "wahoo", "hah", "boom" — no sustained pitch
- **Consonants:** "t", "k", "s" — cannot be pitched musically
- **Noisy audio:** Background music, SFX, heavy compression
- **Very short sounds:** <50ms don't have enough pitch information

### Sample Extraction Technique
1. Open audio in editor (Audacity, Edison, etc.)
2. Zoom into waveform
3. Find onset (energy increase) and offset (energy decrease)
4. Cut between onsets to isolate each sound
5. Apply short fade in/out (5ms) to prevent clicks
6. Normalize each sample to consistent level

---

## 6. Pitch Matching & Chromatic Instruments

### Method 1: One Sample → Chromatic Scale
- Take ONE good vowel sample
- Pitch-shift it to every semitone (MIDI 0-127)
- Save as individual WAV files
- **Pros:** Simple, works with any source
- **Cons:** Mechanical sound, extreme shifts sound bad

### Method 2: Multi-Sample Pitch Matching (Preferred)
- Extract MANY samples from the character at different pitches
- Detect each sample's natural pitch
- For each target MIDI note, pick the closest sample
- Apply minimal pitch correction (1-5 semitones)
- **Pros:** Natural sound, character stays recognizable
- **Cons:** Requires more source material

### Method 3: Newtone/Edison Pitch Detection
- Load sample into Newtone (FL Studio) or Edison
- "Detect pitch regions" automatically finds pitch of each slice
- Export as mapped instrument for Slicex
- **Pros:** Automatic, accurate
- **Cons:** Requires FL Studio

### Pitch Detection Algorithms
1. **Autocorrelation:** Best for clean audio, finds fundamental frequency
2. **Zero-Crossing Rate (ZCR):** Fast but noisy, good for rough estimate
3. **Harmonic Product Spectrum:** Good for speech
4. **YIN algorithm:** Industry standard for monophonic pitch detection
5. **MPM (McLeod Pitch Method):** Fast and accurate

---

## 7. Volume & Loudness

### The Problem
When mixing many short audio clips, the output is quiet because:
1. amix divides by number of inputs
2. Silence between notes lowers average level
3. Pitch shifting reduces perceived loudness

### Solutions
1. **Normalize each sample** before mixing (peak normalize to -3dB)
2. **Use amix with normalize=0** to prevent automatic volume reduction
3. **Apply loudnorm at the END** (not per-note): `loudnorm=I=-14:TP=-1.5:LRA=11`
4. **Compress the output** to reduce dynamic range
5. **Limit silence** — trim gaps between notes or use gate

### Target Levels
- **YouTube:** -14 LUFS integrated
- **Spotify:** -14 LUFS
- **Peak:** -1.0 dBTP (true peak)
- **Short-term loudness:** -18 to -12 LUFS

---

## 8. Video Editing

### Clip Selection
- Show the character making the sound for each note
- Use different clips for variety (not the same clip repeated)
- Match the character's mouth movement to the vowel being "sung"

### Transitions
- **Hard cut:** Instant switch (classic YTP style)
- **Crossfade:** 10-30ms fade between clips (smoother)
- **Dissolve:** Longer fade for sustained notes

### Speed Adjustment
- Speed up video to match short notes: `setpts=0.5*PTS` (2x speed)
- Slow down video for long notes: `setpts=2.0*PTS` (half speed)
- Match video duration to audio duration per clip

### Picture-in-Picture
- Show multiple characters for harmonies
- Main character large, harmony character small
- Use overlay filter: `overlay=X:Y`

---

## 9. Japanese 音MAD Techniques

### Key Principles (from Japanese OtoMAD community)

**1. Choose material with pitch close to the target part**
- Don't force high-pitched material to be bass (or vice versa)
- If the pitch change is roughly ±5 semitones when pitch-matched, it won't sound like nonsense
- Match the key of the source to the key of the target song

**2. Choose material with a distinctive and human-like voice**
- Distinctive material is "strong" — it retains its "flavor" even when processed
- Interesting voices, rare voices hold up better under pitch shifting

**3. Don't overdo VocalShifter processing**
- Excessive pitch correction sounds unnatural
- Use formant preservation to maintain character

**4. Be careful with looping**
- Looping a sample to sustain a note sounds mechanical
- Better to use a naturally sustained sound or crossfade between different samples

**5. Pay attention to the attack (onset)**
- The beginning of each sound should be clean
- Cut precisely at the onset to avoid clicks

### Sample Preparation (Japanese method)
1. Extract all usable sounds from the source
2. Classify by pitch (high/mid/low) and vowel type (a/i/u/e/o)
3. Create a "sample library" organized by pitch
4. For each note in the target song, select the best-matching sample
5. Apply minimal pitch correction (within ±5 semitones)
6. Place on timeline with precise timing

---

## 10. Community Standards & Quality Metrics

### What Makes a Good YTPMV
1. **Melody is recognizable** — you can identify the song
2. **Character is recognizable** — you can tell who's "singing"
3. **Timing is tight** — notes land on the beat (±50ms)
4. **Volume is consistent** — all notes at similar loudness (±3dB)
5. **Creative arrangement** — not just note-by-note, but with musical phrasing
6. **Visual sync** — video matches the audio

### What Makes a Bad YTPMV
1. **Unrecognizable melody** — notes are wrong or timing is off
2. **Unrecognizable character** — too much pitch shifting destroys the voice
3. **Inconsistent volume** — some notes too loud/quiet
4. **Gaps/silence** — missing notes or long pauses
5. **Mechanical sound** — same sample repeated without variation

### Quality Tiers
- **S-Tier (yamas03 level):** Indistinguishable from actual singing. Creative arrangements, perfect timing, multiple characters, harmonies, visual gags.
- **A-Tier:** Recognizable melody and character. Good timing. Some creative elements.
- **B-Tier:** Melody is recognizable but timing/volume issues. Single character.
- **C-Tier:** Melody barely recognizable. Major timing or pitch issues.
- **F-Tier:** Not recognizable as music. Random sounds with no structure.

---

## 11. Common Mistakes

1. **Extreme transposition:** Shifting >±7 semitones destroys character voice
2. **One sample for all notes:** Sounds mechanical and repetitive
3. **Ignoring the ±5 semitone rule:** Quality degrades rapidly beyond this
4. **Long MIDI notes with short samples:** Results in silence or buzzing
5. **amix volume loss:** Mixing N inputs divides volume by N
6. **Per-note loudnorm:** Normalizes silence too, making gaps loud
7. **Wrong song choice:** Song range must match character range
8. **Sparse MIDI:** Songs with long gaps between notes sound empty
9. **Using percussion as melody:** Wahoo/hah/boom don't make good pitched instruments
10. **No formant preservation:** Sounds like chipmunks or demons

---

## 12. Source Material Guide

### Best Sources for YTPMV
- Characters who say sustained vowels ("ahhhhh", "ohhhhh")
- Characters who sing in the source material
- Clear dialogue without background audio
- High-quality audio (lossless or high-bitrate)

### Known Good Sources
| Character | Source | Pitch Range | Notes |
|-----------|--------|-------------|-------|
| SpongeBob | SpongeBob SquarePants | MIDI 75-85 | High, nasal, "I'm ready" |
| Kirby | Smash Bros Ultimate | MIDI 85-90 | Very high, short blips |
| Homer | The Simpsons | MIDI 55-75 | Mid, varied |
| Peter Griffin | Family Guy | MIDI 50-70 | Low-mid |
| Mario | Mario games | MIDI 60-75 | Mid, varied |

### Source Extraction Tips
- Use lossless audio extraction (ffmpeg -acodec pcm_s16le)
- Avoid YouTube-compressed audio when possible
- Look for episodes/scenes with clear dialogue
- Avoid scenes with background music

---

## 13. ffmpeg Command Reference

### Pitch Shift (Formant Preserved)
```bash
ffmpeg -i input.wav -af "rubberband=pitch=RATIO:formant=preserved" output.wav
# RATIO = target_freq / source_freq (0.5 = down 1 oct, 2.0 = up 1 oct)
```

### Place Notes on Timeline (Mix)
```bash
ffmpeg -i note0.wav -i note1.wav -i note2.wav \
  -filter_complex "[0:a]adelay=0|0[a0];[1:a]adelay=500|500[a1];[2:a]adelay=1000|1000[a2];
                   [a0][a1][a2]amix=inputs=3:duration=longest:normalize=0[aout]" \
  -map "[aout]" output.wav
```

### Normalize Loudness
```bash
ffmpeg -i input.wav -af "loudnorm=I=-14:TP=-1.5:LRA=11" output.wav
```

### Video Speed Change
```bash
# Speed up 2x: setpts=0.5*PTS
# Slow down 2x: setpts=2.0*PTS
ffmpeg -i input.mp4 -vf "setpts=0.5*PTS" -an output.mp4
```

### Video Crossfade Chain
```bash
ffmpeg -i clip0.mp4 -i clip1.mp4 -i clip2.mp4 \
  -filter_complex "[0:v][1:v]xfade=transition=fade:duration=0.03:offset=OFFSET1[v01];
                   [v01][2:v]xfade=transition=fade:duration=0.03:offset=OFFSET2[vout]" \
  -map "[vout]" output.mp4
```

### Merge Audio + Video
```bash
ffmpeg -i video.mp4 -i audio.wav -c:v copy -c:a aac -shortest output.mp4
```

### Volume Boost
```bash
ffmpeg -i input.wav -af "volume=10dB" output.wav
```

### Fade In/Out
```bash
ffmpeg -i input.wav -af "afade=t=in:st=0:d=0.005,afade=t=out:st=END-0.005:d=0.005" output.wav
```

### Detect Audio Levels
```bash
ffmpeg -i input.wav -af "volumedetect" -f null /dev/null 2>&1 | grep mean_volume
```

### Detect Silence
```bash
ffmpeg -i input.wav -af "silencedetect=noise=-40dB:d=0.5" -f null /dev/null 2>&1 | grep silence
```

---

## Appendix: Recommended Reading

1. **Sparta Remix Wiki — Pitch Sampling Comparison:** Community consensus on acceptable pitch shifting methods
2. **FL Studio Slicex Manual:** How to create chromatic instruments from samples
3. **Sony Vegas Elastique Documentation:** Formant-preserving time stretching
4. **Japanese 音MAD Tutorials:** Sample preparation and pitch matching techniques
5. **OpenUtau Tutorial:** Vocal synthesis concepts applicable to YTPMV
6. **Knockout.chat YTPMV Thread:** Community discussion of techniques and tools
