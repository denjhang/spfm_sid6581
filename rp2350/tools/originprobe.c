/* originprobe.c - what did find_play_origin() actually pick?
 *
 * The extract and stream paths both contain a "first gap longer than half a
 * frame" search. If it picks the wrong event, the whole tune shifts and the
 * opening reads as early. This prints the first 40 raw PHI1 cycle stamps and
 * their deltas, which is the only way to tell "the tune's init really looks
 * like this" from "the search gave up and fell back to cyc[0]".
 *
 * usage: originprobe <file.sid> [subsong] [seconds]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sid_rsid.h"
#include "sid_lsfp.h"

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: originprobe <file.sid> [subsong] [seconds]\n"); return 1; }
    int subsong = (argc > 2) ? atoi(argv[2]) : -1;
    double seconds = (argc > 3) ? atof(argv[3]) : 3.0;

    sid_event_t *ev = NULL;
    size_t n = 0;
    sid_song_info_t info;
    char err[200];
    if (sid_lsfp_extract(argv[1], subsong, seconds, &ev, &n, &info, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    const double frame_ms = 1000.0 / info.vsync_hz;
    printf("# %s  %s  frame %.3f ms  clock %.0f Hz  events %zu\n",
           argv[1], info.pal ? "PAL" : "NTSC", frame_ms, info.clock_hz, n);
    printf("# extract t_ms values are already origin-relative:\n");
    for (size_t i = 0; i < n && i < 40; i++)
        printf("  [%2zu] t=%10.4f ms  %02X=%02X\n", i, ev[i].t_ms, ev[i].reg, ev[i].val);

    /* Reconstruct: the search's rule is the first gap > 0.5 frame. Report where
     * that lands in the returned (origin-relative) timeline. */
    double prev = ev[0].t_ms;
    for (size_t i = 1; i < n && i < 400; i++) {
        if (ev[i].t_ms - prev > frame_ms * 0.5) {
            printf("# first gap > half a frame: %.4f -> %.4f ms (%.4f ms)\n",
                   prev, ev[i].t_ms, ev[i].t_ms - prev);
            break;
        }
        prev = ev[i].t_ms;
    }
    sid_song_free(ev);
    return 0;
}
