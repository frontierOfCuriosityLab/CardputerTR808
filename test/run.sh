#!/bin/sh
# Build and run the host-side engine tests.
# Usage: [TR808_SAMPLE_RATE=22050] test/run.sh [wav out.wav]     (the wav has every voice + the four demo grooves, for listening)
set -e
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}/tr808_host_test"
c++ -std=gnu++17 -O2 -Wall -Wextra -Wno-unused-function -Wno-unused-const-variable ${TR808_SAMPLE_RATE:+-DTR808_SAMPLE_RATE=$TR808_SAMPLE_RATE} -I.. host_test.cpp -o "$OUT"
"$OUT" "$@"
