/* tables.c: phrase tables, damaged or not, handed to the runtime under the
   sanitizers by test.py.

   tables CASES LINES

   CASES is a run of cases, each a u32 size, a u32 verdict (0: accepted,
   1: refused, 2: either), then the table. LINES holds the text to say with
   each table accepted, a line each. Each table sits in a buffer of exactly
   its size, so a read past its end is caught. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../yapper.h"

static yapper y;

static uint32_t u32(const unsigned char *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int main(int argc, char **argv)
{
    static unsigned char cases[1 << 26];
    static char lines[1 << 16];
    static int16_t pcm[4410];
    size_t n, at = 0;
    unsigned count = 0, accepted = 0;
    FILE *f;

    if (argc != 3 || !(f = fopen(argv[1], "rb")))
        return 2;
    n = fread(cases, 1, sizeof cases, f);
    fclose(f);
    if (!(f = fopen(argv[2], "rb")))
        return 2;
    lines[fread(lines, 1, sizeof lines - 1, f)] = 0;
    fclose(f);

    while (at + 8 <= n) {
        uint32_t size = u32(cases + at), want = u32(cases + at + 4);
        unsigned char *table = malloc(size ? size : 1);
        int got;
        char *line;
        if (!table || at + 8 + size > n)
            return 2;
        memcpy(table, cases + at + 8, size);
        at += 8 + size;
        yap_reset(&y);
        got = yap_phrases(&y, table, size);
        if (want != 2 && (got != 0) != (want == 1)) {
            fprintf(stderr, "tables: case %u: %s, should be %s\n", count, got ? "refused" : "accepted",
                    want ? "refused" : "accepted");
            return 1;
        }
        if (!got) {
            accepted++;
            for (line = lines; *line;) {
                size_t len = strcspn(line, "\n");
                yap_say(&y, &yap_man, line, (uint32_t)len);
                yap_render(&y, pcm, 4410);
                line += len + (line[len] == '\n');
            }
        }
        free(table);
        count++;
    }
    printf("tables: %u cases, %u accepted\n", count, accepted);
    return 0;
}
