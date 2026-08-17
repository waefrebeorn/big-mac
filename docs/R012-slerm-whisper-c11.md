# R012 — SLERM whisper forward pass in pure C11 (Phase 3 roadmap)

**Builds on:** R010 (model selection), R011 (ASR slot integration design),
           R011 Phase 3 goal (SLERM whisper forward pass)
**Goal:** Write a pure C11 whisper forward pass that replaces the whisper-cli
        subprocess — owned, zero third-party, your own SIMD substrate.

## Why SLERM this

You said "push and slerm" — two operations:

1. **Push** the Phase 1 whisper_engine repo to GitHub (done by background job)
2. **SLERM** the whisper forward pass in pure C11 (this document)

Phase 1 (whisper-cli subprocess) ships captions NOW but uses a third party.
Phase 2 (link libwhisper.dylib) eliminates the subprocess but still uses
whisper.cpp as a library. Phase 3 is the endpoint: **own the whole stack**.
No whisper.cpp. No ggml. No third party. Just your C11 code doing the same
forward pass, on your own SIMD substrate (WuBuMath), producing the same
transcript.

## The whisper.cpp architecture (what we SLERM)

whisper.cpp is a full ggml compute-graph engine with CUDA/Metal backends.
For a C11 SLERM targeting tiny.en on a 2c/4t i5-4260U, we strip all of that
away and keep only the forward-pass math.

### Tiny.en model parameters (what we need to implement)

| Component | Dimensions | Notes |
|---|---|---|
| Audio: 16kHz mono PCM | float32 samples | input |
| Mel spectrogram | 80 mel bins × variable frames | STFT + mel filterbank + log |
| Encoder conv1 | 384×80 conv1d (padding 1) | 30720 weights + 384 bias |
| Encoder conv2 | 384×384 conv1d (padding 1) | 147456 weights + 384 bias |
| Encoder LN post | 384×384 (weight+bias) | layernorm after conv2 |
| Encoder positional embedding | 1500×384 | added to conv output |
| Encoder MHA (×4 blocks) | Q/K/V: 384×384 each, out: 384×384 | 4×4×384×384 = 2.36M weights |
| Encoder MLP (×4 blocks) | FC: 384×1536, GeLU, FC: 1536×384 | 4×2×384×1536 = 4.72M weights |
| Decoder positional embedding | 448×384 | added to decoder input |
| Decoder token embedding | 51864×384 | 19.9M weights (the big one) |
| Decoder LN | 384×384 | layernorm before blocks |
| Decoder MHA self-attn (×4) | Q/K/V: 384×384, out: 384×384 | causal mask |
| Decoder cross-attn (×4) | Q: 384×384, K/V: 512×384, out: 384×384 | queries encoder output |
| Decoder MLP (×4) | FC: 384×1536, GeLU, FC: 1536×384 | same shape as encoder MLP |
| Logits head | 384×51864 | = decoder token embedding transposed |

**Total weights (tiny.en, ftype=1 FP16):** ~39M parameters × 2 bytes = ~78 MB
(without tokenizer vocab overhead; the ggml model file with Q5_1 quantization
is 31 MB).

### The three SLERM subsystems

#### A. Mel spectrogram (wb_whisper_mel.c)

Input: 16kHz mono float32 PCM
Output: 80-band log mel spectrogram, [n_mel_frames × 80]

Steps:
1. **STFT** — FFT of 400-sample windows, hop 160. We need an FFT.
   Your WuBuMath has positional encoding, quaternion ops, but does it have FFT?
   If not, we write a radix-2 FFT in C11 (or use the real FFT from your
   wubu_math.c-style libm approach). The STFT produces complex spectrum.
2. **Magnitude** — |X| = sqrt(re² + im²) per frequency bin, 201 bins (n_fft/2+1).
3. **Mel filterbank** — 80 triangular filters on the 201 frequency bins,
   following the mel scale: mel(f) = 2595 × log10(1 + f/700).
   Precomputed from the model file (whisper_filters data, 80×201 floats).
4. **Log** — log(mean(magnitude²) + 1e-10) per mel band.

This is the most expensive part computationally — STFT on every frame.
But we can reuse WuBuMath's float ops.

#### B. Encoder forward pass (wb_whisper_enc.c)

Input: mel spectrogram [n_frames × 80]
Output: encoder embedding [n_frames × 384]

Steps:
1. **Conv1d (padding=1)** — mel @ conv1_w^T + conv1_b → [n_frames × 384]
   Actually: conv1d with kernel_size=3, padding=1 on the 80-dim input.
   This is a 1D convolution: for each frame, convolve 80→384 across 3 adjacent
   frames. Implementation: im2col + matmul, or direct sliding window.
2. **GeLU** — elementwise: x × 0.5 × (1 + tanh(sqrt(2/π) × (x + 0.044715×x³)))
   or the simpler approximation: 0.5x(1 + tanh(x × 0.797885))
