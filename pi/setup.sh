#!/usr/bin/env bash
# setup.sh -- install the noise machine. Run on the Pi as root.
#
#   sudo bash pi/setup.sh
#
# Idempotent: safe to re-run after editing the source or the config.
set -euo pipefail

[ "$(id -u)" -eq 0 ] || { echo "error: run as root (sudo bash pi/setup.sh)" >&2; exit 1; }

REPO="$(cd "$(dirname "$0")/.." && pwd)"
step() { printf '\n== %s\n' "$*"; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
step "Packages"
# gcc ships with Raspberry Pi OS; only the ALSA headers are missing. pkg-config
# is what the Makefile uses to decide whether to build the ALSA backend at all.
apt-get update -qq
apt-get install -y -qq libasound2-dev pkg-config alsa-utils

# ---------------------------------------------------------------------------
step "Build"
make -C "$REPO" clean >/dev/null
make -C "$REPO"
[ -x "$REPO/bin/noise" ] || die "build produced no binary"

# The Makefile compiles the ALSA backend only when pkg-config finds alsa, and
# warns rather than failing if it does not. A binary built without it renders
# files happily and never plays a sound -- so prove the backend is in there,
# instead of discovering it at 3 a.m. Pointed at a device that cannot exist,
# the ALSA build retries; the fallback stub says so and exits.
if timeout 3 "$REPO/bin/noise" --device __no_such_device__ 2>&1 \
     | grep -q 'built without the ALSA backend'; then
    die "built without ALSA support: libasound2-dev or pkg-config is missing"
fi
install -m 0755 "$REPO/bin/noise"   /usr/local/bin/noise
install -m 0755 "$REPO/bin/analyze" /usr/local/bin/noise-analyze
echo "installed /usr/local/bin/noise and /usr/local/bin/noise-analyze"

# ---------------------------------------------------------------------------
step "Sound card"
# Address the card by NAME, never by index. Indices depend on module probe
# order, and with the KMS stack an HDMI card can enumerate ahead of the jack.
CARD="$(aplay -l 2>/dev/null | sed -n 's/^card [0-9]*: \([A-Za-z0-9_]*\) \[.*/\1/p' \
        | grep -iv 'hdmi\|vc4' | head -1 || true)"
[ -n "$CARD" ] || die "no non-HDMI playback card found. Check: aplay -l"
echo "using card '$CARD'"

# Prove the mixer control exists and accepts 0 dB BEFORE relying on it at boot.
amixer -c "$CARD" sset PCM 0dB unmute >/dev/null \
  || die "card '$CARD' has no 'PCM' control that accepts 0dB. Check: amixer -c $CARD scontrols"
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
noise --config /etc/noise-machine.conf --print-config >/dev/null \
  || die "the config file is not parseable"

# ---------------------------------------------------------------------------
step "Units"
sed "s/CARD_NAME/$CARD/" "$REPO/pi/noise-mixer.service" > /etc/systemd/system/noise-mixer.service
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
    if systemctl is-enabled "$t" >/dev/null 2>&1; then
        systemctl disable --now "$t" >/dev/null 2>&1 && echo "disabled $t"
    fi
done

# ---------------------------------------------------------------------------
step "Start"
systemctl restart noise-mixer.service
systemctl restart noise-machine.service
sleep 3

# ---------------------------------------------------------------------------
step "Verify"
ACTIVE="$(systemctl is-active noise-machine || true)"
echo "service         : $ACTIVE"
[ "$ACTIVE" = "active" ] || { journalctl -u noise-machine -n 30 --no-pager; die "service did not come up"; }

STATE="$(sed -n 's/^state: //p' /proc/asound/card*/pcm0p/sub0/status 2>/dev/null | head -1)"
echo "pcm state       : ${STATE:-unknown}"
[ "$STATE" = "RUNNING" ] || echo "  WARNING: expected RUNNING -- the stream is not actually playing"

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
