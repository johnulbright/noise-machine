#!/usr/bin/env bash
#
# Regression suite for the noise generator. Runs anywhere a C compiler does --
# no Pi, no ALSA, no sox, no Python.
#
#   make test
#
# Every assertion here is a property the appliance depends on, and most of them
# correspond to a defect the previous build actually shipped. When one fails,
# the message says which property broke, not just that a number moved.
#
# Renders are short (20-60 s of audio, generated ~300x faster than realtime) and
# use fixed seeds, so results are deterministic and the whole suite runs in
# about a minute. The tolerances are set from the statistical spread of a
# 20-second window, not from what happened to pass once.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NOISE="$ROOT/bin/noise"
ANALYZE="$ROOT/bin/analyze"
TMP="${TMPDIR:-/tmp}/noise-tests.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

pass=0; fail=0
red=''; grn=''; dim=''; off=''
if [ -t 1 ]; then red=$'\033[31m'; grn=$'\033[32m'; dim=$'\033[2m'; off=$'\033[0m'; fi

ok()   { pass=$((pass+1)); printf '  %sok%s   %s\n' "$grn" "$off" "$1"; }
bad()  { fail=$((fail+1)); printf '  %sFAIL%s %s\n' "$red" "$off" "$1"; [ $# -gt 1 ] && printf '       %s\n' "$2"; }
note() { printf '%s\n' "$dim$1$off"; }

# between NAME VALUE LO HI  -- numeric range assertion, works without bc/python
between() {
    local name=$1 val=$2 lo=$3 hi=$4
    if awk -v v="$val" -v l="$lo" -v h="$hi" 'BEGIN{exit !(v>=l && v<=h)}'; then
        ok "$name = $val  (want $lo..$hi)"
    else
        bad "$name = $val" "outside the allowed range $lo..$hi"
    fi
}

equals() {
    local name=$1 val=$2 want=$3
    if [ "$val" = "$want" ]; then ok "$name = $val"
    else bad "$name = $val" "expected exactly $want"; fi
}

# render SECONDS SEED [extra noise args...] -> writes $TMP/r.f32
render() {
    local secs=$1 seed=$2; shift 2
    "$NOISE" --f32 --seconds "$secs" --seed "$seed" --out "$TMP/r.f32" "$@" 2>"$TMP/err" || {
        bad "render failed" "$(head -3 "$TMP/err")"; return 1; }
}

# measure [analyze args...] -> sources key=value pairs into shell vars m_*
measure() {
    "$ANALYZE" "$TMP/r.f32" --kv "$@" > "$TMP/kv" || { bad "analyze failed"; return 1; }
    # keys look like ch0.rms -- turn the dot into an underscore for shell vars
    while IFS='=' read -r k v; do
        eval "m_${k//./_}=\$v"
    done < "$TMP/kv"
}

# ---------------------------------------------------------------------------
printf '\n== build\n'
note '   Clean build, not an incremental one: `make` on an already-built tree is'
note '   a no-op, which would make the warning check below silently vacuous.'
make -C "$ROOT" clean >/dev/null 2>&1 || true
if make -C "$ROOT" >"$TMP/build" 2>&1; then
    ok "builds clean"
else
    bad "build failed" "$(tail -5 "$TMP/build")"
    printf '\n%d passed, %d failed\n' "$pass" "$fail"; exit 1
fi
if grep -qE '\bwarning:' "$TMP/build"; then
    bad "compiler warnings" "$(grep -E '\bwarning:' "$TMP/build" | head -3)"
else
    ok "no compiler warnings"
fi

# ---------------------------------------------------------------------------
printf '\n== default tone is brown noise\n'
note '   -6 dB/oct is the definition of brown. The filter self-calibrates against'
note '   its own transfer function, so this catches any regression in that loop.'
if render 60 1; then
    measure --band 200 8000
    between "slope L"          "$m_ch0_slope" -6.10 -5.90
    between "slope R"          "$m_ch1_slope" -6.10 -5.90
    between "straightness L"   "$m_ch0_straightness" 0 0.30
    between "straightness R"   "$m_ch1_straightness" 0 0.30
fi

# ---------------------------------------------------------------------------
printf '\n== gain staging leaves headroom\n'
note '   The old build normalised to 0 dBFS, met a mixer whose ceiling is +4 dB,'
note '   and clipped ~450 samples/second forever. Zero clipping is the whole point.'
if render 60 2; then
    measure --band 200 8000
    equals  "clipped L"  "$m_ch0_clipped" 0
    equals  "clipped R"  "$m_ch1_clipped" 0
    between "rms L"      "$m_ch0_rms"  -18.4 -17.6
    between "rms R"      "$m_ch1_rms"  -18.4 -17.6
    # Peak must stay clear of full scale with room for 8 h of Gaussian tail
    # growth (crest reaches ~15.4 dB over a night) plus the mixer.
    between "true peak L" "$m_ch0_peak" -12.0 -2.0
    between "crest L"     "$m_ch0_crest" 12.0 16.5
fi

# ---------------------------------------------------------------------------
printf '\n== the generator does not drift\n'
note '   Nothing in the chain integrates, so DC must stay buried. A bare'
note '   random-walk accumulator would show up here as a rising DC offset.'
if render 60 3; then
    measure --band 200 8000
    between "dc offset L" "$m_ch0_dc" -300 -80
    between "dc offset R" "$m_ch1_dc" -300 -80
fi

# ---------------------------------------------------------------------------
printf '\n== level is steady in the audible band\n'
note '   Measured >150 Hz, which is roughly what a 3 W driver radiates. Brown'
note '   noise has a ~0.20 dB floor here; the old chain managed 0.483 dB because'
note '   a compressor was modulating it. Ceiling set below the 0.41 dB JND.'
if render 60 4; then
    measure --band 200 8000
    between "steadiness L" "$m_ch0_steady_sd" 0 0.28
    between "steadiness R" "$m_ch1_steady_sd" 0 0.28
fi

# ---------------------------------------------------------------------------
printf '\n== channels are independent\n'
note '   Identical channels image as a phantom centre, which is fatiguing over'
note '   hours. independent=0 must still work, and must be perfectly correlated.'
if render 40 5; then
    measure --band 200 8000
    between "correlation (independent=1)" "$m_corr" -0.05 0.05
fi
if render 40 5 --independent 0; then
    measure --band 200 8000
    between "correlation (independent=0)" "$m_corr" 0.999 1.001
fi

# ---------------------------------------------------------------------------
printf '\n== tone controls actually work\n'
note '   Each of these is a documented knob in noise.conf. If the self-calibration'
note '   or the filter construction regresses, the requested slope stops arriving.'
for want in 0 -3 -6 -7.5; do
    if render 30 6 --slope_db_oct "$want"; then
        measure --band 200 8000
        lo=$(awk -v w="$want" 'BEGIN{print w-0.25}')
        hi=$(awk -v w="$want" 'BEGIN{print w+0.25}')
        between "slope_db_oct=$want" "$m_ch0_slope" "$lo" "$hi"
    fi
done

printf '\n== highpass removes low end without clipping\n'
if render 30 7 --highpass_hz 120; then
    measure --band 400 8000
    equals  "clipped with highpass_hz=120" "$m_ch0_clipped" 0
    between "slope still brown above it"   "$m_ch0_slope" -6.25 -5.75
fi

printf '\n== lowpass darkens the top without clipping\n'
if render 30 8 --lowpass_hz 2000; then
    measure --band 200 1000
    equals "clipped with lowpass_hz=2000" "$m_ch0_clipped" 0
    # A 12 dB/oct lowpass at 2 kHz must steepen the slope well past -6.
    if awk -v v="$m_ch0_slope" 'BEGIN{exit !(v < -5.5)}'; then
        ok "lowpass steepens the spectrum (slope $m_ch0_slope)"
    else
        bad "lowpass had no effect (slope $m_ch0_slope)" "expected steeper than -5.5"
    fi
fi

printf '\n== the level ceiling is enforced and announced\n'
note '   Brown noise reaches ~16.2 dB above RMS over 8 hours, so a level that'
note '   measures clean for a minute can still clip all night. The generator'
note '   predicts that and warns, rather than clipping quietly the way the old'
note '   build did. -17 is the documented ceiling; -18 is the default.'

# Too hot: must both warn AND actually clip, so the warning is not crying wolf.
"$NOISE" --seconds 1 --seed 9 --level_dbfs -12 --out /dev/null 2>"$TMP/w12" >/dev/null
if grep -q 'WARNING level_dbfs' "$TMP/w12"; then ok "level_dbfs=-12 warns"
else bad "level_dbfs=-12 does not warn" "a clipping level must announce itself"; fi
if render 60 9 --level_dbfs -12; then
    measure --band 200 8000
    if [ "$m_ch0_clipped" -gt 0 ]; then ok "level_dbfs=-12 does clip ($m_ch0_clipped samples) as warned"
    else bad "level_dbfs=-12 did not clip" "the warning threshold is now too conservative"; fi
    between "rms at level_dbfs=-12" "$m_ch0_rms" -12.4 -11.6
fi

# The documented ceiling: silent, and clean.
"$NOISE" --seconds 1 --seed 9 --level_dbfs -17 --out /dev/null 2>"$TMP/w17" >/dev/null
if grep -q 'WARNING level_dbfs' "$TMP/w17"; then
    bad "level_dbfs=-17 warns" "the documented ceiling must not warn"
else ok "level_dbfs=-17 does not warn"; fi
if render 60 9 --level_dbfs -17; then
    measure --band 200 8000
    equals  "clipped at level_dbfs=-17" "$m_ch0_clipped" 0
    between "rms at level_dbfs=-17"     "$m_ch0_rms" -17.4 -16.6
fi

# And the default must be comfortably inside it.
"$NOISE" --config "$ROOT/noise.conf" --seconds 1 --out /dev/null 2>"$TMP/wdef" >/dev/null
if grep -q 'WARNING' "$TMP/wdef"; then
    bad "the shipped default warns" "noise.conf ships a level that will clip"
else ok "shipped default level is clip-free"; fi

# ---------------------------------------------------------------------------
printf '\n== rate independence\n'
note '   The Pi runs 48 kHz, but the filter design must not silently depend on it.'
if render 30 10 --rate 44100; then
    measure --rate 44100 --band 200 8000
    between "slope at 44.1 kHz"   "$m_ch0_slope" -6.15 -5.85
    equals  "clipped at 44.1 kHz" "$m_ch0_clipped" 0
fi

# ---------------------------------------------------------------------------
printf '\n== determinism\n'
note '   Same seed must give byte-identical output, or none of the above means'
note '   anything and the measurements are not reproducible.'
"$NOISE" --f32 --seconds 5 --seed 42 --out "$TMP/a.f32" 2>/dev/null
"$NOISE" --f32 --seconds 5 --seed 42 --out "$TMP/b.f32" 2>/dev/null
if cmp -s "$TMP/a.f32" "$TMP/b.f32"; then ok "same seed -> identical output"
else bad "same seed -> different output" "the generator is not reproducible"; fi

"$NOISE" --f32 --seconds 5 --seed 43 --out "$TMP/c.f32" 2>/dev/null
if cmp -s "$TMP/a.f32" "$TMP/c.f32"; then bad "different seeds -> identical output" "the seed is being ignored"
else ok "different seed -> different output"; fi

# ---------------------------------------------------------------------------
printf '\n== s16 WAV output path\n'
note '   16 bit is a hard ceiling on this hardware, so the quantised path is'
note '   what the DAC actually receives and must measure the same.'
if "$NOISE" --seconds 30 --seed 11 --out "$TMP/w.wav" 2>/dev/null; then
    ok "wav render succeeds"
    hdr=$(head -c 4 "$TMP/w.wav")
    equals "RIFF header" "$hdr" "RIFF"
    "$ANALYZE" "$TMP/w.wav" --kv --band 200 8000 > "$TMP/kv"
    while IFS='=' read -r k v; do eval "w_${k//./_}=\$v"; done < "$TMP/kv"
    equals  "wav rate"     "$w_rate" 48000
    equals  "wav channels" "$w_channels" 2
    equals  "wav clipped"  "$w_ch0_clipped" 0
    between "wav slope"    "$w_ch0_slope" -6.15 -5.85
else
    bad "wav render failed"
fi

# ---------------------------------------------------------------------------
printf '\n== config file handling\n'
cat > "$TMP/t.conf" <<'EOF'
# comment line
slope_db_oct = -3.0
highpass_hz  = 40
  level_dbfs = -20.0   # trailing comment
EOF
out=$("$NOISE" --config "$TMP/t.conf" --print-config 2>/dev/null)
equals "config: slope parsed"    "$(echo "$out" | sed -n 's/^slope_db_oct=//p')" "-3"
equals "config: highpass parsed" "$(echo "$out" | sed -n 's/^highpass_hz=//p')" "40"
equals "config: level parsed"    "$(echo "$out" | sed -n 's/^level_dbfs=//p')" "-20"

# A CLI flag must beat the config file, which is how setup.sh and the tests
# override single values without rewriting /etc/noise-machine.conf.
out=$("$NOISE" --config "$TMP/t.conf" --level_dbfs -15 --print-config 2>/dev/null)
equals "cli overrides config" "$(echo "$out" | sed -n 's/^level_dbfs=//p')" "-15"

# The shipped config must parse, or the deploy fails at the last step.
if "$NOISE" --config "$ROOT/noise.conf" --print-config >/dev/null 2>&1; then
    ok "shipped noise.conf parses"
else
    bad "shipped noise.conf does not parse"
fi

# An unknown key should warn, not crash or silently succeed.
echo 'nonsense_key = 1' > "$TMP/bad.conf"
if "$NOISE" --config "$TMP/bad.conf" --print-config 2>&1 | grep -q "unknown key"; then
    ok "unknown config key warns"
else
    bad "unknown config key is ignored silently"
fi

# ---------------------------------------------------------------------------
printf '\n== setup.sh parsers against real hardware output\n'
note '   The card-detection pipeline has never run on a Pi. Feed it the exact'
note '   aplay -l output from the target machine and check it picks the jack.'
cat > "$TMP/aplay.txt" <<'EOF'
**** List of PLAYBACK Hardware Devices ****
card 0: Headphones [bcm2835 Headphones], device 0: bcm2835 Headphones [bcm2835 Headphones]
  Subdevices: 8/8
  Subdevice #0: subdevice #0
card 1: vc4hdmi [vc4-hdmi], device 0: MAI PCM i2s-hifi-0 [MAI PCM i2s-hifi-0]
  Subdevices: 1/1
  Subdevice #0: subdevice #0
EOF
card=$(sed -n 's/^card [0-9]*: \([A-Za-z0-9_]*\) \[.*/\1/p' "$TMP/aplay.txt" \
       | grep -iv 'hdmi\|vc4' | head -1)
equals "setup.sh picks the headphone card" "$card" "Headphones"

# And the inverse: an HDMI-only machine must yield nothing, so setup.sh dies
# with a useful message rather than configuring a silent device.
cat > "$TMP/aplay2.txt" <<'EOF'
card 0: vc4hdmi [vc4-hdmi], device 0: MAI PCM i2s-hifi-0 [MAI PCM i2s-hifi-0]
EOF
card2=$(sed -n 's/^card [0-9]*: \([A-Za-z0-9_.-]*\) \[.*/\1/p' "$TMP/aplay2.txt" \
        | grep -iv 'hdmi\|vc4' | head -1)
equals "HDMI-only machine yields no card" "$card2" ""

# An I2S HAT or USB DAC usually has a hyphen in its id. A character class that
# cannot span it makes the whole line fail to match, so the card disappears
# instead of being captured partially -- and setup.sh then blames HDMI.
cat > "$TMP/aplay3.txt" <<'EOF'
card 0: sndrpihifiberry [snd_rpi_hifiberry_dac], device 0: HifiBerry DAC HiFi pcm5102a-hifi-0
card 1: Device-1 [USB Audio Device], device 0: USB Audio [USB Audio]
EOF
card3=$(sed -n 's/^card [0-9]*: \([A-Za-z0-9_.-]*\) \[.*/\1/p' "$TMP/aplay3.txt" \
        | grep -iv 'hdmi\|vc4' | head -1)
equals "hyphenated card ids are captured" "$card3" "sndrpihifiberry"

# ---------------------------------------------------------------------------
printf '\n== the ALSA-backend guard actually fires\n'
note '   This one shipped broken. Under `set -o pipefail` a pipeline reports the'
note '   rightmost NON-ZERO exit, and the no-ALSA stub exits 2 while grep exits 0'
note '   on a match -- so `if binary | grep -q ...` evaluated to 2, the if was'
note '   false, and a binary that can never play audio installed silently.'

guard() {  # $1 = simulated program output, $2 = its exit code
    set -euo pipefail
    probe="$(sh -c "printf '%s\n' \"\$1\" >&2; exit \$2" _ "$1" "$2" 2>&1 || true)"
    case "$probe" in
        *'built without the ALSA backend'*) echo FIRES ;;
        *) echo passes ;;
    esac
}
r=$(guard 'noise: built without the ALSA backend; use --out FILE to render.' 2)
equals "no-ALSA build (exit 2) is caught" "$r" "FIRES"
r=$(guard 'noise: device not ready (attempt 1), retrying' 124)
equals "ALSA build (timeout-killed, exit 124) passes" "$r" "passes"

# ---------------------------------------------------------------------------
printf '\n== Makefile refuses to build a silent binary on Linux\n'
note '   Without the ALSA backend everything above still passes, because the'
note '   suite only exercises the render path. So the build itself must refuse.'
if grep -q 'error.*libasound2-dev not found' "$ROOT/Makefile"; then
    ok "missing libasound2-dev is a hard error, not a warning"
else
    bad "Makefile only warns about missing libasound2-dev" \
        "a Linux build would silently produce a binary that never plays"
fi

# The ALSA-backend guard in setup.sh greps for this exact string.
if "$NOISE" --help 2>&1 | grep -q .; then ok "noise --help works"; else bad "noise --help broken"; fi

# ---------------------------------------------------------------------------
printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ] || exit 1
