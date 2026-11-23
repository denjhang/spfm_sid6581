/*
 * streamcheck - assert that the live recorder and the up-front recorder agree.
 *
 * spfmplayer plays a .sid through sid_lsfp_stream_next(), one frame at a time;
 * sidextract records it through sid_lsfp_extract() in one go. Two code paths,
 * one engine, so they have to produce the same register writes at the same
 * timestamps -- otherwise the live player would be a different tune from the
 * artifact the rest of the project measures.
 *
 *   streamcheck <tune.sid> [seconds]
 *
 * Prints both counts plus the largest gap, then compares every write that both
 * cover (register, value, timestamp within 1 ms). Exit status 0 = identical.
 *
 *   make ref && ./build/streamcheck.exe sid/turboutrun.sid 10
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "sid_lsfp.h"

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s tune.sid [seconds]\n", argv[0]); return 1; }
    double secs = argc > 2 ? atof(argv[2]) : 10.0;
    char err[160];

    sid_event_t *ev = NULL; size_t n = 0;
    sid_song_info_t info;
    if (sid_lsfp_extract(argv[1], -1, secs, &ev, &n, &info, err, sizeof err) != 0) {
        fprintf(stderr, "extract: %s\n", err); return 1;
    }

    sid_lsfp_stream_t *s = sid_lsfp_stream_open(argv[1], -1, err, sizeof err);
    if (!s) { fprintf(stderr, "stream: %s\n", err); return 1; }

    const sid_event_t *sev = NULL; size_t sn = 0;
    ssize_t total = 0;
    double last_t = 0.0, max_gap = 0.0;
    int frames = 0;
    while (sid_lsfp_stream_next(s, &sev, &sn) == 0) {
        for (size_t i = 0; i < sn; i++) {
            if (sev[i].t_ms + 1e-9 < last_t) {
                printf("NON-MONOTONIC at %zd: %.4f < %.4f\n", total, sev[i].t_ms, last_t);
                return 1;
            }
            double g = sev[i].t_ms - last_t;
            if (g > max_gap && total > 0) max_gap = g;
            last_t = sev[i].t_ms;
            total++;
        }
        frames++;
        if (last_t >= secs * 1000.0) break;
    }
    sid_lsfp_stream_close(s);

    printf("extract : %lu writes, last %.2f ms (first %.3f, second %.3f)\n",
           (unsigned long)n, n ? ev[n-1].t_ms : 0.0,
           n ? ev[0].t_ms : 0.0, n > 1 ? ev[1].t_ms : 0.0);
    printf("stream  : %zd writes over %d frames, last %.2f ms\n", total, frames, last_t);
    printf("max gap : %.2f ms\n", max_gap);

    /* Compare the events that both cover. */
    size_t limit = n;
    while (limit > 0 && ev[limit-1].t_ms > last_t) limit--;
    printf("compare first %lu writes (up to %.2f ms): ", (unsigned long)limit, last_t);
    sid_lsfp_stream_t *s2 = sid_lsfp_stream_open(argv[1], -1, err, sizeof err);
    size_t idx = 0; int mismatch = 0;
    while (idx < limit && sid_lsfp_stream_next(s2, &sev, &sn) == 0) {
        for (size_t i = 0; i < sn && idx < limit; i++, idx++) {
            double dt = fabs(sev[i].t_ms - ev[idx].t_ms);
            if (sev[i].reg != ev[idx].reg || sev[i].val != ev[idx].val || dt > 1.0) {
                if (mismatch < 5)
                    printf("\n  [%zu] extract %.3f %02X=%02X  stream %.3f %02X=%02X",
                           idx, ev[idx].t_ms, ev[idx].reg, ev[idx].val,
                           sev[i].t_ms, sev[i].reg, sev[i].val);
                mismatch++;
            }
        }
    }
    sid_lsfp_stream_close(s2);
    printf("%s (%d mismatches of %lu)\n", mismatch ? "DIFF" : "identical", mismatch,
           (unsigned long)limit);
    sid_song_free(ev);
    return mismatch ? 1 : 0;
}
