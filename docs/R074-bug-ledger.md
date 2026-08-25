# R074 Compositor Bug Ledger — 100 items (audit hop 109)

## A. Resolution / scaling (the "wrong size" family)
1. comp_pull allocates fixed 4096x4096 RoD regardless of graph format  **[FIXED]**
2. wb_node_pull clips ROI against hardcoded 4096x4096, not node format  **[FIXED]**
3. wb_node_pull_request same hardcoded 4096 clip  **[FIXED]**
4. No graph-level resolution concept — every node invents its own w/h  **[FIXED]**
5. src_color ignores ROI: writes ROI but leaves rest zeroed (black frame if ROI partial)  **[FIXED]**
6. src_text ignores ROI entirely; draws full frame always  **[FIXED]**
7. trans_pull iterates a->w*a->h linearly, ignoring rx/ry/rw/rh  **[FIXED]**
8. trans_pull has NO a-vs-b size mismatch guard (OOB read on nested graphs)  **[FIXED]**
9. trans out->roi set from request even though loop covers full frame (lie)  **[FIXED]**
10. No resampler/scaler node exists — mismatched sizes can't be reconciled  **[FIXED]**
11. win_cx/cy normalized coords assume square pixels (aspect distortion on anamorphic)  **[FIXED]**
12. vignette maxd uses corner distance; Vegas uses width/2 (different falloff shape)  **[FIXED]**
13. text x/y/scale are absolute pixels — break when frame size changes  **[FIXED]**
14. composite output alpha starts 0; black bg shows where nothing drawn (no background plate option)  **[FIXED]**
15. PPM writer quantizes with +0.5 round but no clamp >1.0 values (wraps to garbage)  **[FIXED]**
16. PPM writer no gamma/color-tag; players assume sRGB — washed output  **[FIXED]**
17. mp4 export hardcodes 64x64-friendly sizes only via caller; no aspect check  **[FIXED]**
18. blur reads/writes full W,H buffers but only clears ROI of tmp (stale data at edges)  **[FIXED]**
19. iris transition maxd assumes center origin; no off-center iris param  **[FIXED]**
20. camera-shake/CGI overlay nodes not audited for DPI/coords consistency  **[FIXED]**

## B. Color science
21. All blending done in sRGB space, not linear light (dark blends wrong)  **[FIXED]**
22. glow adds excess*color without clamping (values >1 wrap in PPM)  **[FIXED]**
23. primary grade applies gain BEFORE gamma (order: lift,gain,gamma — should be lift,gamma,gain per ASC)  **[FIXED]**
24. white balance temp multiplies after grade ops (WB should be first)  **[FIXED]**
25. HSL hue_shift parsed but unimplemented ((void)hsh) — parameter is dead  **[FIXED]**
26. HSL selection softness linear, no smoothstep (banding at edges)  **[FIXED]**
27. curves op maps v>hig into 0.75..1 band but clamps u2>=0 — highlights above wht fold back instead of clipping
28. chroma spill suppression runs even when pixel was fully keyed (a==0 pixels get spill math)  **[FIXED]**
29. luma key uses Rec.709 weights on sRGB values (should be linear-light luma)  **[FIXED]**
30. key_tol default falls back to `gain` param — semantic collision with exposure gain  **[FIXED]**
31. sat computed around Rec.709 luma of sRGB values (same linear-light issue)  **[FIXED]**
32. shadow pass of text draws rgba(0,0,0,0.6) by OVERWRITING pixel (not blend) — kills underlying video  **[FIXED]**
33. text glyph write replaces RGBA instead of source-over blending with existing frame content  **[FIXED]**
34. noise dissolve hash uses px_i*const ^ py_i*const — visible axis-aligned correlation pattern  **[FIXED]**
35. split-flap jitter reuses same hash constants — correlated with checkerboard pattern  **[FIXED]**
36. vignette fall*fall applied multiplicatively to RGB pre-gamma (should be post)

## C. Performance / structure
37. blur executes entire 2-pass box PER PIXEL — O(n^2); must hoist to per-frame passes  **[FIXED]**
38. blur allocates a full temp frame EVERY pixel iteration (malloc storm)  **[FIXED]**
39. comp_pull 4096x4096 calloc = 64MB per pull at default (massive)  **[FIXED]**
40. eff_pull giant if-else chain per pixel — dispatch should be outside loops  **[FIXED]**
41. trans_pull mapf pulled even for ops that never read it  **[FIXED]**
42. cache node unbounded growth risk (max_frames unchecked on realloc path?)  **[FIXED]**
43. wb_node_param_value called multiple times per pixel for same params (hoist!)  **[FIXED]**
44. window params (win_*) re-fetched per pixel inside HSL loop  **[FIXED]**
45. sinf/cosf/powf/sqrtf per pixel — no lookup tables for gamma/vignette  **[FIXED]**
46. two-phase pull_request implemented but most nodes ignore phase 0 semantics
47. wb_frame lacks refcount — every stage copies or re-walks buffers
48. no tile-based rendering despite wb_compositor_tile_size() existing (dead API)
49. GPU backend flag exists but nothing checks it (dead enum)

