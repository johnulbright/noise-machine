# noise-machine

A Raspberry Pi 3 standalone brown noise machine. Synthesizes brown noise in real
time and writes it straight to ALSA, so there is no audio file, no loop point,
and no SD card in the audio path. Plays on boot, with a hardware volume knob via
a PAM8403 amplifier board.

## Bill of Materials

| Item | Notes |
|------|-------|
| Raspberry Pi 3 (any variant) | Running Raspberry Pi OS Lite (headless) |
| MicroSD card (8 GB+) | Class 10 or better |
| 5.1V/2.5A micro-USB power supply | See **Power** below — this matters more than it looks |
| PAM8403 amplifier board with potentiometer | ~$2, class-D, 2x3W, built-in volume knob |
| Passive speaker driver(s) | 4-8 ohm, 2-3W, small form factor |
| 3.5mm audio cable | Male-to-male (Pi jack to amp input) |
| 2 jumper wires (female-to-female) | For amp power (5V + GND) |

## Wiring

The PAM8403 board is powered from the Pi's 5V GPIO rail. Its built-in
potentiometer handles volume control — no software or GPIO wiring needed for
that.

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

Audio path: Pi 3.5mm jack → 3.5mm cable → PAM8403 input → passive speaker(s).

> **Never apply power to the PAM8403 with no speakers connected.** Per the
> board's documentation this damages the amplifier.

### Power

The PAM8403 draws ~200-300 mA average, and more on peaks — class-D current
draw tracks output. That is within the Pi 3's budget *if the supply is good*,
and the failure mode when it is not is subtle.

Check for it:

```bash
vcgencmd get_throttled     # want 0x0
vcgencmd measure_temp
```

Anything other than `throttled=0x0` means the 5 V rail is sagging below ~4.63 V.
Bit 0 is under-voltage now, bit 2 is throttled now, bits 16/18 mean it has
happened since boot. Two common causes, in order of likelihood:

1. **A weak supply or a thin cable.** Chargers labelled "5V 2A" often sag under
   load, and thin micro-USB cables drop 0.3-0.5 V at 1 A. The official Pi 3
   supply is **5.1 V** deliberately, to give back what the cable loses. Fix this
   first; it is cheap and usually sufficient.
2. **The amp pulling on the Pi's rail.** If a good supply does not clear it,
   give the amp its own 5 V source. Keep **GND connected to the Pi's GND** — the
   audio signal on the 3.5 mm cable needs a shared ground reference, and without
   it you get hum.

Under-voltage will not cause dropouts in this design — the generator uses well
under 1 % of one core, so throttling to 600 MHz starves nothing. What it does
risk is SD card corruption, and switching noise from the amp coupling into the
Pi's analog output stage as hiss or whine.

## Setup

### 1. Copy the repo to the Pi

```bash
scp -r . pi@<pi-address>:~/noise-machine
```

There is no audio file to generate or transfer. Everything is source.

### 2. Run setup

```bash
ssh pi@<pi-address>
sudo bash ~/noise-machine/pi/setup.sh
```

That installs `libasound2-dev`, builds `noise` and `noise-analyze`, creates a
`noise` service account, installs `/etc/noise-machine.conf`, sets the mixer,
installs two systemd units, quiets the periodic timers that can stall audio,
starts playback, and then verifies it. It is idempotent — re-run it after
editing the source.

Audio starts immediately and on every boot thereafter.

## Usage

- **Volume:** turn the knob on the PAM8403 board.
- **Tone:** edit `/etc/noise-machine.conf`, then
  `sudo systemctl restart noise-machine`. Spectral slope, highpass corner,
  lowpass corner, level and stereo mode are all adjustable without rebuilding.
- **Watch it:** `journalctl -u noise-machine -f` — it logs the device geometry,
  its scheduling policy, and an hourly count of late deadlines and recoveries.
- **Stop/start:** `sudo systemctl stop noise-machine` / `start`.

```bash
# what the program actually resolved, after config + defaults
noise --config /etc/noise-machine.conf --print-config
```

## Listening on a Mac first

`noise` plays on macOS as well as on the Pi — same generator, same DSP, same
16-bit quantisation, only the sink differs (CoreAudio instead of ALSA). So the
tone can be chosen on a laptop before any of this is deployed.

```bash
make
./bin/noise --config noise.conf        # plays until ^C
```

Start with your system volume low: `level_dbfs = -18` is moderate, but on
headphones at a high system setting it is not quiet.

