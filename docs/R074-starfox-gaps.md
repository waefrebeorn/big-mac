# R074 Star Fox Probe — 100-Gap Ledger (hop 113 audit)

Probed by building the corridor-run demo. Every friction, silent
failure, missing feature, and quality gap found on the way is a gap.

## A. wb_anim / 3D core (1-25)
1. G-SF001 [FIXED hop113] WB_ANIM_MAX_OBJS=32 hard cap — demo needed 46 objects; silent -1 drops **[FIXED]**
2. G-SF002 [FIXED hop113] WB_ANIM_MAX_KEYS=64/object — long flights need hundreds **[FIXED]**
3. G-SF003 No per-channel keying — all 8 channels set together (pos/rot/scale coupled) **[FIXED]**
4. G-SF004 No keyframe delete/move API **[FIXED]**
5. G-SF005 [FIXED hop113] No looping/wrap keys (floor strips need manual cycle hacks) **[FIXED]**
6. G-SF006 No path/curve animation (bezier flight paths for ships) **[FIXED]**
7. G-SF007 Camera has no FOV/focal control **[FIXED]**
8. G-SF008 Camera can't be parented or shake-composed per-frame cleanly **[FIXED]**
9. G-SF009 No look-at constraint (rings should face camera) **[FIXED]**
10. G-SF010 No billboard sprites (explosions/stars should always face camera) **[FIXED]**
11. G-SF011 Mesh primitives only box/sphere/cyl/cone/torus/plane/arrow — no capsule, no wedge **[FIXED]**
12. G-SF012 No mesh boolean/CSG
13. G-SF013 No vertex colors beyond single paint color per object **[FIXED]**
14. G-SF014 No texture mapping / UV coords at all
15. G-SF015 No emissive materials (engine glow faked with bright color) **[FIXED]**
16. G-SF016 No transparency in 3D (no alpha-sorted transparent pass) **[FIXED]**
17. G-SF017 No fog/atmospheric depth fade **[FIXED]**
18. G-SF018 No particle system (explosions, engine trails, sparks) **[FIXED]**
19. G-SF019 No instancing (24 starfield boxes = 24 objects instead of 1 draw) **[FIXED]**
20. G-SF020 No skeletal animation / bones
21. G-SF021 No morph targets
22. G-SF022 Rotation interpolation is Euler-only — gimbal artifacts possible **[FIXED]**
23. G-SF023 No animation events/callbacks at key times (spawn SFX at ring pass) **[FIXED]**
24. G-SF024 anim render output alpha-keyed but no depth composite against bg Z **[FIXED]**
25. G-SF025 No LOD system **[FIXED]**

## B. Renderer / rasterizer (26-45)
26. G-SF026 No resolution scale option (locked to anim WxH) **[FIXED]**
27. G-SF027 Flat shading only — no gouraud/phong interpolation option **[FIXED]**
28. G-SF028 Single directional sun — no point lights **[FIXED]**
29. G-SF029 No shadow casting (ship has no ground shadow) **[FIXED]**
30. G-SF030 [FIXED hop113] No dithering mode (SNES had ordered dither!) **[FIXED]**
31. G-SF031 [FIXED hop113] No palette quantize render target (true SNES 256-color mode) **[FIXED]**
32. G-SF032 No affine texture warp (the actual Mode-7 effect) **[FIXED]**
33. G-SF033 [FIXED hop113] Z-buffer precision unconfigurable; far plane clipping at z=-160 observed **[FIXED]**
34. G-SF034 No wireframe/debug draw mode **[FIXED]**
35. G-SF035 No viewport scissor/region rendering **[FIXED]**
36. G-SF036 No motion blur accumulation buffer **[FIXED]**
37. G-SF037 No post-FX chain inside 3D (bloom on engine glow) **[FIXED]** (glow/bloom node exists; chain after CGI source)
38. G-SF038 Rasterizer perf: no scanline SIMD; 15fps@640x360 was near budget **[FIXED]**
39. G-SF039 No back-face material variation (two-sided lighting wrong) **[FIXED]**
40. G-SF040 No anti-aliasing option (SSAA would help tiny stars) **[FIXED]**
41. G-SF041 No screenshot/frame grab API from live session **[FIXED]**
42. G-SF042 No multi-scene compositing with per-layer Z **[FIXED]**
43. G-SF043 No skybox/environment map **[FIXED]**
44. G-SF044 No screen-space reflections
45. G-SF045 No render-to-texture (portal/radar effects) **[FIXED]**

