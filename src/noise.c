/*
 * noise.c -- continuous brown-noise generator for the noise-machine appliance.
 *
 * Two backends from one source:
 *   --out FILE   render to a 16-bit WAV (or raw float32 with --f32) and exit.
 *                Portable; this is how the signal gets measured on a dev machine.
 *   (default)    open ALSA and write forever. Linux only, needs -DHAVE_ALSA.
 *
 * The signal chain, per channel:
 *   PCG32 white noise
 *     -> fractional-slope filter   (configurable dB/octave; -6 is brown)
 *     -> 2nd-order highpass        (removes inaudible rumble that only eats headroom)
 *     -> optional 2nd-order lowpass (tone control)
 *     -> gain, calibrated at startup to hit a target RMS
 *     -> TPDF dither -> clamp -> S16_LE
 *
 * Design notes that matter:
 *
 * - No file, no loop point, no disk in the audio path. The noise never repeats,
 *   so there is nothing to hear at a loop boundary.
 *
 * - The slope filter is a cascade of first-order shelves with unity DC gain. It
 *   cannot random-walk the way a bare accumulator can, so the output is bounded
 *   for an unbounded run time by construction rather than by a safety clamp.
 *
 * - ALSA's stop_threshold is pushed to the ring boundary so a late refill does
 *   not kill the stream. The hardware keeps reading the ring and replays the
 *   last period. For *noise* specifically, replayed noise is indistinguishable
 *   from fresh noise, so a missed deadline is inaudible rather than a dropout.
 *   Lateness is still detected and counted, so it shows up in the journal.
 *
 * - The stream is opened once and never stopped. Starting and stopping the Pi's
 *   PWM output swings a DC step through the amp's coupling cap, which a 24 dB
 *   amplifier turns into a thump; never stopping is the only real fix.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_ALSA
#include <alsa/asoundlib.h>
#include <sched.h>
#include <sys/mman.h>
#endif

#ifdef HAVE_COREAUDIO
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

#ifdef __linux__
#include <sys/socket.h>
#include <sys/un.h>
#endif

/* ------------------------------------------------------------------ config */

#define SLOPE_SECTIONS 24 /* shelf sections in the fractional-slope filter */

typedef struct {
    unsigned rate;
    unsigned channels;
    double slope_db_oct;   /* -6 = brown, -3 = pink, 0 = white */
    double slope_lo_hz;    /* below this the spectrum flattens (keeps it bounded) */
    double slope_hi_hz;    /* above this the spectrum flattens */
    double highpass_hz;    /* 2nd-order; 0 disables */
    double lowpass_hz;     /* 2nd-order; 0 disables */
    double level_dbfs;     /* target RMS, per channel */
    bool dither;
    bool independent;      /* true: decorrelated L/R. false: identical mono */
    char device[128];
    unsigned period_frames;
    unsigned periods;
    uint64_t seed;
} Config;

static void config_defaults(Config *c)
{
    memset(c, 0, sizeof *c);
    c->rate         = 48000; /* the Pi's firmware resamples everything anyway; 48k avoids one extra ratio */
    c->channels     = 2;
    c->slope_db_oct = -6.0;
    c->slope_lo_hz  = 8.0;
    c->slope_hi_hz  = 20000.0;
    c->highpass_hz  = 25.0;
    c->lowpass_hz   = 0.0;
    c->level_dbfs   = -18.0;
    c->dither       = true;
    c->independent  = true;
    c->period_frames = 8192;
    c->periods       = 4;
    c->seed          = 0; /* 0 = seed from the clock */
    snprintf(c->device, sizeof c->device, "hw:CARD=Headphones,DEV=0");
}

/* --------------------------------------------------------------------- rng */

/* PCG32 (O'Neill). Small, fast, good quality, no syscalls in the audio loop. */
typedef struct { uint64_t state, inc; } Pcg;

static void pcg_seed(Pcg *r, uint64_t seed, uint64_t seq)
{
    r->state = 0u;
    r->inc   = (seq << 1u) | 1u;
    r->state = r->state * 6364136223846793005ULL + r->inc;
    r->state += seed;
    r->state = r->state * 6364136223846793005ULL + r->inc;
}