Any setting can be overridden on the command line, so tone can be A/B'd in
seconds without editing the config:

```bash
./bin/noise --config noise.conf --slope_db_oct -4.5   # brighter, more "hiss"
./bin/noise --config noise.conf --slope_db_oct -7.5   # darker, more "rumble"
./bin/noise --config noise.conf --highpass_hz 60      # thinner, less low end
./bin/noise --config noise.conf --lowpass_hz 4000     # muffled, more distant
./bin/noise --config noise.conf --independent 0       # identical mono both sides
```

When you settle on something, put those values in `/etc/noise-machine.conf` on
the Pi and `sudo systemctl restart noise-machine`.

> **What the Mac test does and does not tell you.** Your Mac has a real DAC, so
> this is a clean audition of the *signal* — tone, level, steadiness. The Pi's
> 3.5 mm jack is PWM, not a DAC, and will add hiss and a raised noise floor on
> top. That difference is useful: if it sounds right on the Mac and wrong on the
> Pi, the problem is the analog stage, and a DAC HAT is the fix. See below.
>
> Also note this must be run from a normal terminal. Some sandboxed environments
> block access to `coreaudiod`, in which case both this and Apple's own `afplay`
> fail with `AudioQueueStart failed (-66680)`, which is an
> `kAudioQueueErr_InvalidDevice` — a permissions problem, not a bug.

## Verifying the sound

The generator renders to a file as readily as it plays, and `noise-analyze`
measures it. Both build and run on macOS or Linux with nothing but a C
compiler — no sox, ffmpeg, numpy or Pi required.

```bash
make
./bin/noise --f32 --seconds 600 --seed 1 --out - | ./bin/analyze - --band 200 8000
```

What a good render looks like, and what each line is for:

```
  rms            -18.03 dBFS     matches level_dbfs
  true peak       -3.46 dBFS     headroom for the mixer and the amp
  crest factor    14.57 dB       grows to ~15.4 dB over 8 hours
  clipped             0 samples  must be exactly zero
  dc offset     -118.05 dBFS     a drifting generator shows up here first
  slope          -6.002 dB/oct   -6.000 is brown
  straightness    0.145 dB       worst deviation from a straight line
  steadiness      0.197 dB sd    audible-band level wander, >150 Hz
  L/R correlation -0.0013        independent channels
```

`steadiness` is the one worth understanding. It is the standard deviation of the
level in 0.5 s windows, measured above 150 Hz because that is roughly what a
3 W micro driver actually radiates. Brown noise has an inherent floor here —
its power is concentrated at the bottom of the spectrum, so short-window level
estimates wander no matter what. Measured floors, same chain, different slopes:

| slope | steadiness (sd) |
|-------|-----------------|
| 0 dB/oct (white) | 0.024 dB |
| -3 dB/oct (pink) | 0.066 dB |
| -6 dB/oct (brown) | 0.200 dB |

So ~0.2 dB is physics, not a defect, and it sits below the ~0.41 dB just-
noticeable difference for wideband noise level. If you ever measure materially
worse than that at `-6`, something is modulating the signal.

An 8-hour soak (2.76 billion samples, streamed, no disk) reports 0 clipped
samples, a true peak of -2.59 dBFS, and a DC offset of -160 dBFS — *lower* than
the 10-minute run, which is what a genuinely zero-mean bounded process does. A
drifting integrator would go the other way.

```bash
./bin/noise --f32 --seconds 28800 --out - | ./bin/analyze - --band 200 8000
```

## How it works

Per channel: PCG32 white noise → a fractional-slope filter → a 12 dB/octave
highpass → optional lowpass → a gain calibrated at startup → TPDF dither →
16-bit.

Three design choices carry most of the weight:

- **No file.** Nothing to loop, nothing to seek, no SD read in the audio path.
- **The slope filter is a cascade of first-order shelves with unity DC gain,**
  not an integrator. It self-calibrates against its own transfer function at
  startup, which is why `slope_db_oct` lands within ~0.02 dB/oct of target
  anywhere from 0 to -7.5. Because nothing integrates, the output is bounded
  for any run length by construction rather than by a safety clamp.
- **ALSA's `stop_threshold` is pushed to the ring boundary.** A missed deadline
  replays the previous period instead of killing the stream — and replayed
  noise is indistinguishable from fresh noise, so lateness is inaudible rather
  than a dropout. It is still counted and logged.

The stream is opened once and never stopped, because starting and stopping the
Pi's PWM output swings a DC step through the amp's coupling capacitor that
24 dB of gain turns into a thump.

