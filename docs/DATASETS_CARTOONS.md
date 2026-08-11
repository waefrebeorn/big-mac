# Big Mac — Training Datasets & Cartoon Voice Lab (research log)

> 2026-08-11 · the "learn to become the characters" corpus + preset library

## Online datasets (for training the absorb engine)

| Dataset | Size | Speakers | License | Status |
|---------|------|----------|---------|--------|
| **LibriSpeech** (openslr.org/12) | ~1000h | 2484 | CC BY 4.0 | ✅ dev-clean downloaded (338MB, 5.4h) → 142 flac samples of 2 speakers extracted |
| VCTK (datashare.is.ed.ac.uk/10283/3443) | ~10GB | 110 | CC BY 4.0 | queued (diverse accents — good for voice-print variety) |
| Common Voice (commonvoice.mozilla.org) | huge | 130+ langs | CC0 | queued (multilingual) |
| MLS — Multilingual LibriSpeech (openslr.org/94) | 50k h | many | CC BY 4.0 | queued |

## Popular cartoon characters (voice-print presets — 15 implemented)

Source: Paste Magazine 50 Best + Animaker 57 Iconic rankings.

| Character | Voice actor | Preset (f0 / tenseness / tract) | Signature line |
|-----------|-------------|----------------------------------|----------------|
| Mickey Mouse | Walt Disney / Bret Iwan | 320 / 0.85 / female(36) | "hee hee, oh boy, that was fun" |
| Bugs Bunny | Mel Blanc | 175 / 0.70 / male | "eh, what's up doc" |
| Homer Simpson | Dan Castellaneta | 105 / 0.45 / male | "d'oh, mmm donuts" |
| Donald Duck | Clarence Nash | 250 / 0.90 / female | "a-hyuck, oh boy oh boy" |
| SpongeBob | Tom Kenny | 300 / 0.80 / female | "i'm ready, i'm ready" |
| Daffy Duck | Mel Blanc | 210 / 0.75 / male | "you're despicable, thufferin' thuccotash" |
| Eric Cartman | Trey Parker | 330 / 0.80 / child(28) | "screw you guys, i'm going home" |
| Stewie Griffin | Seth MacFarlane | 240 / 0.70 / female | "victory is mine" |
| Bart Simpson | Nancy Cartwright | 290 / 0.65 / child | "eat my shorts" |
| Scooby-Doo | Don Messick | 130 / 0.55 / male | "ruh roh, raggy" |
| Betty Boop | Mae Questel | 380 / 0.85 / female | "boop oop a doop" |
| Popeye | Jack Mercer | 120 / 0.60 / male | "i yam what i yam" |
| Shaggy | Casey Kasem | 150 / 0.50 / male | "like, zoinks man" |
| Yoda | Frank Oz | 160 / 0.55 / male | "do or do not, there is no try" |
| Bender | John DiMaggio | 140 / 0.60 / male | "bite my shiny metal ass" |

## Verified (honest numbers)
- homer: measured f0=106.8Hz vs preset 105 → 1.7% error ✓
- Each character measures distinctly (different formant sets) ✓
- Real human voice (LibriSpeech 3000): F0=115.7Hz, F1 fit 245→253 (3.4%) ✓

## Known gaps → R003
- YIN returns 0 for f0 > 300Hz on the 36-section tract (high falsetto chars)
- Quality gate needs voiced-only segmentation (real speech has 40% voicing)
- Cartoon F2/F3 (esp. Donald's quack) need the full 44-diameter fitting

## Paths
- Voices: `~/Music/BigMac-Voices/*.wav` (15 characters)
- Corpus: `~/Documents/WuBu-Learning/L4-voice/corpus/librispeech/`
- Voice-prints: `/tmp/*.json` (wb_absorb output)
