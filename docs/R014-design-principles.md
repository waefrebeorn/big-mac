# R014 — Consolidated Design Principles (SDL + DAW + Video Editor + Compositor)

**Date:** 2026-08-18
**Scope:** The durable design principles Big Mac must hold across ALL four
surfaces — the SDL presentation layer, the audio DAW engine, the video editor
(R008/R009), and the node compositor we are lifting to Fusion/Resolve standard
(R013). These are the rules that survive every individual feature; new code is
judged against them.

Grounded in real source: Ardour/LMMS (R002), SDL2 vendored source, Natron
Engine + OpenFX SDK (R013). Each principle cites its origin so it is not
arbitrary.

---

## P1 — The engine is a STAGED, ALLOCATION-FREE, NON-BLOCKING pipeline
**Origin:** R002 (Ardour `process_callback` try-lock, LMMS `renderStage*`).
**Rule:** The realtime/render path must never malloc, never block, never take a
contended lock. Use staged passes (schedule → instruments → effects), double-
buffered output, and try-lock + Xrun-report on contention.
**Applies to:** audio RT thread (done), video render pull (D3 cache must be
lock-free on the hot path), OFX render dispatch (must not block the UI thread).
**Anti-pattern:** calling `malloc` or `fopen` inside a per-frame pull.

