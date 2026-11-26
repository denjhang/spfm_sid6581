/* pairprobe.c - reproduce "the second tune in a playlist opens too fast".
 *
 * Sequence, exactly as the playlist does it between two files:
 *   1. queue A's opening, CMD_SID_START, let the board play it out
 *   2. queue B's opening (no CMD_RESET - the playlist only clears registers),
 *      send the clear, then CMD_SID_START
 *   3. sample the queue every 100 ms
 *
 * If the board honours B's deltas the queue falls at B's own music rate
 * (~1.2 events/ms for Golden Axe). If B's deltas were added to a deadline that
 * is already in the past, the queue collapses at bus speed (~50 events/ms).
 *
 * usage: pairprobe <A.sidvgm> <B.sidvgm> [events]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "sidvgm.h"
#include "rpfm_link.h"

typedef struct { sid_event_t *ev; size_t n; sid_song_info_t info; } song_t;

static int load(const char *p, song_t *s)
{
    memset(s, 0, sizeof *s);
    return sidvgm_read(p, &s->ev, &s->n, &s->info);
}

static int64_t s_pushed;

static void push_raw(int64_t due, uint8_t reg, uint8_t val)
{
    int64_t dt = due - s_pushed;
    if (dt < 0) dt = 0;
    while (dt > 65535) { rpfm_sid_push(65535, 0xFF, 0); dt -= 65535; s_pushed += 65535; }
    rpfm_sid_push((uint16_t)dt, reg, val);
    s_pushed = due;
}

static void queue_open(const song_t *s, int count)
{
    if (count > (int)s->n) count = (int)s->n;
    s_pushed = 0;
    rpfm_sid_stream_begin();
    for (int i = 0; i < count; i++)
        push_raw((int64_t)(s->ev[i].t_ms * 1000.0 + 0.5), s->ev[i].reg, s->ev[i].val);
    rpfm_sid_flush();
}

static void drain_report(const char *tag, const song_t *s, int count)
{
    double span = s->ev[count - 1].t_ms;
    int64_t t0 = rpfm_now_us();
    uint16_t q0 = rpfm_link_queue_level();
    printf("%s: %d events, music span %.0f ms, queue %u\n", tag, count, span, q0);
    for (int k = 0; k < 20; k++) {
        Sleep(100);
        uint16_t used = rpfm_link_queue_level();
        double ms = (rpfm_now_us() - t0) / 1000.0;
        printf("   +%5.0f ms  queue=%5u  drained=%5d  %.2f events/ms\n",
               ms, used, q0 - used, (q0 - used) / ms);
        if (used == 0) break;
    }
    double ms = (rpfm_now_us() - t0) / 1000.0;
    printf("   total %u in %.0f ms = %.2f events/ms (music says %.2f)\n\n",
           q0, ms, q0 / ms, count / span);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: pairprobe <A.sidvgm> <B.sidvgm> [events]\n"); return 1; }
    int count = (argc > 3) ? atoi(argv[3]) : 1200;

    song_t a, b;
    if (load(argv[1], &a) != 0 || load(argv[2], &b) != 0) { fprintf(stderr, "cannot read\n"); return 1; }
    if (count > (int)a.n) count = (int)a.n;
    if (count > (int)b.n) count = (int)b.n;

    if (rpfm_link_open() != 0) { fprintf(stderr, "board not found\n"); return 1; }
    rpfm_set_phi2(a.info.pal ? 0 : 1);
    rpfm_link_reset();
    Sleep(200);

    /* ---- A from a reset board ---- */
    queue_open(&a, count);
    rpfm_sid_start();
    drain_report("A (fresh board)", &a, count);
    Sleep(400);

    /* ---- B right after A, the playlist way: STOP, clear, prefill, start ---- */
    rpfm_sid_stop();
    uint8_t p[50]; uint8_t pn = 0;
    for (uint8_t r = 0; r <= 0x18; r++) { p[pn++] = r; p[pn++] = 0x00; }
    rpfm_link_send(CMD_SID_WRITE, p, pn, NULL);

    queue_open(&b, count);
    rpfm_sid_start();
    drain_report("B (after A, STOP boundary)", &b, count);

    rpfm_link_reset();
    Sleep(200);
    rpfm_link_close();
    sid_song_free(a.ev);
    sid_song_free(b.ev);
    return 0;
}
