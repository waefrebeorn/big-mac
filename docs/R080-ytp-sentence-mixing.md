# R080 — YTP Sentence Mixing: Technique Analysis

## What is Sentence Mixing?

Sentence mixing is the core YTP technique of taking audio clips (phonemes, words, or syllables) from a source and rearranging them to create new, often nonsensical sentences. It's the audio equivalent of visual collage.

## How It Works

1. **Source Analysis**: Identify all spoken words/phrases in the source
2. **Phoneme Extraction**: Isolate individual sounds (consonants, vowels, syllables)
3. **Reconstruction**: Arrange phonemes into new words/sentences
4. **Timing**: Match mouth movements to the new audio (lip-sync approximation)

## Common Techniques

### Word Salad
Take words from different parts of the source and string them together:
- Source: "I am going to the store" + "Do you want some food?"
- Result: "I am going to do you want some food?"

### Phoneme Chopping
Cut at the syllable level for more natural-sounding results:
- "Mario" → "Mar-i-o" → "I-o Mar" → "I'm a Mario"

### Pitch Shifting
Combine with pitch changes for character voices:
- Chipmunk voice: "Pingas" (famous YTP word from Sonic)
- Demon voice: "The sky had a Weegee"

### Stutter Loops
Repeat a single syllable rapidly:
- "Ba-ba-ba-ba-ba-butter"
- "Ping-ping-ping-ping-pingas"

## Famous YTP Sentence Mix Examples

| Phrase | Source | Meaning |
|--------|--------|---------|
| "Pingas" | Sonic Adventure (Dr. Robotnik) | Nonsense word, YTP staple |
| "The sky had a Weegee" | SpongeBob "Shanghaied" | Famous Viacom lawsuit YTP |
| "Me amo boat" | Steamboat Willie | Early YTP classic |
| "Do you want some food?" | Various | Common sentence mix building block |
| "I'd say he's hot on our tail" | Mario Bros 3 | First YTP ever made |

## Implementation Notes

For automated sentence mixing, we need:
1. **Speech-to-text** to identify words in the source
2. **Audio segmentation** to isolate individual words/syllables
3. **Phoneme classification** to categorize sounds
4. **Reconstruction engine** to arrange phonemes into new sentences
5. **Lip-sync matching** to align video with new audio

This is a complex pipeline that would require:
- Whisper or similar STT for transcription
- Audio segmentation (silence detection, phoneme boundary detection)
- A phoneme database for the source character
- FFmpeg for audio reconstruction

## Current Status

Not yet implemented. This is a future enhancement for the Big Mac YTP pipeline.
The current experiments focus on simpler effects (pitch shift, speed change, reverse, stutter).
