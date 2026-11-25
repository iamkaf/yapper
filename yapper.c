/* yapper.c: the synthesizer; see yapper.h. Freestanding C, no C library. */

#include "yapper.h"

#define BLK  35                 /* samples a control step: 0.8 ms */
#define TICK 21                 /* control steps between checks for the end: 1/60 s */
#define MS(n) ((n) * 44100 / 1000 / BLK)
#define ONE  (1 << 30)
#define HUSH 11                 /* the fade of yap_hush, per control step: about 20 ms */
#define TAG  0x31485059u        /* "YPH1", a phrase table's first four bytes */

/* ------------------------------------------------------------------ the letters' sounds
   One row a letter, then the schwa, "sh" and a breath drawn in. F1 and F2
   in 10 Hz, F3 in 20 Hz, for a grown man (Peterson and Barney 1952 for the
   vowels; for consonants, the loci the formants move toward); voicing |
   frication << 4, each 0 to 15; the frication's centre in 100 Hz (0: breath
   through the vocal tract); length in 5 ms | kind << 6. */
enum { VOW, SON = 1 << 6, STOP = 2 << 6, FRIC = 3 << 6 };
enum { SCHWA = 26, SH, BREATH, PAUSE = 255 };
static const uint8_t phone[29][6] = {
    {66, 172, 122, 15, 0, 22},                  /* a  as in "cat" */
    {20, 90, 110, 6 | 4 << 4, 15, 12 | STOP},   /* b */
    {20, 180, 125, 0 | 6 << 4, 22, 13 | STOP},  /* c  (read as k or s) */
    {20, 170, 130, 6 | 4 << 4, 35, 12 | STOP},  /* d */
    {53, 184, 124, 15, 0, 19},                  /* e  "bet" */
    {30, 110, 115, 0 | 5 << 4, 16, 17 | FRIC},  /* f */
    {20, 200, 125, 6 | 4 << 4, 22, 12 | STOP},  /* g */
    {50, 150, 125, 0 | 5 << 4, 0, 11 | FRIC},   /* h  breath through the vowel */
    {39, 199, 128, 15, 0, 17},                  /* i  "bit" */
    {26, 210, 135, 6 | 4 << 4, 26, 15 | FRIC},  /* j */
    {20, 180, 125, 0 | 6 << 4, 22, 13 | STOP},  /* k */
    {36, 100, 120, 12, 0, 11 | SON},            /* l */
    {28, 100, 110, 10, 0, 12 | SON},            /* m */
    {28, 160, 130, 10, 0, 11 | SON},            /* n */
    {57, 84, 120, 15, 0, 22},                   /* o  "caught" */
    {20, 90, 110, 0 | 6 << 4, 15, 13 | STOP},   /* p */
    {20, 180, 125, 0 | 6 << 4, 22, 13 | STOP},  /* q  (read as k) */
    {42, 130, 80, 12, 0, 11 | SON},             /* r  a low F3 */
    {30, 170, 130, 0 | 5 << 4, 40, 17 | FRIC},  /* s */
    {20, 170, 130, 0 | 6 << 4, 36, 12 | STOP},  /* t */
    {64, 119, 120, 15, 0, 18},                  /* u  "but" */
    {30, 110, 115, 7 | 2 << 4, 16, 12 | FRIC},  /* v */
    {30, 70, 110, 11, 0, 10 | SON},             /* w */
    {20, 180, 125, 0 | 6 << 4, 22, 13 | STOP},  /* x  (read as k s) */
    {27, 229, 150, 14, 0, 16},                  /* y  "beet" */
    {30, 170, 130, 6 | 3 << 4, 40, 14 | FRIC},  /* z */
    {50, 150, 125, 13, 0, 10},                  /* schwa */
    {30, 190, 125, 0 | 5 << 4, 26, 17 | FRIC},  /* sh */
    {50, 150, 125, 0 | 2 << 4, 0, 30 | FRIC},   /* a breath drawn in */
};

/* Women's vowels (YAP_WOMAN): F1, F2 and F3 in Hz, the averages Hillenbrand
   et al. 1995 measured for American English women. The table above is men's,
   and a woman's are not men's scaled by one number (Fant 1966: the ratio
   changes from vowel to vowel and formant to formant). For a tract of 154;
   another scales them. The schwa, and r. */
static const uint16_t woman[8][4] = {
    {0, 669, 2349, 2972},              /* a  "had" */
    {4, 731, 2058, 2979},              /* e  "head" */
    {8, 483, 2365, 3053},              /* i  "hid" */
    {14, 781, 1136, 2824},             /* o  "hawed" */
    {17, 523, 1588, 1929},             /* r  "heard" */
    {20, 753, 1426, 2933},             /* u  "hud" */
    {24, 437, 2761, 3372},             /* y  "heed" */
    {SCHWA, 600, 1750, 2900},
};

/* ------------------------------------------------------------------ a phrase table's sounds
   The sounds a phrase table names, in this order (compile.py's PHONES): F1
   F2 F3 at the start and at the end (Hz, a man's, for a tract of 128; an
   end of 0 is the start), voicing | frication << 4, the frication's centre
   (100 Hz; 0, breath through the tract), and the kind. The letters' vowels
   keep their numbers; the rest are Peterson and Barney 1952's men's (aa uh
   uw er) and Hillenbrand et al. 1995's (ey ow), and the diphthongs glide
   through their length. */
