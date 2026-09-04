# R102 Research: How YTP/YTPMV Is Actually Made

## The Real Workflow (from FL Studio creators)

### YTPMV Production (the actual craft):

1. **Source Audio** → Find a video with clear speech/dialogue
2. **Phoneme Isolation** → Chop audio into individual syllables/words
3. **Pitch Detection** → Use Newtone/Melodyne to detect the pitch of each chop
4. **Pitch Correction** → Tune each chop to a musical scale (usually chromatic or major)
5. **Sampler Channel** → Load each pitch-corrected chop into FL Studio's Channel Rack (sampler)
6. **Piano Roll** → Place notes on the piano roll where each note triggers a specific chop at a specific pitch
7. **Graph Editor** → Fine-tune pitch (Fine Pitch), panning (Pan), and timing (Shift) per note
8. **Beat Sync** → Quantize the note placement to the BPM grid
9. **Video** → Each audio chop has a matching video clip; when the audio plays, the video shows the character's mouth moving
10. **Composite** → Layer the video clips on a timeline synced to the audio

### Key Insight: The "Instrument" is the Voice

In FL Studio YTPMV:
- Each phoneme (vowel sound) becomes a **sampler instrument**
- The **piano roll** plays these instruments at different pitches
- **Fine Pitch** adjusts each note +/- 100 cents for perfect tuning
- **Note properties** control panning, velocity, and timing offset
- The **graph editor** provides per-note parameter control

### What Makes It Sound Good:

1. **Tight tuning** — Every phoneme must be perfectly in tune with the scale
2. **Rhythmic placement** — Notes must be precisely on the beat grid
3. **Velocity variation** — Dynamic velocity makes it musical, not robotic
4. **Harmonies** — Layer multiple phoneme tracks in thirds/fifths
5. **Bass drops** — Pitch a phoneme down an octave for bass notes
6. **Stutter effects** — Rapid note repeats on beats
7. **Sentence mixing** — Rearrange words to form new sentences

### YTP (not YTPMV) Visual Craft:

1. **Source abuse** — Take a single source and push it to absurdity
2. **Sentence mixing** — Rearrange dialogue to say new things
3. **Stutter loops** — Repeat a clip with increasing intensity
4. **Ear rape** — Sudden volume spikes for shock value
5. **Visual puns** — Replace objects with unrelated images
6. **Color grading** — Deep fry, invert, RGB shift for visual interest
7. **Transitions** — Hard cuts, strobes, impact frames between scenes
8. **Recursion** — Video within video (Droste effect)
9. **Steganography** — Hidden frames, subliminal messages

### What Big Mac is Missing (the real gaps):

1. **Sampler-as-instrument system** — Load a phoneme, play it at any pitch via piano roll
2. **Per-note pitch control** — Fine pitch adjustment per note (like FL's graph editor)
3. **Per-note video trigger** — Each note triggers a specific video clip
4. **Beat grid quantizer** — Snap notes to BPM grid with swing
5. **Velocity-sensitive playback** — Note velocity affects volume AND video speed
6. **Harmony generator** — Auto-generate harmony tracks from melody
7. **Bass drop generator** — Auto-detect bass notes and pitch down
8. **Stutter retrigger** — Rapid note repeat on beat drops
9. **Video clip database** — Catalog of phoneme video clips with metadata
10. **Composite timeline** — Layer video clips with transforms, blend modes, keyframes

### The "After Effects Style" 3D Matte FX:

This refers to:
- **Track mattes** — Use one layer's alpha/luma to mask another
- **3D camera** — Perspective-correct 3D space with depth
- **Painting effects** — Hand-painted matte layers for stylized looks
- **Character animation** — 3D models as animated overlays with lip-sync
- **Particle systems** — GPU-accelerated particles attached to characters
- **Blend modes** — 27 compositing modes (multiply, screen, overlay, etc.)
- **Adjustment layers** — Color grade child layers
- **Null objects** — Invisible transform parents for complex animation

### What Needs to Be Built:

1. **Sampler Instrument Engine** — The core YTPMV instrument system
2. **Piano Roll Sequencer** — Note placement with per-note pitch/pan/velocity
3. **Video Clip Trigger** — Note → video clip mapping with pitch-shifted playback
4. **Beat Grid Quantizer** — BPM-aware note snapping with swing
5. **Harmony Engine** — Auto-generate harmonies from melody tracks
6. **Composite Timeline** — Video layer stack with transforms and blend modes
7. **Track Matte System** — Alpha/luma matte layers for compositing
8. **Keyframe Interpolation** — Smooth animation between keyframes
