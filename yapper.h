/* yapper.h: a tiny formant speech synthesizer in freestanding C.

   Text goes in. Out come 44.1 kHz mono samples, how far through the text
   the voice has got, and whether its mouth is open. A glottal pulse with
   breath in it goes through a cascade of resonators that stand for the
   vocal tract (after Klatt 1980), driven by sounds made from the text, with
   the prosody of speech: about four syllables a second, a pitch that falls
   through a phrase and lifts on stressed syllables, a longer last syllable,
   pauses at punctuation, and the small wander of a real voice.

   Words are read from their letters by a few rules of English spelling.
   A phrase table (FORMAT.md), made ahead of time by compile.py, can say
   phrases better: each one's sounds, their lengths, stress and pitch.

   Integer math only: the same calls give the same samples on every machine.
   No C library, no allocation. All state lives in a yapper the caller
   owns, so any number of voices can talk at once. */

#ifndef YAPPER_H
#define YAPPER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define YAP_RATE   44100        /* samples per second */
#define YAP_TEXT   1024         /* bytes of text a yap_say takes, at most */
#define YAP_EVENTS 1536         /* sounds and pauses a text can make, at most */

/* style bits. */
enum {
    YAP_WOMAN    = 1,           /* women's vowel formants, not men's scaled */
    YAP_ASPIRATE = 2,           /* air through the glottis, in time with it */
    YAP_PARALLEL = 4,           /* formants side by side, not in a cascade */
    YAP_SMOOTH   = 8            /* a glottis that closes smoothly */
};

/* A voice. Every field is used as given; yap_say clamps a few to keep the
   arithmetic in range. */
typedef struct yap_voice {
    uint16_t pitch;     /* speaking pitch in Hz, 20 to 1000 */
    uint8_t  range;     /* the tune's width, 256ths of the pitch */
    uint8_t  pace;      /* each sound's length in percent: smaller is quicker */
    uint8_t  tract;     /* the formants' scale: 128 is a grown man's, about 150 a woman's */
    uint8_t  oq;        /* open quotient: how much of each period the glottis is open, /256, 1 to 255 */
    uint8_t  tilt;      /* the pulse's brightness, /256: lower is darker */
    uint8_t  breath;    /* breath in the voice, /256 */
    uint8_t  flutter;   /* how much the pitch wanders, /256 */
    uint8_t  tremor;    /* an old voice's 5.5 Hz shake, /256 */
    uint8_t  volume;    /* /256 */
    uint16_t b1;        /* the first formant's extra width in Hz, 0 to 1000 */
    uint8_t  soft;      /* a second pole on the pulse, /256: takes the buzz off */
    uint8_t  style;     /* YAP_ bits */
    uint8_t  pause;     /* phrase pauses in percent, with a breath now and then; 0: short, even pauses */
    uint8_t  radio;     /* 0, or 1 to 3: over a radio channel, each stronger */
} yap_voice;

extern const yap_voice yap_man, yap_woman, yap_child, yap_elder, yap_giant, yap_robot;

struct yap_event {
    uint8_t  row, amp;
    uint16_t len, f0, pos;
};

/* A voice's state. Its fields are private: size it, place it, and use the
   functions below. */
typedef struct yapper {
    yap_voice        v;
    const uint8_t   *table;
    uint32_t         table_size, len, nev, at, left, steps, step, seed, phase, jit, inc, trem;
    int32_t          busy, hushed, cur;
    int32_t          f[5], av, af, f0, src, src2, wob[2], shim, lvl;
    int32_t          z1[6], z2[6], uprev, nprev;
    int32_t          radio, rsq, rtail, rq[5][4];
    int64_t          k[6][3];
    struct yap_event ev[YAP_EVENTS];
    uint8_t          mark[YAP_EVENTS];
    char             page[YAP_TEXT + 1];
} yapper;

/* Silence, and every part of the state as new, phrase table included. Call
   it once before anything else. */
void     yap_reset(yapper *y);

/* Use a phrase table (FORMAT.md) from now on, or none with table 0. The
   table is not copied: keep it alive while it is in use. Returns 0, or -1
   if the table is malformed, in which case none is used. */
int      yap_phrases(yapper *y, const uint8_t *table, uint32_t size);

/* Start saying the first n bytes of text, or up to a NUL, at most YAP_TEXT.
   It replaces whatever was being said. What is in brackets, ( ) or [ ], is
   not said: "(quietly)" is how, not what. */
void     yap_say(yapper *y, const yap_voice *v, const char *text, uint32_t n);

/* The next n samples: added into mix at gain/256 (256 is full scale), or
   written to out, clipped to 16 bits. Silence once the voice is done. */
void     yap_mix(yapper *y, int32_t *mix, uint32_t n, int32_t gain);
void     yap_render(yapper *y, int16_t *out, uint32_t n);

/* How many bytes of the text the voice has begun, each word with the
   punctuation after it; the whole text once it is done. */
uint32_t yap_reached(const yapper *y);

/* 1 while a vowel is sounding: the mouth is open. */
int      yap_mouth(const yapper *y);

/* 1 while there is sound to come. */
int      yap_busy(const yapper *y);

/* Fade out over about 20 ms. */
void     yap_hush(yapper *y);

#ifdef __cplusplus
}
#endif

#endif
