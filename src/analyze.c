/*
 * analyze.c -- measure a rendered noise file against the spec.
 *
 * Dependency-free on purpose: this has to run on a bare macOS or Pi toolchain
 * with no sox, ffmpeg, numpy or scipy. `cc -O2 -o analyze analyze.c -lm`.
 *
 * Reads a 16-bit PCM WAV or raw float32 (stereo interleaved) and reports the
 * things that actually distinguish a good noise machine from a bad one:
 *
 *   RMS / true peak / crest      gain staging -- the old build clipped ~1 % of
 *                                samples continuously because a 0 dBFS file met
 *                                a mixer whose ceiling is +4 dB, not 0 dB.
 *   clipped sample count         must be exactly 0.
 *   DC offset                    a drifting generator shows up here first.
 *   spectral slope (dB/octave)   -6.00 is brown. Fitted over log-spaced bands
 *                                so the top octave cannot dominate the fit.
 *   0.5 s level stability        the metric that catches "breathing". The old
 *                                chain wandered 4.08 dB peak-to-peak against a
 *                                0.41 dB JND; anything under ~0.15 dB sd is
 *                                inaudibly steady.
 *   L/R correlation             ~0 confirms the two channels are independent
 *                                (no phantom-centre imaging), 1.0 means mono.
 *
 * Everything streams, so an 8-hour render costs no more memory than a 1-second
 * one.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NFFT   8192
#define NBINS  (NFFT / 2 + 1)
#define CH     2

/* ---------------------------------------------------------------------- fft */

