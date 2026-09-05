# YTPMV Video Effects, Sync & Compositing — Complete Knowledge Base

**Compiled: 2026-09-05**
**Method: 7-hop recursive research, 25+ sources, Triple-DA cross-referencing**
**Previous: docs/R121-ytpmv-production-knowledge-base.md**

---

## Table of Contents

1. [Video Editing Pipeline for YTPMV](#video-editing-pipeline)
2. [Audio-Video Synchronization](#audio-video-sync)
3. [Beat Detection & Onset Analysis](#beat-detection)
4. [Video Transitions & Crossfades](#transitions)
5. [Chroma Key & Compositing](#chroma-key)
6. [Audio-Reactive Video Effects](#audio-reactive)
7. [Epilepsy Safety & Flash Prevention](#epilepsy-safety)
8. [Sample Selection & Cataloging](#sample-selection)
9. [Video Effects Tracks](#effects-tracks)
10. [ffmpeg Complete Filter Reference](#ffmpeg-reference)
11. [Research Sources](#research-sources)

---

## 1. Video Editing Pipeline for YTPMV

### 1.1 The Three Standard Workflows

#### Sony Vegas Pro (Most Common for YTPMV)

Sony Vegas is the most widely used NLE for YTPMV production. Key features:
- **Trim window**: Precise audio/event selection with I/O points
- **+/- keys**: Pitch-shift audio by one semitone
- **Ctrl+Drag**: Copy-paste clips on timeline
- **Shift+Space**: Play from beginning for timing checks
- **Hold Shift+Scroll**: Horizontal scroll
- **Hold Ctrl+Scroll**: Zoom in/out
- **Hold Shift+Drag**: Disable snapping for frame-accurate placement

**Standard Vegas YTPMV Workflow**:
1. Import source video to trimmer
2. Find clean audio segment (vowel sound)
3. Drag to timeline
4. Press +/- to pitch-shift to target note
5. Line up with target song waveform
6. Ctrl+Drag to copy for next note
7. Repeat for entire melody
8. Add reverb/delay to audio track
9. Render

#### FL Studio (Audio-First Approach)

FL Studio is used when the audio is the primary focus and video is secondary:
1. Load sample into Slicex or DirectWave
2. Detect pitch regions
3. Map to MIDI keyboard
4. Import target MIDI file
5. Render audio
6. Import rendered audio into video editor
7. Sync video to rendered audio

#### After Effects (Compositing-Focused)

After Effects is used for advanced compositing:
- **Audio Convert to Keyframes**: Auto-generates keyframes from audio amplitude
- **Time remapping**: Expression-driven video speed from audio
- **Motion tracking**: Track character movement
- **Green screen keying**: Advanced chroma key with spill suppression

### 1.2 Video Editing Principles for YTPMV

**Rule 1: Cut on Note Boundaries**
Every note in the melody = one video clip. The video changes when the pitch changes.

**Rule 2: Clip Duration = Note Duration**
Speed up or slow down the video clip to match the exact length of the audio note.

**Rule 3: Hard Cuts for Rhythm**
No transitions between most notes — hard cuts create the rhythmic feel. Exceptions: sustained notes, section transitions.

**Rule 4: Visual Variety**
Don't use the same video clip for every note. Cycle through 3-5 different clips to maintain visual interest.

**Rule 5: Character Focus**
The video should show the character whose voice is being used. Face/close-up shots work best for the "singing" illusion.

### 1.3 Video Timing Calculations

```
Given:
  note_start = 1.5s (from MIDI)
  note_duration = 0.25s (from MIDI)
  source_clip_duration = 0.5s (original video clip)

Video speed factor = source_clip_duration / note_duration
                   = 0.5 / 0.25 = 2.0x speed

ffmpeg: -vf "setpts=0.5*PTS"  (0.5 = 1/speed = 1/2.0)
```

**Clamp speeds to 0.25x - 4.0x range**. Beyond this, video looks unnatural.

**Minimum clip duration**: 0.33s (3 per second) for epilepsy safety
**Maximum clip duration**: 2-3s (longer looks static)

---

## 2. Audio-Video Synchronization

### 2.1 Synchronization Strategies

#### Strategy A: Audio-Driven Video (Most Common)

The audio is created first (pitched samples), then video is synced to match:

1. Create pitched audio for each note
2. Place audio on timeline at correct positions
3. For each audio segment, extract a video clip
4. Speed-adjust video to match audio duration
5. Place video at same timeline position as audio

**Advantage**: Perfect audio timing, video follows

#### Strategy B: Video-Driven Audio

The video is edited first, then audio is created to match:

1. Edit video to desired rhythm/timing
2. Detect beat positions from video cuts
3. Create pitched audio for each video segment
4. Place audio to match video timing

**Advantage**: Perfect visual timing, audio follows

#### Strategy C: MIDI-Driven (Most Precise)

Both audio and video are driven by a MIDI file:

1. Parse MIDI file for note events (start, duration, pitch)
2. For each note:
   a. Pitch-shift audio sample to note pitch
   b. Extract video clip and speed to note duration
   c. Place both at note start time
3. Mix all audio together
4. Composite all video together

**Advantage**: Perfect A/V sync, reproducible, editable

### 2.2 Timing Precision Requirements

| Tolerance | Quality | Use Case |
|-----------|---------|----------|
| ±2ms | Tight | Professional YTPMV |
| ±5ms | Acceptable | Good YTPMV |
| ±10ms | Noticeable | Amateur |
| ±20ms | Loose | Unacceptable |

### 2.3 ffmpeg Synchronization Methods

#### Audio Delay (adelay filter)
```
-af "adelay=1500|1500"  # Delay by 1500ms (stereo)
```

#### Video Offset (setpts with overlay)
```
-filter_complex "[1:v]overlay=enable='between(t,1.5,1.75)'"  # Show at t=1.5-1.75s
```

#### Frame-Accurate Placement
```
-filter_complex "[1:v]setpts=PTS+1.5/TB[out]"  # Offset video by 1.5s
```

---

## 3. Beat Detection & Onset Analysis

### 3.1 Algorithms Available

#### aubio (Command Line, Best for YTPMV)

```bash
# Detect onsets (note starts)
aubio onset input.wav
# Output: 0.120 0.510 1.010 1.520 ...

# Detect beats
aubio beat input.wav
# Output: 0.500 1.000 1.500 2.000 ...

# Detect tempo (BPM)
aubio tempo input.wav
# Output: 120.5

# Detect pitch
aubio pitch input.wav
# Output: 0.000 440.0
#         0.120 523.2

# Detect notes (onset + pitch)
aubionotes input.wav
# Output: 0.120 60 100
#         (time midi_note velocity)

# Cut audio at onsets
aubiocut input.wav -c -o output_dir/
```

#### Essentia (Python Library)

```python
import essentia.standard as es

audio = es.MonoLoader(filename="input.wav")()
bpm, beats, confidence, _, _ = es.RhythmExtractor2013()(audio)
```

#### ffmpeg (Built-in)

```bash
# Detect volume levels per frame
ffmpeg -i input.wav -af "volumedetect" -f null /dev/null

# Show audio waveform as video
ffmpeg -i input.wav -filter_complex "showwaves=s=1280x720" waveform.mp4

# Audio spectrum visualization
ffmpeg -i input.wav -filter_complex "showspectrum=s=1280x720" spectrum.mp4
```

### 3.2 Onset Detection Methods (aubio)

| Method | Best For | Sensitivity |
|--------|----------|-------------|
| default (hfc) | Percussive sounds | Medium |
| energy | General purpose | High |
| hfc | Percussive/plucks | High |
| complex | Polyphonic music | Medium |
| phase | Complex signals | Medium |
| specdiff | Music with drums | High |
| kl | Speech | Low |
| mkl | Noisy audio | Low |
| specflux | Music with drums | High |

**Recommendation for YTPMV**: Use `specflux` or `hfc` for detecting character voice onsets. Use `energy` for general purpose.

### 3.3 Tempo Estimation

```bash
# aubio tempo
aubio tempo input.wav

# aubio beat (with positions)
aubio beat input.wav

# ffmpeg (indirect, via silencedetect for gaps)
ffmpeg -i input.wav -af "silencedetect=noise=-30dB:d=0.1" -f null /dev/null
```

---

## 4. Video Transitions & Crossfades

### 4.1 ffmpeg xfade Filter

The `xfade` filter crossfades between two video clips. **Essential for epilepsy-safe YTPMV**.

#### Available Transitions (44 total)

| Category | Transitions |
|----------|------------|
| **Basic** | fade, fadeblack, fadewhite, fadegrays |
| **Wipe** | wipeleft, wiperight, wipeup, wipedown |
| **Slide** | slideleft, slideright, slideup, slidedown |
| **Crop** | cropzoom, diagtl, diagtr, diagbl, diagbr |
| **Dissolve** | dissolve, pixelize, diagtile, radialsqueeze |
| **Special** | hlslice, vuslice, hrslice, vdslice, hblur |
| **Advanced** | circlecrop, rectcrop, distance, fadeblack, fadewhite |

#### Basic Usage

```bash
# Crossfade between two clips, 0.5s transition at 3.5s offset
ffmpeg -i clip1.mp4 -i clip2.mp4 \
  -filter_complex "[0:v][1:v]xfade=transition=fade:duration=0.5:offset=3.5" \
  output.mp4

# With audio crossfade
ffmpeg -i clip1.mp4 -i clip2.mp4 \
  -filter_complex "[0:v][1:v]xfade=transition=fade:duration=0.5:offset=3.5[v];
                   [0:a][1:a]acrossfade=d=0.5:c1=tri:c2=tri[a]" \
  -map "[v]" -map "[a]" output.mp4
```

#### Chaining Multiple Clips

```bash
# Three clips with crossfades
ffmpeg -i clip0.mp4 -i clip1.mp4 -i clip2.mp4 \
  -filter_complex "
    [0][1]xfade=transition=fade:duration=0.5:offset=3.5[v01];
    [v01][2]xfade=transition=fade:duration=0.5:offset=7.0[vout];
    [0:a][1:a]acrossfade=d=0.5:a01];
    [a01][2:a]acrossfade=d=0.5:aout]
  " -map "[vout]" -map "[aout]" output.mp4
```

**Offset calculation**: offset = sum_of_previous_clip_durations - transition_duration

### 4.2 Audio Crossfade (acrossfade)

```bash
# Crossfade audio by 0.5s with triangular volume curve
-af "acrossfade=d=0.5:c1=tri:c2=tri"

# Parameters:
#   d = duration of crossfade
#   c1 = volume curve for first input (tri, qsin, hsin, etc.)
#   c2 = volume curve for second input
```

### 4.3 Fade In/Out

```bash
# Video fade in (first 0.5s)
-vf "fade=t=in:st=0:d=0.5"

# Video fade out (last 0.5s)
-vf "fade=t=out:st=9.5:d=0.5"

# Audio fade in/out
-af "afade=t=in:st=0:d=0.005,afade=t=out:st=0.145:d=0.005"
```

### 4.4 Epilepsy-Safe Transition Guidelines

**Minimum transition duration**: 20ms (1 frame at 50fps)
**Recommended**: 50-100ms for smooth cuts
**Maximum**: 500ms (longer feels sluggish)

**Safe transition types** (low flash risk):
- `fade` — smooth opacity transition
- `fadegrays` — fade through gray (not black/white)
- `dissolve` — pixel dissolve

**Unsafe transition types** (high flash risk):
- `fadeblack` — flashes to black
- `fadewhite` — flashes to white (WORST)
- `pixelize` — can cause strobing
- `diagtile` — complex pattern changes

---

## 5. Chroma Key & Compositing

### 5.1 Green Screen Removal

#### ffmpeg chromakey filter

```bash
# Basic green screen removal
-vf "chromakey=0x00FF00:0.3:0.1"
# Parameters: color:similarity:blend

# Composite over background
-filter_complex "
  [1:v]chromakey=0x00FF00:0.3:0.1[fg];
  [0:v][fg]overlay=0:0
"
```

#### ffmpeg colorkey filter

```bash
# More precise color keying
-vf "colorkey=0x00FF00:0.3:0.1"
# Parameters: color:similarity:blend
```

### 5.2 Compositing Pipeline

```
1. Extract character from green screen
   → chromakey filter → alpha matte

2. Scale/position character
   → scale filter → reposition

3. Overlay on background
   → overlay filter → composited frame

4. Add shadows/highlights (optional)
   → color correction → final output
```

### 5.3 Advanced Compositing with Spill Suppression

```bash
# Remove green spill on edges
-filter_complex "
  [1:v]chromakey=0x00FF00:0.25:0.05,
  colorchannelmixer=gg=0.8:bg=0.8[fg];
  [0:v][fg]overlay=100:50
"
```

### 5.4 Character Cutout Without Green Screen

For sources without green screen, options are:
1. **AI segmentation** (MediaPipe, RVM) — person segmentation
2. **Rotoscoping** — manual frame-by-frame masking
3. **Background subtraction** — compare to reference frame
4. **Luma key** — key out dark/light backgrounds

---

## 6. Audio-Reactive Video Effects

### 6.1 Audio Waveform Visualization

```bash
# Animated waveform
ffmpeg -i audio.wav -filter_complex "
  [0:a]showwaves=s=1280x720:mode=cline:colors=white|blue:scale=sqrt
" waveform.mp4

# Waveform with custom colors
ffmpeg -i audio.wav -filter_complex "
  [0:a]showwaves=s=1920x400:mode=cline:colors=red|blue:scale=sqrt,
  colorkey=black:0.1:0.1[w];
  color=c=white:s=1920x400[bg];
  [bg][w]overlay
" waveform_bg.mp4
```

### 6.2 Audio Spectrum Visualization

```bash
# Spectrum analyzer
ffmpeg -i audio.wav -filter_complex "
  [0:a]showspectrum=s=1280x720:mode=combined:color=fire:scale=log
" spectrum.mp4

# Multi-band spectrum
ffmpeg -i audio.wav -filter_complex "
  asplit=3[a1][a2][a3];
  [a1]lowpass=300,showwaves=s=640x200:colors=red[l];
  [a2]bandpass=f=1000:width_type=h:w=1400,showwaves=s=640x200:colors=green[m];
  [a3]highpass=3000,showwaves=s=640x200:colors=blue[h];
  [l][m][h]vstack=inputs=3
" multiband.mp4
```

### 6.3 Audio-Reactive Video Scaling

The concept: video scale/position reacts to audio amplitude.

**ffmpeg approach** (using zoompan with audio):
```bash
# This requires generating keyframes from audio, then applying zoompan
# Step 1: Generate volume data
ffmpeg -i audio.wav -af "volumedetect" -f null /dev/null 2> vol.txt

# Step 2: Create zoom expression from volume data
# (Requires custom script to convert volume → zoom expression)
```

**After Effects approach** (easier):
1. Convert Audio to Keyframes
2. Apply expression to scale property: `transform.scale = [100 + audioLevel * 50, 100 + audioLevel * 50]`

### 6.4 Beat-Synced Zoom Pulses

```bash
# Zoom pulse on each beat (requires knowing beat times)
# For beats at t=0.5, 1.0, 1.5, 2.0:
ffmpeg -i video.mp4 -vf "
  zoompan=z='1+0.2*sin(on*2*PI/20)':x='iw/2-(iw/zoom/2)':y='ih/2-(ih/zoom/2)'
" output.mp4
```

### 6.5 Audio Phase Visualization

```bash
# Goniometer (stereo phase correlation)
ffmpeg -i stereo.wav -filter_complex "
  [0:a]aphasemeter=s=1920x1080:mpc=red
" phase.mp4

# Vector scope
ffmpeg -i stereo.wav -filter_complex "
  [0:a]avectorscope=s=1920x1080:draw=line
" vectorscope.mp4
```

### 6.6 Advanced Audio Visualization

```bash
# Constant Q Transform (musical note visualization)
ffmpeg -i audio.wav -filter_complex "
  [0:a]showcqt=s=1920x1080:count=6:sono_h=0
" cqt.mp4

# Continuous Wavelet Transform
ffmpeg -i audio.wav -filter_complex "
  [0:a]showcwt=s=1920x1080:direction=ud
" cwt.mp4

# 3D Audio Scope
ffmpeg -i audio.wav -filter_complex "
  a3dscope=rate=30:s=1920x1080:fov=90:roll=1:pitch=0.5:yaw=0.3
" 3dscope.mp4
```

---

## 7. Epilepsy Safety & Flash Prevention

### 7.1 WCAG 2.1 Guidelines

**Maximum flash rate**: 3 flashes per second (fps) sustained
**Definition of flash**: Simultaneous contrast reversal of 10%+ of screen area
**Red flash**: Especially dangerous (red flashes at >3Hz can trigger seizures)

### 7.2 YTPMV-Specific Safety Rules

1. **Minimum clip duration**: 0.5s (2 cuts per second max for safety margin)
2. **Maximum sustained cut rate**: 3 cuts per second (absolute max)
3. **Use crossfades**: 20-50ms between all clips
4. **Avoid black/white flashes**: Use `fadegrays` or `dissolve` instead of `fadeblack`/`fadewhite`
5. **Constant background**: Keep a video layer that never changes
6. **No red flashes**: Avoid red color transitions
7. **Test with flash detection**: Use tools like PEAT (Photosensitive Epilepsy Analysis Tool)

### 7.3 Safe Video Pipeline Architecture

```
┌─────────────────────────────────────────────┐
│           Background Layer (constant)        │
│  (Static image or slow-moving video)         │
├─────────────────────────────────────────────┤
│           Character Layer (changing)         │
│  (Crossfaded clips, min 0.5s each)          │
├─────────────────────────────────────────────┤
│           Effects Layer (optional)           │
│  (Subtle overlays, no flashing)             │
└─────────────────────────────────────────────┘
```

**Key principle**: The background layer NEVER changes rapidly. Only the foreground character layer changes, and it uses crossfades.

### 7.4 ffmpeg Safety Filters

```bash
# Minimum clip duration enforcement
# (Skip clips shorter than 0.5s by duplicating previous frame)

# Crossfade between all clips (20ms)
xfade=transition=fade:duration=0.02:offset=...

# Fade through gray (not black)
xfade=transition=fadegrays:duration=0.05

# Add constant background
-filter_complex "
  color=c=0x333333:s=1920x1080:d=10[bg];
  [bg][0:v]overlay=shortest=1[bgv];
  [bgv][1:v]overlay=enable='between(t,1.0,1.5)'[out]
"
```

---

## 8. Sample Selection & Cataloging

### 8.1 Finding Clean Vowels (Algorithmic)

```python
import numpy as np
from scipy.io import wavfile

sr, data = wavfile.read("source.wav")
data = data.astype(float) / 32768.0

# Parameters
window_size = int(sr * 0.01)  # 10ms windows
hop_size = window_size // 2

vowel_segments = []

for i in range(0, len(data) - window_size, hop_size):
    chunk = data[i:i + window_size]
    
    # Energy
    energy = np.sum(chunk ** 2) / window_size
    
    # Zero-crossing rate
    crossings = np.sum(np.diff(np.sign(chunk)) != 0)
    zcr = crossings / window_size
    
    # Spectral centroid (brightness)
    spectrum = np.abs(np.fft.rfft(chunk))
    freqs = np.fft.rfftfreq(len(chunk), 1.0/sr)
    if np.sum(spectrum) > 0:
        centroid = np.sum(freqs * spectrum) / np.sum(spectrum)
    else:
        centroid = 0
    
    # Vowel detection criteria:
    # - High energy (> threshold)
    # - Moderate ZCR (0.05-0.3 for vowels)
    # - Spectral centroid in vowel range (200-3500Hz)
    time = i / sr
    
    if energy > 0.001 and 0.05 < zcr < 0.3 and 200 < centroid < 3500:
        vowel_segments.append((time, energy, zcr, centroid))

# Merge adjacent segments
merged = []
if vowel_segments:
    start = vowel_segments[0][0]
    end = start
    for time, _, _, _ in vowel_segments[1:]:
        if time - end < 0.05:  # Within 50ms = same segment
            end = time
        else:
            if end - start > 0.05:  # Min 50ms duration
                merged.append((start, end))
            start = time
            end = time
    merged.append((start, end))

print(f"Found {len(merged)} vowel segments:")
for start, end in merged:
    print(f"  t={start:.3f}s - {end:.3f}s (dur={end-start:.3f}s)")
```

### 8.2 Sample Catalog Format

```json
{
  "character": "SpongeBob",
  "source": "spongebob_s01e05_420.mp4",
  "samples": [
    {
      "id": "low_ah",
      "file": "spongebob_low_ah.wav",
      "start_time": 1.7,
      "duration": 0.15,
      "native_pitch_hz": 180,
      "native_pitch_midi": 50,
      "usable_range": {"min": 45, "max": 55},
      "type": "vowel",
      "quality": 0.9,
      "notes": "Clean 'ah' sound, no background"
    },
    {
      "id": "mid_oh",
      "file": "spongebob_mid_oh.wav",
      "start_time": 3.2,
      "duration": 0.20,
      "native_pitch_hz": 280,
      "native_pitch_midi": 57,
      "usable_range": {"min": 52, "max": 62},
      "type": "vowel",
      "quality": 0.85,
      "notes": "Sustained 'oh' sound"
    }
  ],
  "video_clips": [
    {
      "id": "face_closeup",
      "file": "spongebob_face_01.mp4",
      "start_time": 0.0,
      "duration": 2.0,
      "type": "closeup",
      "has_greenscreen": false
    }
  ]
}
```

### 8.3 The ±5 Semitone Rule Implementation

```python
def select_sample(target_midi, samples):
    """Select the best sample for a target MIDI note."""
    best = None
    best_distance = 999
    
    for sample in samples:
        native_midi = sample["native_pitch_midi"]
        distance = abs(target_midi - native_midi)
        
        if distance > 5:
            continue  # Skip samples outside ±5 semitone range
            
        if distance < best_distance:
            best_distance = distance
            best = sample
    
    if best is None:
        # Fallback: use closest sample even if outside range
        best = min(samples, key=lambda s: abs(target_midi - s["native_pitch_midi"]))
    
    return best, best_distance

def compute_pitch_shift(sample, target_midi):
    """Compute rubberband pitch ratio."""
    target_freq = 440.0 * (2.0 ** ((target_midi - 69) / 12.0))
    source_freq = sample["native_pitch_hz"]
    ratio = target_freq / source_freq
    return max(0.33, min(ratio, 3.0))  # Clamp to valid range
```

---

## 9. Video Effects Tracks

### 9.1 Track Architecture

A professional YTPMV has multiple video tracks:

```
Track 5: Subtitles/Text (optional)
Track 4: Effects/Overlays (beat-synced flashes, particles)
Track 3: Character Cutout (chroma keyed character on transparent bg)
Track 2: Character Base (full clips with crossfades)
Track 1: Background (constant, never flashes)
```

### 9.2 Effects Track Types

#### Beat-Synced Zoom
```bash
# Zoom in on each beat
zoompan=z='1+0.1*sin(on*2*PI/10)':x='iw/2-(iw/zoom/2)':y='ih/2-(ih/zoom/2)'
```

#### Beat-Synced Color Flash
```bash
# Colorize on each beat (subtle)
hue='H=2*PI*t:b=1+0.3*sin(on*2*PI/10)'
```

#### Beat-Synced Rotation
```bash
# Slight rotation on each beat
rotate='PI/36*sin(on*2*PI/10)'  # ±5 degree wobble
```

#### Stutter/Gross Beat Effect
```bash
# Repeat frames for stutter effect
# Requires segmenting the video and duplicating segments
```

### 9.3 Audio-to-Video Effect Pipeline

```
1. Analyze audio → detect onsets/beats
2. Generate keyframes from beat times
3. Apply keyframes to video effect parameters
4. Render video with effects
5. Composite with character video
6. Output final video
```

### 9.4 Luma Key for Text/Overlays

```bash
# Key out dark areas to reveal overlay
-filter_complex "
  [0:v]lumakey=min=200[fg];
  [1:v][fg]overlay
"
```

---

## 10. ffmpeg Complete Filter Reference for YTPMV

### 10.1 Audio Filters

| Filter | Purpose | Example |
|--------|---------|---------|
| `adelay` | Delay audio | `adelay=1000\|1000` |
| `aecho` | Echo effect | `aecho=0.8:0.9:1000:0.3` |
| `afade` | Fade in/out | `afade=t=in:st=0:d=0.005` |
| `amix` | Mix audio streams | `amix=inputs=3:duration=longest` |
| `rubberband` | Pitch shift | `rubberband=pitch=1.5:formant=preserved` |
| `atempo` | Speed change | `atempo=1.5` |
| `volume` | Volume adjust | `volume=0.8` |
| `acompressor` | Compression | `acompressor=threshold=-20dB:ratio=4` |
| `aecho` | Reverb/echo | `aecho=0.8:0.88:60:0.4` |
| `highpass` | Remove low freq | `highpass=f=200` |
| `lowpass` | Remove high freq | `lowpass=f=3000` |
| `equalizer` | EQ | `equalizer=f=1000:width_type=h:w=200:g=3` |
| `showwaves` | Waveform video | `showwaves=s=1280x720` |
| `showspectrum` | Spectrum video | `showspectrum=s=1280x720` |
| `showcqt` | Musical note video | `showcqt=s=1920x1080` |
| `volumedetect` | Volume analysis | `volumedetect` |
| `silencedetect` | Silence detection | `silencedetect=noise=-30dB:d=0.1` |

### 10.2 Video Filters

| Filter | Purpose | Example |
|--------|---------|---------|
| `setpts` | Speed change | `setpts=0.5*PTS` (2x speed) |
| `overlay` | Composit layers | `overlay=100:50` |
| `xfade` | Crossfade | `xfade=transition=fade:d=0.5:offset=3` |
| `fade` | Fade in/out | `fade=t=in:st=0:d=0.5` |
| `chromakey` | Green screen | `chromakey=0x00FF00:0.3:0.1` |
| `colorkey` | Color key | `colorkey=0x00FF00:0.3:0.1` |
| `scale` | Resize | `scale=640:480` |
| `zoompan` | Zoom/pan | `zoompan=z='1+0.1*t'` |
| `rotate` | Rotation | `rotate=PI/4` |
| `hue` | Color adjust | `hue=H=2*PI*t` |
| `eq` | Brightness/contrast | `eq=brightness=0.1:contrast=1.2` |
| `boxblur` | Blur | `boxblur=5:1` |
| `gblur` | Gaussian blur | `gblur=sigma=3` |
| `drawtext` | Text overlay | `drawtext=text='Hello':x=10:y=10` |
| `crop` | Crop region | `crop=640:480:100:50` |
| `pad` | Add borders | `pad=1920:1080:0:0:black` |
| `colorchannelmixer` | Channel adjust | `colorchannelmixer=rr=1:rg=0:rb=0` |
| `vflip`/`hflip` | Flip | `vflip` |
| `transpose` | Rotate 90/180/270 | `transpose=1` |

### 10.3 Complex Filter Graphs for YTPMV

#### Single Note with Audio + Video
```bash
ffmpeg \
  -i background.mp4 \
  -ss 1.0 -t 0.25 -i character.mp4 \
  -ss 2.0 -t 0.15 -i vowel.wav \
  -filter_complex "
    [1:v]setpts=0.6*PTS[char];
    [2:a]rubberband=pitch=1.5:formant=preserved,
    afade=t=in:st=0:d=0.005,
    afade=t=out:st=0.145:d=0.005,
    adelay=0|0[audio];
    [0:v][char]overlay=enable='between(t,0,0.25)':x=100:y=50[video];
    [video][audio]concat=n=1:v=1:a=1[outv][outa]
  " \
  -map "[outv]" -map "[outa]" output.mp4
```

#### Multiple Notes with Crossfades
```bash
# For each note, create a segment, then xfade between them
# See Section 4.1 for chaining pattern
```

#### Full YTPMV Pipeline
```bash
# 1. Create background
ffmpeg -f lavfi -i "color=c=0x222222:s=1920x1080:d=10" bg.mp4

# 2. For each note, create pitched audio + video segment
# (Loop in Python/bash)

# 3. Mix all audio with adelay
ffmpeg -i seg_a0.wav -i seg_a1.wav ... \
  -filter_complex "
    [0:a]adelay=0|0[a0];
    [1:a]adelay=120|120[a1];
    ...
    [a0][a1]...amix=inputs=N:duration=longest[aout]
  " -map "[aout]" mixed_audio.m4a

# 4. Composite all video with overlay
ffmpeg -i bg.mp4 -i seg_v0.mp4 -i seg_v1.mp4 ... \
  -filter_complex "
    [1:v]setpts=PTS+0.0/TB[v0];
    [2:v]setpts=PTS+0.12/TB[v1];
    ...
    [0:v][v0]overlay=enable='between(t,0,0.12)'[v01];
    [v01][v1]overlay=enable='between(t,0.12,0.37)'[vout]
  " -map "[vout]" composited_video.mp4

# 5. Merge audio + video
ffmpeg -i composited_video.mp4 -i mixed_audio.m4a \
  -c:v copy -c:a aac output.mp4
```

---

## 11. Research Sources

### Primary Sources
1. **ffmpeg xfade documentation** — Official ffmpeg filter docs with 44 transition types
2. **ffmpeg chromakey documentation** — Green screen removal with similarity/blend parameters
3. **aubio command line tools** — onset, beat, tempo, pitch, notes detection
4. **Essentia beat detection** — RhythmExtractor2013, PercivalBpmEstimator
5. **FL Studio Newtone manual** — Pitch correction, formant control, Slicex workflow
6. **Sony Vegas Creative Blog** — Syncing cuts to musical notes
7. **"How to preserve the original sound in YTPMV and OtoMAD"** — きゅう, Japanese OtoMAD creator

### Secondary Sources
8. **Stack Overflow: ffmpeg xfade chaining** — Multi-clip crossfade patterns
9. **Super User: ffmpeg crossfade** — offset parameter explanation
10. **Ottverse: ffmpeg xfade guide** — Transition types and parameters
11. **ffmpeg-micro.com: chromakey guide** — Green screen removal patterns
12. **S Anand: ffmpeg creative capabilities** — Audio-reactive visualizations
13. **Beat2Cut: Automatic beat detection** — Onset detection algorithms
14. **Reddit r/YTPMV** — Community software recommendations
15. **Reddit r/FL_Studio** — Gross Beat stutter effects

### Technical References
16. **WCAG 2.1** — Flash rate safety guidelines
17. **aubio manual pages** — aubioonset, aubiopitch, aubiotrack, aubiocut
18. **Essentia tutorials** — Beat detection and tempo estimation
19. **ffmpeg filters documentation** — Complete filter reference
20. **audiojs/beat** — JavaScript tempo/onset detection algorithms

### Video Effect References
21. **ffmpeg showcqt** — Constant Q Transform (musical note visualization)
22. **ffmpeg showspectrum** — Spectrum analyzer visualization
23. **ffmpeg showwaves** — Waveform visualization
24. **ffmpeg a3dscope** — 3D audio scope
25. **ffmpeg aphasemeter** — Phase correlation visualization

---

## Appendix A: Quick Reference Card

### YTPMV Production Checklist

- [ ] Source audio is clean (no background music/SFX)
- [ ] Source video shows character clearly
- [ ] Vowel samples extracted at multiple pitches
- [ ] Each sample within ±5 semitones of target notes
- [ ] MIDI file parsed for note events
- [ ] Audio pitch-shifted with formant preservation
- [ ] Video segments speed-adjusted to note duration
- [ ] Minimum clip duration: 0.5s
- [ ] Crossfades between clips: 20-50ms
- [ ] Background layer is constant (no flashing)
- [ ] No fadeblack/fadewhite transitions
- [ ] Audio mixed with proper timing (adelay)
- [ ] Video composited with overlay
- [ ] Final output: audio + video merged

### ffmpeg One-Liners

```bash
# Pitch shift audio
ffmpeg -i in.wav -af "rubberband=pitch=1.5:formant=preserved" out.wav

# Speed up video 2x
ffmpeg -i in.mp4 -vf "setpts=0.5*PTS" -an out.mp4

# Crossfade two videos
ffmpeg -i a.mp4 -i b.mp4 -filter_complex "xfade=transition=fade:duration=0.5:offset=3" out.mp4

# Green screen removal
ffmpeg -i greenscreen.mp4 -vf "chromakey=0x00FF00:0.3:0.1" -c:v libvpx-vp9 -pix_fmt yuva420p out.webm

# Detect onsets
aubio onset input.wav

# Detect BPM
aubio tempo input.wav

# Audio waveform video
ffmpeg -i audio.wav -filter_complex "showwaves=s=1280x720" waveform.mp4

# Fade in/out audio
ffmpeg -i in.wav -af "afade=t=in:st=0:d=0.005,afade=t=out:st=0.095:d=0.005" out.wav

# Delay audio by 1 second
ffmpeg -i in.wav -af "adelay=1000|1000" out.wav

# Mix multiple audio files
ffmpeg -i a.wav -i b.wav -i c.wav -filter_complex "amix=inputs=3:duration=longest" out.wav
```

---

*This document is part of the Big Mac YTPMV research series.*
*Previous: R121-ytpmv-production-knowledge-base.md (Audio pipeline, pitch mapping, sample selection)*
*Next: R123-ytpmv-implementation-plan.md (Specific Big Mac code changes)*
