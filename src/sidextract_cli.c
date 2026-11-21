/*
 * sidextract_cli.c - command-line front end for the 6502 SID stream extractor.
 *
 * Kept separate from sidtest so the emulator can be built and used on a
 * machine without the FTDI driver or a board attached; this is what batch
 * processing a music directory uses.
 *
 *   sidextract <file.sid> [<out.sidreg>] [--seconds N] [--subsong N] [--rsid]
 *
 * With no output path the stream is summarised instead of written, which is
 * the quick way to check a file converted at all.
 *
 * --rsid switches the emulator to the RSID model (full clock + interrupts, so
 * digi sample players fire). It is off by default because those streams carry
 * thousands of writes per second, which the SPFM link cannot carry in real
 * time; see sid_rsid.h.
 *
 * --db / --stil point at HVSC's Songlengths.md5 and STIL.txt. They are
 * optional: without them there is simply no length or per-subsong name to
 * report, which is the state every SID player is in - a .sid file carries
 * neither a length nor a loop point of its own.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sid_rsid.h"
#include "sidmeta.h"

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s <file.sid> [<out.sidreg>] [--seconds N] [--subsong N] [--rsid]\n"
            "          [--limit HZ] [--db SONGLENGTHS.md5] [--stil STIL.txt]\n"
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
    const char *db_path = NULL;
    const char *stil_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            seconds = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--subsong") && i + 1 < argc) {
            subsong = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--rsid")) {
            sid_set_rsid_mode(1);
        } else if (!strcmp(argv[i], "--limit") && i + 1 < argc) {
            sid_set_volume_limit(atof(argv[++i]));
        } else if (!strcmp(argv[i], "--db") && i + 1 < argc) {
            db_path = argv[++i];
        } else if (!strcmp(argv[i], "--stil") && i + 1 < argc) {
            stil_path = argv[++i];
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

    if (info.n_sids > 1) {
        printf("  SIDs    : %u", info.n_sids);
        for (int i = 0; i < info.n_sids; i++)
            printf("  #%d at $%04X", i, info.sid_addr[i]);
        printf("\n");
    }

    /* Side databases: length from Songlengths.md5 / SSL, names from STIL.
     * Both are optional and both fail quietly - a tune that is not in HVSC
     * simply has no length to report. */
    {
        unsigned song = (unsigned)(subsong >= 0 ? subsong + 1
                                  : (info.start_song ? info.start_song : 1));
        long len = sidmeta_song_length_ms(in_path, song, db_path);
        if (len >= 0)
            printf("  length  : %ld:%02ld.%03ld  (subsong %u)\n",
                   len / 60000, (len / 1000) % 60, len % 1000, song);

        char title[128], artist[128];
        if (sidmeta_stil(stil_path, in_path, song,
                         title, sizeof title, artist, sizeof artist) == 0)
            printf("  STIL    : %s%s%s\n", title,
                   artist[0] ? " / " : "", artist);
    }

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
    printf("  play rate: %.2f Hz (%s, %lu cycles)\n", info.play_hz,
           info.cia_timed ? "CIA timer A" : "video frame",
           (unsigned long)info.play_cycles);
    if (info.n_sids > 1)
        printf("  multi-SID: %u chips (%lu writes to the extra ones)\n",
               info.n_sids, sidr_dbg_other_sid);
    if (!sid_get_rsid_mode())
        printf("  model   : PSID (play per frame, no interrupts) - use --rsid "
               "for digi/sample playback\n");
    else
        printf("  model   : RSID (full clock + interrupts)\n");

    double lim = sid_get_volume_limit();
    if (lim > 0.0) {
        unsigned long made = 0, kept = 0;
        sid_get_limit_stats(&made, &kept);
        double secs = ev && n ? ev[n - 1].t_ms / 1000.0 : seconds;
        printf("  $D418   : limited to %.0f Hz - %lu of %lu samples kept",
               lim, kept, made);
        if (secs > 0.0)
            printf(" (%.0f/s, was %.0f/s)", (double)kept / secs,
                   (double)made / secs);
        printf("\n");
    }
    if (getenv("SIDR_DEBUG")) sidr_debug_report();

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