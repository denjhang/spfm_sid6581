/*
 * sidvgmplay.c - play .sidvgm / .sidreg / .sid through a software SID model.
 *
 * No hardware. A pre-rendered stream (sidvgm or sidreg) is already a register
 * stream, so this only has to (a) walk it against a clock and (b) turn writes
 * into samples. A .sid is auto-converted first: the 6502 runs once, the writes
 * are recorded, and the result is cached as a .sidvgm so the next play of the
 * same file is instant.
 *
 * The playback loop is the shape libvgm's DirectSound backend uses: render a
 * buffer, hand it to the OS, and let the device's own fill pointer decide when
 * more is needed. Two sinks are offered:
 *
 *   --wav out.wav   render offline as fast as possible (use this to check a
 *                   recording by ear without waiting real time)
 *   default         real-time through waveOut, buffer-queue driven
 *
 * Playlist: pass several files, or a directory, and the loop plays them in
 * order. [n] skips to the next track, [q] quits.
 *
 * The stream is walked by absolute tick, not by sleeping per event: each render
 * block consumes exactly `block` samples worth of ticks, so a slow block makes
 * the notes bunch up rather than drift.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "sid_rsid.h"
#include "sidvgm.h"
#include "sidmodel.h"

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#include <conio.h>
#include <direct.h>
#include <io.h>
#include <process.h>
#endif

#define OUT_RATE      44100
#define BLOCK_FRAMES  1024
#define WAVE_BUFFERS  4

/* Duration used when a .sid has to be converted and the caller did not say how
 * long to record. .sid has no length field, so something has to be picked. */
#define SID_DEFAULT_SECONDS 180.0

typedef struct {
    const sid_event_t *ev;
    size_t n;
    size_t pos;
    long long tick;          /* current position in 1/44100 s */
} stream_cursor_t;

static void cursor_seek(stream_cursor_t *c, long long tick)
{
    if (tick <= c->tick) {
        c->pos = 0;
        c->tick = 0;
    }
    while (c->pos < c->n) {
        long long t = (long long)llround(c->ev[c->pos].t_ms * 44.1);
        if (t > tick) break;
        c->pos++;
    }
    c->tick = tick;
}

/* Render `frames` samples, applying every event whose tick falls in the block. */
static void render_block(sid_model_t *m, stream_cursor_t *c,
                         short *buf, size_t frames)
{
    size_t done = 0;
    while (done < frames) {
        size_t chunk = frames - done;
        if (c->pos < c->n) {
            long long t = (long long)llround(c->ev[c->pos].t_ms * 44.1);
            long long delta = t - c->tick;
            if (delta <= 0) {
                sid_model_write(m, c->ev[c->pos].reg, c->ev[c->pos].val);
                c->pos++;
                continue;
            }
            if (delta < (long long)chunk) chunk = (size_t)delta;
        }
        sid_model_render(m, buf + done, chunk);
        done += chunk;
        c->tick += (long long)chunk;
    }
}

/* ------------------------------------------------------------------ */
/* keyboard                                                            */
/* ------------------------------------------------------------------ */

/* Same shape as the hardware player's reader: a blocking thread, not a poll,
 * so a key lands immediately even while a track is mid-buffer. */
static volatile int s_action = 0;

#ifdef _WIN32
static int kb_read_blocking(void)
{
    if (!_isatty(_fileno(stdin))) {
        unsigned char c;
        if (_read(0, &c, 1) != 1) return -1;
        return c;
    }
    int c = _getch();
    if (c == 0 || c == 0xE0) { _getch(); return 0; }
    return c;
}
#else
#include <unistd.h>
static int kb_read_blocking(void)
{
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;
    return c;
}
#endif

