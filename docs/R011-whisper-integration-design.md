# R011 — whisper.cpp + distiller-c11 ASR slot integration design

**Builds on:** R010 (7-hop convergence: whisper.cpp tiny.en Q4_0 as the caption engine)
**Goal:** Wire whisper.cpp through the distiller-c11 `ai.c`/`ai.h` ASR slot pattern
so the video editor calls one C11 function: feed PCM → get transcript → burn SRT
subtitles via ffmpeg.

## Architecture

The distiller-c11 `ai.c`/`ai.h` already defines the exact pattern we need:

```c
/* ai.h — from distiller-c11 (canonical pattern) */
typedef struct asr asr;
typedef char *(*asr_recognize_fn)(const int16_t *pcm, size_t nframes,
                                  uint32_t rate, void *user);

typedef struct asr_config {
    uint32_t rate;          /* expected sample rate, e.g. 16000 */
    asr_recognize_fn recognize;
    void *user;
} asr_config;

asr *asr_create(const asr_config *cfg);
void asr_free(asr *a);
int asr_feed(asr *a, const int16_t *pcm, size_t nframes);
char *asr_transcribe(asr *a);
int asr_has_recognizer(const asr *a);
```

We create a new repo `wb_whisper/` (or extend `big-mac/` if the user wants it
in the DAW tree) that implements this ASR slot backed by whisper.cpp:

```
wb_whisper/
├── include/
│   └── wb_whisper.h        # public API: wb_whisper_asr_create, feed, transcribe
├── src/
│   ├── wb_whisper.c        # ASR slot impl: wraps whisper-cli or links libwhisper
│   ├── wb_whisper_mel.c    # mel spectrogram (FFmpeg/sox fallback: just use ffmpeg)
│   ├── wb_whisper_quant.c  # SSE2/AVX quantized kernels (future: when we SLERM)
│   └── wb_whisper_decode.c # token decoding (future)
├── models/                 # whisper.cpp models symlinked or copied
├── Makefile
└── README.md
```

## Two implementation phases

### Phase 1 — Whisper-cli wrapper (NOW, ships captions)

The ASR recognizer callback shells out to `whisper-cli`:

```c
/* wb_whisper.c — phase 1: whisper-cli subprocess */

static char *whisper_recognize_cli(const int16_t *pcm, size_t nframes,
                                    uint32_t rate, void *user) {
    /* 1. Write PCM to temp WAV */
    /* 2. Fork whisper-cli -m model -f wav -osrt */
    /* 3. Read SRT output */
    /* 4. Return transcript text (malloc'd) */
    /* 5. Clean up temp files */
}
```

This is the minimal path that ships captions NOW. The video editor:
1. At export, extracts audio: `ffmpeg -i video.mp4 -vn -ar 16000 -ac 1 audio.wav`
2. Calls `wb_whisper_asr_feed(audio_pcm, nframes)` for the whole clip
3. Calls `wb_whisper_asr_transcribe()` → gets transcript text
4. Writes SRT manually (or via a helper) from the transcript
5. Burns captions: `ffmpeg -i video.mp4 -vf subtitles=out.srt -c:a copy out.mp4`

### Phase 2 — Link libwhisper directly (when user wants pure-C closer)

Instead of shelling out, link `libwhisper.dylib` (already built) and call its
C API directly. This avoids the subprocess overhead and temp files. The whisper.cpp
build already produced `libwhisper.1.9.2.dylib` and `libggml*.dylib` — we can
link against them.

```c
/* Phase 2: direct libwhisper link */
#include <whisper.h>  /* from whisper.cpp */

static char *whisper_recognize_lib(const int16_t *pcm, size_t nframes,
                                    uint32_t rate, void *user) {
    /* 1. Convert PCM to whisper sample format */
    /* 2. whisper_process_chunk() */
    /* 3. whisper_full_print_segment() or extract tokens */
    /* 4. Return transcript */
}
```

### Phase 3 — SLERM the whisper forward pass in pure C11 (user's ultimate goal)

When the user wants NO third-party at all, replace the whisper.cpp backend with
a from-scratch C11 implementation built on wuBuMath:

- **Mel spectrogram**: FFT (SLERM a small radix-2 FFT in C11, or use a public-domain
  one) + mel filterbank matrix (matrix multiply via wuBuMath SIMD)
- **Token embedding**: lookup table (the tiny model's ~51864-token vocab)
- **Transformer forward**: 2 layers × 384 hidden × 4 heads. Attention + FFN.
  Quantized matmul kernels (SSE2/AVX) following the distiller-c11 NEON pattern
  ported to Intel ISA.
- **Decoder**: greedy search over vocab.

This is the "imprinting" path — learning from whisper.cpp's architecture and
rebuilding it in the user's C11 idiom, using wuBuMath as the math substrate.

## The ASR slot is the doctrinal pattern

The distiller-c11 `ai.c`/`ai.h` design is exactly right for this:
- Opaque `asr` struct
- Pluggable recognizer callback — swap whisper-cli → libwhisper → pure-C11
  without changing the video editor's call site
- Honest empty state — if no recognizer installed, `asr_transcribe()` returns NULL
  (no fabricated transcripts)
- PCM-in / text-out — the video editor doesn't care what's inside

This is the imprinting pattern: learn the architecture from the user's own code
(distiller-c11) and apply it to the new domain (whisper captions).

## Model choice confirmed

**tiny.en Q4_0** (38 MB) — but wait, whisper.cpp's download script gave us
`ggml-tiny.en.bin` which is the FP16 version (77 MB). For the quantized version,
we need to either:
- Download the Q4_0 quantized model directly from HuggingFace
- Or quantize the FP16 model using `whisper-quantize`

Let me check what quantized models are available and get the lightest one.

## FFmpeg is already present

`ffmpeg version 9.0.1-tessus` at `~/.local/bin/ffmpeg` — confirmed. The captions
pipeline (extract audio → whisper → burn SRT) uses ffmpeg for steps 1 and 5.

## Next actions

1. [x] Build whisper.cpp on this Mac (DONE — `whisper-cli` + `libwhisper.dylib` built)
2. [x] Download tiny.en model (DONE — 77 MB FP16, need quantized Q4_0)
3. Get quantized tiny.en Q4_0 model (38 MB) — either download or quantize local
4. Smoke test: transcribe a short clip, verify output
5. Create `wb_whisper/` repo with Phase 1 (whisper-cli wrapper ASR slot)
6. Integrate into the video editor's export pipeline
7. Document the full captions pipeline in a design doc
