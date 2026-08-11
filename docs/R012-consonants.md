# R012 — Consonant Hardening: fixing the "wawa" / mushy consonants

> Big Mac · 2026-08-11 · the intelligibility pass. The vowels were getting
> clean, but the consonants sounded like "wawa" — mushy, indistinct, no
> clear place/voice/manner cues. This doc tracks the articulation work
> that turns consonant gestures from a smear into discrete, place-cued
> events.
>
> **Core insight:** articulatory synthesis is only intelligible when the
> consonants have *landmarks* (Stevens 2002) — a stop must CLOSE, HOLD, and
> RELEASE; a fricative must put its noise where its front-cavity resonance
> sits; a nasal must open the velum. Without those landmarks the tract just
> mumbles.

## A1 — Fricative spectral shaping (Birkholz 2006)
The /s/ vs /ʃ/ vs /f/ distinction is carried by the *front-cavity
resonance* of the noise source, not by amplitude. We band-pass the
turbulence noise to each fricative's spectral center:
- /s/ ~6000 Hz, /ʃ/ ~3200 Hz, /f/ ~2500 Hz (labiodental)
- implemented as a per-phone biquad bandpass (`wb_biquad_bandpass`),
  state kept across the phone.

## A2 — Context-dependent stop bursts (Dorman 1977)
A stop's place (/p/ vs /t/ vs /k/) is cued by the *burst spectrum* and the
F2 transition. We shape the release burst to each stop's place center:
- /p/ /b/ labial ~500 Hz, /t/ /d/ alveolar ~4500 Hz, /k/ /g/ velar ~2000 Hz
- burst is a decaying noise transient shaped by the same biquad.

## A3 — Nasal tuning (partial velum)
/m/ /n/ /ŋ/ need the velum open to couple the nasal cavity. Full open
velum smears the vowel; we use a *partial* velum (0.7) for m/n/ŋ nasal
timbre while keeping the oral coupling for vowel color.

## A4 — Stop closure phase + affricate structure (this pass)
The old code fired a stop's burst at phone onset with **no closure
silence** — a stop has to HOLD the tract closed before releasing, or it
reads as a mushy blip. This pass adds:

- **Closure phase** (~55% of a stop's duration): tract closed, silent for
  voiceless /p t k/, low-amplitude **voice bar** for voiced /b d g/
  (voicing continues through the closure, as in real speech).
- **Release** (remainder): the place-shaped burst (A2) + a light
  aspiration tail for voiceless stops.
- **Affricates CH/JH** are now stops that release into their homorganic
  fricative (CH → ʃ, JH → ʒ) instead of a burst — a short ~40% closure then
  the fricative, matching [t͡ʃ] / [d͡ʒ].

### Why this fixes "wawa"
Measured on `cat` / `bad` / `kick` before vs after: the waveform now shows
the landmark structure `[closure-silence] → [burst] → [vowel] → [closure]
→ [burst]` instead of one undifferentiated noise block. The voiced-stop
voice bar (`bad`) appears as a distinct low-energy closure between the
vowels. A stop that holds then releases is intelligible; one that just
puffs is not.

## Verification
- `make` green, `make test` green (F0=140.1 Hz, jitter=0.90%,
  shimmer=6.66%, HNR=14.9 dB on the tone gate).
- Waveform landmark check: `cat`, `bad`, `kick`, `church`, `judge`
  rendered and the 2–5 ms RMS envelope inspected — closure gaps + bursts
  present.
- Pure vowels are periodic (per-frame autocorr 0.82–0.90, HNR +6.5 to
  +9.4 dB); consonant-flanked vowels still drop to ~+1 dB HNR — the
  transition smear is reduced but **the final acoustic judgment needs a
  listen** (Big Mac is muted while babies sleep).

## Still open (honest)
- Consonant→vowel formant transitions still undershoot (R010 §A) — the
  biggest remaining intelligibility gap.
- Liquids /l/ (lateral) and /r/ (retroflex) use crude tongue shapes; no
  dedicated lateral/bunched articulation yet.
- Voiceless-aspirated stops (VOT) are only approximated by the aspiration
  tail; a real VOT should delay the following vowel's voicing onset.
- Overall HNR is low (~−5 dB) because fricatives + breathiness dominate;
  the vowel harmonics need to be cleaner and louder relative to noise.

## References (7-hop grounding)
- Birkholz, P. (2006) — fricative source/front-cavity spectral shaping.
- Dorman, M. et al. (1977) — stop burst spectra cue place of articulation.
- Stevens, K. (2002) — *Acoustic Phonetics*, landmark theory.
- VocalTractLab (Birkholz) — evidence articulatory synthesis can be
  high-quality and intelligible.
- Klatt (1980) — duration rules; R010 gap list.
