/*
 * spfmplayer.c - SPFM SIDVGM Player: plays SID music on a real 6581/8580.
 *
 * This is the listening program, as opposed to build\sidtest.exe, which is the
 * hardware bring-up and register-poking tool. It accepts everything this
 * project can produce or consume:
 *
 *   .sid      run the tune and push its register writes out as they happen
 *   .sidvgm   replay a recorded register stream (the project's own container)
 *   .sidreg   replay the old text stream
 *   <dir>     playlist of the .sid files inside
 *
 * For .sid the default recorder is libsidplayfp + its own VIC/CIA, not the
 * embedded fake6502. That matters for two classes of tune:
 *
 *   - digi drums, which write $D418 faster than once per frame. The fake6502
 *     path only calls play() once per frame and loses every in-frame write, so
 *     those tunes come out silent; libsidplayfp calls play() from the timer or
 *     raster interrupt exactly as the original code asked.
 *   - real RSID tunes, whose play address is 0000 and which install their own
 *     CIA/VIC vectors. There is nothing for a "call play(0000)" guess to hit,
 *     so the old path played them at the wrong rate or not at all.
 *
 * The emulation is streamed one frame at a time: frame N+1 is emulated while
 * frame N is still sounding on the chip, so an endless .sid needs no
 * pre-rendering and there is no stall.
 *
 * Scheduling is the same absolute-deadline model as sidtest's player: one
 * start timestamp, every event waits until start + t_ms. Sleeping "for N ms"
 * per event would accumulate the scheduler's error and one frame of drift
 * (19.96 ms) is already audible.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "sid_rsid.h"
#include "sidvgm.h"
#include "sid_lsfp.h"
#include "sid6581.h"
#include "spfm_transport.h"

#ifdef _WIN32
#include <conio.h>
#include <direct.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <pthread.h>
#endif

#define PLAYER_NAME    "SPFM SIDVGM Player"
#define PLAYER_VERSION "1.0"

/* ------------------------------------------------------------------ */
/* options                                                             */
/* ------------------------------------------------------------------ */

static struct {
    uint8_t slot;
    int     device;
    int     dry_run;
    double  seconds;        /* 0 = until interrupted */
    double  speed;
    int     subsong;        /* -1 = the file's default */
    int     use_lsfp;       /* 1 = libsidplayfp recorder, 0 = built-in 6502 */
    int     loop;           /* buffer streams: 0 = once, N = N times, -1 = forever */
    int     flush_register; /* 0 = one transfer per frame, 1 = per register */
    int     keep_filter;
} opt = {
    SID_DEFAULT_SLOT, 0, 0, 0.0, 1.0, -1, 1, 0, 0, 0
};

/* Minimum gap (in 1/44100 s ticks) before a mid-frame flush is worth it.
 * Ported from sidtest's player, where it was measured: a burst of writes with
 * no gap stays in one USB transfer, and the frame boundary always flushes. */
#define FLUSH_THRESHOLD_TICKS 4

/* ------------------------------------------------------------------ */
/* hardware                                                            */
/* ------------------------------------------------------------------ */

