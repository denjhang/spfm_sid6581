/* drainprobe.c - measure the board's ACTUAL consumption rate.
 *
 * queue_level = pushed - consumed, so if I push a chunk of a real tune and then
 * stop pushing, the rate at which the level falls is the rate at which the board
 * is writing registers. Compare it with the file's own event rate:
 *
 *   - drain == file rate  -> the deadline logic works, tempo is right
 *   - drain >> file rate  -> the board is dumping the queue, which is exactly
 *                            what "the first second rushes" sounds like
 *
 * usage: drainprobe <file.sidvgm> [from_ms] [count]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "sidvgm.h"
#include "rpfm_link.h"

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: drainprobe <file.sidvgm> [from_ms] [count]\n"); return 1; }

    double from_ms = (argc > 2) ? atof(argv[2]) : 0.0;
    int count = (argc > 3) ? atoi(argv[3]) : 1500;

    sid_event_t *ev = NULL;
    size_t n = 0;
    sid_song_info_t info;
    memset(&info, 0, sizeof info);
    if (sidvgm_read(argv[1], &ev, &n, &info) != 0) { fprintf(stderr, "cannot read\n"); return 1; }

    /* First index at or after from_ms. */
    size_t start = 0;
    while (start < n && ev[start].t_ms < from_ms) start++;
    if (start + count > n) count = (int)(n - start);

    /* The file's own spacing over that window: events per ms. */
    double span = ev[start + count - 1].t_ms - ev[start].t_ms;
    printf("file: %lu events, taking #%lu.. for %d writes over %.1f ms (%.2f writes/ms)\n",
           (unsigned long)n, (unsigned long)start, count, span, span > 0 ? count / span : 0);

    if (rpfm_link_open() != 0) { fprintf(stderr, "board not found\n"); return 1; }
    rpfm_set_phi2(info.pal ? 0 : 1);
    rpfm_link_reset();
    Sleep(200);
    rpfm_sid_start();          /* release the gate: nothing is queued yet */
    Sleep(20);

    /* Push the window with its true deltas. */
    rpfm_sid_stream_begin();
    int64_t prev = (int64_t)(ev[start].t_ms * 1000.0);
    rpfm_sid_push(20000, ev[start].reg, ev[start].val);  /* anchor with a real gap */
    prev = (int64_t)(ev[start].t_ms * 1000.0);
    for (int i = 1; i < count; i++) {
        int64_t due = (int64_t)(ev[start + i].t_ms * 1000.0);
        int64_t dt = due - prev;
        if (dt < 0) dt = 0;
        while (dt > 65535) { rpfm_sid_push(65535, 0xFF, 0); dt -= 65535; }
        rpfm_sid_push((uint16_t)dt, ev[start + i].reg, ev[start + i].val);
        prev = due;
    }
    rpfm_sid_flush();

    /* Now only watch. A full queue at the real tempo should take `span` ms. */
    int64_t t0 = rpfm_now_us();
    uint16_t q0 = rpfm_link_queue_level();
    printf("start: queue=%u\n", q0);
    for (int k = 0; k < 40; k++) {
        Sleep(50);
        uint16_t used = rpfm_link_queue_level();
        double ms = (rpfm_now_us() - t0) / 1000.0;
        printf("  +%6.0f ms  queue=%5u  drained=%5d  rate=%.2f writes/ms\n",
               ms, used, q0 - used, ms > 0 ? (q0 - used) / ms : 0.0);
        if (used == 0) break;
    }
    double total_ms = (rpfm_now_us() - t0) / 1000.0;
    printf("drained %u writes in %.0f ms -> %.2f writes/ms (file says %.2f)\n",
           q0, total_ms, total_ms > 0 ? q0 / total_ms : 0.0, span > 0 ? count / span : 0.0);

    rpfm_link_reset();
    Sleep(200);
    rpfm_link_close();
    sid_song_free(ev);
    return 0;
}
