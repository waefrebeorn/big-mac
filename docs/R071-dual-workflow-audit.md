# R071 — Dual workflow audit: human & agent make a music video

## The scenario

A 3-minute song exists (`song.wav`). Goal: a music video — footage on the
timeline cut to the beat, a CGI logo sting, a live-VJ chorus section,
delivered to YouTube.

---

## WALKTHROUGH A — THE HUMAN (GUI only, no terminal)

### A1. Import the song
Drag `song.wav` into MEDIA tab → lands on an audio track. ✅ natural.

### A2. Import footage
`^I` (import) in EDIT → picks up demo.mp4 or file dialog. ✅ works.
**FRICTION 1:** each import lands at timeline position **0.0**. Importing
5 clips stacks them all at zero; the user must drag each one to its slot
by hand with pixel-precision trim handles. There is no "append to end of
track" import mode — the single most common NLE operation.

### A3. Cut to the beat
Play the track, tap M at each beat? **No beat-detection marker tool
exists.** The user marks markers by hand via the MARKER button and types
positions. **FRICTION 2:** no "tap marker at playhead" key. Markers exist
(chapters use them) but there's no fast performance path to lay down 60
beat markers.

### A4. Trim clips to markers
Possible with the R043 handles (left/right trim, fades). ✅ but slow:
**FRICTION 3:** no ripple trim — trimming clip 2 leaves a hole instead of
pulling clips 3..n left. Every edit after the first means manually
re-sliding everything.

### A4b. JKL scrubbing ✅ (R067) genuinely good here.

### A5. CGI logo sting
The GUI has a 3D-CGI tier… but the CGI scene editor is AGI-bridge-first.
**FRICTION 4:** a human cannot place a cgi-box/keyframe from the ribbon;
there are no ribbon buttons for primitives/keys. The tier renders the
scene but offers no authoring UI.

### A6. VJ the chorus
PERFORM tier: fire decks live while the transport plays, RECORD ARM
captures it. ✅ this flow is excellent. Then perf-freeze… **FRICTION 5:**
perf-freeze is agent-only! The human has NO button to freeze their own
recording onto the timeline. They can record it but not nest it.

### A7. Deliver
EXPORT tab exports video; loudness normalize + chapters are agent
commands only. **FRICTION 6:** no "DELIVER" button that does
export→normalize→chapters in one click.

### Human verdict
Core loop works; the friction is all *authoring speed*: append-import,
tap-markers, ripple trim, CGI buttons, freeze button, deliver button.
Six gaps, all GUI-side.

---

## WALKTHROUGH B — THE AGENT (stdin protocol)

```
import /music/song.wav          # ⚠ imports as VIDEO clip? do_import always
                                #   makes a video track + video clip!
split 0 0 12.5                  # cut at first drop
...
export /out.mp4                 # plain export
normalize /out.mp4 -14          # ❌ normalize takes WAV paths, not mp4
chapters /out.txt               # ✅
shadow-save /proj.shadowbin.json # ✅
```

### Agent findings

1. **`import` is video-only.** `do_import` never branches on extension;
   importing `song.wav` creates a broken video clip pointing at a WAV.
   The agent has no way to add an audio clip by path. **BLOCKER.**

2. **No `move <track> <clip> <new_pos>`.** After split, the agent can't
   reposition pieces. It can build a fresh session in order, but any
   non-linear arrangement is impossible. **GAP.**

3. **No `delete <track> <clip>`.** A wrong import poisons the session —
   undo exists (checkpoint before import? import doesn't checkpoint!) but
   `import` isn't wrapped in checkpoint like `split` is. **INCONSISTENCY.**

4. **`normalize` is wav-only** while `export` produces mp4. The agent
   must know to render audio separately (`wb_engine_render_session`) —
   there's no `render-audio <out.wav>` command. **GAP.**

5. **No `deliver` compound** — agent must chain export + render-audio +
   normalize + chapters itself. Four commands where one should do, and
   ordering pitfalls between them. **GAP.**

6. **No `marker <pos_s> <label>` command.** Chapters come from markers,
   but the agent can't create markers! Only the GUI button can.
   **BLOCKER for the chapters story.**

7. **No `state clips` detail** — `state` prints counts only; the agent
   can't see what's on the timeline to reason about it. Blind editing.

8. **cgi-render/perf-freeze work well** ✅. Idempotent, ERR-prefixed.

### Agent verdict
The verbs exist for a linear build-once session but the agent cannot
*recover* (no delete), cannot *arrange* (no move), cannot *see* (no clip
listing), and two BLOCKERs (audio import, markers) break the music-video
story entirely.

---

## SYMBIOSIS GAPS (the handoff points)

| Handoff | Status |
|---|---|
| Human records VJ take → agent freezes/polishes | freeze is agent-only; needs GUI button + agent both ✓ |
| Agent builds rough cut → human refines in GUI | shadow-load restores layout ✅ but positions-only (no gain restore on reload? verified ✅) |
| Human marks beats → agent cuts to them | markers invisible to agent (no state/markers output) |
| Agent normalizes → human hears result | no playback-of-file path |

---

## FIX LIST (priority order)

**BLOCKERS**
1. `import-audio <path> [pos_s]` — real audio clip import by path (+ fix
   do_import to branch on extension).
2. `marker <pos_s> <label>` — agent marker creation.
3. GUI: FREEZE button in PERFORMANCE view (wraps same call as agent).

**HIGH**
4. `move <track> <clip> <pos_s>` + `delete <track> <clip>` (+ checkpoint).
5. `render-audio <out.wav>` command.
6. `deliver <out.mp4> [-14]` compound: export_perf_overlays → render-audio
   → normalize → mux → chapters text.
7. `state clips` — per-track clip listing for agent reasoning.

**MEDIUM (GUI)**
8. Append-import mode (drop at end of track).
9. Tap-marker key (T) during playback.
10. Ripple trim toggle.
11. DELIVER button = deliver command with UI.