## D. Correctness / logic
50. D78 debug fprintf(stderr) left inside hot HSL pixel loop (spams + slows)  **[FIXED]**
51. gain param override: `kv != 0 || e->gain == 0` — cannot set gain to exactly 0 when static nonzero  **[FIXED]**
52. same "gain" param doubles as blur radius fallback — semantic overload  **[FIXED]**
53. trans_pull `if (!a) return b;` leaks a's siblings when one input missing mid-graph  **[FIXED]**
54. transition duration relative to t=0 only — no start-time offset param (can't place at t=5s)  **[FIXED]**
55. nested transitions receive outer t directly — inner transition sees full timeline, not local time  **[FIXED]**
56. split-flap cw/ch integer div rounds down — last row/col cells wider than others  **[FIXED]**
57. checkerboard cell size fixed 16px — not proportional to resolution  **[FIXED]**
58. venetian strips fixed 16px — same issue  **[FIXED]**
59. clock wipe hand anchored at frame center only  **[FIXED]**
60. barn-door dir field reused for axis AND direction semantics unclearly (overload)
61. zoom-blur tap count hardcoded 5; spin-blur too — no quality param  **[FIXED]**
62. directional-blur wipe only works horizontally (hardcoded x taps)  **[FIXED]**
63. ripple echo ring offset hardcoded 0.15*maxd  **[FIXED]**
64. map dissolve threshold uses strict > (no soft knee) unlike documented feather  **[FIXED]**
65. crossfade blends alpha linearly — premultiplied vs straight alpha ambiguity unresolved
66. composite alpha-over formula assumes straight alpha; sources are premultiplied-ish (inconsistent)
67. text anim modes 3/4 use d->anim_dur but mode 1 typewriter divides by same dur (ok) yet mode 2 slide uses w/2 fixed  **[FIXED]**
68. text cx param overrides x but y has no cy equivalent (asymmetric API)  **[FIXED]**
69. wb_frame_set_gpu flag set but GPU path never frees differently (leak pattern ready)
70. roi_clip negative rx adjustment can produce rw<0 before final check (works but fragile)  **[FIXED]**

## E. API / plumbing
71. wb_transition_preset returns node with params bound to internal tracks — caller can't retime them
72. preset 2 silently overrides requested duration (<1.5 forced) — surprising behavior  **[FIXED]**
73. no wb_node_set_start_time — all timing baked at t=0 origin  **[FIXED]**
74. wb_compositor_export_mp4 uses system() — no error capture, shell injection via path possible  **[FIXED]**
75. mp4 export fps/dur ints only; no audio-video sync guarantee beyond -shortest
76. PPM sequence filenames %05d overflow after 99999 frames (silent overwrite)
77. export_mp4 mkdir race between concurrent exports (pid-suffixed but rmdir non-empty fails silently)  **[FIXED]**
78. no way to query a node's output dimensions before pulling (needed for plumbing)  **[FIXED]**
79. wb_node_add_param returns slot idx but -1 on dup? duplicates silently replace (undocumented)  **[FIXED]**
80. param tracks owned by caller but node keeps raw pointers — dangling if caller frees early
81. transition dir field overloaded (wipe direction vs barn-door axis)
82. no refcounting on wb_frame across cache/composite (documented ownership maze)
83. selftest still probes stderr D78 line (test depends on debug spam)  **[FIXED]**
84. test suite never tests ROI < frame anywhere (ROI paths untested)  **[FIXED]**
85. no test for size-mismatched transition inputs (crash class uncovered)  **[FIXED]**
86. no fuzz/property test for PPM roundtrip  **[FIXED]**
87. catalog doc says ops 0-18 but presets expose only 4 — discoverability gap  **[FIXED]**
88. wb_render --transition-frames hardcodes 2s duration and 64x64  **[FIXED]**
89. wb_render video mode ignores --lufs (audio/video flags disjoint)  **[FIXED]**
90. no version string in CLI outputs (can't tell which build rendered a file)  **[FIXED]**

## F. Demo-facing polish
91. showcase renders at 64x64 (blocky) — needs 640x480+ path  **[FIXED]**
92. title scale=2 tiny at real resolutions — needs resolution-relative sizing  **[FIXED]**
93. soundtrack is bare sine — no envelope, clicks at boundaries  **[FIXED]**
94. scene colors are flat fields — no gradients/patterns to show grading  **[FIXED]**
95. no fade-from-black at demo start / fade-to-black at end  **[FIXED]**
96. transitions all centered — no movement to show directional features  **[FIXED]**
97. GIF palette generated at fps=6 without dithering flags (banding)
98. showcase.mp4 has no faststart flag (moov at end — stalls web playback)  **[FIXED]**
99. no poster/thumbnail extraction alongside exports  **[FIXED]**
100. demo graph built ad-hoc in test file — no reusable builder API for apps  **[FIXED]**
