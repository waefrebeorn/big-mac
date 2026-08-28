# Big Mac Post-Production Pipeline — "BIG 100" + 50 YTP Categories

**Date:** 2026-08-26
**Method:** 7-hop Kevin Bacon recursive research (50 queries × 2 rounds + deep dive)
**Target:** Complete post-production pipeline — character animation, VFX, editing, memes, AGI

---

## PART 1: THE BIG 100 — Post-Production Pipeline

### CATEGORY A: CHARACTER ANIMATION (1-20)

#### A1-A5: Skeletal & Bone Systems
1. **2D Bone Rigging** — skeleton hierarchy (bones + joints), parent-child transforms, forward kinematics. Spine/DragonBones format parser. Each bone = {position, rotation, scale, length, parent_index}.
2. **Inverse Kinematics (IK)** — given end-effector target (hand/foot position), solve chain of joint angles. CCD (Cyclic Coordinate Descent) or FABRIK algorithm. 2-bone IK: analytical solution via law of cosines.
3. **Skinning & Weight Painting** — assign vertices to bones with weights (0-1). Auto-skinning: distance-based or heat diffusion. Vertex final_pos = Σ(weight_i × bone_transform_i × rest_pos).
4. **Auto-Rigging** — detect character silhouette → place joints automatically. Bounding box → spine chain + limb chains. Template-based: match to humanoid/quadruped template.
5. **Mesh Deformation** — Free-Form Deformation (FFD): lattice of control points, Bernstein polynomial weights. Warp bitmap by deforming underlying mesh. Smooth interpolation.

#### A6-A10: Facial Animation & Lip Sync
6. **Viseme-Based Lip Sync** — map phonemes → mouth shapes (~15 visemes). Standard set: A (open), E (wide), O (round), F/V (lip bite), L (tongue), M/B/P (closed), etc. Real-time: audio amplitude → mouth open amount.
7. **Facial Blend Shapes** — morph targets for expressions: happy, sad, angry, surprised, blink, brow raise, sneer. 2D equivalent: sprite swapping or mesh vertex deltas.
8. **Eye Blink Animation** — timed blink cycle (every 2-4 sec, 4-6 frames). Sprite sequence: open → half → closed → half → open. Pupil tracking: eye follows target position.
9. **Pupil Tracking** — eye sprite offset based on look direction. Compute angle from character to target → offset pupil sprite within eye bounds.
10. **Eyebrow Animation** — raise/lower/tilt for expression. Sprite swap or mesh warp. Keyframe: brow position + angle over time.

#### A11-A15: Body Animation
11. **Walk Cycle** — 8-frame or 12-frame loop: contact, down, pass, up (×2 for each leg). Hip bob, arm swing, torso rotation. Speed = stride_length × fps.
12. **Squash & Stretch** — animation principle: deform shape based on velocity/impact. Bouncing ball: stretch vertically at speed, squash on ground contact. Preserves volume: sx = 1/sy.
13. **Smear Frames** — single stretched frame between keyframes to simulate motion blur. Stretch sprite along velocity vector. Subconscious effect — feels smooth.
14. **Secondary Animation** — follow-through: hair, clothing, tails continue moving after body stops. Overlapping action: different parts move at different times.
15. **Cloth/Cape Simulation** — spring-mass system: grid of point masses + structural + shear + bending springs. Verlet integration. Pin top row to shoulders/spine.

#### A16-A20: Hair & Physics
16. **Hair Simulation** — mass-spring chain per strand. Edge springs (length), bending springs (angle), torsion springs. Simplified: 5-10 control points per strand, 20-50 strands.
17. **Spring-Mass Physics** — Verlet integration: x_new = 2x - x_old + a×dt². Constraints: distance between neighbors. Stable, fast, no velocity storage.
18. **Jiggle Physics** — secondary bone with spring-damper. Offset = spring(velocity, stiffness, damping). Applied to: ears, tails, breasts, bellies, etc.
19. **Collision Detection** — simple circle/segment collision for hair vs body. Push out of collision sphere. Keeps hair from clipping through head/torso.
20. **Wind/Force Fields** — global force vector applied to all physics objects. Animated wind: noise-based direction + strength. Affects hair, cloth, particles.

---

### CATEGORY B: LIGHTING & SHADOWS (21-35)

