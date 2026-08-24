# Big Mac DAW — Accessibility Conformance Statement (VPAT-style)

G61 — an honest self-assessment against Section 508 / EN 301 549 criteria,
in the spirit of a VPAT. Status vocabulary: **Supports** / **Partially
supports** / **Does not support**.

## 1. Success criteria

| Criterion | Requirement | Status | Notes |
|---|---|---|---|
| 2.4.7 Focus visibility | Visible focus indicator on operable controls | Partially supports | Clicked buttons highlight via the active state; no keyboard focus ring yet. Tracked as future work. |
| 2.3.1 Three flashes / motion safety | No content flashing >3 Hz; motion can be reduced | Supports | `WB_REDUCED_MOTION=1` disables pad flashes; meters use smooth ballistic decay, no strobe. |
| Keyboard operability (508 §402) | All core functions reachable by keyboard | Partially supports | Transport (space/JKL), BPM, save/new/template, prefs, MIDI learn, AX dump are keyboard-reachable; clip/step grid editing still mouse-first. |
| Labels/names for controls (508 §402 / EN 9.1.1.1 equivalent for UI) | Controls expose names to assistive tech | Partially supports | Every button registers its label + rect in the accessibility registry (`Ctrl+Shift+A` dumps `/tmp/bigmac_ax.json`). Native macOS AX-tree bridging is not wired into SDL's window yet. |
| Contrast (WCAG AA) | Text contrast >= 4.5:1 | Supports | Verified programmatically in the self-test gate (R029). |

## 2. Screen-reader integration

The label registry is the semantic source of truth: every interactive control
registers `(id, label, rect, state)` each frame. `Ctrl+Shift+A` writes
`/tmp/bigmac_ax.json`, which assistive tooling (and tests) consume. A native
NSAccessibility bridge that publishes these entries as real AX elements is the
next step; the registry was designed so the bridge needs only the dump logic,
no per-control rework.

## 3. Customizable shortcuts

`/tmp/bigmac_keys.txt` remaps core actions (`save=…`, `new=…`) at startup.
Full coverage of every binding is tracked under G50 follow-up.

## 4. Known gaps

- No screen-reader speech output inside the app itself; the JSON map is the
  current contract.
- Timeline/arrangement objects (clips, notes) have no non-visual editing path.
- No OS-level high-contrast theme switch.

*Statement generated for R073 gap ledger; update alongside any G49/G60 work.*
