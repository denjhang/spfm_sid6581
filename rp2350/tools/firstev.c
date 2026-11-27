/* firstev.c - the first N events of both extraction paths, full precision.
 *
 * Two different origins are possible in this codebase:
 *   sid_lsfp_extract      -> find_play_origin() over the whole capture
 *   sid_lsfp_stream_next  -> its own incremental search, 1 s budget
 * If they disagree, the live player starts a tune at a different point than
 * the recorder, which reads as "the first note is early".
 *
 * usage: firstev <file.sid> [subsong] [count]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sid_rsid.h"
#include "sid_lsfp.h"

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: firstev <file.sid> [subsong] [count]\n"); return 1; }
    int subsong = (argc > 2) ? atoi(argv[2]) : -1;
    int want = (argc > 3) ? atoi(argv[3]) : 45;

    sid_event_t *ev = NULL;
    size_t n = 0;
    sid_song_info_t info;
    char err[200];
    if (sid_lsfp_extract(argv[1], subsong, 3.0, &ev, &n, &info, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    printf("== extract (find_play_origin over the whole capture) ==\n");
    for (size_t i = 0; i < n && (int)i < want; i++)
        printf("  [%2zu] %10.4f %02X=%02X\n", i, ev[i].t_ms, ev[i].reg, ev[i].val);
    sid_song_free(ev);

    char e2[200];
    sid_lsfp_stream_t *s = sid_lsfp_stream_open(argv[1], subsong, e2, sizeof e2);
    if (!s) { fprintf(stderr, "%s\n", e2); return 1; }
    printf("== stream (origin searched incrementally, 1 s budget) ==\n");
    const sid_event_t *sev = NULL;
    size_t sn = 0;
    int shown = 0;
    for (int frame = 0; frame < 4 && shown < want; frame++) {
        if (sid_lsfp_stream_next(s, &sev, &sn) != 0) break;
        for (size_t i = 0; i < sn && shown < want; i++, shown++)
            printf("  [%2d] %10.4f %02X=%02X\n", shown, sev[i].t_ms, sev[i].reg, sev[i].val);
    }
    sid_lsfp_stream_close(s);
    return 0;
}