enum { CPH = 32, NCPH = 40 };
enum { CK_VOW, CK_SON, CK_STOP, CK_FRIC };
static const struct { uint16_t a[3], b[3]; uint8_t vf, fc, kind; } cph[NCPH] = {
    { { 270, 2290, 3000 }, { 0 }, 15, 0, CK_VOW },              /* iy */
    { { 390, 1990, 2560 }, { 0 }, 15, 0, CK_VOW },              /* ih */
    { { 476, 2089, 2691 }, { 270, 2290, 3000 }, 15, 0, CK_VOW },  /* ey */
    { { 530, 1840, 2480 }, { 0 }, 15, 0, CK_VOW },              /* eh */
    { { 660, 1720, 2440 }, { 0 }, 15, 0, CK_VOW },              /* ae */
    { { 730, 1090, 2440 }, { 0 }, 15, 0, CK_VOW },              /* aa */
    { { 570, 840, 2400 }, { 0 }, 15, 0, CK_VOW },               /* ao */
    { { 497, 910, 2459 }, { 300, 870, 2240 }, 15, 0, CK_VOW },    /* ow */
    { { 440, 1020, 2240 }, { 0 }, 15, 0, CK_VOW },              /* uh */
    { { 300, 870, 2240 }, { 0 }, 15, 0, CK_VOW },               /* uw */
    { { 640, 1190, 2400 }, { 0 }, 15, 0, CK_VOW },              /* ah */
    { { 500, 1500, 2500 }, { 0 }, 13, 0, CK_VOW },              /* ax */
    { { 490, 1350, 1690 }, { 0 }, 15, 0, CK_VOW },              /* er */
    { { 730, 1090, 2440 }, { 390, 1990, 2560 }, 15, 0, CK_VOW },  /* ay */
    { { 730, 1090, 2440 }, { 440, 1020, 2240 }, 15, 0, CK_VOW },  /* aw */
    { { 570, 840, 2400 }, { 270, 2290, 3000 }, 15, 0, CK_VOW },   /* oy */
    { { 200, 900, 2200 }, { 0 }, 6 | 4 << 4, 15, CK_STOP },     /* b */
    { { 200, 1700, 2600 }, { 0 }, 6 | 4 << 4, 35, CK_STOP },    /* d */
    { { 200, 2000, 2500 }, { 0 }, 6 | 4 << 4, 22, CK_STOP },    /* g */
    { { 200, 900, 2200 }, { 0 }, 6 << 4, 15, CK_STOP },         /* p */
    { { 200, 1700, 2600 }, { 0 }, 6 << 4, 36, CK_STOP },        /* t */
    { { 200, 1800, 2500 }, { 0 }, 6 << 4, 22, CK_STOP },        /* k */
    { { 300, 1100, 2300 }, { 0 }, 5 << 4, 16, CK_FRIC },        /* f */
    { { 300, 1100, 2300 }, { 0 }, 7 | 2 << 4, 16, CK_FRIC },    /* v */
    { { 300, 1500, 2600 }, { 0 }, 4 << 4, 30, CK_FRIC },        /* th */
    { { 300, 1500, 2600 }, { 0 }, 7 | 2 << 4, 30, CK_FRIC },    /* dh */
    { { 300, 1700, 2600 }, { 0 }, 5 << 4, 40, CK_FRIC },        /* s */
    { { 300, 1700, 2600 }, { 0 }, 6 | 3 << 4, 40, CK_FRIC },    /* z */
    { { 300, 1900, 2500 }, { 0 }, 5 << 4, 26, CK_FRIC },        /* sh */
    { { 300, 1900, 2500 }, { 0 }, 6 | 3 << 4, 26, CK_FRIC },    /* zh */
    { { 500, 1500, 2500 }, { 0 }, 5 << 4, 0, CK_FRIC },         /* hh */
    { { 300, 1900, 2500 }, { 0 }, 6 << 4, 26, CK_STOP },        /* ch */
    { { 260, 2100, 2700 }, { 0 }, 6 | 4 << 4, 26, CK_STOP },    /* jh */
    { { 280, 1000, 2200 }, { 0 }, 10, 0, CK_SON },              /* m */
    { { 280, 1600, 2600 }, { 0 }, 10, 0, CK_SON },              /* n */
    { { 280, 2000, 2500 }, { 0 }, 10, 0, CK_SON },              /* ng */
    { { 360, 1000, 2400 }, { 0 }, 12, 0, CK_SON },              /* l */
    { { 420, 1300, 1600 }, { 0 }, 12, 0, CK_SON },              /* r */
    { { 300, 700, 2200 }, { 0 }, 11, 0, CK_SON },               /* w */
    { { 270, 2290, 3000 }, { 0 }, 14, 0, CK_SON },              /* y */
};

/* The same sounds for YAP_WOMAN, for a tract of 154: Hillenbrand et al.
   1995's women's vowels, start and end, and h and y from her schwa and her
   "heed". The consonants are the man's, scaled. */
static const uint16_t cwoman[18][7] = {
    { 0, 437, 2761, 3372 },   { 1, 483, 2365, 3053 },   { 2, 536, 2530, 3047, 437, 2761, 3372 },
    { 3, 731, 2058, 2979 },   { 4, 669, 2349, 2972 },   { 5, 936, 1551, 2815 },
    { 6, 781, 1136, 2824 },   { 7, 555, 1035, 2828, 459, 1105, 2735 },   { 8, 519, 1225, 2827 },
    { 9, 459, 1105, 2735 },   { 10, 753, 1426, 2933 },  { 11, 600, 1750, 2900 },
    { 12, 523, 1588, 1929 },  { 13, 936, 1551, 2815, 483, 2365, 3053 },
    { 14, 936, 1551, 2815, 519, 1225, 2827 },   { 15, 781, 1136, 2824, 437, 2761, 3372 },
    { 30, 600, 1750, 2900 },  { 39, 437, 2761, 3372 },
};

/* 2^(i/48), Q15: a quarter semitone a step. */
static const uint16_t qst[48] = {
    32768, 33245, 33728, 34219, 34716, 35221, 35734, 36254, 36781, 37316, 37859, 38409,
    38968, 39535, 40110, 40693, 41285, 41886, 42495, 43113, 43740, 44376, 45022, 45677,
    46341, 47015, 47699, 48393, 49097, 49811, 50535, 51270, 52016, 52773, 53540, 54319,
    55109, 55911, 56724, 57549, 58386, 59235, 60097, 60971, 61858, 62757, 63670, 64596,
};