static void kb_thread_body(void)
{
    for (;;) {
        int c = kb_read_blocking();
        if (c < 0) break;
        switch (c) {
        case 'q': case 'Q': case 27: s_action = 'q'; break;
        case 'n': case 'N':          s_action = 'n'; break;
        case 'b': case 'B':          s_action = 'b'; break;
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

static void kb_start(void)
{
    if (!_isatty(_fileno(stdin)) && !getenv("SIDPLAY_KEYS")) return;
    uintptr_t h = _beginthreadex(NULL, 0, kb_thread, NULL, 0, NULL);
    if (h) CloseHandle((HANDLE)h);
}
#else
#include <pthread.h>
static void *kb_thread(void *arg)
{
    (void)arg;
    kb_thread_body();
    return NULL;
}

static void kb_start(void)
{
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
}
#endif

static int kb_take(void)
{
    int a = s_action;
    if (a) s_action = 0;
    return a;
}

/* ------------------------------------------------------------------ */
/* song loading                                                        */
/* ------------------------------------------------------------------ */

typedef struct song_s {
    sid_event_t   *ev;
    size_t         n;
    sid_song_info_t info;
    int            owns;      /* ev must be released with sid_song_free */
    char           title[64];
} song_t;

static int has_suffix(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    if (a < b) return 0;
    s += a - b;
    for (size_t i = 0; i < b; i++)
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)suf[i])) return 0;
    return 1;
}

static const char *basename_of(const char *p)
{
    const char *b = p;
    for (const char *q = p; *q; q++)
        if (*q == '/' || *q == '\\') b = q + 1;
    return b;
}

/* Cache name for a converted .sid: <dir>/<stem>_s<sub>_d<sec>.sidvgm */
static void convert_cache_name(char *buf, size_t len, const char *cache_dir,
                               const char *sid_path, int subsong, double seconds)
{
    const char *base = basename_of(sid_path);
    char stem[260];
    snprintf(stem, sizeof stem, "%s", base);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;
    snprintf(buf, len, "%s%s_s%d_d%ld.sidvgm", cache_dir, stem,
             subsong < 0 ? 0 : subsong, (long)(seconds + 0.5));
}

/* Run the 6502 and record. On success the events are owned by the caller. */
static int convert_sid(const char *path, int subsong, double seconds,
                       int use_cache, song_t *out,
                       char *err, size_t errlen)
{
    char cache_dir[512] = "sid_cache/";
    char cache_path[768];
    convert_cache_name(cache_path, sizeof cache_path, cache_dir, path,
                       subsong, seconds);

    if (use_cache && sidvgm_is_file(cache_path)) {
        if (sidvgm_read(cache_path, &out->ev, &out->n, &out->info) == 0) {
            out->owns = 1;
            snprintf(out->title, sizeof out->title, "%.63s", out->info.name);
            printf("  cache: %s\n", cache_path);
            return 0;
        }
    }

    if (sid_song_extract(path, subsong, seconds, &out->ev, &out->n,
                         &out->info, err, errlen) != 0)
        return -1;
    out->owns = 1;
    snprintf(out->title, sizeof out->title, "%.63s",
             out->info.name[0] ? out->info.name : basename_of(path));

    if (use_cache && out->n) {
#ifdef _WIN32
        _mkdir("sid_cache");
#else
        mkdir("sid_cache", 0755);
#endif
        if (sidvgm_write(cache_path, out->ev, out->n, &out->info,
                         "converted by sidvgmplay") == 0)
            printf("  cached: %s\n", cache_path);
    }
    return 0;
}

