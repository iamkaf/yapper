/* compile.c: yapper compile, a phrase table (FORMAT.md) from a text file of
   lines. It never ships with the runtime.

   yapper compile LINES.txt -o TABLE.yph

   LINES.txt holds one line of speech a line, UTF-8; blank lines and lines
   that start with "#" are skipped. Each line is split into phrases at
   , . ; : ! ? as the runtime splits it, and each phrase gets a record: its
   words' sounds, each sound's length, and each vowel's stress and pitch.

   It runs two speech programs, which it needs installed and which never
   ship with the runtime: flite says the whole line in its slt voice, and
   gives each word's sounds as its lexicon says them in context, and each
   sound's length; espeak-ng says which syllable of each word is stressed.
   pitch.c reads each vowel's pitch off flite's recording.

   compile.py is the reference, and test.py holds this file to it byte for
   byte, so a change to one is a change to both. Where Python decides a
   byte, this file does as Python does: str.strip(), round(), sum(),
   difflib's matching blocks, and the pitch read back from pitch.c's
   printed numbers. */

#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#else
#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE        /* macOS declares mkdtemp only with this */
#endif
#endif

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define popen _popen
#define pclose _pclose
#define rmdir _rmdir
#define SEP "\\"
#else
#include <unistd.h>
#define SEP "/"
#endif

#define TEXT 1024               /* yapper.h's YAP_TEXT: what is past it is never said */
#define BATCH 16                /* how many programs run at once */

int compile_command(int argc, char **argv);
double *pitch_track(const char *wav, int nlab, const double *ends, const char *const *phones, int *frames);

/* The sounds a record names, by number: yapper.c's cph[]. 0 to 15 are vowels. */
static const char *const PHONES[40] = {
    "iy", "ih", "ey", "eh", "ae", "aa", "ao", "ow", "uh", "uw", "ah", "ax", "er", "ay", "aw", "oy",
    "b", "d", "g", "p", "t", "k", "f", "v", "th", "dh", "s", "z", "sh", "zh", "hh", "ch", "jh",
    "m", "n", "ng", "l", "r", "w", "y",
};
/* Words said unstressed whatever espeak-ng says. */
static const char *const FUNCTION[] = {
    "a", "an", "the", "and", "or", "but", "of", "to", "in", "on", "at", "by", "for", "with", "from", "as",
    "is", "was", "were", "are", "be", "been", "am", "will", "would", "shall", "should", "can", "could",
    "do", "does", "did", "have", "has", "had", "i", "you", "he", "she", "it", "we", "they", "me", "him",
    "her", "us", "them", "my", "your", "his", "its", "our", "their", "that", "this", "these", "those",
    "there", "not", "no", "so", "if", "than", "then", "up", "out", 0,
};
/* The IPA vowels, as code points. */
static const uint32_t IPA_V[] = {
    0x61, 0x65, 0x69, 0x6f, 0x75, 0x79, 0xe6, 0x251, 0x252, 0x254, 0x259, 0x25a, 0x25b, 0x25c,
    0x25d, 0x26a, 0x28a, 0x28c, 0x250, 0xf8, 0x153, 0x258, 0x275, 0x264, 0x289, 0x26f, 0x268, 0,
};

typedef struct { char *p; size_t n, cap; } buf;     /* bytes, always NUL-terminated */
typedef struct { int a, b; } span;

static char tmpdir[4096];

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("yapper: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static void *grow(void *p, size_t size)
{
    if (!(p = realloc(p, size ? size : 1)))
        die("out of memory");
    return p;
}

static void *zeros(size_t n, size_t size)
{
    void *p = calloc(n ? n : 1, size);
    if (!p)
        die("out of memory");
    return p;
}

static void put(buf *b, const void *s, size_t n)
{
    if (b->n + n + 1 > b->cap)
        b->p = grow(b->p, b->cap = (b->n + n + 1) * 2);
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void puts_(buf *b, const char *s)
{
    put(b, s, strlen(s));
}

static void putc_(buf *b, int c)
{
    char ch = (char)c;
    put(b, &ch, 1);
}

/* ---------------------------------------------------------------- UTF-8, as Python reads it */

/* The length of the well-formed UTF-8 sequence at s, at most n bytes, and
   its code point in *cp; 0 if there is none. */
static int utf8(const unsigned char *s, size_t n, uint32_t *cp)
{
    static const uint32_t least[5] = { 0, 0, 0x80, 0x800, 0x10000 };
    int len = s[0] < 0x80 ? 1 : s[0] < 0xC2 ? 0 : s[0] < 0xE0 ? 2 : s[0] < 0xF0 ? 3 : s[0] < 0xF5 ? 4 : 0;
    uint32_t c = len == 1 ? s[0] : s[0] & (0x7Fu >> len);
    if (!len || (size_t)len > n)
        return 0;
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80)
            return 0;
        c = c << 6 | (s[i] & 0x3F);
    }
    if (c < least[len] || c > 0x10FFFF || (c >= 0xD800 && c < 0xE000))
        return 0;
    *cp = c;
    return len;
}

