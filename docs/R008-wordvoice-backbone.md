# R008 — WordVoice: The TTS Backbone Research (7-hop deep dive)

> Big Mac · 2026-08-11 · status: research → plan → build
> **Goal:** make WordVoice the fast TTS backbone of our inference engine —
> in OUR style (C11 SLERM, no third party) — giving us text-to-voice for
> the API AND voice-to-voice, best-in-class.

## What WordVoice is (the X post + model card + paper)

**WordVoice** (arXiv 2607.06461, Sihang Nie et al., 2026) is a speech
generation framework on top of **CosyVoice3** (FunAudioLLM) that turns
implicit end-to-end TTS into **explicit, decoupled word-level control**:

- **5 acoustic dimensions per word, fully controllable:**
  - ⏱️ **Duration** — word-level pronunciation length
  - ⏸️ **Boundary** — 5-level pause class (b0–b4)
  - 🔊 **Energy** — word-level loudness (0–1)
  - 🎵 **Pitch** — word-level core F0 (−1..1)
  - 📈 **Tone** — 7 prosodic morphologies (flat, rise, strong rise, fall,
    strong fall, peak, valley)
- **"Acoustic thinking" bound-token (`<b>`)**: before generating a word's
  speech tokens, the LLM explicitly predicts its acoustic attributes —
  *planning prosody first, then generating sound*. Adaptive multi-task
  prosodic planning + flexible manual intervention.
- **Dual mode**: auto-pilot (CosyVoice3-style) OR full manual word control.
- **Zero-shot cloning** from a ~10s prompt (CosyVoice3 lineage).

## The architecture (from the MLX port model card — exact specs)

```
Text ───────────┐
                ├─► LLM (Qwen2.5-0.5B) ─► Speech tokens (FSQ 6561)
Ref transcript ─┘
                                          │
Reference WAV ─► S3-Tokenizer-v3 ─► prompt_token ─┐
              ─► Matcha mel ──► prompt_feat ───────┼─► DiT Flow Matching ─► Mel
              ─► CAM++ ────► flow_embedding ───────┘   (bf16, 10 ODE steps) │
                                                                             ▼
                                                                        HiFi-GAN
                                                                   (NSF + F0 pred
                                                                    + ISTFT)
                                                                             │
                                                                             ▼
                                                                       Audio 24 kHz
```

| Component | Architecture | Size |
|---|---|---|
| LLM | Qwen2.5-0.5B: 24 layers, 896 hidden, 14Q/2KV heads (GQA), RoPE, SwiGLU, RMSNorm, tied embeddings | ~2 GB fp32 (int8: 640 MB) |
| Flow | DiT (diffusion transformer) flow matching: 22-layer, 1024 hidden, 16 heads, 10 ODE steps | ~1.3 GB (bf16: 634 MB) |
| Vocoder | HiFi-GAN with NSF (sine+noise source), F0 predictor, ISTFT | ~79 MB |
| Tokenizer | S3-Tokenizer-v3: 12-layer Conformer + FSMN + FSQ (242M params), 25 Hz token rate, FSQ codebook 6561 | 462 MB |
| Speaker emb | CAM++ (for flow_embedding) | — |

**Weights (WordVoice-base-0.5B):** `wordvoice_llm_en.pt` 2.0 GB,
`wordvoice_llm_zh.pt` 2.0 GB (bilingual!), `wordvoice_fm.pt` 1.3 GB.
Base model: FunAudioLLM/Fun-CosyVoice3-0.5B-2512. **16 finetunes exist.**

## The dataset (WordVoice-5A)

- **4.7k hours bilingual** (2.5k zh + 2.1k en, 52.26M words), re-annotated
  from LEMAS (150k-hour multilingual suite)
