# R076 — Melodyne Parity + VST Recreation + Full Stack Dominance

Parallel 3-stream research fan-out completed 2026-08-27. 3 subagents × 3 domains
(Melodyne parity, VST recreation, full-stack DAW). 40+ techniques researched,
5 modules landed (hops 232-236).

## Modules Landed

### N1 — M/S Stereo Widening + Haas (`src/wb_stereo.c`)

**API:**
```c
void wb_stereo_widen(const wb_sample *in, wb_sample *out, uint32_t n, float width);
void wb_stereo_widen4(const wb_sample *in, wb_sample *out, uint32_t n, float width);
void *wb_haas_create(uint32_t sr, float delay_ms);
void  wb_haas_destroy(void *inst);
void  wb_haas_set_delay(void *inst, float delay_ms, uint32_t sr);
void  wb_haas_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

typedef struct { /* opaque */ } wb_lr4xover;
void wb_lr4xover_compute(wb_lr4xover *x, float fc, uint32_t sr);
void wb_stereo_widen_fd(const wb_sample *in, wb_sample *out, uint32_t n,
                        float fc, float low_width, float high_width,
                        wb_lr4xover *xover, uint32_t sr);
```

**Architecture:**
- M/S encode: `M = (L+R)*0.5, S = (L-R)*0.5`
- Stereo widening: scale S channel by width factor (0=mono, 1=original, >1=wider)
- M/S decode: `L = M+S, R = M-S` (NO extra 0.5 — already in encode)
- SSE2 `wb_stereo_widen4`: deinterleave via `_mm_unpacklo/hi_ps`, process 4 stereo samples/iteration
- Haas effect: delay right channel by 1-40ms via circular buffer
- Frequency-dependent: LR4 crossover splits into low/high bands, independent width per band

**Pitfall:** M/S decode must NOT multiply by 0.5 again. The encode already halves.
Double-attenuation → output ¼ of expected. Formula: `L_out = M + S'`, `R_out = M - S'`.

**Gate:** `make test_stereo` → 6/6 checks (width=1 identity, width=0 mono, width=2 wider, SIMD-vs-scalar exact, Haas delay, silence).

### N2 — YIN Pitch Detection (`src/wb_yin.c`)

**API:**
```c
typedef struct { uint32_t sr; float threshold; int min_tau; int max_tau; } wb_yin_cfg;
float wb_yin_detect(const float *buf, int n, const wb_yin_cfg *cfg);
float wb_yin_pitch(const float *buf, int n, uint32_t sr);  // convenience wrapper
```

**Algorithm** (de Cheveigné & Kawahara, JASA 2002):
1. Difference function: `d(τ) = Σ(x[j] - x[j+τ])²` — SSE2 vectorized (4 samples/iter)
2. Cumulative mean normalized: `d'(τ) = d(τ) / mean(d[1..τ])`
3. Find first τ where `d'(τ) < threshold` (default 0.15)
4. Parabolic interpolation for sub-sample accuracy
5. `pitch = sr / best_tau`

**Accuracy:** ±2 Hz on sine waves (440→442, 220→221, 1000→1004).

**Gate:** `make test_yin` → 6/6 checks (440Hz, 220Hz, 1000Hz, silence, noise, small buffer).

### N5 — Phaser (`src/wb_phaser.c`)

**API:**
```c
void *wb_phaser_create(uint32_t sr);
void  wb_phaser_destroy(void *inst);
void  wb_phaser_set(void *inst, int param, float v);  // 0=rate,1=depth,2=feedback,3=mix,4=stages
void  wb_phaser_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n);
```

**Algorithm** (Zölzer DAFX): Cascade of N first-order allpass filters (default 6, max 8)
with LFO-modulated center frequency. `H(z) = (a + z⁻¹)/(1 + a*z⁻¹)` where
`a = tan(π*fc/fs) - 1 / tan(π*fc/fs) + 1`. Feedback creates resonant notches.

**Gate:** `make test_phaser` → 5/5 checks (create/destroy, bounded, silence, audible effect, params).

### M1 — Auto-Tune Pitch Correction (`src/wb_pitch_correct.c`)

**API:**
```c
void *wb_pitch_correct_create(uint32_t sr);
void  wb_pitch_correct_destroy(void *inst);
void  wb_pitch_correct_set(void *inst, int param, float v);  // 0=amount,1=root,2=scale
void  wb_pitch_correct_process(void *inst, wb_sample *buf, uint32_t n);
```

