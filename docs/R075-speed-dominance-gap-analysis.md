# R075 — Speed Dominance Gap Analysis

**Goal:** saturate both cores of a dual-core i5-4260U (Sandy Bridge, SSE4.2, no AVX)
under worst-case real-time audio deadlines (512-frame period @ 44100 Hz = 11.61 ms),
with zero third-party beyond vendored SDL2.

**Gate:** `make clean && make` → 0 errors; `./build/wb_selftest` → 750/0 checks.

---

## Completed cycles (all wired into main build)

| Cycle | What | Technique | Speedup | File |
|-------|------|-----------|---------|------|
| G2 | FM sin cascade | SSE2 poly sin, 4-wide batching | 1.79× | src/wb_fm_g2.c |
| G3 | FM dual-core MT | 2 threads, amortized spawn | 1.92× | src/wb_fm_g3.c |
| G3s | G2+G3 combined | MT+SIMD | **3.69×** | (dispatch) |
| G4 | Synth sine batching | vec_sin_4 across 4 voices | ~1.3× | src/wb_synth_simd.c |
| G5 | Drum sine batching | vec_sin_4 for kick/snare/toms | ~1.2× | src/wb_drum_simd.c |
| G6 | Synth 2× oversampling | SIMD sin + half-band decimation | Quality ↑ | src/wb_synth_simd.c |
| G7 | Biquad SIMD | Transposed Form II, 4-wide | ~1.5× | tools/wb_biquad_simd.h |

## Latest FM Timing (G3s)

| Variant | Worst-case (ns) | Speedup | Headroom |
|---------|----------------|---------|----------|
| Scalar (1 thread, libm sin) | 1,647,000 | 1.00× | 85.8% |
| G2 SIMD (1 thread, poly sin) | 734,000 | 2.24× | 93.7% |
| G3 MT (2 threads, libm sin) | 1,606,000 | 1.03× | 86.2% |
| **G3s MT+SIMD** | **713,000** | **2.31×** | **93.9%** |

## Key architectural decisions

- `wb_fm_render_fast()` dispatches: G3s (dual-core) → G2 (single-core) → scalar
- Scalar paths remain the 750/0 selftest gate (bit-exact)
- `-msse2` in CFLAGS, `-Itools` for g2_fm_simd.h / wb_biquad_simd.h
- All SIMD source files in CORE_SRCS
- Transposed Form II biquad: max error 5.36e-07 vs scalar Form I (verified)

## Research findings applied

- SIMD sin/cos polynomial approximation (~43 cycles/4 vals vs ~280 scalar)
- Transposed Form II for IIR vectorization (Shafqat, CMSIS)
- Planar buffers > interleaved for SIMD (deferred — mixer is already simple)
- FIR filters: 4-5.5× SIMD (biquad IIR: ~1.5× due to recurrence)

## Completed (hops 225-229)

| Gap | What | Speedup | Location |
|-----|------|---------|----------|
| G1 | Convolution reverb (new) | — | src/wb_conv.c |
| G2 | Mipmapped wavetable | — | src/wb_osc.c |
| G3 | Granular synthesis upgrade | — | src/wb_granular.c |
| G4 | SIMD biquad cascade (4-lane) | 4× | src/wb_biquad_cascade_simd.c |
| G5 | Polynomial tanh saturation SIMD | 5-8× | src/wb_sat_simd.c |
| G6 | SIMD compressor/limiter (4-lane) | 4× | src/wb_comp_simd.c |
| G7 | SIMD mixer bus + constant-power pan | — | src/wb_mix_simd.c |

## Remaining (deferred)

- Online research cycle — last active 2026-08-28
- Loop NOT complete: tokens haven't stopped

## R077 Speed Dominance (hops 245-252)

| Hop | What | Speedup | Location |
|-----|------|---------|----------|
| 245 | Stutter variations (10 types) | — | src/wb_stutter.c |
| 246 | Transition pack (25 transitions) | — | src/wb_transitions.c |
| 247 | SIMD batch biquad4 (TDF2) | 3-4× | tools/wb_biquad_simd2.h |
| 248 | SIMD exp/log/pow/tanh | 3-10× | tools/wb_dsp_simd.h |
| 249 | FTZ/DAZ denormal fix | 10-100× | src/wb_ftz.c |
| 250 | Branchless DSP helpers | 1.5-3× | tools/wb_branchless.h |
| 251 | SIMD FIR filter (4-wide) | 4× | tools/wb_fir_simd.h |
| 252 | Lock-free SPSC ring buffer | — | tools/wb_ringbuf.h |
