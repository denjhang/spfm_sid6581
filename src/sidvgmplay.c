/*
 * sidvgmplay.c - play a .sidvgm (or .sidreg) through a software SID model.
 *
 * No hardware, no 6502: the file already holds the register stream, so this
 * only has to (a) walk the stream against a clock and (b) turn register writes
 * into samples.
 *
 * The playback loop is the shape libvgm's DirectSound backend uses: render a
 * buffer, hand it to the OS, and let the device's own fill pointer decide when
 * more is needed. Two sinks are offered:
 *
 *   --wav out.wav   render offline as fast as possible (use this to check a
 *                   recording by ear without waiting real time)
 *   default         real-time through waveOut, buffer-queue driven
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
#endif

#define OUT_RATE      44100
#define BLOCK_FRAMES  1024
#define WAVE_BUFFERS  4

typedef struct {
    const sid_event_t *ev;
    size_t n;
    size_t pos;
    long long tick;          /* current position in 1/44100 s */
    long long total_ticks;   /* end of stream */
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

/* Render `frames` samples, applying every event whose tick falls in the block.
 * Returns the number of events consumed. */
static size_t render_block(sid_model_t *m, stream_cursor_t *c,
                          short *buf, size_t frames)
{
    size_t consumed = 0;
    size_t done = 0;

    while (done < frames) {
        size_t chunk = frames - done;
        long long until_next = -1;

        if (c->pos < c->n) {
            long long t = (long long)llround(c->ev[c->pos].t_ms * 44.1);
            long long delta = t - c->tick;
            if (delta <= 0) {
                sid_model_write(m, c->ev[c->pos].reg, c->ev[c->pos].val);
                c->pos++;
                consumed++;
                continue;
            }
            if (delta < (long long)chunk) chunk = (size_t)delta;
            until_next = delta;
        }

        sid_model_render(m, buf + done, chunk);
        done += chunk;
        c->tick += (long long)chunk;
        (void)until_next;
    }
    return consumed;
}

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

    /* Total frames is known up front, so the header can be written once. */
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
 * pointer) say when a buffer came back. */
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
        return -1;
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

    printf("  playing %.1f s at %d Hz - press Ctrl+C to stop\n",
           (double)total_ticks / 44100.0, OUT_RATE);

    long long rendered = 0;
    for (;;) {
        if (rendered >= total_ticks) break;
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
    }

    /* Drain: wait for the last queued buffers to be consumed. */
    for (int i = 0; i < WAVE_BUFFERS; i++) {
        while (!(hdrs[i].dwFlags & WHDR_DONE)) Sleep(10);
        if (hdrs[i].dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(hwo, &hdrs[i], sizeof(WAVEHDR));
        free(bufs[i]);
    }
    waveOutClose(hwo);
    return 0;
}
#endif

static void usage(const char *a0)
{
    fprintf(stderr,
        "usage: %s <file.sidvgm|file.sidreg> [options]\n"
        "  --wav FILE       render to a 16-bit mono WAV instead of playing\n"
        "  --seconds N      stop after N seconds (default: whole stream)\n"
        "  --start N        start at N seconds\n"
        "  --8580           use the 8580 filter curve (default 6581)\n"
        "  --no-filter      bypass the filter\n"
        "  --info           print the stream header and exit\n", a0);
}

