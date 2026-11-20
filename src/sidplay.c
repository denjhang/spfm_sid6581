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

#ifndef _WIN32
#define _mkdir(p) mkdir((p), 0755)
#endif

/* ------------------------------------------------------------------ */
/* shared state                                                        */
/* ------------------------------------------------------------------ */

static sidplay_opts_t s_opt;

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

/* A dedicated blocking read thread, not a poll.
 *
 * The first version called _kbhit() between events, which means a key only
 * registered while the player happened to be waiting for the next note, and
 * never at all while a track was being flushed or the pause between tracks
 * ran. Keys are supposed to interrupt immediately, so the reader blocks in
 * _getch() and just records the latest request; the replay loop checks that
 * record at every event and drops whatever it is doing. */

static volatile int s_action = 0;      /* 'q','n','b',',','.' or 0 */

static int kb_read_blocking(void)
{
#ifdef _WIN32
    /* _getch() talks to the console directly (CONIN$), so it ignores redirected
     * stdin and returns EOF the moment there is no console. Read fd 0 instead
     * when stdin is not a terminal, which is what makes piped input - and the
     * SIDPLAY_KEYS test path - actually reach the reader. */
    if (!_isatty(_fileno(stdin))) {
        unsigned char c;
        if (_read(0, &c, 1) != 1) return -1;
        return c;
    }
    int c = _getch();
    /* Arrow and function keys arrive as two bytes: 0x00 or 0xE0 then a code.
     * Swallow the pair so it cannot be mistaken for ',' or '.'. */
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
        if (c < 0) break;              /* stdin closed, nothing more to read */
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
    /* No console at all (output redirected into a file, run from a scheduler):
     * _getch() would fail immediately and the reader would spin. SIDPLAY_KEYS=1
     * forces the reader on anyway, which is how the interrupt path is tested
     * with piped input. */
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

/* Take the pending request, if any. Consuming it is what lets the caller tell
 * "the user pressed something this track" from "nothing happened". */
static int kb_take(void)
{
    int a = s_action;
    if (a) s_action = 0;
    return a;
}

/* The key that interrupted the last replay. replay() consumes the request to
 * decide it should stop, but the playlist still needs to know *which* key it
 * was to pick the next track, so the choice is kept here. */
static int s_interrupt = 0;

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

/* Play a .sid forever, running the 6502 live.
 *
 * A .sid has no end: the play routine is meant to be called indefinitely and
 * loops inside itself. Rendering a fixed buffer therefore imposes an arbitrary
 * duration, which is what the earlier version did (and why tracks stopped at
 * 60 seconds). Here the emulator is advanced one frame at a time, and each
 * frame's writes are emitted at the times they carry.
 *
 * Cost: one play() call is tens of microseconds against the ~20 ms the
 * previous frame is playing, so the emulation hides inside the playback and
 * there is no audible stall. The frame is emitted as soon as it is ready, and
 * the wait for the next frame is the only place the process blocks.
 *
 * Returns 0 if the user asked for the next track, -1 if they asked to quit. */
static int stream_play_sid(const char *path, int subsong, double speed,
                           sid_song_info_t *out_info)
{
    char err[128];
    sid_song_info_t info;
    if (sid_stream_open(path, subsong, &info, err, sizeof err) != 0) {
        fprintf(stderr, "  %s\n", err);
        return -1;
    }
    if (out_info) *out_info = info;

    const double frame_ms = sid_stream_frame_ms();
    const double us_per_ms = 1000.0 / speed;
    const int64_t start = spfm_now_us();

    /* The init writes are the first frame; they have t_ms == 0 and must all
     * land before the first play frame, so they are emitted without waiting. */
    const sid_event_t *ev = NULL;
    size_t n = 0;
    if (sid_stream_next_frame(&ev, &n) != 0) {
        sid_stream_close();
        return -1;
    }
    sid_set_deferred_flush(true);
    for (size_t i = 0; i < n; i++) emit(&ev[i]);

    int64_t worst_lag_us = 0;
    double played_ms = 0.0;
    int64_t next_report = 1000;      /* ms of music between progress updates */

    for (;;) {
        if (sid_stream_next_frame(&ev, &n) != 0) break;

        /* Emit this frame's writes, waiting for each one's due time. The
         * frame's timestamps are absolute from the song start, so they stay
         * correct no matter how many frames have gone by. */
        for (size_t i = 0; i < n; i++) {
            int64_t due = start + (int64_t)(ev[i].t_ms * us_per_ms);
            if (due > spfm_now_us()) {
                sid_flush();
                while (due > spfm_now_us()) {
                    int k = kb_take();
                    if (k) {
                        s_interrupt = k;
                        sid_set_deferred_flush(false);
                        sid_flush();
                        sid_stream_close();
                        return k == 'q' ? -1 : 0;
                    }
                    int64_t left = due - spfm_now_us();
                    spfm_wait_until_us(spfm_now_us() + (left > 5000 ? 5000 : left));
                }
            } else {
                int64_t lag = spfm_now_us() - due;
                if (lag > worst_lag_us) worst_lag_us = lag;
            }
            emit(&ev[i]);
        }

        /* Interrupt check between frames as well: a sparse song may have no
         * event to wait on for hundreds of milliseconds. */
        int k = kb_take();
        if (k) {
            s_interrupt = k;
            sid_set_deferred_flush(false);
            sid_flush();
            sid_stream_close();
            return k == 'q' ? -1 : 0;
        }

        played_ms += frame_ms;

        if (played_ms >= next_report) {
            next_report = (int64_t)(played_ms / 5000 + 1) * 5000;
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

/* Replay a pre-rendered `.sidreg` buffer. Returns 0 at the end of the buffer,
 * -1 if the user interrupted. */
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
            /* Interruptible wait: never sleep longer than a few ms, so a key
             * press takes effect straight away instead of at the next note. */
            while (due > spfm_now_us()) {
                int k = kb_take();
                if (k) {
                    s_interrupt = k;
                    sid_set_deferred_flush(false);
                    sid_flush();
                    return -1;
                }
                int64_t left = due - spfm_now_us();
                spfm_wait_until_us(spfm_now_us() + (left > 5000 ? 5000 : left));
            }
        } else {
            int64_t lag = spfm_now_us() - due;
            if (lag > worst_lag_us) worst_lag_us = lag;
        }

        emit(&ev[i]);

        /* Dense passages have no gap long enough to land in, so check here as
         * well: one compare per event is nothing next to the USB write. */
        int k = kb_take();
        if (k) {
            s_interrupt = k;
            sid_set_deferred_flush(false);
            sid_flush();
            return -1;
        }

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
    double seconds = 0.0;          /* 0 = forever, the default */
    double speed = 1.0;
    int subsong = -1;
    int use_cache = 1;
    int pace = 0;

    /* PSID model unless asked otherwise: see sid_rsid.h. Digi tracks need
     * --rsid, and their write rate is beyond what the SPFM link can carry. */
    sid_set_rsid_mode(0);

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc)      seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--speed") && i + 1 < argc)   speed = atof(argv[++i]);
        else if (!strcmp(argv[i], "--subsong") && i + 1 < argc) subsong = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-cache"))                use_cache = 0;
        else if (!strcmp(argv[i], "--rsid"))                    sid_set_rsid_mode(1);
        else if (!strcmp(argv[i], "--cache-dir") && i + 1 < argc)
            sidplay_set_cache_dir(argv[++i]);
        else if (!strcmp(argv[i], "--raw-filter"))              s_keep_filter = 1;
        else if (!strcmp(argv[i], "--proto") && i + 1 < argc) {
            if (sidplay_set_proto(argv[++i]) != 0) {
                fprintf(stderr, "--proto must be 1step or 2step\n");
                return 1;
            }
        }
        else if (!strcmp(argv[i], "--pace"))                    pace = 1;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            return 1;
        } else path = argv[i];
    }

    if (!path) {
        fprintf(stderr, "usage: sidplay <file.sid|file.sidreg> [options]\n"
                        "  --proto 1step|2step   write protocol (default 1step)\n"
                        "  --rsid                run the machine with clocks and "
                        "interrupts (digi drums; heavy)\n"
                        "  --seconds N  --subsong N  --speed X  --no-cache\n");
        return 1;
    }
    if (speed <= 0) speed = 1.0;

    sid_song_info_t info;
    memset(&info, 0, sizeof info);

    /* A pre-rendered .sidreg has a fixed length by construction - it is a
     * dump, not a song - so that path still replays the buffer once. A real
     * .sid runs live and forever. */
    if (is_sidreg(path)) {
        stream_t st;
        char err[128];
        if (load_stream(path, subsong, seconds > 0 ? seconds : 1e9,
                        use_cache, &st, err, sizeof err) != 0) {
            fprintf(stderr, "%s\n", err);
            return 1;
        }
        if (st.count == 0) {
            fprintf(stderr, "no register writes in %s\n", path);
            stream_free(&st);
            return 1;
        }
        printf("stream: %s\n%lu writes over %.1f s\n", st.info.name,
               (unsigned long)st.count, st.events[st.count - 1].t_ms / 1000.0);
        fflush(stdout);
        if (hw_connect() != 0) { stream_free(&st); return 1; }
        sid_clear_all_regs(s_opt.slot);
        sid_flush();
        kb_start();
        int rc = replay(st.events, st.count, speed, pace);
        hw_mute();
        hw_disconnect();
        stream_free(&st);
        return rc == 0 ? 0 : 1;
    }

    sid_song_info_t probe;
    if (sid_song_probe(path, &probe) != 0) {
        fprintf(stderr, "cannot read header of %s\n", path);
        return 1;
    }
    if (subsong < 0) subsong = probe.start_song ? probe.start_song - 1 : 0;
    if (subsong >= (int)probe.n_songs) subsong = probe.n_songs - 1;

    printf("song: %s / %s\n", probe.name, probe.author);
    if (seconds > 0)
        printf("playing %.0f s (from the start)\n", seconds);
    else
        printf("playing until interrupted: [n] next-track marker, [q] quit\n");
    printf("subsong %d of %u, speed %.2fx, %s writes, %s model\n",
           subsong, probe.n_songs, speed, sidplay_proto_name(),
           sid_get_rsid_mode() ? "RSID" : "PSID");
    fflush(stdout);

    if (hw_connect() != 0) return 1;

    /* Start from a known state: clear every register, then let the song's own
     * init writes define the chip. */
    sid_clear_all_regs(s_opt.slot);
    sid_flush();
    kb_start();

    int rc;
    if (seconds > 0) {
        /* Bounded run: render just that much and replay it. Used for quick
         * checks and for `--seconds` given explicitly. */
        stream_t st;
        char err[128];
        if (load_stream(path, subsong, seconds, 0, &st, err, sizeof err) != 0) {
            fprintf(stderr, "%s\n", err);
            hw_disconnect();
            return 1;
        }
        rc = replay(st.events, st.count, speed, pace);
        stream_free(&st);
    } else {
        rc = stream_play_sid(path, subsong, speed, &info);
    }

    hw_mute();
    hw_disconnect();
    return (rc < 0) ? 1 : 0;
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
    double seconds = 0.0;          /* 0 = play each track forever */
    double speed = 1.0;
    int start_idx = 0;
    int subsong = 0;

    sid_set_rsid_mode(0);

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc)      seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--speed") && i + 1 < argc)   speed = atof(argv[++i]);
        else if (!strcmp(argv[i], "--subsong") && i + 1 < argc) subsong = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rsid"))                    sid_set_rsid_mode(1);
        else if (!strcmp(argv[i], "--from") && i + 1 < argc)    start_idx = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cache-dir") && i + 1 < argc)
            sidplay_set_cache_dir(argv[++i]);
        else if (!strcmp(argv[i], "--raw-filter"))              s_keep_filter = 1;
        else if (!strcmp(argv[i], "--proto") && i + 1 < argc) {
            if (sidplay_set_proto(argv[++i]) != 0) {
                fprintf(stderr, "--proto must be 1step or 2step\n");
                return 1;
            }
        }
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

    kb_start();

    printf("\n%d files in %s\n", nfiles, dir);
    printf("each track plays until you press a key.  "
           "[n] next   [b] back   [,] prev subsong   [.] next subsong   [q] quit\n");
    printf("write protocol: %s, %s model\n\n", sidplay_proto_name(),
           sid_get_rsid_mode() ? "RSID" : "PSID");

    int idx = start_idx < 0 ? 0 : (start_idx >= nfiles ? nfiles - 1 : start_idx);

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
        printf("        subsong %d, playing...\n", subsong);
        fflush(stdout);

        s_action = 0;
        s_interrupt = 0;

        sid_clear_all_regs(s_opt.slot);
        sid_flush();

        int rc;
        if (seconds > 0) {
            /* Bounded mode: still available for a quick audition. */
            stream_t st;
            char err[128];
            if (load_stream(path, subsong, seconds, 0, &st, err, sizeof err) != 0) {
                fprintf(stderr, "  skip: %s\n", err);
                idx++;
                continue;
            }
            rc = st.count ? replay(st.events, st.count, speed, 1) : 0;
            stream_free(&st);
        } else {
            /* Normal mode: run live, forever, until a key arrives. */
            rc = stream_play_sid(path, subsong, speed, NULL);
        }

        hw_mute();

        /* If playback was interrupted, that key decides what happens next;
         * otherwise no key was pressed at all, so advance. */
        int action = s_interrupt ? s_interrupt : kb_take();
        if (action == 'q') break;

        switch (action) {
        case 'b': idx = idx > 0 ? idx - 1 : 0; printf("\n[BACK]\n\n"); break;
        case ',': subsong = subsong > 0 ? subsong - 1 : 0; printf("\n[SUBSONG %d]\n\n", subsong); break;
        case '.': subsong++; printf("\n[SUBSONG %d]\n\n", subsong); break;
        default:  idx++; break;
        }
        (void)rc;
        spfm_delay_ms(200);
    }

    printf("\nplaylist end\n");
    hw_disconnect();
    free(files);
    return 0;
}