## C. Compositor integration (46-60)
46. G-SF046 [FIXED hop113] wb_node_source_frame copies full frame every pull — no dirty flag **[FIXED]**
47. G-SF047 [FIXED hop113] No direct anim->node bridge (demo hand-rolled rgba plumbing) **[FIXED]**
48. G-SF048 Composite RoD fixed 4096 scan — no auto-size from first pull **[FIXED]**
49. G-SF049 No layer transform on composited CGI (scale/pos of the 3D overlay) **[FIXED]**
50. G-SF050 [FIXED hop113] HUD text drawn manually onto rgba — no text node over 3D **[FIXED]**
51. G-SF051 No letterbox/safe-area guides **[FIXED]**
52. G-SF052 No scanline emulator filter node (CRT look) **[FIXED]**
53. G-SF053 No chromatic aberration / VHS node **[FIXED]**
54. G-SF054 Frame source has no fps/timebase metadata **[FIXED]**
55. G-SF055 No audio-reactive hooks into 3D anim (CGI bands exist but not wired here) **[FIXED]**
56. G-SF056 No per-object visibility track (blink/pulse objects) **[FIXED]**
57. G-SF057 Composite order fixed by add order — no reorder API **[FIXED]**
58. G-SF058 No nested comp caching (bg re-pulled per frame) **[FIXED]** (wb_node_cache wraps any nested comp graph)
59. G-SF059 PPM intermediate frames waste disk I/O — no pipe to ffmpeg stdin **[FIXED]**
60. G-SF060 No progress callback during export loops **[FIXED]**

## D. Audio / music (61-78)
61. G-SF061 No SMF (.mid) file loader **[FIXED]**
62. G-SF062 No SF2 soundfont loader/player **[FIXED]**
63. G-SF063 Soundtrack synthesized in Python outside the engine — engine should render its own demo audio **[FIXED]**
64. G-SF064 No FM synthesis voice (SNES-y timbres are limited with subtractive) **[FIXED]**
65. G-SF065 No sample-rate decimation/bitcrush unit for chiptune crunch **[FIXED]**
66. G-SF066 No arpeggiator midifx used/verified in demo path **[FIXED]** (existing engine feature verified)
67. G-SF067 No tempo map / BPM automation **[FIXED]**
68. G-SF068 No pattern sequencer (loops written as raw samples)
69. G-SF069 No sidechain-triggered duck tied to video events **[FIXED]**
70. G-SF070 Engine SFX: no procedural laser/explosion synth presets **[FIXED]**
71. G-SF071 No stereo pan automation per note **[FIXED]**
72. G-SF072 No reverb send routing verified for game-style ambience **[FIXED]**
73. G-SF073 Audio mux done by external ffmpeg — no internal AAC encode **[FIXED]**
74. G-SF074 No loudness normalization on the demo path (wb_delivery exists but unused) **[FIXED]**
75. G-SF075 No loop-region playback for previewing sections **[FIXED]** (existing engine feature verified)
76. G-SF076 No MIDI file export (compose in engine, save .mid) **[FIXED]**
77. G-SF077 Waveform editor view for synthesized stems **[FIXED]**
78. G-SF078 No chord-track/theory helpers **[FIXED]** (existing engine feature verified)

## E. CLI / workflow (79-90)
79. G-SF079 --starfox hardcoded scene — needs a scene description format **[FIXED]**
80. G-SF080 No project save/load for compositor+anim graphs (session-only)
81. G-SF081 No render queue (batch multiple demos) **[FIXED]**
82. G-SF082 No --quality flag (draft vs final passes) **[FIXED]**
83. G-SF083 No resume/checkpoint for long renders
84. G-SF084 GIF export not integrated (external ffmpeg call)
85. G-SF085 Poster/thumbnail extraction manual per demo **[FIXED]**
86. G-SF086 No --preview lowres fast pass flag **[FIXED]**
87. G-SF087 Agent commands (cgi-*) don't cover anim graphs **[FIXED]**
88. G-SF088 No unit test gate for sf_render_loop (untested code shipped) **[FIXED]**
89. G-SF089 No error surfacing when wb_anim_add_object hits cap (rc ignored) **[FIXED]**
90. G-SF090 Demo assets not versioned under projects/ **[FIXED]** (assets versioned under projects/)

## F. Deeper engine gaps exposed (91-100)
91. G-SF091 wb_clip layout freeze blocks per-clip 3D scene refs **[FIXED]**
92. G-SF092 Video track + CGI scene sync has no clock contract **[FIXED]**
93. G-SF093 No timeline video-event model for spawning scenes at clips **[FIXED]** (wb_anim event system)
94. G-SF094 Performance tier (wb_perf) doesn't know about anim sources
95. G-SF095 No GPU assist story even optional (dual-core CPU ceiling)
96. G-SF096 Memory: meshes copied per object without sharing/refcount **[FIXED]**
97. G-SF097 No deterministic float policy (render reproducibility across runs) **[FIXED]**
98. G-SF098 No color management between 3D (linear) and comp (gamma) **[FIXED]**
99. G-SF099 Threading: raster single-threaded while second core idles
100. G-SF100 No documentation page for the 3D pipeline (docs/ has DAW only) **[FIXED]**

