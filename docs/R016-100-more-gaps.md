# R016 — 100+ More Code Gaps (articulatory knobs + the next tiers)

> Big Mac · 2026-08-11 · Third research pass, aimed at "make more tract, more
> dials/knobs" and "100 more code gaps." Converges the R015 findings with a
> fresh 7+ domain sweep (Maeda articulatory params, LF glottal source,
> conical-KL DSP, nasalization pole-zeros, prosody, timing/pauses, connected
> speech, and objective evaluation).

## Convergent truth (one sentence)
> The voice is a SYSTEM OF KNOBS: the more independent articulatory + source
> parameters the tract exposes (Maeda's 7 articulators, LF glottal terms,
> velum/nasal poles), the more voices it can absorb and reproduce — and
> every knob needs a matching verification meter.

## A. ARTICULATORY MODEL — MORE KNOBS (the big one, "more dials")
Maeda 1990 (the SOTA articulatory parameter set) — 7 independent params:
**jaw height, tongue-body position, tongue-body shape, tongue-tip position,
lip height, lip protrusion, larynx height.** We currently expose ti, td,
lips (aperture only), velum, round (≈protrusion). Missing knobs:
1. **Jaw height** (drives F1). A separate knob.
2. **Tongue-tip vs tongue-body vs tongue-dorsum** as separate knobs (we have
   ONE tongue index). This is THE front-vowel /i/ fix.