/* ------------------------------------------------------------------ */
/* write protocol                                                      */
/* ------------------------------------------------------------------ */

/* The two protocols that matter for the "is one step enough?" experiment.
 * Everything else in sid_write_mode_t is a probe for a specific failure
 * (split to separate the two phases, *-soft to isolate USB pacing); these two
 * are the honest candidates for production. */
/* Set when the caller named a protocol (or a write mode) on the command line.
 * Without it, apply_common() would keep overwriting an explicit choice with
 * the playback default. */
static int s_proto_explicit = 0;

int sidplay_set_proto(const char *name)
{
    if (!name) return -1;
    if (!strcmp(name, "2step") || !strcmp(name, "two-phase") ||
        !strcmp(name, "two_phase") || !strcmp(name, "2")) {
        sid_set_write_mode(SID_WRITE_TWO_PHASE);
        s_proto_explicit = 1;
        return 0;
    }
    if (!strcmp(name, "1step") || !strcmp(name, "path-a") ||
        !strcmp(name, "burst") || !strcmp(name, "1")) {
        sid_set_write_mode(SID_WRITE_BURST);
        s_proto_explicit = 1;
        return 0;
    }
    return -1;
}

void sidplay_set_proto_explicit(void)
{
    s_proto_explicit = 1;
}

/* Read back from the driver rather than remembering a name, so --write-mode
 * and --proto cannot disagree about what is actually on the bus. */
const char *sidplay_proto_name(void)
{
    switch (sid_get_write_mode()) {
    case SID_WRITE_BURST:          return "1step";
    case SID_WRITE_TWO_PHASE:      return "2step";
    case SID_WRITE_TWO_PHASE_NOP:  return "2step+nop";
    case SID_WRITE_PATH_A_SOFT:    return "1step+flush";
    case SID_WRITE_SPLIT:          return "split";
    default:                       return "?";
    }
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
    if (s_opt.write_mode > 0) {
        sid_set_write_mode((sid_write_mode_t)s_opt.write_mode);
        s_proto_explicit = 1;
    }
    /* Playback default: one Path A command per register write. Measured on
     * this board, 1step played several tunes that 2step mangled, which fits
     * the async model - the SID latches on /CS and R/W, so there is nothing
     * for a host-controlled gap to accomplish. 2step stays one flag away
     * because it is what the earlier wiring needed. */
    if (!s_proto_explicit)
        sid_set_write_mode(SID_WRITE_BURST);
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