#### B21-B25: 2D Lighting Systems
21. **Normal Mapping (2D)** — encode surface direction in RGB texture. Per-pixel: light_contribution = dot(normal, light_dir). Multiple lights: sum contributions.
22. **Point Lights** — radial falloff: intensity = 1/(1 + k×distance²). Color + position + radius + intensity. Render to light buffer, multiply with scene.
23. **Directional Lights** — sun/moon: parallel rays, no falloff. Angle + intensity + color. Shadow direction = opposite of light angle.
24. **Spot Lights** — cone-shaped: inner angle (full intensity) + outer angle (falloff). Flashlight, stage light. Smooth edge falloff.
25. **Ambient Light** — base illumination level. Ensures nothing is pure black. Color temperature (warm/cool).

#### B26-B30: Shadow Systems
26. **Shadow Casting (2D)** — for each light, project silhouette edges away from light. Build shadow polygon (quad per edge facing away). Render as dark semi-transparent overlay.
27. **Shadow Mapping (2.5D)** — render depth from light's perspective → compare with camera depth. In software: ray-march from pixel toward light, check occlusion.
28. **Blob Shadows** — simple ellipse under character. Cheap, effective. Scale with height above ground. Soft edge (gradient).
29. **Volumetric Light** — light shafts/god rays. Radial gradient from light source, masked by occluders. Additive blend for glow.
30. **Specular Highlights** — Blinn-Phong: spec = dot(half_vector, normal)^shininess. Adds shiny spots on eyes, metal, wet surfaces.

#### B31-B35: Color & Atmosphere
31. **Color Grading** — lift (shadows), gamma (midtones), gain (highlights). Per-channel RGB wheels. Offset for overall tint.
32. **3D LUT Application** — .cube file parser. Trilinear interpolation in 33³ RGB lattice. O(1) per pixel after LUT loaded.
33. **Tone Mapping** — HDR → LDR conversion. Reinhard: x/(1+x). ACES filmic curve. Prevents clipping in bright areas.
34. **Color Temperature** — warm (orange) to cool (blue) shift. Kelvin scale: 3000K (warm) to 10000K (cool). Multiply R/B channels.
35. **Fog/Atmosphere** — distance-based color blend. Fog_color × (1 - e^(-density×distance)) + scene × e^(-density×distance). Depth cueing.

---

### CATEGORY C: CAMERA & PERSPECTIVE (36-50)

#### C36-C40: Camera Movement
36. **Pan** — horizontal rotation from fixed position. Translate camera x by delta. Background layers move at parallax rates.
37. **Tilt** — vertical rotation. Translate camera y. Same as pan but vertical.
38. **Dolly/Zoom** — camera moves toward/away from subject (dolly) or focal length changes (zoom). Scale all layers uniformly.
39. **Truck/Crab** — camera moves left/right (horizontal translation). Different from pan: position changes, not rotation.
40. **Pedestal** — camera moves up/down. Different from tilt: position changes vertically.

#### C41-C45: Camera Effects
41. **Camera Shake** — random offset per frame. Intensity decays over time. Impact shake: sudden jolt + decay. Handheld: low-frequency noise.
42. **Dolly Zoom (Vertigo)** — simultaneous dolly in + zoom out (or vice versa). Background appears to stretch/compress while subject stays same size.
43. **Depth of Field** — blur based on distance from focal plane. Gaussian blur with radius proportional to |z - focus_distance|. Bokeh for out-of-focus highlights.
44. **Motion Blur** — accumulate multiple sub-frames or stretch along velocity vector. Directional blur based on per-pixel motion vector.
45. **Lens Distortion** — barrel (convex) or pincushion (concave). Radial displacement: r' = r × (1 + k×r²). Chromatic aberration: separate R/G/B channels.

#### C46-C50: Perspective & Projection
46. **2.5D Parallax** — multiple layers at different depths. Each layer scales/moves based on camera z. Creates depth illusion from 2D sprites.
47. **Perspective Projection** — 3D point → 2D screen: x_screen = x × fov / z. Vanishing point where parallel lines converge.
48. **Isometric Projection** — 30° angle, no perspective distortion. x' = (x-y)×cos(30°), y' = (x+y)×sin(30°) - z. Retro game look.
49. **Oblique Projection** — front face true size, depth at 45° angle. Cabinet projection: depth halved. Technical/blueprint look.
50. **Skew/Shear** — affine transform: x' = x + skew_x × y. Used for italic text, dynamic ground planes, speed lines.

