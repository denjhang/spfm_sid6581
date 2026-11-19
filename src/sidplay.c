/*
 * sidplay.c - replay a SID register stream on the SPFM hardware.
 *
 * Two halves, matching the two things a .sid actually is:
 *
 *   1. Emulation (sid_rsid.c): run the 6502 init/play routines and record
 *      every $D400-$D41F write with a timestamp.
 *   2. Scheduling (this file): push those writes out over USB so each one
 *      reaches the SID at the same wall-clock offset it had in the original.
 *
 * The scheduling half is the interesting one. Naively sleeping "for N ms" per
 * event accumulates error, and a drift of 19.96 ms is one whole C64 frame, so
 * after a few minutes the music is visibly behind the melody it should have
 * been playing. Instead the player records one start timestamp and waits
 * against absolute deadlines (spfm_now_us + spfm_wait_until_us), so the error
 * stays bounded for the whole song.
 *
 * The pacing model follows Denjhang_Music_Player's VGM thread: work out how
 * far the clock says we should have progressed, emit everything due by now,
 * then yield. Being late is not fatal - the events still go out in order, just
 * bunched - so a slow USB transfer degrades timing rather than dropping notes.
 */
#include "sid_rsid.h"
#include "sidplay.h"
#include "sid6581.h"
#include "spfm_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#ifdef _WIN32
#include <conio.h>
#include <direct.h>
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#endif

#ifndef _WIN32
#define _mkdir(p) mkdir((p), 0755)
#endif

/* ------------------------------------------------------------------ */
/* shared state                                                        */
/* ------------------------------------------------------------------ */

static sidplay_opts_t s_opt;
static int            s_stop_requested = 0;   /* q */
static int            s_next_action = 0;      /* 'n', 'b', ',', '.' */

/* Registers defs worth suppressing while the melody is playing: leaving the
 * filter wide open is usually closer to what the composer heard on a 6581 than
 * whatever cutoff a 6581-specific init wrote. Off by default - see --no-touch. */
static int s_keep_filter = 0;

/* ------------------------------------------------------------------ */
/* hardware                                                            */
/* ------------------------------------------------------------------ */

static int hw_connect(void)
{
    if (s_opt.dry_run) {
        spfm_dry_run_enable(true);
        return 0;
    }

    uint32_t ids[16];
    char descs[16][64];
    int n = spfm_list_devices(ids, 16, descs);
    if (n <= 0) {
        fprintf(stderr, "no FTDI device found (is the SPFM plugged in?)\n");
        return -1;
    }
    if (s_opt.device >= n) {
        fprintf(stderr, "--device %d out of range, %d present\n", s_opt.device, n);
        return -1;
    }
    if (spfm_open(s_opt.device) != 0) return -1;
    printf("device %d: %s\n", s_opt.device, descs[s_opt.device]);
    return 0;
}

static void hw_disconnect(void)
{
    if (!s_opt.dry_run) spfm_close();
}

/* Stop all three voices without waiting for the release phase, so a song
 * change does not carry the previous tune's tail into the next one. */
static void hw_mute(void)
{
    for (uint8_t v = 0; v < 3; v++)
        sid_write(s_opt.slot, (uint8_t)(v * 7 + SID_REG_CONTROL), 0x00);
    sid_write(s_opt.slot, SID_REG_MODE_VOL, 0x00);
    sid_flush();
}

/* ------------------------------------------------------------------ */
/* keyboard (playlist control)                                         */
/* ------------------------------------------------------------------ */

static int kb_hit(void)
{
#ifdef _WIN32
    return _kbhit() ? 1 : 0;
#else
    /* Raw mode is only worth the trouble on a terminal; when stdin is a pipe
     * (scripted run) there is nothing to read. */
    return 0;
#endif
}

static int kb_get(void)
{
#ifdef _WIN32
    return _getch();
#else
    return getchar();
#endif
}

/* Polled between events rather than in a thread: the player already wakes up
 * on every event boundary, so a 10 ms reaction time costs nothing and there is
 * no second thread racing the FTDI handle. */
static void kb_poll(void)
{
    while (kb_hit()) {
        int c = kb_get();
        switch (c) {
        case 'q': case 'Q': case 27:      s_stop_requested = 1; return;
        case 'n': case 'N':               s_next_action = 'n';   return;
        case 'b': case 'B':               s_next_action = 'b';   return;
        case ',':                         s_next_action = ',';   return;
        case '.':                         s_next_action = '.';   return;
        default: break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* event stream loading                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    sid_event_t *events;     /* owned unless `borrowed` */
    size_t       count;
    int          borrowed;   /* sid_sidreg_read hands back a static buffer */
    sid_song_info_t info;
} stream_t;

static int has_suffix(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    if (a < b) return 0;
    s += a - b;
    for (size_t i = 0; i < b; i++)
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)suf[i])) return 0;
    return 1;
}

