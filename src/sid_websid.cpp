/*
 * sid_websid.cpp - WebSid-backed register recorder and audio renderer.
 *
 * WebSid (third_party/websid) already contains a capture hook for exactly this
 * job: src/sid.cpp's SID::writeMem() calls
 *
 *     recordPokeSID(SYS_CYCLES(), reg, value);
 *
 * when compiled with -DRPI4. That is how the upstream Raspberry Pi driver
 * drives a real SID chip. Here the same callback collects the writes into a
 * timestamped stream instead.
 *
 * Because SYS_CYCLES() is the emulator's own cycle counter, the timestamps are
 * derived from cycle-exact CPU/VIC/CIA emulation rather than from our
 * hand-written 6502 (fake6502), whose cycle counts are approximate in several
 * addressing modes. That approximation is what made recorded .sidvgm files
 * drift against reference emulators.
 *
 * Two entry points, independent of each other:
 *   1. sid_websid_extract()  - run the song, return the register stream.
 *   2. sid_websid_render_*   - turn a register stream into samples.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sid_websid.h"

extern "C" {
#include "core.h"
#include "system.h"
#include "vic.h"
#include "memory.h"
}
#include "sid.h"
#include "loaders.h"
#include "filter.h"

/* The upstream RPi driver entry points (src/sidplayer.cpp). Declared here so we
 * do not have to link the driver itself, which drags in libc++ streams and the
 * whole file-loading UI. */
extern "C" {
uint32_t loadSidFile(uint32_t is_mus, void *in_buffer, uint32_t in_buf_size,
                     uint32_t sample_rate, char *filename, void *basic_ROM,
                     void *char_ROM, void *kernal_ROM);
uint32_t playTune(uint32_t selected_track, uint32_t trace_sid);
void     sidWriteMem(uint16_t addr, uint8_t value);
}

/* ---- capture ------------------------------------------------------- */

typedef struct {
    sid_event_t *ev;
    size_t       n, cap;
    int          enabled;
    int          oom;
    /* Everything the init routine writes belongs to t=0: the player has to
     * have the whole init state applied before the first play frame, the same
     * convention sid_rsid.c uses. */
    int          in_init;
    uint32_t     init_cycles;
    double       cycles_to_ms;
} capture_t;

static capture_t g_cap;

/* sid.cpp declares this as plain C++ (`extern void recordPokeSID(...)`), the
 * same way the upstream Raspberry Pi driver defines it. */
void recordPokeSID(uint32_t ts, uint8_t reg, uint8_t value)
{
    if (!g_cap.enabled || g_cap.oom) return;

    double t_ms;
    if (g_cap.in_init) {
        t_ms = 0.0;
    } else {
        /* Cycle counter is 32-bit and wraps after ~72 minutes of PAL clock. */
        int64_t d = (int64_t)(uint32_t)(ts - g_cap.init_cycles);
        if (d < 0) d = 0;
        t_ms = (double)d * g_cap.cycles_to_ms;
    }

    if (g_cap.n == g_cap.cap) {
        size_t want = g_cap.cap ? g_cap.cap * 2 : 4096;
        sid_event_t *nv = (sid_event_t *)realloc(g_cap.ev, want * sizeof *nv);
        if (!nv) { g_cap.oom = 1; return; }
        g_cap.ev = nv;
        g_cap.cap = want;
    }
    g_cap.ev[g_cap.n].t_ms = t_ms;
    g_cap.ev[g_cap.n].reg  = reg;
    g_cap.ev[g_cap.n].val  = value;
    g_cap.n++;
}

/* ---- shared song setup ---------------------------------------------- */

static int read_file(const char *path, unsigned char **out, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz <= 0 || sz > 0xffff) { fclose(f); return -1; }
    rewind(f);
    unsigned char *buf = (unsigned char *)malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);
    *out = buf;
    *len = (size_t)sz;
    return 0;
}