---

### CATEGORY D: VISUAL EFFECTS (51-70)

#### D51-D55: Particle Systems
51. **Particle System Core** — emitter + particles with {position, velocity, life, color, size}. Update: pos += vel×dt, vel += accel×dt, life -= dt. Render as point sprites.
52. **Fire Effect** — upward velocity + turbulence + orange→red→transparent gradient. Additive blend. Particle count: 100-500.
53. **Smoke Effect** — gray particles, slow rise + expand + fade. Normal blend with alpha. Turbulence noise for swirling.
54. **Sparkle/Magic** — bright particles with star/cross shape. Short life, high velocity, gravity. Additive blend for glow.
55. **Explosion** — burst emitter: 100+ particles in all directions. Debris (solid sprites) + fire (additive) + smoke (alpha). Shockwave ring.

#### D56-D60: Distortion Effects
56. **RGB Shift / Chromatic Aberration** — offset R, G, B channels horizontally. Amount increases toward edges. Glitch aesthetic.
57. **Scanlines** — horizontal dark lines every 2-4 pixels. Retro CRT look. Overlay blend.
58. **VHS Distortion** — tracking lines (bright horizontal band moving down), color bleed, tape noise, timestamp overlay. Multiple layered effects.
59. **Pixel Sorting** — sort pixels in a row/column by brightness within a threshold mask. Creates melting/dripping effect.
60. **Datamosh** — remove I-frames, let P-frames reference wrong base. Motion vectors applied to wrong image = melting faces. FFmpeg: `-g 0` then edit.

#### D61-D65: Color Effects
61. **Deep Fry** — extreme contrast + saturation + sharpening + JPEG artifacts. Multiple sharpen passes. Hue shift. Noise overlay.
62. **Color Curves** — bezier-interpolated LUT. Types: RGB (per channel), Hue vs Sat, Hue vs Hue, Lum vs Sat. 256-entry lookup table.
63. **Posterize** — reduce color depth: levels = 4-8. output = floor(input × levels) / levels. Retro/banded look.
64. **Color Replace** — select hue range → replace with new hue. Tolerance threshold. Green screen prep, costume color change.
65. **Solarize** — invert colors above threshold. output = input > threshold ? 1-input : input. Surreal, psychedelic.

#### D66-D70: Transition Effects
66. **Cross Dissolve** — linear opacity interpolation: out = A×(1-t) + B×t. Standard transition.
67. **Wipe** — directional reveal: B appears behind a moving line. Variants: left, right, circular, star, clock.
68. **Morph** — optical flow warp between scenes. Heavy computation. Simplified: cross-dissolve + scale/position match.
69. **Glitch Transition** — RGB shift + scanlines + frame slice displacement between cuts. Abrupt, energetic.
70. **White Flash** — 1-2 frames of pure white between cuts. Impact moment, jump cut cover.

---

### CATEGORY E: COMPOSITING & LAYOUT (71-85)

#### E71-E75: Layer Compositing
71. **Alpha Compositing** — Porter-Duff "over": out = src + dst×(1-src_alpha). Standard layer blend.
72. **Blend Modes** — multiply, screen, overlay, soft-light, hard-light, difference, exclusion. Per-channel formulas. W3C spec.
73. **Track Matte** — use one layer's alpha/luma as mask for another. Luma matte: bright = visible, dark = transparent.
74. **Picture-in-Picture** — scale down secondary video, position in corner. Optional border + shadow.
75. **Split Screen** — two videos side by side. Vertical or horizontal divider. Animated wipe reveal.

#### E76-E80: Transform & Warp
76. **Ken Burns Effect** — pan + zoom on still image. Keyframe start/end position + scale. Ease in/out.
77. **Corner Pin** — map video to arbitrary quadrilateral. Perspective-correct texture mapping. Sign replacement, screen insert.
78. **Mesh Warp** — deform video using grid of control points. Free-form distortion. Face morph, flag wave.
79. **Lens Flare** — bright light artifact: halo + streaks + ghost reflections. Additive overlay. Position tracks light source.
80. **Light Leak** — warm color wash from edge. Film camera artifact. Overlay blend, animated position.

