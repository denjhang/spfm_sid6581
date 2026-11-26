/* hidbench.c - measure how many 64-byte frames per second this host can push.
 *
 * Sends CMD_NOP frames, which the firmware ignores, so the SID is never touched
 * and nothing is queued. The USB path (WriteFile on the interrupt OUT endpoint)
 * is identical to the one the player uses, so the number this prints is the
 * ceiling on events/s: 15 events per frame.
 */
#include <stdio.h>
#include <windows.h>
#include "rpfm_link.h"

int main(int argc, char **argv)
{
    double seconds = (argc > 1) ? atof(argv[1]) : 2.0;

    if (rpfm_link_open() != 0) {
        fprintf(stderr, "board not found\n");
        return 1;
    }

    uint8_t payload[60];
    for (int i = 0; i < 60; i++) payload[i] = 0;

    int64_t t0 = rpfm_now_us();
    long frames = 0, fails = 0;
    int64_t end = t0 + (int64_t)(seconds * 1e6);

    while (rpfm_now_us() < end) {
        if (rpfm_link_send(0xFF /* CMD_NOP */, payload, 60, NULL))
            frames++;
        else
            fails++;
    }

    int64_t dt = rpfm_now_us() - t0;
    printf("frames=%ld fails=%ld  time=%.3f s\n", frames, fails, dt / 1e6);
    printf("  %.0f frames/s  ->  %.0f events/s (15/frame)  %.0f writes/s\n",
           frames * 1e6 / dt, frames * 15e6 / dt, frames * 15e6 / dt);

    /* Now the same with a response read on every frame, which is what the
     * backpressure poll does every 20 frames. */
    frames = 0; fails = 0;
    t0 = rpfm_now_us();
    end = t0 + (int64_t)(seconds * 1e6);
    rpfm_resp_t r;
    while (rpfm_now_us() < end) {
        if (rpfm_link_send(0xFF, payload, 60, &r)) frames++; else fails++;
    }
    dt = rpfm_now_us() - t0;
    printf("with GetInputReport on every frame: %.0f frames/s -> %.0f writes/s\n",
           frames * 1e6 / dt, frames * 15e6 / dt);

    rpfm_link_close();
    return 0;
}