static inline uint32_t pcg_u32(Pcg *r)
{
    uint64_t old = r->state;
    r->state = old * 6364136223846793005ULL + r->inc;
    uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    uint32_t rot = (uint32_t)(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
}

/* Uniform in [-1, 1). Uniform is fine as filter input -- the filter sums many
 * samples, so the output is Gaussian by the CLT regardless. */
static inline double pcg_bipolar(Pcg *r)
{
    return (double)pcg_u32(r) * (2.0 / 4294967296.0) - 1.0;
}

/* ------------------------------------------------------------------ filters */

/*
 * First-order section: y[n] = b0*x[n] + b1*x[n-1] - a1*y[n-1],
 * i.e. H(z) = (b0 + b1*z^-1) / (1 + a1*z^-1).
 *
 * All three constructors use the bilinear transform with tan() prewarping, so
 * the corner frequency is preserved exactly and the response keeps falling to
 * Nyquist. The obvious-looking `a = exp(-2*pi*fc/fs); y += (1-a)*(x-y)` form
 * does neither: it flattens near Nyquist, which bends a -6 dB/oct cascade up to
 * about -4.4 dB/oct by 8 kHz. Measured, not theorised -- don't "simplify" this.
 */
typedef struct { double b0, b1, a1, x1, y1; } Sec1;

static inline double sec1_run(Sec1 *f, double x)
{
    double y = f->b0 * x + f->b1 * f->x1 - f->a1 * f->y1;
    f->x1 = x; f->y1 = y;
    return y;
}

static void sec1_lowpass(Sec1 *f, double fc, double fs)
{
    double w = tan(M_PI * fc / fs);
    f->b0 = f->b1 = w / (1.0 + w);
    f->a1 = (w - 1.0) / (1.0 + w);
    f->x1 = f->y1 = 0.0;
}

static void sec1_highpass(Sec1 *f, double fc, double fs)
{
    double w = tan(M_PI * fc / fs);
    f->b0 =  1.0 / (1.0 + w);
    f->b1 = -1.0 / (1.0 + w);
    f->a1 = (w - 1.0) / (1.0 + w);
    f->x1 = f->y1 = 0.0;
}

/* Shelf with unity gain at DC and gain g (< 1) above the pole at fp. */
static void sec1_shelf(Sec1 *f, double fp, double g, double fs)
{
    double P = tan(M_PI * fp / fs);
    double u = g / P, v = 1.0 / P;
    f->b0 = (1.0 + u) / (1.0 + v);
    f->b1 = (1.0 - u) / (1.0 + v);
    f->a1 = (1.0 - v) / (1.0 + v);
    f->x1 = f->y1 = 0.0;
}

/*
 * Fractional-slope filter: |H(f)| proportional to f^-beta over [lo, hi].
 *
 * Each section is the first-order shelf (1 + s/wz)/(1 + s/wp), which factors
 * exactly into  g*x + (1-g)*lowpass(x, fp)  with g = wp/wz. That identity is
 * why this is cheap: one lowpass and one lerp per section.
 *
 * Unity gain at DC, gain g < 1 above the corner. Poles are log-spaced by ratio
 * rho and each zero sits at fp*rho^beta, so every section contributes the same
 * -6*beta dB per octave and they tile into a straight line across the band.
 *
 * Because every section has unity DC gain and is stable, a bounded input gives
 * a bounded output for any run length -- no drift, no rail, no safety clamp.
 */
typedef struct {
    int n;
    Sec1 sec[SLOPE_SECTIONS];
} Slope;

static void slope_build(Slope *s, double beta, double lo, double hi, double fs)
{
    s->n = SLOPE_SECTIONS;
    double rho = pow(hi / lo, 1.0 / (double)s->n);
    double g   = pow(rho, -beta); /* per-section high-frequency gain */
    for (int k = 0; k < s->n; k++)
        sec1_shelf(&s->sec[k], lo * pow(rho, (double)k), g, fs);
}

/* |H(f)| of the whole cascade, in dB. Summed in the log domain so the product
 * of 24 sections cannot underflow. */
static double slope_mag_db(const Slope *s, double f, double fs)
{
    double w = 2.0 * M_PI * f / fs, cw = cos(w), sw = sin(w);
    double acc = 0.0;
    for (int k = 0; k < s->n; k++) {
        const Sec1 *q = &s->sec[k];
        double nr = q->b0 + q->b1 * cw, ni = -q->b1 * sw;
        double dr = 1.0   + q->a1 * cw, di = -q->a1 * sw;
        acc += 10.0 * log10((nr * nr + ni * ni) / (dr * dr + di * di));
    }
    return acc;
}

static void slope_init(Slope *s, double db_oct, double lo, double hi, double fs)
{
    double beta = -db_oct / (20.0 * log10(2.0)); /* dB/oct -> amplitude exponent */
    if (beta < 0.0) beta = 0.0;
    if (beta > 1.0) beta = 1.0;

    double nyq = 0.45 * fs;
    if (hi > nyq) hi = nyq;
    if (lo < 0.01) lo = 0.01;
    if (hi <= lo) hi = lo * 2.0;

    /*
     * Build, measure, correct. Log-spacing the poles gets the slope right in
     * the analog prototype, but the bilinear transform warps frequency near
     * Nyquist, which leaves the realized slope about 0.1 dB/oct shallow. Rather
     * than hand-tune a fudge factor that would only hold for one sample rate
     * and one target slope, evaluate the cascade's own transfer function and
     * scale beta until it delivers what was asked. Converges in 2-3 passes.
     */
    double cal_lo = fmax(200.0, lo * 4.0);
    double cal_hi = fmin(fmin(8000.0, hi * 0.5), 0.4 * fs);
    bool can_cal  = beta > 1e-6 && cal_hi > cal_lo * 2.0;

    slope_build(s, beta, lo, hi, fs);
    if (!can_cal) return;

    for (int iter = 0; iter < 4; iter++) {
        double got = (slope_mag_db(s, cal_hi, fs) - slope_mag_db(s, cal_lo, fs))
                   / log2(cal_hi / cal_lo);
        if (!(got < -1e-6)) break;
        if (fabs(got - db_oct) < 0.002) break;
        beta *= db_oct / got;
        if (beta > 1.5) { beta = 1.5; slope_build(s, beta, lo, hi, fs); break; }
        slope_build(s, beta, lo, hi, fs);
    }
}

static inline double slope_run(Slope *s, double x)
{
    for (int k = 0; k < s->n; k++) x = sec1_run(&s->sec[k], x);
    return x;
}

/* ---------------------------------------------------------------- generator */

typedef struct {
    Pcg rng;
    Slope slope;
    Sec1 hp[2], lp[2];
    bool use_hp, use_lp;
    Pcg dither_rng;
} Gen;

static void gen_init(Gen *g, const Config *c, uint64_t seq)
{
    memset(g, 0, sizeof *g);
    pcg_seed(&g->rng, c->seed, seq);
    pcg_seed(&g->dither_rng, c->seed ^ 0x9e3779b97f4a7c15ULL, seq);
    slope_init(&g->slope, c->slope_db_oct, c->slope_lo_hz, c->slope_hi_hz, (double)c->rate);

    g->use_hp = c->highpass_hz > 0.0;
    g->use_lp = c->lowpass_hz  > 0.0;
    for (int i = 0; i < 2; i++) {
        sec1_highpass(&g->hp[i], g->use_hp ? c->highpass_hz : 1.0, (double)c->rate);
        sec1_lowpass (&g->lp[i], g->use_lp ? c->lowpass_hz  : 1.0, (double)c->rate);
    }
}

/* Unity-gain sample: the shaped signal before the calibrated output gain. */
static inline double gen_sample(Gen *g)
{
    double x = slope_run(&g->slope, pcg_bipolar(&g->rng));

    /* Cascaded twice for 12 dB/octave: a first-order highpass would only
     * cancel the slope below the corner, not actually roll the rumble off. */
    if (g->use_hp) { x = sec1_run(&g->hp[0], x); x = sec1_run(&g->hp[1], x); }
    if (g->use_lp) { x = sec1_run(&g->lp[0], x); x = sec1_run(&g->lp[1], x); }
    return x;
}

/* TPDF dither: two independent uniforms summed, +/-1 LSB triangular. */
static inline double gen_dither(Gen *g)
{
    return (pcg_bipolar(&g->dither_rng) + pcg_bipolar(&g->dither_rng)) * 0.5;
}

/*
 * Calibrate the output gain so the chain hits the target RMS.
 *
 * Measured rather than derived: the filter configuration is user-editable, so
 * the only way the level stays correct across arbitrary slope/highpass/lowpass
 * settings is to measure the actual chain at startup. A scratch generator is
 * used so the real channels still begin from a clean state.
 */
static double calibrate_gain(const Config *c)
{
    Gen g;
    gen_init(&g, c, 0x5eed);

    /* 30 s, not 4: most of brown noise's power sits near the bottom of the
     * spectrum, so a short window gives a noisy RMS estimate and the output
     * level lands a few tenths of a dB off target. 30 s costs ~40 ms of CPU. */
    unsigned long warm = c->rate * 2ul;
    unsigned long meas = c->rate * 30ul;

    for (unsigned long i = 0; i < warm; i++) (void)gen_sample(&g);

    double sum = 0.0;
    for (unsigned long i = 0; i < meas; i++) {
        double v = gen_sample(&g);
        sum += v * v;
    }
    double rms = sqrt(sum / (double)meas);
    if (!(rms > 0.0)) return 1.0;

    return pow(10.0, c->level_dbfs / 20.0) / rms;
}

static inline int16_t to_s16(double x, Gen *g, bool dither, unsigned long *clipped)
{
    double v = x * 32767.0;
    if (dither) v += gen_dither(g);
    if (v > 32767.0)  { (*clipped)++; v = 32767.0; }
    if (v < -32768.0) { (*clipped)++; v = -32768.0; }
    return (int16_t)lrint(v);
}

/* ------------------------------------------------------------------- player */

/*
 * One signal source shared by every output path -- file render, ALSA, and
 * CoreAudio. Keeping a single frame-producing function is the point: what you
 * hear on a Mac is then bit-identical to what the Pi generates, so a listening
 * test on a laptop actually means something.
 */
typedef struct {
    Gen gen[2];
    double gain;
    double fade, fade_step;
    bool independent, dither;
    unsigned long clipped;
} Player;

static void player_init(Player *p, const Config *c, double gain, double fade_sec)
{
    memset(p, 0, sizeof *p);
    gen_init(&p->gen[0], c, 1);
    gen_init(&p->gen[1], c, c->independent ? 2 : 1);
    p->gain        = gain;
    p->independent = c->independent;
    p->dither      = c->dither;
    p->fade        = fade_sec > 0.0 ? 0.0 : 1.0;
    p->fade_step   = fade_sec > 0.0 ? 1.0 / (fade_sec * (double)c->rate) : 1.0;
}

static inline void player_frame(Player *p, double *l, double *r)
{
    if (p->fade < 1.0) { p->fade += p->fade_step; if (p->fade > 1.0) p->fade = 1.0; }
    double g = p->gain * p->fade;
    *l = gen_sample(&p->gen[0]) * g;
    *r = p->independent ? gen_sample(&p->gen[1]) * g : *l;
}

static void player_fill_s16(Player *p, int16_t *dst, unsigned frames)
{
    for (unsigned i = 0; i < frames; i++) {
        double l, r;
        player_frame(p, &l, &r);
        dst[i * 2]     = to_s16(l, &p->gen[0], p->dither, &p->clipped);
        dst[i * 2 + 1] = p->independent ? to_s16(r, &p->gen[1], p->dither, &p->clipped)
                                        : dst[i * 2];
    }
}

/* --------------------------------------------------------------- file output */

static void put_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xff); p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff); p[3] = (unsigned char)((v >> 24) & 0xff);
}
static void put_u16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v & 0xff); p[1] = (unsigned char)((v >> 8) & 0xff);
}

