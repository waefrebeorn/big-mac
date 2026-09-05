# YTPMV Production: Complete Knowledge Base & Critical Research

**Compiled: 2026-09-05**
**Method: 7-hop recursive research with Triple-DA cross-referencing**
**Sources: 25+ online references across FL Studio docs, Reddit, YouTube tutorials, Japanese OtoMAD community, VGMusic archive**

---

## Table of Contents

1. [What Is YTPMV](#what-is-ytpmv)
2. [Core Production Techniques](#core-production-techniques)
3. [Audio Pipeline Deep Dive](#audio-pipeline-deep-dive)
4. [Video Editing Pipeline](#video-editing-pipeline)
5. [Sample Selection & Preparation](#sample-selection)
6. [Pitch Shifting: The Critical Knowledge](#pitch-shifting)
7. [Video-Audio Synchronization](#video-audio-sync)
8. [Epilepsy & Flashing Safety](#epilepsy-safety)
9. [Big Mac Implementation Plan](#implementation-plan)
10. [Research Sources](#research-sources)

---

## 1. What Is YTPMV

YTPMV (YouTube Poop Music Video) is a remix microgenre where creators transform spoken or non-musical source material into rhythmic, pitched music using characters' voices as instruments. The defining characteristic: **you can hear the original character "singing" the target melody** while the video shows the character in sync with the music.

**Key distinction from YTP**: YTP is random poop for comedy. YTPMV is **musical** — the output should sound like the character is singing a real song. The pitch mapping must be accurate enough that you can recognize the target melody.

### Sub-genres and Related Terms
- **YTPMV**: YouTube Poop Music Video (Western)
- **OtoMAD** (音MAD): Japanese equivalent, often more technically precise
- **音MAD**: Pronounced "on-mado", the broader Japanese remix culture
- **YTPMV vs OtoMAD**: OtoMAD tends to use UTAU/Vocaloid more, YTPMV uses raw character audio

---

## 2. Core Production Techniques

### 2.1 The FL Studio Slicex Method (Industry Standard)

This is the most common professional YTPMV workflow:

1. **Find a clean sample** of the character making a vowel sound (ah, oh, eh, ee, oo)
2. **Load into Slicex** (or DirectWave, or any sampler)
3. **Detect pitch regions** — Slicex auto-detects the pitch of the sample
4. **Map to MIDI** — the sample is now playable across the piano roll
5. **Import target MIDI** — the song you want the character to "sing"
6. **Play** — the sampler pitch-shifts the sample to each MIDI note
7. **Render audio** — export the pitched audio

**Why this works**: The sampler uses high-quality pitch shifting algorithms (like Elastique or similar) that preserve formants and character. Each note is the SAME sample at a different pitch.

### 2.2 The Sony Vegas Method (Manual Pitch Shifting)

Common for creators who don't use FL Studio:

1. Find a clean clip of the character
2. Drag to timeline
3. Press **+/- keys** to pitch-shift by one semitone at a time
4. Hold **Ctrl + drag** to copy-paste the clip
5. Line up each clip with the target song's waveform
6. Repeat for every note in the melody

**Key shortcut**: Shift+Space plays from the beginning (for checking timing)

### 2.3 The Reaper Method

Similar to Vegas but with Reaper's built-in pitch shifting:
- Reaper has superior pitch shifting algorithms (Elastique Pro)
- Can apply per-item pitch shifting
- Better for complex arrangements

### 2.4 The Melodyne/Newtone Method

The most precise method:
1. Load vocal sample into Newtone
2. Newtone auto-detects pitch of each phoneme
3. Drag notes to target pitches
4. Export as MIDI or audio
5. Formant control preserves character

---

## 3. Audio Pipeline Deep Dive

### 3.1 Source Audio Requirements

**Critical**: The source audio MUST be:
- **Clean**: No background music, sound effects, or other characters
- **Steady pitch**: A sustained vowel, not a word with pitch changes
- **Good quality**: Not overly compressed or noisy
- **Long enough**: At least 0.1-0.2 seconds for short notes, longer for sustained notes

**Best source materials**:
- Character saying "ahhhh" or "ohhhh" (sustained vowels)
- Character laughing (can be chopped into segments)
- Character making exclamations ("Hey!", "What?!", "No!")
- Clean dialogue with clear vowels

**Worst source materials**:
- Words with many consonants ("strengths", "twelfths")
- Whispered speech
- Audio with background music
- Audio with sound effects overlapping
- Very short exclamations (< 0.05s)

### 3.2 Sample Selection Strategy

**The Japanese OtoMAD community's #1 rule**: 

> "Choose material with a pitch close to the pitch of that part. If you force high-pitched material to be used for bass, you have to lower the pitch a lot, so of course the original sound will disappear. If the pitch change is roughly -5 to +5 semitones when pitch-matched, it probably won't sound like complete nonsense."

**Practical implication**: You need MULTIPLE samples at different pitches:
- A "low" sample for bass notes (maybe the character's low-pitched laugh)
- A "mid" sample for mid-range notes (normal speaking voice)
- A "high" sample for treble notes (character's high-pitched exclamation)

Each sample should be within ±5 semitones of the notes it will be mapped to.

### 3.3 Pitch Shifting Quality Hierarchy

From best to worst quality preservation:

1. **Sampler playback** (Slicex, DirectWave, Kontakt) — best formant preservation
2. **Melodyne/Newtone** — excellent formant control, can edit individual phonemes
3. **Rubberband** (ffmpeg) — good quality, formant preservation available
4. **Paulstretch/Élastique** — good for extreme shifts
5. **Simple resampling** — worst, causes chipmunk effect

**Formant preservation is critical**: Formants are the resonances of the vocal tract. Without formant preservation, pitch shifting makes voices sound like chipmunks (high) or demons (low). WITH formant preservation, the voice sounds natural at any pitch.

### 3.4 Audio Rendering Pipeline

The correct order of operations:

1. **Source audio** → clean vowel sample
2. **Pitch shift** → shift to target note with formant preservation
3. **Time stretch** → adjust duration to match note length (optional, can degrade quality)
4. **Volume envelope** → fade in/out to prevent clicks
5. **EQ** → carve space if multiple samples overlap
6. **Reverb/Delay** → add spatial cohesion (subtle)
7. **Mix** → combine all pitched samples on the timeline

---

## 4. Video Editing Pipeline

### 4.1 Video-Audio Relationship

In YTPMV, the video is NOT just a background — it's synchronized to the audio:

**Option A: Video follows audio (most common)**
- Each pitched note triggers a video clip of the character
- The video clip's duration matches the note duration
- Video cuts happen on note boundaries
- The character appears to "sing" each note

**Option B: Audio follows video**
- Video is edited to a rhythm
- Audio is pitched to match the video timing
- Less common, harder to make musical

**Option C: Composite (advanced)**
- Multiple video layers, some synced to notes, some ambient
- Character cutout on green screen composited over background
- Background video changes with song sections

### 4.2 Video Timing Rules

- **Cut on note boundaries**: Each note = one video clip
- **Clip duration = note duration**: Speed up/slow down video to match
- **Minimum clip duration**: 0.05s (shorter than this looks like a glitch)
- **Maximum clip duration**: 2-3s (longer than this looks static)
- **Transition between clips**: Hard cut (no crossfade) for rhythmic precision

### 4.3 Video Source Selection

The video should show:
- The character whose voice is being used
- Ideally the character's face/mouth (for lip-sync illusion)
- Green screen if possible (for compositing)
- Multiple angles/clips for variety

### 4.4 Video Speed Adjustment

When video clip duration doesn't match note duration:
- **Speed up**: Use `setpts=0.5*PTS` (2x speed) for short notes
- **Slow down**: Use `setpts=2.0*PTS` (0.5x speed) for long notes
- **Clamp speed**: Never go beyond 0.25x-4.0x (looks unnatural)
- **Alternative**: Loop a short clip or trim a long clip to fit

---

## 5. Sample Selection & Preparation

### 5.1 Finding Clean Vowels

**Method 1: Energy + ZCR analysis**
- High energy + moderate zero-crossing rate (0.05-0.3) = vowel
- High energy + high ZCR = consonant/fricative
- Low energy = silence

**Method 2: Spectral analysis**
- Vowels show clear formant peaks at 250-3500Hz
- Consonants show broadband noise or no clear peaks

**Method 3: Listen and manually select**
- The human ear is still the best tool
- Look for sustained "ah", "oh", "eh", "ee", "oo" sounds
- Avoid plosives (p, b, t, d, k, g) and fricatives (s, f, sh)

### 5.2 Sample Preparation

1. **Extract** the clean vowel segment
2. **Trim** precisely at zero crossings (prevents clicks)
3. **Fade in/out** 2-5ms at boundaries
4. **Normalize** to -3dB peak
5. **Detect pitch** (for formant-preserving pitch shift)
6. **Catalog** by pitch range (low/mid/high)

### 5.3 Multi-Sample Strategy

For a YTPMV covering a wide pitch range (more than 1 octave):

| Sample | Native Pitch | Usable Range | Best For |
|--------|-------------|-------------|----------|
| Low sample | ~150-200Hz | ±5 semitones | Bass notes |
| Mid sample | ~250-350Hz | ±5 semitones | Mid-range notes |
| High sample | ~400-600Hz | ±5 semitones | Treble notes |

This limits pitch shifting to ±5 semitones maximum, preserving the original character.

---

## 6. Pitch Shifting: The Critical Knowledge

### 6.1 The ±5 Semitone Rule

**From the Japanese OtoMAD community (きゅう, 2024)**:

> "If the pitch of the original song and the pitch of the material are similar enough that the pitch change is roughly -5 to +5 when pitch-matched, it probably won't sound like complete nonsense. But make sure to match the key!"

This is the single most important rule for YTPMV quality.

### 6.2 Why Formant Preservation Matters

**From FL Studio Newtone manual**:

> "Formants are resonances caused by the size and shape of the vocal tract. Changing the formant will change the apparent size, age or even sex of the vocalist."

**Without formant preservation**: Pitch shifting changes the perceived size of the vocalist. High pitch = small creature (chipmunk). Low pitch = giant (demon).

**With formant preservation**: The vocal tract size stays constant. The voice sounds natural at any pitch.

### 6.3 Pitch Shifting Algorithms Compared

| Algorithm | Quality | Formant Preservation | Speed | Available In |
|-----------|---------|---------------------|-------|-------------|
| Sampler (Slicex) | ★★★★★ | Yes (built-in) | Real-time | FL Studio |
| Melodyne | ★★★★★ | Yes (full control) | Offline | Standalone/Plugin |
| Rubberband | ★★★★ | Yes (option) | Fast | ffmpeg |
| Élastique Pro | ★★★★ | Yes | Fast | Reaper |
| Paulstretch | ★★★ | Partial | Slow | Audacity |
| Simple resampling | ★ | No | Fastest | Any |

### 6.4 Practical Pitch Shifting with Rubberband (ffmpeg)

```
rubberband=pitch=RATIO:formant=preserved
```

Where RATIO = target_freq / source_freq

- RATIO > 1.0 = pitch up
- RATIO < 1.0 = pitch down
- RATIO = 1.5 = pitch up by ~7 semitones
- RATIO = 0.5 = pitch down by 1 octave

**Limitations**: 
- Maximum useful range: ±1 octave (±12 semitones)
- Best quality: ±5 semitones
- Beyond ±1 octave: quality degrades significantly

---

## 7. Video-Audio Synchronization

### 7.1 Timeline-Based Approach

The most reliable method for YTPMV:

1. Create a timeline with the target song's MIDI notes
2. For each note:
   - Place the pitched audio at the note's start time
   - Place the video clip at the same start time
   - Speed-adjust video to match note duration
3. Mix all audio together
4. Concatenate all video segments

### 7.2 Timing Precision

- **Audio placement**: Exact to the millisecond (use ffmpeg adelay)
- **Video placement**: Frame-accurate (use setpts overlay)
- **Tolerance**: ±10ms is noticeable, ±5ms is acceptable, ±2ms is tight

### 7.3 The "Child Mashing Piano" Problem

When notes are at the right pitch but don't sound musical:

**Causes**:
1. No volume envelope (clicks at note boundaries)
2. No pitch transition between notes (robotic)
3. Wrong sample for the pitch range (too much shifting)
4. No velocity variation (all notes same volume)
5. No timing humanization (too rigid)

**Solutions**:
1. Apply 2-5ms fade in/out at every note boundary
2. Add slight pitch ramp between notes (portamento)
3. Use multiple samples at different native pitches
4. Vary volume based on MIDI velocity
5. Add ±5-10ms random timing offset for human feel

---

## 8. Epilepsy & Flashing Safety

### 8.1 The Problem

Rapid video cuts between different frames can trigger photosensitive epilepsy. This is a SERIOUS concern for YTPMV because the genre inherently involves rapid visual changes synchronized to music.

### 8.2 Safety Guidelines

**Maximum flash rate**: No more than 3 flashes per second (WCAG 2.1 guideline)
**Minimum clip duration**: 0.33s (3 per second) absolute minimum
**Recommended**: 0.5s minimum for safety and aesthetics

**Avoid**:
- Rapid black/white alternation
- High-contrast strobing
- More than 3 cuts per second sustained

**Safe practices**:
- Use crossfades between clips (10-20ms)
- Keep background constant (don't flash to black)
- Use similar-colored clips in sequence
- Limit cuts to 2-3 per second

### 8.3 Implementation for Big Mac

- Default minimum clip duration: 0.5s
- Optional crossfade between clips: 20ms
- Background layer: constant (not flashing)
- Configurable max cuts per second: default 3

---

## 9. Big Mac Implementation Plan

### 9.1 Current Status (R120)

**Working**:
- MIDI file parsing (Python mido)
- Vowel sample extraction
- Rubberband pitch shifting with formant preservation
- Audio timeline mixing (ffmpeg adelay + amix)
- Video segment extraction with setpts speed adjustment
- Basic video overlay on black background

**Broken/Incomplete**:
- Video flashing to black (epilepsy risk)
- No volume envelopes (clicks at boundaries)
- No multi-sample strategy (single vowel for all pitches)
- No pitch range matching (±5 semitone rule violated)
- No velocity variation
- No crossfades between video clips
- Video doesn't show character cutout/compositing

### 9.2 Priority Fixes

**P0 — Epilepsy Safety**:
1. Add constant background (don't flash to black)
2. Add minimum clip duration (0.5s default)
3. Add crossfades between clips (20ms)

**P1 — Audio Quality**:
1. Implement multi-sample strategy (low/mid/high samples)
2. Apply volume envelopes (fade in/out at boundaries)
3. Limit pitch shift to ±5 semitones per sample
4. Add velocity-based volume variation

**P2 — Video Quality**:
1. Implement video crossfades (xfade filter)
2. Add character cutout via chroma key
3. Multiple video source cycling
4. Lip-sync approximation (mouth open/close per note)

**P3 — Advanced**:
1. Portamento between notes
2. Timing humanization (±5-10ms random offset)
3. Reverb/delay for spatial cohesion
4. EQ for frequency carving

### 9.3 Architecture

```
┌─────────────────────────────────────────┐
│           YTPMV Production Tool          │
├─────────────────────────────────────────┤
│                                         │
│  ┌─────────┐    ┌──────────────────┐   │
│  │ MIDI In │───>│ Note Extraction  │   │
│  └─────────┘    └────────┬─────────┘   │
│                          │              │
│  ┌─────────┐    ┌────────▼─────────┐   │
│  │ Audio   │───>│ Sample Selection │   │
│  │ Source  │    │ (pitch-matched)  │   │
│  └─────────┘    └────────┬─────────┘   │
│                          │              │
│                 ┌────────▼─────────┐   │
│                 │ Pitch Shift      │   │
│                 │ (rubberband,     │   │
│                 │  formant=preserved│   │
│                 │  ±5 semitones)   │   │
│                 └────────┬─────────┘   │
│                          │              │
│  ┌─────────┐    ┌────────▼─────────┐   │
│  │ Video   │───>│ Video Segment    │   │
│  │ Source  │    │ Extraction       │   │
│  └─────────┘    │ (speed-adjusted) │   │
│                 └────────┬─────────┘   │
│                          │              │
│                 ┌────────▼─────────┐   │
│                 │ Timeline Mixer   │   │
│                 │ (adelay + amix   │   │
│                 │  for audio,      │   │
│                 │  overlay/xfade   │   │
│                 │  for video)      │   │
│                 └────────┬─────────┘   │
│                          │              │
│                 ┌────────▼─────────┐   │
│                 │ Final Render     │   │
│                 │ (audio + video   │   │
│                 │  merge)          │   │
│                 └──────────────────┘   │
│                                         │
└─────────────────────────────────────────┘
```

### 9.4 Sample Catalog Strategy

For each YTPMV source character, we need:

```
character_name/
  low_sample.wav     # ~150-200Hz native pitch
  mid_sample.wav     # ~250-350Hz native pitch  
  high_sample.wav    # ~400-600Hz native pitch
  video_clips/       # Collection of video segments
    face_01.mp4
    face_02.mp4
    full_body_01.mp4
    green_screen_01.mp4
  metadata.json      # Pitch info, source timing, etc.
```

---

## 10. Research Sources

### Primary Sources
1. **FL Studio Newtone Manual** — Image-Line official documentation on pitch editing, formant control, and the Slicex workflow
2. **"How to preserve the original sound in YTPMV and OtoMAD"** — きゅう (Kyu), Japanese OtoMAD creator, 2024. The ±5 semitone rule and sample selection strategy
3. **"Make ANY Audio Clip Into an Instrument!"** — Cantersoft, YouTube tutorial on the Slicex YTPMV workflow
4. **"Basic YTPMV tutorial (Sony Vegas)"** — Oasty, YouTube, 2014. The manual +/- key method
5. **Reddit r/YTPMV** — Community discussions on software, techniques, and workflow

### Secondary Sources
6. **Melodigging YTPMV genre page** — Description of YTPMV production techniques
7. **Reddit r/FL_Studio** — Pitch shifting samples on piano roll
8. **Reddit r/Reaper** — YTPMV production in Reaper
9. **Sony Vegas Creative Blog** — Syncing cuts to musical notes
10. **Newtone Video Tutorial series** — Image-Line official YouTube playlist

### Technical References
11. **ffmpeg rubberband filter docs** — pitch shifting with formant preservation
12. **Rubberband library docs** — Time-stretching and pitch-shifting engine
13. **mido Python library** — MIDI file parsing
14. **WCAG 2.1** — Flash rate safety guidelines (epilepsy)

### Japanese Sources (OtoMAD community)
15. **note.com/54654658** — きゅう's comprehensive guide to preserving original sound
16. **音MAD tutorial playlist** — YouTube, Japanese language

---

## Appendix A: Key Terminology

| Term | Definition |
|------|-----------|
| **Formant** | Resonance frequencies of the vocal tract; determines voice character |
| **Phoneme** | Smallest unit of speech sound |
| **Viseme** | Visual representation of a phoneme (mouth shape) |
| **Slicex** | FL Studio sampler plugin for chopping and mapping samples |
| **Newtone** | FL Studio pitch correction/editing plugin |
| **Rubberband** | Open-source time-stretching/pitch-shifting library |
| **Elastique** | zplane's proprietary pitch-shifting algorithm |
| **OtoMAD** (音MAD) | Japanese remix culture, YTPMV equivalent |
| **UTAU** | Free Japanese vocal synthesizer |
| **RVC** | Retrieval-based Voice Conversion (AI voice cloning) |
| **setpts** | ffmpeg filter for changing video playback speed |
| **atempo** | ffmpeg filter for changing audio playback speed |
| **adelay** | ffmpeg filter for delaying audio |
| **amix** | ffmpeg filter for mixing multiple audio streams |
| **overlay** | ffmpeg filter for compositing video layers |
| **xfade** | ffmpeg filter for crossfading between video clips |

## Appendix B: ffmpeg Filter Reference for YTPMV

```bash
# Pitch shift with formant preservation
-af "rubberband=pitch=1.5:formant=preserved"

# Video speed change (0.5x = half speed, 2.0x = double speed)
-vf "setpts=0.5*PTS"

# Audio speed change (chain for values outside 0.5-2.0)
-af "atempo=2.0,atempo=1.5"

# Delay audio by N milliseconds
-af "adelay=1000|1000"  # 1 second delay

# Mix multiple audio inputs
-filter_complex "[0:a][1:a]amix=inputs=2:duration=longest"

# Overlay video at specific time
-filter_complex "[0:v][1:v]overlay=enable='between(t,1.0,2.0)'"

# Crossfade between videos
-filter_complex "[0:v][1:v]xfade=transition=fade:duration=0.02:offset=1.0"

# Fade in/out audio
-af "afade=t=in:st=0:d=0.005,afade=t=out:st=0.095:d=0.005"
```

## Appendix C: The ±5 Semitone Rule in Practice

Given a target MIDI note at frequency F_target, and a source sample at frequency F_source:

1. Calculate required shift: `semitones = 12 * log2(F_target / F_source)`
2. If `|semitones| <= 5`: Use this sample directly
3. If `semitones > 5`: Use a higher-pitched sample
4. If `semitones < -5`: Use a lower-pitched sample

**Example**: 
- Target note: C5 (523Hz)
- Sample 1: "ah" at 262Hz (C4) → shift = +12 semitones → TOO MUCH
- Sample 2: "oh" at 440Hz (A4) → shift = +3.2 semitones → GOOD
- Sample 3: "ee" at 659Hz (E5) → shift = -3.8 semitones → GOOD

Choose Sample 2 or 3 (within ±5 semitones).

---

*This document is a living reference. Update as new techniques are discovered and implemented.*
*Next review: After implementing P0 (epilepsy safety) and P1 (audio quality) fixes.*
