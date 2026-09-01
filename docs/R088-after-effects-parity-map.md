# Big Mac vs Adobe After Effects — Parity Map (R088)

## After Effects Core Architecture
- **Layer-based** compositing (not track-based like NLEs)
- **Composition** = container for layers with own resolution/FPS/duration
- **Shape layers** = vector primitives (rect, ellipse, polygon, star, path)
- **Masks** = per-layer alpha masks (vector or raster)
- **Effects** = per-layer pixel operations (100+ built-in)
- **Expressions** = JavaScript-based property animation system
- **3D renderer** = Classic 3D, Advanced 3D (Cinema 4D engine), Ray-traced
- **Keyframe interpolation** = Bezier, linear, hold, ease-in/out
- **Parenting** = layer-to-layer transform hierarchy
- **Null objects** = invisible transform controllers
- **Pre-comps** = nested compositions (like nested sequences)
- **Motion blur** = per-layer and composition-level
- **Rotoscoping** = Roto Brush (AI-powered), mask tracking
- **Time remapping** = per-layer speed with frame blending
- **Blend modes** = 30+ modes (same as Photoshop)
- **Adjustment layers** = effect layers that affect all layers below
- **Track mattes** = use one layer's alpha/luma as another's mask
- **Puppet tool** = mesh-based character deformation
- **Particle systems** = CC Particle World, Particular (3rd party)
- **Expressions scripting** = JavaScript engine for procedural animation
- **Motion Graphics templates** = .mogrt export for Premiere

## Feature Parity Matrix

| Feature | After Effects | Big Mac | Gap? |
|---------|--------------|---------|------|
| **Node-based compositing** | ❌ (layer-based) | ✅ | **BETTER** |
| **Layer-based compositing** | ✅ | ❌ | **GAP** |
| **Shape layers (vector)** | ✅ | ❌ | **GAP** |
| **Masks (per-layer)** | ✅ | ❌ | **GAP** |
| **Rotoscoping / Roto Brush** | ✅ | ❌ | **GAP** |
| **Mask tracking** | ✅ | ✅ (motion_track) | PARITY |
| **Keyframe interpolation (Bezier)** | ✅ | ✅ | PARITY |
| **Expressions (JavaScript)** | ✅ | ❌ | **GAP** |
| **Null objects / parenting** | ✅ | ❌ | **GAP** |
| **Pre-comps / nesting** | ✅ | ✅ (nested sequences) | PARITY |
| **Adjustment layers** | ✅ | ❌ | **GAP** |
| **Track mattes** | ✅ | ❌ | **GAP** |
| **Puppet tool (mesh warp)** | ✅ | ❌ | **GAP** |
| **Particle systems** | ✅ | ✅ (wb_particle.c) | PARITY |
| **3D compositing** | ✅ Advanced 3D | ✅ (stereo 3D) | PARTIAL |
| **True 3D models (OBJ/C4D)** | ✅ | ❌ | **GAP** |
| **Camera + lights** | ✅ | ❌ | **GAP** |
| **Depth of field** | ✅ | ❌ | **GAP** |
| **Motion blur** | ✅ | ❌ | **GAP** |
| **Blend modes (30+)** | ✅ | ✅ (14 modes) | PARTIAL |
| **Color correction** | ✅ | ✅ | PARITY |
| **Color grading** | ✅ | ✅ | PARITY |
| **LUT support** | ✅ | ✅ | PARITY |
| **Chroma key** | ✅ | ✅ | PARITY |
| **Stabilization** | ✅ | ✅ | PARITY |
| **Transitions** | ✅ | ✅ | PARITY |
| **Text/titles** | ✅ | ✅ | PARITY |
| **Animated text (per-char)** | ✅ | ❌ | **GAP** |
| **Time remapping** | ✅ | ✅ (speed) | PARITY |
| **Frame blending** | ✅ | ❌ | **GAP** |
| **Proxy workflow** | ✅ | ✅ | PARITY |
| **GPU acceleration** | ✅ (CUDA/OpenCL/Metal) | ✅ (Metal) | PARITY |
| **HDR preview** | ✅ | ❌ | **GAP** |
| **32-bit float processing** | ✅ | ✅ (float wb_px) | PARITY |
| **Audio effects** | ✅ | ✅ | PARITY |
| **Video export (MP4)** | ✅ | ✅ | PARITY |
| **GIF export** | ✅ | ✅ | PARITY |
| **Image sequence** | ✅ | ✅ | PARITY |
| **SVG import** | ✅ | ❌ | **GAP** |
| **PSD import** | ✅ | ❌ | **GAP** |
| **AI/Illustrator import** | ✅ | ❌ | **GAP** |
| **Scripting/automation** | ✅ (JS expressions) | ✅ (agent API) | PARITY |
| **Motion Graphics templates** | ✅ (.mogrt) | ❌ | **GAP** |
| **Third-party plugins** | ✅ (huge ecosystem) | ✅ (OFX/VST3) | PARITY |

## After Effects Gaps to Close (15 features)

### TIER 1: Core VFX/Compositing (high value, achievable)
1. **Shape layers** — vector primitives (rect, ellipse, polygon, star, path)
2. **Per-layer masks** — vector mask with feather, expansion, animation
3. **Blend modes expansion** — add remaining 16 modes (overlay, soft light, hard light, etc.)
4. **Adjustment layers** — effect layer affecting all layers below
5. **Track mattes** — alpha/luma matte from another layer
6. **Motion blur** — per-pixel motion blur based on transform velocity
7. **Frame blending** — optical flow / pixel motion for slow-mo

### TIER 2: Motion Graphics (high value, moderate effort)
8. **Per-character text animation** — text animators (position, scale, rotation, opacity per char)
9. **Puppet tool** — mesh-based deformation with pins
10. **Null objects + parenting** — transform hierarchy
11. **Expressions engine** — mini scripting language for procedural animation

### TIER 3: 3D & Import (moderate value, high effort)
12. **3D camera + lights** — virtual camera with DOF, lights with shadows
13. **True 3D model import** — OBJ/GLTF loading with materials
14. **SVG import** — parse SVG paths into shape layers
15. **HDR preview** — HDR display output

## Strategy

After Effects is layer-based; Big Mac is node-based. We don't need to
replicate AE's layer model — we need to match its CAPABILITIES through
the node graph. Key insight: many AE features map naturally to nodes:

- Shape layer → wb_node_source_shape() (vector primitive generator)
- Mask → wb_node_effect_mask() (per-node alpha mask)
- Adjustment layer → wb_node_effect() applied to a composite
- Track matte → wb_node_composite() with matte input
- Motion blur → wb_node_effect_motion_blur()
- Frame blending → wb_node_effect_frame_blend()
- Puppet tool → wb_node_effect_mesh_warp()
- Null/parenting → wb_node_transform() hierarchy
- Expressions → wb_node_param_expression() (formula string → value)

The node graph is actually MORE powerful than AE's layer stack for
compositing — we just need more node types.