int main(int argc, char **argv)
{
    const char *in_path = NULL;
    const char *wav_path = NULL;
    double seconds = 0.0, start = 0.0;
    int use8580 = 0, no_filter = 0, info_only = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--wav") && i + 1 < argc)            wav_path = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)   seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--start") && i + 1 < argc)     start = atof(argv[++i]);
        else if (!strcmp(argv[i], "--8580"))                      use8580 = 1;
        else if (!strcmp(argv[i], "--no-filter"))                 no_filter = 1;
        else if (!strcmp(argv[i], "--info"))                      info_only = 1;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(argv[0]); return 0; }
        else if (argv[i][0] == '-') { fprintf(stderr, "unknown option '%s'\n", argv[i]); return 1; }
        else if (!in_path) in_path = argv[i];
        else { fprintf(stderr, "unexpected argument '%s'\n", argv[i]); return 1; }
    }
    if (!in_path) { usage(argv[0]); return 1; }

    /* ---- load ---- */
    sid_event_t *ev = NULL;
    size_t n = 0;
    int owns_ev = 1;         /* sid_sidreg_read hands back a shared buffer */
    sid_song_info_t info;
    memset(&info, 0, sizeof info);

    if (sidvgm_is_file(in_path)) {
        if (sidvgm_read(in_path, &ev, &n, &info) != 0) {
            fprintf(stderr, "cannot read %s as sidvgm\n", in_path);
            return 1;
        }
    } else {
        if (sid_sidreg_read(in_path, &ev, &n) != 0) {
            fprintf(stderr, "cannot read %s\n", in_path);
            return 1;
        }
        owns_ev = 0;
        info.pal = 1;
        info.clock_hz = 985248.0;
        info.vsync_hz = 50.125;
        info.n_songs = 1;
        const char *base = in_path;
        for (const char *p = in_path; *p; p++)
            if (*p == '/' || *p == '\\') base = p + 1;
        snprintf(info.name, sizeof info.name, "%.32s", base);
    }

    printf("file      : %s\n", in_path);
    printf("title     : %s\n", info.name[0] ? info.name : "(none)");
    printf("author    : %s\n", info.author[0] ? info.author : "(none)");
    printf("clock     : %s, %.0f Hz\n", info.pal ? "PAL" : "NTSC", info.clock_hz);
    printf("events    : %lu\n", (unsigned long)n);
    if (n) printf("length    : %.2f s\n", ev[n - 1].t_ms / 1000.0);

    if (info_only) {
        sid_song_free(ev);
        return 0;
    }
    if (n == 0) {
        fprintf(stderr, "stream has no register writes\n");
        sid_song_free(ev);
        return 1;
    }

    /* ---- clock the stream ----
     * The end of a stream is its last write, not a duration: .sidreg has no
     * length column and a .sid has no length field, so the final note keeps
     * ringing after the final write. --seconds therefore also extends the
     * render past that write, which is what makes a short test file
     * auditionable. */
    long long total_ticks = (long long)llround(ev[n - 1].t_ms * 44.1);
    if (seconds > 0.0) {
        long long want = (long long)llround(seconds * 44100.0);
        if (want > total_ticks) total_ticks = want;
    }
    long long start_tick = (long long)llround(start * 44100.0);
    if (start_tick < 0) start_tick = 0;
    if (start_tick > total_ticks) start_tick = total_ticks;

    stream_cursor_t cur;
    memset(&cur, 0, sizeof cur);
    cur.ev = ev;
    cur.n = n;
    cursor_seek(&cur, start_tick);

    sid_model_t *m = sid_model_new(info.clock_hz, use8580, OUT_RATE);
    if (!m) { if (owns_ev) sid_song_free(ev); return 1; }
    sid_model_set_filter(m, no_filter ? 0 : 1);
    sid_model_reset(m);

    /* Fast-forward: apply every write at or before the start point so the
     * model begins in the right state. */
    for (size_t i = 0; i < cur.pos; i++)
        sid_model_write(m, ev[i].reg, ev[i].val);

    long long span = total_ticks - start_tick;
    int rc;
    if (wav_path) {
        rc = render_to_wav(m, &cur, wav_path, span);
    } else {
#ifdef _WIN32
        rc = play_realtime(m, &cur, span);
#else
        fprintf(stderr, "real-time output is Windows-only; use --wav\n");
        rc = -1;
#endif
    }

    sid_model_free(m);
    if (owns_ev) sid_song_free(ev);
    return rc == 0 ? 0 : 1;
}
