/*
 * rpfm_sid.c - RPFM SID player: plays SID music on the RP2350A + real 6581.
 *
 * This is src/spfmplayer.c with the transport swapped out. Everything above
 * the hardware layer is the same program: the same playlist walk, the same
 * keyboard reader, the same libsidplayfp recorder for .sid, the same
 * .sidvgm / .sidreg replay, the same "one start stamp, absolute deadlines"
 * scheduling idea.
 *
 * What changed, and why:
 *
 *   SPFM transport            -> RPFM HID frames
 *   sid_write() + sid_flush() -> rpfm_sid_push(dt_us, reg, val)
 *   host-side deadline waits  -> the firmware owns the deadline
 *   --pad / --digi / --proto  -> gone: those existed to make a 1.5 Mbaud UART
 *                                reproduce sub-frame timing. Here the event
 *                                carries its own microsecond delta and the
 *                                chip is written by the board, so there is no
 *                                wire-time problem left to solve.
 *
 * The one idea worth keeping from the SPFM player is the host's music clock:
 * we never sleep "for N ms" per event, we compute where each event sits in the
 * tune and push it there. The difference is that we no longer wait for the
 * deadline here - we only keep the board's event queue a few hundred ms ahead
 * and let it wait. So host scheduling jitter shifts *when an event arrives*,
 * never when it plays, and it cannot accumulate.
 *
 * Recorder: .sid goes through libsidplayfp (its own 6502/VIC/CIA), not the
 * embedded fake6502. That is the reference cadence: CIA timer tunes and
 * $D418 digi drums write faster than once a frame, and only libsidplayfp
 * reports those writes when they actually happen. --engine native is kept
 * because it is the copied code path, not because it is the right default.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "sid_rsid.h"
#include "sidvgm.h"
#include "sid_lsfp.h"
#include "rpfm_link.h"

#ifdef _WIN32
#include <conio.h>
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

#define PLAYER_NAME    "RPFM SID Player"
#define PLAYER_VERSION "0.1"

/* How far ahead of the wall clock the host may run. The board's event queue is
 * 8192 entries, and the densest SID tune in this collection peaks at about 800
 * writes/s, so 400 ms is ~320 queued events - a comfortable margin that also
 * absorbs a scheduling hiccup. Larger costs nothing but makes a skip feel
 * laggy. */
#define LEAD_US 400000

/* A .sid has no length field, so a bounded conversion needs a default. Same
 * number the SPFM player uses. */
#define SID_DEFAULT_SECONDS 180.0

/* ------------------------------------------------------------------ */
/* options                                                             */
/* ------------------------------------------------------------------ */

static struct {
    int    dry_run;
    double seconds;        /* 0 = until interrupted / end of stream */
    double speed;
    int    subsong;        /* -1 = the file's default */
    int    use_lsfp;       /* 1 = libsidplayfp recorder, 0 = built-in 6502 */
    int    loop;           /* buffer streams: 0 = once, N = N times, -1 = for ever */
} opt = {
    0, 0.0, 1.0, -1, 1, 0
};

/* ------------------------------------------------------------------ */
/* hardware                                                            */
/* ------------------------------------------------------------------ */

static int hw_connect(void)
{
    if (opt.dry_run) return 0;
    if (rpfm_link_open() != 0) {
        fprintf(stderr, "rpfm_sid_player not found (VID=2E8A PID=1090)\n");
        return -1;
    }
    return 0;
}

static void hw_disconnect(void)
{
    if (!opt.dry_run) rpfm_link_close();
}

/* Clear every register in one frame. 25 register/value pairs = 50 bytes, which
 * fits a single CMD_SID_WRITE payload. Doing this at a track boundary is what
 * keeps a previous tune's gate/envelope state out of the next one. */
static void hw_clear_regs(void)
{
    if (opt.dry_run) return;
    uint8_t p[50];
    uint8_t n = 0;
    for (uint8_t r = 0; r <= 0x18; r++) { p[n++] = r; p[n++] = 0x00; }
    rpfm_link_send(CMD_SID_WRITE, p, n, NULL);
}

static void hw_mute(void)
{
    hw_clear_regs();
}

/* The tune's own region decides the clock the board generates. A PAL tune on a
 * 1 MHz clock would be 26 cents sharp, which is why this is not a constant. */
static void hw_set_clock(const sid_song_info_t *info)
{
    if (opt.dry_run) return;
    rpfm_set_phi2(info && !info->pal);
}

