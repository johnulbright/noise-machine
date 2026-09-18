#!/usr/bin/env bash
# setup.sh -- install the noise machine. Run on the Pi as root.
#
#   sudo bash pi/setup.sh
#
# Idempotent: safe to re-run after editing the source or the config.
set -euo pipefail

[ "$(id -u)" -eq 0 ] || { echo "error: run as root (sudo bash pi/setup.sh)" >&2; exit 1; }

# sudo does not reliably hand a script a usable PATH -- depending on how
# secure_path and env_reset are configured it may pass through the invoking
# user's login PATH, which typically omits /usr/sbin entirely. That is where
# useradd and usermod live, so inheriting it breaks this script in two separate
# places. Set it explicitly instead of hoping.
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

REPO="$(cd "$(dirname "$0")/.." && pwd)"
step() { printf '\n== %s\n' "$*"; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

# This installs a systemd service and an ALSA device. Running it on the machine
# you developed on is an easy mistake and the failure it produces otherwise
# ("apt-get: command not found", eleven lines in) does not point at the cause.
[ "$(uname -s)" = "Linux" ] || die "this runs on the Raspberry Pi, not on your
       development machine -- uname reports $(uname -s). Copy the source over and
       run it there:  ssh pi@<pi-address>  then  sudo bash ~/noise-machine/pi/setup.sh"

# Name everything that is missing at once, rather than dying a third of the way
# through a part-finished install. pkg-config, aplay and amixer are excluded:
# apt-get installs those below.
missing=
for c in apt-get make gcc install chown stat systemctl useradd usermod \
         sed grep timeout; do
    command -v "$c" >/dev/null 2>&1 || missing="$missing $c"
done
[ -z "$missing" ] || die "required commands not on PATH:$missing (PATH=$PATH)"

# ---------------------------------------------------------------------------
step "Packages"
# gcc ships with Raspberry Pi OS; only the ALSA headers are missing. pkg-config
# is what the Makefile uses to decide whether to build the ALSA backend at all.
apt-get -q update
apt-get install -y -q libasound2-dev pkg-config alsa-utils

# ---------------------------------------------------------------------------
step "Build"
make -C "$REPO" clean >/dev/null
make -C "$REPO"
[ -x "$REPO/bin/noise" ] || die "build produced no binary"
# make ran under sudo, so bin/ is root-owned and the invoking user can no
# longer rebuild in their own checkout. Hand it back.
chown -R "$(stat -c '%u:%g' "$REPO")" "$REPO/bin" 2>/dev/null || true

# A binary built without the ALSA backend renders files happily and never plays
# a sound, so prove the backend is in there rather than discovering it at 3 a.m.
# Pointed at a device that cannot exist, the ALSA build retries forever; the
# fallback stub prints its excuse and exits.
#
# Captured into a variable rather than piped into grep, deliberately. Under
# `set -o pipefail` the pipeline's status is the rightmost NON-ZERO exit, and
# the stub exits 2 while grep exits 0 on a match -- so `if ... | grep -q ...`
# evaluates the whole pipeline as 2, the `if` is false, and the guard never
# fires in either direction. Tested: it silently passed a non-ALSA build.
alsa_probe="$(timeout 10 "$REPO/bin/noise" --device __no_such_device__ 2>&1 || true)"
case "$alsa_probe" in
    *'built without the ALSA backend'*)
        die "built without ALSA support: libasound2-dev or pkg-config is missing" ;;
esac
install -m 0755 "$REPO/bin/noise"   /usr/local/bin/noise
install -m 0755 "$REPO/bin/analyze" /usr/local/bin/noise-analyze
echo "installed /usr/local/bin/noise and /usr/local/bin/noise-analyze"

# ---------------------------------------------------------------------------
step "Sound card"
# Address the card by NAME, never by index. Indices depend on module probe
# order, and with the KMS stack an HDMI card can enumerate ahead of the jack.
# The character class must include '-' and '.': an I2S HAT or USB DAC often has
# a hyphen in its ALSA id, and a class that cannot span it makes the whole line
# fail to match, so the card silently vanishes from the candidate list rather
# than being captured partially.
CARD="$(aplay -l 2>/dev/null | sed -n 's/^card [0-9]*: \([A-Za-z0-9_.-]*\) \[.*/\1/p' \
        | grep -iv 'hdmi\|vc4' | head -1 || true)"