/* Whether the file needs the 6502 emulator, or is already a rendered stream. */
static int is_sidreg(const char *path)
{
    return has_suffix(path, ".sidreg");
}

static const char *g_cache_dir_override = NULL;

void sidplay_set_cache_dir(const char *dir)
{
    g_cache_dir_override = dir;
}

static int cache_dir_for(const char *sid_path, char *buf, size_t len)
{
    (void)sid_path;
    if (g_cache_dir_override && *g_cache_dir_override) {
        snprintf(buf, len, "%s", g_cache_dir_override);
        return 0;
    }
    /* Default: sid_cache in the working directory, matching the STC32G tools
     * layout so the two projects can share rendered streams. */
    snprintf(buf, len, "sid_cache");
    return 0;
}

static void stream_free(stream_t *st)
{
    if (!st->borrowed && st->events) sid_song_free(st->events);
    st->events = NULL;
    st->count = 0;
}

static int load_stream(const char *path, int subsong, double seconds,
                       int use_cache, stream_t *st, char *err, size_t errlen)
{
    memset(st, 0, sizeof(*st));

    /* A pre-rendered stream needs no emulation at all - useful when iterating
     * on playback timing without paying the 6502 run every time. */
    if (is_sidreg(path)) {
        if (sid_sidreg_read(path, &st->events, &st->count) != 0) {
            snprintf(err, errlen, "cannot read %s", path);
            return -1;
        }
        st->borrowed = 1;
        /* info.name is shown as the song title, so give it the file's stem
         * rather than the full path (which would also get cut at 32 chars). */
        const char *base = path;
        for (const char *p = path; *p; p++)
            if (*p == '/' || *p == '\\') base = p + 1;
        snprintf(st->info.name, sizeof(st->info.name), "%s", base);
        st->info.pal = 1;
        st->info.vsync_hz = 50.125;
        st->info.clock_hz = 985248.0;
        st->info.n_songs = 1;
        return 0;
    }

    if (use_cache) {
        char cache_dir[512], cache_path[768];
        cache_dir_for(path, cache_dir, sizeof cache_dir);
        if (sid_sidreg_cache_name(cache_path, sizeof cache_path, cache_dir,
                                  path, subsong, seconds) == 0 &&
            sid_sidreg_exists(cache_path)) {
            if (sid_sidreg_read(cache_path, &st->events, &st->count) == 0) {
                st->borrowed = 1;
                sid_song_probe(path, &st->info);
                printf("  cache: %s (%lu events)\n", cache_path,
                       (unsigned long)st->count);
                return 0;
            }
        }
    }

    if (sid_song_extract(path, subsong, seconds, &st->events, &st->count,
                         &st->info, err, errlen) != 0) {
        return -1;
    }

    if (use_cache && st->count) {
        char cache_dir[512], cache_path[768];
        cache_dir_for(path, cache_dir, sizeof cache_dir);
        if (sid_sidreg_cache_name(cache_path, sizeof cache_path, cache_dir,
                                  path, subsong, seconds) == 0) {
            /* Failing to write the cache is never worth aborting playback. */
            _mkdir(cache_dir);
            sid_sidreg_write(cache_path, st->events, st->count);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* the replay loop                                                     */
/* ------------------------------------------------------------------ */

/* Emit one event. Every register is written straight through: the stream is
 * already in the order the original code produced it, and any reordering would
 * break the gate/reset sequences that produce percussion and digi samples. */
static void emit(const sid_event_t *e)
{
    if (s_keep_filter) {
        /* Optional: drop the song's filter programming so the raw voices stay
         * bright. Register 0x18 is a special case - it carries the master
         * volume in its low nibble, so instead of skipping it we keep the
         * volume and clear the filter mode. Skipping it outright would mute
         * the chip. */
        if (e->reg == SID_REG_RES_FILT ||
            e->reg == SID_REG_FC_LO ||
            e->reg == SID_REG_FC_HI)
            return;
        if (e->reg == SID_REG_MODE_VOL) {
            sid_write(s_opt.slot, e->reg, (uint8_t)(e->val & SID_VOLUME_MASK));
            return;
        }
    }
    sid_write(s_opt.slot, e->reg, e->val);
}

/* Replay `ev` at wall-clock pace. Returns 0 when the stream finished, -1 when
 * interrupted by the user. */
static int replay(const sid_event_t *ev, size_t n, double speed, int pace)
{
    if (n == 0) return 0;

    /* Dry run: walk the stream as fast as possible unless --pace was given.
     * Pacing would only prove that Sleep() works, and waiting 60 s to check
     * that the bytes came out right is not a useful test. */
    if (s_opt.dry_run && !pace) {
        for (size_t i = 0; i < n; i++) emit(&ev[i]);
        sid_flush();
        printf("  dry-run: %lu events -> %lu bytes\n",
               (unsigned long)n, (unsigned long)spfm_capture_size());
        return 0;
    }

    const double us_per_ms = 1000.0 / speed;
    int64_t start = spfm_now_us();
    int64_t worst_lag_us = 0;
    size_t last_report = (size_t)-1;

    /* One USB transfer per frame instead of one per register write. A C64 play
     * routine writes its whole frame's worth of registers in a burst, so the
     * events arrive in tight groups separated by ~20 ms of silence; the flush
     * below lands in that silence and the bytes still reach the SID before the
     * next frame is due. Doing it per write instead would put 50 latency-timer
     * expiries inside one frame and the music would fall apart. */
    sid_set_deferred_flush(true);

    for (size_t i = 0; i < n; i++) {
        int64_t due = start + (int64_t)(ev[i].t_ms * us_per_ms);

        /* Wait for events that are still in the future; anything already due
         * is emitted immediately so a stall turns into a bunch rather than a
         * permanent offset. */
        if (due > spfm_now_us()) {
            sid_flush();      /* ship everything due so far, then block */
            /* Poll the keyboard while waiting, in small steps, so [q] does not
             * have to wait for the next note. */
            while (due > spfm_now_us()) {
                kb_poll();
                if (s_stop_requested) { sid_set_deferred_flush(false); return -1; }
                int64_t left = due - spfm_now_us();
                spfm_wait_until_us(spfm_now_us() + (left > 5000 ? 5000 : left));
            }
        } else {
            int64_t lag = spfm_now_us() - due;
            if (lag > worst_lag_us) worst_lag_us = lag;
        }

        emit(&ev[i]);

        /* Progress is reported per second of music, not per event: at 1200
         * events/s a per-event line would be unreadable. */
        size_t sec = (size_t)(ev[i].t_ms / 1000.0);
        if (sec != last_report) {
            last_report = sec;
            printf("\r  %5.1f s  event %lu/%lu  lag %ld ms   ",
                   ev[i].t_ms / 1000.0,
                   (unsigned long)(i + 1), (unsigned long)n,
                   (long)(worst_lag_us / 1000));
            fflush(stdout);
        }
    }

    sid_flush();
    sid_set_deferred_flush(false);
    printf("\r  %5.1f s  %lu events  worst lag %ld ms        \n",
           ev[n - 1].t_ms / 1000.0, (unsigned long)n,
           (long)(worst_lag_us / 1000));
    return 0;
}

/* ------------------------------------------------------------------ */
/* commands                                                            */
/* ------------------------------------------------------------------ */

static void print_info(const sid_song_info_t *in, const char *path)
{
    printf("file      : %s\n", path);
    printf("title     : %s\n", in->name[0] ? in->name : "(none)");
    printf("author    : %s\n", in->author[0] ? in->author : "(none)");
    printf("format    : %s\n", in->is_rsid ? "RSID" : "PSID");
    printf("clock     : %s (%.3f Hz cpu, %.3f Hz vsync)\n",
           in->pal ? "PAL" : "NTSC", in->clock_hz, in->vsync_hz);
    printf("subsongs  : %u (default %u)\n", in->n_songs, in->start_song);
    printf("load=0x%04X init=0x%04X play=0x%04X\n",
           in->load_addr, in->init_addr, in->play_addr);
    printf("speed     : 0x%08lX%s\n", (unsigned long)in->speed,
           in->speed == 0 ? " (vblank)" : " (CIA timer)");
}

static int cmd_info(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "usage: sidinfo <file.sid>\n");
        return 1;
    }
    sid_song_info_t in;
    if (sid_song_probe(argv[0], &in) != 0) {
        fprintf(stderr, "cannot read header of %s\n", argv[0]);
        return 1;
    }
    print_info(&in, argv[0]);
    return 0;
}

static int cmd_extract(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "usage: sidextract <file.sid> <out.sidreg> [--seconds N] "
                "[--subsong N]\n");
        return 1;
    }
    const char *in_path = argv[0];
    const char *out_path = argv[1];
    double seconds = 60.0;
    int subsong = -1;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--subsong") && i + 1 < argc) subsong = atoi(argv[++i]);
        else { fprintf(stderr, "unknown option '%s'\n", argv[i]); return 1; }
    }

    sid_event_t *ev = NULL;
    size_t n = 0;
    sid_song_info_t in;
    char err[128];

    printf("running 6502 for %.1f s of music...\n", seconds);
    if (sid_song_extract(in_path, subsong, seconds, &ev, &n, &in, err, sizeof err) != 0) {
        fprintf(stderr, "extract failed: %s\n", err);
        return 1;
    }
    printf("%s: %lu register writes\n", in.name, (unsigned long)n);

    if (sid_sidreg_write(out_path, ev, n) != 0) {
        fprintf(stderr, "cannot write %s\n", out_path);
        sid_song_free(ev);
        return 1;
    }
    printf("-> %s\n", out_path);
    sid_song_free(ev);
    return 0;
}

