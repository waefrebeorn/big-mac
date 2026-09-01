#!/bin/bash
# bigmac_status.sh — canonical source of truth for Big Mac counts.
# Run this instead of hardcoding numbers in docs/skills.
# Usage: bash tools/bigmac_status.sh

cd "$(dirname "$0")/.."

echo "=== Big Mac DAW — live counts ==="
echo ""

# Engine gate
if [ -f build/wb_selftest ]; then
    GATE=$(./build/wb_selftest 2>&1 | grep -oE '[0-9]+ checks, [0-9]+ failures' | tail -1)
    echo "engine_gate: $GATE"
else
    echo "engine_gate: (build/wb_selftest not built — run 'make')"
fi

# Source modules
echo "src_modules: $(ls src/wb_*.c 2>/dev/null | wc -l | tr -d ' ')"

# Test modules
echo "test_modules: $(ls tests/test_*.c 2>/dev/null | wc -l | tr -d ' ')"

# Tool modules
echo "tool_modules: $(ls tools/wb_*.c tools/ytp_*.c tools/wubusearch 2>/dev/null | wc -l | tr -d ' ')"

# Headers
echo "public_headers: $(ls include/wbus/*.h 2>/dev/null | wc -l | tr -d ' ')"

# Total LOC
echo "total_loc: $(cat src/*.c include/wbus/*.h 2>/dev/null | wc -l | tr -d ' ')"

# Gap ledger
if [ -f docs/R072-giant-gap-research.md ]; then
    TOTAL=$(grep -cE '^\[G[0-9]+\]' docs/R072-giant-gap-research.md)
    WIRED=$(grep -oE '^\[G[0-9]+\]' docs/R072-giant-gap-research.md | while read g; do grep -A1 "$g" docs/R072-giant-gap-research.md | grep -q WIRED && echo "$g"; done | wc -l | tr -d ' ')
    echo "gaps: $TOTAL total, $WIRED wired, $((TOTAL - WIRED)) open"
fi

# Git
echo "git_commits: $(git rev-list --count HEAD 2>/dev/null)"
echo "git_branch: $(git branch --show-current 2>/dev/null)"

# YTP assets
echo "ytp_videos: $(find assets/ytp_sources/video -name '*.mp4' 2>/dev/null | wc -l | tr -d ' ')"
echo "mocap_files: $(find assets/mocap -name '*.bvh' 2>/dev/null | wc -l | tr -d ' ')"