static int hw_connect(void)
{
    if (opt.dry_run) {
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
    if (opt.device >= n) {
        fprintf(stderr, "--device %d out of range, %d present\n", opt.device, n);
        return -1;
    }
    if (spfm_open(opt.device) != 0) return -1;
    printf("device %d: %s\n", opt.device, descs[opt.device]);
    return 0;
}

static void hw_disconnect(void)
{
    if (!opt.dry_run) spfm_close();
}

static void hw_mute(void)
{
    for (uint8_t v = 0; v < 3; v++)
        sid_write(opt.slot, (uint8_t)(v * 7 + SID_REG_CONTROL), 0x00);
    sid_write(opt.slot, SID_REG_MODE_VOL, 0x00);
    sid_flush();
}

/* ------------------------------------------------------------------ */
/* keyboard: a blocking reader thread, so a key works even mid-frame   */
/* ------------------------------------------------------------------ */

static volatile int s_action = 0;   /* 'q','n','b',',','.' or 0 */
static int          s_interrupt = 0;

static int kb_read_blocking(void)
{
#ifdef _WIN32
    if (!_isatty(_fileno(stdin))) {
        unsigned char c;
        if (_read(0, &c, 1) != 1) return -1;
        return c;
    }
    int c = _getch();
    if (c == 0 || c == 0xE0) { _getch(); return 0; }
    return c;
#else
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;
    return c;
#endif
}

static void kb_thread_body(void)
{
    for (;;) {
        int c = kb_read_blocking();
        if (c < 0) break;
        switch (c) {
        case 'q': case 'Q': case 27: s_action = 'q'; break;
        case 'n': case 'N':          s_action = 'n'; break;
        case 'b': case 'B':          s_action = 'b'; break;
        case ',':                    s_action = ','; break;
        case '.':                    s_action = '.'; break;
        default: break;
        }
    }
}

#ifdef _WIN32
static unsigned __stdcall kb_thread(void *arg)
{
    (void)arg;
    kb_thread_body();
    return 0;
}
#else
static void *kb_thread(void *arg)
{
    (void)arg;
    kb_thread_body();
    return NULL;
}
#endif

static void kb_start(void)
{
#ifdef _WIN32
    if (!_isatty(_fileno(stdin)) && !getenv("SIDPLAY_KEYS")) return;
    uintptr_t h = _beginthreadex(NULL, 0, kb_thread, NULL, 0, NULL);
    if (h) CloseHandle((HANDLE)h);
#else
    static int raw_done = 0;
    if (!raw_done && isatty(STDIN_FILENO)) {
        struct termios t;
        if (tcgetattr(STDIN_FILENO, &t) == 0) {
            t.c_lflag &= ~(ICANON | ECHO);
            t.c_cc[VMIN] = 1;
            t.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &t);
        }
        raw_done = 1;
    }
    pthread_t th;
    pthread_create(&th, NULL, kb_thread, NULL);
    pthread_detach(th);
#endif
}

static int kb_take(void)
{
    int a = s_action;
    if (a) s_action = 0;
    return a;
}

/* ------------------------------------------------------------------ */
/* scheduling                                                          */
/* ------------------------------------------------------------------ */

static void emit(const sid_event_t *e)
{
    if (opt.keep_filter) {
        if (e->reg == SID_REG_RES_FILT || e->reg == SID_REG_FC_LO ||
            e->reg == SID_REG_FC_HI)
            return;
        if (e->reg == SID_REG_MODE_VOL) {
            sid_write(opt.slot, e->reg, (uint8_t)(e->val & SID_VOLUME_MASK));
            if (opt.flush_register) sid_flush();
            return;
        }
    }

    sid_write(opt.slot, e->reg, e->val);
    if (opt.flush_register) sid_flush();
}

/* Wait until `due_us`, servicing keys. Returns 0 if a key interrupted the
 * wait, the key code otherwise (0 when the wait completed). */
static int wait_due(int64_t due_us)
{
    while (due_us > spfm_now_us()) {
        int k = kb_take();
        if (k) { s_interrupt = k; return k; }
        int64_t left = due_us - spfm_now_us();
        spfm_wait_until_us(spfm_now_us() + (left > 5000 ? 5000 : left));
    }
    return 0;
}

/* Replay a finished event buffer (a .sidvgm / .sidreg / bounded recording).
 *
 * Returns 0 normally, -1 when the user asked to quit and 1 when they asked for
 * the next track; both are reported through *interrupted_key.
 *
 * With a dry run and no pacing the whole buffer goes out as fast as the
 * buffer accepts it: waiting 60 s just to prove the bytes came out right is
 * not a useful test. */
