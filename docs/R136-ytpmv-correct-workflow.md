# YTPMV Production: Correct Workflow

**Learned from failure and iteration.**

## The Wrong Approach (what I was doing)
1. Download random video
2. Detect "vowels" using energy+ZCR (catches everything, not just clean vowels)
3. Pitch-shift each fragment in real-time to match MIDI notes
4. Use random video clips at random positions
5. Result: garbled chipmunk noise with no musicality

## The Correct Approach

### Phase 1: Build a Chromatic Instrument
1. Find ONE clean, sustained vowel from the character (ah, eh, oh)
2. Extract ~200ms of clean audio
3. Use rubberband to create a chromatic scale (every semitone from the root)
4. Save as individual WAV files (note_60.wav, note_61.wav, etc.)
5. Apply loudnorm to each for consistent volume

### Phase 2: Select/Transpose the Song
1. Determine the character's comfortable pitch range (e.g., SpongeBob = MIDI 80-84)
2. Transpose the target MIDI melody to fit within the instrument range
3. Center the melody in the instrument's range for minimal pitch shifting

### Phase 3: Produce Audio
1. For each MIDI note, use the pre-built note file (no real-time pitch shifting!)
2. Use adelay filter to place each note at the correct time
3. Mix all notes with amix
4. Apply loudnorm to final mix (-14 to -16 LUFS)

### Phase 4: Produce Video
1. Extract short clips from the source video at varied positions
2. Speed/slow clips to match note durations
3. Concatenate all clips into a single video track
4. Merge audio + video

## Key Insights
- **Pre-built instruments > real-time pitch shifting** — consistent quality
- **Transpose the song to the source** — not the source to the song
- **One clean vowel is enough** — rubberband can shift ±12 semitones with formant preservation
- **Volume normalization is critical** — rubberband reduces volume significantly
- **adelay > itsoffset** — amix doesn't respect itsoffset
- **Batch processing** — ffmpeg has limits on number of inputs