#### E81-E85: Text & Graphics
81. **Lower Third** — name/title overlay in lower 1/3 of screen. Background bar + text + optional logo. Slide-in animation.
82. **Animated Title** — text with keyframe animation: position, scale, rotation, opacity, color. Easing curves.
83. **Subtitle Rendering** — SRT/ASS parser → CoreGraphics text → bitmap → alpha composite. Font, size, color, outline, shadow.
84. **Safe Area Guides** — 80% (title-safe) / 90% (action-safe) overlay. Rule-of-thirds grid. Platform-specific (TikTok/Instagram).
85. **Watermark/Logo** — semi-transparent image overlay. Corner position. Optional animation (fade in/out).

---

### CATEGORY F: AUDIO-VISUAL SYNC (86-95)

#### F86-F90: Audio Visualization
86. **Audio Waveform Display** — peak file (min/max per time bucket). Render as filled polygon. Stereo: two rows.
87. **Audio Spectrum** — FFT magnitude per frequency band. Bar graph or circular. Color by frequency.
88. **Beat Detection** — onset detection: energy difference between frames. Threshold → beat timestamps. Sync visual effects to beats.
89. **Auto-Ducking** — sidechain: when dialogue present, reduce music volume. Envelope follower on dialogue track → gain on music track.
90. **Sound Effect Trigger** — map sound effects to visual events. Vine boom → screen shake. Airhorn → flash frame.

#### F91-F95: Lip Sync & Timing
91. **Sentence Mixing** — cut phonemes/syllables → rearrange into new phrases. Audio: pitch-corrected segments. Video: mouth shape matching.
92. **Stutter Loop** — repeat short clip N times. Audio: repeat waveform. Video: repeat frames. Rhythmic effect.
93. **Pitch Shift** — change audio pitch without speed. Phase vocoder or FFT-based. YTP staple: chipmunk (high) or demon (low).
94. **Ear-Rape** — sudden volume spike. Audio: clip to max. Video: flash frame + screen shake. Shock humor.
95. **Reverse Audio** — play sound backwards. Reverb-like effect. Combined with reverse video for rewind gag.

---

### CATEGORY G: AGI & AUTOMATION (96-100)

96. **Agent Command System** — natural language → video edit commands. "Cut this scene" → find boundary → split. "Add boom here" → insert SFX + shake.
97. **Auto-Assembly** → detect scenes → score by motion/audio energy → select highlights → assemble rough cut. Silence removal.
98. **Semantic Search** → transcribe audio (whisper.cpp) → index text → find segments by keyword → jump to timestamp.
99. **Template-Based Editing** → project template: intro + clips + transitions + outro + music. Agent fills in content.
100. **Smart Reframing** → detect subject → auto crop to aspect ratio (16:9 → 9:16). Track subject position.

---

## PART 2: 50 YOUTUBE POOP / MEME EDITING CATEGORIES

### GROUP 1: YTP POOPISMS (1-15)
1. **Stutter Loop** — repeat clip segment N times (video + audio)
2. **Stutter Loop Plus** — stutter with different effect each loop iteration
3. **Stutter Loop Minus** — stutter with video removed (audio only)
4. **Buzzing Stutter** — stutter loop with added buzzing noise overlay
5. **Sentence Mixing** — cut syllables → rearrange into new phrases
6. **Ear-Rape** — sudden max volume blast + screen flash
7. **Pitch Shift Up** — chipmunk voice (2× pitch, formant preserved)
8. **Pitch Shift Down** — demon voice (0.5× pitch)
9. **Reverse Clip** — play segment backwards (video + audio)
10. **Speed Up** — 2×-8× speed (time stretch, pitch rises)
11. **Slow Down** — 0.25×-0.5× speed (dramatic, deep voice)
12. **SpaDinner** — insert famous soundbites ("Dinner", "Lotsa Spaghetti")
13. **Word Salad** — random word insertion from source material
14. **Climax** — build up with increasing intensity → sudden stop
15. **Loop Outro** — end on infinite loop of funny moment

