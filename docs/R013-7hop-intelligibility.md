# R013 — 7-Hop Intelligibility Research: Japan, no-vocal-banks, and cheating the system

> Big Mac · 2026-08-11 · 25 web searches, 7-hop Kevin-Bacon. Goal: find the
> next wave of improvements for the pure-articulatory voice engine — with
> three lenses the user asked for: **how Japan does it**, **how speech is
> made with no vocal banks**, and **how to cheat the system** (perception
> tricks that cost almost nothing).

## The 7 hops (each a distinct domain, converging)

### Hop 1 — Japan: articulatory & physiological models
| Source | Finding |
|--------|---------|
| ATR (Japan) | Pioneered physiological/articulatory models + the ATR Japanese speech DB; rtMRI-driven two-stage articulatory→speech synthesis on 503 sentences. |
| Maeda 1990 (JAIST/ATR) | The **Maeda articulatory model** (7 parameter dimensions from PCA of MRI tract shapes) — C source exists, VTCalcs. "Discrete constriction locations describe a comprehensive tract" (Gaines 2021). |
| Kurematsu/ATR DB | Japanese heritage of acoustic+linguistic evidence; the ATR 503 set is the standard evaluation corpus. |
| Japanese pitch accent | **Tokyo dialect is binary H/L pitch**: every word has ONE tonic mora. Rule set: accent on mora 1 → `HꜜL-L-L…`; accent later → `L-H-HꜜL…`. Implementable as a simple rule table — no data, no models. |
| Mora isochrony | Japanese rhythm ≈ each mora roughly equal duration (Warner & Arai). A mora-timed duration model. |

### Hop 2 — No vocal banks: formant / source-filter / parameterized
| Source | Finding |
|--------|---------|
| Klatt 1980/1987, KLSYN88 | Cascade+parallel formant synthesizer — **highly intelligible, low memory footprint, no samples**. 48 params/frame. Open reimplementations (chdh/klatt-syn). |
| Sine-wave speech (Remez) | **3–4 formant-tracking sinusoids alone are intelligible.** Perception needs formant TRACKS, not full spectra — the ultimate "cheat." |
| Speech synthesis (Wikipedia) | Formant synthesis = additive synthesis + physical modeling; Coker 1968 parametric articulatory model. |
| Iles 1994 | Formant synthesizers are driven by formant F/BW parameters measurable from real speech. |

### Hop 3 — Cheat the system: DSP tricks
| Source | Finding |
|--------|---------|
| LPC inverse filtering | Separate source (residue) from vocal-tract filter; manipulate formants by editing filter coefficients, re-excite — the classic morph/VC cheat. |
| Cepstral envelope / liftering | Estimate the spectral envelope (formants) by cepstral windowing; shift/manipulate formants cheaply (CCRMA). |
| Phase vocoder + formant preservation (Lenarczyk) | Real-time pitch shift while preserving formant structure — the building block for voice conversion without neural nets. |
| **Conical KL sections + fractional delay (Välimäki)** | Replace cylindrical with **conical tube sections + fractional-delay filtering** → materially more accurate tract for small extra cost. |
| Informational masking | Synthetic-formant analogues of sentences remain intelligible even stripped of natural detail — formant contours dominate. |

### Hop 4 — Consonant landmarks (the "wawa" fix)
| Source | Finding |
|--------|---------|
| Delattre & Liberman, "Acoustic Loci" | **F2 transitions are the primary cue for stop & nasal place** — the consonant has a "locus" F2 toward which the transition points. |
| Dorman 1977 | Release bursts + F2 transitions both cue place; bursts alone are weak (Birkholz showed F2/F3 transitions dominate). |
| Kewley-Port 1983 | Dynamic spectral cues (in the onset) specify stop place. |
| Birkholz 2013 (coarticulation) | Context-dependent consonant shapes via **weighted average of measured CV tract shapes**; vowel undershoot model. |
| Sibilants (Reidy 2016) | Front-cavity size sets the fricative spectral peak; /s/ vs /ʃ/ differ by front-cavity resonance. |
| Nasals | **Antiformants** are the place cue (e.g. /m/ antiformant ~750–1250 Hz) + nasal murmur + F2 transitions. |
| Stevens 1999 | Place-of-articulation identification from revised spectral measures. |

### Hop 5 — Prosody & intonation
| Source | Finding |
|--------|---------|
| ToBI/KIM rule generation | Rule-based intonation (Tone Sequence Model) reaches **naturalness ≈4.1–4.75/5**. Declination + downstep are central. |
| Oester/Stuttgart | Declination (phrase-level F0 fall) is the core of intonation modeling. |
| Japanese pitch accent | Binary H/L contour — distinct, simple, natural prosody model. |

