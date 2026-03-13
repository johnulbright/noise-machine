#!/usr/bin/env bash
# setup.sh -- Run on the Raspberry Pi as root to configure the noise machine.
# Usage: sudo bash setup.sh
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "Error: run as root (sudo bash setup.sh)"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=== Noise Machine Setup ==="
echo ""

# 1. Install packages
echo "[1/7] Installing packages..."
apt-get update -qq
apt-get install -y -qq mpv alsa-utils

# 2. Create application directory and copy audio file
echo "[2/7] Setting up /opt/noise-machine/..."
mkdir -p /opt/noise-machine

if [ -f "$SCRIPT_DIR/../brown-noise.wav" ]; then
    cp "$SCRIPT_DIR/../brown-noise.wav" /opt/noise-machine/
    echo "  Copied brown-noise.wav"
elif [ ! -f /opt/noise-machine/brown-noise.wav ]; then
    echo ""
    echo "  WARNING: brown-noise.wav not found!"
    echo "  Copy it from your Mac:"
    echo "    scp brown-noise.wav pi@$(hostname -I | awk '{print $1}'):/opt/noise-machine/"
    echo ""
fi

chown -R pi:audio /opt/noise-machine

# 3. Configure ALSA for 3.5mm headphone output
echo "[3/7] Configuring ALSA..."
CARD_NAME=$(aplay -l 2>/dev/null | grep -oi 'Headphones\|bcm2835 Headphones\|Headphone' | head -1 || true)
if [ -z "$CARD_NAME" ]; then
    CARD_NAME=$(aplay -l 2>/dev/null | grep "^card" | head -1 | sed 's/card [0-9]*: \([^]]*\)\[.*/\1/' | xargs || echo "Headphones")
fi

cat > /etc/asound.conf << EOF
pcm.!default {
    type hw
    card ${CARD_NAME}
    device 0
}

ctl.!default {
    type hw
    card ${CARD_NAME}
}
EOF
echo "  ALSA card: ${CARD_NAME}"

# 4. Enable audio in boot config
echo "[4/7] Enabling audio output..."
CONFIG_FILE="/boot/config.txt"
[ -f "/boot/firmware/config.txt" ] && CONFIG_FILE="/boot/firmware/config.txt"

if ! grep -q "^dtparam=audio=on" "$CONFIG_FILE" 2>/dev/null; then
    echo "dtparam=audio=on" >> "$CONFIG_FILE"
fi

# Force audio to 3.5mm jack
raspi-config nonint do_audio 1 2>/dev/null || true

# Set ALSA volume to max -- the PAM8403's onboard potentiometer handles volume
amixer -c 0 set 'PCM' 100% unmute 2>/dev/null || \
amixer -c 0 set 'Headphone' 100% unmute 2>/dev/null || \
echo "  Could not set volume automatically. Check: amixer -c 0 scontrols"
alsactl store 2>/dev/null || true

# 5. Install systemd service
echo "[5/7] Installing systemd service..."
cp "$SCRIPT_DIR/noise-machine.service" /etc/systemd/system/
systemctl daemon-reload
systemctl enable noise-machine.service

# 6. Disable unnecessary services for faster boot
echo "[6/7] Optimizing boot time..."
systemctl disable bluetooth 2>/dev/null || true
systemctl disable NetworkManager-wait-online.service 2>/dev/null || true
systemctl disable ModemManager.service 2>/dev/null || true
systemctl disable cloud-init-main.service 2>/dev/null || true
systemctl disable cloud-init-local.service 2>/dev/null || true
systemctl disable cloud-final.service 2>/dev/null || true
systemctl disable cloud-config.service 2>/dev/null || true
systemctl disable apt-daily.timer 2>/dev/null || true
systemctl disable apt-daily-upgrade.timer 2>/dev/null || true
systemctl disable man-db.timer 2>/dev/null || true
systemctl disable e2scrub_all.timer 2>/dev/null || true

# 7. Disable swap (unnecessary for this appliance, reduces SD wear)
echo "[7/7] Disabling swap..."
dphys-swapfile swapoff 2>/dev/null || true
systemctl disable dphys-swapfile 2>/dev/null || true

echo ""
echo "=== Setup complete ==="
echo ""
echo "Reboot to start automatically, or run now:"
echo "  sudo systemctl start noise-machine"
echo ""
echo "To make the filesystem read-only (recommended after verifying everything works):"
echo "  sudo raspi-config nonint enable_overlayfs"
echo "  sudo raspi-config nonint enable_bootro"
echo "  sudo reboot"
