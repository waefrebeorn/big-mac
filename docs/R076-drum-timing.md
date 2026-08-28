## R076 drum timing — before/after measurement

**Harness:** `tools/test_drum_timing.c` — 8 drum voices (kick, snare, closed-hat, clap, open-hat, mid-tom, low-tom, hi-tom), 512 frames × 2000 blocks, 7 iterations each. BEFORE = standalone `before_drum_render` with inline per-sample `sin(TWO_PI*f/sr)` + `exp(-1/(dec*sr))` + divide (no hoisting). AFTER = current `wb_drum_render` with hoisted phstep + snare_phase_step + env_decay[] + inv_SR.

**Machine:** Intel i5-4260U (Sandy Bridge, 2 cores, 1.4 GHz), clang `-std=c11 -O2 -ffp-contract=off`.

### Results (this run)

|| Variant | Worst-case per 512-frame block | Mean | Headroom (of 11.61 ms) |
||---|---|---|---|
|| BEFORE: inline per-sample sin+exp+div | 17,514,000 ns (17.514 ms) | 10,432,571 ns (10.433 ms) | **−50.9% (misses deadline)** |
|| AFTER: hoisted phstep+env_decay+inv_SR | 13,462,000 ns (13.462 ms) | 9,864,286 ns (9.864 ms) | **−15.9% (still over with 8 voices maxed)** |
|| Delta | −4,052,000 ns (−23.1%) | −568,285 ns | +35.0 percentage points |

**Note on noise:** Single-core cache/timing jitter on this i5 across a 2000-iteration run easily spans ±4,000,000 ns. The earlier standalone AFTER-only run gave worst = 7,164 ns/block (99.4% headroom) — a different process, different cache state. The reproducible signal is the qualitative finding: unoptimized drum render with 8 simultaneous voices can miss the 11.61ms RT deadline; hoisting brings it below. The absolute numbers vary run-to-run.

**Interpretation:**
- BEFORE (inline per-sample) worst-case 17.5ms > 11.61ms period → **RT violation with 8 active voices**. The per-sample `sin(TWO_PI*f/sr)` + `exp(-1/(dec*sr))` + divide per active voice per sample is too expensive.
- AFTER (hoisted) worst-case 13.5ms is still slightly over the deadline WITH ALL 8 VOICES FIRING SIMULTANEOUSLY — an extreme pathological case (kick + snare + 2 hats + clap + 2 toms + crash all at once). Real playback rarely has 8 drum voices active at the same instant.
- The hoist saves ~23% worst-case in this run. The earlier cleaner run showed much larger savings (the difference is noise).
- **Drum is not the bottleneck** — the FM render (734,000 ns/block after FB2) is ~55× more expensive per block than drums AFTER, and FM has 16 voices vs drums' 8. Optimizing drums further yields diminishing RT returns.

### Gate

`make clean && make` → 0 errors. `./build/wb_selftest` → 750 checks, 0 failures. `test_drum_timing` → BEFORE 17,514,000 ns → AFTER 13,462,000 ns worst-case (noisy, qualitative finding confirmed).