static int replay_buffer(const sid_event_t *ev, size_t n, int paced, int *key)
{
    *key = 0;
    if (n == 0) return 0;

    if (opt.dry_run && !paced) {
        for (size_t i = 0; i < n; i++) emit(&ev[i]);
        sid_flush();
        printf("  dry-run: %lu writes -> %lu bytes\n",
               (unsigned long)n, (unsigned long)spfm_capture_size());
        return 0;
    }

    const double us_per_ms = 1000.0 / opt.speed;
    const int64_t start = spfm_now_us();
    int64_t worst_lag_us = 0;
    size_t last_sec = (size_t)-1;

    sid_set_deferred_flush(!opt.flush_register);

    for (size_t i = 0; i < n; i++) {
        int64_t due = start + (int64_t)(ev[i].t_ms * us_per_ms);

        if (due > spfm_now_us()) {
            /* Ship what is already due before blocking, so a long wait
             * mid-frame does not hold the batch back. */
            if (spfm_now_us() + (int64_t)FLUSH_THRESHOLD_TICKS * 1000000 / 44100 <= due)
                sid_flush();
            int k = wait_due(due);
            if (k) {
                sid_set_deferred_flush(false);
                sid_flush();
                *key = k;
                return k == 'q' ? -1 : 1;
            }
        } else {
            int64_t lag = spfm_now_us() - due;
            if (lag > worst_lag_us) worst_lag_us = lag;
        }

        emit(&ev[i]);

        int k = kb_take();
        if (k) {
            sid_set_deferred_flush(false);
            sid_flush();
            *key = k;
            return k == 'q' ? -1 : 1;
        }

        size_t sec = (size_t)(ev[i].t_ms / 1000.0);
        if (sec != last_sec) {
            last_sec = sec;
            printf("\r  %5.1f s  %lu/%lu  lag %ld ms   ",
                   ev[i].t_ms / 1000.0, (unsigned long)(i + 1),
                   (unsigned long)n, (long)(worst_lag_us / 1000));
            fflush(stdout);
        }
    }

    sid_flush();
    sid_set_deferred_flush(false);
    printf("\r  %5.1f s  %lu writes  worst lag %ld ms        \n",
           ev[n - 1].t_ms / 1000.0, (unsigned long)n,
           (long)(worst_lag_us / 1000));
    return 0;
}

/* Play a .sid live through libsidplayfp, one frame at a time.
 *
 * The recorder hands back frame k's writes as soon as they exist, with
 * absolute timestamps, so the emit loop is the same as a buffer replay. What
 * makes it unbounded is that the next frame is emulated during the current
 * frame's wall-clock time. */
static int play_live_lsfp(const char *path, int subsong, sid_song_info_t *out,
                          int *key)
{
    char err[160];
    sid_lsfp_stream_t *s = sid_lsfp_stream_open(path, subsong, err, sizeof err);
    if (!s) {
        fprintf(stderr, "  %s\n", err);
        return 2;               /* unloadable, not a quit */
    }

    if (out) {
        memset(out, 0, sizeof *out);
        if (sid_song_probe(path, out) != 0) out->pal = 1;
    }

    const double frame_ms = sid_lsfp_stream_frame_ms(s);
    const double us_per_ms = 1000.0 / opt.speed;
    const int64_t start = spfm_now_us();
    int64_t worst_lag_us = 0;
    double played_ms = 0.0;
    double next_report = 5000.0;

    sid_set_deferred_flush(!opt.flush_register);

    /* First call also returns the init writes, all stamped 0. */
    const sid_event_t *ev = NULL;
    size_t n = 0;
    if (sid_lsfp_stream_next(s, &ev, &n) != 0) {
        sid_lsfp_stream_close(s);
        return 2;
    }
    for (size_t i = 0; i < n; i++) emit(&ev[i]);
    sid_flush();

    for (;;) {
        if (sid_lsfp_stream_next(s, &ev, &n) != 0) break;

        for (size_t i = 0; i < n; i++) {
            int64_t due = start + (int64_t)(ev[i].t_ms * us_per_ms);
            if (due > spfm_now_us()) {
                if (spfm_now_us() + (int64_t)FLUSH_THRESHOLD_TICKS * 1000000 / 44100 <= due)
                    sid_flush();
                int k = wait_due(due);
                if (k) {
                    sid_set_deferred_flush(false);
                    sid_flush();
                    sid_lsfp_stream_close(s);
                    *key = k;
                    return k == 'q' ? -1 : 1;
                }
            } else {
                int64_t lag = spfm_now_us() - due;
                if (lag > worst_lag_us) worst_lag_us = lag;
            }
            emit(&ev[i]);
        }
        sid_flush();

        int k = kb_take();
        if (k) {
            sid_set_deferred_flush(false);
            sid_flush();
            sid_lsfp_stream_close(s);
            *key = k;
            return k == 'q' ? -1 : 1;
        }

        played_ms += frame_ms;

        if (opt.seconds > 0.0 && played_ms >= opt.seconds * 1000.0) {
            sid_set_deferred_flush(false);
            sid_flush();
            sid_lsfp_stream_close(s);
            printf("\r  %5.1f s  reached --seconds limit        \n", played_ms / 1000.0);
            return 0;
        }

        if (played_ms >= next_report) {
            next_report += 5000.0;
            printf("\r  %4.0f s  lag %ld ms    ",
                   played_ms / 1000.0, (long)(worst_lag_us / 1000));
            fflush(stdout);
        }
    }

    sid_flush();
    sid_set_deferred_flush(false);
    sid_lsfp_stream_close(s);
    return 0;
}

