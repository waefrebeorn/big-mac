# R013 — The "Fusion Sauce": node-based compositor study (Natron + OpenFX)

**Date:** 2026-08-18
**Scope:** What makes DaVinci Resolve / Fusion-class node compositors good, and how
to lift Big Mac's video editor (R008/R009) to that standard. Also: design
principles for the SDL UI layer and the audio engine.

## 0. Premise correction (read this first)

> **DaVinci Resolve / Fusion source code is PROPRIETARY. Blackmagic never
> open-sourced Fusion. There is no "download the source" path.** Any site
> offering "Fusion source" is a scam or a config/API dump, not the compositor.

The genuine, open, **SLERM-able** twin in the exact same class (node-graph
compositor, Fusion/Nuke-shaped) is **Natron** (GPLv2). Fusion's *plugin*
standard is the **OpenFX (OFX) API** — which is open and is precisely what
Fusion, Nuke, Resolve, HitFilm and Natron all implement. So the real "sauce"
is recoverable two ways:

1. **Architecture sauce** → study Natron's Engine source (real node-graph
   evaluation, RoI/RoD pull model, disk image cache, mipmap/proxy scale).
2. **Extensibility sauce** → study the OpenFX SDK (the plugin contract
   Fusion/Resolve themselves use; a host can load Fusion-built OFX plugins).

Sources acquired (local clones, like the `~/ref/ardour` + `~/ref/lmms` from R002):
- `~/ref/natron/` — Natron `main` (shallow), Engine + Gui + App. 82 MB.
- `~/ref/openfx/` — OpenFX SDK `main` (shallow), `include/` (spec headers) +
  `HostSupport/` (reference host) + `Examples/` (reference plugins). 24 MB.
- SDL2 source already vendored at `third_party/SDL2-2.32.10/` (101 MB) — studied in place.

## 1. 7-hop Kevin Bacon — ≥7 sources, ≥3 domains, ONE convergent truth

### Domain A — Node-graph evaluation model (the core difference vs a timeline editor)
1. **Natron `Engine/EffectInstance.cpp` `getImagePlane()`** (line 748): the
   engine is **pull-based**. A downstream node *pulls* an image from an input
   for a given RoI; the input re-enters `getImagePlane` recursively. There is no
   global "render everything" pass — evaluation is demand-driven from the
   viewer/output node.
2. **Natron `EffectInstance::resolveRoIForGetImage()`** (line 632): before
   pulling, a node calls `getRegionsOfInterest` on itself to compute which
   sub-rect of *each input* it needs for the current render window, then pulls
   only that. This is the **RoI (Region of Interest) propagation** that makes
   node compositors resolution-independent and fast.
3. **Natron `getRegionOfDefinition_public`** (line 660+): each node also
   declares its **RoD (Region of Definition)** — the bounding box of pixels it
   can produce, often grown by its effect (blur radius, transform translate,
   etc.). RoD flows downstream; the output RoD is the union.
4. **Natron `TreeRender`** (`Engine/TreeRender.h` line 153): a render is a
   `(time, view, node, plane, proxyScale, mipMapLevel, RoI)` tuple. Multiple
   TreeRenders run concurrently, each uniquely identified, each with its own
   OpenGL context (`TreeRender::create` line 240). This is the **task model**
   that lets scrubbing/playback/interactive-param co-exist without blocking.

### Domain B — Image caching (why node compositors feel instant)
5. **Natron `ImageCacheKey`** (`Engine/ImageCacheKey.h` line 33): every rendered
   tile is keyed by `(node, time, view, plane, proxyScale, mipMapLevel, rect)`.
   A pull hits the cache first; only a cache miss descends the graph. This is
   why changing one blur radius only re-renders the affected subtree.
6. **Multi-resolution / proxy**: Natron's `RenderScale` (proxyScale × mipMapLevel)
   means the *same graph* renders at 1/4/1/16 res during scrubbing and full res
   on export — parameters are scaled, not re-authored. Mirror of our 480p
   proxy decision (R008 §E) but generalized to a scale factor.