#define WS_RATE 44100

/* Samples per emulated frame: one VIC screen refresh, exactly like the
 * upstream driver computes it. */
static uint16_t frame_chunk(void)
{
    int n = (int)(WS_RATE / vicFramesPerSecond());
    return (uint16_t)(n > 0 ? n : 1);
}

int sid_websid_available(void) { return 1; }

int sid_websid_extract(const char *path, int subsong, double seconds,
                       sid_event_t **events, size_t *count,
                       sid_song_info_t *info, char *err, size_t errlen)
{
    if (err && errlen) err[0] = 0;

    memset(&g_cap, 0, sizeof g_cap);
    g_cap.cycles_to_ms = 1000.0 / info->clock_hz;
    g_cap.in_init = 1;
    g_cap.enabled = 1;

    unsigned char *buf = NULL;
    size_t len = 0;
    if (read_file(path, &buf, &len) != 0) {
        snprintf(err, errlen, "cannot read %s", path);
        g_cap.enabled = 0;
        return -1;
    }
    if (loadSidFile(0, buf, (uint32_t)len, WS_RATE, (char *)path, 0, 0, 0) != 0) {
        free(buf);
        snprintf(err, errlen, "WebSid rejected %s as a SID file", path);
        g_cap.enabled = 0;
        return -1;
    }
    free(buf);

    /* Adopt the emulator's own PAL/NTSC decision for the timestamp conversion
     * and report it back, so the container header matches what was recorded. */
    uint8_t is_ntsc = FileLoader::getNTSCMode();
    info->pal = is_ntsc ? 0 : 1;
    info->clock_hz = is_ntsc ? 1022727.0 : 985249.0;
    info->vsync_hz = is_ntsc ? 59.8260895 : 50.124542;
    g_cap.cycles_to_ms = 1000.0 / info->clock_hz;

    /* A .sid header names its default song (1-based); -1 means "use it", the
     * same convention sid_song_extract() follows. Falling back to 0 here
     * silently recorded the wrong song (Yie_A's default is #19). */
    int track = subsong < 0 ? (int)info->start_song - 1 : subsong;
    if (track < 0) track = 0;

    if (playTune((uint32_t)track, 0) != 0) {
        snprintf(err, errlen, "WebSid cannot select subsong %d", track);
        g_cap.enabled = 0;
        free(g_cap.ev);
        g_cap.ev = NULL;
        return -1;
    }

    /* Init is done: from here on timestamps are relative to this point. */
    g_cap.init_cycles = sysCycles();
    g_cap.in_init = 0;

    uint8_t simple = (uint8_t)!FileLoader::isExtendedSidFile();
    uint8_t speed = FileLoader::getCurrentSongSpeed();
    uint16_t chunk = frame_chunk();
    int16_t *synth = (int16_t *)malloc(sizeof(int16_t) * ((size_t)chunk * 2 + 1));
    if (!synth) {
        snprintf(err, errlen, "out of memory");
        g_cap.enabled = 0;
        free(g_cap.ev);
        g_cap.ev = NULL;
        return -1;
    }

    double frame_ms = 1000.0 / info->vsync_hz;
    long long frames_needed = (long long)(seconds * 1000.0 / frame_ms + 0.5);
    for (long long i = 0; i < frames_needed; i++)
        Core::runOneFrame(simple, speed, synth, 0, chunk);

    free(synth);
    g_cap.enabled = 0;

    if (g_cap.oom) {
        snprintf(err, errlen, "out of memory while recording");
        free(g_cap.ev);
        g_cap.ev = NULL;
        return -1;
    }

    *events = g_cap.ev;
    *count = g_cap.n;
    g_cap.ev = NULL;
    g_cap.n = g_cap.cap = 0;
    return 0;
}

/* ---- audio renderer ------------------------------------------------- */