## P2 — Device/platform independence behind a thin native abstraction
**Origin:** SDL2 `README` ("cross-platform library... thin abstraction over
native media APIs"); Ardour device-independent backends.
**Rule:** The engine and graph model know nothing about the OS. One portable C
API (SDL2 for window/texture, our own backends for audio/codec) sits between
engine and metal. Never let a platform type (Cocoa/ALSA/DirectX) into engine
structs.
**Applies to:** SDL2 owns window+blit ONLY (R013 D6). FFmpeg owns decode/encode
ONLY. The graph is pure C11.
**Anti-pattern:** `#ifdef __APPLE__` inside `wb_video_node_pull`.

## P3 — Demand-driven (PULL) evaluation, not global passes
**Origin:** R013 Natron `getImagePlane` pull model.
**Rule:** A consumer pulls what it needs; the graph descends only on cache miss.
No "render everything every frame." This is what makes node compositors fast and
resolution-independent, and it is the correct model for our preview too.
**Applies to:** video preview (decode only the needed proxy slice), effect
chains (only re-render the subtree a param changed), captions overlay.
**Anti-pattern:** decoding the full 1080p frame every preview tick.

## P4 — Regions (RoI/RoD) flow as first-class data
**Origin:** R013 `resolveRoIForGetImage` + `getRegionOfDefinition`.
**Rule:** Every node declares its Region of Definition (what it can produce) and
computes, for each input, the Region of Interest it needs for the current
render window. Spatial effects declare a *grown* RoI (blur radius, transform
offset); non-spatial effects pull exact. Getting this wrong = transparent-border
artifacts — a correctness property, not an optimization.
**Applies to:** every video effect node, the compositor merge, the transform.
**Anti-pattern:** a blur that reads only its output rect and eats its own edge.

## P5 — Cache by content identity, addressed by (node, time, view, plane, scale, rect)
**Origin:** R013 `ImageCacheKey`; proxy-scale from R008 §E.
**Rule:** Rendered output is keyed on the full identity of what produced it, so
repeated pulls (scrub, re-eval) are free. Tiling (rect in the key) is what makes
huge frames and partial re-renders possible. Scale in the key = proxy during
edit, full on export — the graph is scale-parametric, not re-authored.
**Applies to:** video image cache (D3), could extend to audio offline-render
memoization.
**Anti-pattern:** a single global "last frame" buffer.

## P6 — A render is a coordinate: (time, view, plane, scale, RoI)
**Origin:** R013 `TreeRender::CtorArgs` (time, view, plane, proxyScale,
mipMapLevel, canonicalRoI).
**Rule:** Treat a render request as a 5-tuple, not "an image." `view` = stereo
later, `plane` = color/alpha/mask later, `scale` = proxy/full now. Designing the
struct this way from day one is what lets Fusion-class features (masks, stereo,
multi-plane) land without refactoring.
**Applies to:** `wb_video_node_pull` signature, cache key, export.
**Anti-pattern:** a `uint8_t *frame` with implicit "this is RGBA at full res."

## P7 — Effects are PASSIVE, FUNCTIONAL plugins behind an action contract
**Origin:** R013 OpenFX (`kOfxActionLoad`/`Describe`/`Render(RoI)`); Natron
`EffectInstance` is a passive instance the host drives.
**Rule:** An effect node does not own the loop, the memory, or the schedule. The
host calls `Load` → `Describe` (declare clips/params/RoI support) → `Render(RoI)`
into a host-provided buffer. This is what makes effect nodes swappable,
externally loadable (Fusion/Resolve OFX), and testable in isolation.
**Applies to:** our built-in video effect nodes (D4) and the future OFX host.
**Anti-pattern:** an effect that opens its own window or spins its own thread.

## P8 — The graph is the user's, not the engine's
**Origin:** R013 (explicit user-editable node graph) vs R002 (fixed insert
chain is a degenerate graph).
**Rule:** A fixed per-track insert chain is a special case of a node graph. Where
the user benefits from rewiring (video effects, and eventually audio sends), make
the graph explicit and editable rather than hard-coding order. The engine
generalizes; the UI exposes the generalization.
**Applies to:** video per-track node chain (D1), eventual audio routing.
**Anti-pattern:** hard-coding "comp then eq then reverb" when a node graph fits.

## P9 — ONE surface, color = state, no menu-diving
**Origin:** R006 (Launchpad/HCI cognitive-load) carried into R008 §F.
**Rule:** Every mode the user must reach should be on one visible surface; state
is shown by color, not buried in menus. The video editor inherits this: tabs
minimal (VIDEO/AUDIO/CAPTIONS/EXPORT per R009 §4.3), transport reused, color
codes track kind / selection / playhead.
**Applies to:** video editor UI, any new panel.
**Anti-pattern:** a separate modal dialog for every export setting.

## P10 — Local-first, zero third-party compile burden (WuBu + Big Mac)
**Origin:** wubu-methodology (no third party, SLERM), R008 §G (FFmpeg only).
**Rule:** The only external libs are SDL2 (vendored) and FFmpeg (prebuilt).
Everything else — engine, effects, OFX host — is ours, written from understanding
not wrappers. Reference sources (Natron/OpenFX/Ardour/LMMS) are studied and
SLERM'd; we never link or vendor their code.
**Applies to:** the OFX host (SLERM `hostDemo*`, do not link it), effects
(SLERM `Invert`/`Basic`, do not copy them).
**Anti-pattern:** `git submodule add natron` or `#include <ofxhost>`.

## P11 — Seconds for video, samples for audio; convert at ONE boundary
**Origin:** R009 §2.1 (seconds vs sample timebase).
**Rule:** Video timeline is seconds (FFmpeg-natural); audio timeline is samples.
The conversion happens at exactly one place (the audio/video track boundary),
never scattered. The render coordinate (P6) expresses time in the consumer's
unit.
**Applies to:** transport, preview seek, export.
**Anti-pattern:** mixing `double sec` and `int sample` in the same struct field.

## P12 — Verify every claim by running it (no fake output)
**Origin:** wubu-methodology Triple-DA P1, Big Mac selftest gate.
**Rule:** A change is not done until `./build/wb_selftest` (151+), the video
decode test, the proxy test, and the `--screenshot` smoke all pass. Counts come
from the runner, never hand-transcribed. ASan-clean on the hot path.
**Applies to:** every doc in this set; each design delta above has a testable
build order (R013 §3).
**Anti-pattern:** "should work" without a green gate.

---

## How to use this doc
- New feature proposal → check it against P1–P12. If it violates one, the
  principle wins or the principle is amended here with a cited reason.
- P3/P4/P5/P6/P7 are the "Fusion standard" cluster (R013). P1/P2 are the engine
  substrate. P8/P9 are the UX rules. P10/P11/P12 are the WuBu/Big-Mac discipline.
- This doc is the index for the design language; R001–R013 are the evidence.