### Why it was rebuilt

The previous design rendered a 5-minute WAV and looped it with `mpv`. Three
independent defects, all verified:

1. **A hard dropout every 5 minutes.** The file was 300.000 s. `mpv`'s
   `--demuxer-max-back-bytes` defaults to 50 MiB = 297.215 s of that stream, so
   2.785 s had to be re-read from SD at every loop wrap — and `--cache=yes`
   enables `--cache-pause` with a 1.0 s wait, turning that stall into a
   deliberate ≥1 s hole.
2. **Continuous clipping.** The generator normalized to 0 dBFS and setup set the
   mixer to 100% — but the bcm2835 `PCM` control's ceiling is **+4.00 dB**, not
   unity. Roughly 1 % of samples hard-clipped, about 450 per second, forever.
3. **Audible breathing.** `bass +10 60` put 96.5 % of the file's power below
   100 Hz, and `compand`'s broadband detector converted that sub-bass wander
   into audible-band amplitude modulation — 4.08 dB peak-to-peak against a
   0.41 dB JND.

Worth recording what was *not* wrong, so it does not get re-fixed: the
crossfade was sample-exact and exactly equal-power; `mpv`'s loop itself is
gapless; and a 300 s period is far beyond human periodicity detection
(~20 s ceiling). The file generation was mostly right. The problems were
elsewhere.

## Making the Filesystem Read-Only (optional, last)

Do this only once everything is working, and verify it — `raspi-config`'s
overlay has been reported to silently fail to activate on recent images.

```bash
sudo raspi-config nonint enable_overlayfs
sudo raspi-config nonint enable_bootro
sudo reboot
```

Then confirm it actually took:

```bash
findmnt -no FSTYPE /     # must print 'overlay'
sudo touch /x            # must fail
```

If it reports `ext4` and the `touch` succeeds, the overlay is not active and you
have none of the protection. Note also that the overlay's upper layer is a
tmpfs with no `size=` option, which defaults to 50 % of RAM; on a 1 GB Pi that
is ~470 MB of writes accumulating until reboot.

To make changes later:

```bash
sudo raspi-config nonint disable_overlayfs
sudo raspi-config nonint disable_bootro
sudo reboot
```

## Troubleshooting

**No audio:**

```bash
systemctl status noise-machine
journalctl -u noise-machine -n 50 --no-pager

aplay -l                                  # which cards exist, and their NAMES
amixer -c Headphones sget PCM             # should read 0dB and [on]
cat /proc/asound/card0/pcm0p/sub0/status  # state: should be RUNNING
```

The mixer control on a Pi 3 is `PCM`, not `Headphone` or `Master`. Set it to
`0dB` rather than `100%` — 100% is +4 dB and will clip.

**Clicks or dropouts:**

```bash
journalctl -u noise-machine | grep -E 'late|recovered|underrun'
vcgencmd get_throttled                    # want 0x0; see Power above
systemctl list-timers --all               # anything waking up to touch the SD card
```

The service logs `scheduler SCHED_FIFO` on startup. If it says
`SCHED_OTHER (no realtime priority)`, the unit's scheduling settings are not
being applied and jitter tolerance is much lower.

**Hiss or whine rather than dropouts:** that is the analog stage, not the
software — see **Power**, and consider a DAC HAT.

## Optional: I2S DAC HAT

The Pi's 3.5 mm jack has no DAC. It generates analog audio by PWM — switching a
pin fast and filtering the result — which is why it hisses, thumps on
start/stop, and reclocks everything to 48,828 Hz through a 512-tap FIR. A ~$10-25
I2S DAC HAT (HiFiBerry DAC+ Zero, InnoMaker HiFi DAC, Pimoroni pHAT DAC, generic
PCM5102A) replaces all of that with a real converter.

Four things change, not one:

1. Add the overlay to `/boot/firmware/config.txt`, e.g. `dtoverlay=hifiberry-dac`.
2. Set `device` in `/etc/noise-machine.conf` to the new card — find the name with
   `aplay -l` and use the `hw:CARD=<name>,DEV=0` form.
3. Update the mixer unit. Many PCM5102A boards have **no hardware volume control
   at all**; if `amixer -c <card> scontrols` is empty, disable
   `noise-mixer.service` rather than letting it fail.
4. Check the GPIO header. Your amp taps pins 2 and 6 for power; some HATs pass
   those through and some cover them. Most DAC HATs output RCA rather than
   3.5 mm, so you will likely need a different cable.
