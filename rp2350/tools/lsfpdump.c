/* lsfpdump.c - dump the live libsidplayfp stream to text, no hardware.
 *
 * Same API the player's live path uses (sid_lsfp_stream_open/next), so a diff
 * against a recorded .sidvgm says whether the live path produces the same
 * event stream or not.
 *
 * usage: lsfpdump <file.sid> [subsong] [seconds]
 */
#include <stdio.h>
#include <stdlib.h>

#include "sid_rsid.h"
#include "sid_lsfp.h"

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: lsfpdump <file.sid> [subsong] [seconds]\n"); return 1; }
    int subsong = (argc > 2) ? atoi(argv[2]) : -1;
    double seconds = (argc > 3) ? atof(argv[3]) : 20.0;

    char err[160];
    sid_lsfp_stream_t *s = sid_lsfp_stream_open(argv[1], subsong, err, sizeof err);
    if (!s) { fprintf(stderr, "%s\n", err); return 1; }

    const sid_event_t *ev = NULL;
    size_t n = 0;
    double played = 0.0, frame_ms = sid_lsfp_stream_frame_ms(s);
    long total = 0;

    while (played <= seconds * 1000.0) {
        if (sid_lsfp_stream_next(s, &ev, &n) != 0) break;
        for (size_t i = 0; i < n; i++) {
            printf("%.3f %02X %02X\n", ev[i].t_ms, ev[i].reg, ev[i].val);
            total++;
        }
        played += frame_ms;
    }
    fprintf(stderr, "frames=%.0f writes=%ld frame_ms=%.3f\n",
            played / frame_ms, total, frame_ms);
    sid_lsfp_stream_close(s);
    return 0;
}