static int cmd_play(int argc, char **argv)
{
    const char *path = NULL;
    double seconds = 60.0;
    double speed = 1.0;
    int subsong = -1;
    int use_cache = 1;
    int loops = 1;
    int pace = 0;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc)      seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--speed") && i + 1 < argc)   speed = atof(argv[++i]);
        else if (!strcmp(argv[i], "--subsong") && i + 1 < argc) subsong = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--loops") && i + 1 < argc)   loops = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-cache"))                use_cache = 0;
        else if (!strcmp(argv[i], "--cache-dir") && i + 1 < argc)
            sidplay_set_cache_dir(argv[++i]);
        else if (!strcmp(argv[i], "--raw-filter"))              s_keep_filter = 1;
        else if (!strcmp(argv[i], "--pace"))                    pace = 1;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            return 1;
        } else path = argv[i];
    }

    if (!path) {
        fprintf(stderr, "usage: sidplay <file.sid|file.sidreg> [options]\n");
        return 1;
    }
    if (speed <= 0) speed = 1.0;
    if (loops < 1) loops = 1;

    stream_t st;
    char err[128];
    if (load_stream(path, subsong, seconds, use_cache, &st, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    if (st.count == 0) {
        fprintf(stderr, "no register writes captured (subsong out of range?)\n");
        stream_free(&st);
        return 1;
    }

    printf("song: %s\n", st.info.name);
    printf("%lu writes over %.1f s (%s), speed %.2fx, %d loop(s)\n",
           (unsigned long)st.count,
           st.events[st.count - 1].t_ms / 1000.0,
           st.info.pal ? "PAL" : "NTSC", speed, loops);
    fflush(stdout);

    if (hw_connect() != 0) { stream_free(&st); return 1; }

    /* Start from a known state: clear every register, then let the song's own
     * init writes (all timestamped t=0) define the chip. */
    sid_clear_all_regs(s_opt.slot);
    sid_flush();

    int rc = 0;
    for (int loop = 0; loop < loops && rc == 0; loop++) {
        if (loops > 1) printf("--- loop %d/%d ---\n", loop + 1, loops);
        rc = replay(st.events, st.count, speed, pace);
        if (rc != 0) break;
        hw_mute();
        if (loop + 1 < loops) spfm_delay_ms(250);
    }

    hw_mute();
    hw_disconnect();
    stream_free(&st);
    return rc == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* playlist                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    char name[260];
} entry_t;

static int cmp_entry(const void *a, const void *b)
{
    return strcmp(((const entry_t *)a)->name, ((const entry_t *)b)->name);
}

static int scan_dir(const char *dir, entry_t **out, int *count)
{
    entry_t *list = NULL;
    int n = 0, cap = 0;

#ifdef _WIN32
    char pattern[512];
    snprintf(pattern, sizeof pattern, "%s/*.sid", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            entry_t *nl = (entry_t *)realloc(list, (size_t)cap * sizeof(*nl));
            if (!nl) { free(list); FindClose(h); return -1; }
            list = nl;
        }
        snprintf(list[n].name, sizeof list[n].name, "%s", fd.cFileName);
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!has_suffix(de->d_name, ".sid")) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            entry_t *nl = (entry_t *)realloc(list, (size_t)cap * sizeof(*nl));
            if (!nl) { free(list); closedir(d); return -1; }
            list = nl;
        }
        snprintf(list[n].name, sizeof list[n].name, "%s", de->d_name);
        n++;
    }
    closedir(d);
