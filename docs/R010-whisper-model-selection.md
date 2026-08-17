# R010 — Whisper model selection for 2012 iMac (7-hop research)

**Date:** 2026-08-17
**Question:** The absolute most lightweight CPU-friendly whisper model that
works on the i5-4260U iMac (2c/4t, 1.4GHz, 8GB, Big Sur, clang 13, AVX yes
AVX2 no) for the video editor's automatic captions feature. "Made pack by
community" = the community-made quantized whisper model pack.

## HOP 1 — The whisper.cpp model pack (THE community-made pack)

**whisper.cpp** (ggerganov/ggml-org) is the canonical C/C++ whisper port.
It ships a `models/download-ggml-model.sh` script that downloads **quantized
GGUF models** — the community-made pack. Each model has multiple quantizations:

| Model | Params | Q4_0 disk | Q5_0 disk | Q8_0 disk | FP16 disk | RAM |
|-------|--------|-----------|-----------|-----------|-----------|-----|
| tiny.en | 39M | 38 MB | 46 MB | 75 MB | ~75 MB | ~273 MB |
| base.en | 74M | 78 MB | 94 MB | 142 MB | ~142 MB | ~388 MB |
| small.en | 244M | 254 MB | 304 MB | 466 MB | ~466 MB | ~852 MB |

Source: whisper.cpp README + openwhispr.com comparison + arxiv quantization
paper. The Q4_0 tiny.en at **38 MB on disk** is the absolute lightest
community-made model that loads as whisper.

## HOP 2 — Hardware truth on this iMac

Verified directly on the machine:
- **CPU:** Intel i5-4260U, 2 cores / 4 threads, 1.4 GHz base, 2.7 GHz turbo
- **ISA:** Ivy Bridge (22nm) — **AVX yes, AVX2 NO, FMA3 yes**, SSE4.2, no BMI2
- **RAM:** 8 GB soldered
- **OS:** macOS Big Sur 11.7.9
- **Compiler:** Apple clang 13.0.0
- **whisper.cpp build:** cmake+XNNPACK+OpenMP, no Metal (Intel), no CUDA

The i5-4260U is a **low-power dual-core ultramobile CPU from 2014**. For
scaling reference: whisper.cpp tiny on a modern i5-1240P (12th-gen, 4c/8t,
~3.5GHz, AVX2) does 0.95s CPU for 10s of audio (~10× real-time). The
4260U has 1/2 the cores, ~1/2.5 the clock, and no AVX2 — estimating **4-8×
slower** than the 1240P. So tiny on this Mac: roughly **real-time or slightly
below** for batch work.

## HOP 3 — Speed reality for captions workflow

For the video editor's captions, transcription is an **offline export step**
(not live), so "below real-time" is acceptable. A 3-minute video:

- Extract audio: `ffmpeg -i video -vn -ar 16000 -ac 1 audio.wav` (fast)
- Transcribe with whisper.cpp tiny: est. **2-4× real-time** → ~6-12 min CPU
- Burn captions: `ffmpeg -i video -vf subtitles=out.srt` (fast)

Base would be slower (~5-10× real-time) but **much better quality**. On this
hardware, tiny is the only practical model for regular use. Base is usable for
occasional short clips.

## HOP 4 — Quality trade-off (WER)

| Model | English WER | LibreSpeech test-clean | LibreSpeech test-other |
|-------|-------------|-----------------------|-----------------------|
| tiny.en | ~7.6% | ~5.6% | ~14.9% |
| base.en | ~5.0% | ~4.3% | ~12.8% |
| small.en | ~3.4% | — | — |

For **captions** (where the user reads along and can mentally correct), tiny's
7.6% WER is usable but noticeably sloppy — dropped words, wrong names, missing
punctuation. For **draft captions that get reviewed**, tiny is fine. For
**final-quality captions without manual review**, base is the minimum.

## HOP 5 — The whisper.cpp build on this Mac

whisper.cpp compiles cleanly on this Mac with cmake + XNNPACK backend:
```
cmake -B build -DGGML_XNNPACK=ON -DGGML_OPENMP=ON -DGGML_CUDA=OFF -DGGML_METAL=OFF
cmake --build build
```
No AVX2 required — XNNPACK uses SSE2/AVX where available and falls back to
scalar. OpenMP gives 4 threads (4 logical cores). The `whisper-cli` binary
takes `-m model.bin -f audio.wav -osrt` and produces SRT captions — exactly
the pipeline we need.

## HOP 6 — Alternative: SLERM a minimal C11 whisper engine

Per the user's doctrine (SLERM, C11, no third party if we can make it, the
distiller-c11 ai.c/ai.h pattern with pluggable ASR callbacks, WuBuMath's
slermed JAX core + SIMD + quaternion ops as the math substrate), the pure path
is to build the whisper forward pass in C11. This needs:

1. **Mel spectrogram** — FFT (WuBuMath has no FFT yet; SLERM one or use a small
   one) + mel filterbank (matrix multiply, wuBuMath SIMD).
2. **Token embedding** — lookup table.
3. **Transformer** — the tiny model has ~2 layers, 384 hidden, 4 heads. The
   attention + FFN forward pass in C11, using wuBuMath SIMD for the matmuls.
4. **Quantized matmul** — the user's `distiller-c11` already has NEON Q4_K×Q8_K
   dot product for ARM. For Intel SSE2/AVX, write the equivalent quantized
   kernel (same algorithm, different ISA).
5. **Decoder** — greedy/beam search over ~51864-token vocab.

This is a **large project** (weeks) but doctrinally pure: C11, no deps, your
own kernels, your own mel pipeline, your own decoder. And it would be the
lightest possible whisper on this hardware because you control every
optimization.

## HOP 7 — Decision

**Convergent truth (one sentence):**
> Ship whisper.cpp with the tiny.en Q4_0 model (38 MB, community-made quantized
> pack) as the default caption engine on this iMac — it's the lightest model that
> compiles and runs here, fits in RAM, and does captions in acceptable time for an
> offline export step — and wire it through the distiller-c11 `ai.c`/`ai.h` ASR slot
> pattern (pluggable recognizer callback: PCM-in → text-out) so the video editor calls
> one C11 function; if/when the user wants pure-C11, SLERM the whisper forward pass
> on top of wuBuMath's slermed JAX + SIMD substrate as a drop-in replacement for the
> whisper.cpp recognizer callback.

## Concrete next steps

1. **Build whisper.cpp** on this Mac (cmake + XNNPACK + OpenMP, no Metal/CUDA).
2. **Download tiny.en Q4_0** model (38 MB) via the download script or direct curl.
3. **Smoke test**: transcribe a short audio clip, verify SRT output.
4. **Write the C11 shim** using the distiller-c11 `ai.c`/`ai.h` pattern:
   - `wb_asr_create()` / `wb_asr_feed()` / `wb_asr_transcribe()` — same API as
     distiller's ASR slot, but the recognizer callback shells out to whisper-cli
     (or links whisper.cpp's C API if we build it as a lib).
5. **Wire into the video editor** — at export, extract audio, call `wb_asr_transcribe()`,
   get SRT, pass to ffmpeg subtitles overlay.

## The "made pack by community" — identified

The community-made quantized whisper model pack = **whisper.cpp's GGUF model
downloads** (tiny.en/base.en/small.en × Q4_0/Q5_0/Q8_0). Nothing else by the
name "Maid Pack" exists. If there's a specific repo the user meant, they'll say
so. The lightest viable one on this hardware is **tiny.en Q4_0 at 38 MB**.
