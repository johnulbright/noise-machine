#!/usr/bin/env bash
# generate-noise.sh -- Generate a seamlessly loopable brown noise WAV file.
# Run on macOS. Requires: brew install sox
set -euo pipefail

DURATION=310        # seconds (target 300 + 10 for crossfade)
CROSSFADE=10        # seconds of overlap for seamless loop
RATE=44100
BITS=16
CHANNELS=2
OUTPUT="brown-noise.wav"
TMPDIR=$(mktemp -d)

trap 'rm -rf "$TMPDIR"' EXIT

command -v sox >/dev/null 2>&1 || { echo "Error: sox not found. Install it: brew install sox"; exit 1; }

echo "Generating ${DURATION}s of brown noise..."
sox -n -r "$RATE" -b "$BITS" -c "$CHANNELS" "$TMPDIR/raw.wav" \
    synth "$DURATION" brownnoise vol 0.2 \
    highpass 10 \
    bass +10 60 \
    compand 0.02,0.5 -90,-90,-60,-50,-30,-25,-10,-8,0,-3 -3 \
    norm -1

TOTAL_SAMPLES=$(soxi -s "$TMPDIR/raw.wav")
XFADE_SAMPLES=$((CROSSFADE * RATE))
BODY_SAMPLES=$((TOTAL_SAMPLES - XFADE_SAMPLES))
MIDDLE_SAMPLES=$((BODY_SAMPLES - XFADE_SAMPLES))

echo "Creating crossfade for seamless loop..."

# Extract and fade the tail (last segment) -- quarter-sine (equal-power) fade-out
sox "$TMPDIR/raw.wav" "$TMPDIR/tail.wav" trim "${BODY_SAMPLES}s" "${XFADE_SAMPLES}s"
sox "$TMPDIR/tail.wav" "$TMPDIR/tail-faded.wav" fade q 0 "$CROSSFADE" "$CROSSFADE"

# Extract and fade the head (first segment) -- quarter-sine (equal-power) fade-in
sox "$TMPDIR/raw.wav" "$TMPDIR/head.wav" trim 0 "${XFADE_SAMPLES}s"
sox "$TMPDIR/head.wav" "$TMPDIR/head-faded.wav" fade q "$CROSSFADE" 0 0

# Mix at unity gain (sox -m auto-scales by 1/n; -v 1 overrides that)
sox -m -v 1 "$TMPDIR/tail-faded.wav" -v 1 "$TMPDIR/head-faded.wav" "$TMPDIR/crossfade.wav"

# Extract the untouched middle section
sox "$TMPDIR/raw.wav" "$TMPDIR/middle.wav" trim "${XFADE_SAMPLES}s" "${MIDDLE_SAMPLES}s"

# Assemble: crossfade region + middle
sox "$TMPDIR/crossfade.wav" "$TMPDIR/middle.wav" "$TMPDIR/assembled.wav"

# Normalize to 0 dBFS
sox "$TMPDIR/assembled.wav" "$OUTPUT" norm

FILE_SIZE=$(ls -lh "$OUTPUT" | awk '{print $5}')
FILE_DURATION=$(soxi -D "$OUTPUT" | cut -d. -f1)

echo ""
echo "Created: $OUTPUT (${FILE_SIZE}, ${FILE_DURATION}s)"
echo ""
echo "Verify the loop is seamless:"
echo "  play $OUTPUT repeat 3"