### Domain C — The OpenFX plugin contract (the part Fusion/Resolve share)
7. **OpenFX `OfxPlugin` struct** (`include/ofxCore.h` line 94) + `OfxGetNumberOfPlugins`
   / `OfxPluginMain`: a plugin is a C struct with a `setHost`, and a dispatch
   function receiving **actions** (`kOfxActionLoad`, `kOfxActionDescribe`,
   `kOfxImageEffectActionRender`). The host owns the event loop; the plugin is
   passive and purely functional on render.
8. **OpenFX `kOfxImageEffectActionRender`** (`include/ofxImageEffect.h` line 515):
   the render action is called with an **RoI**, the host provides input images
   already allocated (the plugin asks via `clipGetImage`), and the plugin writes
   its output into a host-provided buffer. **Tiling is host-controlled** — the
   plugin declares `kOfxImageEffectPropSupportsTiles` and the host may call
   render many times with different RoIs. This is the secret to Fusion's
   "infinite canvas / huge images": no node ever holds the whole frame.
9. **OpenFX `HostSupport/examples/hostDemo*`** (real reference host in the SDK):
   `hostDemoHostDescriptor`, `hostDemoEffectInstance`, `hostDemoClipInstance`,
   `hostDemoParamInstance` show exactly how a host loads a `.ofx` bundle, calls
   `Describe`, builds clip/param instances, and drives the render action.
   **This is the SLERM target if we want Big Mac to load Fusion/Resolve/Nuke
   OFX plugins.**
10. **OpenFX `Examples/Invert/invert.cpp`, `Examples/Basic/basic.cpp`**: minimal
    reference plugins — the template for our own "built-in nodes" expressed as
    OFX plugins, so the same node runs in Natron and Big Mac.

### Domain D — SDL2 as the portable presentation layer (vendored, studied in place)
11. **SDL2 `README.md` / `README-SDL.txt`**: "cross-platform development library
    designed to make it easy to write multi-media software." Design principle:
    **thin abstraction over native media APIs** (each platform has a backend
    `src/video/cocoa`, `src/audio/coreaudio`, etc.) behind one stable C API.
    Match: our engine is the "native media" and SDL2 is just the window/texture
    surface — keep it that way, do not let SDL leak into the graph model.

### Domain E — Carryover: why a node graph beats a long effects chain (DAW lesson)
12. **R002 staged-pipeline lesson (Ardour DAG of routes)**: a fixed
    insert-chain is a degenerate node graph (linear). Fusion's lesson: make the
    graph *explicit and user-editable* so any node can be inserted/rewired
    without recompiling the engine. Big Mac's audio engine is currently a fixed
    per-track insert chain — the node model is its natural generalization for the
    video side (and eventually audio).

### Domain F — RoI correctness is a correctness property, not an optimization
13. **Natron `supportsMultiResolution()` branching** (`EffectInstance.cpp` 683-702):
    when a node does NOT support multi-res, the engine merges all input RoIs
    into one expanded rect; when it DOES, it pulls each input at exactly the
    needed sub-rect. Getting this wrong = subtle edge artifacts (a blur eating
    its own transparent border). **This is the #1 bug class in node compositors**
    and the standard we must meet.

### Domain G — Time + view + plane as first-class axes
14. **Natron `TimeValue` / `ViewIdx` / `ImagePlaneDesc`**: a node output is not
    "an image" but "(time, view[stereo], plane[color/alpha/depth/...)". The graph
    is evaluated per (time, view, plane). Our video editor currently thinks in
    single-stream video; the standard is multi-plane (e.g. alpha, masks) — the
    Fusion differentiator.

## Convergent truth (one sentence)

> **The "Fusion standard" is a PULL-BASED, REGION-OF-INTEREST / REGION-OF-DEFINITION
> node graph with a tile-addressed disk image cache, rendered through an
> OpenFX-plugin host contract so that effect nodes are passive, functional, and
> externally loadable — and every render is a (time, view, plane, scale, RoI)
> task that can run concurrently and at proxy resolution. Big Mac's video editor
> meets this standard not by copying Fusion's GUI, but by (a) turning the FFmpeg
> decode/proxy/export chain into RoI-pull nodes, (b) adding a tile cache keyed on
> (node,time,plane,scale,rect), and (c) speaking OpenFX so Fusion/Resolve OFX
> plugins become loadable effect nodes.**

## 2. Sauce → Big Mac design deltas (what "new standards" means concretely)