/* ------------------------------------------------------------------ */
/* keyboard: a blocking reader thread, so a key works mid-stream       */
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
/* scheduling: push events, let the board hold the deadline            */
/* ------------------------------------------------------------------ */

static int64_t s_start_us = 0;     /* wall clock at the first event */
static int64_t s_pushed_us = 0;    /* music position already handed over */

static void sched_reset(void)
{
    s_start_us  = rpfm_now_us();
    s_pushed_us = 0;
    rpfm_sid_stream_begin();
}

/* Sleep, but stay responsive to keys. Returns the key if one arrived. */
static int sched_sleep_us(int64_t us)
{
    int64_t until = rpfm_now_us() + us;
    while (rpfm_now_us() < until) {
        int k = kb_take();
        if (k) { s_interrupt = k; return k; }
#ifdef _WIN32
        Sleep(1);
#else
        usleep(1000);
#endif
    }
    return 0;
}

/* Hand one event over. `t_ms` is its position in the tune, so the delta is
 * computed against absolute music time and no rounding error accumulates.
 *
 * If the event is further ahead than LEAD_US the caller is early, so we wait:
 * that is the only flow control in the system, and it bounds the board's
 * queue by construction. */
static int push_event(double t_ms, uint8_t reg, uint8_t val)
{
    int64_t due = (int64_t)(t_ms * 1000.0 / opt.speed + 0.5);

    while (due - (rpfm_now_us() - s_start_us) > LEAD_US) {
        int64_t head = due - LEAD_US - (rpfm_now_us() - s_start_us);
        int k = sched_sleep_us(head > 5000 ? 5000 : head);
        if (k) return k;
    }

    int64_t dt = due - s_pushed_us;
    if (dt < 0) dt = 0;

    /* The delta field is 16 bits. A longer gap is split into wait-only events:
     * addr 0xFF tells the firmware to advance the deadline without writing. */
    while (dt > 65535) {
        if (!rpfm_sid_push(65535, 0xFF, 0x00)) return 'q';
        dt -= 65535;
        s_pushed_us += 65535;
    }
    if (!rpfm_sid_push((uint16_t)dt, reg, val)) return 'q';
    s_pushed_us = due;
    return 0;
}

/* Wait for the board to consume what it already has. Called before leaving a
 * tune so the last notes are not cut off by the next file's init burst. */
static void drain_tail(void)
{
    int64_t music_us = s_pushed_us;
    while (rpfm_now_us() - s_start_us < music_us) {
        int k = kb_take();
        if (k) break;
#ifdef _WIN32
        Sleep(5);
#else
        usleep(5000);
#endif
    }
    if (opt.dry_run) return;
    rpfm_sid_flush();
}

/* ------------------------------------------------------------------ */
/* replay a finished stream                                            */
/* ------------------------------------------------------------------ */

