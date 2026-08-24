# G56 — IMF (SMPTE ST 2067) Packaging: Roadmap

IMF is the studio-standard delivery format for versioned masters. This
document records how Big Mac would adopt it, so the groundwork already in the
engine (delivery profiles, LUFS targets, EDL export, shadow-bin) maps onto a
concrete path. Status: **roadmap / not implemented**.

## Why IMF matters for this codebase

An IMF package is: an **OPL** (playing list, XML), one or more **CPLs**
(composition playlists — one per version), **track files** (MXF-wrapped
essences), and **PKL** (asset checksums). Our existing pieces line up:

| IMF concept | Big Mac equivalent today |
|---|---|
| CPL timeline | `wb_session` arrangement (tracks, clips, markers) |
| Marker/segment metadata | `wb_marker` sections (G82 chord-track style grid) |
| Versioned variants | Session copies + our delivery profiles (G55) |
| Audio loudness conformance | `wb_lufs` measurement + per-profile normalization |
| Supplemental packages | Shadow-bin sidecars (incremental assets) |

## Implementation phases

1. **CPL export** (smallest first step). Emit a valid CPL XML describing the
   video track as a sequence of resources from clip start/duration/source.
   The CMX3600 EDL writer (`wb_session_export_edl`) is the template; the FCPXML
   exporter proves we can emit time-accurate XML with color-intent metadata.
2. **Sidecar validation.** Verify loudness (already have `wb_lufs`), and
   peak levels against the delivery profile's TP ceiling before packaging.
3. **Track file wrapping.** MXF wrapping requires an external muxer pass;
   plan is ffmpeg `-f mxf` with our existing ffmpeg integration, keeping
   essence generation in-engine.
4. **PKL/OPL generation.** Pure XML + SHA-1 asset checksums — straightforward
   C11 once CPL exists.
5. **Supplemental packages.** Version deltas: reuse session-copy plus
   clip-level diffs to emit only changed track files.

## Non-goals until demand

- Full IMP validation tooling
- Encryption/KDM workflows (studio-only concern)

*Ledger reference: R072 G56. Revisit when a delivery partner requires IMF.*
