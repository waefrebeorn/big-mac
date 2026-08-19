# R018 — Recursive Research Loop (frontier after R017)

**Method:** 7-hop, multi-domain web recon (25+ sources) → converge on the
highest-leverage gaps that move `big-mac` toward "complete and total supremacy"
(video + audio editor). Each gap is implemented, verified by `make test_*`,
committed, then the loop re-opens.

**Loop status:** HOP 1 complete (6 domains searched). Gaps below ranked by
leverage × achievability on our C11/SDL/FFmpeg/Node-Compositor base.

## Convergent findings (HOP 1)
| Domain | Convergent truth | Source signal |
|---|---|---|
| Resolve architecture | Multi-level node graph (Clip→GroupPre→GroupPost→Timeline) + 32-bit float + two-step ColorSpaceTransform (camera→working→display) + HDR/Dolby Vision | frame.io color-mgmt nodes; blackmagicdesign.com/resolve/color |
| 2026 AI layer | auto-reframe, voice isolation, object removal/inpainting, transcript editing are the reliable timeline-AI | storyflow 2026 tools; ngram; flonnect |
| Apple Silicon codecs | NO hardware AV1 (CPU-only even M4); ProRes + BRAW/ProRes RAW + HDR are the pro standards | reddit r/AV1; macrumors M3; iina #5383 (FFmpeg 8 AV1 hw) |
| Interchange | OTIO transfers "intent" not just data; EDL/AAF/FCPXML are legacy | liftgammagain OTIO; blackmagic forum; steinberg Nuendo 14 |
| Pro codecs | ProRes RAW/BRAW native; HDR10+/Dolby Vision/ST.2084/HLG tone mapping expected | apple ProRes authorized; blackmagic ProRes RAW threads |
| OSS weakness | Kdenlive/Shotcut/OpenShot = project corruption + high latency + GPU-only wins | YouTube "vs 2026" audit; reddit kdenlive vs shotcut |

## R018 gaps (ranked)
| # | Gap (where we aren't) | Why it matters | Evidence | Action | Status |
|---|---|---|---|---|---|
| A | **No ProRes export** (H.264 only) | ProRes 422/HQ is the editorial standard; NLEs exchange ProRes, not H.264 | apple ProRes authorized products; every pro pipeline | Add `wb_video_export_prores` (ffmpeg `prores_ks -profile:v 3` + `yuv422p10le`) + test | OPEN |
| B | **No HDR / wide-gamut color pipeline** in compositor | Resolve's moat = 32-bit float + two-step CST + ST.2084/HLG tone mapping; our compositor is 8-bit RGBA only | frame.io CST; blackmagic HDR grading | Add `wb_frame` 16-bit/half-float path + `wb_node_colorspace` (camera→working→display) + HDR tone-map node | OPEN |
| C | **No OTIO-class interchange** (we have EDL/FCPXML) | OTIO "transfers intent" — the modern convergence point | liftgammagain; steinberg Nuendo 14 | Extend FCPXML to carry color/audio intent; add a thin `wb_session_export_otio`-shaped adapter later | OPEN |
| D | **No dedicated voice isolation** (voice-polish exists but not spectral isolate) | 2026 AI = voice isolation / enhance speech is table-stakes | storyflow; flonnect | Add `wb_voice_isolate` (spectral gate tuned for speech) reusing WB_SAMPLE_RATE | OPEN |
| E | **No auto-reframe / object-aware** | smart reframe is a 2026 differentiator | storyflow auto-reframe | Defer: ML-heavy; stub a saliency-roi reframe node later | OPEN |
| F | **GPU rendering not realized** (G12 boundary stubbed) | Shotcut wins on GPU; our boundary exists but no Metal back-end | YouTube OSS audit | Defer: Metal interop layer slots into G12 `wb_frame.gpu` | OPEN |

## Convergence target (the "7 steps to Kevin Bacon 25")
R017 closed the *structural* gaps (node compositor, OFX, EDL/FCPXML, agent,
captions, two-pass loudness, param bus, GPU boundary). R018 closes the
*professional-output* gaps that make the editor genuinely competitive with
Resolve/Premiere: **ProRes + HDR color pipeline + OTIO-class intent + voice
isolation**. These are the next "Kevin Bacon" hops from where we stand.

## Plan of record
1. R018-A ProRes export — implement + test + commit.
2. R018-B HDR/wide-gamut color pipeline — implement + test + commit.
3. R018-C FCPXML intent enrichment (color/audio) — implement + test + commit.
4. R018-D voice isolation — implement + test + commit.
5. Re-open loop: HOP 2 recon on ML reframe / Metal GPU / BRAW decode.
