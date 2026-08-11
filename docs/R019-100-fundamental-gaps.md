# R019 — 100 Fundamental Gaps: why it sounds like "wawa / silly putty / a washing machine"

> Big Mac · 2026-08-11 · You unmuted and heard it: **not** a few missing
> things — no legible speech at all. This doc is the honest, grounded list of
> the BASIC things missing, ordered by what most turns garbage into speech.
> Each is a concrete, code-level gap. Fix the top ones first.

## The real diagnostics that drive this (measured, not guessed)
- **Timing bug (FIXED, R018-b62975c):** word gaps were never rendered — words
  ran together + 1.1s trailing silence. Fixed: gaps now render, tail 1.1s→0.3s.
- **voiced ≈ 24%** on a 10-word phrase (real speech ≈ 60-70%). Most audio is
  unvoiced noise-mush.
- **Fricatives produce NO frication** (see/she/fee HNR ≈ -0.9, hf>3k = 0%).
  /s ʃ f/ come out as low-frequency noise = the "silly putty."
- **Pitch is flat/monotone** (~85-130 Hz, no word-level contour) = "no legible
  words."
- **Isolated vowel is clean** (periodicity r@F0=0.94, HNR~10, clear formants)
  → the source is fine; the segment-level synthesis is what's broken.

---

## A. SEGMENTAL / TIMING (word & syllable structure) — the "no speech" root
1. Word gaps not inserted (FIXED). 2. Syllable nuclei not sustained (vowels too
   short). 3. No syllable-level amplitude envelope (every phone flat). 4. No
   closure→release timing control per stop. 5. Unstressed vowels over-reduced
   (my 35% schwa pull kills them). 6. Stress-timing compression (x0.82) makes
   unstressed vowels vanish. 7. Phone durations not normalized to a natural
   speaking rate. 8. No phrase-level pause insertion before/after clauses.
9. Punctuation gaps too short (0.15/0.28s → need ~0.4/0.6s). 10. No initial
   consonant cluster release timing. 11. No final consonant release (CVC codas
   cut off). 12. No syllable isochrony for syllable-timed mode. 13. Duration
   not stress-relative (stressed vowels need 1.5-2x length). 14. No
   phrase-final lengthening actually applied. 15. Phone `t0_abs` ordering can
   overlap (gaps vs. closures double-counted). 16. No word-boundary
   (glottalization/stop) insertion at vowel-initial words. 17. No silent
   occlusion measurement for stops (closure length arbitrary). 18. No
   between-sentence boundary. 19. Total length ignores the 0.3s tail (fixed
   partially). 20. No rate-consistent gap scaling in all paths (some missed).