static int load_song(const char *path, int subsong, double seconds,
                     int use_cache, song_t *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof *out);
    if (err && errlen) err[0] = 0;

    if (has_suffix(path, ".sid")) {
        return convert_sid(path, subsong, seconds, use_cache, out, err, errlen);
    }

    if (sidvgm_is_file(path)) {
        if (sidvgm_read(path, &out->ev, &out->n, &out->info) != 0) {
            if (err && errlen) snprintf(err, errlen, "not a readable sidvgm");
            return -1;
        }
        out->owns = 1;
        snprintf(out->title, sizeof out->title, "%.63s",
                 out->info.name[0] ? out->info.name : basename_of(path));
        return 0;
    }

    /* .sidreg: the reader hands back a shared buffer, so do not free it. */
    if (sid_sidreg_read(path, &out->ev, &out->n) != 0) {
        if (err && errlen) snprintf(err, errlen, "unknown container");
        return -1;
    }
    out->owns = 0;
    out->info.pal = 1;
    out->info.clock_hz = 985248.0;
    out->info.vsync_hz = 50.125;
    out->info.n_songs = 1;
    snprintf(out->title, sizeof out->title, "%.63s", basename_of(path));
    return 0;
}

static void free_song(song_t *s)
{
    if (s->owns && s->ev) sid_song_free(s->ev);
    s->ev = NULL;
    s->n = 0;
}

/* ------------------------------------------------------------------ */
/* sinks                                                               */
/* ------------------------------------------------------------------ */

static int write_wav_header(FILE *f, unsigned rate, unsigned long frames)
{
    unsigned long data = frames * 2;
    unsigned long riff = 36 + data;
    unsigned char h[44];
    memset(h, 0, sizeof h);
    memcpy(h, "RIFF", 4);
    h[4] = (unsigned char)(riff & 0xFF); h[5] = (unsigned char)((riff >> 8) & 0xFF);
    h[6] = (unsigned char)((riff >> 16) & 0xFF); h[7] = (unsigned char)((riff >> 24) & 0xFF);
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16;
    h[20] = 1;                       /* PCM */
    h[22] = 1;                       /* mono */
    h[24] = (unsigned char)(rate & 0xFF);
    h[25] = (unsigned char)((rate >> 8) & 0xFF);
    h[26] = (unsigned char)((rate >> 16) & 0xFF);
    h[27] = (unsigned char)((rate >> 24) & 0xFF);
    unsigned long byte_rate = rate * 2;
    h[28] = (unsigned char)(byte_rate & 0xFF);
    h[29] = (unsigned char)((byte_rate >> 8) & 0xFF);
    h[30] = (unsigned char)((byte_rate >> 16) & 0xFF);
    h[31] = (unsigned char)((byte_rate >> 24) & 0xFF);
    h[32] = 2;                       /* block align */
    h[34] = 16;                      /* bits */
    memcpy(h + 36, "data", 4);
    h[40] = (unsigned char)(data & 0xFF); h[41] = (unsigned char)((data >> 8) & 0xFF);
    h[42] = (unsigned char)((data >> 16) & 0xFF); h[43] = (unsigned char)((data >> 24) & 0xFF);
    return fwrite(h, 1, 44, f) == 44 ? 0 : -1;
}

static int render_to_wav(sid_model_t *m, stream_cursor_t *c, const char *path,
                         long long total_ticks)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return -1; }

    unsigned long frames = (unsigned long)((total_ticks * OUT_RATE) / 44100);
    if (write_wav_header(f, OUT_RATE, frames) != 0) { fclose(f); return -1; }

    short buf[BLOCK_FRAMES];
    unsigned long written = 0;
    while (written < frames) {
        size_t want = frames - written;
        if (want > BLOCK_FRAMES) want = BLOCK_FRAMES;
        render_block(m, c, buf, want);
        if (fwrite(buf, 2, want, f) != want) { fclose(f); return -1; }
        written += (unsigned long)want;
    }
    fclose(f);
    printf("  -> %s (%lu frames, %.1f s)\n", path, frames,
           (double)frames / OUT_RATE);
    return 0;
}

#ifdef _WIN32
/* Real-time sink. Same contract as libvgm's DSound thread: keep the device
 * queue fed, and let WAVEHDR.dwFlags (the equivalent of the DirectSound fill
 * pointer) say when a buffer came back. Returns 'q' if the user asked to quit,
 * 'n' for next track, 0 when the stream ran out. */