/* Returns 0 normally, -1 when the user asked to quit, 1 for "next track". */
static int replay_buffer(const sid_event_t *ev, size_t n, int *key)
{
    *key = 0;
    if (n == 0) return 0;

    if (opt.dry_run) {
        printf("  dry-run: %lu writes would go out\n", (unsigned long)n);
        return 0;
    }

    sched_reset();

    /* --seconds caps a longer stream; a shorter one ends at its last write. */
    double limit_ms = opt.seconds > 0.0 ? opt.seconds * 1000.0 : 1e18;

    int64_t start = rpfm_now_us();
    size_t last_sec = (size_t)-1;
    size_t sent = 0;
    double last_ms = 0.0;

    for (size_t i = 0; i < n; i++) {
        if (ev[i].t_ms > limit_ms) break;

        int k = push_event(ev[i].t_ms, ev[i].reg, ev[i].val);
        if (k) {
            *key = k;
            s_interrupt = k;
            return k == 'q' ? -1 : 1;
        }
        sent = i + 1;
        last_ms = ev[i].t_ms;

        size_t sec = (size_t)(ev[i].t_ms / 1000.0);
        if (sec != last_sec) {
            last_sec = sec;
            printf("\r  %5.1f s  %lu/%lu   ",
                   ev[i].t_ms / 1000.0, (unsigned long)(i + 1),
                   (unsigned long)n);
            fflush(stdout);
        }
        (void)start;
    }

    rpfm_sid_flush();
    printf("\r  %5.1f s  %lu writes sent      \n", last_ms / 1000.0,
           (unsigned long)sent);

    /* Let the board play out the tail we have already handed over. Measured
     * from the last write actually sent, not from the end of the file: a
     * --seconds cut must not then wait out the rest of the stream. */
    int64_t tail_us = (int64_t)(last_ms * 1000.0 / opt.speed);
    while (rpfm_now_us() - s_start_us < tail_us) {
        int k = kb_take();
        if (k) { *key = k; s_interrupt = k; return k == 'q' ? -1 : 1; }
#ifdef _WIN32
        Sleep(5);
#else
        usleep(5000);
#endif
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* live .sid through libsidplayfp                                      */
/* ------------------------------------------------------------------ */

static int play_live_lsfp(const char *path, int subsong, sid_song_info_t *out,
                          int *key)
{
    char err[160];
    sid_lsfp_stream_t *s = sid_lsfp_stream_open(path, subsong, err, sizeof err);
    if (!s) {
        fprintf(stderr, "  %s\n", err);
        return 2;
    }

    if (out) {
        memset(out, 0, sizeof *out);
        if (sid_song_probe(path, out) != 0) out->pal = 1;
    }

    const double frame_ms = sid_lsfp_stream_frame_ms(s);

    if (opt.dry_run) {
        sid_lsfp_stream_close(s);
        printf("  dry-run: would stream one frame every %.2f ms\n", frame_ms);
        return 0;
    }

    sched_reset();

    /* The first call also returns the init writes, all stamped 0. */
    const sid_event_t *ev = NULL;
    size_t n = 0;
    double played_ms = 0.0;
    double next_report = 5000.0;
    double limit_ms = opt.seconds > 0.0 ? opt.seconds * 1000.0 : 1e18;

    for (;;) {
        if (sid_lsfp_stream_next(s, &ev, &n) != 0) break;

        for (size_t i = 0; i < n; i++) {
            if (ev[i].t_ms > limit_ms) {
                played_ms = ev[i].t_ms;
                goto done;
            }
            int k = push_event(ev[i].t_ms, ev[i].reg, ev[i].val);
            if (k) {
                sid_lsfp_stream_close(s);
                *key = k;
                s_interrupt = k;
                return k == 'q' ? -1 : 1;
            }
        }
        rpfm_sid_flush();

        played_ms += frame_ms;

        if (played_ms >= limit_ms) break;

        /* Generating this frame already throttled against LEAD_US, so reaching
         * here means the emulator is keeping up with real time. */
        if (played_ms >= next_report) {
            next_report += 5000.0;
            printf("\r  %4.0f s   ", played_ms / 1000.0);
            fflush(stdout);
            int k = kb_take();
            if (k) {
                sid_lsfp_stream_close(s);
                *key = k;
                s_interrupt = k;
                return k == 'q' ? -1 : 1;
            }
        }
    }

done:
    rpfm_sid_flush();
    printf("\r  %4.0f s  stream end      \n", played_ms / 1000.0);
    drain_tail();
    sid_lsfp_stream_close(s);
    return 0;
}

/* Same, through the built-in 6502. Kept because it is the copied path and it
 * has no libsidplayfp dependency; it cannot see intra-frame writes, so digi
 * tunes lose their drums here. */
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

    if (opt.dry_run) {
        sid_stream_close();
        printf("  dry-run: would stream one frame every %.2f ms\n", frame_ms);
        return 0;
    }

    sched_reset();

    const sid_event_t *ev = NULL;
    size_t n = 0;
    double played_ms = 0.0;
    double next_report = 5000.0;
    double limit_ms = opt.seconds > 0.0 ? opt.seconds * 1000.0 : 1e18;

    for (;;) {
        if (sid_stream_next_frame(&ev, &n) != 0) break;

        for (size_t i = 0; i < n; i++) {
            if (ev[i].t_ms > limit_ms) { played_ms = ev[i].t_ms; goto done; }
            int k = push_event(ev[i].t_ms, ev[i].reg, ev[i].val);
            if (k) {
                sid_stream_close();
                *key = k;
                s_interrupt = k;
                return k == 'q' ? -1 : 1;
            }
        }
        rpfm_sid_flush();

        played_ms += frame_ms;
        if (played_ms >= limit_ms) break;

        if (played_ms >= next_report) {
            next_report += 5000.0;
            printf("\r  %4.0f s   ", played_ms / 1000.0);
            fflush(stdout);
            int k = kb_take();
            if (k) {
                sid_stream_close();
                *key = k;
                s_interrupt = k;
                return k == 'q' ? -1 : 1;
            }
        }
    }

done:
    rpfm_sid_flush();
    printf("\r  %4.0f s  stream end      \n", played_ms / 1000.0);
    drain_tail();
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

/* Everything this player can open, so a folder of any mix plays. */
static int is_playable_name(const char *name)
{
    return has_suffix(name, ".sid") || has_suffix(name, ".sidvgm") ||
           has_suffix(name, ".sidreg");
}

/* Play one file. Returns 0 when it finished, 1 if the user asked for the next
 * track, -1 to quit, 2 on a load error (skip it). */
static int play_file(const char *path, int *key)
{
    *key = 0;

    /* ---- a recorded stream ---- */
    if (sidvgm_is_file(path) || has_suffix(path, ".sidreg")) {
        sid_event_t *ev = NULL;
        size_t n = 0;
        sid_song_info_t info;
        memset(&info, 0, sizeof info);

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

        /* The header says which clock the tune was recorded for. */
        hw_set_clock(&info);

        int loops = (opt.loop == 0) ? 1 : opt.loop;
        for (int l = 0; loops < 0 || l < loops; l++) {
            if (loops != 1)
                printf("  pass %d%s\n", l + 1, loops < 0 ? " (forever)" : "");
            rc = replay_buffer(ev, n, key);
            if (rc != 0) break;
            if (loops < 0 || l + 1 < loops) {
                hw_mute();
                if (!opt.dry_run) Sleep(120);
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

    hw_set_clock(&probe);

    if (opt.seconds > 0.0) {
        /* Bounded: record the whole span up front and replay it. */
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
        rc = replay_buffer(ev, n, key);
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
        fprintf(stderr, "no playable files in '%s'\n", dir);
        free(files);
        return 1;
    }

    if (hw_connect() != 0) { free(files); return 1; }

    kb_start();

    printf("%d files in %s\n", nfiles, dir);
    printf("keys: [n] next  [b] back  [,] prev subsong  [.] next subsong  [q] quit\n\n");

    int idx = start_idx < 0 ? 0 : (start_idx >= nfiles ? nfiles - 1 : start_idx);

    /* The subsong follows the tune, not the playlist: after pressing '.' on a
     * 9-subsong file, the next tune must start at its own default again. */
    const int base_sub = (opt.subsong < 0) ? 0 : opt.subsong;
    int subsong = base_sub;

    while (idx >= 0 && idx < nfiles) {
        char path[768];
        snprintf(path, sizeof path, "%s/%s", dir, files[idx].name);

        sid_song_info_t probe;
        /* Only a .sid has a PSID/RSID header. Probing a .sidvgm would make
         * sid_song_probe print "not PSID/RSID" for every file in the folder. */
        if (has_suffix(path, ".sid") && sid_song_probe(path, &probe) == 0) {
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

        hw_clear_regs();

        int local = subsong;
        opt.subsong = local;
        int key = 0;
        play_file(path, &key);
        hw_mute();

        int action = s_interrupt ? s_interrupt : kb_take();
        if (action == 'q') break;

        switch (action) {
        case 'b':
            idx = idx > 0 ? idx - 1 : 0;
            subsong = base_sub;
            printf("\n[BACK]\n\n");
            break;
        case ',': subsong = subsong > 0 ? subsong - 1 : 0;
                  printf("\n[SUBSONG %d]\n\n", subsong); break;
        case '.': subsong++; printf("\n[SUBSONG %d]\n\n", subsong); break;
        default:
            idx++;
            subsong = base_sub;
            break;
        }
#ifdef _WIN32
        Sleep(200);
#else
        usleep(200000);
#endif
    }

    printf("\nplaylist end\n");
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
    printf("usage: rpfm_sid <file|directory> [options]\n\n");
    printf("  .sid      run the tune on libsidplayfp and push its writes\n");
    printf("  .sidvgm   replay a recorded register stream\n");
    printf("  .sidreg   replay the old text stream\n");
    printf("  <dir>     playlist: every playable file, sorted by name\n\n");
    printf("options:\n");
    printf("  --seconds N     play N seconds (default 0 = whole stream / for ever)\n");
    printf("  --subsong N     subsong, 0-based (default: the file's own)\n");
    printf("  --speed X       playback rate (default 1.0)\n");
    printf("  --loop N        repeat a buffered stream N times (0 = once, -1 = for ever)\n");
    printf("  --engine E      .sid recorder: lsfp (default) | native\n");
    printf("                  lsfp   = libsidplayfp: digi drums and the real\n");
    printf("                           CIA/VIC cadence; needed for digi + RSID\n");
    printf("                  native = built-in 6502 (pure C). No in-frame\n");
    printf("                           $D418, but plain PSID sounds fine\n");
    printf("  --dry-run       no hardware, just count the writes\n");
    printf("  --list          list the playable files in a directory\n");
    printf("  --help          this text\n\n");
    printf("while playing: [n] next  [b] back  [,] [.] subsong  [q] quit\n");
}

static int parse_args(int argc, char **argv, const char **path, int *list_only,
                      int *from_idx)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--seconds") && i + 1 < argc)      opt.seconds = atof(argv[++i]);
        else if (!strcmp(a, "--speed") && i + 1 < argc)   opt.speed = atof(argv[++i]);
        else if (!strcmp(a, "--subsong") && i + 1 < argc) opt.subsong = atoi(argv[++i]);
        else if (!strcmp(a, "--loop") && i + 1 < argc)    opt.loop = atoi(argv[++i]);
        else if (!strcmp(a, "--from") && i + 1 < argc)    *from_idx = atoi(argv[++i]);
        else if (!strcmp(a, "--engine") && i + 1 < argc) {
            const char *e = argv[++i];
            if (!strcmp(e, "lsfp") || !strcmp(e, "libsidplayfp")) opt.use_lsfp = 1;
            else if (!strcmp(e, "native")) opt.use_lsfp = 0;
            else { fprintf(stderr, "--engine must be lsfp or native\n"); return -1; }
        }
        else if (!strcmp(a, "--dry-run")) opt.dry_run = 1;
        else if (!strcmp(a, "--list"))    *list_only = 1;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); exit(0); }
        else if (a[0] == '-') {
            fprintf(stderr, "unknown option '%s'\n", a);
            return -1;
        }
        else if (!*path) *path = a;
        else { fprintf(stderr, "unexpected argument '%s'\n", a); return -1; }
    }

    if (opt.speed <= 0.0) opt.speed = 1.0;
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
                fprintf(stderr, "no playable files in %s\n", path);
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

    /* A new tune must not inherit the previous one's gates or the board's
     * queue; CMD_RESET pulses IC# and clears both on the board side. */
    if (!opt.dry_run) {
        rpfm_link_reset();
#ifdef _WIN32
        Sleep(150);
#else
        usleep(150000);
#endif
    }
    hw_clear_regs();
    kb_start();

    printf("transport: RPFM HID   board deadline   recorder: %s\n",
           opt.use_lsfp ? "libsidplayfp" : "built-in 6502");

    /* One file is still a playlist of one: ',' and '.' must walk its subsongs
     * instead of doing nothing. */
    int rc = 0;
    if (!has_suffix(path, ".sidvgm") && !has_suffix(path, ".sidreg")) {
        sid_song_info_t probe;
        int nsongs = 1, cur = opt.subsong;
        if (sid_song_probe(path, &probe) == 0) {
            nsongs = probe.n_songs ? probe.n_songs : 1;
            if (cur < 0) cur = probe.start_song ? probe.start_song - 1 : 0;
        } else if (cur < 0) {
            cur = 0;
        }
        if (cur >= nsongs) cur = nsongs - 1;

        for (;;) {
            s_action = 0;
            s_interrupt = 0;

            hw_clear_regs();

            opt.subsong = cur;
            int key = 0;
            rc = play_file(path, &key);
            hw_mute();
            if (rc == 2) break;

            int action = s_interrupt ? s_interrupt : kb_take();
            if (action == 'q') break;

            if (action == '.') {
                if (cur + 1 < nsongs) {
                    cur++;
                    printf("\n[SUBSONG %d/%d]\n\n", cur, nsongs);
                } else {
                    printf("\n[SUBSONG %d/%d]  last subsong\n\n", cur, nsongs);
                }
                continue;
            }
            if (action == ',') {
                if (cur > 0) {
                    cur--;
                    printf("\n[SUBSONG %d/%d]\n\n", cur, nsongs);
                } else {
                    printf("\n[SUBSONG 0/%d]  first subsong\n\n", nsongs);
                }
                continue;
            }
            break;
        }
    } else {
        int key = 0;
        rc = play_file(path, &key);
    }

    hw_mute();
    hw_disconnect();
    return (rc == 2) ? 1 : 0;
}