/* The code point at s, or U+FFFD for a byte that starts none; *len its bytes. */
static uint32_t next_cp(const char *s, size_t n, int *len)
{
    uint32_t c = 0xFFFD;
    *len = utf8((const unsigned char *)s, n, &c);
    if (!*len)
        *len = 1;
    return c;
}

/* Python's str.isspace(). */
static int pyspace(uint32_t c)
{
    return (c >= 9 && c <= 13) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
           c == 0x3000;
}

/* What str.splitlines() splits at, besides "\r\n". */
static int pybreak(uint32_t c)
{
    return c == 10 || c == 11 || c == 12 || c == 13 || (c >= 0x1C && c <= 0x1E) || c == 0x85 || c == 0x2028 ||
           c == 0x2029;
}

/* Whether s[0, n) holds anything but white space. */
static int has_text(const char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        int len;
        if (!pyspace(next_cp(s + i, n - i, &len)))
            return 1;
        i += (size_t)len;
    }
    return 0;
}

/* ---------------------------------------------------------------- Python's arithmetic */

/* round(x): the nearest whole number, halves to even. */
static double pyround(double x)
{
    double r = round(x);
    return fabs(x - r) == 0.5 ? 2.0 * round(x / 2.0) : r;
}

/* A number as Python reads it back after a program printed it with fmt. */
static double printed(const char *fmt, double x)
{
    char s[64];
    snprintf(s, sizeof s, fmt, x);
    return strtod(s, 0);
}

/* ---------------------------------------------------------------- as the runtime reads a line */

