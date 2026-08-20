#!/bin/bash
# bm_desktop.sh — turnkey desktop driver for the Big Mac DAW on macOS 11.
# Requires: Accessibility checkbox enabled for the app that runs this (Terminal/
# iTerm) OR for Big Mac DAW.app, in System Preferences -> Security & Privacy ->
# Accessibility. Once granted, this fully drives the DAW: launch, screenshot,
# click, type, and close. No ScreenCaptureKit (macOS 12.3+) needed.
#
# Usage:
#   bm_desktop.sh launch            # open the DAW .app
#   bm_desktop.sh shot [path.png]   # screencapture the main screen
#   bm_desktop.sh click X Y         # CGEvent click at coords
#   bm_desktop.sh type "text"       # CGEvent type
#   bm_desktop.sh key "a"           # CGEvent keystroke (or combo like "return")
#   bm_desktop.sh close             # quit the DAW
set -u
APP=/Users/waefrebeorn/Documents/big-mac/build/wb_daw.app
CLICK=/Users/waefrebeorn/homebrew/bin/cliclick
CMD="${1:-help}"; shift 2>/dev/null || true

case "$CMD" in
  launch)  open -a "$APP"; sleep 3; echo "launched" ;;
  shot)    OUT="${1:-/tmp/bm_desktop.png}"; screencapture -x "$OUT"; ls -la "$OUT" ;;
  click)   "$CLICK" c:"$1,$2" ;;
  type)    "$CLICK" t:"$1" ;;
  key)     "$CLICK" k:"$1" ;;
  close)   osascript -e 'tell application "Big Mac DAW" to quit' 2>/dev/null || pkill -f wb_daw; echo "closed" ;;
  *) echo "usage: bm_desktop.sh {launch|shot|click X Y|type T|key K|close}" ;;
esac