/* Same, through the built-in 6502. Kept because it is the only recorder with
 * no libsidplayfp dependency and is useful when comparing the two; it cannot
 * see intra-frame writes, so digi tunes lose their drums here. */
static int play_live_native(const char *path, int subsong, sid_song_info_t *out,
                            int *key)
{
    char err[160];
    sid_song_info_t info;
    if (sid_stream_open(path, subsong, &info, err, sizeof err) != 0) {
        fprintf(stderr, "  %s\n", err);
        return 2;
    }
    if (out) *out = info;

    const double frame_ms = sid_stream_frame_ms();
    const double us_per_ms = 1000.0 / opt.speed;
    const int64_t start = spfm_now_us();
    int64_t worst_lag_us = 0;
    double played_ms = 0.0;
    double next_report = 5000.0;

    sid_set_deferred_flush(!opt.flush_register);

    const sid_event_t *ev = NULL;
    size_t n = 0;
    if (sid_stream_next_frame(&ev, &n) != 0) {
        sid_stream_close();
        return 2;
    }
    for (size_t i = 0; i < n; i++) emit(&ev[i]);
    sid_flush();

    for (;;) {
        if (sid_stream_next_frame(&ev, &n) != 0) break;

        for (size_t i = 0; i < n; i++) {
            int64_t due = start + (int64_t)(ev[i].t_ms * us_per_ms);
            if (due > spfm_now_us()) {
                if (spfm_now_us() + (int64_t)FLUSH_THRESHOLD_TICKS * 1000000 / 44100 <= due)
                    sid_flush();
                int k = wait_due(due);
                if (k) {
                    sid_set_deferred_flush(false);
                    sid_flush();
                    sid_stream_close();
                    *key = k;
                    return k == 'q' ? -1 : 1;
                }
            } else {
                int64_t lag = spfm_now_us() - due;
                if (lag > worst_lag_us) worst_lag_us = lag;
            }
            emit(&ev[i]);
        }
        sid_flush();

        int k = kb_take();
        if (k) {
            sid_set_deferred_flush(false);
            sid_flush();
            sid_stream_close();
            *key = k;
            return k == 'q' ? -1 : 1;
        }

        played_ms += frame_ms;

        if (opt.seconds > 0.0 && played_ms >= opt.seconds * 1000.0) {
            sid_set_deferred_flush(false);
            sid_flush();
            sid_stream_close();
            printf("\r  %5.1f s  reached --seconds limit        \n", played_ms / 1000.0);
            return 0;
        }

        if (played_ms >= next_report) {
            next_report += 5000.0;
            printf("\r  %4.0f s  lag %ld ms    ",
                   played_ms / 1000.0, (long)(worst_lag_us / 1000));
            fflush(stdout);
        }
    }

    sid_flush();
    sid_set_deferred_flush(false);
    sid_stream_close();
    return 0;
}

/* ------------------------------------------------------------------ */
/* input files                                                         */
/* ------------------------------------------------------------------ */

static int has_suffix(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    if (a < b) return 0;
    s += a - b;
    for (size_t i = 0; i < b; i++)
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)suf[i])) return 0;
    return 1;
}

/* Everything spfmplayer can open, so a folder of any mix plays. */
static int is_playable_name(const char *name)
{
    return has_suffix(name, ".sid") || has_suffix(name, ".sidvgm") ||
           has_suffix(name, ".sidreg");
}

/* Play one file. Returns 0 when the file finished, 1 if the user asked for the
 * next track, -1 to quit, 2 on a load/emulation error (skip it). */