static int write_wav_header(FILE *f, const Config *c, uint32_t frames)
{
    unsigned char h[44];
    uint32_t bytes = frames * c->channels * 2u;
    memcpy(h, "RIFF", 4);           put_u32(h + 4, 36u + bytes);
    memcpy(h + 8, "WAVEfmt ", 8);   put_u32(h + 16, 16u);
    put_u16(h + 20, 1);             put_u16(h + 22, (uint16_t)c->channels);
    put_u32(h + 24, c->rate);       put_u32(h + 28, c->rate * c->channels * 2u);
    put_u16(h + 32, (uint16_t)(c->channels * 2u));
    put_u16(h + 34, 16);
    memcpy(h + 36, "data", 4);      put_u32(h + 40, bytes);
    return fwrite(h, 1, sizeof h, f) == sizeof h ? 0 : -1;
}

static int render_file(const Config *c, const char *path, double seconds, bool f32,
                       double gain)
{
    FILE *f = strcmp(path, "-") == 0 ? stdout : fopen(path, "wb");
    if (!f) { fprintf(stderr, "noise: cannot open %s: %s\n", path, strerror(errno)); return 1; }

    uint32_t frames = (uint32_t)(seconds * (double)c->rate);
    if (!f32 && write_wav_header(f, c, frames) != 0) {
        fprintf(stderr, "noise: short header write\n");
        return 1;
    }

    /* No fade on a render: a measurement should see the steady state. */
    Player p;
    player_init(&p, c, gain, 0.0);

    for (uint32_t i = 0; i < frames; i++) {
        if (f32) {
            double l, r;
            player_frame(&p, &l, &r);
            float v[2] = { (float)l, (float)r };
            if (fwrite(v, sizeof(float), c->channels, f) != c->channels) break;
        } else {
            int16_t v[2];
            player_fill_s16(&p, v, 1);
            if (fwrite(v, sizeof(int16_t), c->channels, f) != c->channels) break;
        }
    }

    if (f != stdout) fclose(f);
    fprintf(stderr, "noise: wrote %.1f s to %s (%s), gain %.4f, clipped %lu\n",
            seconds, path, f32 ? "float32" : "s16le wav", gain, p.clipped);
    return 0;
}