3. **Conv1d (padding=1)** — same shape, conv2_w, conv2_b
4. **LayerNorm** — (x - mean) / sqrt(var + eps) × weight + bias
5. **Add positional embedding** — encoder_output + pe[:n_frames]
6. **N× transformer blocks** (4 for tiny):
   For each block:
   a. **Self-attention:**
      - LN(input) → Q = input @ q_w^T + q_b [n×384]
      - LN(input) → K = input @ k_w^T [n×384]
      - LN(input) → V = input @ v_w^T [n×384]
      - Scale: Q @ K^T / sqrt(384/6) = Q @ K^T / sqrt(64)
      - Softmax (row-wise)
      - Attention: softmax @ V → [n×384]
      - Output: attn @ out_w^T + out_b → [n×384]
      - Residual: input + attn_out
   b. **MLP:**
      - LN(residual) → FC: residual @ mlp_0_w^T + mlp_0_b → [n×1536]
      - GeLU
      - FC: geLU_out @ mlp_2_w^T + mlp_2_b → [n×384]
      - Residual: residual + mlp_out

For the i5-4260U, the encoder matmuls are: ~220 frames × 384 dim = 84,480
elements per matmul. With 4 blocks × (4 attention matmuls + 2 MLP matmuls) =
24 matmuls of this size, plus layer norms and activations. This is very
tractable on your CPU with WuBuMath SIMD.

#### C. Decoder forward pass (wb_whisper_dec.c)

Input: encoder embedding [n_frames × 384], previous tokens [n_prev × 1]
Output: next token logits [1 × 51864]

The decoder is autoregressive — one token at a time. This is the slowest part
because it runs N times (one per output token).

Steps (per decoder step):
1. **Token embedding lookup** — embed token_id → [1 × 384] from d_te
2. **Positional embedding** — pe[position] → [1 × 384]
3. **Sum** — token_emb + pos_emb → decoder_input [1 × 384]
4. **N× decoder blocks** (4 for tiny):
   For each block:
   a. **Self-attention (causal):**
      - LN(decoder_input) → Q/K/V projections
      - Causal mask: positions ≥ current masked to -∞ before softmax
      - Same scaled dot-product attention as encoder
      - Output + residual
   b. **Cross-attention:**
      - LN(residual) → Q = residual @ cross_q_w^T
      - K = encoder_output @ cross_k_w^T [n_frames × 384]
      - V = encoder_output @ cross_v_w^T [n_frames × 384]
      - Scaled dot-product: Q @ K^T / sqrt(64) → [1 × n_frames]
      - Softmax → attention @ V → [1 × 384]
      - Output + residual
   c. **MLP:**
      - Same as encoder MLP (LN → FC → GeLU → FC → residual)
5. **Final LN** on decoder output
6. **Logits** — decoder_output @ d_te^T → [1 × 51864]
   (This is the token embedding transposed — weight sharing between embedding
   and logits head.)
7. **Softmax** over logits → probabilities
8. **Sample** — greedy (argmax) or beam search

#### D. Token sampling + text (wb_whisper_decode.c)

Input: logits [1 × 51864]
Output: text string

Steps:
1. **Greedy decoding** (simplest): argmax(logits) → next_token_id
2. **Special token handling:**
   - SOT (start of transcript) = 50257
   - EOT (end of transcript) = 50256 → stop
   - No-timestamps = 50362 → control behavior
3. **Token → text** — look up id_to_token map, decode UTF-8 bytes
4. **Concatenate** tokens into output string

The vocabulary is 51864 tokens for tiny.en. The token→text mapping is stored
in the model file (read during load). For the SLERM, we need to parse this from
the ggml model file format (plain text tokens with length prefix).

## The ggml dependency problem

whisper.cpp uses ggml for ALL tensor operations and compute graph scheduling.
For our C11 SLERM, we eliminate ggml entirely. Instead:

- **Tensors** = flat float32 arrays with shape metadata (our own struct)
- **Matmul** = WuBuMath SIMD dot product (SSE2/SSE4.1 on Intel, no AVX2 needed)
- **LayerNorm** = our own elementwise with mean/var reduction
- **Softmax** = our own with max-subtraction for numerical stability
- **GeLU** = our own approximation
- **FFT** = our own radix-2 (or we find one in WuBuMath)

The model weights are loaded from the ggml file format directly — we parse
the binary format (magic, hparams, mel filters, vocab, then weight tensors in
order). The Q5_1 quantized weights would need dequantization on load, OR we
load the FP16 version (ggml-base.en.bin, 142 MB — bigger but simpler to parse).

For the SLERM prototype, I recommend **loading the FP16 model** (not quantized)
to avoid the dequantization complexity. The FP16 model is 142 MB — fits in RAM
on 8 GB. Once the FP16 forward pass works, we add Q5_1 dequantization as an
optimization.

## Memory budget (FP16 tiny.en, n_audio_ctx=1500, n_text_ctx=448)

| Component | Size | Notes |
|---|---|---|
| Mel spectrogram | 1500 × 80 × 4 = 480 KB | float32 |
| Encoder output | 1500 × 384 × 4 = 2.3 MB | float32 |
| Encoder weights (FP16) | ~3 MB | conv+ln+pe+4×(MHA+MLP) |
| Decoder weights (FP16) | ~23 MB | token emb dominates (19.9M × 2B) |
| Decoder KV cache | 448 × 4 × 2 × 384 × 4 = 1.3 MB | K+V per layer per position |
| Working buffers | ~10 MB | matmul intermediates, softmax, etc. |
| **Total** | ~40 MB | fits easily in 8 GB |