/* In-place iterative radix-2 FFT. NFFT is a power of two by construction. */
static void fft(double *re, double *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * M_PI / (double)len;
        double wr = cos(ang), wi = sin(ang);
        for (int i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = a + len / 2;
                double xr = re[b] * cr - im[b] * ci;
                double xi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr;        im[a] += xi;
                double nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

/* ------------------------------------------------------------------ filters */

/* Bilinear one-pole highpass, used only to weight the level-stability metric. */
typedef struct { double b0, b1, a1, x1, y1; } Sec1;

static void sec1_highpass(Sec1 *f, double fc, double fs)
{
    double w = tan(M_PI * fc / fs);
    f->b0 =  1.0 / (1.0 + w);
    f->b1 = -1.0 / (1.0 + w);
    f->a1 = (w - 1.0) / (1.0 + w);
    f->x1 = f->y1 = 0.0;
}

static inline double sec1_run(Sec1 *f, double x)
{
    double y = f->b0 * x + f->b1 * f->x1 - f->a1 * f->y1;
    f->x1 = x; f->y1 = y;
    return y;
}

/* ------------------------------------------------------------- accumulators */

typedef struct {
    double sum, sumsq;
    double peak;
    unsigned long clipped;
    unsigned long long n;

    /* Welch periodogram state. Two half-windows rather than one sliding
     * buffer: shifting NFFT samples per input sample would dominate runtime. */
    double prev[NFFT / 2];
    double cur[NFFT / 2];
    int    fill;
    int    primed;
    double psd[NBINS];
    unsigned long segments;

    /* 0.5 s level windows, broadband and through the audibility highpass */
    double wsum, wsum_hp;
    unsigned long wn;
    Sec1 lhp[2];
} Chan;

typedef struct {
    double *v;
    size_t n, cap;
} Vec;

static void vec_push(Vec *v, double x)
{
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 1024;
        v->v = realloc(v->v, v->cap * sizeof *v->v);
        if (!v->v) { fprintf(stderr, "analyze: out of memory\n"); exit(1); }
    }
    v->v[v->n++] = x;
}

static double hann[NFFT];

static void hann_init(void)
{
    for (int i = 0; i < NFFT; i++)
        hann[i] = 0.5 - 0.5 * cos(2.0 * M_PI * (double)i / (double)NFFT);
}

/* Transform prev ++ cur as one NFFT-point segment. */
static void chan_transform(Chan *c)
{
    static double re[NFFT], im[NFFT];
    for (int i = 0; i < NFFT / 2; i++) {
        re[i]              = c->prev[i] * hann[i];
        re[i + NFFT / 2]   = c->cur[i]  * hann[i + NFFT / 2];
        im[i] = im[i + NFFT / 2] = 0.0;
    }
    fft(re, im, NFFT);
    for (int k = 0; k < NBINS; k++)
        c->psd[k] += re[k] * re[k] + im[k] * im[k];
    c->segments++;
}

static void chan_push(Chan *c, double x, unsigned wlen, Vec *levels, Vec *levels_hp)
{
    c->sum   += x;
    c->sumsq += x * x;
    double a = fabs(x);
    if (a > c->peak) c->peak = a;
    if (a >= 0.99997) c->clipped++; /* within 1 LSB of 16-bit full scale */
    c->n++;

    /* Fill the current half; on each completion transform prev++cur, then the
     * current half becomes the previous one. That is 50 % overlap at O(1) per
     * sample amortised. */
    c->cur[c->fill++] = x;
    if (c->fill == NFFT / 2) {
        c->fill = 0;
        if (c->primed) chan_transform(c);
        else           c->primed = 1;
        memcpy(c->prev, c->cur, sizeof c->prev);
    }

    double xh = sec1_run(&c->lhp[1], sec1_run(&c->lhp[0], x));
    c->wsum    += x * x;
    c->wsum_hp += xh * xh;
    if (++c->wn >= wlen) {
        vec_push(levels,    sqrt(c->wsum    / (double)c->wn));
        vec_push(levels_hp, sqrt(c->wsum_hp / (double)c->wn));
        c->wsum = c->wsum_hp = 0.0; c->wn = 0;
    }
}

static double db(double x) { return x > 1e-30 ? 20.0 * log10(x) : -300.0; }

/* Standard deviation and peak-to-peak of a set of window levels, in dB. */
static void level_stats(const Vec *v, double *sd, double *pp)
{
    if (v->n < 2) { *sd = *pp = 0.0; return; }
    double m = 0.0, lo = 1e9, hi = -1e9;
    for (size_t i = 0; i < v->n; i++) {
        double d = db(v->v[i]);
        m += d; if (d < lo) lo = d; if (d > hi) hi = d;
    }
    m /= (double)v->n;
    double var = 0.0;
    for (size_t i = 0; i < v->n; i++) { double d = db(v->v[i]) - m; var += d * d; }
    *sd = sqrt(var / (double)(v->n - 1));
    *pp = hi - lo;
}

/* ------------------------------------------------------------- slope fitting */

/*
 * Fit dB vs log2(f) by least squares, but first average the linear FFT bins
 * into 1/6-octave bands. Without that, the bins-per-octave grows with
 * frequency and the fit is dominated by the top octave alone.
 */
static void fit_slope(const double *psd, unsigned long segs, double rate,
                      double flo, double fhi,
                      double *slope_out, double *ripple_out, int *nbands_out)
{
    const double step = pow(2.0, 1.0 / 6.0);
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int nb = 0;
    double bx[512], by[512];

    for (double f0 = flo; f0 * step <= fhi && nb < 512; f0 *= step) {
        double f1 = f0 * step;
        int k0 = (int)ceil(f0 / rate * NFFT);
        int k1 = (int)floor(f1 / rate * NFFT);
        if (k1 < k0 || k0 < 1 || k1 >= NBINS) continue;

        double p = 0.0;
        for (int k = k0; k <= k1; k++) p += psd[k];
        p /= (double)(k1 - k0 + 1) * (double)segs;
        if (!(p > 0.0)) continue;

        double x = log2(sqrt(f0 * f1));
        double y = 10.0 * log10(p); /* power dB */
        bx[nb] = x; by[nb] = y; nb++;
        sx += x; sy += y; sxx += x * x; sxy += x * y;
    }

    if (nb < 3) { *slope_out = NAN; *ripple_out = NAN; *nbands_out = nb; return; }

    double d = (double)nb * sxx - sx * sx;
    double m = ((double)nb * sxy - sx * sy) / d;
    double b = (sy - m * sx) / (double)nb;

    double worst = 0.0;
    for (int i = 0; i < nb; i++) {
        double r = fabs(by[i] - (m * bx[i] + b));
        if (r > worst) worst = r;
    }
    *slope_out = m; *ripple_out = worst; *nbands_out = nb;
}

/* --------------------------------------------------------------------- input */

typedef struct {
    FILE *f;
    bool f32;
    unsigned rate, channels;
    unsigned char pend[12]; /* probe bytes to replay when the input is a pipe */
    size_t npend;
} Src;

/* Read that transparently consumes the probe bytes first, so a raw stream can
 * arrive on stdin (an 8-hour soak is ~11 GB -- never worth touching disk). */
static size_t src_read(Src *s, unsigned char *dst, size_t n)
{
    size_t got = 0;
    while (s->npend && got < n) { dst[got++] = s->pend[0];
        memmove(s->pend, s->pend + 1, --s->npend); }
    return got + fread(dst + got, 1, n - got, s->f);
}

static int open_src(const char *path, unsigned rate_hint, Src *s)
{
    s->npend = 0;
    s->f = strcmp(path, "-") == 0 ? stdin : fopen(path, "rb");
    if (!s->f) { fprintf(stderr, "analyze: cannot open %s\n", path); return -1; }

    unsigned char h[12];
    if (fread(h, 1, 12, s->f) != 12) { fprintf(stderr, "analyze: file too short\n"); return -1; }

    if (!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) {
        s->f32 = false;
        unsigned char ck[8];
        while (fread(ck, 1, 8, s->f) == 8) {
            uint32_t sz = (uint32_t)ck[4] | ((uint32_t)ck[5] << 8) |
                          ((uint32_t)ck[6] << 16) | ((uint32_t)ck[7] << 24);
            if (!memcmp(ck, "fmt ", 4)) {
                unsigned char fmt[16];
                if (fread(fmt, 1, 16, s->f) != 16) return -1;
                s->channels = (unsigned)fmt[2] | ((unsigned)fmt[3] << 8);
                s->rate = (unsigned)fmt[4] | ((unsigned)fmt[5] << 8) |
                          ((unsigned)fmt[6] << 16) | ((unsigned)fmt[7] << 24);
                unsigned bits = (unsigned)fmt[14] | ((unsigned)fmt[15] << 8);
                if (bits != 16) { fprintf(stderr, "analyze: need 16-bit PCM, got %u\n", bits); return -1; }
                if (sz > 16) fseek(s->f, (long)sz - 16, SEEK_CUR);
            } else if (!memcmp(ck, "data", 4)) {
                return 0;
            } else {
                fseek(s->f, (long)sz + (sz & 1), SEEK_CUR);
            }
        }
        fprintf(stderr, "analyze: no data chunk\n");
        return -1;
    }

    /* raw float32, stereo interleaved -- replay the probe bytes rather than
     * seeking, so this works on a pipe. */
    s->f32 = true; s->rate = rate_hint; s->channels = CH;
    memcpy(s->pend, h, 12);
    s->npend = 12;
    return 0;
}

/* ---------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    static const char *use =
        "usage: analyze FILE [--rate N] [--band LO HI] [--window SEC] [--level-hp HZ] [--kv]\n";
    const char *path = NULL;
    unsigned rate_hint = 48000;
    double flo = 50.0, fhi = 8000.0;
    double wsec = 0.5;
    double level_hp = 150.0; /* what a small driver actually radiates */
    bool kv = false;          /* key=value output, for the test suite */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rate") && i + 1 < argc)          rate_hint = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--band") && i + 2 < argc)    { flo = atof(argv[++i]); fhi = atof(argv[++i]); }
        else if (!strcmp(argv[i], "--window") && i + 1 < argc)    wsec = atof(argv[++i]);
        else if (!strcmp(argv[i], "--level-hp") && i + 1 < argc)  level_hp = atof(argv[++i]);
        else if (!strcmp(argv[i], "--kv"))                        kv = true;
        else if (argv[i][0] != '-' || !argv[i][1])                path = argv[i]; /* "-" = stdin */
        else { fputs(use, stderr); return 2; }
    }
    if (!path) { fputs(use, stderr); return 2; }

    Src s;
    if (open_src(path, rate_hint, &s) != 0) return 1;
    if (s.channels != CH) { fprintf(stderr, "analyze: expected stereo, got %u ch\n", s.channels); return 1; }

    hann_init();

    Chan *c = calloc(CH, sizeof *c);
    Vec  lev[CH], levhp[CH];
    memset(lev, 0, sizeof lev);
    memset(levhp, 0, sizeof levhp);
    for (int ch = 0; ch < CH; ch++)
        for (int i = 0; i < 2; i++)
            sec1_highpass(&c[ch].lhp[i], level_hp, (double)s.rate);
    unsigned wlen = (unsigned)(wsec * (double)s.rate);
    double cross = 0.0;

    unsigned char raw[4096 * CH * 4];
    size_t item = s.f32 ? 4 : 2;
    size_t frame = item * CH;
    size_t got;

    while ((got = src_read(&s, raw, (sizeof raw / frame) * frame) / frame) > 0) {
        for (size_t i = 0; i < got; i++) {
            double v[CH];
            for (int ch = 0; ch < CH; ch++) {
                const unsigned char *p = raw + i * frame + (size_t)ch * item;
                if (s.f32) {
                    float f; memcpy(&f, p, 4); v[ch] = (double)f;
                } else {
                    int16_t iv = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
                    v[ch] = (double)iv / 32768.0;
                }
                chan_push(&c[ch], v[ch], wlen, &lev[ch], &levhp[ch]);
            }
            cross += v[0] * v[1];
        }
    }
    fclose(s.f);

    if (c[0].n == 0) { fprintf(stderr, "analyze: no samples\n"); return 1; }

    if (kv) {
        printf("rate=%u\nchannels=%u\ndur=%.3f\n",
               s.rate, s.channels, (double)c[0].n / (double)s.rate);
    } else {
        printf("file            %s (%s, %u Hz, %u ch)\n",
               path, s.f32 ? "float32" : "s16 wav", s.rate, s.channels);
        printf("duration        %.2f s (%llu frames)\n",
               (double)c[0].n / (double)s.rate, (unsigned long long)c[0].n);
        printf("fft             %d-pt Hann, 50%% overlap, %lu segments\n\n", NFFT, c[0].segments);
    }

    for (int ch = 0; ch < CH; ch++) {
        double rms  = sqrt(c[ch].sumsq / (double)c[ch].n);
        double dc   = c[ch].sum / (double)c[ch].n;
        double slope, ripple; int nb;
        fit_slope(c[ch].psd, c[ch].segments, (double)s.rate, flo, fhi, &slope, &ripple, &nb);

        double sd, pp, sd_hp, pp_hp;
        level_stats(&lev[ch],   &sd,    &pp);
        level_stats(&levhp[ch], &sd_hp, &pp_hp);

        if (kv) {
            printf("ch%d.rms=%.3f\nch%d.peak=%.3f\nch%d.crest=%.3f\nch%d.clipped=%lu\n"
                   "ch%d.dc=%.2f\nch%d.slope=%.4f\nch%d.straightness=%.4f\n"
                   "ch%d.steady_sd=%.4f\nch%d.steady_pp=%.4f\nch%d.bb_sd=%.4f\n",
                   ch, db(rms), ch, db(c[ch].peak), ch, db(c[ch].peak) - db(rms),
                   ch, c[ch].clipped, ch, db(fabs(dc)), ch, slope, ch, ripple,
                   ch, sd_hp, ch, pp_hp, ch, sd);
        } else {
            printf("channel %d\n", ch);
            printf("  rms           %+7.2f dBFS\n", db(rms));
            printf("  true peak     %+7.2f dBFS\n", db(c[ch].peak));
            printf("  crest factor  %7.2f dB\n", db(c[ch].peak) - db(rms));
            printf("  clipped       %7lu samples%s\n", c[ch].clipped,
                   c[ch].clipped ? "   <-- FAIL" : "");
            printf("  dc offset     %+7.2f dBFS\n", db(fabs(dc)));
            printf("  slope         %+7.3f dB/oct  (%.0f-%.0f Hz, %d bands)\n", slope, flo, fhi, nb);
            printf("  straightness  %7.3f dB     worst deviation from the fitted line\n", ripple);
            printf("  steadiness    %7.3f dB sd  %.3f dB p-p   >%.0f Hz, %.1fs windows\n",
                   sd_hp, pp_hp, level_hp, wsec);
            printf("  (broadband)   %7.3f dB sd  %.3f dB p-p   includes inaudible LF wander\n\n",
                   sd, pp);
        }
    }

    double r = cross / (double)c[0].n /
               (sqrt(c[0].sumsq / (double)c[0].n) * sqrt(c[1].sumsq / (double)c[1].n));
    if (kv)
        printf("corr=%.4f\n", r);
    else
        printf("L/R correlation %+.4f  (%s)\n", r,
               fabs(r) < 0.05 ? "independent" : fabs(r) > 0.95 ? "mono/identical" : "partially correlated");

    free(c);
    for (int ch = 0; ch < CH; ch++) { free(lev[ch].v); free(levhp[ch].v); }
    return 0;
}