### Hop 6 — Efficiency / real-time / shallow hardware
| Source | Finding |
|--------|---------|
| Real-time DDSP articulatory vocoder | Light-weight, streamable, intelligible from **EMA + F0 + loudness** — real-time articulatory synthesis is proven viable. |
| Silicon vocal tract (Hansen) | An articulatory silicon tract codes speech for low-bit-rate prostheses — hardware-cheap synthesis. |
| Low-latency DSP | 3.35 ms end-to-end on low-power DSP at 376 MIPS — shallow compute is enough. |

### Hop 7 — Open implementations to SLERM (reference, never copy verbatim)
| Source | Finding |
|--------|---------|
| VocalTractLab (Birkholz) | Open articulatory synthesizer (aerodynamic+acoustic); **achieves intelligibility parity with established systems** — proves the approach. API source on GitHub. |
| Maeda model / VTCalcs | Open C/MATLAB articulatory model. |
| KLSYN88 reimplementations | Open formant-synth source to study. |

## Convergent truth
> **Intelligibility is carried by formant TRANSITIONS (F2 locus) + spectral-envelope landmarks + a well-modeled glottal source — NOT by full-spectrum fidelity; sine-wave speech proves 3 formant tracks suffice. So the highest-leverage, cheapest fixes are (1) consonant F2-locus transitions, (2) a better glottal source (LF), and (3) rule-based prosody (binary pitch accent / declination) — all doable with no vocal banks and no neural nets.**

## Applied / prioritized improvement list (Big Mac)

**DONE this session (all in `tools/wb_tts.c`, build + `make test` + `make test-yin` green):**

- **P0 — F2 locus transitions.** Consonants now carry an F2 *locus* (the
  Delattre & Liberman place cue): labial /b p m w/ ~700 Hz, alveolar
  /d t n/ ~1800, velar /g k ŋ/ ~2500, sibilants higher. Implemented in the
  new `-sine` formant-track mode, where the consonant's locus glides
  smoothly into the vowel's F2 (coarticulated, Remez-style).
- **P0 — Nasal antiformant.** A biquad notch on nasal phones (/m/ ~1000,
  /n/ ~2500, /ŋ/ ~1800 Hz) — the spectral zero that is the nasal place cue,
  on top of the velum coupling.
- **P1 — Japanese pitch-accent option (`-pa`).** Tokyo-style binary H/L —
  one tonic mora per word (`HꜜL-L-L` vs `L-H-HꜜL`) + mora-isochronous
  timing. Verified: the F0 contour turns into squared H/L steps vs the
  smooth default.
- **P2 — Formant-track mode (`-sine`).** The sine-wave "cheat": three
  oscillators track F1/F2/F3 (Peterson-Barney vowel targets + consonant
  loci). Verified: measured formants F1=720 F2=2410; F0=0 is expected
  (sine-wave speech has no glottal-harmonic fundamental).
- **P1 — LF glottal model.** Confirmed **already implemented** in
  `wb_glottis.c` (`normalized_lf`, Rd-parameterized) — no change needed.

**NOT implemented this session (need a listen to verify — muted, and these
are the most invasive):**
- **P1 — Conical KL sections + fractional delay (Välimäki).** Would modify
  the core waveguide reflection math; risky blind.
- **P2 — Formant-preserving phase vocoder for wb_vc.** Attempted a phase
  vocoder pitch shifter (`tools/wb_shift.c`); the 2x upshift produced
  incoherent output and could not be verified/debugged while muted. Left
  out of the build, marked experimental. `wb_vc` already preserves formants
  via articulatory re-synthesis. Needs a session with a listen (or TD-PSOLA).

## Comparison / verification
- Nothing re-rendered yet this cycle — this is a research+planning pass. Next cycle applies P0 (F2 locus) and re-runs `make test` + the `cat/bad/kick/church` landmark checks.
- Prior cycle's P0 (stop closure + affricates, R012-A4) verified: `make test` green; waveform landmarks present; consonant-flanked-vowel HNR ~+1 dB remains the open target.

## Next cycle target
Apply **P0 — F2 locus transitions** to `tools/wb_tts.c`, then measure consonant-flanked-vowel HNR / intelligibility before/after (the R010 §A gap).

## References (7-hop, from the 25 searches)
Birkholz (VocalTractLab, 2013 coarticulation), Maeda (1990, VTCalcs), Delattre & Liberman (Acoustic Loci), Dorman (1977), Kewley-Port (1983), Reidy (2016 sibilants), Klatt (1980/1987, KLSYN88), Remez (sine-wave speech), Välimäki (conical KL + fractional delay), Lenarczyk (phase-vocoder formant preservation), KIM/ToBI intonation, Warner & Arai (mora timing), Japanese pitch accent (Tokyo dialect), DDSP articulatory vocoder, Hansen (silicon vocal tract).