static int play_file(const char *path, int *key)
{
    *key = 0;

    /* ---- a recorded stream ---- */
    if (sidvgm_is_file(path) || has_suffix(path, ".sidreg")) {
        sid_event_t *ev = NULL;
        size_t n = 0;
        sid_song_info_t info;
        memset(&info, 0, sizeof info);

        /* sidvgm_read() hands back a malloc'd buffer, sid_sidreg_read() a
         * static one that lives until the next call. Only the first may be
         * freed. */
        const int is_vgm = sidvgm_is_file(path);
        int rc = is_vgm ? sidvgm_read(path, &ev, &n, &info)
                        : sid_sidreg_read(path, &ev, &n);
        if (rc != 0) {
            fprintf(stderr, "  cannot read register stream: %s\n", path);
            return 2;
        }
        if (!info.name[0]) {
            const char *base = path;
            for (const char *p = path; *p; p++)
                if (*p == '/' || *p == '\\') base = p + 1;
            snprintf(info.name, sizeof info.name, "%s", base);
        }
        printf("stream: %s\n  %lu writes / %.1f s\n", info.name,
               (unsigned long)n, sidvgm_length_ms(ev, n, &info) / 1000.0);
        fflush(stdout);

        /* A buffer has a natural end, so it can be repeated on request. */
        int loops = (opt.loop == 0) ? 1 : opt.loop;
        for (int l = 0; loops < 0 || l < loops; l++) {
            if (loops != 1)
                printf("  pass %d%s\n", l + 1, loops < 0 ? " (forever)" : "");
            rc = replay_buffer(ev, n, 1, key);
            if (rc != 0) break;
            if (loops < 0 || l + 1 < loops) {
                sid_clear_all_regs(opt.slot);
                sid_flush();
            }
        }
        if (is_vgm) sid_song_free(ev);
        return rc;
    }

    /* ---- a .sid ---- */
    sid_song_info_t probe;
    if (sid_song_probe(path, &probe) != 0) {
        fprintf(stderr, "  cannot read .sid header: %s\n", path);
        return 2;
    }
    int subsong = opt.subsong;
    if (subsong < 0) subsong = probe.start_song ? probe.start_song - 1 : 0;
    if (subsong >= (int)probe.n_songs) subsong = probe.n_songs - 1;

    printf("song: %s / %s\n", probe.name, probe.author);
    printf("  %s, subsong %d/%u, %s recorder, speed %.2fx\n",
           probe.pal ? "PAL" : "NTSC", subsong, probe.n_songs,
           opt.use_lsfp ? "libsidplayfp" : "built-in 6502", opt.speed);
    fflush(stdout);

    if (opt.seconds > 0.0) {
        /* Bounded: record the whole span up front and replay it. Also the
         * only way to get a fixed artifact out of a sparse tune quickly. */
        sid_event_t *ev = NULL;
        size_t n = 0;
        char err[160];
        int rc;
        printf("  recording %.1f s...\n", opt.seconds);
        fflush(stdout);
        rc = opt.use_lsfp
           ? sid_lsfp_extract(path, subsong, opt.seconds, &ev, &n, &probe,
                              err, sizeof err)
           : sid_song_extract(path, subsong, opt.seconds, &ev, &n, &probe,
                              err, sizeof err);
        if (rc != 0) {
            fprintf(stderr, "  recording failed: %s\n", err);
            return 2;
        }
        printf("  %lu writes, playing\n", (unsigned long)n);
        rc = replay_buffer(ev, n, 1, key);
        sid_song_free(ev);
        return rc;
    }

    return opt.use_lsfp ? play_live_lsfp(path, subsong, NULL, key)
                        : play_live_native(path, subsong, NULL, key);
}

/* ------------------------------------------------------------------ */
/* playlist                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    char name[512];     /* relative to the playlist root, so a recursive walk
                         * keeps the subdirectory ("rsid/x.sidvgm") */
} entry_t;

static int cmp_entry(const void *a, const void *b)
{
    return strcmp(((const entry_t *)a)->name, ((const entry_t *)b)->name);
}

static int add_entry(entry_t **list, int *n, int *cap, const char *relname)
{
    if (*n == *cap) {
        int want = *cap ? *cap * 2 : 64;
        entry_t *nl = (entry_t *)realloc(*list, (size_t)want * sizeof(*nl));
        if (!nl) return -1;
        *list = nl;
        *cap = want;
    }
    snprintf((*list)[*n].name, sizeof (*list)[*n].name, "%.511s", relname);
    (*n)++;
    return 0;
}