3. **Tongue-body shape** (flat vs bunched) — distinguishes /r/ coloring.
4. **Lip height vs lip protrusion** separate (we merged into aperture+round).
5. **Larynx height** (shifts all formants).
6. **Pharyngeal width** knob.
7. **Tongue-groove** (grooved vs flat — /s/ vs /θ/ distinction).
8. **Palatal contact / retroflexion** knob.
9. **Tongue base / root** knob (pharyngeal vowels).
10. **Tract length modulation** (independent of section count).
11. Per-articulator **movement speed / effort** (fast speech undershoot).
12. **Articulator targets as vectors** (VTL-style target sets), not scalars.
13. **Jaw-tongue coupling** (jaw affects tongue height naturally).
14. **Velum as continuous trajectory**, not a scalar.
15. **Lip corners** (spread vs smile) — the smiling-vowel effect.
16. **Lower-lip vs upper-lip** independent.
17. **Tooth / dental constriction** as a real surface, not one section.
18. **Sublingual cavity** (a cavity below the tongue tip — sibilant cue).
19. **Palatal vault / hard-palate geometry** (speaker identity).
20. **Articulatory coarticulation resistance** per articulator (gap #10 R010).

## B. GLOTTAL / SOURCE MODEL (more source knobs)
21. **LF model full param set**: open quotient (OQ), speed quotient (SQ),
    skew — beyond the current Rd.
22. **Glottal formant** (~3 kHz source resonance) control.
23. **Open-quotient control** as an explicit knob (breathiness).
24. **Pulse skew / asymmetry** (independent of OQ).
25. **Subglottal / tracheal coupling** (poles near 1.5 & 3 kHz).
26. **Vocal-fold adduction** (breathy↔pressed continuum).
27. **Diplophonia** (two-pulse cycle, rough voices — gap #48 R010).
28. **Vocal fry / creak register** (gap #47) — low-frequency pulse train.
29. **Modal↔falsetto↔fry register switching** (Titze bi-stable).
30. **Aspiration noise injection point** (at the glottis, spectral-shaped).
31. **Pitch-synchronous spectral tilt** (varies with F0).
32. **Glottal source noise** (aperiodicity) control per register.
33. **F0-intensity coupling** (louder = higher F0, gap #71).
34. **Vocal tremor** (slow 4-7 Hz, gap #78).
35. **Voice-onset/offset transients** beyond the current 10ms ramp.

## C. NASALIZATION (pole-zero fidelity)
36. **Vowel nasalization before nasals** (velum partial-open, gap #52).
37. **Nasal formant + antiformant on vowels** (pole-zero pairs), not just
    the consonant notch.
38. **Velum timing trajectory** (opens/closes over the nasal sequence).
39. **Nasal murmur spectrum** (low-frequency dominance, correct zeros).
40. **Nasal-vowel antiformant tracking** over time.
41. **Nasal coarticulation direction** (anticipatory vs carryover).
42. **Hypernasality control** (for pathologized/vocal characters).
43. **Nasal port cross-sectional area** (the actual knob).

## D. TRACT DSP / PHYSICS (accuracy)
44. **Conical tube sections** (Välimäki) instead of cylindrical (gap P1 R013).
45. **Fractional-delay filtering** in the waveguide.
46. **2-D digital waveguide mesh** (York — accurate but pricier).
47. **Wall vibration / losses** (soft-tissue compliance).
48. **Lip radiation impedance** (frequency-dependent, not a fixed reflection).
49. **Frequency-dependent losses** in sections.
50. **Higher section count** (44 → 60-80) for finer geometry.
51. **Subglottal tract** modeled below the glottis.
52. **Asymmetric (non-uniform) section lengths**.
53. **Nasal side-branch impedance matching** (proper 3-way junction).
54. **Radiation high-pass** (lips boost high freqs — gap #?).

## E. PROSODY / INTONATION
55. **Downstep** (register lowering after a L tone, gap #25).
56. **Focus / contrast accent** (extra prominence on focused word, gap #27).
57. **Given vs new information** deaccentuation.
58. **Post-focal compression** (F0 flattens after focus).
59. **Pitch reset between sentences** (gap #26).
60. **Intonational-phrase boundary tones** (IP tones).
61. **Speech-act contours** (statement/request/exclamation/list, gap #38).
62. **Microvariation in F0/timing/amplitude** (gaps #31-33).
63. **Stress-based amplitude jumps** (~30%, gap #28).
64. **Phrase-final lengthening + declination coupling**.
65. **ToBI-style annotation input** (if markup added).
66. **Lombard effect** (raise F0/amplitude in noise).

## F. TIMING / DURATION / PAUSES
67. **Klatt 11 duration rules** (gap #12).
68. **Stress-based duration** (stressed longer, gap #13).
69. **Consonant-cluster shortening** (gap #16).
70. **Word-frequency effect** (common words faster, gap #17).
71. **Speaking-rate normalization** (gap #18).
72. **Voiced/unvoiced duration asymmetry** (gap #19).
73. **Prosodic-boundary pause model** (pause length ∝ phrase complexity —
    Grosjean/Ferreira).
74. **Topic-shift longer pauses**.
75. **Spontaneous pause variation** (gap #21).
76. **Fillers/hesitation markers** (gap #22).
77. **Function-word reduction timing** (gap #23).
78. **Isochrony: stress-timed vs syllable-timed** per language.

## G. CONNECTED SPEECH / SEGMENTAL
79. **Assimilation** ("ten bikes"→"tem bikes", gap #90).
80. **Elision/deletion** ("facts"→"fæks").
81. **Palatalization / blending** ("got you"→"gotcha").
82. **Resyllabification** ("pick it"→"pi-kit", gap #88).
83. **Function-word reduction to schwa** (gap #89).
84. **Flapping** ("better"→"bedder", gap #56).
85. **Glottal-stop insertion** for vowel-initial (gap #55).
86. **Liaison/linking** across words.
87. **Word-boundary aspiration** (gap #54).
88. **Coalescent assimilation** ("did you"→"didju").
89. **Degemination / cluster simplification**.

## H. VERIFICATION / EVALUATION / LEARNING (the meters for all the knobs)
90. **Formant tracking over time** (not one window — gap #92). Needed to
    verify transitions/coarticulation.
91. **Phoneme-level accuracy check** (gap #94).
92. **Objective quality metrics**: STOI, PESQ, MCD (compute without MOS).
93. **Intelligibility score** from a recognizer.
94. **Speaker-embedding similarity** (does the fitted voice sound like the
    target — the "best voice" meter).
95. **Closed-loop reward from measured intelligibility** (wire the Q-learner /
    MLP planner to objective metrics, gap #100).
96. **MOS/CMOS listening harness** (for when unmuted, gap #93).
97. **A/B regression test** (compare ours vs reference, gap #96).
98. **Per-phone HNR/periodicity report** (voice quality meter).
99. **Real-time factor meter** (streaming viability).

## I. DELIVERY / INTEGRATION
100. **Streaming / real-time TTS** (gap #97).
101. **SSML/markup** (pause/emphasis syntax, gap #98).
102. **Multi-voice in one sentence** (gap #99).
103. **Multilingual phoneme front-end** (per-language phone tables + prosody).
104. **Direct-phone + prosody-markup combined** render API.

## Priority (the first to build, given we are "just the voice")
1. **A2 tongue-tip/body split** — unlocks front vowels /i ɪ e æ/ (the wawa).
2. **A1 jaw knob** — the cleanest new F1 dial (cheap, big effect).
3. **B23 open-quotient knob** — breathiness control (voice quality).
4. **D44 conical KL + D45 fractional delay** — physical accuracy.
5. **F67 Klatt duration + F73 pause model** — rhythm/naturalness.
6. **H90 formant tracking** — the meter to verify all the above.

## Next cycle target
A1 (jaw knob) + A2 (tongue-tip/body split) — the first "more knobs" — then
H90 (formant tracking) so every knob is verifiable.