static int play_realtime(sid_model_t *m, stream_cursor_t *c, long long total_ticks)
{
    HWAVEOUT hwo = NULL;
    WAVEFORMATEX wfx;
    memset(&wfx, 0, sizeof wfx);
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = OUT_RATE;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 2;
    wfx.nAvgBytesPerSec = OUT_RATE * 2;

    if (waveOutOpen(&hwo, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        fprintf(stderr, "waveOutOpen failed; use --wav to render offline\n");
        return 'q';
    }

    short *bufs[WAVE_BUFFERS];
    WAVEHDR hdrs[WAVE_BUFFERS];
    memset(hdrs, 0, sizeof hdrs);
    for (int i = 0; i < WAVE_BUFFERS; i++) {
        bufs[i] = (short *)calloc(BLOCK_FRAMES, sizeof(short));
        hdrs[i].lpData = (LPSTR)bufs[i];
        hdrs[i].dwBufferLength = 0;
        hdrs[i].dwFlags = WHDR_DONE;
    }

    long long rendered = 0;
    int action = 0;
    while (rendered < total_ticks && !action) {
        for (int i = 0; i < WAVE_BUFFERS; i++) {
            if (!(hdrs[i].dwFlags & WHDR_DONE)) continue;
            if (rendered >= total_ticks) break;

            if (hdrs[i].dwFlags & WHDR_PREPARED)
                waveOutUnprepareHeader(hwo, &hdrs[i], sizeof(WAVEHDR));

            size_t want = BLOCK_FRAMES;
            long long left = total_ticks - rendered;
            if ((long long)want > left) want = (size_t)left;

            render_block(m, c, bufs[i], want);
            rendered += (long long)want;

            hdrs[i].dwBufferLength = (DWORD)(want * 2);
            hdrs[i].dwFlags = 0;
            waveOutPrepareHeader(hwo, &hdrs[i], sizeof(WAVEHDR));
            waveOutWrite(hwo, &hdrs[i], sizeof(WAVEHDR));
        }
        Sleep(10);
        int k = kb_take();
        if (k == 'q' || k == 'n' || k == 'b') action = k;
    }

    /* Drain, unless the user is skipping: then cut the tail immediately. */
    for (int i = 0; i < WAVE_BUFFERS; i++) {
        if (!action) {
            while (!(hdrs[i].dwFlags & WHDR_DONE)) Sleep(10);
        }
        if (hdrs[i].dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(hwo, &hdrs[i], sizeof(WAVEHDR));
        free(bufs[i]);
    }
    waveOutClose(hwo);
    return action;
}
#endif

/* ------------------------------------------------------------------ */
/* playlist                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    char **v;
    int n, cap;
} list_t;

static int list_add(list_t *l, const char *s)
{
    if (l->n == l->cap) {
        int want = l->cap ? l->cap * 2 : 32;
        char **nv = (char **)realloc(l->v, (size_t)want * sizeof(char *));
        if (!nv) return -1;
        l->v = nv;
        l->cap = want;
    }
    size_t len = strlen(s) + 1;
    l->v[l->n] = (char *)malloc(len);
    if (!l->v[l->n]) return -1;
    memcpy(l->v[l->n], s, len);
    l->n++;
    return 0;
}

static void list_free(list_t *l)
{
    for (int i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    l->v = NULL;
    l->n = l->cap = 0;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int is_playable(const char *name)
{
    return has_suffix(name, ".sid") || has_suffix(name, ".sidvgm") ||
           has_suffix(name, ".sidreg");
}

#ifdef _WIN32
static int collect_path(const char *path, list_t *l)
{
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "no such file or directory: %s\n", path);
        return -1;
    }
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) return list_add(l, path);

    /* Directory: take every playable file, sorted, like `sidlist`. */
    char pattern[1024];
    snprintf(pattern, sizeof pattern, "%s/*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    list_t sub = { 0 };
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!is_playable(fd.cFileName)) continue;
        char full[1024];
        size_t plen = strlen(path);
        int sep = (plen && (path[plen - 1] == '/' || path[plen - 1] == '\\'));
        snprintf(full, sizeof full, "%s%s%s", path, sep ? "" : "/", fd.cFileName);
        list_add(&sub, full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    if (sub.n > 1) qsort(sub.v, (size_t)sub.n, sizeof(char *), cmp_str);
    for (int i = 0; i < sub.n; i++) list_add(l, sub.v[i]);
    list_free(&sub);
    return 0;
}
#else
#include <dirent.h>
#include <sys/stat.h>
static int collect_path(const char *path, list_t *l)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "no such file or directory: %s\n", path);
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) return list_add(l, path);

    DIR *d = opendir(path);
    if (!d) return 0;
    list_t sub = { 0 };
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (!is_playable(e->d_name)) continue;
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        list_add(&sub, full);
    }
    closedir(d);
    if (sub.n > 1) qsort(sub.v, (size_t)sub.n, sizeof(char *), cmp_str);
    for (int i = 0; i < sub.n; i++) list_add(l, sub.v[i]);
    list_free(&sub);
    return 0;
}
#endif

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char *a0)
{
    fprintf(stderr,
        "usage: %s <file|dir> [more files or dirs...] [options]\n"
        "  .sidvgm / .sidreg play as-is; .sid is converted first (cached)\n"
        "\n"
        "  --wav FILE       render to 16-bit mono WAV instead of playing\n"
        "                   (with several inputs: FILE_1.wav, FILE_2.wav, ...)\n"
        "  --seconds N      length per track (default: whole stream; for a .sid\n"
        "                   conversion the default is %.0f s)\n"
        "  --start N        start at N seconds\n"
        "  --subsong N      subsong for .sid conversion, 0-based\n"
        "  --8580           use the 8580 filter curve (default 6581)\n"
        "  --no-filter      bypass the filter\n"
        "  --no-cache       do not read or write sid_cache/\n"
        "  --info           print each stream header and exit\n"
        "\n"
        "keys while playing: [n] next  [b] back  [q] quit\n",
        a0, SID_DEFAULT_SECONDS);
}

