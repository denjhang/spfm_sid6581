/* qprobe.c - does the firmware really report its event-queue fill level?
 *
 * This is the one thing the host's backpressure depends on. If
 * HidD_GetInputReport returns 0 (or the wrong field), wait_for_queue_room()
 * silently does nothing and a dense tune overflows the 8192-entry queue, where
 * sid_q_push() drops events without a word.
 *
 * Method: push N wait-only events (addr 0xFF, dt = 65535 us) which the board
 * queues and then sits on for ~65 ms each, so the queue stays full. Read the
 * level a few times, then CMD_RESET so nothing is left behind.
 */
#include <stdio.h>
#include <windows.h>
#include "rpfm_link.h"

int main(void)
{
    if (rpfm_link_open() != 0) { fprintf(stderr, "board not found\n"); return 1; }
    rpfm_link_reset();
    Sleep(150);

    printf("level when idle: %u\n", rpfm_link_queue_level());

    /* 2000 wait-only events = 5 bytes... no: 15 events/frame, so 134 frames. */
    rpfm_sid_stream_begin();
    for (int i = 0; i < 2000; i++)
        rpfm_sid_push(65535, 0xFF, 0x00);
    rpfm_sid_flush();
    Sleep(20);

    for (int i = 0; i < 6; i++) {
        printf("level after 2000 queued: %u\n", rpfm_link_queue_level());
        Sleep(100);
    }

    /* Now push well past the 8192 capacity and see whether the level saturates
     * there (which proves the queue is the limit and drops are happening). */
    rpfm_sid_stream_begin();
    for (int i = 0; i < 12000; i++)
        rpfm_sid_push(65535, 0xFF, 0x00);
    rpfm_sid_flush();
    Sleep(50);
    printf("level after +12000 queued: %u  (capacity is 8191)\n",
           rpfm_link_queue_level());

    rpfm_link_reset();
    Sleep(200);
    printf("level after reset: %u\n", rpfm_link_queue_level());
    rpfm_link_close();
    return 0;
}