### GROUP 2: MEME EDIT STYLES (16-30)
16. **Phonk Edit** — bass-heavy phonk music + fast cuts + shake + bass boost visual
17. **Anime Edit** — anime clip + music sync + speed ramps + impact frames
18. **AMV (Anime Music Video)** — anime footage edited to song, beat-synced
19. **Deep Fried** — extreme contrast/saturation/sharpen + JPEG artifacts
20. **Vaporwave** — slowed + reverb + pink/cyan tint + scanlines + statue
21. **Cursed Edit** — unsettling combination: wrong audio + distorted video
22. **Wholesome Edit** — soft music + slow motion + warm color grade + hearts
33. **Cinematic Edit** — letterbox + film grain + color grade + slow-mo + epic music
24. **Vine Compilation** — 6-second clips + vine boom transitions + fast pace
25. **TikTok Edit** — vertical 9:16 + captions + trending sounds + quick cuts
26. **Gaming Montage** — kill highlights + bass boost + flash transitions + zoom
27. **Trailer Edit** — movie trailer style: dramatic music + slow-mo + title cards
28. **Meme Review** — screen recording + facecam + zoom on meme + reaction
29. **Shitpost** — intentionally bad editing: wrong timing, jarring cuts, loud audio
30. **Aesthetic Edit** — soft color grade + slow motion + dreamy music + bokeh

### GROUP 3: TRANSITIONS & EFFECTS (31-40)
31. **Whip Pan** — fast horizontal blur between scenes
32. **Zoom Cut** — sudden zoom in on detail → cut to new scene
33. **Flash Frame** — 1-2 white frames between cuts
34. **Shake Transition** — camera shake → cut at peak shake
35. **Speed Ramp** — slow → fast → slow (time remap keyframes)
36. **Match Cut** — similar composition/color between scenes
37. **Jump Cut** — same angle, time removed (jarring)
38. **J-Cut** — audio from next scene starts before video
39. **L-Cut** — video from next scene starts before audio
40. **Invisible Cut** — hidden cut behind object/flash

### GROUP 4: AUDIO MEMES (41-45)
41. **Vine Boom** — impact sound + screen shake + flash
42. **Airhorn** — loud horn sound + zoom + flash
43. **Bruh** — short "bruh" sound effect + freeze frame
44. **Yeet** — whoosh sound + fast zoom + motion blur
45. **Oof** — Roblox death sound + slow-mo + vignette

### GROUP 5: ADVANCED TECHNIQUES (46-50)
46. **Datamosh Transition** — remove I-frames → P-frames reference wrong base → melting
47. **RGB Glitch** — separate R/G/B channels + offset + scanlines
48. **VHS Overlay** — tracking lines + noise + color bleed + timestamp
49. **Face Zoom** — detect face → extreme zoom + earrape + flash
50. **Rewind Effect** — reverse playback + rewind sound + static lines

---

## PART 3: IMPLEMENTATION PRIORITY

### Phase 1: Core Animation (Week 1-2)
- Bone rigging + FK + IK
- Walk cycle + squash/stretch
- Eye blink + pupil tracking
- Basic particle system

### Phase 2: VFX & Compositing (Week 3-4)
- Alpha compositing + blend modes
- Color correction (lift/gamma/gain)
- 3D LUT loader
- Transitions (dissolve/wipe/flash)
- Camera shake + zoom

### Phase 3: Lighting & Camera (Week 5-6)
- 2D lighting (point + directional + normal map)
- Shadow casting (blob + projected)
- Parallax scrolling
- Ken Burns + corner pin

### Phase 4: AGI & Automation (Week 7-8)
- Agent command system
- Auto-assembly
- Scene detection
- Template-based editing

### Phase 5: Meme/YTP Engine (Week 9-10)
- Stutter loop + sentence mixing
- Pitch shift + ear-rape
- Deep fry + VHS + datamosh
- Beat detection + auto-duck
- Sound effect triggers

### Phase 6: Polish & Export (Week 11-12)
- Hardware encoding (VideoToolbox)
- Background render queue
- Project save/load
- Keyboard shortcuts
- Multi-format export

---

## KEY TECHNICAL DECISIONS

1. **All software rendering** — no GPU dependency, SIMD batch processing
2. **Effect chain architecture** — ordered list of per-pixel functions
3. **Bone system** — FK + IK, Spine/DragonBones compatible format
4. **Particle system** — Verlet integration, 100-500 particles per emitter
5. **Audio sync** — beat detection + onset detection for auto-sync
6. **Agent-driven** — natural language → command queue → render
7. **YTP engine** — dedicated module for meme/poop editing techniques
8. **FFmpeg integration** — decode, thumbnail, hardware encode, proxy generation
9. **CoreGraphics** — text rendering, UI, scopes, safe area overlays
10. **Command pattern** — undo/redo for all edit operations