[ -n "$CARD" ] || die "no non-HDMI playback card found. Check: aplay -l"
echo "using card '$CARD'"

# Prove the mixer control exists and accepts 0 dB BEFORE relying on it at boot.
if ! amixer -c "$CARD" sset PCM 0dB >/dev/null 2>&1; then
    echo "  could not set PCM to 0dB on card '$CARD'." >&2
    echo "  Available controls:" >&2
    amixer -c "$CARD" scontrols >&2 || true
    echo "  Ranges:" >&2
    amixer -c "$CARD" sget PCM >&2 || true
    echo "" >&2
    echo "  If the control is named something else, set 'device' in noise.conf and fix" >&2
    echo "  the ExecStart lines in pi/noise-mixer.service to match. If it accepts only" >&2
    echo "  percentages, work out the percent that equals 0 dB from the dB range above" >&2
    echo "  -- do NOT just use 100%, which on bcm2835 is +4 dB and clips." >&2
    die "mixer could not be set to a known-safe level"
fi
# Separately, and tolerantly: not every control has a mute switch.
amixer -c "$CARD" sset PCM unmute >/dev/null 2>&1 || true
echo "mixer PCM set to 0 dB (not 100%, which on bcm2835 is +4 dB and clips)"

# ---------------------------------------------------------------------------
step "Service account"
if ! id -u noise >/dev/null 2>&1; then
    useradd --system --no-create-home --shell /usr/sbin/nologin --groups audio noise
    echo "created user 'noise'"
else
    usermod -aG audio noise
    echo "user 'noise' already present"
fi
# NOTE: the previous setup.sh ran `chown -R pi:audio` under `set -e`, which
# aborts the entire install on any Pi whose user is not literally named 'pi'.
# Nothing here assumes a username.

# ---------------------------------------------------------------------------
step "Configuration"
if [ -f /etc/noise-machine.conf ]; then
    echo "/etc/noise-machine.conf exists, leaving your edits alone"
    echo "  (compare against $REPO/noise.conf for new settings)"
else
    install -m 0644 "$REPO/noise.conf" /etc/noise-machine.conf
    echo "installed /etc/noise-machine.conf"
fi
cfg_complaints="$(/usr/local/bin/noise --config /etc/noise-machine.conf \
                    --print-config 2>&1 >/dev/null || true)"
# --print-config exits 0 even for a typo'd key or `rate = banana`, so the
# only signal is what it wrote to stderr.
[ -z "$cfg_complaints" ] || { printf '%s\n' "$cfg_complaints" >&2
    die "/etc/noise-machine.conf was not accepted cleanly (see above)"; }

# ---------------------------------------------------------------------------
step "Units"
sed "/^ExecStart=/s/CARD_NAME/$CARD/g" "$REPO/pi/noise-mixer.service" > /etc/systemd/system/noise-mixer.service
install -m 0644 "$REPO/pi/noise-machine.service" /etc/systemd/system/noise-machine.service
chmod 0644 /etc/systemd/system/noise-mixer.service
systemctl daemon-reload
systemctl enable noise-mixer.service noise-machine.service >/dev/null
echo "enabled noise-mixer.service and noise-machine.service"

# Remove anything the old build left behind that would now shadow our settings.
# A user ~/.asoundrc takes precedence over /etc/asound.conf, and the old
# setup.sh's `raspi-config nonint do_audio 1` wrote /root/.asoundrc pinned to
# card index 1 -- which on this Pi is HDMI. Rename rather than delete, so the
# previous state is recoverable if any of this turns out to be wrong.
for stale in /etc/asound.conf /root/.asoundrc; do
    if [ -e "$stale" ]; then
        mv "$stale" "$stale.replaced-by-noise-machine"
        echo "moved aside $stale (it would shadow our device selection)"
    fi
