# v17 — additive renderer with steady vowels (end of the invented-renderer line)

## Result
User: "it just sounds like an alien and I can't discern there being any speech."
After 17 versions across THREE invented renderers (KL waveguide, Klatt
cascade, additive harmonic) — NONE produced intelligible speech.

## Honest conclusion
My hand-invented renderers keep failing. The research consistently showed
espeak-ng (additive formant synthesis) IS intelligible. The right move is
NOT to keep inventing, but to FAITHFULLY implement espeak-ng's proven
synthesis pipeline (synthesize.c + wavegen.c + English phoneme data) as the
spec, in pure C11 (SLERM — write every byte myself from the reference).

## Fresh start plan (R015)
1. Study espeak-ng's COMPLETE synthesis (glottal source, formant control,
   per-phone formant trajectories, amplitudes, the full pipeline).
2. Port it faithfully to big-mac in C11 — not approximations.
3. Verify each phone against espeak-ng's output (the oracle).