- **Linguistically-guided annotation pipeline** (the "how to make the
  data" recipe — directly reusable):
  1. **Alignment & clean**: dual-model MFA + Qwen3FA timestamps; loudness-
     based edge optimization (Valley Snapping / Local Refinement, 10%
     shift window, never crossing the syllable nucleus); consistency
     check discards non-overlapping/divergent edges → top 8% quality
  2. **Temporal**: duration + 5-level acoustic boundaries (b0–b4)
  3. **Acoustic/prosodic**: energy, pitch, 7-category tone via truncation
     + morphological modeling (Savitzky-Golay smoothing, ToBI-inspired)

## Variations / research points (for OUR SLERM)

1. **Quantization ports**: MLX 8-bit (LLM int8 g64, DiT bf16, vocoder fp32)
   and MLX 4-bit exist — proves the LLM is the fat part; int8 → 640 MB.
2. **vLLM-Omni integration** exists (CosyVoice3Model) — server path.
3. **Speed research in wuburvc knowledge/** (the RVC engine, same DSP
   family — our steals):
   - `CPU_FASTER_THAN_REALTIME.md`: AVX2 conv1d register micro-kernel
     **160×** (270 ms → 1.7 ms per conv), bit-exact parity; FTZ+DAZ CSR
     hygiene (denormals ~100× slower); nested-OMP MRF stacks
   - `ZERO_POINT_EIGHT_RT_PLAN.md`: FMA vs mul+add on Zen2 (1.69× in
     latency-bound chains — attention/linear/softmax, NOT memory-bound
     convs); Winograd verdict (skip); the "never getenv() in a hot loop"
     bug (29M env lookups = 30-300 s per conv!)
   - **KEY INSIGHT for us**: Big Mac is an Intel i5 WITHOUT AVX2 —
     the 160× conv kernel needs AVX2. Our machine has SSE4.2 only.
     The LLM (Qwen2.5-0.5B) is ~0.5 GFLOP/token forward — on one core
     that's the real budget question.
4. **What we already have (stolen from wuburvc, C11, verified)**:
   - `wubu_stft` + `wubu_fft` (STFT/FFT, OpenMP stripped for us)
   - `wubu_master` (EQ/comp/sat/limiter/loudness — the output polish)
   - `wubu_consonant` + `wubu_breath` (voicing/breath analysis)
   - `wubu_harmony` (dual-f0)
   - `wb_resample` (Kaiser sinc 16k↔44.1k/24k)
   - `wb_vc` (our own voice changer — the voice-to-voice path already
     works at 0.07× realtime with pure articulatory synthesis)
   - `wb_tts` (pure articulatory TTS, 39-phone table, 6 emotions)
5. **What we must SLERM (the new parts)**:
   - Qwen2.5-0.5B LLM in C11 (attention GQA, RoPE, SwiGLU, RMSNorm)
   - FSQ tokenizer (6561 codebook) + S3-Tokenizer-v3 (Conformer+FSMN)
   - DiT flow matching (22-layer, 10 ODE steps)
   - NSF HiFi-GAN vocoder (closest to wuburvc's generator — reuse!)
   - Matcha mel + CAM++ (speaker/flow conditioning)

## The strategy (our style: backwards, small, honest)

Big Mac is the **musical agent** on a 2012 dual-core i5 (no AVX2, 8 GB).
WordVoice full fp32 = 5.4 GB — too big for RAM + weights together with
useful headroom, and 0.5B-token autoregressive on one core is not
real-time. BUT:

1. **The API path (offline, batched)**: SLERM the pipeline in C11 —
   LLM int8 (640 MB) + DiT bf16 (634 MB) + vocoder (79 MB) ≈ **1.4 GB**
   → fits in 8 GB. This gives the ecosystem a **text-to-voice API** with
   word-level emotional control (the WordVoice killer feature) AND
   zero-shot voice cloning — on the same box as wuburvc's RVC.
2. **The real-time path stays articulatory**: `wb_vc` (0.07×) + `wb_tts`
   (pure physics, 6 emotions) remain the live/street-magic voice. The
   WordVoice backbone is the *offline studio-grade* engine; the throat is
   the *live* engine. Two-tier, like a real voice studio.
3. **Training the WordVoice-5A-style data**: the annotation pipeline
   recipe (MFA/Qwen3FA + loudness edge-optimization + consistency) is
   documented — we can build our own word-level annotated corpus from
   LibriSpeech (already downloaded!) for future fine-tuning.

## Verified facts for the build

- LLM: Qwen2.5-0.5B — 24 layers, 896 hidden, GQA 14Q/2KV, RoPE θ=1e6,
  SwiGLU, RMSNorm, tied embeddings. (Qwen2.5 model card)
- FSQ: 25 Hz token rate, codebook 6561 = 9^4 (per CosyVoice3 paper).
- Flow: DiT, 10 ODE steps (MLX port spec).
- Vocoder: NSF + F0 predictor + ISTFT, 24 kHz output.
- Tokenizer: S3-Tokenizer-v3, 12-layer Conformer + FSMN, 242M params.

## ⚠️ The honest hardware truth (measured on THIS machine)

Big Mac's iMac14,4 (2012, dual-core i5 1.4 GHz, DDR3 single-channel):

```
896^3 GEMM (double, -O2):  2.1 GFLOPS -> ~0.48 tokens/sec (0.5B model)
896^3 GEMM (float,  -O2):  2.1 GFLOPS -> ~0.48 tokens/sec  (compiler auto-vec,
                            memory-bound, not arithmetic-bound)
896^3 GEMM (int8,   -O2):  4.3 G-OPS  -> ~0.23 tokens/sec  (scalar int8 is
                            SLOWER than fp32 on this CPU)
```

**Verdict:** a 0.5B autoregressive LLM on this machine is ~0.5 tokens/sec
≈ **80-100 s per sentence**. The wall is DDR3 memory bandwidth
(~2-4 GB/s real), not FLOPs. WordVoice full-stack (5.4 GB weights) is
also too big to run alongside anything else in 8 GB.

**This is NOT a failure of the research — it's the split:**

| Engine | Machine | Role |
|---|---|---|
| **WordVoice SLERM (C11)** | ecosystem's strong boxes (AVX2, wuburvc's Ryzen etc.) | Studio-grade text-to-voice API + zero-shot cloning + word-level emotion — the "best engine" |
| **Big Mac articulatory** (`wb_vc`, `wb_tts`) | THIS iMac | Live/street-magic real-time voice (0.07× proven) — the "magic trick" |

The WordVoice SLERM core is built ONCE in C11 (portable, runs anywhere);
Big Mac hosts the API *client* + the live articulatory engine. The
vocoder, STFT, flow, mastering we already stole from wuburvc are exactly
the WordVoice-family DSP — the heavy LLM/DiT/FSQ parts get the same
SLERM treatment for the strong boxes.

## Status

- [x] Research (this doc, incl. honest hardware measurement)
- [ ] SLERM Qwen2.5-0.5B attention core (C11 — for strong boxes)
- [ ] SLERM FSQ tokenizer + S3-tokenizer path
- [ ] SLERM DiT flow matching (10 steps)
- [ ] Reuse NSF vocoder approach (wuburvc generator parity)
- [ ] `wb_wordvoice` CLI: text → wav (free + control modes)
- [ ] API: text-to-voice endpoint + voice-to-voice bridge
- [ ] Big Mac: API client + live articulatory remains the realtime path

## Sources
- X: @HuggingApps status/2080330151775072537 ("per word control... 0.5B good for its size")
- HF: XXH333/WordVoice-base-0.5B (paper 2607.06461, repo, space, demo)
- arXiv 2607.06461 (WordVoice paper: 5A dataset + bound-token + modulation)
- FunAudioLLM/Fun-CosyVoice3-0.5B-2512 (base model tree)
- aufklarer/CosyVoice3-0.5B-MLX-{4,8}bit (exact architecture specs)
- vLLM-Omni CosyVoice3 docs (pipeline stages)
- wuburvc knowledge/CPU_FASTER_THAN_REALTIME.md + ZERO_POINT_EIGHT_RT_PLAN.md
