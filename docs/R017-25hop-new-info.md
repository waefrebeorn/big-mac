# R017 — 25-search Kevin-Bacon: information we didn't have

> Big Mac · 2026-08-11 · 25 web searches across 15+ domains (singing, voice
> conversion, perception, glottal physics, tone languages, prosody, motor
> control, real-time audio). Goal: find NEW information we don't have — not
> re-confirm old gaps. Convergent truth below.

## Convergent truth (one sentence)
> Pitch, timbre, and voice quality are not three things — tone languages
> fuse pitch-contour with phonation-type (breathy/creaky), the singer's
> voice is a resonance-TUNING trick, and the strongest intelligibility cue
> is the TEMPORAL ENVELOPE, so the highest-leverage work is: implement
> phonation-as-a-knob, resonance tuning for high F0, and a feedforward+
> feedback closed loop (which is exactly what the Q-learner already is).

## The new information (we didn't have this before)

### 1. Tone languages fuse PITCH with PHONATION (biggest new idea)
- **Vietnamese** 6 tones: `ngã`/`sắc` are high-rising but one has
  **glottalization mid-tone**; `nặng` is short + **creaky** at end; `huyền`
  long + **breathy**. Mandarin 4 tones (55/35/214/51) are pure pitch-contour.
  Thai 5 tones. Japanese = binary pitch-accent (we have `-pa`).
- **We don't have:** phonation-type switching per tone. A tone system needs
  contour AND breathy/creaky/glottalized. This is implementable (phonation
  knob + tone contour).

### 2. The singer's formant + resonance tuning (Big Mac SING)
- **Singer's formant** = a 2–4 kHz peak from R3/R4/R5 converging, made by
  **low larynx + narrowed epilaryngeal tube**. Lets the voice cut through an
  orchestra (sound carries 2-4kHz).
- **Formant tuning**: sopranos align tract resonances (esp. F1) WITH a
  harmonic of the sung pitch — R1 (~300-800 Hz) overlaps the soprano F0
  range. When F0 rises near F1, the singer modifies the vowel to keep the
  resonance on a harmonic.
- **We don't have:** a resonance-tuning mode (move F1 to track high F0), nor
  the epilaryngeal constriction that builds the singer's formant.

### 3. Whisper is a different SOURCE (open glottis, no F0)
- Whisper = glottis OPEN, **turbulent airflow** as the excitation, no vocal
  fold vibration → no F0. Whisper-to-speech conversion exists (glottal flow
  synthesis + pitch estimation).
- **We don't have:** a whisper mode (open-glottis turbulence source, F0 off).

### 4. Source and tract are NOT independent (accuracy)
- **Source-tract interaction**: standing-wave pressures in the tract FEED
  BACK and modify glottal airflow/fold motion. Level-1 interaction = glottal
  flow change due to tract pressure. The epilaryngeal tube is an **impedance
  matcher** between glottis and tract.
- **We don't have:** any tract→glottis coupling. We treat source and filter
  as independent. Even a simple feedback term (glottal reflection scaled by
  tract back-pressure) would add realism.

### 5. Temporal envelope is THE intelligibility cue (the cheat)
- Shannon 1995: **speech is intelligible from slowly-changing temporal
  envelope in a few spectral channels** — you don't need spectral fine
  structure. (Music needs it; speech mostly doesn't.)
- **We don't have:** we chase spectral fidelity, but the biggest perceptual
  win is crisp amplitude/timing envelopes (the temporal contour).

### 6. Speech motor control = feedforward + feedback (the Q-learner)
- Speech is executed by **feedforward** (planned articulation) PLUS
  **feedback** (auditory + somatosensory error correction), hierarchical
  state-feedback control (Perkell, Kawato). Speakers acquire auditory goals
  and maintain them via feedback.
- **This is EXACTLY the Big Mac loop**: wb_tts = feedforward plan, wb_measure
  = sensory feedback, wb_learn Q-table = learning the correction. We already
  have the architecture; we don't reward/close the loop end-to-end (R010 gap
  #100).

### 7. Fujisaki intonation model (implementable prosody)
- F0 = superposition of **phrase commands** (slow) + **accent commands**
  (fast) on log-F0, physiologically meaningful, standard for synthesis.
- **We don't have:** a parameterized F0 model. We hand-roll contours. Fujisaki
  gives a principled, compact F0 control (phrase + accent) — better than the
  current ad-hoc contours.

### 8. ΔF / vocal-tract-length normalization (the tract-length knob)
- Average formant spacing **ΔF = c/(2·VTL)**; a longer tract → all formants
  lower + more closely spaced. VTL can be scaled (Nordström-Lindblom:
  scale by F3 ratio). Male/female ≈ 10-15% VTL difference.
- **We don't have:** a global tract-length / formant-scale knob. THIS one
  genuinely moves all formants (unlike the jaw/lip attempts). Character
  identity largely = VTL.

### 9. Articulatory-to-acoustic INVERSION (the math for "absorb")
- Analysis-by-synthesis recovers vocal-tract shape from a signal (chain
  matrices + Maeda model; cepstral ABs). This is the full inverse of our
  render. `wb_fit` already does 44-diameter fitting — this research confirms
  the approach and adds cepstral matching (better than formant-only).

### 10. Pitch shifting done right: PSOLA / spectral envelope / WORLD
- TD-PSOLA preserves the spectral envelope while shifting pitch (pitch marks +
  overlap-add). FFT/phase-vocoder variants scale magnitude RELATIVE to a
  cepstral spectral envelope. WORLD vocoder (F0 + spectral + aperiodic) is the
  modern standard. Chipmunk = formants shifted with pitch (wrong).
- **We don't have:** a working formant-preserving pitch shifter (wb_shift was
  the broken attempt). TD-PSOLA is the robust path — and it's what a real-time
  voice changer needs.

