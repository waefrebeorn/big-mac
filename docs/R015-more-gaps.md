# R015 — More Gaps: 7-hop research after the articulation fix

> Big Mac · 2026-08-11 · Second research pass. The R014 fix (tongue reaches
> tube) closed the #1 gap and exposed the next layer. This doc finds MORE
> gaps via fresh 7-hop research, converged on the two remaining wawa-killers
> (front vowels, fricatives) plus the next tier of voice-quality, timing,
> register, and nasalization gaps.

## The 7 hops (each a distinct domain, converging)

### Hop 1 — Articulation / vowel acoustics (the /i/ fix)
| Source | Finding |
|--------|---------|
| eupmc 2606790 / acoustic phonetics | For /i/: "jaw and tongue body raised, creating a **narrow front cavity**, and the tongue body moved forward, **enlarging the back cavity**." |
| IU vocal pedagogy (Miller) | Front cavity SMALLER than back cavity for /i ɪ e æ y/; tongue tip against lower teeth; velum closed. |
| White Rose thesis (2-D waveguide) | Positions of lip, **tongue tip, tongue body, tongue dorsum** are separate articulators — one lumped "tongue index" can't make front vowels. |

→ **Converges on:** high F2 /i/ requires a narrow-front + enlarged-back tract,
which one broad tongue hump cannot produce. This is R014 gap #63 in the flesh.

### Hop 2 — Fricative noise sources (the other wawa half)
| Source | Finding |
|--------|---------|
| Birkholz 2006, "Noise Sources and Area Functions for Fricative Consonants" | Each fricative needs its own **noise-source position** AND **area function** (constriction) — the two are coupled. |
| Intervocalic fricative perception (EP) | Sophisticated noise-source model in articulatory synthesis drives /f v s z ʃ ʒ/ perception. |
| R014 finding | The frication path works in isolation (30% high-freq) but the short coarticulated fricative never forms its constriction in-render. |

→ **Converges on:** frication needs source-position + area-function + **timing**
(the constriction must be formed at the moment noise is injected).

### Hop 3 — Voice quality / spectral tilt (Garellek model)
| Source | Finding |
|--------|---------|
| Garellek, "The phonetics of voice" | Voice quality = **4 spectral-tilt slopes (H1-H2, H2-H4, H4-2kHz, 2k-5k)** + **HNR (cepstral)**. Breathy = higher tilt + lower HNR; modal in between; creaky = lower tilt + lower HNR. |
| Garellek 2022 | H1-H2 and other tilt slopes are THE standard voice-quality measures. |

→ **Converges on:** a breathiness/pressed control should manipulate multiple
tilt slopes + HNR together, not just H1-H2. We have H1-H2 + HNR only.

### Hop 4 — Stop bursts + VOT
| Source | Finding |
|--------|---------|
| JASA burst spectrum (voicing) | Burst spectrum + VOT are the stop voicing/place cues. |
| EduHK acoustic consonants | VOT = release burst + short frication + time to voicing onset. |

→ **Converges on:** VOT is a real, measurable stop cue (R012 did closure+burst;
VOT contrast could be sharpened).

### Hop 5 — Duration / rhythm (Klatt rules)
| Source | Finding |
|--------|---------|
| Klatt 1976/1979 | 11 duration rules; **unstressed segments considerably shorter**; stress-timed English. |
| van Santen 1993 | Quantitative, data-driven segmental duration modeling. |

→ **Converges on:** the engine's flat durations are a big naturalness gap
(R010 B11-B20); Klatt stress rules are cheap and implementable.

### Hop 6 — Register / vocal fry (source model)
| Source | Finding |
|--------|---------|
| Titze 2014, "Bi-stable vocal fold adduction" | Modal↔falsetto = bi-stable fold adduction; register = fold activation pattern. |
| Vocal fry register (wiki) | Fry = loose glottal closure, air "bubbling" slowly with popping. |

→ **Converges on:** a register/creak parameter (modal/falsetto/fry) belongs in
the glottal source (R010 D47/D79).

### Hop 7 — Nasalization / velum control
| Source | Finding |
|--------|---------|
| Oral configs during vowel nasalization | Nasalization = velopharyngeal port opening; nasal formant + **antiformant (pole-zero pairs)**. |
| Hypernasality / nasalization measurement | Nasal coupling adds formant-antiformant pairs (nasal formant + anti-resonance). |

→ **Converges on:** vowels before/after nasals should be nasalized (velum
partially open), not just the nasal consonant itself (R010 D52/D53).

## Convergent truth
> The two remaining "wawa" killers are (1) front vowels need a **narrow-front /
> enlarged-back** tract that one tongue hump can't make, and (2) fricatives
> need the **noise source, constriction area, AND timing** to line up.
> Beyond that, the next tier is voice-quality tilt (multi-slope), Klatt
> duration rules, register/creak, and nasal coarticulation.

## More gaps found (prioritized)

**P0 — the wawa, implementable blind:**
- **F1. Front-vowel articulation**: add a narrow front constriction (or a
  tip/body distinction) so /i ɪ e æ/ reach high F2. Structural, measurable.
- **F2. Fricative frication timing**: ensure the constriction is formed at the
  noise-source position the moment noise is injected (longer/faster fricative
  onset). Structural (hf>3k measurable).

**P1 — voice quality + timing, partly needs a listen:**
- **F3. Multi-slope spectral tilt + HNR breathiness**: add H2-H4, H4-2kHz,
  2k-5k tilt slopes; wire a breathiness control that raises tilt + lowers HNR
  (Garellek). Measurable.
- **F4. Klatt duration rules**: stress-based + unstressed shortening + phrase-
  final lengthening (R010 B11-B16). Measurable (word duration).
- **F5. Register/creak**: modal/falsetto/vocal-fry glottal parameter + phrase-
  final creak (D47/D79). Needs a listen for quality.
- **F6. Nasal coarticulation**: velum partial-open on vowels adjacent to
  nasals (D52/D53). Structural.

**P2 — sharpening:**
- **F7. VOT contrast refinement** (D40).
- **F8. Fricative area-function per phone** (Birkholz 2006) — tune each
  fricative's constriction width + noise amplitude.

## Next cycle target
F1 (front-vowel narrow constriction) and F2 (fricative timing) — the two
remaining wawa-killers — then F3 (tilt/HNR breathiness).
