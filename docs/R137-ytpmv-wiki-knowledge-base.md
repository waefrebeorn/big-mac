# YTPMV Wiki — Local Knowledge Base

## Compiled from online research, 2026-09-05

---

## 1. What is YTPMV?

YouTube Poop Music Video (YTPMV) is a genre where a character "sings" a song using their voice from the source material. The audio is pitch-shifted character dialogue/sounds arranged to match a target melody, and the video is synced to show the character "singing."

## 2. The Correct Workflow (from experienced creators)

### Sony Vegas Method (most common):
1. Import reference song + source video to timeline
2. Cut source audio into individual words/syllables
3. Place each clip on timeline to match reference song rhythm
4. Pitch-shift each clip by semitones (+/- keys) to match target note
5. Use **Elastique Pro** stretch method with **formant preservation** enabled
6. Sync video to audio

### FL Studio Method:
1. Use **Newtone** or **Melodyne** to detect pitch of each syllable
2. Create a chromatic scale by pitch-shifting one sample to all semitones
3. Load into **Slicex** sampler
4. Play MIDI of target song through the sampler

### REAPER Method:
1. Use **ReaPitch** with envelope automation for per-note pitch shifting
2. MIDI can guide timing but manual placement sounds better

## 3. Critical Rules

### The ±5 Semitone Rule:
- Never shift a sample more than ±5 semitones from its natural pitch
- Beyond that, the character's voice disappears (chipmunk effect or demonic bass)
- Solution: use MULTIPLE samples at different native pitches

### Volume:
- amix (ffmpeg) reduces volume proportionally to number of inputs
- Use `amix=inputs=N:normalize=0` to prevent automatic normalization
- Apply `loudnorm` ONLY at the final output, not per-note
- Each note should be rendered at consistent volume before mixing

### Song Selection:
- The song's pitch range MUST overlap with the character's natural range
- Dense, rhythmic songs work better than sparse, slow songs
- Piano solos with long held notes DON'T work (samples are too short)

## 4. Character Voice Ranges (approximate)

| Character | Natural MIDI Range | Voice Type |
|-----------|-------------------|------------|
| SpongeBob | MIDI 75-85 | High, nasal |
| Kirby (SSBU) | MIDI 85-90 | Very high, short blips |
| Homer Simpson | MIDI 55-75 | Mid, varied |
| Peter Griffin | MIDI 50-70 | Low-mid |

## 5. Sample Extraction

### What makes a good sample:
- Sustained vowel sound (ah, oh, ee) — NOT percussive (wahoo, hah, boom)
- Clear pitch (stable fundamental frequency)
- Minimal background noise/sfx
- At least 100ms of stable sound

### What makes a BAD sample:
- Percussive sounds (grunts, laughs, impact sounds)
- Sounds with no clear pitch (white noise, explosions)
- Sounds with heavy background music/sfx
- Very short sounds (<50ms)

## 6. Tools Comparison

| Tool | Pros | Cons |
|------|------|------|
| Sony Vegas | Easy +/- semitone shifting, Elastique Pro formant preservation | Paid, Windows only |
| FL Studio + Newtone | Professional pitch detection, Slicex sampling | Paid, steep learning curve |
| REAPER | Cheap, flexible, ReaPitch with envelopes | Steep learning curve |
| ffmpeg + rubberband | Free, scriptable, formant preservation | No GUI, manual workflow |

## 7. Common Mistakes

1. **Extreme transposition**: Shifting >±5 semitones destroys voice character
2. **One sample for all notes**: Sounds mechanical, use multiple samples
3. **Wrong song choice**: Song range must match character range
4. **Long MIDI notes with short samples**: Results in silence between notes
5. **amix volume loss**: Mixing many quiet inputs produces quiet output
6. **Per-note loudnorm**: Normalizes silence too, making gaps loud
7. **Using percussion as melody**: Wahoo/hah/boom don't make good pitched instruments

## 8. ffmpeg Commands Reference

### Pitch shift with formant preservation:
```
ffmpeg -i input.wav -af "rubberband=pitch=RATIO:formant=preserved" output.wav
```
Where RATIO = target_freq / source_freq (0.5 = down 1 octave, 2.0 = up 1 octave)

### Mix many notes on timeline:
```
ffmpeg -i note0.wav -i note1.wav ... -filter_complex \
  "[0:a]adelay=0|0[a0];[1:a]adelay=500|500[a1];...\
   [a0][a1]...amix=inputs=N:normalize=0:duration=longest[aout]" \
  -map "[aout]" output.wav
```

### Final loudnorm:
```
ffmpeg -i input.wav -af "loudnorm=I=-14:TP=-1.5:LRA=11" output.wav
```

## 9. Source Material Quality Guide

### Best sources for YTPMV:
- Character saying sustained vowels ("ahhhhh", "ohhhhh")
- Characters singing in the original source
- Clear dialogue without background music

### Worst sources:
- Sound effects (explosions, impacts)
- Laughter, grunts, non-vocal sounds
- Audio with heavy compression/artifacts
- Audio with background music mixed in