/* ------------------------------------------------------------- sd_notify(3) */

#ifdef __linux__
static int notify_fd = -1;
static struct sockaddr_un notify_addr;
static socklen_t notify_len;

static void notify_init(void)
{
    const char *p = getenv("NOTIFY_SOCKET");
    if (!p || !*p) return;
    notify_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (notify_fd < 0) return;
    memset(&notify_addr, 0, sizeof notify_addr);
    notify_addr.sun_family = AF_UNIX;
    strncpy(notify_addr.sun_path, p, sizeof notify_addr.sun_path - 1);
    if (notify_addr.sun_path[0] == '@') notify_addr.sun_path[0] = '\0';
    notify_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(p));
}

static void notify(const char *msg)
{
    if (notify_fd < 0) return;
    (void)sendto(notify_fd, msg, strlen(msg), MSG_NOSIGNAL,
                 (struct sockaddr *)&notify_addr, notify_len);
}
#else
__attribute__((unused)) static void notify_init(void) {}
__attribute__((unused)) static void notify(const char *msg) { (void)msg; }
#endif

/* -------------------------------------------------------- CoreAudio backend */

#ifdef HAVE_COREAUDIO

/*
 * macOS playback, so the sound can be auditioned and the tone settings chosen
 * on a laptop before any of this reaches the Pi. Same Player, same DSP, same
 * 16-bit quantisation as the ALSA path -- only the sink differs.
 *
 * AudioQueue rather than an AudioUnit: the callback runs on a thread CoreAudio
 * manages, three buffers deep, which is plenty for a source that costs well
 * under 1 % of a core and has no latency requirement whatsoever.
 */