/* Walk the whole tree: sid/ and sidvgm/ both keep an rsid/ subdirectory, and a
 * top-level-only listing silently hid almost every tune. */
static int scan_walk(const char *root, const char *rel,
                     entry_t **list, int *n, int *cap)
{
#ifdef _WIN32
    char pattern[1024];
    if (*rel) snprintf(pattern, sizeof pattern, "%s/%s/*", root, rel);
    else      snprintf(pattern, sizeof pattern, "%s/*", root);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return (*n > 0) ? 0 : -1;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;

        char child[1024];
        if (*rel) snprintf(child, sizeof child, "%s/%s", rel, fd.cFileName);
        else      snprintf(child, sizeof child, "%s", fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            scan_walk(root, child, list, n, cap);
            continue;
        }
        if (!is_playable_name(fd.cFileName)) continue;
        if (add_entry(list, n, cap, child) != 0) { FindClose(h); return -1; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    char full[1024];
    if (*rel) snprintf(full, sizeof full, "%s/%s", root, rel);
    else      snprintf(full, sizeof full, "%s", root);

    DIR *d = opendir(full);
    if (!d) return (*n > 0) ? 0 : -1;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;

        char child[1024];
        if (*rel) snprintf(child, sizeof child, "%s/%s", rel, de->d_name);
        else      snprintf(child, sizeof child, "%s", de->d_name);

        char childfull[1200];
        snprintf(childfull, sizeof childfull, "%s/%s", root, child);
        struct stat sst;
        if (stat(childfull, &sst) == 0 && S_ISDIR(sst.st_mode)) {
            scan_walk(root, child, list, n, cap);
            continue;
        }
        if (!is_playable_name(de->d_name)) continue;
        if (add_entry(list, n, cap, child) != 0) { closedir(d); return -1; }
    }
    closedir(d);
#endif
    return 0;
}

static int scan_dir(const char *dir, entry_t **out, int *count)
{
    entry_t *list = NULL;
    int n = 0, cap = 0;

    if (scan_walk(dir, "", &list, &n, &cap) != 0 && n == 0) {
        free(list);
        return -1;
    }

    if (n > 1) qsort(list, (size_t)n, sizeof(*list), cmp_entry);
    *out = list;
    *count = n;
    return 0;
}

static int run_playlist(const char *dir, int start_idx)
{
    entry_t *files = NULL;
    int nfiles = 0;
    if (scan_dir(dir, &files, &nfiles) != 0 || nfiles == 0) {
        fprintf(stderr, "no .sid files in '%s'\n", dir);
        free(files);
        return 1;
    }

    if (hw_connect() != 0) { free(files); return 1; }

    spfm_thread_boost_begin();
    kb_start();

    printf("%d files in %s\n", nfiles, dir);
    printf("keys: [n] next  [b] back  [,] prev subsong  [.] next subsong  [q] quit\n\n");

    int idx = start_idx < 0 ? 0 : (start_idx >= nfiles ? nfiles - 1 : start_idx);
    int subsong = (opt.subsong < 0) ? 0 : opt.subsong;

    while (idx >= 0 && idx < nfiles) {
        char path[768];
        snprintf(path, sizeof path, "%s/%s", dir, files[idx].name);

        sid_song_info_t probe;
        if (sid_song_probe(path, &probe) == 0) {
            int nsongs = probe.n_songs ? probe.n_songs : 1;
            if (subsong >= nsongs) subsong = nsongs - 1;
            printf("[%d/%d] %s\n        %s / %s  %d subsong%s\n",
                   idx + 1, nfiles, files[idx].name,
                   probe.name[0] ? probe.name : "?",
                   probe.author[0] ? probe.author : "?", nsongs,
                   nsongs == 1 ? "" : "s");
        } else {
            printf("[%d/%d] %s\n", idx + 1, nfiles, files[idx].name);
        }
        fflush(stdout);

        s_action = 0;
        s_interrupt = 0;

        sid_clear_all_regs(opt.slot);
        sid_flush();

        int local = subsong;
        opt.subsong = local;
        int key = 0;
        play_file(path, &key);
        hw_mute();

        int action = s_interrupt ? s_interrupt : kb_take();
        if (action == 'q') break;

        switch (action) {
        case 'b': idx = idx > 0 ? idx - 1 : 0; printf("\n[BACK]\n\n"); break;
        case ',': subsong = subsong > 0 ? subsong - 1 : 0;
                  printf("\n[SUBSONG %d]\n\n", subsong); break;
        case '.': subsong++; printf("\n[SUBSONG %d]\n\n", subsong); break;
        default:  idx++; break;
        }
        spfm_delay_ms(200);
    }

    printf("\nplaylist end\n");
    spfm_thread_boost_end();
    hw_disconnect();
    free(files);
    return 0;
}

