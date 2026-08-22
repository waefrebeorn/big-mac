# R055 — Blender feature survey → Big Mac "MiniBlender" CGI engine scope

Research pass: Blender's toolset (Wikipedia feature list, Blender 5.x manual,
release notes) mapped against what a C11 software renderer inside a video
editor can honestly deliver for an AGI author of 3D diagrams/animations.

## What Blender does (surveyed)

| Domain | Blender capability |
|---|---|
| Modeling | primitives, meshes, curves, NURBS, metaballs, text; edit mode (extrude/bevel/subdivide) |
| Modifiers | non-destructive stack: Array, Bevel, Boolean, Build, Decimate, EdgeSplit, Mask, Mirror, Multires, Remesh, Screw, Solidify, SubdivSurf, Triangulate, Wireframe; deform: Cast/Curve/Displace/Lattice/Shrinkwrap/SimpleDeform/Smooth/Wave; physics: Cloth/Fluid/Particle/SoftBody/Ocean |
| Shading | node materials, procedural textures, PBR (Principled BSDF), texture/vertex painting |
| Animation | keyframes + f-curves, armatures/IK, shape keys, constraints, lattices, hooks, nonlinear animation (NLA) |
| Rendering | EEVEE (rasterized PBR realtime), Cycles (path tracer), workbench; passes, DOF, motion blur |
| Compositing | node compositor (glare, blur, color grade, alpha-over) |
| Video | Sequencer (VSE) with strip modifiers — compositing applied in the edit |
| Simulation | particles, fluid/smoke (Mantaflow), cloth, soft bodies, rigid bodies |

## Honest scope for MiniBlender v1 (this repo)

What an AGI actually needs to make *diagram/animation videos* on a
dual-core iMac, ranked by leverage:

| # | Feature | Blender analogue | Status |
|---|---|---|---|
| 1 | Modifier stack (non-destructive) | Modifiers | **BUILD NOW** — array/mirror/subdiv-wave/solidify-lite/twist |
| 2 | Scene graph w/ parenting | Collections/parents | **BUILD NOW** — child transforms follow parent |
| 3 | Keyframe channels per property + easing | F-curves | have linear; ADD ease-in/out/bounce |
| 4 | Camera as animatable object | Camera | BUILD — orbit/dolly/zoom keyframed like any object |
| 5 | Lights: directional + point, Lambert+specular | EEVEE lights | BUILD — cheap, huge visual gain over baked shade |
| 6 | Depth buffer (Z-test) instead of painter's sort | Z-buffer | BUILD — fixes interpenetration correctness |
| 7 | Motion blur (per-frame temporal supersample) | Cycles MB | BUILD — 2-4 taps, big cinematic win |
| 8 | Text-to-3D title objects | Text object | LATER (needs font outlines; use bitmap extrude lite) |
| 9 | Particle system (simple emitters) | Particles | LATER v2 |
| 10 | Boolean ops | Boolean modifier | LATER (hard; CSG is a project) |
| 11 | Node compositor | Compositor | HAVE-equivalent: ffmpeg filtergraph at export |
| 12 | Path tracing | Cycles | OUT OF SCOPE by doctrine (realtime raster only) |

## Build order this round

1. `wb_rast`: z-buffer + directional light + specular (replaces painter sort)
2. `wb_mod`: modifier stack — ARRAY, MIRROR, WAVE(deform), SOLIDIFY-lite, SUBDIV(catmull-rom lite)
3. `wb_anim`: easing functions (linear/ease-in-out/bounce/elastic), camera object, parenting
4. Export path: `wb_anim_render_frame` -> RGBA PNG seq / rawvideo pipe -> ffmpeg overlay onto clip
5. AGI bridge: `wb_agi` task type "cgi-render" = scene JSON-ish text protocol -> frames -> overlay

## Doctrine notes

- All C11, opaque structs, self-contained modules; no god headers.
- Rasterizer stays fixed-function realtime style (Jet lineage). No path tracing.
- Everything gated: test_rast/test_mesh/test_anim extended + new test_mod.