static void ca_callback(void *user, AudioQueueRef q, AudioQueueBufferRef buf)
{
    Player *p = (Player *)user;
    unsigned frames = buf->mAudioDataBytesCapacity / (2 * sizeof(int16_t));
    player_fill_s16(p, (int16_t *)buf->mAudioData, frames);
    buf->mAudioDataByteSize = frames * 2 * sizeof(int16_t);
    AudioQueueEnqueueBuffer(q, buf, 0, NULL);
}

static int run_coreaudio(const Config *c, double gain)
{
    AudioStreamBasicDescription fmt;
    memset(&fmt, 0, sizeof fmt);
    fmt.mSampleRate       = (Float64)c->rate;
    fmt.mFormatID         = kAudioFormatLinearPCM;
    fmt.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    fmt.mBitsPerChannel   = 16;
    fmt.mChannelsPerFrame = 2;
    fmt.mFramesPerPacket  = 1;
    fmt.mBytesPerFrame    = 4;
    fmt.mBytesPerPacket   = 4;

    static Player p;
    player_init(&p, c, gain, 0.25);

    AudioQueueRef q = NULL;
    OSStatus err = AudioQueueNewOutput(&fmt, ca_callback, &p, NULL, NULL, 0, &q);
    if (err) { fprintf(stderr, "noise: AudioQueueNewOutput failed (%d)\n", (int)err); return 1; }

    const unsigned nbuf = 3;
    UInt32 bytes = c->period_frames * 4;
    for (unsigned i = 0; i < nbuf; i++) {
        AudioQueueBufferRef b = NULL;
        if ((err = AudioQueueAllocateBuffer(q, bytes, &b))) {
            fprintf(stderr, "noise: AudioQueueAllocateBuffer failed (%d)\n", (int)err);
            return 1;
        }
        ca_callback(&p, q, b); /* prime it before starting */
    }

    if ((err = AudioQueueStart(q, NULL))) {
        fprintf(stderr, "noise: AudioQueueStart failed (%d)\n", (int)err);
        return 1;
    }

    fprintf(stderr, "noise: CoreAudio  %u Hz  2 ch  s16  %u x %u frames (%.0f ms)\n"
                    "noise: playing. ^C to stop.\n",
            c->rate, nbuf, c->period_frames,
            1000.0 * (double)(nbuf * c->period_frames) / (double)c->rate);

    CFRunLoopRun();
    return 0;
}

