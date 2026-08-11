# What WordVoice Teaches Us (applied to a NON-neural engine)

> R008b · 2026-08-11 · the lessons we steal, translated to shallow MLP + Q-learner

## Lesson 1: Acoustic planning BEFORE synthesis (the bound-token)

WordVoice's LLM predicts a word's acoustic attributes *before* generating
its speech tokens — "planning prosody first, then generating sound."

**Our version:** a **two-stage pipeline** in wb_tts:
```
Stage 1 (PLANNER):  text → words → word features (length, stress,
  position, phoneme counts, punctuation) → shallow MLP predicts
  (duration, energy, pitch, tone class) per word
Stage 2 (LEARNER):  Q-table refines the plan per voice-region, choosing
  boundary pauses + tone shapes; reward = measured naturalness (HNR,
  jitter, formant match) of what we actually rendered
Stage 3 (RENDER):   articulatory engine executes the plan
```
This is the bound-token idea with a 2-layer MLP instead of 0.5B params.

## Lesson 2: Decoupled dimensions

WordVoice controls 5 dimensions independently: **duration, boundary
(5 pause levels b0-b4), energy, pitch, tone (7 classes)**.

**Our version:** wb_tts events get exactly these fields, computed
independently:
- `dur_mult` — word stretch (0.5×..2×)
- `boundary` — b0 none .. b4 long pause after the word
- `energy` — glottis intensity multiplier (0..1)
- `pitch` — f0 offset (−1..1 → −6..+6 semitones)
- `tone` — f0 contour shape over the word

## Lesson 3: The 7-tone taxonomy (we implement it directly)

WordVoice's tone classes → concrete f0 contour shapes for our renderer:
| Tone | f0 shape over the word |
|---|---|
| flat | constant |
| rise | linear up |
| strong_rise | steep up (more than rise) |
| fall | linear down |
| strong_fall | steep down |
| peak | up then down |
| valley | down then up |

These are cheap to render in C11 — the f0 glide is already per-phone.

## Lesson 4: Boundary levels (pauses)

b0 = no pause, b1 = micro (40ms), b2 = short (120ms), b3 = medium (280ms),
b4 = long (500ms). Punctuation maps: comma→b2, period→b3, question→b3+
pause, paragraph→b4.

## Lesson 5: Dual mode (auto-pilot vs manual)

WordVoice supports auto (model decides) OR manual (user sets attributes).
**Our version:** wb_tts gets a control syntax so a user can override any
word: `"i [really:dur=1.5,energy=0.9,pitch=+2,tone=peak] want it"`
Parser + MLP defaults + manual overrides merge.

## Lesson 6: The annotation pipeline (how to make our own data)

WordVoice-5A: align (MFA+Qwen3FA), loudness edge-optimization, consistency
filter. **Our version** (no forced aligner needed):
1. Render a sentence with our TTS → we KNOW the ground-truth plan
   (duration/energy/pitch/tone per word) — self-supervised labels
2. Measure the actual output with wb_measure (f0, energy, HNR) per word
3. Train the MLP on (word features → plan) with the rendered+measured
   result as the label — the learner learns what actually worked

## Lesson 7: Fine-grained modulation after tokens

WordVoice adds a modulation module in the token→waveform stage to bridge
discrete tokens ↔ continuous waveform. **Our version:** the Q-learner's
tract tunes (wb_learn: brighter/darker/open/closed lips) are exactly this
— post-plan articulation modulation per word.

## The non-neural architecture (what we build)

```
text
 └─ word features: len, stress(dict), position, phones, punct, is_question
     └─ SHALLOW MLP (1 hidden layer, ~16 units, trained in C11)
         └─ predicts dur_mult, energy, pitch, tone_class, boundary
             └─ Q-LEARNER (8x8x5 table, wb_learn) refines tone/boundary
                 └─ ARTICULATORY RENDER (wb_tts engine, 39 phones)
                     └─ wb_measure scores the result → reward → Q update
```

Training data: our own renders (ground truth known) + LibriSpeech
(real-speech measurements). MLP ~200 params — trains in seconds on one core.