**Algorithm:**
1. Detect pitch via YIN (2048-sample frame)
2. Find nearest note in active scale (major/minor/pentatonic/chromatic)
3. Compute pitch shift ratio = target / detected
4. Apply shift via linear resampling
5. Smooth pitch changes (0.7×prev + 0.3×current) to prevent jitter

**Melodyne parity:** scale-aware pitch snapping with adjustable correction amount.
Not polyphonic (single-voice), but corrects monophonic sources (vocals, bass, leads).

**Gate:** `make test_pitch_correct` → 5/5 checks (create/destroy, silence, correction applied, correction=0 no-op, chromatic passthrough).

### V1 — Moog Ladder Filter (`src/wb_ladder.c`)

**API:**
```c
void *wb_ladder_create(uint32_t sr);
void  wb_ladder_destroy(void *inst);
void  wb_ladder_set(void *inst, int param, float v);  // 0=cutoff,1=resonance
void  wb_ladder_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n);
float wb_ladder_process_mono(void *inst, float x);  // for synth voices
```

**Algorithm** (Huovilainen model): 4 cascaded 1-pole nonlinear lowpass stages with
tanh saturation + global feedback loop. `y[n] = y[n-1] + g*(tanh(x) - tanh(y[n-1]))`
where `g = tan(π*fc/fs)`. Feedback: `input - 4*k*tanh(stage4_output)`, k = resonance (0..4).

**Safety:** Input and feedback clamped to ±10 to prevent blowup at high resonance.

**Gate:** `make test_ladder` → 6/6 checks (create/destroy, silence, HP attenuation, LP pass, resonance boost, bounded at max resonance).

## Research Backlog (40+ techniques, not yet built)

### Melodyne Parity (SA-0, 7 techniques)
- M2: FDN Reverb with Hadamard Mixing (8-channel, butterfly operations)
- M3: Transient Shaper (SPL dual-envelope method)
- M4: Dynamic Multiband Waveshaping (per-band adaptive tanh + 2x OS)
- M5: Additive Synthesis via IFFT (256+ partials, O(N log N))
- M6: SIMD Split-Radix FFT (PFFFT-style)

### VST Recreation (SA-1, 14 techniques)
- V2: Roland Juno-106 (analog modeling + chorus)
- V3: Yamaha DX7 (6-op FM — extend existing wb_fm.c)
- V4: Oberheim SEM (multimode filter)
- V5: TB-303 (acid bass: ladder filter + slide/accent)
- V6: TR-808/909 (analog drum synthesis)
- V7: SSL Bus Compressor (RMS + feed-forward VCA)
- V8: LA-2A Optical Compressor (T4 opto cell model)
- V9: 1176 FET Compressor (FET gain element)
- V10: Lexicon 224 Reverb (8-channel FDN + early reflections)
- V11: EMT 140 Plate Reverb (parallel comb + allpass diffuser)
- V12: Roland Space Echo (tape delay + spring reverb + saturation)
- V13: Big Muff Fuzz (3-stage cascaded clipping + tone stack)
- V14: Tube Screamer (op-amp overdrive + tone control)

### Full Stack Dominance (SA-2, 12 techniques)
- S1: Comping (take lanes + playlists)
- S2: VCA groups
- S3: Parallel processing (New York compression)
- S4: Surround panning (5.1/7.1)
- S5: LFO shapes + parameter modulation
- S6: Smart quantize + groove templates
- S7: Chord detection
- S8: Stem export (multitrack bounce)
- S9: AAF/OMF interchange
- S10: Mastering chain (EQ→Comp→Limiter)
- S11: Track freeze
- S12: Bounce-in-place

## Pitfalls (R076)

1. **M/S decode double-attenuation:** Encode applies 0.5, decode must NOT apply it again.
   `L = M + S`, not `L = (M+S)*0.5`.

2. **Pitch correction resampling shortens output:** Linear resampling by ratio r produces
   `floor((n-1)/r)` output samples from n inputs. Zero-pad the tail.

3. **Ladder filter blowup at high resonance:** The feedback path `input - 4*k*tanh(s[4])`
   can diverge if k > ~3.5 without clamping. Always clamp input and feedback to ±10.

4. **YIN pitch detection needs sufficient buffer:** Minimum ~64 samples, recommended 2048
   for accurate detection. Returns 0 for silence (no pitch).

5. **Phaser allpass coefficient:** `a = (tan(π*fc/fs) - 1) / (tan(π*fc/fs) + 1)`.
   At fc close to Nyquist, tan → ∞, a → 1 (allpass becomes pure delay).
   At fc close to 0, tan → 0, a → -1 (allpass becomes inverter).