#endif /* HAVE_COREAUDIO */

/* ------------------------------------------------------------- ALSA backend */

#ifdef HAVE_ALSA

static int alsa_open(const Config *c, snd_pcm_t **out,
                     snd_pcm_uframes_t *period, snd_pcm_uframes_t *buffer)
{
    snd_pcm_t *pcm = NULL;
    int err = snd_pcm_open(&pcm, c->device, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "noise: open %s: %s\n", c->device, snd_strerror(err));
        return err;
    }

    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);

    if ((err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0 ||
        (err = snd_pcm_hw_params_set_channels(pcm, hw, c->channels)) < 0) {
        fprintf(stderr, "noise: hw_params: %s\n", snd_strerror(err));
        goto fail;
    }

    unsigned rate = c->rate;
    if ((err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, 0)) < 0) {
        fprintf(stderr, "noise: set_rate: %s\n", snd_strerror(err));
        goto fail;
    }
    if (rate != c->rate)
        fprintf(stderr, "noise: warning: asked %u Hz, device gave %u Hz\n", c->rate, rate);

    /* bcm2835 caps BUFFER_BYTES at 131072, i.e. 32768 frames at s16 stereo
     * (683 ms at 48 kHz). Ask for as much as the device will give -- latency is
     * irrelevant here and buffer depth is the entire defence against jitter. */
    snd_pcm_uframes_t p = c->period_frames;
    if ((err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &p, 0)) < 0) {
        fprintf(stderr, "noise: set_period: %s\n", snd_strerror(err));
        goto fail;
    }
    snd_pcm_uframes_t b = p * c->periods;
    if ((err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &b)) < 0) {
        fprintf(stderr, "noise: set_buffer: %s\n", snd_strerror(err));
        goto fail;
    }
    if ((err = snd_pcm_hw_params(pcm, hw)) < 0) {
        fprintf(stderr, "noise: hw_params commit: %s\n", snd_strerror(err));
        goto fail;
    }
    snd_pcm_hw_params_get_period_size(hw, &p, 0);
    snd_pcm_hw_params_get_buffer_size(hw, &b);

    snd_pcm_sw_params_t *sw;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(pcm, sw);

    /* Start only once the ring is full, so playback never begins underfed. */
    snd_pcm_sw_params_set_start_threshold(pcm, sw, b);

    /* Push stop_threshold to the ring boundary: a missed deadline replays the
     * previous period instead of killing the stream. Replayed noise sounds
     * exactly like fresh noise, so lateness becomes inaudible, not a dropout. */
    snd_pcm_uframes_t boundary = 0;
    snd_pcm_sw_params_get_boundary(sw, &boundary);
    snd_pcm_sw_params_set_stop_threshold(pcm, sw, boundary);
    snd_pcm_sw_params_set_avail_min(pcm, sw, p);

    if ((err = snd_pcm_sw_params(pcm, sw)) < 0) {
        fprintf(stderr, "noise: sw_params: %s\n", snd_strerror(err));
        goto fail;
    }

    fprintf(stderr, "noise: %s  %u Hz  %u ch  s16_le  period %lu  buffer %lu (%.0f ms)\n",
            c->device, rate, c->channels, (unsigned long)p, (unsigned long)b,
            1000.0 * (double)b / (double)rate);

    *out = pcm; *period = p; *buffer = b;
    return 0;

fail:
    snd_pcm_close(pcm);
    return err;
}

