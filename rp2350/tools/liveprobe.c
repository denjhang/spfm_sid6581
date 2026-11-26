/* liveprobe.c - run exactly the player's live .sid loop, but log the board's
 * queue depth and any wall-clock slippage.
 *
 * The .sidvgm path and the .sid path emit an identical register stream (verified
 * by diffing lsfpdump against the recording), so if a .sid sounds worse the
 * difference has to be in the transport: the queue must be starving or
 * overflowing. This prints which.
 *
 * usage: liveprobe <file.sid> [subsong] [seconds]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "sid_rsid.h"
#include "sid_lsfp.h"
#include "rpfm_link.h"

#define LEAD_US   400000
#define PRE_FILL  1500
#define Q_HIGH    4096
#define Q_LOW     2048
#define POLL_EVERY 20

static int64_t s_start_us, s_pushed_us;
static unsigned s_poll;
static uint16_t s_min_q = 0xFFFF, s_max_q = 0;
static long s_over_high, s_polls;

static int push_raw(int64_t due_us, uint8_t reg, uint8_t val)
{
    int64_t dt = due_us - s_pushed_us;
    if (dt < 0) dt = 0;
    while (dt > 65535) {
        if (!rpfm_sid_push(65535, 0xFF, 0x00)) return -1;
        dt -= 65535;
        s_pushed_us += 65535;
    }
    if (!rpfm_sid_push((uint16_t)dt, reg, val)) return -1;
    s_pushed_us = due_us;
    return 0;
}

static void poll_queue(void)
{
    if (++s_poll < POLL_EVERY) return;
    s_poll = 0;
    uint16_t used = rpfm_link_queue_level();
    s_polls++;
    if (used < s_min_q) s_min_q = used;
    if (used > s_max_q) s_max_q = used;
    if (used >= Q_HIGH) {
        s_over_high++;
        while (used > Q_LOW) { Sleep(2); used = rpfm_link_queue_level(); }
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: liveprobe <file.sid> [subsong] [seconds]\n"); return 1; }
    int subsong = (argc > 2) ? atoi(argv[2]) : -1;
    double seconds = (argc > 3) ? atof(argv[3]) : 15.0;

    sid_song_info_t info;
    memset(&info, 0, sizeof info);
    if (sid_song_probe(argv[1], &info) != 0) { fprintf(stderr, "not a sid\n"); return 1; }

    char err[160];
    sid_lsfp_stream_t *s = sid_lsfp_stream_open(argv[1], subsong, err, sizeof err);
    if (!s) { fprintf(stderr, "%s\n", err); return 1; }
    double frame_ms = sid_lsfp_stream_frame_ms(s);
    printf("%s: %s clock, frame %.3f ms\n", argv[1], info.pal ? "PAL" : "NTSC", frame_ms);

    if (rpfm_link_open() != 0) { fprintf(stderr, "board not found\n"); return 1; }
    rpfm_set_phi2(info.pal ? 0 : 1);
    rpfm_link_reset();
    Sleep(150);

    const sid_event_t *ev = NULL;
    size_t n = 0;
    s_start_us = rpfm_now_us();
    s_pushed_us = 0;
    s_poll = 0;
    rpfm_sid_stream_begin();

    /* ---- prefill, same rule as play_live_lsfp ---- */
    size_t primed = 0;
    double primed_ms = 0.0;
    while (primed_ms < 6000.0 && primed < 6000 &&
           !(primed_ms >= 800.0 && primed >= 1500)) {
        if (sid_lsfp_stream_next(s, &ev, &n) != 0) break;
        for (size_t i = 0; i < n; i++) {
            push_raw((int64_t)(ev[i].t_ms * 1000.0 + 0.5), ev[i].reg, ev[i].val);
            primed++;
        }
        rpfm_sid_flush();
        primed_ms += frame_ms;
    }
    rpfm_sid_start();
    s_start_us = rpfm_now_us() - s_pushed_us;
    printf("prefilled %lu events over %.0f ms of music\n",
           (unsigned long)primed, primed_ms);

    int64_t t_play = rpfm_now_us(), next_log = t_play;
    double played_ms = primed_ms;
    long pushed = (long)primed;

    for (;;) {
        if (sid_lsfp_stream_next(s, &ev, &n) != 0) break;

        for (size_t i = 0; i < n; i++) {
            int64_t due = (int64_t)(ev[i].t_ms * 1000.0 + 0.5);
            while (due - (rpfm_now_us() - s_start_us) > LEAD_US) {
                int64_t head = due - LEAD_US - (rpfm_now_us() - s_start_us);
                Sleep((DWORD)((head > 5000 ? 5000 : (head > 0 ? head : 1) + 999) / 1000));
            }
            push_raw(due, ev[i].reg, ev[i].val);
            poll_queue();
            pushed++;
        }
        rpfm_sid_flush();
        played_ms += frame_ms;

        if (rpfm_now_us() >= next_log) {
            next_log += 1000000;
            double wall = (rpfm_now_us() - t_play) / 1000.0;
            printf("  music=%6.0f ms wall=%6.0f ms  q=%5u  (min %u max %u)\n",
                   played_ms, wall, rpfm_link_queue_level(), s_min_q, s_max_q);
            s_min_q = 0xFFFF; s_max_q = 0;
            fflush(stdout);
        }
        if (played_ms >= seconds * 1000.0) break;
    }

    printf("pushed=%ld polls=%ld stalled-on-full=%ld\n", pushed, s_polls, s_over_high);
    rpfm_link_reset();
    Sleep(200);
    rpfm_link_close();
    sid_lsfp_stream_close(s);
    return 0;
}
