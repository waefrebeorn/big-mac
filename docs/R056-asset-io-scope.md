# R056 — Asset I/O: import/export everything (research + scope)

Recursive-learning pass on interchange formats. Philosophy per user:
**integrate everything** — Kenney asset kits (user owns them), Blender
interchange formats, and our own scene format, so Hermes/AGI can pull any
asset into a 3D diagram video.

## Format survey (researched online)

| Format | What it is | Priority |
|---|---|---|
| **OBJ (+MTL)** | Wavefront text: `v` verts, `f` faces (v/vt/vn), groups; MTL has Kd colors | **v1 IMPORT (have) + EXPORT (build)** — universal fallback |
| **GLB / glTF 2.0** | Khronos runtime format: 12-byte header (`glTF` magic, version 2), JSON chunk (0x4E4F534A) + BIN chunk (0x004E4942); accessors/bufferViews over the binary blob | **v1 IMPORT** — Kenney ships GLB as primary format |
| **Kenney kits** | CC0 packs (Car Kit, City Kit, Platformer Kit…), thousands of models in OBJ+FBX+GLB, consistent scale/pivots | **LIBRARY INTEGRATION** — index + browse-by-kit + stamp into scenes |
| PLY | point/mesh, ascii+binary | later |
| FBX | proprietary; skip (GLB covers it) | never |

## glTF 2.0 parsing notes (from spec)
- GLB: `magic u32 = 0x46546C67`, `version u32 = 2`, `length u32`; then chunks:
  `{chunkLength u32, chunkType u32=0x4E4F534A "JSON", data}` then optional BIN.
- Mesh primitives reference **accessors**: each accessor = {bufferView,
  componentType (5126=float etc.), count, type (VEC3/SCALAR)}.
- For v1 we need only: POSITION (VEC3 float) + indices (SCALAR, u32/u16) +
  COLOR_0 optional. Materials: baseColorFactor if present else flat gray.
- Node hierarchy: apply node TRS (translation/rotation quat/scale) down the
  tree, flatten to world-space mesh (matches wb_mesh's baked philosophy).

## Build order

1. **wb_gltf.c** — GLB container parse (header/chunks), minimal JSON scan
   (hand-rolled, no deps: find "accessors"/"meshes"/"nodes" objects by key),
   POSITION+indices extraction with node-transform flattening → wb_mesh.
2. **OBJ export** — wb_mesh_write_obj(): v lines + f lines + per-face color
   via MTL (Kd). Round-trip test: export box → reimport → same counts.
3. **Asset library** — `assets/kits/<kit>/<model>.glb` layout + manifest;
   `wb_assets` module: scan dir, list kits/models, load by name (caches
   wb_mesh per model). Ships with an index of Kenney kit names.
4. **AGI bridge** — task type extensions: `cgi-add <kit/model> x y z`,
   `cgi-list-kits`, `cgi-list <kit>`.

## Honest v1 limits (documented, not hidden)
- GLB: no skins/animations/textures/samplers — geometry + color only.
- OBJ import: already have (fan-triangulated); MTL Kd honored on load now.
- Draco compression: not supported (skip compressed GLBs).