static int run_alsa(const Config *c, double gain)
{
    snd_pcm_t *pcm = NULL;
    snd_pcm_uframes_t period = 0, buffer = 0;

    /* Retry forever rather than exit: the unit must never reach a state a
     * human has to clear, and the sound device may not be probed yet. */
    for (unsigned attempt = 1;; attempt++) {
        if (alsa_open(c, &pcm, &period, &buffer) == 0) break;
        if (attempt == 1 || attempt % 30 == 0)
            fprintf(stderr, "noise: device not ready (attempt %u), retrying\n", attempt);
        sleep(1);
    }

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        fprintf(stderr, "noise: mlockall failed (%s); pages may be swapped\n", strerror(errno));

    int policy = sched_getscheduler(0);
    fprintf(stderr, "noise: scheduler %s\n",
            policy == SCHED_FIFO ? "SCHED_FIFO" :
            policy == SCHED_RR   ? "SCHED_RR" : "SCHED_OTHER (no realtime priority)");

    int16_t *buf = calloc(period * c->channels, sizeof *buf);
    Player *p = calloc(1, sizeof *p);
    if (!buf || !p) { fprintf(stderr, "noise: out of memory\n"); return 1; }
    /* 250 ms fade so the first buffer does not step the amp's coupling cap. */
    player_init(p, c, gain, 0.25);

    unsigned long late = 0, recovered = 0;
    unsigned long long frames_out = 0;
    unsigned long long next_report = (unsigned long long)c->rate * 3600ull;
    bool ready = false;

    notify_init();

    for (;;) {
        player_fill_s16(p, buf, (unsigned)period);

        /* A full ring plus one period pending means we missed a deadline. */
        snd_pcm_sframes_t avail = snd_pcm_avail(pcm);
        if (avail > (snd_pcm_sframes_t)buffer) late++;

        snd_pcm_uframes_t left = period;
        int16_t *w = buf;
        while (left > 0) {
            snd_pcm_sframes_t n = snd_pcm_writei(pcm, w, left);
            if (n < 0) {
                if (n == -EAGAIN) continue;
                int err = snd_pcm_recover(pcm, (int)n, 1);
                if (err < 0) {
                    fprintf(stderr, "noise: unrecoverable: %s; reopening\n", snd_strerror((int)n));
                    snd_pcm_close(pcm);
                    while (alsa_open(c, &pcm, &period, &buffer) != 0) sleep(1);
                    break;
                }
                recovered++;
                continue;
            }
            left -= (snd_pcm_uframes_t)n;
            w    += (size_t)n * c->channels;
        }

        frames_out += period;

        if (!ready) {
            ready = true;
            notify("READY=1\nSTATUS=playing\n");
        }
        notify("WATCHDOG=1");

        if (frames_out >= next_report) {
            fprintf(stderr, "noise: %llu h uptime, late %lu, recovered %lu, clipped %lu\n",
                    frames_out / ((unsigned long long)c->rate * 3600ull),
                    late, recovered, p->clipped);
            next_report += (unsigned long long)c->rate * 3600ull;
        }
    }
}

#endif /* HAVE_ALSA */

/* Pick whatever sink this build actually has. */
static int run_live(const Config *c, double gain)
{
#if defined(HAVE_ALSA)
    return run_alsa(c, gain);
#elif defined(HAVE_COREAUDIO)
    return run_coreaudio(c, gain);
#else
    (void)c; (void)gain;
    fprintf(stderr, "noise: built without the ALSA backend; use --out FILE to render.\n");
    return 2;
#endif
}

/* -------------------------------------------------------------------- setup */

static int parse_kv(Config *c, const char *k, const char *v)
{
    if      (!strcmp(k, "rate"))          c->rate = (unsigned)strtoul(v, NULL, 10);
    else if (!strcmp(k, "slope_db_oct"))  c->slope_db_oct = strtod(v, NULL);
    else if (!strcmp(k, "slope_lo_hz"))   c->slope_lo_hz = strtod(v, NULL);
    else if (!strcmp(k, "slope_hi_hz"))   c->slope_hi_hz = strtod(v, NULL);
    else if (!strcmp(k, "highpass_hz"))   c->highpass_hz = strtod(v, NULL);
    else if (!strcmp(k, "lowpass_hz"))    c->lowpass_hz = strtod(v, NULL);
    else if (!strcmp(k, "level_dbfs"))    c->level_dbfs = strtod(v, NULL);
    else if (!strcmp(k, "dither"))        c->dither = atoi(v) != 0;
    else if (!strcmp(k, "independent"))   c->independent = atoi(v) != 0;
    else if (!strcmp(k, "period_frames")) c->period_frames = (unsigned)strtoul(v, NULL, 10);
    else if (!strcmp(k, "periods"))       c->periods = (unsigned)strtoul(v, NULL, 10);
    else if (!strcmp(k, "seed"))          c->seed = strtoull(v, NULL, 10);
    else if (!strcmp(k, "device"))        snprintf(c->device, sizeof c->device, "%s", v);
    else return -1;
    return 0;
}

