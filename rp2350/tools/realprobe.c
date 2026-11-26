/* realprobe.c - report the spacing libsidplayfp actually produces, in µs.
 *
 * This is the ground truth before any container touches it. The .sidvgm
 * container has a 44100 Hz tick, so a recorded stream can only express
 * multiples of 22.676 us; the engine itself reports PHI1 cycles, and a real
 * tune can write every 4 cycles (4.06 us). If a player only ever sees .sidvgm
 * it will believe 22.676 us is the floor, which is wrong.
 *
 * Prints one tab-separated line:
 *   min p1 p10 p50  dt0  nonzero  <18.9us  <6.1us  <4.1us  file
 *
 * usage: realprobe <file.sid> [seconds]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sid_rsid.h"
#include "sid_lsfp.h"

typedef struct { double d; } dt_t;

static int cmpd(const void *a, const void *b)
{
    double x = ((const dt_t *)a)->d, y = ((const dt_t *)b)->d;
    return x < y ? -1 : x > y ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: realprobe <file.sid> [seconds]\n");
        return 1;
    }
    double seconds = (argc > 2) ? atof(argv[2]) : 30.0;

    sid_event_t *ev = NULL;
    size_t n = 0;
    sid_song_info_t info;
    char err[200];
    if (sid_lsfp_extract(argv[1], -1, seconds, &ev, &n, &info,
                         err, sizeof err) != 0) {
        fprintf(stderr, "ERR\t%s\t%s\n", argv[1], err);
        return 1;
    }
    if (n < 2) { printf("EMPTY\t%s\n", argv[1]); return 0; }

    dt_t *ds = malloc(n * sizeof *ds);
    size_t k = 0;
    long zero = 0;
    for (size_t i = 1; i < n; i++) {
        double d = (ev[i].t_ms - ev[i-1].t_ms) * 1000.0;   /* us, full precision */
        if (d < 0.0) d = 0.0;
        if (d == 0.0) { zero++; continue; }
        ds[k++].d = d;
    }
    if (!k) { printf("ALLZERO\t%s\tn=%zu\n", argv[1], n); return 0; }

    qsort(ds, k, sizeof *ds, cmpd);
    long u189 = 0, u61 = 0, u41 = 0;
    for (size_t i = 0; i < k; i++) {
        if (ds[i].d < 18.9) u189++;
        if (ds[i].d < 6.1)  u61++;
        if (ds[i].d < 4.1)  u41++;
    }
    printf("%.3f\t%.3f\t%.3f\t%.3f\t%ld\t%zu\t%ld\t%ld\t%ld\t%s\n",
           ds[0].d, ds[k/100].d, ds[k/10].d, ds[k/2].d,
           zero, k, u189, u61, u41, argv[1]);

    sid_song_free(ev);
    free(ds);
    return 0;
}
