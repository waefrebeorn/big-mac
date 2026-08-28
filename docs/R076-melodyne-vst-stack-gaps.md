# R076 — Melodyne Parity + VST Recreation + Full Stack Dominance

## Completed (hops 232-236)

| Hop | Gap | What | Gate | Location |
|-----|-----|------|------|----------|
| 232 | N1 | M/S stereo widening + Haas effect | 6/6 | src/wb_stereo.c |
| 233 | N2 | YIN pitch detection | 6/6 | src/wb_yin.c |
| 234 | N5 | Phaser (allpass cascade + LFO) | 5/5 | src/wb_phaser.c |
| 235 | M1 | Auto-tune pitch correction | 5/5 | src/wb_pitch_correct.c |
| 236 | V1 | Moog ladder filter | 6/6 | src/wb_ladder.c |

## Melodyne Parity (research SA-0)

| # | Technique | Core Algorithm | SSE2 Feasibility |
|---|-----------|---------------|------------------|
| M1 | Auto-tune pitch correction | YIN + scale snapping + resampling | ✅ DONE |
| M2 | FDN Reverb with Hadamard Mixing | 8-channel Hadamard butterfly, 24 add/sub per sample | ★★★★☆ |
| M3 | Transient Shaper (SPL method) | Dual envelope (fast/slow), gain = fast/slow | ★★★★☆ |
| M4 | Dynamic Multiband Waveshaping | Per-band adaptive tanh + 2x oversampling | ★★★☆☆ |
| M5 | Additive Synthesis via IFFT | Frequency-domain bin population + IFFT | ★★★★☆ |
| M6 | SIMD Split-Radix FFT | PFFFT-style conjugate-pair butterfly | ★★★☆☆ |

## VST Recreation (research SA-1)

| # | Plugin | Core Algorithm | SSE2 Feasibility |
|---|--------|---------------|------------------|
| V1 | Moog Minimoog (ladder filter) | Huovilainen 4-pole nonlinear | ✅ DONE |
| V2 | Roland Juno-106 | Analog modeling + chorus | ★★★★☆ |
| V3 | Yamaha DX7 (FM) | 6-operator FM (already have wb_fm.c) | ★★★★★ |
| V4 | Oberheim SEM | Multimode filter (ladder + state variable) | ★★★★☆ |
| V5 | TB-303 | Acid bass: ladder filter + slide/accent envelope | ★★★★☆ |
| V6 | TR-808/909 | Analog drum synthesis (sine + noise + decay) | ★★★★★ |
| V7 | SSL Bus Compressor | RMS detector + feed-forward VCA | ★★★★☆ |
| V8 | LA-2A Optical Compressor | T4 opto cell model (program-dependent) | ★★★☆☆ |
| V9 | 1176 FET Compressor | FET gain element + program-dependent attack | ★★★★☆ |
| V10 | Lexicon 224 Reverb | 8-channel FDN + early reflections | ★★★★☆ |
| V11 | EMT 140 Plate Reverb | Parallel comb + allpass diffuser | ★★★★☆ |
| V12 | Roland Space Echo | Tape delay + spring reverb + saturation | ★★★★☆ |
| V13 | Big Muff Fuzz | 3-stage cascaded clipping + tone stack | ★★★★☆ |
| V14 | Tube Screamer | Op-amp overdrive + tone control | ★★★★☆ |

## Full Stack Dominance (research SA-2)

| # | Feature | Layer | Feasibility |
|---|---------|-------|-------------|
| S1 | Comping (take lanes + playlists) | Arrangement | ★★★★☆ |
| S2 | VCA groups | Mixing | ★★★★☆ |
| S3 | Parallel processing (New York comp) | Mixing | ★★★★☆ |
| S4 | Surround panning (5.1/7.1) | Mixing | ★★★☆☆ |
| S5 | LFO shapes + parameter modulation | Automation | ★★★★★ |
| S6 | Smart quantize + groove templates | Workflow | ★★★★☆ |
| S7 | Chord detection | Workflow | ★★★★☆ |
| S8 | Stem export (multitrack bounce) | Export | ★★★★★ |
| S9 | AAF/OMF interchange | Export | ★★★☆☆ |
| S10 | Mastering chain (EQ→Comp→Limiter) | Export | ★★★★★ |
| S11 | Track freeze | Performance | ★★★★☆ |
| S12 | Bounce-in-place | Performance | ★★★★☆ |

## Next Build Targets (ranked)

1. **V6: TR-808/909 drum machine** — new instrument, high impact
2. **V13: Big Muff fuzz** — new effect, simple cascaded clipping
3. **M3: Transient shaper** — new dynamics processor
4. **S5: LFO shapes + parameter modulation** — automation backbone
5. **S10: Mastering chain** — export pipeline
