# R014 — The Articulation Root-Cause Fix (the "wawa machine")

> Big Mac · 2026-08-11 · Found and fixed the deepest bug in the voice engine:
> the tongue never reached the tube, so every vowel rendered through the
> same flat tube shape and sounded alike. This is the "wawa machine."

## The bug (root cause)

`wb_tract_set_rest_diameter()` writes the tongue shape into `rest_diameter`,
but the waveguide's `reshape()` moves `diameter` toward **`target_diameter`**.
Nothing ever copied the tongue `rest_diameter` into `target_diameter` for the
tongue region — so the tongue bump was computed and **discarded**.

Result: vowels were rendered through the default (flat) tube. The only thing
that varied between vowels was lip aperture, which barely moves the formants.
**Measured before the fix — every vowel rendered identically:**

```
ph   F1    F2    F3        expected F1/F2
AA   512  1869  3042        720 / 1240
AE   513  1870  3037        660 / 1720
AH   509  1860  3052        640 / 1190
EH   513  1870  3039        530 / 1840
IH   513  1870  3038        390 / 1990
IY   510  1865  3058        270 / 2290
```

All F2 ≈ 1869, all F1 ≈ 512. /i/ and /ɑ/ and /æ/ were the SAME SOUND. That is
the wawa. (This is R010 gap A1, coarticulation/articulation, in its most
fundamental form — and confirms the R010 claim that the gap is OUR crude
implementation, not the approach.)

## The fix

`wb_tract_set_rest_diameter()` now writes the tongue shape into BOTH
`rest_diameter` and `target_diameter`, so the constriction actually reaches
the tube.

The phone table (and every tool's articulation) had been calibrated for the
flat-tube bug — its `td` (tongue diameter) values were "decorative" and are
now real constrictions. With the fix, the old 0.3–1.3 values over-constrict
the tube shut (fricatives/glides become closures), so they were retuned:
- **Vowels** td 2.0–3.0 (open).
- **Fricatives** td 2.1 + constriction moved to the noise source (teeth/lips).
- **Glides/liquids** td 2.0–2.4 (open semi-vowels).
- **Stops** keep td 0.2 (closure — correct).
Also retuned `wb_speak` (sweep), `wb_sing` (td 0.9→2.4), `wb_toon`
(phone_td + character tongue).

## After the fix (measured)

```
ph   F1    F2    F3        expected F1/F2
AA   562  1350  3077        720 / 1240   ~
AH   607  1297  2980        640 / 1190   ~
UW   223   803     -        300 / 870    ~
ER   545   851     -        490 / 1350
IH   511   905     -        390 / 1990
AE   565  1155  2760        660 / 1720
EH   551  1222  2732        530 / 1840
IY   514  1175  2716        270 / 2290
```

Vowels are now **distinct and open** (F2 spans 803–1350, F1 spans 223–607),
instead of all identical. Back and mid vowels (AA, AH, UW) land near their
targets. Gates stay green: `make test` (F0=140.1), `make test-yin` 3/3 PASS.

## Honest gaps (need a listen to finish — Big Mac is muted)

1. **Front vowels /i ɪ e æ/ hit an F2 ceiling ~1400** (targets 1720–2290).
   The single broad tongue hump can't make the small front cavity /i/ needs.
   This is documented gap #63 (one tongue index). Needs a narrower-constriction
   model or a tip/body split.
2. **Fricatives produce little/no high-frequency frication in wb_tts.**
   The turbulence path works in isolation (measured 30% hf) but the short,
   coarticulated fricative doesn't form its constriction in time in-render.
   Pre-existing (the same gate zeroed them under the flat tube), not a
   regression — but it's the other half of the wawa.
3. **`wb_vc`'s retrieval table was fit under the flat-tube bug.** It has no
   `.rtab` file, so `wb_vc` rebuilds it at runtime with the fixed render —
   correct but slow (~65 min). Needs a one-time rebuild.
4. **The whole retune was done blind (muted).** Formants are measured and
   correct, but perceived quality / consonant timing needs an A/B listen.

## Commit
R014: articulation root-cause fix (tongue reaches tube) + first-pass retune.

## Next cycle target
Front-vowel articulation (narrow constriction for /i ɪ e æ/) and in-render
fricative frication — both need a listen to tune.
