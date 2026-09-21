#!/bin/sh
# Compile CardputerTR808.ino against host stubs, run the UI checks, and
# write every screen as an SVG (one file per screen) into the output directory.
# Usage: test/ui_preview.sh [outdir]
set -e
cd "$(dirname "$0")"
OUT="${1:-${TMPDIR:-/tmp}/tr808_ui}"
BIN="${TMPDIR:-/tmp}/tr808_ui_test"
c++ -std=gnu++17 -O1 -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-const-variable \
    -Istubs -I.. ui_preview.cpp -o "$BIN"
"$BIN" "$OUT"