struct sid_websid_render {
    double   clock_hz;
    int      model8580;
    double   cycles_per_sample;
    double   accum;
    double   scale;
    int      noise_filter_off;
    int16_t *stereo;        /* interleaved scratch for SID::synthSample */
    size_t   stereo_frames;
};

sid_websid_render_t *sid_websid_render_new(double clock_hz, int model8580,
                                           double sample_rate)
{
    (void)sample_rate;      /* the SID is always clocked at its own rate here */
    sid_websid_render_t *r = (sid_websid_render_t *)calloc(1, sizeof *r);
    if (!r) return NULL;

    /* SID::writeMem() mirrors every write into the I/O page, so the RAM and
     * the $D000 page must exist even though this path never runs a CPU. The
     * loader normally does that; here it has to be done by hand. */
    memResetKernelROM(0);
    memResetRAM(0, 1);
    memResetIO();

    r->clock_hz = clock_hz;
    r->model8580 = model8580;
    r->cycles_per_sample = clock_hz / WS_RATE;
    r->accum = 0.0;

    uint8_t addr_list[4] = { 0, 0, 0, 0 };
    uint16_t flags = model8580 ? 0x20 : 0x00;
    SID::getHWConfigurator()->configure(0, 2, flags, addr_list);
    SID::resetAll(WS_RATE, (uint32_t)(clock_hz + 0.5), 0, 1);

    r->scale = SID::getScale();
    r->stereo_frames = 0;
    r->stereo = NULL;
    return r;
}

void sid_websid_render_free(sid_websid_render_t *r)
{
    if (!r) return;
    free(r->stereo);
    free(r);
}

void sid_websid_render_reset(sid_websid_render_t *r)
{
    if (!r) return;
    memset(&g_cap, 0, sizeof g_cap);   /* drop anything a stray recordPokeSID saw */
    memResetIO();
    uint8_t addr_list[4] = { 0, 0, 0, 0 };
    uint16_t flags = r->model8580 ? 0x20 : 0x00;
    SID::getHWConfigurator()->configure(0, 2, flags, addr_list);
    SID::resetAll(WS_RATE, (uint32_t)(r->clock_hz + 0.5), 0, 1);
    r->cycles_per_sample = r->clock_hz / WS_RATE;
    r->accum = 0.0;
    r->scale = SID::getScale();
}

void sid_websid_render_write(sid_websid_render_t *r, uint8_t reg, uint8_t val)
{
    if (!r) return;
    if (r->noise_filter_off) {
        /* "filter bypass" without touching the emulator: stop routing voices
         * through the filter and clear the filter mode bits, keeping the
         * $D418 master volume. */
        if (reg == 0x17) val = 0x00;
        if (reg == 0x18) val = (uint8_t)(val & 0x0F);
    }
    sidWriteMem((uint16_t)(0xd400 + reg), val);
}

void sid_websid_render_render(sid_websid_render_t *r, short *buf, size_t frames)
{
    if (!r || !frames) return;

    if (r->stereo_frames < frames) {
        int16_t *nv = (int16_t *)realloc(r->stereo, frames * 2 * sizeof(int16_t));
        if (!nv) return;
        r->stereo = nv;
        r->stereo_frames = frames;
    }
    memset(r->stereo, 0, frames * 2 * sizeof(int16_t));

    for (size_t i = 0; i < frames; i++) {
        while (r->accum < r->cycles_per_sample) {
            SID::clockAll();
            r->accum += 1.0;
        }
        r->accum -= r->cycles_per_sample;
        SID::synthSample(r->stereo, 0, &r->scale, (uint32_t)i);
        /* single SID without an explicit 2nd channel: only the left channel
         * carries signal (see SID::synthSample's _dest_channel). */
        buf[i] = r->stereo[i * 2];
    }
}

void sid_websid_render_set_filter(sid_websid_render_t *r, int on)
{
    if (!r) return;
    r->noise_filter_off = on ? 0 : 1;
}