### D1 — Promote the video timeline to a node graph (not just tracks)
- Today (R009): video tracks + a single FFmpeg filter-graph export. That is a
  *fixed chain*. The standard is a **user-editable node graph** where each clip
  is a `Read` node, `Transform`/`ColorCorrect`/`Blur` are nodes, and a `Viewer`
  (or `Write`) is the sink.
- Minimum viable: keep the timeline for *assembly* (trim/split/B-roll as now),
  but render each video track through a small **per-track node chain** (Read →
  optional effects → Output), pull-based. This reuses R009's decode/proxy and
  adds compositing headroom without a full node UI on day one.

### D2 — RoI/RoD pull model for preview (the real fix for "video on top of audio")
- Mirror Natron `resolveRoIForGetImage`: the compositor asks each node "what
  rect of input do you need for this output RoI?" and pulls only that. For our
  preview this means: decode only the proxy frame slice the viewport needs, in
  the proxy scale (480p), not the whole 1080p frame every tick.
- `supportsMultiResolution` → our effects that are spatial (blur, transform,
  grade) must declare they need a *grown* RoI; non-spatial (color grade,
  invert) can pull exact. This prevents the transparent-border artifact (F13).

### D3 — Tile-addressed disk/mem image cache (the "instant" feel)
- Add `wb_video_cache_key { node_id, time, view, plane, scale, rect }`. A pull
  checks the cache; on miss it descends. This is what makes scrubbing feel free
  and is the direct analogue of Natron `ImageCacheKey`.
- Tie scale to our 480p proxy: scale=proxy during edit, scale=1 on export.

### D4 — OpenFX host = extensibility to Fusion/Resolve plugins (the big one)
- The long-term "new standard" move: implement a thin **OpenFX host** in C11
  (`wb_ofx_host`) that can load `.ofx` bundles. Then Fusion/Resolve/Nuke OFX
  plugins become effect nodes in Big Mac for free — that is literally the
  Fusion sauce, recovered legally via the open standard both sides implement.
- Near-term: model our *built-in* effect nodes (transform, grade, blur, invert,
  merge) with the **same action contract** (`Load`/`Describe`/`Render(RoI)`)
  even before we load third-party bundles, so the host slot is trivial later.
- Reference: `~/ref/openfx/HostSupport/examples/hostDemo*` (host) and
  `~/ref/openfx/Examples/Invert/invert.cpp` (plugin) — SLERM these, do not link them.

### D5 — (time, view, plane, scale) as the render coordinate
- Promote our single-stream video concept to `(time, view, plane, scale, RoI)`.
  `view` stays 0 (mono) for now; `plane` starts at color but leaves room for
  alpha/mask (Fusion's node masks). `scale` = proxy vs full.

### D6 — SDL2 stays the dumb surface (don't let it leak)
- SDL2's own design (D11) is a thin native-abstraction layer. Keep it that way:
  it owns window + texture blit only. The graph/cache/OFX logic lives in pure
  C11 (`src/wb_video*.c`), matching the rest of the engine. No SDL types in the
  node structs beyond the final texture blit.

## 3. Build order to reach the standard (incremental, verify each)
1. RoI/RoD helper structs + `wb_video_cache_key` + cache (mem LRU, file for disk).
   Test: render a 2-node chain twice; second hit is a cache read (no decode).
2. Pull-based preview: `wb_video_node_pull(node, time, roi)` → decode-only-needed.
   Test: viewport decode pulls one proxy slice, not full frame.
3. Per-track node chain (Read→effects→Output) reusing R009 decode/proxy.
   Test: a tracked blur with a grown RoI produces correct borders (no artifact).
4. `wb_ofx_host` skeleton speaking Load/Describe/Render; one built-in node
   expressed as an OFX-style action plugin. Test: host loads our node, renders.
5. (Later, big) load a real third-party OFX bundle. Test: external plugin node
   renders in Big Mac.
6. Screenshot smoke per view; commit.

## 4. Wired/Open ledger delta (see INDEX.md)
- New refs: `~/ref/natron`, `~/ref/openfx` (OpenFX SDK).
- Sauce captured: pull eval, RoI/RoD, tile cache key, OpenFX host/plugin action
  contract, multi-res correctness, (time,view,plane,scale) render coords.
- Open (next): D1–D5 implementation; OFX host is the headline "new standard" item.