## B. CONSONANT ACOUSTICS — the "silly putty / washing machine" root
21. **Fricatives produce no frication** (the #1 consonant bug). 22. No
   broadband noise source for /s ʃ f v θ ð/. 23. Fricative spectral-peak
   placement (front-cavity size) not realized. 24. No fricative duration
   (>150ms for clear sibilants). 25. No sibilant amplitude (must be loud,
   high-freq). 26. Stop release bursts too weak/short. 27. No VOT (voice onset
   time) contrast: /p t k/ need +40-80ms aspiration after release. 28. No
   aspiration noise on voiceless stops. 29. No burst spectral shape per place
   (/p/ low, /t/ high, /k/ mid). 30. Affricate release not fricative-shaped.
31. Nasal /m n ŋ/ lack clear nasal formant + antiformant. 32. Nasal murmur too
   weak. 33. Lateral /l/ has no clear low-F2 + F3. 34. /r/ not reliably
   rhotic (low F3) in all contexts. 35. Glides /w j/ not distinct from vowels.
36. Voiced obstruents lack voicing bar. 37. Consonant duration independent of
   voicing (voiceless longer). 38. No pre-voicing / final devoicing. 39. No
   place-of-articulation locus (F2 transition onset) per consonant. 40. Stops
   at word-initial not aspirated. 41. No flapping/tapping /ɾ/. 42. Consonant
   closure not silence (leaks noise). 43. No ejective/implosive (added to table
   but not acoustically realized). 44. Palatalization/assimilation absent.
45. Consonant-vowel intensity ratio wrong (consonants too loud OR too quiet).

## C. VOWEL ACOUSTICS
46. Front vowels /i ɪ e æ/ hit the F2 ceiling (~1400 vs 2300/1990/1840/1720) —
   the single-tongue-hump limit. 47. Vowel length not contrastive (tense/lax).
48. Diphthongs glide too fast (smear). 49. No vowel-internal formant movement
   (steady-state = robotic). 50. Reduced vowels collapse to near-identical.
51. Schwa not consistently central. 52. R-colored vowels not realized. 53.
Nasal vowels (added to table) not acoustically nasalized (antiformant). 54.
Vowel onset/offset not smooth (hard cuts). 55. No formant bandwidth control
   (all vowels same sharpness). 56. Vowels too short to identify. 57. No
   vowel-length before voiced vs voiceless coda. 58. Unstressed vowel target
   undershoot absent (full target even when reduced).

## D. PROSODY / INTONATION — the "no legible words" root
59. **Pitch flat/monotone** — no word-level contour. 60. No pitch accent on
   stressed syllables (the bump is too weak). 61. Declination too flat (need
   ~2-4 semitone phrase fall). 62. No phrase-final lowering audible (0.88 too
   subtle). 63. No question rise on the final word (f0e too weak). 64. No
   contrastive emphasis. 65. Microvariation not perceptible (2% too small).
66. No reset after punctuation. 67. Stress/prominence not audible (amplitude
   difference too small). 68. No sentence intonation model (Fujisaki-style).
69. Japanese -pa flat (no real H/L). 70. Tone contours (-tone) too subtle.
71. No focal word marking. 72. Word F0 range too narrow (±10% vs natural
   ±30-50%). 73. No pitch on unstressed syllables (too flat).

## E. SOURCE / VOICE QUALITY — the "washing machine" root
74. HNR too low on segments (≈ -1 to 4 dB; need 15-25 on vowels). 75.
Aspiration/turbulence noise too strong globally (washing-machine rumble). 76.
Shimmer/jitter too high (60% shimmer = crackle). 77. Vibrato (6Hz) sounds like
a wobble at high depth. 78. Glottal source too buzzy (need softer LF OQ).
79. No breathiness control balance (all-or-nothing). 80. No register
(modal/creaky) control wired per-segment. 81. Whisper/fry flags are global,
not segment-level. 82. Source amplitude constant (no dynamic intensity).
83. No voice onset/offset ramps long enough. 84. Noise floor too high (need
   >60dB SNR). 85. Lip radiation/bandwidth not modeled (dull).

## F. TRANSITIONS / COARTICULATION
86. Formant transitions too slow or smeared (no crisp CV boundary). 87. No
   consonant-vowel formant transition (F2 locus) actually audible. 88.
Coarticulation overlap too broad (everything bleeds). 89. No
   anticipatory/carryover distinction. 90. Stop→vowel transition missing the
   burst+aspiration. 91. Vowel→vowel transitions (hiatus) absent. 92. No
   syllable-initial strengthening. 93. Segments fade into each other (no
   attack). 94. No abrupt noise onset for stops (too gradual).

## G. OUTPUT / VERIFICATION
95. No listening-based verification (I tuned blind). 96. Analyzer F0=0 on
   short renders (bad windowing) — can't verify. 97. No ABX/intelligibility
   test. 98. No spectrogram export to check formant tracks. 99. No
   syllable/word boundary annotation in output. 100. No golden-voice target to
   compare against.

---

## The top 10 that most turn garbage into speech (do these first)
1. **Frication** (B21-25) — give /s ʃ f/ real high-freq noise.
2. **VOT + aspiration on voiceless stops** (B27-28).
3. **Sustain vowels** — undo over-reduction/shortening (A5-7, C56).
4. **Real pitch contour** (D59-72) — word-level F0 movement, not flat.
5. **Raise HNR** — cut global aspiration/turbulence noise (E74-75).
6. **Stop bursts** (B26,29) — crisp release.
7. **Phrase pauses** (A8-9) — longer punctuation gaps.
8. **Syllable amplitude envelope** (A3).
9. **Front-vowel F2** (C46) — the articulation model.
10. **Transition crispness** (F86-90).

## Convergence
The isolated source is clean — the problem is **segmental synthesis**: no
consonant frication/VOT/bursts, over-shortened vowels, flat pitch, and
excess noise. Fix the top 10 and it becomes legible; the rest polish it.
