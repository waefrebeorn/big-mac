# Big Mac ← WuBuRVC: The Steal (attribution + boundary)

> What Big Mac pulled from `github.com/waefrebeorn/wuburvc` (the other
> agent's RVC engine, 73MB, parity-verified corr 0.9999 vs PyTorch),
> and — critically — what it did NOT pull.

## The boundary (the user's instruction)
> "you can steal from this **without being a neural net voice**"

Big Mac is a **pure articulatory synthesis** engine (waveguide vocal
tract + LF glottis, no samples, no neural networks). WuBuRVC is a
**neural voice-conversion** engine. The steal is the DSP, not the brain.

## Pulled (pure DSP, no neural nets) — 6 modules
| Module | What it does | Where it landed |
|--------|--------------|-----------------|
| `wubu_master.{c,h}` | EQ→comp→sat→width→limiter→loudness chain | `src/wuburvc/` |
| `wubu_consonant.{c,h}` | spectral-flatness (Wiener entropy) voicing mask — the SOTA way to do voiced/unvoiced | `src/wuburvc/` |
| `wubu_breath.{c,h}` | SILENCE/BREATH/CONSONANT/VOICED per-frame classes (INTERSPEECH 2024 recipe) | `src/wuburvc/` |
| `wubu_harmony.{c,h}` | dual-fundamental detection (lead + harmony pitch) | `src/wuburvc/` |
| `wubu_fft.{c,h}` | FFT | `src/wuburvc/` |
| `wubu_stft.{c,h}` | STFT (OpenMP stripped — this iMac has no libomp; serial fallback) | `src/wuburvc/` |

## NOT pulled (the neural voice — stays out)
- `wubu_rmvpe.*` — DeepUnet+BiGRU pitch network
- `wubu_rvc_hubert.*` — HuBERT content features
- `wubu_rvc.*` / flow / NSF vocoder — the generator
- `wubu_train.*` — training
- Any CUDA/Vulkan/GGUF machinery

## Integration
- `tools/wb_master.c` — masters any Big Mac render (RMS −18 dBFS target,
  verified: homer peaks −11.5 dBFS, RMS −18.0 dBFS)
- All 15 cartoon voices mastered → `~/Music/BigMac-Voices/mastered/`
- Next: wire `wubu_consonant` (voicing) + `wubu_breath` (breath events)
  into `wb_measure` for SOTA-quality analysis

## Integrity rules (doctrine)
1. Pulled code is **unmodified** except mechanical fixes (include paths,
   OpenMP strip). The 2 warnings in `wubu_harmony.c` are upstream's.
2. Attribution: headers retain `WaefreBeorn-UMV3` license text.
3. Big Mac never pushes to wuburvc; pull-only relationship.