## The SLERM approach

This is a from-scratch C11 implementation. The architecture:

```
wb_whisper_engine/
├── wb_whisper.h           # public API (same ASR slot shape as Phase 1)
├── wb_whisper_model.c     # load ggml FP16 model file, parse weights
├── wb_whisper_mel.c       # STFT → mel spectrogram (own FFT, own mel filterbank)
├── wb_whisper_enc.c       # encoder forward pass (conv1d, GeLU, LN, MHA, MLP)
├── wb_whisper_dec.c       # decoder forward pass (causal+cross attn, MLP, logits)
├── wb_whisper_sample.c    # greedy/beam sampling, token→text
├── wb_whisper_math.c      # matmul, softmax, layer norm, GeLU, FFT (WuBuMath-style)
├── wb_whisper_test.c      # test: load model, feed PCM, transcribe
├── Makefile
└── README.md
```

### Phase 3a: Mel spectrogram + encoder only (validate against whisper-cli)

The encoder produces a 384-dim embedding per frame. We can't easily validate
the encoder output against whisper.cpp's internal state (it's hidden inside
the ggml graph). Instead, we validate the **full pipeline**: feed the same PCM,
run our encoder, run our decoder, and compare the final transcript text to
whisper-cli's output. If the transcripts match, the whole stack is correct.

### Phase 3b: Decoder + sampling

Once the encoder is validated (transcript matches), add the decoder. The
decoder is autoregressive, so it's harder to debug. Strategy: use whisper-cli
as oracle, compare token-by-token.

### Phase 3c: Optimization

Once the FP16 forward pass works, optimize:
- Q5_1 dequantization (load quantized weights, dequantize on the fly)
- WuBuMath SIMD matmul (SSE2/SSE4.1 kernels)
- Multi-threaded encoder (frame-parallel, since encoder blocks are independent
  across frames once the conv is done)

## What we SLERM vs what we keep from whisper.cpp

| whisper.cpp component | SLERM decision |
|---|---|
| ggml tensor/context/graph API | **Nuke** — our own flat tensors |
| ggml backend/CUDA/Metal | **Nuke** — CPU only, our own SIMD |
| ggml-quantized weight loading | **Replace** — load FP16, our own parser |
| mel filterbank data | **Keep** — parse from ggml file |
| vocab/tokenizer | **Keep** — parse from ggml file |
| encoder/decoder weight tensors | **Keep** — parse from ggml file |
| encoder/decoder forward pass | **SLERM** — our own matmuls, attn, LN, GeLU |
| STFT/mel spectrogram | **SLERM** — our own FFT + mel filterbank |
| greedy/beam sampling | **SLERM** — our own softmax + argmax/beam |
| KV cache | **SLERM** — our own circular buffer |
| whisper_full() orchestration | **SLERM** — our own ASR slot (same API shape) |

## Test strategy

1. Load tiny.en FP16 model from ggml file (parse binary format)
2. Feed same JFK PCM (176000 samples @ 16kHz) to our engine
3. Run mel → encoder → decoder → greedy sampling
4. Compare transcript to whisper-cli output:
   Expected: "And so my fellow Americans ask not what your country can do for you. Ask what you can do for your country."
5. If match → Phase 3 complete. If not → debug encoder/decoder/adapter.

## Timeline estimate

This is a substantial SLERM. The math itself isn't hard (transformer encoder/
decoder is standard architecture), but getting the weight loading, mel spec,
and decoder sampling right so the transcript matches takes iteration.

- **Mel spectrogram:** 3-5 days (FFT + mel filterbank + log, validate against
  whisper-cli's mel output if possible)
- **Encoder:** 5-10 days (conv1d + 4 transformer blocks, validate transcript)
- **Decoder:** 5-10 days (autoregressive + cross-attn + sampling, validate)
- **Optimization (Q5_1, SIMD):** 5-10 days
- **Total:** 3-6 weeks of focused work

But you said "DO IT ALL" and "TIME IRRELEVANT" — so we do it properly.

## File: design doc for the C11 whisper engine

This is R012. The implementation files go in a new repo or under
big-mac/ as wb_whisper_engine/. The first implementation file is
wb_whisper_model.c — parsing the ggml FP16 model file format.

## Next step

Create the SLERM repo structure and start with the model file parser —
we need to load the FP16 weights from the ggml file before we can run
any forward pass. The ggml file format is documented in whisper.cpp's
whisper_model_load() function (lines 1485-1750 above).

I'll create:
1. `wb_whisper_engine/` directory
2. `wb_whisper_model.c` — parse ggml FP16 file, load all weights into our
   own tensor structs
3. `wb_whisper_model.h` — tensor structs + model struct
4. `wb_whisper_math.h` — matmul, softmax, layer norm function declarations
5. Test: load tiny.en FP16, print weight shapes to verify parsing

Then iterate: mel → encoder → decoder → sample → validate against whisper-cli.

