# R065 — PERFORMANCE ribbon tab: live VJ layer with recorded replay
## Concept (user directive)

A new workspace tier: **PERFORMANCE**. Video-DJ session on top of the music
you're working on:

- **Deck grid**: pads holding clips / CGI scenes / effects (Makepad VJ-style
  deck library with thumbnails)
- **Live play**: hit pads to fire visuals in sync with the music; crossfade
  between A/B decks; tweak effect params while it runs
- **Record the performance**: every pad hit, fader move, and param change is
  captured as a timed EVENT LIST. The recording becomes a *nested element*
  on the timeline — a reproducible clip that soft-renders (replays the event
  list deterministically) inside any export.
- **AI assist**: Hermes can trigger pads/params via agent commands, or
  generate an entire performance pass from a music analysis.

## Reference: Makepad VJ console (makepad/makepad, work branch)
Borrowed ideas (philosophy, not code — they're Rust/C++17+GL, we're C11):
- Effect-tile library grid; decks keep playing while others load
- "Politeness verdicts": widen resources when idle, narrow when performing
- Transition system between decks
- Measured-first culture: instrument the hot path before optimizing

## Big Mac mapping (C11, existing engine)

| Makepad concept | Big Mac implementation |
|---|---|
| Deck | `wb_perf_deck`: mesh/clip slot + params |
| Pad hit | timed event `{deck_id, action, t}` |
| Fader move | timed event stream -> automation lane at record-stop |
| Crossfade A/B | two wb_rast scenes + alpha blend per frame |
| Nested element | new clip type: PERF (event-list reference, soft-rendered) |
| AI assist | agent commands: `perf-fire <deck>`, `perf-fade <pos>`, `perf-record`, `perf-stop` |

## Build order
1. `wb_perf.c/h`: decks, event list, capture state machine (arm/stop),
   deterministic replay (`wb_perf_render_frame(t)` = pure function of events)
2. Ribbon: add PERFORMANCE tier; wire draw_cgi_view-style perf view
3. Timeline nested element: PERF clips render via the event list during
   export and preview
4. Agent commands for hands-free performance