done
if [ -d /opt/noise-machine ]; then
    echo "found the old /opt/noise-machine (50 MB WAV and friends); it is no longer used."
    echo "  remove it yourself when you are happy: sudo rm -rf /opt/noise-machine"
fi

# ---------------------------------------------------------------------------
step "Quieting periodic work"
# Anything that wakes up and touches the SD card can stall the audio thread.
# Persistent=true is the sharp edge: a timer whose scheduled run was missed
# while the box was unplugged fires immediately at the next boot, which is
# exactly when audio is starting.
#
# zram is deliberately left alone. The old script ran `dphys-swapfile swapoff`,
# but this image uses zram (see rpi-zram-writeback.timer) -- RAM-backed, no SD
# wear -- and removing swap entirely from a 1 GB box makes reclaim stalls worse,
# not better.
for t in systemd-tmpfiles-clean.timer man-db.timer apt-daily.timer \
         apt-daily-upgrade.timer dpkg-db-backup.timer e2scrub_all.timer \
         fstrim.timer logrotate.timer; do
    state="$(systemctl is-enabled "$t" 2>/dev/null || true)"
    case "$state" in
        enabled|enabled-runtime)
            systemctl disable --now "$t" >/dev/null 2>&1 && echo "disabled $t" ;;
        static)
            # No [Install] section, so `disable` is a no-op that still reports
            # success. Masking is the only thing that actually stops it.
            systemctl mask --now "$t" >/dev/null 2>&1 && echo "masked $t (static)" ;;
        *) ;;
    esac
done

# ---------------------------------------------------------------------------
step "Start"
systemctl restart noise-mixer.service \
  || die "noise-mixer.service failed -- see: systemctl status noise-mixer.service"
# Deliberately not fatal: Type=notify means a start failure exits non-zero, and
# under `set -e` that would kill the script before the Verify block below could
# dump the journal explaining why.
systemctl restart noise-machine.service \
  || echo "  restart reported failure -- continuing to Verify for the journal" >&2
sleep 3

# ---------------------------------------------------------------------------
step "Verify"
ACTIVE="$(systemctl is-active noise-machine || true)"
echo "service         : $ACTIVE"
[ "$ACTIVE" = "active" ] || { journalctl -u noise-machine -n 30 --no-pager; die "service did not come up"; }

# /proc/asound has a symlink per card NAME, so this reads the card we actually
# chose. A card* glob could report the HDMI device instead.
# sed exits 2 if the file is absent; pipefail would propagate that and `set -e`
# would kill the script mid-report. Also globbed across substreams: bcm2835
# exposes 8, and the stream may not land on sub0.
STATE="$(sed -n 's/^state: //p' /proc/asound/"$CARD"/pcm0p/sub*/status 2>/dev/null \
         | sort -u | paste -sd, - || true)"
echo "pcm state       : ${STATE:-unknown}"
case "$STATE" in
    *RUNNING*) ;;
    *) echo "  WARNING: expected RUNNING -- the stream is not actually playing" ;;
esac

echo "mixer           : $(amixer -c "$CARD" sget PCM | sed -n 's/.*\[\(-\?[0-9.]*dB\)\].*/\1/p' | head -1)"
echo "scheduler       : $(journalctl -u noise-machine -n 40 --no-pager | sed -n 's/.*scheduler \(.*\)/\1/p' | tail -1)"
echo "buffer          : $(journalctl -u noise-machine -n 40 --no-pager | sed -n 's/.*\(period [0-9]* *buffer .*\)/\1/p' | tail -1)"
echo "power           : $(vcgencmd get_throttled) $(vcgencmd measure_temp)"

cat <<'EOF'

Done. Audio should be playing now; turn the pot on the amp board for volume.

  journalctl -u noise-machine -f        watch it, including hourly xrun counts
  systemctl restart noise-machine       apply an edit to /etc/noise-machine.conf
  noise --config /etc/noise-machine.conf --print-config

If `power` above reports anything other than throttled=0x0, fix that before
judging the sound: under-voltage destabilises SD I/O and couples switching
noise from the amp into the Pi's analog output stage.
EOF
