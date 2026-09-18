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

### 1. Copy the source to the Pi

```bash
ssh pi@<pi-address> 'mkdir -p ~/noise-machine'
scp -r Makefile noise.conf README.md src test ./pi pi@<pi-address>:~/noise-machine/
```

There is no audio file to generate or transfer. Everything is source. (Copying
the whole directory instead would drag along `.git` and the host's own compiled
binaries, so the explicit list is deliberate.)

### 2. Build and test, before changing anything

```bash
ssh pi@<pi-address>
sudo apt-get update && sudo apt-get install -y libasound2-dev pkg-config
cd ~/noise-machine && make && make test
```

Worth doing separately rather than letting `setup.sh` do it, for two reasons.
A compile failure surfaces while the system is still untouched. And `make test`
re-validates the DSP on **aarch64** — the filter coefficients come out of
`exp`, `log` and `tan`, and a different libm can shift them in the last bits.
The tolerances absorb that, but it is worth confirming rather than assuming.

Then check the ALSA backend actually linked in:

```bash
./bin/noise --device __no_such_device__
```

You want `noise: device not ready (attempt 1), retrying`. If you see
`built without the ALSA backend` instead, `pkg-config` did not find alsa.
Ctrl-C either way.

### 3. Hear it once, in the foreground

Before involving systemd, confirm the audio path works on its own. This
separates two questions that are painful to debug together — *does ALSA work*
and *does the service work*. If this plays, the rest is plumbing.

**With headphones only** is the best first test: no amp, no wiring, nothing but
the jack. The Pi's 3.5 mm output drives headphones directly; the amplifier
exists to reach a passive speaker, not because the jack needs help.

```bash
amixer -c Headphones -- sset PCM -25dB   # start quiet, they are on your head
./bin/noise --config noise.conf
```

From a second SSH session, bring it up while it plays:

```bash
amixer -c Headphones -- sset PCM -15dB
amixer -c Headphones -- sset PCM -5dB
amixer -c Headphones sset PCM 0dB        # the intended operating point
```

The `--` matters: `amixer` parses with getopt, so a negative dB value is read as
a bundle of option flags without it (`invalid option -- '2'`). `0dB` has no
leading hyphen, so it needs no `--`.

Do not go above `0dB`. The control's ceiling is +4 dB, and that gain is what
clipped the previous build.

It should run without `sudo` — the default Pi user is in the `audio` group. It
prints the device it opened, the rate, and the period and buffer it actually
negotiated, then plays. Ctrl-C to stop.

Two things to expect. It will log `SCHED_OTHER (no realtime priority)`, which is
correct: realtime scheduling comes from the systemd unit you are not using yet.
And headphones expose the PWM noise floor far more than a small speaker does —
hiss underneath the brown noise is the hardware, and is what a DAC HAT removes.

**With the amp connected**, same command; turn its pot up slowly instead. Leave
the amp disconnected until you have speakers wired to it — powering that board
with no speakers attached damages it.

### 4. Run setup

```bash
sudo bash ~/noise-machine/pi/setup.sh
```

That installs `libasound2-dev`, builds `noise` and `noise-analyze`, creates a
`noise` service account, installs `/etc/noise-machine.conf`, sets the mixer,
installs two systemd units, quiets the periodic timers that can stall audio,
starts playback, and then verifies it. It is idempotent — re-run it after
editing the source.

It also renames any existing `/etc/asound.conf` or `/root/.asoundrc` to
`.replaced-by-noise-machine`, because a user `~/.asoundrc` takes precedence over
`/etc/asound.conf` and either can silently redirect audio to the wrong card.

### 5. Read the verification block

Setup ends by printing what it actually achieved. What you want:

```
service         : active
pcm state       : RUNNING
mixer           : 0.00dB
scheduler       : SCHED_FIFO
buffer          : period 8192  buffer 32768 (683 ms)
power           : throttled=0x0 temp=38.1'C
```

- `pcm state` anything but `RUNNING` — the service is up but not feeding the
  device.
- `scheduler` reading `SCHED_OTHER` — the unit's realtime settings are not
  applying. It will still work, with much less tolerance for jitter.
- `power` anything but `throttled=0x0` — see **Power** above, and fix that
  before judging how it sounds.

If the service did not start, setup dumps the last 30 journal lines for you.

Audio starts immediately and on every boot thereafter.

To back it all out:

```bash
sudo systemctl disable --now noise-machine noise-mixer
```

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
make soak       # the 8-hour run above
make check      # a 10-minute render, measured
```

### The regression suite

```bash
make test       # ~1 minute, 52 assertions
```

Every assertion is a property the appliance depends on, and most correspond to a
defect the previous build actually shipped: the spectrum really is −6 dB/oct,
nothing clips, DC stays buried, the level is steady in the audible band, the
channels are decorrelated, each tone control has the effect it claims, the same
seed gives byte-identical output, and the 16-bit path measures like the float
one. It also runs `setup.sh`'s card-detection pipeline against the verbatim
`aplay -l` output from the target Pi, and checks that an HDMI-only machine
yields nothing so setup fails loudly rather than configuring a silent device.

It needs only a C compiler. No Pi, no ALSA, no sox, no Python.

### A note on how loud you can go

`level_dbfs` has a hard ceiling of **−17**, and the program warns at startup if
you exceed it.

Do not trust a short render here. Brown noise is Gaussian-ish, so its peak is
unbounded and grows with run length — the expected maximum of N samples is about
σ·√(2·ln N), which over eight hours at 48 kHz is **16.2 dB above RMS**. So:

| `level_dbfs` | 5-minute render | 8-hour reality |
|---|---|---|
| −12 | 402 clipped, peak +1.1 dBFS | clips immediately |
| −16 | 0 clipped, peak −2.9 dBFS | **predicted peak +0.2 dBFS — clips overnight** |
| −17 | 0 clipped | ceiling, no warning |
| −18 (default) | 0 clipped | measured peak −2.59 dBFS over 8 h |

That `−16` row is the trap, and it is why the warning is computed from the
predicted overnight peak rather than from what a test render happens to show.

If it isn't loud enough, turn up the amp's pot first, then the ALSA mixer (which
has +4 dB of range above 0 dB). Both are better than raising this.

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
- **A 683 ms ALSA buffer, taken at the device's maximum.** Latency is
  irrelevant for noise, so buffer depth is free robustness against jitter.

The stream is opened once and never deliberately stopped, because starting and
stopping the Pi's PWM output swings a DC step through the amp's coupling
capacitor that 24 dB of gain turns into a thump. When an underrun *does* force
a restart, the 250 ms fade-in is re-armed rather than resuming at full
amplitude.

> **A design assumption that turned out to be wrong, recorded so it isn't
> re-introduced.** `stop_threshold` is set to the ring boundary, which on a
> conventional DMA-ring driver makes a late refill harmless — the hardware keeps
> cycling the ring and replays stale samples, which for noise is inaudible. That
> is not what happens here. `bcm2835-audio` is a `snd_pcm_indirect` driver: its
> `.pointer` callback returns `snd_pcm_indirect_playback_pointer()`, the bytes
> the DAC actually drains live in VideoCore firmware rather than in the ALSA
> ring, and `bcm2835_playback_fifo()` calls
> `snd_pcm_stop(substream, SNDRV_PCM_STATE_XRUN)` on its own authority without
> ever consulting `runtime->stop_threshold` (verified against the Raspberry Pi
> kernel source). On this hardware an underrun is a real stop with a real gap.
>
> The setting is kept because it is correct and free on any other device this
> might run on, but nothing here relies on it. The same analysis killed the
> original lateness detector: `avail > buffer_size` requires the hardware
> pointer to overtake ours, which an indirect driver's bounded `hw_ptr` makes
> impossible, so it could only ever have reported zero.

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

**Reading the health line:**

Every `stats_sec` (3600 by default) the service logs:

```
noise: up 3600s, xruns 0, near-miss 0, clipped 0, min queue 24576/32768 frames
```

- **`xruns`** — real dropouts. `bcm2835` forces `SNDRV_PCM_STATE_XRUN` itself,
  so these come back from `snd_pcm_writei` as `-EPIPE`. Each one is an audible
  gap plus a fade-in. This is the number that matters; it should be 0.
- **`near-miss`** — times the queue fell below one period (8192 frames) without
  actually running dry. An early warning: rising near-misses mean you are
  heading for xruns.
- **`min queue`** — the low-water mark since the last report, out of 32768.
  Steady state should sit near 24576 (the buffer less one period). A number
  trending toward 0 is the same warning with more resolution.

**Confirming the counters actually count (worth doing once):**

Prove the instrumentation works before trusting a quiet log, in about a minute:

```bash
sudo systemctl stop noise-machine
# run it by hand, reporting every 10 s instead of hourly, with no realtime
# priority so it is easy to starve
sudo -u noise /usr/local/bin/noise --config /etc/noise-machine.conf --stats_sec 10
```

In another shell, starve it:

```bash
for i in 1 2 3 4 5 6 7 8; do (while :; do :; done) & done
sleep 30; kill %1 %2 %3 %4 %5 %6 %7 %8
```

You should see `min queue` collapse toward 0 and `near-miss` climb while the
load is on, and probably some `xruns` too. If all three stay at their idle
values through obvious audible disturbance, the instrumentation is lying and
should not be trusted. `Ctrl-C`, then `sudo systemctl start noise-machine`.

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