### 11. LTAS for speaker identity (the "best voice" meter)
- Long-term average spectrum (average spectrum over a long sample) captures
  speaker resonance + source characteristics; used in speaker recognition and
  voice screening.
- **We don't have:** an LTAS identity signature to verify "does the fitted
  voice sound like the target" — the objective meter for the absorb loop.

### 12. LF/Rosenberg glottal parameters (the source knobs)
- LF model: open-phase sine + **return phase** (equivalent to a 1st-order
  low-pass; the return-phase quotient adds high-freq spectral tilt). Open
  quotient + asymmetry set the glottal-formant/low-frequency peak.
- **We don't have:** explicit OQ + return-phase (RQ) controls. We have
  `normalized_lf` (Rd) but not the individual OQ/SQ/RQ knobs.

### 13. Real-time voice agent pipeline (delivery)
- DSP voice changers reach **<20ms latency**; the pipeline = VAD → (STT) →
  streaming → (TTS) → playback with a latency budget; barge-in <100ms;
  >1s feels sluggish. Audio fails silently — needs health checks.
- **We don't have:** a streaming/real-time render (R010 #97), VAD, or a
  low-latency DSP voice-changer path for the core product.

### 14. Tense/lax voice = laryngeal constriction (valves model)
- Voice quality = a system of **valves** (glottal, epilaryngeal, etc.).
  Tense = constricted + high regular pitch (HNR unchanged); breathy = spread +
  high tilt + low HNR; creaky = constricted + low F0 + low HNR. Esling's
  laryngeal articulator model ties tone + voice quality + larynx constriction.
- **We don't have:** a valves/laryngeal-constriction model, nor the
  tense↔breathy↔creaky continuum as a single knob.

### 15. Target-locus scaling (coarticulation, done right)
- Formant transitions are **scaled copies** of consonant-specific shapes; the
  scale factors come from the vowel onset/offset. Speaking rate changes the
  coarticulation degree. This formalizes the F2-locus work.
- **We don't have:** the scaled-shape model (R013 did F2 locus as a table;
  this gives the principled transition shape + rate dependence).

## What this means for Big Mac (highest-leverage, in order)
1. **Tone + phonation** (info #1): a `-tone` mode that per-tone sets contour +
  breathy/creaky/glottalized phonation. Big, new, implementable.
2. **Resonance tuning for singing** (info #2): move F1 to track high F0 so
  high notes stay clear — the musical agent's core trick.
3. **Tract-length / ΔF knob** (info #8): the ONE tract knob that verifiably
  moves all formants; gives character identity + matches male/female.
4. **Feedforward+feedback closed loop** (info #6): wire wb_measure reward into
  wb_learn so the Q-table tunes articulation from measured quality — "absorb
  all the info and make the best voice with math."
5. **Fujisaki F0** (info #7): principled phrase+accent intonation.
6. **TD-PSOLA pitch shifter** (info #10): the real-time VC core (replaces the
  broken wb_shift).
7. **Whisper mode** (info #3): open-glottis turbulence source.
8. **LTAS identity meter** (info #11): verify the absorb loop.

## Next cycle target
Info #1 (tone+phonation) and #2 (resonance tuning) — both are Big Mac's
musical identity and both are implementable. Then #8 (tract-length knob) as
the one genuinely-working new knob.
