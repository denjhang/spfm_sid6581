/*
 * sidextract_cli.c - command-line front end for the 6502 SID stream extractor.
 *
 * Kept separate from sidtest so the emulator can be built and used on a
 * machine without the FTDI driver or a board attached; this is what batch
 * processing a music directory uses.
 *
 *   sidextract <file.sid> [<out.sidreg>] [--seconds N] [--subsong N]
 *
 * With no output path the stream is summarised instead of written, which is
 * the quick way to check a file converted at all.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sid_rsid.h"

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s <file.sid> [<out.sidreg>] [--seconds N] [--subsong N]\n"
            "       %s <file.sid> --info\n",
            argv0, argv0);
}

int main(int argc, char **argv)
{
    const char *in_path = NULL;
    const char *out_path = NULL;
    double seconds = 60.0;
    int subsong = -1;
    int info_only = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            seconds = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--subsong") && i + 1 < argc) {
            subsong = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--info") || !strcmp(argv[i], "-i")) {
            info_only = 1;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            usage(argv[0]);
            return 1;
        } else if (!in_path) {
            in_path = argv[i];
        } else if (!out_path) {
            out_path = argv[i];
        } else {
            fprintf(stderr, "unexpected argument '%s'\n", argv[i]);
            return 1;
        }
    }

    if (!in_path) { usage(argv[0]); return 1; }

    sid_song_info_t info;
    if (sid_song_probe(in_path, &info) != 0) {
        fprintf(stderr, "cannot read header of %s\n", in_path);
        return 1;
    }

    printf("%s\n", in_path);
    printf("  title   : %s\n", info.name[0] ? info.name : "(none)");
    printf("  author  : %s\n", info.author[0] ? info.author : "(none)");
    printf("  format  : %s, %s, %.3f Hz\n", info.is_rsid ? "RSID" : "PSID",
           info.pal ? "PAL" : "NTSC", info.clock_hz);
    printf("  subsongs: %u (default %u)\n", info.n_songs, info.start_song);
    printf("  load=0x%04X init=0x%04X play=0x%04X speed=0x%08lX\n",
           info.load_addr, info.init_addr, info.play_addr,
           (unsigned long)info.speed);

    if (info_only) return 0;

    printf("  running 6502 for %.1f s...\n", seconds);
    sid_event_t *ev = NULL;
    size_t n = 0;
    char err[128];
    if (sid_song_extract(in_path, subsong, seconds, &ev, &n, &info,
                         err, sizeof err) != 0) {
        fprintf(stderr, "extract failed: %s\n", err);
        return 1;
    }
    printf("  %lu register writes over %.2f s\n", (unsigned long)n,
           n ? ev[n - 1].t_ms / 1000.0 : 0.0);

    if (out_path) {
        if (sid_sidreg_write(out_path, ev, n) != 0) {
            fprintf(stderr, "cannot write %s\n", out_path);
            sid_song_free(ev);
            return 1;
        }
        printf("  -> %s\n", out_path);
    }

    sid_song_free(ev);
    return 0;
}
