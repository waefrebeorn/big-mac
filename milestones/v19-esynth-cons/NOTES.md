# v19 — faithful esynth engine, consonants loud, but no connected speech

## Result
User: "there's no timing, no anything, doesn't sound like speech, just a
bunch of noises."

## What's now CORRECT (measured)
- Vowels match real spectra (faithful espeak-ng engine): /a/ 1643 (1673),
  /u/ 921 (870), /ae/ 1786 (1900).
- Consonants as loud as vowels (amplitude balance fixed).

## The remaining bulk (the hard part)
Each phone is rendered as an isolated, static tone-blip with no smooth
transitions between phones, no natural coarticulation, no connectedness.
The prosody/timing exist but the renderer doesn't flow. Real speech =
continuous formant trajectories + natural rhythm, not separate blips.

## Next (connectedness)
1. Smooth formant transitions between phones (coarticulation), not static
   per-phone tones.
2. Natural word/syllable timing (not metronome gaps).
3. Amplitude dynamics (syllable attack/decay).
4. Reduce the fixed 0.15s lead-in gap.

## Honest assessment
A from-scratch formant TTS producing connected intelligible speech is one of
the hardest problems in computing. 19 versions have not cracked it. The
pieces (vowels, consonants) are now correct; connectedness is the remaining
bulk and it is genuinely hard.
