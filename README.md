# noise-machine

A Raspberry Pi 3 standalone brown noise machine. Plays seamlessly looping brown noise
on boot with a hardware volume knob via a PAM8403 amplifier board.

## Bill of Materials

| Item | Notes |
|------|-------|
| Raspberry Pi 3 (any variant) | Running Raspberry Pi OS Lite (headless) |
| MicroSD card (8 GB+) | Class 10 or better |
| 5V/2.5A micro-USB power supply | Quality supply prevents audio glitches |
| PAM8403 amplifier board with potentiometer | ~$2, class-D, 2x3W, built-in volume knob |
| Passive speaker driver(s) | 4-8 ohm, 2-3W, small form factor |
| 3.5mm audio cable | Male-to-male (Pi jack to amp input) |
| 2 jumper wires (female-to-female) | For amp power (5V + GND) |

## Wiring

The PAM8403 board is powered from the Pi's 5V GPIO rail. Its built-in potentiometer
handles volume control -- no software or GPIO wiring needed for that.

```
PAM8403 + Pot       Raspberry Pi 3
-------------       ---------------------------------
VCC   ------------> Pin 2   (5V)
GND   ------------> Pin 6   (GND)
Audio input ------> 3.5mm headphone jack (via 3.5mm cable)
Speaker out ------> Passive speaker driver(s), 4-8 ohm
```

GPIO header reference:

```
              +-----+
     3.3V  1 [ ][ ] 2  5V ---------> PAM8403 VCC
              [ ][ ]
              [ ][ ] 6  GND -------> PAM8403 GND
              ...
              +-----+
```

Audio path: Pi 3.5mm jack --3.5mm cable--> PAM8403 input --> passive speaker(s).
The PAM8403 draws ~200-300mA at moderate volume, well within the Pi's 5V power budget.

## Setup

### 1. Generate the noise file (on your Mac)

```bash
brew install sox
./generate-noise.sh
```

This creates `brown-noise.wav` (~50 MB, ~5 minutes). Verify the loop is seamless:

```bash
play brown-noise.wav repeat 3
```

### 2. Flash Raspberry Pi OS Lite

Use the [Raspberry Pi Imager](https://www.raspberrypi.com/software/) to write
Raspberry Pi OS Lite (no desktop) to your microSD card. In the imager settings:
- Set hostname (e.g., `noise-machine`)
- Enable SSH
- Set username/password (default: `pi`)
- Configure Wi-Fi if needed for initial setup

### 3. Deploy to the Pi

```bash
# Copy the noise file
scp brown-noise.wav pi@noise-machine.local:/opt/noise-machine/

# Copy the setup files
scp -r pi/* pi@noise-machine.local:/tmp/noise-setup/
```

Or if `/opt/noise-machine` doesn't exist yet:

```bash
scp brown-noise.wav pi@noise-machine.local:/tmp/
ssh pi@noise-machine.local
sudo mkdir -p /opt/noise-machine
sudo mv /tmp/brown-noise.wav /opt/noise-machine/
```

### 4. Run setup on the Pi

```bash
ssh pi@noise-machine.local
sudo bash /tmp/noise-setup/setup.sh
sudo reboot
```

After reboot, brown noise plays automatically. Turn the potentiometer on the amp
board to adjust volume.

## Usage

The noise machine is designed as an appliance -- just plug it in and it plays.

- **Volume:** turn the knob on the PAM8403 board
- **Stop/start manually:**
  ```bash
  sudo systemctl stop noise-machine
  sudo systemctl start noise-machine
  ```

## Making the Filesystem Read-Only (Recommended)

After everything is working, lock down the SD card to prevent corruption from power loss:

```bash
sudo raspi-config nonint enable_overlayfs
sudo raspi-config nonint enable_bootro
sudo reboot
```

To make changes later, you'll need to disable the overlay:

```bash
sudo raspi-config nonint disable_overlayfs
sudo raspi-config nonint disable_bootro
sudo reboot
```

## Troubleshooting

**No audio output:**
```bash
# Check the service is running
sudo systemctl status noise-machine

# Verify ALSA sees the headphone output
aplay -l

# Test audio directly
speaker-test -t wav -c 2

# Check/set volume (should be 100% -- the amp pot controls actual volume)
amixer get Headphone
amixer set Headphone 100% unmute
```

**Service won't start:**
```bash
sudo journalctl -u noise-machine -e
```

## Optional: DAC HAT

The Pi's built-in audio is adequate for brown noise but has a noisy analog stage.
For cleaner output, add an I2S DAC HAT (e.g., HiFiBerry DAC+, InnoMaker HiFi DAC HAT).

Add the appropriate overlay to `/boot/config.txt` (or `/boot/firmware/config.txt`):
```
dtoverlay=hifiberry-dac
```

You may need to update the ALSA mixer control name in the setup script if the
DAC HAT uses `PCM` or `Digital` instead of `Headphone`.
