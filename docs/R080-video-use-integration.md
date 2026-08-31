# R080 — Video-Use Integration Plan

## Source: github.com/browser-use/video-use (21.7k stars)

Video-use is a framework for editing videos with coding agents. It provides:
- Transcript-first editing (text-based interface to video)
- Timeline view (filmstrip + waveform + word labels)
- EDL (Edit Decision List) structured edit format
- Self-eval loop (verify cuts at boundaries)
- Audio-primary editing (cuts from speech boundaries)

## Key Concepts to Integrate

### 1. Transcript-First Editing
**Current problem**: We extract random clips and apply effects blindly.
**Solution**: Transcribe source → identify key moments by text → edit based on content.

Pipeline:
```
Source Video → Whisper STT → Transcript with timestamps → Identify moments → Extract clips → Apply effects
```

### 2. Timeline View
**Current problem**: We don't know what's in a video until we watch it.
**Solution**: Generate a visual summary (filmstrip + audio waveform) for quick scanning.

Pipeline:
```
Source Video → Extract frames at intervals → Generate waveform → Composite into single image → Vision analyze
```

### 3. EDL (Edit Decision List)
**Current problem**: Edits are ad-hoc ffmpeg commands with no structure.
**Solution**: Use structured edit decisions that can be reviewed and modified.

Format:
```
# EDL for YTP 034 - Shrek
00:00:00-00:00:03  CLIP swamp  PITCH_UP 2.0x
00:00:03-00:00:06  CLIP mob    VOLUME 20x
00:00:06-00:00:10  CLIP confront REVERSE
```

### 4. Self-Eval Loop
**Current problem**: We don't verify if edits look good.
**Solution**: After rendering, extract frames at cut boundaries → vision analyze → verify quality.

### 5. Audio-Primary Editing
**Current problem**: We apply effects randomly.
**Solution**: Identify speech boundaries → make cuts at natural pauses → sync visual effects to audio events.

## Implementation for Big Mac YTP

### Phase 1: Transcript Pipeline
1. Use ffmpeg to extract audio
2. Run whisper/STT to get transcript with timestamps
3. Parse transcript into words with start/end times
4. Build searchable index of content

### Phase 2: Timeline View Generator
1. Extract 1 frame per 5 seconds
2. Generate audio waveform
3. Composite into filmstrip
4. Use vision analysis to describe content

### Phase 3: EDL System
1. Define EDL format for YTP edits
2. Parser to convert EDL → ffmpeg commands
3. Review step before execution
4. Self-eval after rendering

### Phase 4: Integration with Big Mac
1. Use Big Mac's audio engine for analysis
2. Use wb_audio_reactive for audio-reactive effects
3. Use wb_video_edit for NLE operations
4. Export via wb_render pipeline

## Immediate Next Steps

1. **Build transcript pipeline** — Extract audio → transcribe → index
2. **Build timeline view** — Filmstrip + waveform generator
3. **Build EDL parser** → Structured edit format
4. **Make 2 high-quality YTPs** using the new pipeline

The key insight: **Don't edit video by watching it. Edit video by reading its transcript.**
This is what makes agent-based editing scalable — text is cheaper than pixels.
