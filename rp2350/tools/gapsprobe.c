/* gapsprobe.c - verify the starved-queue re-anchor.
 *
 * A tune that is silent for a while and then fires a dense $D418 burst is the
 * case that ruins a digi: if the board keeps its old absolute deadline, the
 * whole burst is already "late" and gets written at bus speed instead of at the
 * spacing the recorder intended.
 *
 * Sequence: one event to anchor, a 500 ms wait, then 300 events 137 us apart.
 *   correct   -> the 300 take ~41 ms to drain
 *   re-anchor missing -> they take ~6 ms (300 x 19 us)
 *
 * usage: gapsprobe
 */
#include <stdio.h>
#include <windows.h>
#include "rpfm_link.h"

int main(void)
{
    if (rpfm_link_open() != 0) { fprintf(stderr, "board not found\n"); return 1; }
    rpfm_link_reset();
    Sleep(200);

    rpfm_sid_stream_begin();
    rpfm_sid_push(1000, 0x18, 0x0F);        /* anchor */
    for (int i = 0; i < 8; i++)             /* 500 ms of silence */
        rpfm_sid_push(65535, 0xFF, 0x00);
    rpfm_sid_push(65535 - (8 * 65535 - 500000) + 1, 0xFF, 0x00); /* pad to 500 ms */
    for (int i = 0; i < 300; i++)           /* the burst: 137 us apart */
        rpfm_sid_push(137, 0x18, (uint8_t)(i & 0x0F));
    rpfm_sid_flush();
    rpfm_sid_start();

    Sleep(560);                             /* let it reach the burst */

    int64_t t0 = rpfm_now_us();
    uint16_t q0 = rpfm_link_queue_level();
    for (;;) {
        Sleep(2);
        if (rpfm_link_queue_level() == 0) break;
        if (rpfm_now_us() - t0 > 5LL * 1000000) break;
    }
    double ms = (rpfm_now_us() - t0) / 1000.0;

    printf("burst: %u events left when sampling started\n", q0);
    printf("drained in %.1f ms  -> %.0f us per write\n", ms, ms * 1000.0 / 300.0);
    printf("expect ~41 ms (137 us spacing). ~6 ms means the deadline was stale.\n");

    rpfm_link_reset();
    Sleep(200);
    rpfm_link_close();
    return 0;
}
