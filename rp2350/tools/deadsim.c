/* deadsim.c - replay the firmware's deadline scheduler offline, in µs.
 *
 * The board does exactly this per event (rp2350/src/main.c core1_sid_main):
 *
 *   queue of (dt, reg, val) from the host, dt = due - previous due
 *   if need_anchor and dt == 0 : write now, do not anchor
 *   if need_anchor and dt != 0 : deadline = now; anchor cleared
 *   deadline += dt;  while (now < deadline) spin;
 *   write;  now += 1.65 + (hold + 5) * 1.015     (one PIO write)
 *
 * No USB, no queue depth, no host scheduling: this is the board's own tempo.
 * `now` is board time measured from the first write; `expect` maps music time
 * onto that same origin, so `lag = now - expect` is "how late this note is".
 *
 * usage: deadsim <file.sid> [subsong] [seconds] [hold] [-v]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sid_rsid.h"
#include "sid_lsfp.h"

static double write_cost(int hold) { return 1.65 + (hold + 5) * 1.015; }

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: deadsim <file.sid> [subsong] [seconds] [hold] [-v]\n");
        return 1;
    }
    int subsong = (argc > 2) ? atoi(argv[2]) : -1;
    double seconds = (argc > 3) ? atof(argv[3]) : 5.0;
    int hold = (argc > 4) ? atoi(argv[4]) : 6;
    int verbose = 0;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "-v")) verbose = 1;
    if (hold < 1) hold = 1;

    char err[200];
    sid_lsfp_stream_t *s = sid_lsfp_stream_open(argv[1], subsong, err, sizeof err);
    if (!s) { fprintf(stderr, "%s\n", err); return 1; }

    const double frame_ms = sid_lsfp_stream_frame_ms(s);
    const double cost = write_cost(hold);
    printf("# %s  hold=%d  one write = %.2f us  frame %.2f ms\n",
           argv[1], hold, cost, frame_ms);

    int64_t prev_due = 0;          /* host: music position already handed over */
    double  now = 0.0;             /* board: us since the first write */
    double  deadline = 0.0;
    int     anchor = 1;
    double  anchor_now = 0.0, anchor_music = 0.0;
    size_t  idx = 0;
    long    writes = 0, waits_only = 0;
    double  played_ms = 0.0;
    double  worst_early = 0.0, worst_early_at = 0.0;
    double  worst_late = 0.0, worst_late_at = 0.0;
    long    over_500 = 0, over_2000 = 0;
    long    shown = 0;

    const sid_event_t *ev = NULL;
    size_t n = 0;

    while (played_ms <= seconds * 1000.0) {
        if (sid_lsfp_stream_next(s, &ev, &n) != 0) break;
        for (size_t k = 0; k < n; k++) {
            const int64_t due = (int64_t)(ev[k].t_ms * 1000.0 + 0.5);
            int64_t dt = due - prev_due;
            if (dt < 0) dt = 0;
            prev_due = due;

            /* The 16-bit delta field cannot express a gap longer than 65535 us,
             * so the host splits it into wait-only events (addr 0xFF). The
             * firmware does not distinguish them: a wait-only event with a
             * non-zero delta anchors the clock like any other. `from` tracks the
             * music position the remaining delta is measured from. */
            int64_t from = due - dt;
            while (dt > 65535) {
                if (anchor) {
                    anchor = 0;
                    deadline = now;
                    anchor_now = now;
                    anchor_music = (double)from;
                }
                deadline += 65535.0;
                if (now < deadline) now = deadline;
                dt -= 65535;
                from += 65535;
                waits_only++;
            }

            if (anchor) {
                if (dt == 0) {                 /* t=0 init: runs at once */
                    now += cost;
                    idx++;
                    continue;
                }
                anchor = 0;
                deadline = now;
                anchor_now = now;
                anchor_music = (double)from;
            }

            deadline += (double)dt;
            if (now < deadline) now = deadline;

            const double expect = anchor_now + ((double)due - anchor_music);
            const double lag = now - expect;

            if (verbose && shown < 60) {
                printf("  #%zu music=%9.3f fire=%9.3f lag=%+9.1f us %02X=%02X\n",
                       idx, (double)due / 1000.0, now / 1000.0, lag,
                       ev[k].reg, ev[k].val);
                shown++;
            }
            if (lag < worst_early) { worst_early = lag; worst_early_at = (double)due / 1000.0; }
            if (lag > worst_late)  { worst_late  = lag; worst_late_at  = (double)due / 1000.0; }
            if (lag > 500.0) over_500++;
            if (lag > 2000.0) over_2000++;

            now += cost;
            idx++; writes++;
        }
        played_ms += frame_ms;
    }

    printf("writes=%zu waits_only=%ld  board=%.1f ms  music=%.1f ms  ratio=%.5f\n",
           idx, waits_only, now / 1000.0, played_ms, now / (played_ms * 1000.0));
    printf("lag: early %+.1f us @ %.1f ms   late %+.1f us @ %.1f ms   "
           ">0.5ms %ld   >2ms %ld\n",
           worst_early, worst_early_at, worst_late, worst_late_at,
           over_500, over_2000);

    sid_lsfp_stream_close(s);
    return 0;
}