/* ------------------------------------------------------------------ */
/* CLI                                                                 */
/* ------------------------------------------------------------------ */

static void usage(void)
{
    printf("%s %s\n\n", PLAYER_NAME, PLAYER_VERSION);
    printf("usage: spfmplayer <file|directory> [options]\n\n");
    printf("  .sid      run the tune and push its writes out as they happen\n");
    printf("  .sidvgm   replay a recorded register stream\n");
    printf("  .sidreg   replay the old text stream\n");
    printf("  <dir>     playlist: every .sid in the directory, sorted by name\n\n");
    printf("options:\n");
    printf("  --seconds N     play N seconds (default 0 = until a key)\n");
    printf("  --subsong N     subsong, 0-based (default: the file's own)\n");
    printf("  --speed X       playback rate (default 1.0)\n");
    printf("  --slot N        SPFM slot (default %u)\n", SID_DEFAULT_SLOT);
    printf("  --device N      FTDI device index (default 0)\n");
    printf("  --engine E      .sid recorder: lsfp (default) | native\n");
    printf("                  lsfp   = libsidplayfp: digi drums and the real\n");
    printf("                           CIA/VIC cadence; needed for digi + RSID\n");
    printf("                  native = built-in 6502 (pure C). No in-frame\n");
    printf("                           $D418, but plain PSID sounds better\n");
    printf("  --proto P       write protocol: 1step (default) | 2step\n");
    printf("  --flush M       USB flush granularity: command (default) | register\n");
    printf("  --loop N        repeat a buffered stream N times (0 = once, -1 = forever)\n");
    printf("  --raw-filter    drop the tune's filter setup, keep the volume\n");
    printf("  --timer T       wait primitive: hprec (default) | hybrid | mmtimer | vgmplay\n");
    printf("  --repeat N      send each write N times (default 1)\n");
    printf("  --gap N         NOP ticks between the two phases (default 0)\n");
    printf("  --gap-rand N    add 0..N random ticks to the gap\n");
    printf("  --dry-run       no hardware, just walk the byte stream\n");
    printf("  --list          list the .sid files in a directory\n");
    printf("  --help          this text\n\n");
    printf("while playing: [n] next  [b] back  [,] [.] subsong  [q] quit\n");
}

/* Walk the whole argv so options can sit before or after the file name. The
 * first non-option argument is the input path. */