/* x * 2^(q/48), for x >= 0 */
static int32_t up(int32_t x, int32_t q)
{
    int32_t o = q >= 0 ? q / 48 : -((47 - q) / 48);
    x = (int32_t)((int64_t)x * qst[q - o * 48] >> 15);
    return o >= 0 ? (o > 20 ? x : x << o) : (o < -30 ? 0 : x >> -o);
}

/* ------------------------------------------------------------------ presets */

const yap_voice yap_man = {
    .pitch = 128, .range = 70, .pace = 95, .tract = 128, .oq = 150, .tilt = 62,
    .breath = 30, .flutter = 10, .volume = 219, .pause = 100,
};
const yap_voice yap_woman = {
    .pitch = 196, .range = 100, .pace = 90, .tract = 150, .oq = 230, .tilt = 52,
    .breath = 60, .flutter = 8, .volume = 151, .b1 = 400, .soft = 200,
    .style = YAP_WOMAN | YAP_SMOOTH, .pause = 100,
};
const yap_voice yap_child = {
    .pitch = 270, .range = 120, .pace = 86, .tract = 176, .oq = 200, .tilt = 56,
    .breath = 70, .flutter = 16, .volume = 130, .b1 = 200, .soft = 160,
    .style = YAP_WOMAN | YAP_SMOOTH, .pause = 100,
};
const yap_voice yap_elder = {
    .pitch = 132, .range = 44, .pace = 116, .tract = 132, .oq = 190, .tilt = 44,
    .breath = 130, .flutter = 16, .tremor = 40, .volume = 172, .style = YAP_SMOOTH,
    .pause = 120,
};
const yap_voice yap_giant = {
    .pitch = 70, .range = 26, .pace = 126, .tract = 96, .oq = 150, .tilt = 44,
    .breath = 60, .flutter = 4, .tremor = 6, .volume = 231, .style = YAP_SMOOTH,
    .pause = 130,
};
const yap_voice yap_robot = {
    .pitch = 110, .range = 0, .pace = 100, .tract = 124, .oq = 96, .tilt = 120,
    .volume = 196,
};

/* ------------------------------------------------------------------ the radio
   Over a channel (radio 1 to 3): the telephone band, 400 Hz to 3.4 kHz,
   four Butterworth stages, so the fundamental goes as it does on a radio; a
   transmitter's soft saturation; a faint carrier hiss in the band while it
   is keyed; a burst of squelch as it keys on and as it keys off.
   2 is a narrower band (600 Hz to 2.8 kHz), harder driven, with a louder
   carrier and the honk of a small speaker (a resonance at 1.7 kHz); 3 is
   harder still, with a plain carrier and the crackle of a weak signal.
   Each row: drive (/2), the limit, the carrier's shift, the honk (/8),
   crackle, make-up gain (/256). */
#define RADIO_ON  (44100 * 70 / 1000)           /* samples of squelch as the channel opens */
#define RADIO_OFF (44100 * 110 / 1000)          /* and as it closes, after the last word */
#define HP400 15737, -31474, 15737, -31448, 15115   /* Q14 */
static const int16_t radio_k[3][6] = {
    { 4, 12000, 10, 0, 0, 555 },
    { 10, 7000, 8, 6, 0, 465 },
    { 18, 5000, 7, 8, 1, 305 },
};

static int32_t channel(yapper *y, int32_t s, int32_t n, int closing)
{
    static const int32_t wide[4][5] = { { HP400 }, { HP400 },               /* high-pass 400 Hz, twice, */
                                        { 709, 1418, 709, -21814, 8267 },  /* low-pass 3.4 kHz, twice */
                                        { 709, 1418, 709, -21814, 8267 } };
    static const int32_t thin[4][5] = { { 15423, -30846, 15423, -30789, 14518 },   /* 600 Hz */
                                        { 15423, -30846, 15423, -30789, 14518 },
                                        { 505, 1009, 505, -23688, 9323 },      /* 2.8 kHz */
                                        { 505, 1009, 505, -23688, 9323 } };
    static const int32_t honk[5] = { 630, 0, -630, -30589, 15124 };            /* band-pass 1.7 kHz, Q 3 */
    const int16_t *k = radio_k[y->radio - 1];
    const int32_t (*c)[5] = y->radio > 1 ? thin : wide;
    int32_t (*rq)[4] = y->rq;
    int32_t x, i, h, m;

    x = s + (y->rsq > 0 || closing ? n >> 6 : n >> k[2]);  /* the carrier, a burst as it keys on and off */
    y->rsq -= y->rsq > 0;
    if (k[4] && (n & 0x1fff) == 0x1000)         /* a weak signal's crackle: a pop now and then */
        x += n > 0 ? 2500 : -2500;
    x = x * k[0] / 2;                           /* driven, and softly limited */
    x = (int32_t)((int64_t)x * k[1] / (k[1] + (x < 0 ? -x : x)));
    for (i = 0; i < 4; i++) {
        int32_t o = (int32_t)((c[i][0] * ((int64_t)x + rq[i][1]) + c[i][1] * (int64_t)rq[i][0]
                               - c[i][3] * (int64_t)rq[i][2] - c[i][4] * (int64_t)rq[i][3]) >> 14);
        rq[i][1] = rq[i][0];
        rq[i][0] = x;
        rq[i][3] = rq[i][2];
        rq[i][2] = o;
        x = o;
    }
    h = (int32_t)((honk[0] * ((int64_t)x - rq[4][1]) - honk[3] * (int64_t)rq[4][2] - honk[4] * (int64_t)rq[4][3]) >> 14);
    rq[4][1] = rq[4][0];
    rq[4][0] = x;
    rq[4][3] = rq[4][2];
    rq[4][2] = h;
    x += h * k[3] / 8;                          /* a small speaker's honk */
    x = x * k[5] >> 8;                          /* as loud as it went in */
    m = x < 0 ? -x : x;                         /* the peaks the band sharpened, held by */
    if (m > 3000) {                             /* a soft knee at 3000, 2000 more at most */
        m = 3000 + (int32_t)((int64_t)(m - 3000) * 2000 / (2000 + m - 3000));
        x = x < 0 ? -m : m;
    }
    return x;
}

