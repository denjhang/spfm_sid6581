/* wbench.c - measure the board's real SID-write throughput.
 *
 * Every event carries dt_us = 0, so after the anchor the firmware writes them
 * as fast as the bus allows: queue drain time / count is the per-write cost.
 * This is the number that decides whether a dense $D418 digi can be replayed
 * at all, and it is the one thing the host's pacing has to stay under.
 *
 * usage: wbench [count] [hold_phi2_periods]
 */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "rpfm_link.h"

int main(int argc, char **argv)
{
    int count = (argc > 1) ? atoi(argv[1]) : 4000;
    int hold  = (argc > 2) ? atoi(argv[2]) : 0;

    if (rpfm_link_open() != 0) { fprintf(stderr, "board not found\n"); return 1; }
    if (hold > 0) rpfm_set_sid_step(hold);
    rpfm_link_reset();
    Sleep(200);

    /* Anchor first: one event with a non-zero delta, so the following zero-dt
     * writes are not swallowed as "init". */
    rpfm_sid_stream_begin();
    rpfm_sid_push(1000, 0x18, 0x0F);
    rpfm_sid_flush();
    rpfm_sid_start();
    Sleep(50);

    int64_t t0 = rpfm_now_us();
    rpfm_sid_stream_begin();
    for (int i = 0; i < count; i++)
        rpfm_sid_push(0, 0x18, (uint8_t)(i & 0x0F));
    rpfm_sid_flush();

    uint16_t peak = 0;
    for (;;) {
        Sleep(5);
        uint16_t used = rpfm_link_queue_level();
        if (used > peak) peak = used;
        if (used == 0) break;
        if (rpfm_now_us() - t0 > 60LL * 1000000) { printf("timeout at %u\n", used); break; }
    }
    int64_t dt = rpfm_now_us() - t0;

    printf("count=%d hold=%d  drain=%.3f s  peak_queue=%u\n", count, hold, dt / 1e6, peak);
    printf("  -> %.1f us per register write, %.0f writes/s\n",
           (double)dt / count, count * 1e6 / dt);

    rpfm_link_reset();
    Sleep(200);
    rpfm_link_close();
    return 0;
}
