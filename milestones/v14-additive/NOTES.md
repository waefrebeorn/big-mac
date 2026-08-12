# v14 — additive-harmonic renderer (milestone)

## What this version is
The ground-up redesign's renderer: additive harmonic synthesis (espeak-ng
style) with per-vowel formant amplitudes.

## Measured result (the breakthrough)
- Phrase spectral centroid: 1726Hz vs real speech 1665Hz — MATCHES.
- Isolated vowels match real spectra: /i/ 2912 (speech 3186),
  /a/ 1599 (1673), /ae/ 1977 (1900).
- For context: KL waveguide was 828Hz (low hum), Klatt 439Hz (broken speaker).

## User feedback (2026-08-11)
"that's an alien lol ... but this isn't a voice"
- The spectral balance is CORRECT now (matches speech).
- But it still sounds like an instrument/alien, NOT a human voice.
- Cause: pure harmonic sum = clean sines, no natural breathiness/aspiration.

## Next step (not yet done)
Add natural glottal source characteristics to the additive renderer:
1. Aspiration/breathiness noise mixed into the harmonics (the "air" that
   makes it human — real voice is harmonic + noise).
2. Natural jitter/shimmer.
3. Then the consonant/timing work.
This should take it from "clean alien instrument" to "human voice."
