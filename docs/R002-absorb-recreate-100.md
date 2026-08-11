# R002 — ABSORB-AND-RECREATE: Analysis-by-Synthesis Voice Engine (100 Improvements)

> **Big Mac · 2026-08-11 · method: Kevin-Bacon 7-hop · status: research → apply**
> **The goal:** Big Mac becomes the best vocal synthesis engine by ABSORBING
> the qualities of a vocal demo (analysis) and RE-CREATING it (synthesis) —
> purely in C11, no samples, no vocal banks, no neural networks.

---

## THE 7 HOPS (research → convergence)

| Hop | Source | Core finding | Converges on |
|-----|--------|--------------|--------------|
| 1 | Glottal inverse filtering (GFM-IAIF, GIPSA-lab; Drugman LF fitting; ARMAX estimation arXiv:2410.04704) | The glottal source can be ESTIMATED from the waveform by inverse filtering the tract; LF model parameters (Rd, tenseness, OQ) are directly fittable | **Analysis side:** inverse filter → LF parameters. We already synthesize LF — now we can measure it |
| 2 | YIN / pYIN pitch detection (Cheveigné; real-time C theses; OneBitPitch) | Autocorrelation-based F0 with difference function + CMND is accurate (0.5% error) and cheap — C implementations exist | **Analysis side:** F0 contour from a demo, sample-accurate enough to drive the glottis |
| 3 | LPC formant estimation (MATLAB docs; WLP-HPSV for sopranos; power-weighted LPC) | LPC coefficients → roots → formants; weighted LPC needed for high-pitched singing (F0 > F1/4) | **Analysis side:** F1-F4 from any demo, including high soprano |
| 4 | Vocal-tract area function estimation (Kaburagi IS2012; Story; Carré; lossy KL trajectory lib.tkk.fi 2010) | **Formants → area function is a solved inverse problem**: iterate a KL model, fit diameters to match measured formants. THE bridge | **THE KEY HOP:** measured formants → our 44 KL tube diameters. Direct demo→tract mapping |
| 5 | Voice quality parameterization (Praat; Teixeira jitter/shimmer/HNR formulas; CPP; H1-H2, H1-A2, H1-A3 spectral-tilt measures) | jitter (jitta/jitt/RAP/ppq5), shimmer (shim/SHDB/apq3/apq5), HNR, CPP, H1-H2 (open quotient), spectral tilt — ALL computable from a window of audio | **Analysis side:** the full quality fingerprint. Each maps 1:1 to a knob we already synthesize |
| 6 | Singing formant tuning (UNSW; soprano F1:F0 tuning; vibrato cycle acoustics; HAL 2017 thesis) | Sopranos tune F1 to F0 above ~A4; vibrato = 5-7Hz, 50-100 cent; singers formant = 2.5-3.5kHz boost | **Synthesis side:** formant tuning + vibrato + singers formant = the singing difference (already in our roadmap #20/#21) |
| 7 | KL waveguide improvements (Aalto conical tubes + fractional delay; CCRMA singing KL; differentiable copy-synthesis arXiv:2606.04943) | Conical (not cylindrical) tube sections + fractional delay filtering improve KL accuracy; the KL model is differentiable → gradient fitting is possible | **Synthesis side:** better tube geometry + the fitting target for hop 4 |

**CONVERGENT TRUTH:** *"The demo is a parameter vector. F0 and jitter/shimmer/HNR come from the waveform, formants come from LPC, the tract area function comes from the formants via KL fitting, and the glottis comes from inverse filtering — so absorbing a voice is just measuring, and re-creating it is just setting our existing C11 knobs to those measurements. The engine that can measure and set the same parameter set is the best vocal synthesis engine, because it can copy ANY voice without a sample."*

---

## THE 100 IMPROVEMENTS

### A. ANALYSIS — ABSORB THE DEMO (the measurement layer) — 26 items

**A1-A8: F0 estimation (pitch)**
- A01 YIN difference function (CMND) — the canonical robust F0
- A02 pYIN — probabilistic F0 with voiced/unvoiced confidence
- A03 Autocorrelation with parabolic interpolation (sub-sample period)
- A04 Pitch-marks (glottal pulse onset detection) — needed for jitter/shimmer
- A05 Voiced/unvoiced segmentation (energy + periodicity gate)
- A06 F0 smoothing + median filter (octave-jump suppression)
- A07 Vibrato measurement: rate (Hz) + depth (cents) from the F0 contour
- A08 F0 range stats: mean, min, max, SD, contour slope (prosody profile)

**A9-A16: Spectral analysis (formants)**
- A09 LPC analysis (Levinson-Durbin, order 12-24) — our own C11
- A10 LPC root solving (Bairstow / companion-matrix QR) → formant peaks
- A11 Weighted LPC for high-pitched singing (WLP-HPSV idea, simplified)
- A12 Formant tracking across frames (peak continuity + smoothness cost)
- A13 Formant bandwidth estimation (pole radius → BW)
- A14 Cepstral smoothing (liftering) for spectral envelope
- A15 FFT (radix-2, in-place, C11 — for spectral tools + CPP)
- A16 Spectral tilt measurement (H1-A2, H1-A3) + slope (dB/octave)

**A17-A22: Voice quality fingerprint**
- A17 Jitter: jitta (µs), jitt (%), RAP, ppq5 — exact Praat formulas
- A18 Shimmer: shim (%), SHDB, apq3, apq5 — exact Praat formulas
- A19 HNR (harmonic-to-noise ratio via autocorrelation, Boersma)
- A20 CPP (cepstral peak prominence) — the modern primary quality measure
- A21 H1-H2 (open quotient correlate) → glottal tenseness estimate
- A22 Breathiness index (noise in high band, HNR per band)

**A23-A26: Source-tract separation**
- A23 Glottal inverse filtering (iterative: estimate tract → remove → residual = glottal flow)
- A24 LF model fitting to the glottal residual (Rd, Ta, Tp, Te via least squares)
- A25 Spectral envelope of glottal residual → source spectrum (tilt + roll-off)
- A26 Subharmonic/growl detection (energy at F0/2) for vocal-fry/rock modes

### B. MAPPING — QUALITIES → PARAMETERS (the translator) — 14 items

- B01 **Formants → KL diameters** (the Kaburagi/Story iteration: run KL, measure formants, adjust diameters, repeat — the core absorb loop)
- B02 Measured F0 → glottis frequency track (time-aligned)
- B03 Measured jitter → glottis jitter knob (per-period random ±X%)
- B04 Measured shimmer → glottis shimmer knob
- B05 Measured HNR → aspiration/noise floor gain
- B06 H1-H2 / tenseness → LF Rd parameter (breathy ↔ pressed)
- B07 Spectral tilt → LF E0/alpha shaping (source roll-off)
- B08 Formant bandwidths → tract wall loss / tube damping
- B09 F0 range + contour → prosody profile (stress, declination)
- B10 Vibrato rate/depth → glottis vibrato knobs
- B11 **Voice print** = the whole measured vector (the "bank" we can save/load)
- B12 Voice-print interpolation (morph between two absorbed voices)
- B13 Articulatory inference: phone boundaries → tongue/lip target table
- B14 Quality deltas: compare two demos → the parameter diff that makes voice B sound like voice A

### C. SYNTHESIS — RE-CREATE IT (the generation layer) — 30 items

**C1-C12: Glottis (source)**
- C01 LF model (have) + measured Rd driving (new)
- C02 R++ glottal model (perceptually ≡ LF, cheaper — roadmap #1)
- C03 Glottal flow wavetable (precomputed period, phase-accumulator — roadmap #2)
- C04 Measured-jitter injection (per-period random ±X% — roadmap #3)
- C05 Measured-shimmer injection (roadmap #4)
- C06 Aspiration-to-voicing AH/AV ratio (breathy continuum)
- C07 Vibrato: rate+depth from measurement (roadmap #6)
- C08 Growl/vocal-fry: subharmonic excitation (F0/2)
- C09 Glottal source spectrum shaping (tilt knob from measured tilt)
- C10 Pitch glide/portamento smoothing (natural transitions)
- C11 Voice-onset-time control (consonant voicing lag)
- C12 Whisper mode (unvoiced noise source through tract)

**C13-C22: Tract (filter)**
- C13 **KL diameters FROM analysis** (the absorb loop's output drives the tract)
- C14 Conical tube sections (Aalto — roadmap #8)
- C15 Fractional delay filtering (Aalto — roadmap #9)
- C16 Formant bandwidth control (wall loss — roadmap #10)
- C17 Tract length scaling: male/female/child (roadmap #11)
- C18 Measured-bandwidth fitting (match demo's formant sharpness)
- C19 Lips: measured aperture tracking (from F1/F2 relationships)
- C20 Teeth: dental constriction for s/z/th (roadmap "teeth and lips")
- C21 Nasal coupling (velum) from measured nasalance
- C22 Tract radiation impedance (more accurate lip output)

**C23-C30: Articulation & prosody**
- C23 Phone→articulation table (have — measured-target refinement)
- C24 Coarticulation lookahead (roadmap #14)
- C25 Diphthong/triphthong glide table (roadmap #15)
- C26 Stress-based prosody from measured contour (roadmap #16)
- C27 Intonation contours: statement/question/list (roadmap #17)
- C28 Phrase tempo + pause model (roadmap #18)
- C29 Formant tuning: sung F0→F1 tracking (roadmap #20 — THE singing lever)
- C30 Singers formant boost 2.5-3.5kHz (roadmap #21)

### D. ENGINE — C11 EXCELLENCE (the performance layer) — 16 items

- D01 numpy-free pure-C11 hot loop (have — the waveguide is already C)
- D02 Vectorize scattering via SIMD intrinsics where available (SSE2 baseline on this iMac)
- D03 Block processing: render 64 samples per call (amortize dispatch)
- D04 Glottal wavetable read via phase accumulator (no per-sample LF math)
- D05 Fixed-point option for the waveguide (integer-only mode for embedded)
- D06 Lookup tables: sin/cos/exp for the hot path (or own polynomial)
- D07 Single allocator of record (arena) — no per-render malloc (kernel-layer doctrine)
- D08 Opaque struct seams (have) + strict `-std=c11 -Wall -Wextra -Werror` build
- D09 Streaming render (callback API: render block-by-block, not whole-file)
- D10 Double-buffered block API (real-time safe for MIDI live input)
- D11 ASan/UBSan test builds (0 leaks, 0 UB — WuBuPad pattern)
- D12 Deterministic LCG noise (have) + seeded variants for reproducible renders
- D13 Sample-rate independence (44.1/48k/96k via ratio-scaled delays)
- D14 64-bit clean, no float-precision drift over long renders
- D15 Benchmark harness: realtime factor per block size (the comparison loop)
- D16 Memory budget: whole engine < 2MB RSS (this iMac's 8GB is plenty, but doctrine)

### E. INTEGRATION — THE MUSICAL AGENT — 14 items

- E01 WAV writer (have) + AIFF writer (GarageBand-native)
- E02 MIDI file writer (MIDIUtil knowledge, hand-rolled C11 — no Python)
- E03 MIDI keyboard live input (CoreMIDI on macOS) — Launchpad Mk1 / M-Audio 88
- E04 MIDI→parameter mapping: notes=F0, velocity=loudness, mod=vibrato, aftertouch=tenseness
- E05 GarageBand double-track: render voice AIFF + MIDI together
- E06 `wb_analyze` tool: demo.wav → voice-print.json (the absorber)
- E07 `wb_speak` tool: text → voice-print → WAV (the re-creator)
- E08 `wb_sing` tool: lyric + MIDI notes → sung WAV (formant-tuned)
- E09 `wb_morph` tool: two voice-prints → blended voice
- E10 Voice-print library: the "vocal banks" we make programmatically (files, not samples)
- E11 JSON voice-print reader/writer (our own tiny C11 JSON — no third party)
- E12 WAV reader (reverse of our writer — parse demos in C11)
- E13 Demo self-test: render → analyze → compare (the closed loop, in C11)
- E14 The honest comparison report: measured F0/jitter/formants of re-creation vs original

---

## THE ABSORB-AND-RECREATE LOOP (how it all fits)

```
demo.wav
  │  wb_analyze (A: F0, jitter, shimmer, HNR, formants, tilt, vibrato)
  ▼
voice-print.json (B11: the measured parameter vector)
  │  wb_morph (B12-B14: optional blending)
  ▼
C11 engine (C: glottis knobs + KL diameters from B01 + articulation)
  │  wb_speak / wb_sing (D: efficient block render)
  ▼
recreated.wav
  │  wb_analyze AGAIN (E13: measure the re-creation)
  ▼
comparison report (E14): |original − recreated| per parameter → TUNE → repeat
```

**This loop IS the recursion**: each cycle's comparison feeds the next
fitting pass. The engine that closes this loop best IS the best vocal
synthesis engine — and it never touches a sample bank.

---

## COMPARISON PLAN (the tuning signal — honest numbers only)

| Metric | How measured | Target |
|--------|-------------|--------|
| F0 error | mean abs diff, original vs recreated contour | < 2% |
| Formant error (F1-F3) | LPC on both, mean abs Hz diff | < 5% |
| Jitter/shimmer error | Praat formulas on both | < 20% relative |
| HNR error | autocorrelation HNR on both | < 3 dB |
| Realtime factor | seconds to render 1s audio | < 1x on this iMac |
| Bytes moved per sample | profiler (Roofline doctrine) | decreasing |

## NEXT CYCLE TARGET (R003)
Implement B01 (formants → KL diameters fitting) + A01 (YIN) + A09 (LPC) in
C11 — the first closed absorb loop on a synthetic reference vowel, then a
real recorded demo.

---
*Sources: GFM-IAIF (GIPSA-lab), Drugman LF fitting (EUSIPCO'08), ARMAX glottal
estimation (arXiv:2410.04704), YIN/pYIN, Kaburagi IS2012, Story JASA 119,
Rasilo TKK 2010 lossy KL, Teixeira Procedia 2013 (jitter/shimmer/HNR),
Praat/Boersma, UNSW voice acoustics, Aalto conical KL (ICSLP'94), CCRMA
singing KL, arXiv:2606.04943 differentiable copy-synthesis, HAL 2017 singing
thesis, VOXplot, PhonaLab.*
