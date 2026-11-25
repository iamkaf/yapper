#include <stdio.h>
#include <string.h>
#include <time.h>
#include "yapper.h"
static yapper y;
int main(int argc, char **argv)
{
    const char *t = "Why would anyone build a whole speech synthesizer that fits in a few kilobytes? Because it's fun, and because small things travel well. Shall we listen to it again?";
    static int16_t buf[4096];
    double best = 1e9; unsigned long n = 0;
    for (int r = 0; r < 5; r++) {
        struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
        yap_reset(&y); yap_say(&y, &yap_man, t, strlen(t)); n = 0;
        while (yap_busy(&y)) { yap_render(&y, buf, 4096); n += 4096; }
        clock_gettime(CLOCK_MONOTONIC, &b);
        double s = b.tv_sec - a.tv_sec + (b.tv_nsec - a.tv_nsec) / 1e9; if (s < best) best = s;
    }
    printf("%.2f s of speech in %.4f s: %.0fx real time\n", n / 44100.0, best, n / 44100.0 / best);
}
