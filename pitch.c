/* pitch.c: the pitch of a recording, every 5 ms, for compile.py. It never
   ships with the runtime.

   pitch IN.wav [IN.lab]

   IN.wav is 16-bit PCM at any rate; IN.lab its phone labels (a "#" line,
   then "end-time 125 phone" a line), which say where a sound is voiceless
   or silent. Every 5 ms it writes "t f0": the frame's centre in seconds
   and the pitch in Hz, 0 where there is no voice.

   Pitch by YIN (de Cheveigne and Kawahara 2002), 120 to 420 Hz. A voiced
   sound where YIN finds no period takes the pitch of the nearest frames
   that have one, then each voiced frame takes the median of five. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOP 0.005

static double *x;               /* the recording, -1 to 1 */
static int nx, sr;
static struct lab { double end; char ph[8]; } *lb;
static int nlb;

static void fail(const char *what)
{
    fprintf(stderr, "pitch: %s\n", what);
    exit(2);
}

static void readwav(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned char h[12], c[8], fm[64];
    int ch = 1, bits = 16;
    if (!f || fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
        fail("not a WAV file");
    for (;;) {
        unsigned n;
        if (fread(c, 1, 8, f) != 8)
            fail("no data");
        n = c[4] | c[5] << 8 | c[6] << 16 | (unsigned)c[7] << 24;
        if (!memcmp(c, "fmt ", 4)) {
            if (n < 16 || n > sizeof fm || fread(fm, 1, n, f) != n)
                fail("a bad fmt chunk");
            ch = fm[2] | fm[3] << 8;
            sr = fm[4] | fm[5] << 8 | fm[6] << 16;
            bits = fm[14];
            if (n & 1)
                fseek(f, 1, SEEK_CUR);
        } else if (!memcmp(c, "data", 4)) {
            short *s;
            if (bits != 16 || ch < 1 || sr < 8000)
                fail("16-bit PCM only");
            nx = (int)(n / 2 / ch);
            s = malloc(sizeof *s * ((size_t)nx * ch + 1));
            x = malloc(sizeof *x * (nx + 1));
            if (!s || !x || fread(s, 2, (size_t)nx * ch, f) != (size_t)nx * ch)
                fail("short data");
            for (int i = 0; i < nx; i++)
                x[i] = s[i * ch] / 32768.0;
            free(s);
            break;
        } else
            fseek(f, n + (n & 1), SEEK_CUR);
    }
    fclose(f);
}

static void readlab(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[128];
    int body = 0, cap = 0;
    if (!f)
        fail("cannot read the labels");
    while (fgets(line, sizeof line, f)) {
        double t;
        int k;
        char ph[16];
        if (line[0] == '#') {
            body = 1;
            continue;
        }
        if (body && sscanf(line, "%lf %d %15s", &t, &k, ph) == 3) {
            if (nlb == cap && !(lb = realloc(lb, sizeof *lb * (cap = cap ? cap * 2 : 64))))
                fail("out of memory");
            lb[nlb].end = t;
            snprintf(lb[nlb].ph, sizeof lb[nlb].ph, "%s", ph);
            nlb++;
        }
    }
    fclose(f);
}

static const char *phone_at(double t)
{
    for (int i = 0; i < nlb; i++)
        if (t < lb[i].end)
            return lb[i].ph;
    return "pau";
}

static int voiceless(const char *p)
{
    static const char *v[] = { "pau", "p", "t", "k", "f", "s", "sh", "th", "hh", "ch", "ssil", "h#", 0 };
    for (int i = 0; v[i]; i++)
        if (!strcmp(p, v[i]))
            return 1;
    return 0;
}

static double yin(int c, double *d)
{
    int w = (int)(0.030 * sr), lo = sr / 420, hi = sr / 120, s0 = c - w / 2;
    double run = 0;
    if (s0 < 0 || c + w / 2 + hi >= nx)
        return 0;
    d[0] = 1;
    for (int tau = 1; tau <= hi; tau++) {
        double e = 0;
        for (int i = 0; i < w; i++) {
            double q = x[s0 + i] - x[s0 + i + tau];
            e += q * q;
        }
        run += e;
        d[tau] = run > 0 ? e * tau / run : 1;
    }
    for (int tau = lo; tau < hi; tau++)
        if (d[tau] < 0.3 && d[tau] <= d[tau + 1]) {
            double a = d[tau - 1], b = d[tau], cc = d[tau + 1], den = a - 2 * b + cc;
            return (double)sr / (tau + (den > 0 ? 0.5 * (a - cc) / den : 0));
        }
    return 0;
}

static double med5(const double *v, int i, int n)
{
    double w[5];
    int m = 0;
    for (int k = i - 2; k <= i + 2; k++)
        if (k >= 0 && k < n)
            w[m++] = v[k];
    for (int a = 0; a < m; a++)
        for (int b = a + 1; b < m; b++)
            if (w[b] < w[a]) {
                double t = w[a];
                w[a] = w[b];
                w[b] = t;
            }
    return w[m / 2];
}

int main(int argc, char **argv)
{
    double *f0, *rms, *tmp, *d;
    int nf, *vc;

    if (argc < 2)
        fail("usage: pitch IN.wav [IN.lab]");
    readwav(argv[1]);
    if (argc > 2)
        readlab(argv[2]);
    nf = (int)(nx / (sr * HOP));
    f0 = calloc(nf + 1, sizeof *f0);
    rms = calloc(nf + 1, sizeof *rms);
    tmp = calloc(nf + 1, sizeof *tmp);
    vc = calloc(nf + 1, sizeof *vc);
    d = calloc(sr / 120 + 2, sizeof *d);
    if (!f0 || !rms || !tmp || !vc || !d)
        fail("out of memory");

    for (int t = 0; t < nf; t++) {
        double tc = (t + 0.5) * HOP, e = 0;
        int c = (int)(tc * sr), w = (int)(0.025 * sr), s0 = c - w / 2;
        for (int i = 0; i < w; i++) {
            int k = s0 + i;
            double v = k >= 0 && k < nx ? x[k] : 0;
            e += v * v;
        }
        rms[t] = sqrt(e / w);
        f0[t] = voiceless(phone_at(tc)) ? 0 : yin(c, d);
        vc[t] = f0[t] > 0 && rms[t] > 0.002;
        if (!vc[t])
            f0[t] = 0;
    }
    for (int t = 0; t < nf; t++) {
        int a = t, b = t;
        if (f0[t] > 0 || voiceless(phone_at((t + 0.5) * HOP)) || rms[t] < 0.02)
            continue;
        while (a > 0 && f0[a] <= 0)
            a--;
        while (b < nf - 1 && f0[b] <= 0)
            b++;
        if (f0[a] > 0 && f0[b] > 0)
            f0[t] = f0[a] + (f0[b] - f0[a]) * (t - a) / (double)(b - a);
        else
            f0[t] = f0[a] > 0 ? f0[a] : f0[b];
        vc[t] = f0[t] > 0;
    }
    for (int t = 0; t < nf; t++)
        tmp[t] = f0[t];
    for (int t = 0; t < nf; t++)
        if (vc[t]) {
            double m = med5(tmp, t, nf);
            if (m > 0)
                f0[t] = m;
        }
    for (int t = 0; t < nf; t++)
        printf("%.3f %.1f\n", (t + 0.5) * HOP, f0[t]);
    return 0;
}