static int parse_args(int argc, char **argv, const char **path, int *list_only,
                      int *from_idx)
{
    int proto_set = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--seconds") && i + 1 < argc)        opt.seconds = atof(argv[++i]);
        else if (!strcmp(a, "--speed") && i + 1 < argc)     opt.speed = atof(argv[++i]);
        else if (!strcmp(a, "--subsong") && i + 1 < argc)   opt.subsong = atoi(argv[++i]);
        else if (!strcmp(a, "--slot") && i + 1 < argc)      opt.slot = (uint8_t)atoi(argv[++i]);
        else if (!strcmp(a, "--device") && i + 1 < argc)    opt.device = atoi(argv[++i]);
        else if (!strcmp(a, "--loop") && i + 1 < argc)      opt.loop = atoi(argv[++i]);
        else if (!strcmp(a, "--from") && i + 1 < argc)      *from_idx = atoi(argv[++i]);
        else if (!strcmp(a, "--repeat") && i + 1 < argc)
            sid_set_write_repeat((uint32_t)strtoul(argv[++i], NULL, 0));
        else if (!strcmp(a, "--gap") && i + 1 < argc)
            sid_set_phase_gap((uint32_t)strtoul(argv[++i], NULL, 0));
        else if (!strcmp(a, "--gap-rand") && i + 1 < argc)
            sid_set_gap_random((uint32_t)strtoul(argv[++i], NULL, 0));
        else if (!strcmp(a, "--engine") && i + 1 < argc) {
            const char *e = argv[++i];
            if (!strcmp(e, "lsfp") || !strcmp(e, "libsidplayfp")) opt.use_lsfp = 1;
            else if (!strcmp(e, "native")) opt.use_lsfp = 0;
            else { fprintf(stderr, "--engine must be lsfp or native\n"); return -1; }
        }
        else if (!strcmp(a, "--proto") && i + 1 < argc) {
            const char *p = argv[++i];
            if (!strcmp(p, "1step") || !strcmp(p, "path-a")) {
                sid_set_write_mode(SID_WRITE_BURST);
            } else if (!strcmp(p, "2step") || !strcmp(p, "two-phase")) {
                sid_set_write_mode(SID_WRITE_TWO_PHASE);
            } else { fprintf(stderr, "--proto must be 1step or 2step\n"); return -1; }
            proto_set = 1;
        }
        else if (!strcmp(a, "--flush") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "register"))     opt.flush_register = 1;
            else if (!strcmp(m, "command")) opt.flush_register = 0;
            else { fprintf(stderr, "--flush must be command or register\n"); return -1; }
        }
        else if (!strcmp(a, "--timer") && i + 1 < argc) {
            if (spfm_timer_set(argv[++i]) != 0) {
                fprintf(stderr, "--timer must be hprec|hybrid|mmtimer|vgmplay\n");
                return -1;
            }
        }
        else if (!strcmp(a, "--raw-filter")) opt.keep_filter = 1;
        else if (!strcmp(a, "--dry-run"))    opt.dry_run = 1;
        else if (!strcmp(a, "--list"))       *list_only = 1;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); exit(0); }
        else if (a[0] == '-') {
            fprintf(stderr, "unknown option '%s'\n", a);
            return -1;
        }
        else if (!*path) *path = a;
        else { fprintf(stderr, "unexpected argument '%s'\n", a); return -1; }
    }

    /* Same measured default as sidtest: one Path A command per write is the
     * protocol that plays the whole sid/ directory. */
    if (!proto_set) sid_set_write_mode(SID_WRITE_BURST);
    if (opt.speed <= 0.0) opt.speed = 1.0;
    if (opt.slot > SPFM_MAX_SLOT) {
        fprintf(stderr, "--slot must be 0..%d\n", SPFM_MAX_SLOT);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    int list_only = 0;
    int from_idx = 0;

    if (argc < 2) { usage(); return 1; }
    if (parse_args(argc, argv, &path, &list_only, &from_idx) != 0) {
        usage();
        return 1;
    }
    if (!path) { usage(); return 1; }

    printf("%s %s\n", PLAYER_NAME, PLAYER_VERSION);

    /* A directory is a playlist. */
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    int is_dir = (attr != INVALID_FILE_ATTRIBUTES) &&
                 (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    int is_dir = (stat(path, &st) == 0) && S_ISDIR(st.st_mode);
#endif

    if (is_dir) {
        if (list_only) {
            entry_t *files = NULL;
            int n = 0;
            if (scan_dir(path, &files, &n) != 0 || n == 0) {
                fprintf(stderr, "no .sid files in %s\n", path);
                free(files);
                return 1;
            }
            for (int i = 0; i < n; i++) printf("  %s\n", files[i].name);
            free(files);
            return 0;
        }
        return run_playlist(path, from_idx);
    }

    if (opt.use_lsfp && !sid_lsfp_available()) {
        fprintf(stderr, "this build has no libsidplayfp; use --engine native\n");
        return 1;
    }

    if (hw_connect() != 0) return 1;

    sid_clear_all_regs(opt.slot);
    sid_flush();
    spfm_thread_boost_begin();
    kb_start();

    printf("write protocol: %s   timer: %s   flush: %s\n",
           sid_get_write_mode() == SID_WRITE_BURST ? "1step" : "2step",
           spfm_timer_name(),
           opt.flush_register ? "register" : "command");

    int key = 0;
    int rc = play_file(path, &key);

    spfm_thread_boost_end();
    hw_mute();
    hw_disconnect();
    /* rc 2 = the file could not be loaded; -1 = the user pressed q. Quitting
     * is not a failure, so only a real error sets the exit status. */
    return (rc == 2) ? 1 : 0;
}