/* ------------------------------------------------------------------ text */

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int letter(int c) { c = lower(c); return c >= 'a' && c <= 'z'; }
static int vowel(int c) { c = lower(c); return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y'; }
static int stop(int c) { return c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?'; }

/* Flags kept in mark while a phrase is built: a syllable's nucleus,
   stressed, shortened. */
enum { NUC = 1, STRESS = 2, SHORT = 4 };

static void push(yapper *y, int row, int len, int flags)
{
    if (y->nev < YAP_EVENTS) {
        y->ev[y->nev].row = (uint8_t)row;
        y->ev[y->nev].len = (uint16_t)len;
        y->ev[y->nev].pos = (uint16_t)y->cur;
        y->mark[y->nev++] = (uint8_t)flags;
    }
}

/* A letter's sound at its own length, scaled by the voice's pace. */
static void put(yapper *y, int row, int flags)
{
    int len = (phone[row][5] & 63) * MS(5) * y->v.pace / 100;
    if (flags & SHORT)
        len = len * 5 / 8;
    push(y, row, len, flags);
}

/* A word's letters read as sounds, with a few rules of English spelling.
   content: a word that carries meaning, whose first syllable is stressed.
   Returns its syllables. */
static int word(yapper *y, const char *w, int n, int content)
{
    int i, s = 0, c, nx, pv = 0;
    for (i = 0; i < n; i++) {
        c = lower(w[i]);
        nx = i + 1 < n ? lower(w[i + 1]) : 0;
        if (vowel(c) && !(c == 'y' && i == 0)) {
            int first = !pv, fl = SHORT * !content;
            if (c == 'e' && i == n - 1 && n > 3 && !vowel(w[i - 1]))
                continue;                       /* a silent final e */
            if (first)
                fl |= NUC | (content && !s ? STRESS : 0), s++;
            if (vowel(nx) || !first)
                fl |= SHORT;                    /* half of a diphthong */
            put(y, c == 'a' && n == 1 ? SCHWA : c - 'a', fl);
            pv = 1;
            continue;
        }
        pv = 0;
        if (c == nx)
            continue;                           /* a doubled letter */
        if (c == 's' && nx == 'h') c = SH + 'a', i++;
        else if (c == 'c' && nx == 'h') put(y, 't' - 'a', 0), c = SH + 'a', i++;
        else if (c == 't' && nx == 'h') c = 'v', i++;
        else if (c == 'p' && nx == 'h') c = 'f', i++;
        else if ((c == 'g' && nx == 'h') || (c == 'w' && nx == 'h')) { i++; if (c == 'g') continue; }
        else if ((c == 'n' && nx == 'g') || (c == 'c' && nx == 'k')) c = c == 'c' ? 'k' : c, i++;
        else if (c == 'c') c = nx == 'e' || nx == 'i' || nx == 'y' ? 's' : 'k';
        else if (c == 'q') c = 'k', i += nx == 'u';
        else if (c == 'x') put(y, 'k' - 'a', 0), c = 's';
        put(y, c - 'a', c == 'y' ? SHORT : 0);  /* a 'y' that starts a word glides */
    }
    return s;
}

/* The tune and timing of one phrase, events p0 to nev, with s syllables:
   the pitch starts high and falls through the phrase (declination), each
   stressed syllable is lifted (a pitch accent), and the last syllable falls
   for a statement, rises for a yes-or-no question, and lifts a little
   before a comma; the last syllable is longer, and the loudness falls.
   keep: a phrase from the table, whose lengths and pitches are its own,
   takes only the loudness. */
static void tune(yapper *y, uint32_t p0, int s, int end, int keep)
{
    int32_t fz = y->v.pitch * 16, r = fz * y->v.range >> 8;
    int32_t hi = fz + r * 3 / 5, lo = fz - r / 5, kk = -1, last, pitch, amp;
    uint32_t i;
    for (i = p0; i < y->nev; i++) {
        int m = y->mark[i];
        if (m & NUC)
            kk++;
        last = kk == s - 1;
        pitch = hi + (lo - hi) * (kk < 0 ? 0 : kk) / (s > 1 ? s - 1 : 1);
        amp = 255 - 70 * (kk < 0 ? 0 : kk) / (s > 0 ? s : 1);
        if (m & STRESS)
            pitch += r / 2;
        else if (m & NUC)
            amp = amp * 13 / 16;
        if (m & SHORT && m & NUC)
            amp = amp * 7 / 8;
        if (last && kk >= 0 && !keep) {         /* the last vowel and what follows it */
            y->ev[i].len = (uint16_t)(y->ev[i].len * (m & NUC ? 3 : 5) / (m & NUC ? 2 : 4));
            pitch = end == '?' ? fz + r : end == ',' ? fz + r / 6 : fz - r * 9 / 20;
        }
        if (!keep)
            y->ev[i].f0 = (uint16_t)pitch;
        y->ev[i].amp = (uint8_t)amp;
    }
}

/* A wh-question falls like a statement; a yes-or-no question rises. */
static int ending(const char *t, const char *start)
{
    int c = *t;
    if (c == '!' || c == '.')
        return '.';
    if (c == '?') {
        while (*start && !letter(*start))
            start++;
        return lower(start[0]) == 'w' && lower(start[1]) == 'h' ? '.' :
               lower(start[0]) == 'h' && lower(start[1]) == 'o' && lower(start[2]) == 'w' ? '.' : '?';
    }
    return c == ',' || c == ';' || c == ':' ? ',' : '.';
}

/* The pause after a phrase, scaled by the voice's pause (a percent): a
   comma a quarter of a second, a semicolon or a colon more, the end of a
   sentence half a second. An ellipsis, trailing off or hesitating, is
   longer wherever its dots stand. Once the voice has spoken for a second
   and a half since its last breath, the pause before a new sentence ends
   in a short, soft breath drawn in. The last phrase pauses as any other
   voice's. t is the punctuation that ends the phrase, lim the text's end. */
#define BREATH_AFTER MS(1500)

static void breathe(yapper *y, const char *t, const char *lim)
{
    int dots = 0, more, ms, k = y->v.pause;
    uint32_t since = 0, i;
    const char *u = t;

    while (u < lim && *u && !letter(*u)) {      /* to the next word: "No? ...Yes" hesitates */
        dots += *u == '.';
        u++;
    }
    more = u < lim && *u != 0;
    if (!more) {
        push(y, PAUSE, MS(300), 0);
        return;
    }
    if (*t == ',' && dots < 3) {
        push(y, PAUSE, MS(260 * k / 100), 0);
        return;
    }
    if ((*t == ';' || *t == ':') && dots < 3) {
        push(y, PAUSE, MS(380 * k / 100), 0);
        return;
    }
    ms = dots >= 3 ? 700 : 480;
    push(y, PAUSE, MS(ms * k / 100), 0);
    for (i = y->nev; i-- > 0 && y->ev[i].row != BREATH;)
        since += y->ev[i].row == PAUSE ? 0 : y->ev[i].len;
    if (since >= BREATH_AFTER) {
        push(y, BREATH, MS(170 * k / 100), 0);
        y->ev[y->nev - 1].amp = 45;
    }
}

/* ------------------------------------------------------------------ the phrase table (FORMAT.md) */

static uint32_t u32at(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int yap_phrases(yapper *y, const uint8_t *d, uint32_t size)
{
    uint32_t n, i, base;

    y->table = 0;
    y->table_size = 0;
    if (!d)
        return 0;
    if (size < 12 || u32at(d) != TAG || (n = u32at(d + 4)) > (size - 12) / 8)
        return -1;
    base = 12 + 8 * n;
    if (u32at(d + 8 + 4 * n) != 0 || u32at(d + 8 + 8 * n) != size - base)
        return -1;
    for (i = 1; i < n; i++)
        if (u32at(d + 8 + 4 * i) <= u32at(d + 4 + 4 * i))
            return -1;                          /* hashes, strictly ascending */
    for (i = 0; i < n; i++)
        if (u32at(d + 12 + 4 * n + 4 * i) < u32at(d + 8 + 4 * n + 4 * i))
            return -1;                          /* offsets, never falling */
    y->table = d;
    y->table_size = size;
    return 0;
}

/* A record's sounds, walked: how many words, or -1 unless they are well
   made and fill exactly len bytes. */
static int cwords(const uint8_t *p, uint32_t len)
{
    uint32_t i = 0;
    int words = 0;
    while (i < len && p[i] != 0xff) {
        if (p[i] == 0xfe) {
            words++;
            i++;
        } else if ((p[i] & 63) < NCPH)
            i += p[i] >> 6 ? 3 : 2;
        else
            return -1;
    }
    return i + 1 == len ? words : -1;
}

/* The phrase from t to its punctuation, from the table, or 0. Its words
   hashed as compile.py hashes them: lower case, no apostrophes, a space
   between, FNV-1a. */
static const uint8_t *said(const yapper *y, const char *t)
{
    const uint8_t *d = y->table;
    uint32_t n, h = 2166136261u, a = 0, b, at, len;
    int words = 0;

    if (!d)
        return 0;
    while (*t && !stop(*t)) {
        if (!letter(*t)) {
            t++;
            continue;
        }
        if (words++)
            h = (h ^ ' ') * 16777619u;
        for (; letter(*t) || (*t == '\'' && letter(t[1])); t++)
            if (*t != '\'')
                h = (h ^ (uint32_t)lower(*t)) * 16777619u;
    }
    n = u32at(d + 4);
    if (!words)
        return 0;
    for (b = n; a < b;) {                       /* the hashes, ascending */
        uint32_t m = a + (b - a) / 2, k = u32at(d + 8 + 4 * m);
        if (k == h) {
            a = m;
            break;
        }
        if (k < h)
            a = m + 1;
        else
            b = m;
    }
    if (a >= n || u32at(d + 8 + 4 * a) != h)
        return 0;
    at = u32at(d + 8 + 4 * n + 4 * a);
    len = u32at(d + 12 + 4 * n + 4 * a) - at;
    d += 12 + 8 * n + at;
    return len && cwords(d, len) == words ? d : 0;
}

/* One word of a table's phrase from p: its sounds pushed, each its row CPH
   + its number, with its length scaled by the pace and its vowels' pitch
   by the range; adds its syllables to *s. Returns past it. A pace of 76
   says the sounds as long as the table has them, and a range of 135 its
   tune as wide. */
static const uint8_t *cword(yapper *y, const uint8_t *p, int *s)
{
    int32_t fz = y->v.pitch * 16, r = y->v.range;

    while (*p != 0xfe && *p != 0xff) {
        int k = p[0] >> 6, len = p[1] * 63 * y->v.pace / 760;
        if (y->nev < YAP_EVENTS) {
            int32_t hz = k && p[2] ? up(fz, (p[2] - 128) * r / 135) : 0;
            push(y, CPH + (p[0] & 63), len > 0 ? len : 1, k ? NUC | (k == 2 ? STRESS : k == 1 ? SHORT : 0) : 0);
            y->ev[y->nev - 1].f0 = (uint16_t)(hz > 65535 ? 65535 : hz);
            *s += k != 0;
        }
        p += k ? 3 : 2;
    }
    return p + (*p == 0xfe);
}

/* Each table sound with no pitch of its own takes its vowel's: the last
   one before it, or the phrase's first. */
static void cfill(yapper *y, uint32_t p0)
{
    uint32_t i;
    uint16_t last = 0;
    for (i = p0; i < y->nev && !last; i++)
        last = y->ev[i].f0;
    if (!last)
        last = (uint16_t)(y->v.pitch * 16);
    for (i = p0; i < y->nev; i++) {
        if (y->ev[i].f0)
            last = y->ev[i].f0;
        else
            y->ev[i].f0 = last;
    }
}

/* ------------------------------------------------------------------ saying */

/* Silence at once. The source's phase, jitter, shimmer, flutter and tremor
   carry on, as a voice goes on from one line to the next. */
static void silence(yapper *y)
{
    uint32_t i;

    y->busy = 0;
    y->nev = y->at = y->left = 0;
    y->av = y->af = y->src = y->src2 = 0;
    y->hushed = 0;
    y->lvl = 256;
    y->radio = y->rsq = y->rtail = 0;
    for (i = 0; i < 20; i++)
        y->rq[i >> 2][i & 3] = 0;
    for (i = 0; i < 6; i++)
        y->z1[i] = y->z2[i] = 0;
    y->uprev = y->nprev = 0;
}

void yap_reset(yapper *y)
{
    uint32_t i;

    silence(y);
    y->table = 0;
    y->table_size = 0;
    y->len = 0;
    y->seed = 1;
    y->phase = y->jit = y->inc = y->trem = 0;
    y->steps = y->step = 0;
    y->f0 = 0;
    y->wob[0] = y->wob[1] = 0;
    y->shim = 256;
    for (i = 0; i < 5; i++)
        y->f[i] = 0;
    for (i = 0; i < 18; i++)
        y->k[i / 3][i % 3] = 0;
}

void yap_say(yapper *y, const yap_voice *v, const char *text, uint32_t n)
{
    const char *t = y->page, *sentence = y->page, *lim;
    uint32_t i;
    int depth;

    if (!v || !text) {
        silence(y);
        return;
    }
    y->v = *v;
    if (y->v.pitch < 20) y->v.pitch = 20;
    if (y->v.pitch > 1000) y->v.pitch = 1000;
    if (y->v.b1 > 1000) y->v.b1 = 1000;
    if (y->v.radio > 3) y->v.radio = 3;
    if (!y->v.oq) y->v.oq = 1;

    for (i = 0; i < n && i < YAP_TEXT && text[i]; i++)
        y->page[i] = text[i];
    y->page[i] = 0;
    y->len = i;
    lim = y->page + i;
    /* What is in brackets is how it is said, not what: blanked where it
       stands, so every word keeps its place. */
    for (i = 0, depth = 0; y->page[i]; i++) {
        int open = y->page[i] == '(' || y->page[i] == '[', close = y->page[i] == ')' || y->page[i] == ']';
        depth += open;
        if (depth)
            y->page[i] = ' ';
        if (close && depth)
            depth--;
    }

    y->radio = y->v.radio;
    y->rsq = y->radio ? RADIO_ON : 0;
    y->rtail = y->radio ? RADIO_OFF : 0;
    y->busy = 1;
    y->nev = y->at = y->left = 0;
    y->cur = 0;
    y->lvl = 256;
    y->hushed = 0;
    y->steps = y->step = 0;
    push(y, BREATH, MS(170), 0);                /* a breath before the first word */
    tune(y, 0, 1, '.', 0);
    y->ev[0].amp = 120;
    if (y->v.pause) {                           /* "...Well": a hesitation first */
        const char *u = y->page;
        int dots = 0;
        while (u < lim && *u && !letter(*u))
            dots += *u++ == '.';
        if (dots >= 3)
            push(y, PAUSE, MS(400 * y->v.pause / 100), 0);
    }
    while (*t) {
        uint32_t p0 = y->nev;
        int s = 0, end;
        const uint8_t *rp = said(y, t);
        while (*t && !stop(*t)) {
            const char *w = t, *e;
            int m = 0, content, len;
            char buf[24];
            if (!letter(*t)) {
                t++;
                continue;
            }
            for (len = 0; letter(w[len]) || (w[len] == '\'' && letter(w[len + 1])); len++)
                ;
            t = w + len;
            for (e = t; *e && !letter(*e) && *e != ' ' && *e != '\n'; e++)
                ;                               /* the word shows with what follows it */
            y->cur = (int32_t)(e - y->page);
            if (rp)
                rp = cword(y, rp, &s);
            else {
                int j;
                for (j = 0; j < len && m < 23; j++)
                    if (w[j] != '\'')
                        buf[m++] = w[j];
                content = m > 3 || (w[0] >= 'A' && w[0] <= 'Z' && m > 1);
                s += word(y, buf, m, content);
            }
        }
        if (y->nev > p0) {
            end = ending(t, sentence);
            if (rp) {
                cfill(y, p0);
                tune(y, p0, s, end, 1);
            } else
                tune(y, p0, s, end, 0);
            if (y->v.pause)
                breathe(y, t, lim);
            else
                push(y, PAUSE, end == ',' ? MS(140) : MS(300), 0);
        }
        if (*t == '.' || *t == '!' || *t == '?')
            sentence = t + 1;
        while (stop(*t))
            t++;
    }
    y->left = y->ev[0].len;
    y->f[0] = 500 * y->v.tract / 128;
    y->f[1] = 1500 * y->v.tract / 128;
    y->f[2] = 2500 * y->v.tract / 128;
    y->f0 = y->nev > 1 ? y->ev[1].f0 : y->v.pitch * 16;
    y->seed = 1;                                /* the same text, the same sound */
}

void yap_hush(yapper *y)
{
    y->at = y->nev;
    y->left = 0;
    y->hushed = 1;
}

uint32_t yap_reached(const yapper *y)
{
    return y->busy && y->at < y->nev ? y->ev[y->at].pos : y->len;
}

int yap_mouth(const yapper *y)
{
    uint32_t row;
    if (!y->busy || y->at >= y->nev)
        return 0;
    row = y->ev[y->at].row;
    if (row >= CPH && row < CPH + NCPH)
        return cph[row - CPH].kind == CK_VOW;
    return row < PAUSE && (phone[row][5] & 0xc0) == VOW;
}

int yap_busy(const yapper *y)
{
    return y->busy;
}

/* ------------------------------------------------------------------ sound */

static int32_t noise(yapper *y)
{
    y->seed = y->seed * 1664525u + 1013904223u;
    return (int32_t)(y->seed >> 16) - (int32_t)(y->seed >> 15 & 0x10000);   /* the top half, signed */
}

/* A two-pole resonator at fr Hz, bw Hz wide, gain 1 at 0 Hz (Klatt 1980,
   eq. 2): cos and exp by their series, in Q30, close enough below 5 kHz. */
static void coef(int64_t *c, int32_t fr, int32_t bw)
{
    int64_t th = (int64_t)fr * 152981, x = th * th >> 30, x2 = x * x >> 30;
    int64_t u = (int64_t)bw * 76490, u2 = u * u >> 30;
    int64_t cs = ONE - x / 2 + x2 / 24 - (x2 * x >> 30) / 720;
    int64_t r = ONE - u + u2 / 2 - (u2 * u >> 30) / 6;
    c[1] = 2 * r * cs >> 30;
    c[2] = -(r * r >> 30);
    c[0] = ONE - c[1] - c[2];
}

static int32_t res(yapper *y, int i, int32_t x)
{
    int32_t o = (int32_t)((y->k[i][0] * x + y->k[i][1] * y->z1[i] + y->k[i][2] * y->z2[i]) >> 30);
    y->z2[i] = y->z1[i];
    y->z1[i] = o;
    return o;
}

/* A table sound's control step, as a letter's: the formants glide to its
   targets (a diphthong's moving through its length), voicing and
   frication at its loudness, a stop silent for its first 70%. */
static void compiled(yapper *y, const struct yap_event *e, int32_t *tav, int32_t *taf)
{
    int id = e->row - CPH, j, div = 128;
    int32_t q = e->len ? (int32_t)((e->len - y->left) * 256 / e->len) : 128, to;
    const uint16_t *a = cph[id].a, *b = cph[id].b;

    if (y->v.style & YAP_WOMAN)
        for (j = 0; j < 18; j++)
            if (cwoman[j][0] == id) {
                a = cwoman[j] + 1;
                b = cwoman[j] + 4;
                div = 154;
            }
    for (j = 0; j < 3; j++) {
        to = (a[j] + ((b[j] ? b[j] : a[j]) - a[j]) * q / 256) * y->v.tract / div;
        y->f[j] += (to - y->f[j]) >> 4;
    }
    y->f[4] = cph[id].fc * 100;
    *tav = (cph[id].vf & 15) * (cph[id].vf & 15) * e->amp >> 4;
    *taf = (cph[id].vf >> 4) * e->amp;
    if (cph[id].kind == CK_STOP && y->left > e->len * 3u / 10)
        *taf = 0;                               /* the closure, then the burst */
    y->f0 += (e->f0 - y->f0) >> 5;
}

/* One control step: the next sound, the formants' glide, and the
   resonators' coefficients. Returns 0 once a hush has faded out. */
static int control(yapper *y)
{
    static const uint16_t bw[4] = { 70, 100, 150, 384 };
    int32_t tav = 0, taf = 0, fe, tri, tract = y->v.tract;
    int j;

    if (y->at < y->nev && !y->left && ++y->at < y->nev)
        y->left = y->ev[y->at].len;
    if (y->at < y->nev) {
        const struct yap_event *e = &y->ev[y->at];
        y->left -= y->left > 0;
        if (e->row >= CPH && e->row < CPH + NCPH)
            compiled(y, e, &tav, &taf);
        else if (e->row != PAUSE) {
            const uint8_t *p = phone[e->row];
            int32_t to[3];
            to[0] = p[0] * 10 * tract / 128;
            to[1] = p[1] * 10 * tract / 128;
            to[2] = p[2] * 20 * tract / 128;
            if (y->v.style & YAP_WOMAN)
                for (j = 0; j < 8; j++)
                    if (woman[j][0] == e->row) {
                        to[0] = woman[j][1] * tract / 154;
                        to[1] = woman[j][2] * tract / 154;
                        to[2] = woman[j][3] * tract / 154;
                    }
            for (j = 0; j < 3; j++)             /* the formants glide */
                y->f[j] += (to[j] - y->f[j]) >> 4;
            y->f[4] = p[4] * 100;
            tav = (p[3] & 15) * (p[3] & 15) * e->amp >> 4;
            taf = (p[3] >> 4) * e->amp;
            if ((p[5] & 0xc0) == STOP && y->left > e->len * 3u / 10)
                taf = 0;                        /* the closure, then the burst */
            if (e->f0)                          /* a breath has no pitch: it keeps the voice's */
                y->f0 += (e->f0 - y->f0) >> 5;
        }
    }
    y->av += (tav - y->av) >> 2;
    y->af += (taf - y->af) >> 1;
    if (y->hushed && (y->lvl -= HUSH) <= 0) {
        silence(y);
        return 0;
    }
    /* Flutter: two random walks, slow and slower (Klatt's FL; a 1/f-like
       wander sounds more human than none or than white jitter). */
    y->wob[0] += (noise(y) - y->wob[0]) >> 4;
    y->wob[1] += (noise(y) - y->wob[1]) >> 7;
    fe = y->f0 + (int32_t)((int64_t)y->f0 * ((y->wob[0] + 4 * y->wob[1]) * y->v.flutter >> 8) >> 15);
    /* Tremor: a 5.5 Hz triangle on pitch and loudness. */
    y->trem += 5 * 65536 * BLK / 44100 * 11 / 10;
    tri = (y->trem & 65535) < 32768 ? (int32_t)(y->trem & 65535) - 16384 : 49152 - (int32_t)(y->trem & 65535);
    fe += (int32_t)((int64_t)fe * (tri * y->v.tremor >> 8) >> 18);
    /* F1 to F3, a fixed F4, the fundamental's own band (YAP_PARALLEL),
       and the frication's resonator beside them. */
    y->f[3] = 3400 * tract / 128;
    coef(y->k[5], fe / 16 > 60 ? fe / 16 : 60, 90);
    for (j = 0; j < 5; j++)
        coef(y->k[j], j < 4 ? y->f[j] : y->f[4] ? y->f[4] : 1500,
             j < 4 ? bw[j] * tract / 128 + (j ? 0 : y->v.breath / 4 + y->v.b1) : y->f[4] ? y->f[4] / 2 : 1500);
    y->inc = (uint32_t)((int64_t)fe * 6087);
    return 1;
}

/* One sample. */
static int32_t sample(yapper *y)
{
    static const uint16_t bw[4] = { 70, 100, 150, 384 };
    const yap_voice *v = &y->v;
    int32_t g = 0, n = noise(y), x, s, i, tract = v->tract;
    int open = y->phase < (uint32_t)v->oq << 24;

    y->phase += y->inc + y->jit;
    if (y->phase < y->inc + y->jit) {           /* a new period: jitter, shimmer */
        y->jit = (uint32_t)((int64_t)y->inc * noise(y) >> 23);
        y->shim = 256 + (noise(y) >> 12);
    }
    if (open && v->style & YAP_SMOOTH) {
        /* A smooth glottis: the flow rises and falls as smoothsteps, 60% of
           the open time up and 40% down, and closes gently, with no sudden
           close to ring the tract (the LF model's long return phase). Its
           derivative drives the tract. */
        int32_t t = (int32_t)((y->phase >> 9) / v->oq), u, q;   /* 0 to 32768 of the open time */
        q = t < 19661 ? (int32_t)((int64_t)t * 32768 / 19661) : (int32_t)((int64_t)(32768 - t) * 32768 / 13107);
        q = q > 32768 ? 32768 : q < 0 ? 0 : q;
        u = (int32_t)((int64_t)q * q >> 15) * (98304 - 2 * q) >> 16;   /* 3q^2 - 2q^3, Q15 */
        g = (int32_t)(((int64_t)(u - y->uprev) * ((int64_t)v->oq << 24) / (y->inc ? y->inc : 1)) >> 1);
        y->uprev = u;
    } else if (open) {
        /* The glottal flow's derivative (Rosenberg; Klatt's KLGLOTT88):
           2t - 3t^2 while open, then the sudden close that drives the
           tract; nothing while shut. */
        int32_t t = (int32_t)((y->phase >> 9) / v->oq);
        t = t > 65536 ? 65536 : t;              /* past the close only by a step's phase */
        g = (int32_t)((int64_t)t * (65536 - 3 * t) >> 15);
    } else
        y->uprev = 0;
    y->src += (int32_t)((int64_t)(g - y->src) * v->tilt >> 8);
    /* Breath rides the voice, stronger while the glottis is open; softness,
       a second pole, takes the top off both, and a little breath stays
       above it. */
    x = y->src + ((n * v->breath) >> (open ? 8 : 9));
    y->src2 += ((x - y->src2) * (256 - v->soft)) >> 8;
    x = y->src2 + ((n * v->breath >> 8) * v->soft >> 10);
    x = (x * (256 + v->soft * 15 / 16) >> 8) * (y->av * y->shim >> 8) >> 7;   /* as loud as unsoftened */
    if (!y->f[4])
        x += n * y->af >> 7;                    /* h: breath through the tract */
    if (v->style & YAP_PARALLEL) {
        /* Formants side by side (Holmes's parallel synthesizer), each a band
           at its own level, signs alternating as a cascade's would, and
           beneath them the fundamental's own band. */
        static const int16_t lev[4] = { 256, -150, 70, -35 };
        for (s = 0, i = 0; i < 4; i++)
            s += (int32_t)((int64_t)(res(y, i, x) - x) * lev[i] * (bw[i] * tract / 128)
                           / (y->f[i] > 100 ? y->f[i] : 100) >> 8);
        s = s * 3 + (res(y, 5, x) * 3 >> 2);
    } else
        for (s = x, i = 0; i < 4; i++)
            s = res(y, i, s);
    if (v->style & YAP_ASPIRATE) {
        /* Air through the glottis, high up and in time with it (Klatt and
           Klatt 1990: a woman's voice's chief cue). */
        s += ((n - y->nprev) * v->breath >> (open ? 7 : 9)) * (y->av >> 4) >> 8;
        y->nprev = n;
    }
    if (y->f[4])
        s += res(y, 4, n * y->af >> 7) >> 1;    /* frication beside the tract */
    s = ((s >> 8) * v->volume >> 8) * y->lvl >> 8;
    if (y->radio) {
        int closing = y->at >= y->nev && y->rtail > 0;   /* the words are said: the squelch as it closes */
        y->rtail -= closing;
        s = channel(y, s, n, closing);
    }
    return s * 4;
}

/* n samples into mix (at gain) or out. */
static void run(yapper *y, int32_t *mix, int16_t *out, uint32_t n, int32_t gain)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        int32_t s;
        if (y->busy && !y->step) {
            /* Every 1/60 s from the start of the text: once the words are
               said and the voice is quiet, stop. */
            if (y->steps && y->steps % TICK == 0 && y->at >= y->nev && y->av <= 4 && (!y->radio || y->rtail <= 0))
                silence(y);
            else
                control(y);                     /* 0 once a hush has faded out */
        }
        if (!y->busy) {
            if (out)
                for (; i < n; i++)
                    out[i] = 0;
            return;
        }
        s = (int32_t)((int64_t)sample(y) * gain >> 8);
        if (++y->step == BLK) {
            y->step = 0;
            y->steps++;
        }
        if (mix)
            mix[i] += s;
        else
            out[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }
}

void yap_mix(yapper *y, int32_t *mix, uint32_t n, int32_t gain)
{
    run(y, mix, 0, n, gain);
}

void yap_render(yapper *y, int16_t *out, uint32_t n)
{
    run(y, 0, out, n, 256);
}
