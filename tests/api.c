/* api.c: the C API's promises, checked under the sanitizers by test.py.

   api TABLE.yph */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../yapper.h"

#define MAX (44100 * 30)
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "api.c:%d: %s\n", __LINE__, #c); exit(1); } } while (0)

static yapper a, b;
static int16_t one[MAX], two[MAX];
static int32_t mix[MAX];

static const char *line = "Wait... are you sure? I thought we had more time, but maybe not.";

/* Everything a voice says, rendered n samples at a time. */
static uint32_t whole(yapper *y, const yap_voice *v, const char *text, int16_t *out, uint32_t n)
{
    uint32_t total = 0;
    yap_say(y, v, text, (uint32_t)strlen(text));
    while (yap_busy(y)) {
        CHECK(total + n <= MAX);
        yap_render(y, out + total, n);
        total += n;
    }
    while (total && !out[total - 1])
        total--;
    return total;
}

int main(int argc, char **argv)
{
    static uint8_t table[1 << 16];
    uint32_t n, m, i, size = 0;
    FILE *f;

    CHECK(argc == 2 && (f = fopen(argv[1], "rb")));
    size = (uint32_t)fread(table, 1, sizeof table, f);
    fclose(f);

    /* The same text gives the same samples, rendered in any chunks. */
    yap_reset(&a);
    n = whole(&a, &yap_woman, line, one, 4096);
    CHECK(n > 44100);
    for (m = 1; m < 2000; m = m * 3 + 1) {
        yap_reset(&a);
        memset(two, 0, sizeof two);
        CHECK(whole(&a, &yap_woman, line, two, m) == n && !memcmp(one, two, 2 * n));
    }

    /* Two voices at once, interleaved, each sound as it does alone. */
    yap_reset(&a);
    yap_reset(&b);
    yap_say(&a, &yap_woman, line, (uint32_t)strlen(line));
    yap_say(&b, &yap_giant, "Two voices can talk at once.", 99);
    memset(two, 0, sizeof two);
    for (i = 0; i < n; i += 100) {
        int16_t junk[100];
        yap_render(&a, two + i, n - i < 100 ? n - i : 100);
        yap_render(&b, junk, 100);
    }
    CHECK(!memcmp(one, two, 2 * n));

    /* Mixing at full gain into silence is rendering, before clipping; at
       gain 0 it adds nothing; and it adds. */
    yap_reset(&a);
    yap_say(&a, &yap_woman, line, (uint32_t)strlen(line));
    for (i = 0; i < n; i++)
        mix[i] = 7;
    yap_mix(&a, mix, n, 256);
    for (i = 0; i < n; i++)
        CHECK(mix[i] - 7 == one[i] || (one[i] == 32767 && mix[i] - 7 > 32767) || (one[i] == -32768 && mix[i] - 7 < -32768));
    yap_reset(&a);
    yap_say(&a, &yap_woman, line, (uint32_t)strlen(line));
    for (i = 0; i < n; i++)
        mix[i] = 5;
    yap_mix(&a, mix, n, 0);
    for (i = 0; i < n; i++)
        CHECK(mix[i] == 5);

    /* Reached climbs to the whole text; the mouth opens and shuts. */
    yap_reset(&a);
    CHECK(!yap_busy(&a) && yap_reached(&a) == 0 && !yap_mouth(&a));
    yap_say(&a, &yap_man, line, (uint32_t)strlen(line));
    {
        uint32_t last = 0, opened = 0, shut = 0;
        while (yap_busy(&a)) {
            int16_t s;
            uint32_t r = yap_reached(&a);
            CHECK(r >= last && r <= strlen(line));
            last = r;
            opened += yap_mouth(&a);
            shut += !yap_mouth(&a);
            yap_render(&a, &s, 1);
        }
        CHECK(yap_reached(&a) == strlen(line) && !yap_mouth(&a) && opened > 44100 / 10 && shut > 44100 / 10);
    }

    /* A hush fades within 30 ms, and the text counts as reached. */
    yap_reset(&a);
    yap_say(&a, &yap_man, line, (uint32_t)strlen(line));
    yap_render(&a, two, 30000);
    CHECK(yap_busy(&a) && yap_reached(&a) < strlen(line));
    yap_hush(&a);
    CHECK(yap_reached(&a) == strlen(line) && !yap_mouth(&a));
    yap_render(&a, two, 44100 * 30 / 1000);
    CHECK(!yap_busy(&a));
    yap_render(&a, two, 100);
    for (i = 0; i < 100; i++)
        CHECK(two[i] == 0);

    /* n stops the text short; NUL stops it too; nothing to say is silence. */
    yap_reset(&a);
    yap_say(&a, &yap_man, "Hello there, friend.", 5);
    m = whole(&a, &yap_man, "Hello", one, 512);
    yap_reset(&a);
    CHECK(whole(&a, &yap_man, "Hello there, friend.", two, 512) > m);
    yap_reset(&a);
    yap_say(&a, &yap_man, "Hello there, friend.", 5);
    {
        uint32_t k = 0;
        while (yap_busy(&a)) {
            CHECK(k + 512 <= MAX);
            yap_render(&a, two + k, 512);
            k += 512;
        }
        while (k && !two[k - 1])
            k--;
        CHECK(k == m && !memcmp(one, two, 2 * m) && yap_reached(&a) == 5);
    }
    yap_reset(&a);
    yap_say(&a, &yap_man, 0, 10);
    CHECK(!yap_busy(&a));
    yap_say(&a, 0, "Hello", 5);
    CHECK(!yap_busy(&a));
    yap_say(&a, &yap_man, "", 0);
    yap_render(&a, two, 44100);
    CHECK(yap_reached(&a) == 0);

    /* Text past YAP_TEXT is cut; every byte value is safe to say. */
    {
        static char big[YAP_TEXT * 2 + 1];
        for (i = 0; i < YAP_TEXT * 2; i++)
            big[i] = (char)(1 + (i * 7919u) % 255);
        yap_reset(&a);
        yap_say(&a, &yap_child, big, YAP_TEXT * 2);
        for (i = 0; i < 20 && yap_busy(&a); i++)
            yap_render(&a, two, 44100);
        CHECK(yap_reached(&a) <= YAP_TEXT);
        for (i = 0; i < YAP_TEXT * 2; i++)
            big[i] = "ab, "[i % 4];
        yap_say(&a, &yap_man, big, YAP_TEXT * 2);
        yap_render(&a, two, 44100);
    }

    /* Extreme voices stay in range, for the sanitizers. */
    {
        yap_voice v;
        uint32_t trial;
        uint32_t seed = 7;
        for (trial = 0; trial < 200; trial++) {
            uint8_t *p = (uint8_t *)&v;
            for (i = 0; i < sizeof v; i++) {
                seed = seed * 1103515245u + 12345u;
                p[i] = (uint8_t)(trial < 2 ? (trial ? 255 : 0) : seed >> 16);
            }
            yap_reset(&a);
            yap_phrases(&a, trial & 1 ? table : 0, size);
            yap_say(&a, &v, line, (uint32_t)strlen(line));
            yap_render(&a, two, 44100);
            yap_hush(&a);
            yap_render(&a, two, 4410);
        }
    }

    /* A table, then none again: the letters' samples. */
    yap_reset(&a);
    CHECK(yap_phrases(&a, table, size) == 0);
    CHECK(yap_phrases(&a, 0, 0) == 0);
    yap_reset(&b);
    CHECK(whole(&a, &yap_man, line, one, 735) == whole(&b, &yap_man, line, two, 735));
    printf("api: ok\n");
    return 0;
}
