/* bench.c: how much faster than real time each voice speaks.

   cc -O2 tests/bench.c yapper.c -o bench && ./bench */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../yapper.h"

#define ROUNDS 50

static yapper y;
static int16_t buf[4096];

int main(void)
{
    static const struct { const char *name; const yap_voice *v; } voices[] = {
        { "man", &yap_man }, { "woman", &yap_woman }, { "child", &yap_child },
        { "elder", &yap_elder }, { "giant", &yap_giant }, { "robot", &yap_robot },
    };
    const char *text = "Why would anyone build a whole speech synthesizer that fits in a few kilobytes? "
                       "Because it's fun, and because small things travel well. Shall we listen to it again?";
    for (size_t i = 0; i < sizeof voices / sizeof voices[0]; i++) {
        unsigned long n = 0;
        clock_t start = clock();
        double took;
        for (int round = 0; round < ROUNDS; round++) {
            yap_reset(&y);
            yap_say(&y, voices[i].v, text, (uint32_t)strlen(text));
            for (n = 0; yap_busy(&y); n += 4096)
                yap_render(&y, buf, 4096);
        }
        took = (double)(clock() - start) / CLOCKS_PER_SEC / ROUNDS;
        printf("%-6s %5.2f s of speech in %6.2f ms: %5.0f times real time\n", voices[i].name, n / 44100.0,
               took * 1000, n / 44100.0 / took);
    }
    return 0;
}
