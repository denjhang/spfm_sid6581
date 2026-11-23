/*
 * sidscan.cpp - classify a .sid by how its play routine is driven, and measure
 * what each recording engine costs in bytes.
 *
 * "Does this tune use interrupts and timers" is what decides whether it is an
 * RSID in practice:
 *
 *   RSID  - no play address in the header ($0000). The tune installs its own
 *           CIA/VIC interrupt handler during init and is driven entirely by
 *           emulated interrupts.
 *   PSID  - play address present, so the *player* calls it. The rate is either
 *           the VBI raster IRQ (SidTuneInfo::SPEED_VBI) or CIA timer A
 *           (SPEED_CIA_1A); the header reports which.
 *
 * The header claim is then checked against the recording: the register stream
 * is grouped into bursts (gap > 2.5 ms starts a new one) and the median
 * interval between burst starts gives the actual driver rate in Hz. A 50 Hz
 * median on a PAL tune means VBI; anything else is a timer.
 *
 *   sidscan [--seconds N] [--compare] <file.sid> ...
 *
 * --compare also records with the old hand-written 6502 (engine "native") and
 * prints both sizes, so the growth is visible per tune.
 *
 * One tab-separated line per file.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>

#include <sidplayfp/SidTune.h>
#include <sidplayfp/SidTuneInfo.h>

extern "C" {
#include "sid_rsid.h"
#include "sidvgm.h"
#include "sid_lsfp.h"
}

static const char *magic_of(const char *path)
{
    static char m[8];
    FILE *f = fopen(path, "rb");
    if (!f) return "?";
    size_t n = fread(m, 1, 4, f);
    fclose(f);
    m[n] = 0;
    if (memcmp(m, "RSID", 4) == 0) return "RSID";
    if (memcmp(m, "PSID", 4) == 0) return "PSID";
    return "?";
}

/* Median interval between consecutive burst starts, in Hz (0 = no bursts). */
static double burst_hz(const sid_event_t *ev, size_t n)
{
    std::vector<double> starts;
    const double GAP = 2.5;
    bool first = true;
    for (size_t i = 0; i < n; i++) {
        if (first || ev[i].t_ms - ev[i - 1].t_ms > GAP) {
            starts.push_back(ev[i].t_ms);
            first = false;
        }
    }
    if (starts.size() < 4) return 0.0;

    std::vector<double> d;
    for (size_t i = 1; i < starts.size(); i++) {
        double delta = starts[i] - starts[i - 1];
        if (delta > 0.5) d.push_back(delta);
    }
    if (d.size() < 3) return 0.0;
    std::sort(d.begin(), d.end());
    double med = d[d.size() / 2];
    return med > 0.0 ? 1000.0 / med : 0.0;
}

static size_t count_reg(const sid_event_t *ev, size_t n, uint8_t reg)
{
    size_t c = 0;
    for (size_t i = 0; i < n; i++) if (ev[i].reg == reg) c++;
    return c;
}

/* Size the stream would take as .sidvgm without writing it, or with writing if
 * `write_to` is non-NULL. The container is: header + 3 bytes per write + up to
 * 3 bytes per wait run + GD3. Writing to a temp file is the only way to get
 * the exact number without duplicating the writer, so do that. */
static long stream_size(const char *path, int subsong, double seconds,
                        int engine, long *writes)
{
    sid_event_t *ev = NULL;
    size_t n = 0;
    sid_song_info_t info;
    char err[200];
    int rc = (engine == 0)
        ? sid_lsfp_extract(path, subsong, seconds, &ev, &n, &info, err, sizeof err)
        : sid_song_extract(path, subsong, seconds, &ev, &n, &info, err, sizeof err);
    if (rc != 0) { *writes = -1; return -1; }
    *writes = (long)n;
    if (n == 0) { sid_song_free(ev); return 0; }

    const char *tmp = "sidscan_tmp.sidvgm";
    long size = -1;
    if (sidvgm_write(tmp, ev, n, &info, "sidscan") == 0) {
        FILE *f = fopen(tmp, "rb");
        if (f) { fseek(f, 0, SEEK_END); size = ftell(f); fclose(f); }
        remove(tmp);
    }
    sid_song_free(ev);
    return size;
}

int main(int argc, char **argv)
{
    double seconds = 20.0;
    bool compare = false;
    std::vector<const char *> files;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--compare")) compare = true;
        else files.push_back(argv[i]);
    }
    if (files.empty()) {
        fprintf(stderr, "usage: sidscan [--seconds N] [--compare] <file.sid> ...\n");
        return 1;
    }

    printf("file\tmagic\tclock\theader_play\theader_speed\tburst_hz");
    printf("\twrites\tD418\tsize_lsfp");
    if (compare) printf("\twrites_native\tsize_native\tratio");
    printf("\n");

    for (const char *p : files) {
        SidTune tune(p);
        if (!tune.getStatus()) {
            printf("%s\tERROR\t%s\n", p, tune.statusString());
            continue;
        }
        /* getInfo() before a song is selected reports the default speed table
         * entry for "no song" (VBI), so the tune must be activated first or
         * every CIA tune is labelled VBI. selectSong(0) = the file default. */
        tune.selectSong(0);
        const SidTuneInfo *ti = tune.getInfo();

        const char *clock = "?";
        switch (ti->clockSpeed()) {
        case SidTuneInfo::CLOCK_PAL:  clock = "PAL"; break;
        case SidTuneInfo::CLOCK_NTSC: clock = "NTSC"; break;
        case SidTuneInfo::CLOCK_ANY:  clock = "ANY"; break;
        default: break;
        }
        const char *spd = ti->songSpeed() == SidTuneInfo::SPEED_CIA_1A ? "CIA" : "VBI";

        /* One recording is enough for both the burst rate and the size. */
        sid_event_t *ev = NULL;
        size_t n = 0;
        sid_song_info_t info;
        char err[200];
        double hz = 0;
        long size_new = -1, size_old = -1, w_new = -1, w_old = -1;
        size_t n418 = 0;

        if (sid_lsfp_extract(p, -1, seconds, &ev, &n, &info, err, sizeof err) == 0) {
            hz = burst_hz(ev, n);
            n418 = count_reg(ev, n, 0x18);
            const char *tmp = "sidscan_tmp.sidvgm";
            if (n && sidvgm_write(tmp, ev, n, &info, "sidscan") == 0) {
                FILE *f = fopen(tmp, "rb");
                if (f) { fseek(f, 0, SEEK_END); size_new = ftell(f); fclose(f); }
                remove(tmp);
            } else if (n == 0) {
                size_new = 0;
            }
            w_new = (long)n;
            sid_song_free(ev);
        }

        if (compare) {
            size_old = stream_size(p, -1, seconds, 1, &w_old);
        }

        printf("%s\t%s\t%s\t%04x\t%s\t%.1f\t%ld\t%lu\t%ld",
               p, magic_of(p), clock, ti->playAddr(), spd, hz, w_new,
               (unsigned long)n418, size_new);
        if (compare) {
            double ratio = (size_old > 0 && size_new > 0)
                ? (double)size_new / (double)size_old : 0.0;
            printf("\t%ld\t%ld\t%.2f", w_old, size_old, ratio);
        }
        printf("\n");
        fflush(stdout);
    }
    return 0;
}