int main(int argc, char **argv)
{
    list_t inputs = { 0 };
    const char *wav_path = NULL;
    double seconds = 0.0, start = 0.0;
    int subsong = -1;
    int use8580 = 0, no_filter = 0, info_only = 0, use_cache = 1;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--wav") && i + 1 < argc)            wav_path = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)   seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--start") && i + 1 < argc)     start = atof(argv[++i]);
        else if (!strcmp(argv[i], "--subsong") && i + 1 < argc)   subsong = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--8580"))                      use8580 = 1;
        else if (!strcmp(argv[i], "--no-filter"))                 no_filter = 1;
        else if (!strcmp(argv[i], "--no-cache"))                  use_cache = 0;
        else if (!strcmp(argv[i], "--info"))                      info_only = 1;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(argv[0]); return 0; }
        else if (argv[i][0] == '-') { fprintf(stderr, "unknown option '%s'\n", argv[i]); return 1; }
        else if (collect_path(argv[i], &inputs) != 0) { list_free(&inputs); return 1; }
    }

    if (inputs.n == 0) { usage(argv[0]); return 1; }

    /* One input file: behave exactly like the single-file player, including
     * "no --seconds means play the whole stream". A directory or several
     * inputs is a playlist, which gets the same rule per track. */
    if (!wav_path && !info_only) {
        kb_start();
        printf("%d track%s\n[n] next   [b] back   [q] quit\n\n",
               inputs.n, inputs.n == 1 ? "" : "s");
    }

    int idx = 0;
    double cur_seconds = seconds;
    int action = 0;

    while (idx >= 0 && idx < inputs.n) {
        const char *path = inputs.v[idx];

        /* A .sid has to be recorded before it can be played; give the
         * recorder a finite length even when the user asked for "whole". */
        double conv_seconds = seconds > 0.0 ? seconds : SID_DEFAULT_SECONDS;
        char err[160];
        song_t song;
        printf("[%d/%d] %s\n", idx + 1, inputs.n, path);
        if (load_song(path, subsong, conv_seconds, use_cache, &song,
                      err, sizeof err) != 0) {
            fprintf(stderr, "  %s\n", err[0] ? err : "cannot load");
            idx++;
            continue;
        }

        printf("  title  : %s\n", song.title[0] ? song.title : "(none)");
        if (song.info.author[0]) printf("  author : %s\n", song.info.author);
        printf("  clock  : %s, %.0f Hz\n",
               song.info.pal ? "PAL" : "NTSC", song.info.clock_hz);
        printf("  events : %lu, length %.2f s\n", (unsigned long)song.n,
               song.n ? song.ev[song.n - 1].t_ms / 1000.0 : 0.0);

        if (info_only) {
            free_song(&song);
            idx++;
            continue;
        }
        if (song.n == 0) {
            fprintf(stderr, "  stream has no register writes\n");
            free_song(&song);
            idx++;
            continue;
        }

        /* --seconds is authoritative in both directions: it caps a long
         * stream and extends a short one (a rendered stream ends at its last
         * write, so a note's release tail would otherwise be cut). */
        long long total_ticks = (long long)llround(song.ev[song.n - 1].t_ms * 44.1);
        if (cur_seconds > 0.0)
            total_ticks = (long long)llround(cur_seconds * 44100.0);
        long long start_tick = (long long)llround(start * 44100.0);
        if (start_tick < 0) start_tick = 0;
        if (start_tick > total_ticks) start_tick = total_ticks;

        stream_cursor_t cur;
        memset(&cur, 0, sizeof cur);
        cur.ev = song.ev;
        cur.n = song.n;
        cursor_seek(&cur, start_tick);

        sid_model_t *m = sid_model_new(song.info.clock_hz, use8580, OUT_RATE);
        if (!m) { free_song(&song); list_free(&inputs); return 1; }
        sid_model_set_filter(m, no_filter ? 0 : 1);
        sid_model_reset(m);

        /* Fast-forward: apply every write at or before the start point so the
         * model begins in the right state. */
        for (size_t i = 0; i < cur.pos; i++)
            sid_model_write(m, song.ev[i].reg, song.ev[i].val);

        long long span = total_ticks - start_tick;
        action = 0;

        if (wav_path) {
            char out[1024];
            if (inputs.n > 1) {
                const char *dot = strrchr(wav_path, '.');
                if (dot) {
                    int stem = (int)(dot - wav_path);
                    snprintf(out, sizeof out, "%.*s_%d%s", stem, wav_path,
                             idx + 1, dot);
                } else {
                    snprintf(out, sizeof out, "%s_%d", wav_path, idx + 1);
                }
            } else {
                snprintf(out, sizeof out, "%s", wav_path);
            }
            render_to_wav(m, &cur, out, span);
            idx++;
        } else {
#ifdef _WIN32
            action = play_realtime(m, &cur, span);
            if (action == 'q') idx = inputs.n;
            else if (action == 'b') idx = idx > 0 ? idx - 1 : 0;
            else idx++;
#else
            fprintf(stderr, "real-time output is Windows-only; use --wav\n");
            sid_model_free(m);
            free_song(&song);
            break;
#endif
        }

        sid_model_free(m);
        free_song(&song);

        if (!wav_path && !info_only) {
            int k = kb_take();
            if (k == 'q') break;
            if (k == 'b') idx = idx > 0 ? idx - 1 : 0;
        }
    }

    list_free(&inputs);
    return 0;
}
