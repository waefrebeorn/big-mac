# R014 — Ground-up redesign of Big Mac's speech renderer

> Status: design (research-driven), implementation starting.
> Trigger: after ~12 incremental versions, none produced legible English; the
> Klatt-cascade pivot sounded like a "broken speaker" (a regression). The user
> directed: **redesign from the ground up using the research.**

## What the research converged on (the truth)
From 5 TTS engines (espeak-ng, flite, festival, marytts, piper), 4 vocoder/VC
engines (WORLD, RVC, sinsy, ParallelWaveGAN), 25+ searches, ~100MB source:

1. **Source-filter is the proven architecture.** Glottal source (LF pulse)
   → formant filter → output. Both espeak-ng and Klatt use this; it produces
   intelligible speech. The KL waveguide is a physical tube but has a
   measured low-pass ceiling (~1262Hz) and is a "demo tool", not a TTS engine.
2. **The source must be bright enough to excite the formants.** Measured:
   glottal tenseness 0.3→centroid 598Hz, 0.9→1743Hz. A dark source (0.45)
   physically cannot excite F2/F3 — this is why everything sounded like a
   low hum. Real speech needs tenseness ~0.85.
3. **The Klatt cascade resonator is exact:** `y[n]=a·x+b·y1+c·y2`, `a=1-b-c`
   (DC gain 1), `b=2r·cosθ`, `c=-r²`, `r=exp(-π·bw·t)`. My earlier `a0=1-r`
   crushed /i/ to silence. Corrected version makes vowels ring.
4. **The cascade needs TIME to ring up.** Formant resonators take ~50-100ms
   to build steady-state energy. Connected-speech phones (~80ms) that don't
   sustain leave formants under-excited → the phrase collapsed to 439Hz.
   Vowels must HOLD their target long enough.
5. **Timing is table-driven (Klatt).** Inherent duration × context factors
   (following-consonant voicing, stress, word-finality, phrase-finality,
   cluster, VOT), plus contextual prepause. Not fixed per-class heuristics.
6. **Spectral tilt (`tilt`) shapes the natural rolloff.** 0-24dB down at 3kHz.

## Why the previous builds failed
- **KL waveguide**: structurally low-passes the spectrum (can't reach speech);
  source too dark on top of that.
- **My Klatt cascade**: right resonator eventually, but (a) formants not given
  time to ring in short phrase phones, (b) no per-vowel sustain, (c) tilt/balance
  wrong → "broken speaker". Front vowels /i/ never excited F2/F3.

## The redesign (clean architecture)

```
text ─► words/phones (CMUdict) ─► events (phone, stress, f0, dur, prepause)
                                    │  (Klatt timing model: inherent×context,
                                    │   voicing effect, VOT, prepause)
                                    ▼
renderer (per phone):
  glottal source: LF, tenseness ~0.85 (bright)
    │
    ├─ voiced (vowels/liquids/nasals):
    │    LF pulse ─► Klatt cascade F1..F5 (a=1-b-c) ─► tilt ─► out
    │    (vowels SUSTAINED to let formants ring; smooth formant tracks)
    │
    ├─ fricatives: noise ─► bandpass(fric_fc) ─► out   (parallel path)
    ├─ stops: closure + burst + VOT aspiration (45-80ms)
    └─ nasals: LF ─► cascade F1..F3 ─► antiformant notch
```

## Key design decisions
1. **Primary renderer: Klatt cascade** (state-of-the-art, proven intelligible),
   NOT the KL waveguide. The KL stays as an optional `-wg` mode for the
   "street-magic" physicality but is not the speech path.
2. **Bright source** (tenseness ~0.85) so F2/F3 excite. This is the single
   biggest fix for the "low hum" / "broken speaker" sound.
3. **Vowel sustain**: stressed vowels and vowel targets HOLD long enough
   (~100ms+) for formants to ring. Formant tracks glide smoothly (CV
   transitions) but reach target and stay.
4. **Correct Klatt resonator** + `tilt` for natural rolloff + correct
   formant bandwidths.
5. **Klatt timing** (already implemented in R013): inherent×context, voicing
   effect, VOT, prepause.

## Verification (with the installed tools)
Target: phrase spectral centroid ≈ 1665Hz (speech); vowels reach their
Peterson-Barney F1/F2/F3; isolated /a/ ~1048Hz, /i/ ~2500Hz, /u/ ~870Hz.

## Build order
1. Clean source + cascade renderer with vowel sustain (the core speech path).
2. Verify isolated vowels hit target formants + centroid.
3. Verify phrase centroid ≈ speech; then consonants/timing on top.

## Key finding (during implementation) — additive > pure cascade
The pure Klatt cascade **cannot control formant amplitudes independently**:
with F0=130, /a/'s F3 (2440Hz) lands on harmonic 19 while F1/F2 (730/1090) fall
between harmonics → F3 over-excites, measured /a/ centroid 2739Hz (speech
/ɑ/ = 1673Hz). /i/ works (2300Hz) because its F2/F3 are the identity.
espeak-ng avoids this by using **additive harmonic synthesis with an explicit
formant envelope** (per-vowel formant peak heights), not a pure cascade.

**Decision: the ground-up renderer is additive-harmonic** (espeak-ng wavegen
style): build a formant-peak envelope per vowel, weight each harmonic by it,
sum the harmonics. This gives explicit control over formant amplitudes so
/a/'s F3 is weak and /i/'s F2/F3 are strong — matching real vowel spectra.
Verified so far: corrected Klatt resonator (a=1-b-c) makes vowels ring;
bright source (tenseness ~0.85) excites high formants; vowels must SUSTAIN
~100ms+ for formants to ring.

## Verified progress (measured)
- Corrected cascade resonator: /i/ sustained → centroid 2300Hz (speech ~3186),
  /a/ → 2739Hz (speech 1673, F3 too strong — hence additive decision).
- Source brightness: tenseness 0.3→598Hz, 0.9→1743Hz glottal centroid.
