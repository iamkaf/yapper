/* cli.c: the yapper command. Says a text into a WAV file, and prints when
   each part of the text is reached and when the mouth opens. Its compile
   command, which makes phrase tables, is compile.c. */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yapper.h"

int compile_command(int argc, char **argv); /* compile.c */

static const struct { const char *name; const yap_voice *v; } presets[] = {
    { "man", &yap_man },     { "woman", &yap_woman }, { "child", &yap_child },
    { "elder", &yap_elder }, { "giant", &yap_giant }, { "robot", &yap_robot },
};
#define NPRESETS (sizeof presets / sizeof presets[0])

#define FIELD(f) { #f, offsetof(yap_voice, f), sizeof(((yap_voice *)0)->f), 0 }
static const struct { const char *name; size_t at, size; int unused; } fields[] = {
    FIELD(pitch), FIELD(range), FIELD(pace), FIELD(tract), FIELD(oq), FIELD(tilt),
    FIELD(breath), FIELD(flutter), FIELD(tremor), FIELD(volume), FIELD(b1),
    FIELD(soft), FIELD(style), FIELD(pause), FIELD(radio),
};
#define NFIELDS (sizeof fields / sizeof fields[0])

static unsigned get(const yap_voice *v, size_t f)
{
    const unsigned char *p = (const unsigned char *)v + fields[f].at;
    return fields[f].size == 1 ? p[0] : *(const uint16_t *)(const void *)p;
}

static void set(yap_voice *v, size_t f, unsigned x)
{
    unsigned char *p = (unsigned char *)v + fields[f].at;
    if (fields[f].size == 1)
        p[0] = (unsigned char)x;
    else
        *(uint16_t *)(void *)p = (uint16_t)x;
}

static int usage(void)
{
    fprintf(stderr,
            "usage: yapper say TEXT [-o OUT.wav] [--voice NAME] [--set KEY=VALUE]...\n"
            "                       [--phrases TABLE.yph] [--cues]\n"
            "       yapper voices\n"
            "       yapper compile LINES.txt -o TABLE.yph\n"
            "TEXT - reads the text from standard input.\n");
    return 2;
}

static void le(FILE *f, uint32_t x, int n)
{
    for (int i = 0; i < n; i++)
        fputc((int)(x >> 8 * i & 255), f);
}

static unsigned char *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *d = 0;
    size_t cap = 0, n = 0, got;
    if (!f)
        return 0;
    do {
        if (n == cap && !(d = realloc(d, cap = cap ? cap * 2 : 65536))) {
            fclose(f);
            return 0;
        }
        n += got = fread(d + n, 1, cap - n, f);
    } while (got);
    fclose(f);
    *size = n;
    return d;
}

static int voices(void)
{
    printf("%-6s", "");
    for (size_t f = 0; f < NFIELDS; f++)
        printf(" %7s", fields[f].name);
    printf("\n");
    for (size_t p = 0; p < NPRESETS; p++) {
        printf("%-6s", presets[p].name);
        for (size_t f = 0; f < NFIELDS; f++)
            printf(" %7u", get(presets[p].v, f));
        printf("\n");
    }
    return 0;
}

int main(int argc, char **argv)
{
    static char text[YAP_TEXT + 1];
    static yapper y;
    yap_voice v = yap_man;
    const char *out = 0, *table = 0, *words = 0;
    unsigned char *data = 0;
    size_t size = 0;
    int cues = 0;
    FILE *wav = 0;

    if (argc >= 2 && !strcmp(argv[1], "voices"))
        return voices();
    if (argc >= 2 && !strcmp(argv[1], "compile"))
        return compile_command(argc - 2, argv + 2);
    if (argc < 3 || strcmp(argv[1], "say"))
        return usage();
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-o") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(a, "--phrases") && i + 1 < argc)
            table = argv[++i];
        else if (!strcmp(a, "--cues"))
            cues = 1;
        else if (!strcmp(a, "--voice") && i + 1 < argc) {
            size_t p;
            for (p = 0; p < NPRESETS && strcmp(presets[p].name, argv[i + 1]); p++)
                ;
            if (p == NPRESETS) {
                fprintf(stderr, "yapper: no voice %s (yapper voices lists them)\n", argv[i + 1]);
                return 2;
            }
            v = *presets[p].v;
            i++;
        } else if (!strcmp(a, "--set") && i + 1 < argc) {
            const char *kv = argv[++i], *eq = strchr(kv, '=');
            char *end;
            size_t f;
            unsigned long x;
            for (f = 0; eq && f < NFIELDS; f++)
                if (strlen(fields[f].name) == (size_t)(eq - kv) && !strncmp(fields[f].name, kv, (size_t)(eq - kv)))
                    break;
            if (!eq || f == NFIELDS) {
                fprintf(stderr, "yapper: --set takes KEY=VALUE, KEY one of the columns of yapper voices\n");
                return 2;
            }
            x = strtoul(eq + 1, &end, 10);
            if (!eq[1] || *end || x > (fields[f].size == 1 ? 255ul : 65535ul)) {
                fprintf(stderr, "yapper: %s is out of range\n", kv);
                return 2;
            }
            set(&v, f, (unsigned)x);
        } else if (!words && (a[0] != '-' || !strcmp(a, "-")))
            words = a;
        else
            return usage();
    }
    if (!words || (!out && !cues))
        return usage();
    if (!strcmp(words, "-")) {
        size_t n = fread(text, 1, YAP_TEXT, stdin);
        text[n] = 0;
    } else
        snprintf(text, sizeof text, "%s", words);

    yap_reset(&y);
    if (table) {
        if (!(data = slurp(table, &size))) {
            fprintf(stderr, "yapper: cannot read %s\n", table);
            return 1;
        }
        if (size > 0xffffffffu || yap_phrases(&y, data, (uint32_t)size)) {
            fprintf(stderr, "yapper: %s is not a phrase table\n", table);
            return 1;
        }
    }
    if (out && !(wav = fopen(out, "wb"))) {
        fprintf(stderr, "yapper: cannot write %s\n", out);
        return 1;
    }
    if (wav)
        fseek(wav, 44, SEEK_SET);

    yap_say(&y, &v, text, (uint32_t)strlen(text));
    uint32_t total = 0, reached = (uint32_t)-1;
    int mouth = -1;
    while (yap_busy(&y) && total < YAP_RATE * 600) {
        int16_t s;
        if (cues && (yap_reached(&y) != reached || yap_mouth(&y) != mouth)) {
            reached = yap_reached(&y);
            mouth = yap_mouth(&y);
            printf("%u %u %d\n", total, reached, mouth);
        }
        yap_render(&y, &s, 1);
        if (wav) {
            fputc(s & 255, wav);
            fputc((uint16_t)s >> 8, wav);
        }
        total++;
    }
    if (cues)
        printf("%u %u %d\n", total, yap_reached(&y), yap_mouth(&y));
    if (wav) {
        uint32_t d = total * 2;
        fseek(wav, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, wav);
        le(wav, 36 + d, 4);
        fwrite("WAVEfmt ", 1, 8, wav);
        le(wav, 16, 4);
        le(wav, 1, 2);
        le(wav, 1, 2);
        le(wav, YAP_RATE, 4);
        le(wav, YAP_RATE * 2, 4);
        le(wav, 2, 2);
        le(wav, 16, 2);
        fwrite("data", 1, 4, wav);
        le(wav, d, 4);
        if (fclose(wav)) {
            fprintf(stderr, "yapper: cannot write %s\n", out);
            return 1;
        }
    }
    free(data);
    return 0;
}