static int letter(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static int stop(int c)
{
    return c == ',' || c == '.' || c == ';' || c == ':' || c == '!' || c == '?';
}

/* The line as the runtime holds it: at most TEXT bytes, what is in
   brackets blanked where it stands. Returns its length. */
static int blank(const char *line, unsigned char *page)
{
    int n = 0, depth = 0;
    for (; line[n] && n < TEXT; n++) {
        int c = (unsigned char)line[n], opening = c == '(' || c == '[', closing = c == ')' || c == ']';
        depth += opening;
        page[n] = (unsigned char)(depth ? ' ' : c);
        if (closing && depth)
            depth--;
    }
    return n;
}

/* The page's words as the runtime walks them, into w; phrase i's words
   are first[i] to first[i + 1]. Returns how many phrases, empty ones too. */
static int walk(const unsigned char *page, int n, span *w, int *first)
{
    int t = 0, nw = 0, np = 0;
    while (t < n) {
        first[np++] = nw;
        while (t < n && !stop(page[t])) {
            if (!letter(page[t])) {
                t++;
                continue;
            }
            w[nw].a = t;
            while (t < n && (letter(page[t]) || (page[t] == '\'' && t + 1 < n && letter(page[t + 1]))))
                t++;
            w[nw++].b = t;
        }
        while (t < n && stop(page[t]))
            t++;
    }
    first[np] = nw;
    return np;
}

/* A word in lower case, as compile.py names it. */
static char *lower(const unsigned char *page, span w)
{
    char *s = grow(0, (size_t)(w.b - w.a) + 1);
    for (int i = w.a; i < w.b; i++)
        s[i - w.a] = (char)(page[i] >= 'A' && page[i] <= 'Z' ? page[i] | 32 : page[i]);
    s[w.b - w.a] = 0;
    return s;
}

static uint32_t fnv(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

/* ---------------------------------------------------------------- running flite and espeak-ng */

/* An argument for the shell: POSIX's, or on Windows cmd.exe and then the
   program's C runtime. A text there is quoted for the runtime, and every
   ASCII byte follows a caret so that cmd.exe takes none as its own; a path,
   which holds no quote, is only quoted. */
static void quote(buf *b, const char *s, int path)
{
#ifdef _WIN32
    buf q = { 0 };
    size_t slashes = 0;
    if (path) {
        putc_(b, '"');
        puts_(b, s);
        putc_(b, '"');
        return;
    }
    putc_(&q, '"');
    for (; *s; s++) {
        if (*s == '\\') {
            slashes++;
            continue;
        }
        for (size_t i = 0; i < (*s == '"' ? 2 * slashes + 1 : slashes); i++)
            putc_(&q, '\\');
        putc_(&q, *s);
        slashes = 0;
    }
    for (size_t i = 0; i < 2 * slashes; i++)
        putc_(&q, '\\');
    putc_(&q, '"');
    for (size_t i = 0; i < q.n; i++) {
        if (!((unsigned char)q.p[i] & 0x80))
            putc_(b, '^');
        putc_(b, q.p[i]);
    }
    free(q.p);
#else
    (void)path;
    putc_(b, '\'');
    for (; *s; s++)
        if (*s == '\'')
            puts_(b, "'\\''");
        else
            putc_(b, *s);
    putc_(b, '\'');
#endif
}

static FILE *start(const buf *cmd)
{
    FILE *f = popen(cmd->p, "r");
    if (!f)
        die("cannot run %s", cmd->p);
    return f;
}

/* Runs the n commands, at most BATCH at once, and hands each one's standard
   output to done() in order. Command i + BATCH starts once done() is
   through with command i. Any that fails stops the compiler. */
static void run_all(const buf *cmds, int n, void (*done)(int i, const buf *out, void *ctx), void *ctx)
{
    FILE *f[BATCH];
    buf out = { 0 };
    for (int i = 0; i < n && i < BATCH; i++)
        f[i] = start(&cmds[i]);
    for (int i = 0; i < n; i++) {
        char chunk[4096];
        size_t got;
        out.n = 0;
        put(&out, "", 0);
        while ((got = fread(chunk, 1, sizeof chunk, f[i % BATCH])) > 0)
            put(&out, chunk, got);
        if (pclose(f[i % BATCH]))
            die("this failed: %s", cmds[i].p);
        done(i, &out, ctx);
        if (i + BATCH < n)
            f[i % BATCH] = start(&cmds[i + BATCH]);
    }
    free(out.p);
}

static void temp_path(char *out, size_t size, const char *name)
{
    snprintf(out, size, "%s" SEP "%s", tmpdir, name);
}

static void cleanup(void)
{
    char path[4200], name[16];
    if (!tmpdir[0])
        return;
    temp_path(path, sizeof path, "words.txt");
    remove(path);
    for (int i = 0; i < BATCH; i++) {
        snprintf(name, sizeof name, "l%d.wav", i);
        temp_path(path, sizeof path, name);
        remove(path);
    }
    rmdir(tmpdir);
    tmpdir[0] = 0;
}

/* Stops the compiler unless flite and espeak-ng both run. */
static void installed(void)
{
    static const char *const probe[2][2] = {
        { "flite", "flite -ps -t a -o none 2>&1" },
        { "espeak-ng", "espeak-ng --version 2>&1" },
    };
    for (int i = 0; i < 2; i++) {
        buf cmd = { 0 };
        FILE *f;
        char chunk[256];
        puts_(&cmd, probe[i][1]);
        f = start(&cmd);
        while (fread(chunk, 1, sizeof chunk, f) > 0)
            ;
        free(cmd.p);
        if (pclose(f))
            die("%s is not installed", probe[i][0]);
    }
}

static void make_tmpdir(void)
{
#ifdef _WIN32
    char *name = _tempnam(0, "yapper-");
    if (!name || _mkdir(name))
        die("cannot make a temporary directory");
    snprintf(tmpdir, sizeof tmpdir, "%s", name);
    free(name);
#else
    const char *t = getenv("TMPDIR");
    snprintf(tmpdir, sizeof tmpdir, "%s/yapper-XXXXXX", t && *t ? t : "/tmp");
    if (!mkdtemp(tmpdir)) {
        tmpdir[0] = 0;
        die("cannot make a temporary directory");
    }
#endif
    atexit(cleanup);
}

/* ---------------------------------------------------------------- each word alone */

typedef struct {
    char *word;
    signed char *sounds;        /* flite's, alone: a number in PHONES, or -1 for one not there */
    int nsounds;
    int prim, nsyl;             /* espeak-ng's stressed syllable (-1 for none) and syllables */
    unsigned char *sec;         /* 1 for each syllable with a secondary stress */
} word;

static int phone(const char *s, size_t n)
{
    for (int i = 0; i < 40; i++)
        if (strlen(PHONES[i]) == n && !memcmp(PHONES[i], s, n))
            return i;
    return -1;
}

static int is_space(int c)
{
    return c == ' ' || (c >= 9 && c <= 13);
}

/* flite's sounds for a word said alone, from its output. */
static void sounds_of(word *w, const char *out)
{
    for (const char *s = out; *s;) {
        size_t n = 0;
        while (is_space(*s))
            s++;
        while (s[n] && !is_space(s[n]))
            n++;
        if (n && !(n == 3 && !memcmp(s, "pau", 3))) {
            w->sounds = grow(w->sounds, (size_t)w->nsounds + 1);
            w->sounds[w->nsounds++] = (signed char)phone(s, n);
        }
        s += n;
    }
}

/* Which syllable of espeak-ng's IPA has the primary stress, which a
   secondary, and how many there are, as compile.py's stressed() counts. */
static void stressed(word *w, const char *ipa, size_t n)
{
    int inv = 0;
    uint32_t mark = 0;
    w->prim = -1;
    w->nsyl = 0;
    w->sec = grow(0, n + 1);
    for (size_t i = 0; i < n;) {
        int len, v = 0;
        uint32_t c = next_cp(ipa + i, n - i, &len);
        i += (size_t)len;
        if (c == 0x2C8 || c == 0x2CC) {
            mark = c;
            continue;
        }
        for (int k = 0; IPA_V[k]; k++)
            v |= IPA_V[k] == c;
        if (v) {
            if (!inv) {
                w->sec[w->nsyl] = mark == 0x2CC;
                if (mark == 0x2C8 && w->prim < 0)
                    w->prim = w->nsyl;
                mark = 0;
                w->nsyl++;
            }
            inv = 1;
        } else if (c != 0x2D0)
            inv = 0;
    }
}

static int by_word(const void *a, const void *b)
{
    return strcmp(((const word *)a)->word, ((const word *)b)->word);
}

static word *find(word *words, int n, const char *s)
{
    word key = { 0 };
    key.word = (char *)s;
    return bsearch(&key, words, (size_t)n, sizeof *words, by_word);
}

static void got_sounds(int i, const buf *out, void *words)
{
    sounds_of(&((word *)words)[i], out->p);
}

static void got_stress(int i, const buf *out, void *words)
{
    stressed(&((word *)words)[i], out->p, out->n);
}

static void got_all(int i, const buf *out, void *all)
{
    (void)i;
    put(all, out->p, out->n);
}

/* How many lines of espeak-ng's output have text, the lines split as
   str.splitlines() splits them; with words, each line's stress is the
   next word's. */
static int ipa_lines(const buf *ipa, word *words)
{
    size_t i = 0, start = 0;
    int lines = 0;
    for (;;) {
        int len = 0;
        uint32_t c = i < ipa->n ? next_cp(ipa->p + i, ipa->n - i, &len) : 10;
        if (i < ipa->n && !pybreak(c)) {
            i += (size_t)len;
            continue;
        }
        if (has_text(ipa->p + start, i - start)) {
            if (words)
                stressed(&words[lines], ipa->p + start, i - start);
            lines++;
        }
        if (i == ipa->n)
            return lines;
        i += (size_t)len + (c == 13 && i + 1 < ipa->n && ipa->p[i + 1] == 10);
        start = i;
    }
}

/* Each word's sounds alone, from flite, and its stress, from espeak-ng,
   one word a line, or one word at a time if espeak-ng gives another
   number of lines. */
static void alone(word *words, int n)
{
    buf *cmds = zeros((size_t)n, sizeof *cmds), ipa = { 0 };
    char path[4200];
    FILE *f;
    for (int i = 0; i < n; i++) {
        puts_(&cmds[i], "flite -ps -t ");
        quote(&cmds[i], words[i].word, 0);
        puts_(&cmds[i], " -o none");
    }
    run_all(cmds, n, got_sounds, words);

    temp_path(path, sizeof path, "words.txt");
    if (!(f = fopen(path, "w")))
        die("cannot write %s", path);
    for (int i = 0; i < n; i++)
        fprintf(f, "%s\n", words[i].word);
    if (fclose(f))
        die("cannot write %s", path);
    cmds[0].n = 0;
    puts_(&cmds[0], "espeak-ng -v en-us -q --ipa < ");
    quote(&cmds[0], path, 1);
    put(&ipa, "", 0);
    run_all(cmds, 1, got_all, &ipa);
    remove(path);

    if (ipa_lines(&ipa, 0) == n)
        ipa_lines(&ipa, words);
    else {
        for (int i = 0; i < n; i++) {
            cmds[i].n = 0;
            puts_(&cmds[i], "espeak-ng -v en-us -q --ipa ");
            quote(&cmds[i], words[i].word, 0);
        }
        run_all(cmds, n, got_stress, words);
    }
    for (int i = 0; i < n; i++)
        free(cmds[i].p);
    free(cmds);
    free(ipa.p);
}

/* ---------------------------------------------------------------- one line */

typedef struct {
    uint32_t hash;
    int order;                  /* which came first, among every line's phrases */
    char *words;                /* the phrase as it is hashed */
    buf rec;
} phrase;

typedef struct {
    int at;                     /* a number in PHONES */
    double b, e;                /* when flite says it */
    int word, kind, len, pitch;
} sound;

/* difflib.SequenceMatcher(a=a, b=b, autojunk=False).get_matching_blocks():
   which of b each of a is matched to, in a[alo, ahi) and b[blo, bhi). */
static void match(const int *a, const int *b, int alo, int ahi, int blo, int bhi, int *row, int *to)
{
    int bi = alo, bj = blo, best = 0, *prev = row, *cur = row + (bhi - blo + 1);
    for (int j = 0; j <= bhi - blo; j++)
        prev[j] = 0;
    for (int i = alo; i < ahi; i++) {
        cur[0] = 0;
        for (int j = blo; j < bhi; j++) {
            int k = cur[j - blo + 1] = a[i] == b[j] ? prev[j - blo] + 1 : 0;
            if (k > best) {
                bi = i - k + 1;
                bj = j - k + 1;
                best = k;
            }
        }
        int *t = prev;
        prev = cur;
        cur = t;
    }
    if (!best)
        return;
    for (int k = 0; k < best; k++)
        to[bi + k] = bj + k;
    if (alo < bi && blo < bj)
        match(a, b, alo, bi, blo, bj, row, to);
    if (bi + best < ahi && bj + best < bhi)
        match(a, b, bi + best, ahi, bj + best, bhi, row, to);
}

static int function_word(const char *w)
{
    char s[TEXT + 1];
    int n = 0;
    for (; *w; w++)
        if (*w != '\'')
            s[n++] = *w;
    s[n] = 0;
    for (int i = 0; FUNCTION[i]; i++)
        if (!strcmp(FUNCTION[i], s))
            return 1;
    return 0;
}

/* The line's phrases, appended to *out: what flite printed saying it, and
   its recording at wav. Within a line, a phrase said twice keeps its last
   record. */
static void compile_line(const unsigned char *page, int npage, word *words, int nwords, const char *said,
                         const char *wav, phrase **out, int *nout, int *order)
{
    static span w[TEXT + 1];
    static int first[TEXT + 2];
    int np = walk(page, npage, w, first), nw = first[np], nseg = 0, nflat = 0, nf;
    word **tok = grow(0, sizeof *tok * (size_t)nw + 1);
    sound *seg = 0;
    int *flat = 0, *owner = 0, *names, *to, *row;
    double *ends = 0, *f0, *ft, *fx, mean = 200.0, sum = 0, c = 0;
    const char **lab = 0;
    char **labname = 0;
    int nvoiced = 0, mine = *nout;

    for (int i = 0; i < nw; i++) {
        char *s = lower(page, w[i]);
        tok[i] = find(words, nwords, s);
        free(s);
        flat = grow(flat, sizeof *flat * ((size_t)nflat + (size_t)tok[i]->nsounds) + 1);
        owner = grow(owner, sizeof *owner * ((size_t)nflat + (size_t)tok[i]->nsounds) + 1);
        for (int k = 0; k < tok[i]->nsounds; k++) {
            flat[nflat] = tok[i]->sounds[k];
            owner[nflat++] = i;
        }
    }

    /* flite's "sound:end" a sound, and its labels for pitch.c as compile.py
       writes them, each end with five decimals. */
    double t0 = 0.0;
    for (const char *s = said; *s;) {
        size_t n = 0;
        while (is_space(*s))
            s++;
        while (s[n] && !is_space(s[n]))
            n++;
        if (n) {
            const char *colon = memchr(s, ':', n);
            size_t pn = colon ? (size_t)(colon - s) : n;
            double e = colon ? strtod(colon + 1, 0) : 0;
            if (!colon)
                die("flite said %.*s, which has no end", (int)n, s);
            seg = grow(seg, sizeof *seg * ((size_t)nseg + 1));
            ends = grow(ends, sizeof *ends * ((size_t)nseg + 1));
            labname = grow(labname, sizeof *labname * ((size_t)nseg + 1));
            labname[nseg] = grow(0, pn + 1);
            memcpy(labname[nseg], s, pn);
            labname[nseg][pn] = 0;
            ends[nseg] = printed("%.5f", e);
            seg[nseg].at = phone(s, pn);
            seg[nseg].b = t0;
            seg[nseg].e = e;
            nseg++;
            t0 = e;
        }
        s += n;
    }
    lab = grow(0, sizeof *lab * (size_t)nseg + 1);
    for (int i = 0; i < nseg; i++)
        lab[i] = labname[i];
    f0 = pitch_track(wav, nseg, ends, lab, &nf);
    ft = grow(0, sizeof *ft * (size_t)nf + 1);
    fx = grow(0, sizeof *fx * (size_t)nf + 1);
    for (int t = 0; t < nf; t++) {
        ft[t] = printed("%.3f", (t + 0.5) * 0.005);
        fx[t] = printed("%.1f", f0[t]);
    }

    /* The sounds but the pauses, each given to a word. */
    int ns = 0;
    for (int i = 0; i < nseg; i++) {
        if (!strcmp(labname[i], "pau"))
            continue;
        if (seg[i].at < 0)
            die("flite said a sound the runtime has not: %s", labname[i]);
        seg[ns++] = seg[i];
    }
    names = grow(0, sizeof *names * (size_t)ns + 1);
    to = grow(0, sizeof *to * (size_t)ns + 1);
    row = grow(0, sizeof *row * 2 * ((size_t)nflat + 1));
    for (int i = 0; i < ns; i++) {
        names[i] = seg[i].at;
        to[i] = -1;
    }
    match(names, flat, 0, ns, 0, nflat, row, to);
    for (int i = 0; i < ns; i++) {         /* a sound flite changed in context: its neighbour's word */
        seg[i].word = to[i] >= 0 ? owner[to[i]] : -1;
        if (seg[i].word >= 0)
            continue;
        if (i && seg[i - 1].word >= 0)
            seg[i].word = seg[i - 1].word;
        else {
            int k = i;
            while (k < ns && to[k] < 0)
                k++;
            seg[i].word = k < ns ? owner[to[k]] : nw - 1 > 0 ? nw - 1 : 0;
        }
    }

    /* The mean pitch: exp of the mean log, summed as Python's sum() sums. */
    for (int t = 0; t < nf; t++)
        if (fx[t] > 0) {
            double x = log(fx[t]), s = sum + x;
            c += fabs(sum) >= fabs(x) ? (sum - s) + x : (x - s) + sum;
            sum = s;
            nvoiced++;
        }
    if (c != 0 && isfinite(c))
        sum += c;
    if (nvoiced)
        mean = exp(sum / nvoiced);

    for (int i = 0; i < ns; i++) {
        seg[i].kind = 0;
        seg[i].pitch = 0;
        seg[i].len = (int)fmax(1, fmin(255, pyround((seg[i].e - seg[i].b) * 200)));
    }
    for (int i = 0; i < nw; i++) {
        int nv = 0, prim = 0, k = 0;
        const unsigned char *sec = 0;
        for (int s = 0; s < ns; s++)
            nv += seg[s].word == i && seg[s].at < 16;
        if (function_word(tok[i]->word))
            prim = -1;
        else if (tok[i]->prim >= 0 && tok[i]->nsyl == nv) {
            prim = tok[i]->prim;
            sec = tok[i]->sec;
        }
        for (int s = 0; s < ns; s++) {
            int m = 0;
            if (seg[s].word != i || seg[s].at >= 16)
                continue;
            seg[s].kind = k == prim ? 2 : sec && sec[k] ? 3 : 1;
            for (int t = 0; t < nf; t++)
                m += seg[s].b <= ft[t] && ft[t] <= seg[s].e && fx[t] > 0;
            for (int t = 0, j = 0; t < nf; t++)
                if (seg[s].b <= ft[t] && ft[t] <= seg[s].e && fx[t] > 0 && j++ == m / 2)
                    seg[s].pitch = 128 + (int)fmax(-127, fmin(127, pyround(48 * log2(fx[t] / mean))));
            k++;
        }
    }

    /* A record a phrase. */
    for (int p = 0; p < np; p++) {
        phrase ph = { 0 };
        buf key = { 0 };
        int found;
        if (first[p] == first[p + 1])
            continue;
        for (int i = first[p]; i < first[p + 1]; i++) {
            for (int s = 0; s < ns; s++)
                if (seg[s].word == i) {
                    putc_(&ph.rec, seg[s].at | seg[s].kind << 6);
                    putc_(&ph.rec, seg[s].len);
                    if (seg[s].kind)
                        putc_(&ph.rec, seg[s].pitch);
                }
            putc_(&ph.rec, 0xFE);
            if (i > first[p])
                putc_(&key, ' ');
            for (int k = w[i].a; k < w[i].b; k++)
                if (page[k] != '\'')
                    putc_(&key, page[k] >= 'A' && page[k] <= 'Z' ? page[k] | 32 : page[k]);
        }
        putc_(&ph.rec, 0xFF);
        ph.hash = fnv(key.p, key.n);
        ph.words = key.p;
        for (found = mine; found < *nout && (*out)[found].hash != ph.hash; found++)
            ;
        if (found < *nout) {
            free((*out)[found].words);
            free((*out)[found].rec.p);
            ph.order = (*out)[found].order;
            (*out)[found] = ph;
        } else {
            ph.order = (*order)++;
            *out = grow(*out, sizeof **out * ((size_t)*nout + 1));
            (*out)[(*nout)++] = ph;
        }
    }

    for (int i = 0; i < nseg; i++)
        free(labname[i]);
    free(labname);
    free(lab);
    free(ends);
    free(f0);
    free(ft);
    free(fx);
    free(seg);
    free(flat);
    free(owner);
    free(names);
    free(to);
    free(row);
    free(tok);
}

/* The wav file of line i, one of BATCH, since no more run at once. */
static void slot(char *path, size_t size, int i)
{
    char name[16];
    snprintf(name, sizeof name, "l%d.wav", i % BATCH);
    temp_path(path, size, name);
}

/* The line as flite is given it: its page as Python decodes it, a
   sequence cut short at the end replaced by U+FFFD, and a NUL. */
static void said_text(const char *line, unsigned char *page)
{
    int n = blank(line, page);
    for (int back = 1; back <= 3 && back <= n; back++) {
        int c = page[n - back], need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
        if ((c & 0xC0) == 0x80)
            continue;
        if (need > back) {
            memcpy(page + n - back, "\xEF\xBF\xBD", 3);
            n += 3 - back;
        }
        break;
    }
    page[n] = 0;
}

typedef struct {
    char **lines;
    int *todo;                  /* the lines with words */
    word *words;
    int nwords;
    phrase *out;
    int nout, order;
} job;

static void got_line(int i, const buf *said, void *ctx)
{
    static unsigned char page[TEXT + 4];
    job *j = ctx;
    char wav[4200];
    slot(wav, sizeof wav, i);
    compile_line(page, blank(j->lines[j->todo[i]], page), j->words, j->nwords, said->p, wav, &j->out, &j->nout,
                 &j->order);
    remove(wav);
}

/* ---------------------------------------------------------------- the table */

static int by_hash(const void *a, const void *b)
{
    const phrase *x = a, *y = b;
    return x->hash != y->hash ? (x->hash > y->hash) - (x->hash < y->hash) : x->order - y->order;
}

static void le32(FILE *f, uint32_t x)
{
    for (int i = 0; i < 4; i++)
        fputc((int)(x >> 8 * i & 255), f);
}

/* The lines of the file, as compile.py reads them: UTF-8, split at "\n",
   "\r\n" or "\r", stripped, and blank lines and "#" lines left out. */
static char **read_lines(const char *path, int *count)
{
    FILE *f = fopen(path, "rb");
    buf text = { 0 };
    char chunk[65536], **lines = 0;
    size_t got, start = 0;
    if (!f)
        die("cannot read %s", path);
    put(&text, "", 0);
    while ((got = fread(chunk, 1, sizeof chunk, f)) > 0)
        put(&text, chunk, got);
    fclose(f);
    *count = 0;
    for (size_t i = 0; i <= text.n; i++) {
        uint32_t cp;
        int len;
        if (i < text.n && text.p[i] != '\n' && text.p[i] != '\r') {
            if (!(len = utf8((unsigned char *)text.p + i, text.n - i, &cp)))
                die("%s is not UTF-8", path);
            i += (size_t)len - 1;
            continue;
        }
        size_t a = i, b = start;    /* from the first to past the last that is not white space */
        for (size_t k = start; k < i; k += (size_t)len)
            if (!pyspace(next_cp(text.p + k, i - k, &len))) {
                a = a < k ? a : k;
                b = k + (size_t)len;
            }
        if (b > a && text.p[a] != '#') {
            lines = grow(lines, sizeof *lines * ((size_t)*count + 1));
            lines[*count] = grow(0, b - a + 1);
            memcpy(lines[*count], text.p + a, b - a);
            lines[(*count)++][b - a] = 0;
        }
        if (i < text.n && text.p[i] == '\r' && i + 1 < text.n && text.p[i + 1] == '\n')
            i++;
        start = i + 1;
    }
    free(text.p);
    return lines;
}

static int compile_usage(void)
{
    fprintf(stderr, "usage: yapper compile LINES.txt -o TABLE.yph\n");
    return 2;
}

int compile_command(int argc, char **argv)
{
    const char *in = 0, *outpath = 0;
    char **lines;
    int nlines, nwords = 0, nout = 0, *todo, ntodo = 0;
    word *words = 0;
    phrase *out = 0;
    static unsigned char page[TEXT + 4];
    static span w[TEXT + 1];
    static int first[TEXT + 2];
    FILE *f;

    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], "-o") && i + 1 < argc)
            outpath = argv[++i];
        else if (!in && argv[i][0] != '-')
            in = argv[i];
        else
            return compile_usage();
    if (!in || !outpath)
        return compile_usage();
    lines = read_lines(in, &nlines);

    /* Every word of every line, once, sorted as compile.py sorts them. */
    todo = grow(0, sizeof *todo * (size_t)nlines + 1);
    for (int l = 0; l < nlines; l++) {
        int n = blank(lines[l], page), np = walk(page, n, w, first);
        if (first[np])
            todo[ntodo++] = l;
        for (int i = 0; i < first[np]; i++) {
            words = grow(words, sizeof *words * ((size_t)nwords + 1));
            memset(&words[nwords], 0, sizeof *words);
            words[nwords++].word = lower(page, w[i]);
        }
    }
    if (nwords)
        qsort(words, (size_t)nwords, sizeof *words, by_word);
    int unique = 0;
    for (int i = 0; i < nwords; i++)
        if (unique && !strcmp(words[unique - 1].word, words[i].word))
            free(words[i].word);
        else
            words[unique++] = words[i];
    nwords = unique;

    if (ntodo) {
        job j = { lines, todo, words, nwords, 0, 0, 0 };
        buf *cmds = zeros((size_t)ntodo, sizeof *cmds);
        installed();
        make_tmpdir();
        alone(words, nwords);
        for (int i = 0; i < ntodo; i++) {
            char wav[4200];
            said_text(lines[todo[i]], page);
            slot(wav, sizeof wav, i);
            puts_(&cmds[i], "flite -voice slt -psdur -t ");
            quote(&cmds[i], (const char *)page, 0);
            puts_(&cmds[i], " -o ");
            quote(&cmds[i], wav, 1);
        }
        run_all(cmds, ntodo, got_line, &j);
        for (int i = 0; i < ntodo; i++)
            free(cmds[i].p);
        free(cmds);
        out = j.out;
        nout = j.nout;
        cleanup();
    }

    /* A phrase in more than one line takes its first line's record. */
    if (nout)
        qsort(out, (size_t)nout, sizeof *out, by_hash);
    int n = 0;
    for (int i = 0; i < nout; i++) {
        if (n && out[n - 1].hash == out[i].hash) {
            if (strcmp(out[n - 1].words, out[i].words))
                die("the phrases \"%s\" and \"%s\" hash alike: reword one", out[i].words, out[n - 1].words);
            free(out[i].words);
            free(out[i].rec.p);
        } else
            out[n++] = out[i];
    }

    if (!(f = fopen(outpath, "wb")))
        die("cannot write %s", outpath);
    uint32_t at = 0;
    fwrite("YPH1", 1, 4, f);
    le32(f, (uint32_t)n);
    for (int i = 0; i < n; i++)
        le32(f, out[i].hash);
    for (int i = 0; i < n; i++) {
        le32(f, at);
        at += (uint32_t)out[i].rec.n;
    }
    le32(f, at);
    for (int i = 0; i < n; i++)
        fwrite(out[i].rec.p, 1, out[i].rec.n, f);
    if (fclose(f))
        die("cannot write %s", outpath);
    printf("%s: %d phrases from %d lines\n", outpath, n, nlines);

    for (int i = 0; i < n; i++) {
        free(out[i].words);
        free(out[i].rec.p);
    }
    for (int i = 0; i < nwords; i++) {
        free(words[i].word);
        free(words[i].sounds);
        free(words[i].sec);
    }
    for (int l = 0; l < nlines; l++)
        free(lines[l]);
    free(out);
    free(words);
    free(lines);
    free(todo);
    return 0;
}
