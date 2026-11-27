/* mutebench.c - does the STOP + clear + START mute sequence actually take
 * effect, or can the board be left gated with the clear still queued?
 *
 * The host's hw_mute() sends three frames back to back:
 *   CMD_SID_STOP   (Core 0 records the cut mark, Core 1 truncates the queue)
 *   CMD_SID_WRITE  (25 register/value pairs, all dt=0)
 *   CMD_SID_START  (Core 1: deadline=0, need_anchor=1, running=1)
 *
 * The cut and the gate are Core 1's job, so the two commands can be processed in
 * either order relative to each other. If Core 1 services the stop *after* the
 * start, it sets running=0 and truncates to the mark - which discards the clear
 * as well. The chip then keeps the last tune's volume/gates ("notes ring on
 * after quitting") and the board sits gated with 25 events still queued.
 *
 * The queued level is the tell: after a successful mute it must be 0. A level
 * of ~25 means the clear never ran.
 *
 * usage: mutebench [iterations]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "rpfm_link.h"

int main(int argc, char **argv)
{
    int iters = (argc > 1) ? atoi(argv[1]) : 20;

    if (rpfm_link_open() != 0) { fprintf(stderr, "board not found\n"); return 1; }
    rpfm_link_reset();
    Sleep(200);

    printf("level when idle: %u\n", rpfm_link_queue_level());

    int stranded = 0, drained = 0;
    int hist[64];
    memset(hist, 0, sizeof hist);

    for (int i = 0; i < iters; i++) {
        /* ---- exactly what hw_mute() does ---- */
        rpfm_sid_stop();

        uint8_t p[50];
        uint8_t n = 0;
        for (uint8_t r = 0; r <= 0x18; r++) { p[n++] = r; p[n++] = 0x00; }
        rpfm_link_send(CMD_SID_WRITE, p, n, NULL);

        rpfm_sid_start();

        /* Give Core 1 plenty of time: 25 writes are ~0.5 ms of bus. */
        Sleep(150);

        uint16_t lv = rpfm_link_queue_level();
        if (lv > 63) lv = 63;
        hist[lv]++;
        if (lv == 0) drained++;
        else stranded++;

        printf("  run %2d: level=%u %s\n", i + 1, lv,
               lv ? "<-- clear never ran" : "");
    }

    printf("\n%d runs: %d drained, %d stranded\n", iters, drained, stranded);
    if (stranded)
        printf("VERDICT: the mute is a race - STOP can be serviced after START\n"
               "         and discards the clear (and the gate it just opened).\n");
    else
        printf("VERDICT: every run drained; the clear always ran.\n");

    rpfm_link_reset();
    Sleep(200);
    rpfm_link_close();
    return stranded ? 2 : 0;
}