static void load_config(Config *c, const char *path, bool required)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        if (required) fprintf(stderr, "noise: cannot read %s: %s\n", path, strerror(errno));
        return;
    }
    char line[256];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *h = strchr(line, '#'); if (h) *h = '\0';
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq = '\0';
        char *k = line, *v = eq + 1;
        while (*k == ' ' || *k == '\t') k++;
        char *e = k + strlen(k); while (e > k && (e[-1]==' '||e[-1]=='\t'||e[-1]=='\n')) *--e = '\0';
        while (*v == ' ' || *v == '\t') v++;
        e = v + strlen(v);   while (e > v && (e[-1]==' '||e[-1]=='\t'||e[-1]=='\n'||e[-1]=='\r')) *--e = '\0';
        if (!*k) continue;
        if (parse_kv(c, k, v) != 0)
            fprintf(stderr, "noise: %s:%d: unknown key '%s'\n", path, lineno, k);
    }
    fclose(f);
}

static void usage(void)
{
    fprintf(stderr,
      "usage: noise [options]\n"
      "  --config FILE     read settings (default /etc/noise-machine.conf if present)\n"
      "  --out FILE        render to FILE instead of playing ('-' for stdout)\n"
      "  --seconds N       render length (default 60)\n"
      "  --f32             render raw float32 instead of a 16-bit WAV\n"
      "  --seed N          RNG seed (default: clock; set for reproducible renders)\n"
      "  --print-config    dump the resolved settings and exit\n"
      "  --KEY VALUE       override any config key, e.g. --level_dbfs -14\n");
}

int main(int argc, char **argv)
{
    Config c;
    config_defaults(&c);

    const char *cfg = NULL, *out = NULL;
    double seconds = 60.0;
    bool f32 = false, print_only = false;

    /* A config file given on the command line wins over the system one, and
     * explicit --KEY overrides win over both, so scan for --config first. */
    for (int i = 1; i < argc - 1; i++)
        if (!strcmp(argv[i], "--config")) cfg = argv[i + 1];
    load_config(&c, cfg ? cfg : "/etc/noise-machine.conf", cfg != NULL);

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); return 0; }
        else if (!strcmp(a, "--f32")) f32 = true;
        else if (!strcmp(a, "--print-config")) print_only = true;
        else if (!strcmp(a, "--config") && i + 1 < argc) i++;
        else if (!strcmp(a, "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(a, "--seconds") && i + 1 < argc) seconds = strtod(argv[++i], NULL);
        else if (!strncmp(a, "--", 2) && i + 1 < argc) {
            if (parse_kv(&c, a + 2, argv[i + 1]) != 0) {
                fprintf(stderr, "noise: unknown option %s\n", a);
                usage(); return 2;
            }
            i++;
        } else {
            fprintf(stderr, "noise: unexpected argument %s\n", a);
            usage(); return 2;
        }
    }

    if (c.channels != 2) { fprintf(stderr, "noise: only 2 channels supported\n"); return 2; }
    if (c.seed == 0) c.seed = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32);

    if (print_only) {
        printf("rate=%u\nchannels=%u\nslope_db_oct=%g\nslope_lo_hz=%g\nslope_hi_hz=%g\n"
               "highpass_hz=%g\nlowpass_hz=%g\nlevel_dbfs=%g\ndither=%d\nindependent=%d\n"
               "device=%s\nperiod_frames=%u\nperiods=%u\nseed=%llu\n",
               c.rate, c.channels, c.slope_db_oct, c.slope_lo_hz, c.slope_hi_hz,
               c.highpass_hz, c.lowpass_hz, c.level_dbfs, c.dither, c.independent,
               c.device, c.period_frames, c.periods, (unsigned long long)c.seed);
        return 0;
    }

    double gain = calibrate_gain(&c);
    return out ? render_file(&c, out, seconds, f32, gain) : run_live(&c, gain);
}
