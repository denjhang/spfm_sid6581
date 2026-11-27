/* cmpfirst.c - live libsidplayfp stream vs a recorded .sidvgm, event by event.
 *
 * The two paths must produce the same timestamps for the same tune. If they do
 * not, the difference is the answer to "the .sidvgm sounds right but the .sid
 * opening is early" - it is not a transport problem at all.
 *
 * usage: cmpfirst <file.sid> <file.sidvgm> [count]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "sid_rsid.h"
#include "sid_lsfp.h"
#include "sidvgm.h"

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: cmpfirst <file.sid> <file.sidvgm> [count]\n"); return 1; }
    int want = (argc > 3) ? atoi(argv[3]) : 80;

    /* ---- recorded ---- */
    sid_event_t *rv = NULL;
    size_t rn = 0;
    sid_song_info_t ri;
    memset(&ri, 0, sizeof ri);
    if (sidvgm_read(argv[2], &rv, &rn, &ri) != 0) { fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }

    /* ---- live ---- */
    char err[200];
    sid_lsfp_stream_t *s = sid_lsfp_stream_open(argv[1], -1, err, sizeof err);
    if (!s) { fprintf(stderr, "%s\n", err); return 1; }

    printf("%4s %24s %24s %s\n", "#", "live            reg=val", "recorded        reg=val", "delta");
    const sid_event_t *ev = NULL;
    size_t n = 0;
    size_t li = 0, first_diff = (size_t)-1;
    double  max_dt = 0.0;
    int done = 0;

    while (!done) {
        if (sid_lsfp_stream_next(s, &ev, &n) != 0) break;
        for (size_t k = 0; k < n && li < (size_t)want; k++, li++) {
            double lt = ev[k].t_ms, rt = (li < rn) ? rv[li].t_ms : -1.0;
            uint8_t rr = (li < rn) ? rv[li].reg : 0xFF, rvv = (li < rn) ? rv[li].val : 0xFF;
            double d = lt - rt;
            if (fabs(d) > 0.0015 || ev[k].reg != rr || ev[k].val != rvv) {
                if (first_diff == (size_t)-1) first_diff = li;
                if (fabs(d) > max_dt) max_dt = fabs(d);
            }
            printf("%4zu %10.4f ms %02X=%02X   %10.4f ms %02X=%02X   %+9.4f%s\n",
                   li, lt, ev[k].reg, ev[k].val, rt, rr, rvv, d,
                   (fabs(d) > 0.0015 || ev[k].reg != rr || ev[k].val != rvv) ? "  *" : "");
        }
        if (li >= (size_t)want) done = 1;
    }
    sid_lsfp_stream_close(s);

    printf("\nfirst mismatch at #%zd, max |dt| over mismatches %.4f ms\n",
           (first_diff == (size_t)-1) ? -1 : (ssize_t)first_diff, max_dt);
    sid_song_free(rv);
    return 0;
}
