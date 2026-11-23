/*
 * sidextract_cli.c - command-line front end for the 6502 SID stream extractor.
 *
 * Kept separate from sidtest so the emulator can be built and used on a
 * machine without the FTDI driver or a board attached; this is what batch
 * processing a music directory uses.
 *
 *   sidextract <file.sid> [<out.sidreg|out.sidvgm>] [--seconds N] [--subsong N]
 *   sidextract <file.sid> --info
 *   sidextract <file.sidvgm> --dump [--cmds N]
 *   sidextract <file.sidvgm> <back.sidreg>       // convert a rendered stream
 *
 * The output container follows the output file's extension: `.sidvgm` writes
 * the VGM-shaped binary, anything else writes the old text stream. With no
 * output path the stream is summarised instead of written.
 *
 * This is the offline record path: it runs the 6502 and writes a file. It does
 * not touch the hardware and does not affect the player.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "sid_rsid.h"
#include "sidvgm.h"
#include "sid_websid.h"
#include "sid_lsfp.h"

/* Recording back ends, in order of preference. lsfp is libsidplayfp running
 * its own 6502/VIC/CIA; websid is the vendored WebSid; native is ours. */
enum { ENGINE_LSFP = 0, ENGINE_WEBSID = 1, ENGINE_NATIVE = 2 };

static const char *engine_name(int e)
{
    return e == ENGINE_LSFP ? "lsfp"
         : e == ENGINE_WEBSID ? "websid" : "native";
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s <file.sid> [<out.sidreg|out.sidvgm>] "
            "[--seconds N] [--subsong N] [--comment TEXT] "
            "[--engine lsfp|websid|native]\n"
            "       %s <file.sid> --info\n"
            "       %s <file.sidvgm> --dump [--cmds N]\n"
            "       %s <file.sidvgm> <out.sidreg>\n",
            argv0, argv0, argv0, argv0);
}

static int has_suffix(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    if (a < b) return 0;
    s += a - b;
    for (size_t i = 0; i < b; i++)
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)suf[i])) return 0;
    return 1;
}

int main(int argc, char **argv)
{
    const char *in_path = NULL;
    const char *out_path = NULL;
    const char *comment = NULL;
    double seconds = 60.0;
    int subsong = -1;
    int info_only = 0;
    int dump_only = 0;
    /* libsidplayfp by default: it runs the reference player's own 6502, VIC
     * and CIA, so the register stream and its intra-frame timing are the
     * benchmark. --engine websid / native pick the other recorders. */
    int engine = ENGINE_LSFP;
    unsigned max_cmds = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            seconds = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--engine") && i + 1 < argc) {
            const char *e = argv[++i];
            if (!strcmp(e, "lsfp") || !strcmp(e, "libsidplayfp")) engine = ENGINE_LSFP;
            else if (!strcmp(e, "websid")) engine = ENGINE_WEBSID;
            else if (!strcmp(e, "native")) engine = ENGINE_NATIVE;
            else {
                fprintf(stderr, "--engine must be lsfp, websid or native\n");
                return 1;
            }
        } else if (!strcmp(argv[i], "--subsong") && i + 1 < argc) {
            subsong = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--comment") && i + 1 < argc) {
            comment = argv[++i];
        } else if (!strcmp(argv[i], "--cmds") && i + 1 < argc) {
            max_cmds = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--dump")) {
            dump_only = 1;
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

    /* ---- sidvgm input: dump, or convert back to text ---- */
    if (sidvgm_is_file(in_path)) {
        if (dump_only) {
            if (sidvgm_dump(stdout, in_path, max_cmds) != 0) return 1;
            return 0;
        }
        sid_event_t *ev = NULL;
        size_t n = 0;
        sid_song_info_t info;
        if (sidvgm_read(in_path, &ev, &n, &info) != 0) {
            fprintf(stderr, "cannot read %s as sidvgm\n", in_path);
            return 1;
        }
        printf("%s\n", in_path);
        printf("  title   : %s\n", info.name[0] ? info.name : "(none)");
        printf("  author  : %s\n", info.author[0] ? info.author : "(none)");
        printf("  released: %s\n", info.released[0] ? info.released : "(none)");
        printf("  clock   : %s, %.0f Hz\n", info.pal ? "PAL" : "NTSC",
               info.clock_hz);
        printf("  length  : %.2f s\n", sidvgm_length_ms(ev, n, &info) / 1000.0);
        printf("  events  : %lu\n", (unsigned long)n);

        if (out_path) {
            if (!has_suffix(out_path, ".sidreg")) {
                fprintf(stderr, "only .sidreg output is supported here\n");
                sid_song_free(ev);
                return 1;
            }
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

    /* ---- .sid input ---- */
    sid_song_info_t info;
    if (sid_song_probe(in_path, &info) != 0) {
        fprintf(stderr, "cannot read header of %s\n", in_path);
        return 1;
    }

    printf("%s\n", in_path);
    printf("  title   : %s\n", info.name[0] ? info.name : "(none)");
    printf("  author  : %s\n", info.author[0] ? info.author : "(none)");
    printf("  released: %s\n", info.released[0] ? info.released : "(none)");
    printf("  format  : %s, %s, %.3f Hz\n", info.is_rsid ? "RSID" : "PSID",
           info.pal ? "PAL" : "NTSC", info.clock_hz);
    printf("  subsongs: %u (default %u)\n", info.n_songs, info.start_song);
    printf("  load=0x%04X init=0x%04X play=0x%04X speed=0x%08lX\n",
           info.load_addr, info.init_addr, info.play_addr,
           (unsigned long)info.speed);

    if (info_only) return 0;

    printf("  running %s for %.1f s...\n", engine_name(engine), seconds);
    sid_event_t *ev = NULL;
    size_t n = 0;
    char err[128];
    int rc;
    switch (engine) {
    case ENGINE_LSFP:
        rc = sid_lsfp_extract(in_path, subsong, seconds, &ev, &n, &info,
                              err, sizeof err);
        break;
    case ENGINE_WEBSID:
        rc = sid_websid_extract(in_path, subsong, seconds, &ev, &n, &info,
                                err, sizeof err);
        break;
    default:
        rc = sid_song_extract(in_path, subsong, seconds, &ev, &n, &info,
                              err, sizeof err);
        break;
    }
    if (rc != 0) {
        fprintf(stderr, "extract failed (%s): %s\n", engine_name(engine), err);
        return 1;
    }
    double len_ms = n ? ev[n - 1].t_ms : 0.0;
    printf("  %lu register writes over %.2f s\n", (unsigned long)n,
           len_ms / 1000.0);

    if (out_path) {
        if (has_suffix(out_path, ".sidvgm")) {
            char auto_comment[128];
            if (!comment) {
                snprintf(auto_comment, sizeof auto_comment,
                         "subsong %d, rendered by sidextract",
                         subsong < 0 ? (int)info.start_song - 1 : subsong);
                comment = auto_comment;
            }
            if (sidvgm_write(out_path, ev, n, &info, comment) != 0) {
                fprintf(stderr, "cannot write %s\n", out_path);
                sid_song_free(ev);
                return 1;
            }
            printf("  -> %s (sidvgm)\n", out_path);
        } else {
            if (sid_sidreg_write(out_path, ev, n) != 0) {
                fprintf(stderr, "cannot write %s\n", out_path);
                sid_song_free(ev);
                return 1;
            }
            printf("  -> %s (sidreg)\n", out_path);
        }
    }

    sid_song_free(ev);
    return 0;
}
