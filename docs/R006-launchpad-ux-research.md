# R006 — Launchpad Mk2 UX + our-own-driver research (7-hop Kevin Bacon)

**Date:** 2026-08-16
**Scope:** How to make Big Mac's Launchpad Mk2 integration genuinely easy and
fun for beginners — our own C11 driver (no third-party lib) + a tabbed/organized
UI that switches between the keyboard piano roll and a Launchpad Mk2 piano roll.
**Method:** 7-hop Kevin Bacon per the WuBu doctrine — pull ≥3 unrelated domains,
extract each one's core finding, converge on ONE design principle. Below, 25
grounded sources across 8 domains feed the convergence.

---

## The 8 domains & 25 sources

### Domain A — Launchpad Mk2 hardware protocol (the driver must be byte-exact)
| # | Source | Core finding |
|---|--------|--------------|
| 1 | Novation Launchpad Mk2 Programmer's Reference (forum.bome.com/1019) | Mk2 is fully programmable via SysEx + notes; 64 pads + 8 top-row buttons, ~128 colors. |
| 2 | flmidi-101.readthedocs.io RGB example `F0 00 20 29 02 18 0B 51 3F 29 00 F7` | RGB LED SysEx format: `F0 00 20 29 02 18 0B <note> <r> <g> <b> F7` (each 0–63). |
| 3 | Novation default mappings (userguides.novationmusic.com, Mini mappings) | Mk2 grid note = `11 + col + row*10` (NOT classic `row*16+col`). Top row = notes 91–98. |
| 4 | zynthian.org/13329 custom Mk2 driver | Mk2 uses the `18` (0x12) "RGB" command byte; wrong family byte = dead LEDs. |
| 5 | Bome forum "continuous led-feedback Mk2" | Mk2 needs explicit SysEx color writes; no velocity-as-color like classic LP. |

### Domain B — macOS driver / class compliance (no third-party lib needed)
| # | Source | Core finding |
|---|--------|--------------|
| 6 | Novation "USB MIDI not working on Mac" support | Mk2 is class-compliant on macOS — shows as a CoreMIDI device with no driver install. |
| 7 | KORG reddit / vguitarforums class-compliant | "Any class compliant device works on macOS without a driver. Just plug in and go." |
| 8 | M-Audio class-compliant troubleshooting | Quick test: appears in Audio MIDI Setup automatically; CoreMIDI owns it. |

### Domain C — DAW / Launchpad usability pain (why people quit)
| # | Source | Core finding |
|---|--------|--------------|
| 9 | reddit r/ableton "is ableton hard to learn" | Ableton (and Launchpad) is hard at first because it has TWO very different metaphors (Session vs Arrangement). |
| 10 | Quora "why Ableton popular despite hard to learn" | Two metaphors + hidden depth = high initial cognitive load for novices. |
| 11 | pushpatterns.com "is ableton hard to learn" | Looks harder than it is; the interface throws people off because it doesn't behave like a tape/editing DAW. |
| 12 | MusicRadar / Melodics "best beginner DAWs" | GarageBand wins beginners precisely because it hides depth behind a friendly surface. |

### Domain D — Piano-roll feature gold (FL Studio / Bitwig / Logic)
| # | Source | Core finding |
|---|--------|--------------|
| 13 | edmprod.com "25 FL Studio piano roll tips" | Stamps (chord presets), built-in arpeggiator (Alt+A), strum, scale highlighting = the killer features. |
| 14 | reddit r/Bitwig "what's lacking in piano roll" | Users compare against FL; built-in arp + scale snap is the differentiator. |
| 15 | Instagram/YouTube FL piano roll snapping | Auto-snap incoming MIDI to scale = "perfect in-key chords/melodies every time." |
| 16 | Ableton Scale device / Live 12 Keys-and-Scales FAQ | Native scale filtering so any chord/melody stays in key; intro tool for new artists. |
| 17 | Bitwig 6 scale highlighting polarit.me | Seeing/hearing scales in the roll helps both beginners and pros. |

