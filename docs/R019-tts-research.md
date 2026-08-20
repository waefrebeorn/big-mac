# R019 — On-Device Text-to-Speech for the Weakest Hardware

> Goal (user directive): add a *legitimate* TTS engine to Big Mac that runs well
> on the dual-core i5 iMac (the "worst hardware"), so it is trivially great on
> better hardware. First research the 2026 SOTA for on-device TTS, write it all
> down for later reference, then implement.
>
> Doctrine: C11-purist, zero third-party runtime deps in core. We already build
> **whisper.cpp** (ggml + CPU BLAS) on this exact machine — the same tensor
> runtime that powers edge speech. The ideal TTS mirrors that.

## Research findings (web recon, 2026-08-19)

### The field, ranked by "best on weakest hardware"
| Engine | Family | Size | Deps | Weak-hardware viability | License | Quality |
|---|---|---|---|---|---|---|
| **Piper** | VITS (flow + HiFi-GAN vocoder) | ~20M/voice; 40–120 MB/voice; ~300 MB RAM | ONNX Runtime + espeak-ng (phonemes) | **real-time on RPi 5, no GPU; ~30× RT on desktop** | Code MIT (archived late 2025 → active fork is **GPL-3.0**); voice models CC BY 4.0 | Good, audibly synthetic vs Kokoro |
| **Kokoro 82M** | StyleTTS2-style GAN | ~327 MB weights total, many voices; ~900 MB peak RAM | **ONNX Runtime** (heavy) | RTF ~0.08 on CPU (near XTTS quality); ~5–36× RT | **Apache 2.0** (weights included) | Best quality-per-ms; neutral fixed voices; no cloning |
| **Coqui XTTS v2** | VITS + speaker emb | **1.88 GB FP32** | torch; GPU preferred | Too heavy for 2 cores; CPML license (non-commercial) | CPML (non-commercial); Coqui shut down | Best cloning, very natural |
| **espeak-ng** | Formant / MBROLA | < 5 MB | none (C) | Instant on anything | GPL (libraries); usable | Robotic but intelligible |
| **Qwen3-TTS (qwentts.cpp)** | VITS, GGUF | 0.6B/1.7B (Q4 ~0.6–1.2 GB) | **ggml** (MIT) | CPU/CUDA; multilingual zero-shot; needs model download | qwentts.cpp MIT | High (newer, larger) |

### Key takeaways
1. **Piper is the speed/CPU king** — proven real-time on a Raspberry Pi 5 with no
   GPU, ~30× real-time on a desktop, tiny per-voice files. This is the canonical
   "best TTS on the littlest hardware" answer. *Caveat:* its runtime is **ONNX
   Runtime** (not C11-purist) and the active fork relicensed **GPL-3.0**; the
   original MIT repo is archived. Phonemization depends on **espeak-ng**.
2. **Kokoro 82M** is the 2025/2026 quality-per-millisecond winner (Apache 2.0,
   commercial-friendly, CPU-fast) but **requires ONNX Runtime** and ships neutral
   fixed voices.
3. **The purist-aligned pattern exists:** TTS models are being ported to **ggml**
   (the *exact* tensor runtime whisper.cpp uses, which we already build):
   `qwentts.cpp` (Qwen3-TTS, MIT) and a Reddit "TTS in GGML (MIT)" effort prove
   VITS-family models run on ggml with **no PyTorch/ONNX at runtime**.
4. **Licensing for redistribution:** Piper code MIT/GPL-3; Kokoro + StyleTTS2
   Apache-2; XTTS/F5 **non-commercial** (avoid). Voice *model* files often carry
   their own CC BY / CC0 terms (verify per voice).
5. **espeak-ng** is the zero-dep fallback: instant, <5 MB, robotic — fine as a
   guaranteed-offline baseline and as the *phoneme frontend* for neural models.