#endif

    if (n > 1) qsort(list, (size_t)n, sizeof(*list), cmp_entry);
    *out = list;
    *count = n;
    return 0;
}

static int cmd_playlist(int argc, char **argv)
{
    const char *dir = "sid";
    double seconds = 60.0;
    double speed = 1.0;
    int start_idx = 0;
    int subsong = 0;
    int use_cache = 1;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc)      seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--speed") && i + 1 < argc)   speed = atof(argv[++i]);
        else if (!strcmp(argv[i], "--subsong") && i + 1 < argc) subsong = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--from") && i + 1 < argc)    start_idx = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-cache"))                use_cache = 0;
        else if (!strcmp(argv[i], "--cache-dir") && i + 1 < argc)
            sidplay_set_cache_dir(argv[++i]);
        else if (!strcmp(argv[i], "--raw-filter"))              s_keep_filter = 1;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            return 1;
        } else dir = argv[i];
    }

    entry_t *files = NULL;
    int nfiles = 0;
    if (scan_dir(dir, &files, &nfiles) != 0 || nfiles == 0) {
        fprintf(stderr, "no .sid files in '%s'\n", dir);
        free(files);
        return 1;
    }

    if (hw_connect() != 0) { free(files); return 1; }

    printf("\n%d files in %s\n", nfiles, dir);
    printf("keys: [n] next  [b] back  [,] prev subsong  [.] next subsong  [q] quit\n\n");

    int idx = start_idx < 0 ? 0 : (start_idx >= nfiles ? nfiles - 1 : start_idx);
    int rc = 0;

    while (idx >= 0 && idx < nfiles) {
        char path[768];
        snprintf(path, sizeof path, "%s/%s", dir, files[idx].name);

        sid_song_info_t probe;
        int nsongs = 1;
        if (sid_song_probe(path, &probe) == 0) {
            nsongs = probe.n_songs ? probe.n_songs : 1;
            if (subsong >= nsongs) subsong = nsongs - 1;
            printf("[%d/%d] %s\n        %s / %s  (%d subsong%s)\n",
                   idx + 1, nfiles, files[idx].name,
                   probe.name[0] ? probe.name : "?", probe.author[0] ? probe.author : "?",
                   nsongs, nsongs == 1 ? "" : "s");
        } else {
            printf("[%d/%d] %s\n", idx + 1, nfiles, files[idx].name);
        }
        printf("        subsong %d\n", subsong);
        fflush(stdout);

        stream_t st;
        char err[128];
        if (load_stream(path, subsong, seconds, use_cache, &st, err, sizeof err) != 0) {
            fprintf(stderr, "  skip: %s\n", err);
            idx++;
            continue;
        }

        s_stop_requested = 0;
        s_next_action = 0;

        if (st.count) {
            sid_clear_all_regs(s_opt.slot);
            sid_flush();
            spfm_capture_clear();   /* per-track byte count in dry-run */
            /* In dry-run the playlist would otherwise take a minute per track
             * just to sleep; there is nothing to hear, so skip the pacing. */
            replay(st.events, st.count, speed, !s_opt.dry_run);
        }
        hw_mute();
        stream_free(&st);

        if (s_stop_requested) break;

        switch (s_next_action) {
        case 'b': idx = idx > 0 ? idx - 1 : 0; printf("[BACK]\n\n"); break;
        case ',': subsong = subsong > 0 ? subsong - 1 : 0; printf("[SUBSONG %d]\n\n", subsong); break;
        case '.': subsong++; printf("[SUBSONG %d]\n\n", subsong); break;
        default:  idx++; break;
        }
        spfm_delay_ms(300);
    }

    printf("\nplaylist end\n");
    hw_disconnect();
    free(files);
    return rc;
}

/* ------------------------------------------------------------------ */
/* dispatcher                                                          */
/* ------------------------------------------------------------------ */

/* Each entry point takes the shared options, applies the ones that belong to
 * the driver, then runs its own command. Keeping them separate lets sidtest's
 * dispatcher stay a table of calls rather than a chain of string compares. */
static void apply_common(const sidplay_opts_t *opts)
{
    if (!opts) return;
    s_opt = *opts;
    if (s_opt.write_mode > 0)
        sid_set_write_mode((sid_write_mode_t)s_opt.write_mode);
}

int sidplay_do_play(int argc, char **argv, const sidplay_opts_t *opts)
{
    apply_common(opts);
    return cmd_play(argc, argv);
}

int sidplay_do_extract(int argc, char **argv, const sidplay_opts_t *opts)
{
    apply_common(opts);
    return cmd_extract(argc, argv);
}

int sidplay_do_info(int argc, char **argv, const sidplay_opts_t *opts)
{
    apply_common(opts);
    return cmd_info(argc, argv);
}

int sidplay_do_list(int argc, char **argv, const sidplay_opts_t *opts)
{
    apply_common(opts);
    return cmd_playlist(argc, argv);
}