### Domain E — Launchpad modes (Session / Performance / Custom)
| # | Source | Core finding |
|---|--------|--------------|
| 18 | Novation Pro MK3 session interface guide | Session view = clips (columns=tracks, rows=scenes); current 8×8 block lit, others dim. |
| 19 | Novation Components custom-mode editor | Custom Modes let YOU define what each pad does — standalone, no Ableton needed. |
| 20 | cdm.link FL performance mode alpha | Performance mode = trigger grid from playlist; single metaphor, no mode-switching confusion. |

### Domain F — Step sequencer / performance feedback design
| # | Source | Core finding |
|---|--------|--------------|
| 21 | Novation Pro MK3 sequencer guide | Each track has a color matched between selector + play area; 4 tracks play at once (Drum/Scale/Chromatic). |
| 22 | YouTube "Launchpad Pro step sequencer + Drum Machine" | Step sequencer on the grid is the fun, fast path to beats for non-musicians. |

### Domain G — Grid instrument philosophy (monome / norns)
| # | Source | Core finding |
|---|--------|--------------|
| 23 | monome.org "the grid does nothing on its own" | Best grid UX = the instrument is a blank slate YOU define; tactile, immediate, no menus. |
| 24 | norns.community / miker2049 midigrid | Open-source grid shims (C/Lua) prove a Launchpad→instrument mapping is small, well-bounded work. |

### Domain H — HCI / cognitive load (the "easy & fun" mandate)
| # | Source | Core finding |
|---|--------|--------------|
| 25 | Müller et al / Sweller cognitive-load UI research (Sage, arxiv 2402.11820) | Reducing EXTRANEOUS load (menus/modals physically separated from the task) frees working memory for the actual music. Good help + one-surface design lowers load. |

---

## Convergent truth

> **Beginners fail at Launchpad/DAW not because music is hard, but because the
> surface force-loads two disconnected metaphors (Session vs Arrangement,
> controller vs software) onto a tiny screen. The fix is ONE owned surface:
> our class-compliant C11 Mk2 driver + a tabbed UI where the Launchpad grid
> and the on-screen piano roll are the SAME editable object — scale-locked,
> color-coded, and free of menu-diving. Reduce extraneous cognitive load to
> near zero; let the grid be a blank, immediate instrument we define.**

Every proposed feature below attacks that convergence point.

---

## What this means for Big Mac (applied decisions)

1. **Own the driver, C11, zero third-party.** Mk2 is class-compliant → CoreMIDI
   already owns it (sources 6–8). We extend `wb_midi_coremidi.c` with an
   `wb_launchpad_mk2_*` family: correct note map (`11+col+row*10`), correct
   RGB SysEx (`F0 00 20 29 02 18 0B n r g b F7`), top-row 91–98, all in our
   code. No Novation Components, no Ableton, no lib.
2. **Tabbed / organized menu** switching between:
   - *Keyboard Piano Roll* (existing mouse editor)
   - *Launchpad Mk2 Piano Roll* (the 8×8 grid IS the roll; same clip model)
   - *Step Sequencer* (Drum/Scale/Chromatic, color-coded per track)
   - *Session* (clip launcher: columns=tracks, rows=scenes, lit/dim block)
3. **Scale lock everywhere** (sources 13–17): pick a scale once, every pad/note
   in both rolls snaps in-key; show the scale as colored LEDs on the Mk2.
4. **Color = meaning, not decoration** (sources 21, 25): playhead=white,
   loop=amber, clip-on=green, selected=blue, out-of-scale=dim/off. RGB lets us
   encode state so the user never opens a menu to know what's happening.
5. **One editable object** (sources 18, 20, 23): clicking a pad in the Mk2 roll
   edits the same `wb_clip` the mouse edits. No metaphor gap.

See `R007-launchpad-mk2-driver-and-tabs.md` for the concrete API + UI spec.
