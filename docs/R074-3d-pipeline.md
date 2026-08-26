# Big Mac 3D Pipeline (R074)

Status: wired and gated. Engine selftest 750/0, compositor 262/0.

## Architecture

Pull-based RoI compositor (`src/wb_compositor.c`) feeds a fixed-function
software rasterizer (`src/wb_rast.c`). Animation/scene graph lives in
`src/wb_anim.c`; meshes in `src/wb_mesh.c`. Pure C11, zero third-party,
deterministic float policy (`-ffp-contract=off`, no fast-math).

## Rasterizer (wb_rast)

- Edge-function triangle fill, back-face cull by signed area
- Flat shading (per-tri color) **or** gouraud (`wb_rast_set_shading`):
  vertex normals accumulated from adjacent faces, lit per-vertex,
  barycentric intensity interpolation
- Z-buffer path (`fill_tri_z`) with per-tri alpha blending; transparent
  tris drawn after opaque via depth-bias in the painter's sort
- Wireframe mode (`wb_rast_set_wireframe`)
- Two-sided lighting flag (G-SF039)
- Viewport scissor (`wb_rast_set_scissor`)
- Skybox: vertical gradient behind the scene (`wb_rast_set_skybox`)
- Camera: orbit angles + distance + focal length (G-SF007), animated
  through the same keyframe system as objects; deterministic shake
  composition (`wb_anim_set_shake`, G-SF008)

## Animation (wb_anim)

- Per-channel keys (pos/rot/scale decoupled, G-SF003), key delete/move
  (G-SF004), looping/wrap keys (G-SF005), cubic bezier paths (G-SF006),
  shortest-arc rotation unwrap (G-SF022)
- Object parenting with additive translation inheritance
- Billboard sprites (G-SF010) and yaw-to-camera look-at (G-SF009) flags
- Emissive two-pass render (G-SF015; pass 2 additive over lit frame,
  skipped when unused — hop 199 fix), visibility windows (G-SF056),
  depth fog (G-SF017), resolution override (G-SF026)
- Instancing: `wb_anim_add_instance` shares geometry across static
  transforms (G-SF019)
- Supersampled AA (`wb_anim_render_frame_aa`), temporal motion blur
  (`wb_anim_render_frame_blur`), screenshot API (`wb_anim_screenshot`)
- fps/timebase metadata, progress callback, error surfacing

## Meshes (wb_mesh)

Primitives: box, cone, sphere-ish lathes, capsule, wedge. Append/merge.
Painting: whole mesh, per-face (`wb_mesh_paint_face`), gradients.

## Compositor bridge

CGI renders enter the node graph as `wb_node_source_anim` /
`wb_node_source_frame`; per-layer transforms, layer reorder, Mode-7
warp, letterbox/scanline/chromatic nodes sit downstream. Export pipes
PPM frames straight into ffmpeg (no temp files); `--lufs` applies
two-pass loudnorm on the muxed file; `--quality` / `--preview` set the
QoS dial; `--poster` grabs a thumbnail.

## Determinism policy (G-SF097)

No fast-math anywhere; FMA contraction disabled; all noise/hash uses
fixed seeds (see `wb_sfx.c`). Renders are bit-reproducible on this
machine.

## GPU story (G-SF095)

This machine is a 2012 dual-core iMac with no usable discrete GPU
compute path for our purposes, so Big Mac is **CPU-authoritative by
design** (WB_RENDER_CPU). The offload boundary still exists and is
honest:

- `wb_compositor_set_backend(WB_RENDER_GPU)` marks the intent; the flag
  is refused with a warning on builds without a GPU backend (no dead
  enum — hop 142).
- `wb_frame_set_gpu` likewise refuses: pixel buffers always live on the
  CPU heap, so a future Metal interop can swap the buffer behind
  `wb_frame.px` without changing any node contract.
- Threading instead of GPUs: `wb_rast_render_mt` uses both cores
  (G-SF099), which is where this machine's headroom actually is.

If this repo is ever built on Metal-capable hardware, the seam to fill
is exactly one function: the rasterizer's `set_scene` + `render` pair,
which already takes plain vertex/tri arrays and returns an RGBA buffer.


### G-SF099 real-world measurement (hop 198)
`--starfox` (66 objects, 336 frames, 640x360): ST default **18.2s**,
MT opt-in build **19.9s**. MT loses on this scene: per-frame thread
spawn ×336, ~all objects straddle the band midline, and scene fill is
only ~1.4ms/frame — thread spawn + sync dominates. MT remains
available via `WB_ANIM_USE_MT`; wins only on vertically-spread,
high-tri-count scenes. Default stays deterministic single-thread.

## Performance (R074 hop 193-198)

Measured on the host 2012 iMac, 640x360, 6048-tri sphere:

- Depth sort: O(n²) insertion → qsort; skipped entirely when zbuffer +
  all-opaque tris (order irrelevant). Sort was ~11ms/frame — now ~0.
- Edge functions: fully incremental (x-step per pixel, y-step per row);
  inner loop multiply-free for flat/untextured triangles.
- Affine depth stepping: dz per pixel/row precomputed from the plane
  equation; no barycentric normalize or z-interp in the hot loop.

Net: **12.6 ms → 1.1 ms per frame (~11x)** for the zbuffered fill path.

Threading: `wb_rast_render_mt` exists but measures slower than ST below
~20k tris (thread spawn + duplicated tri setup dominate); it delegates
to ST there. Real-scene check (`--starfox`, 336 frames): 18.2s → 15.6s
via the emissive two-pass fix alone. The second core is better spent on
the ffmpeg encode subprocess, which already runs concurrently.

## Determinism verified end-to-end (hop 201)

Two consecutive `--starfox` exports produce **byte-identical MP4s**
(md5 match) — the full pipeline (rasterizer, compositor, engine audio,
loudnorm, ffmpeg mux) is reproducible on this machine. This validates
the `-ffp-contract=off` policy and fixed-seed noise under real load.