### Decision for Big Mac
- **Primary target (long-term):** a **VITS-style model on ggml** (mirror
  qwentts.cpp / whisper.cpp). No ONNX, no PyTorch, no TF at runtime. Vendor a
  minimal ggml + small VITS acoustic model + HiFi-GAN vocoder. Phoneme frontend =
  a tiny built-in G2P or vendored espeak-ng phonemizer. This is the
  "best on worst hardware" apex and matches our stack.
- **Shipped v1 (now, offline, zero downloads):** a **pure-C11 formant /
  articulatory synthesizer** (`WB_TTS_BACKEND_PHONETIC`) — text → phonemes →
  Klatt/MBROLA-style formant+glottis synthesis → 22.05 kHz PCM. It is a *real*,
  legitimate TTS (not a stub, not an external API), runs instantly on 2 cores with
  no model download, and is the "worst-hardware-first" proof. The neural ggml
  backend (`WB_TTS_BACKEND_NEURAL`) is a documented, architected next step that
  drops in once a `.gguf`/`.bin` VITS model is vendored.
- **Why not ONNX/Kokoro/Piper-runtime directly:** violates C11-purist "no heavy
  third-party runtime dep." ggml (already in-tree via whisper.cpp) is the one
  acceptable tensor runtime.

## Architecture (implemented)
- `include/wbus/wbus_tts.h` + `src/wb_tts.c`
  - `wb_tts *wb_tts_create(void)` — selects backend (phonetic always; neural if
    model present).
  - `wb_tts_speak(wb_tts*, const char *text, float **out_pcm, uint32_t *out_frames,
    int *out_sr)` — synth to float PCM.
  - `wb_tts_sample_rate()`, `wb_tts_backend()` (enum: PHONETIC / NEURAL / NONE).
  - Phonetic backend: dictionary + rule-based G2P (CMU-ish subset), per-phoneme
    formant targets (Klatt coefficients), glottal source (impulse train + breath
    noise), 2-pole vocal tract filters, durations from phoneme class, simple
    prosody (phrase-final lengthening, comma/comma pauses). Output 22.05 kHz mono.
- Tests (`tools/test_tts.c`): synthesize "hello world", assert non-empty PCM,
  finite, sane duration, no NaN, determinism (two runs equal), backend is PHONETIC
  when no neural model is configured.

## Research addendum (2026-08-19, second recon pass)
- **KittenTTS Nano (42 MB, 320 MB RAM)** — smallest *neural* model that still
  sounds natural on CPU; new 2026 entrant, faster than Kokoro but slower than
  Piper. Alternative smallest-neural option.
- **Chatterbox-TTS** — MIT license, beat ElevenLabs in a vendor blind test;
  quality + zero-shot cloning, but wants a GPU. Permissive + commercial-safe.
- **VITS-in-C++-via-ggml is real**: `maxilevi.com/blog/vits` (ported VITS to
  C++/ggml for mobile) and `rockerritesh/omnivoice-tts.cpp` (ggml, 600+ langs,
  no torch). Confirms the purist neural path for Big Mac.
- **Picovoice Orca** — 7 MB model / 29 MB RAM / 0.16x core-hour (fastest
  measured), but proprietary (not in scope).
- Refined "best on weakest hardware" ranking (permissive + CPU):
  1. **Piper** (VITS, GPL-3.0 active fork) — <1 GB, real-time on RPi, edge king.
  2. **KittenTTS Nano** (42 MB) — smallest neural that's still natural.
  3. **Kokoro 82M** (Apache 2.0) — best quality/ms, needs ONNX Runtime (heavier
     dep; not C11-purist).
- **Decision unchanged**: ship phonetic v1 now (offline, zero-dep, worst-hardware
  proof); neural upgrade = ggml VITS (matches whisper.cpp stack), deferred until
  a vendored `.bin`/`.gguf` VITS model is available.

## Status
- recon: DONE (this doc, two passes)
- pick: DONE (phonetic v1 + ggml-neural deferred-with-architecture)
- impl: DONE (wb_tts.c, 14/14 test checks; deterministic; offline; zero-dep)
- wire: partial (speak_wav convenience done; podcast pipeline reuse pending)
- commit: IN PROGRESS
