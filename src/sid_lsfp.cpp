/*
 * sid_lsfp.cpp - libsidplayfp + reSIDfp adapters.
 *
 * Recording: libsidplayfp's Player runs the real psiddrv (its own relocation,
 * CIA/VIC interrupt plumbing and MOS6510). Instead of a real chip it locks a
 * capture SID: a sidemu subclass whose write() appends the register write with
 * libsidplayfp's own PHI1 clock. Nothing about the 6502 timing is our
 * approximation any more.
 *
 * Audio: reSIDfp is driven directly (write / clock), which is exactly what
 * ReSIDfpBuilder does internally and what build/lsfpref.cpp renders with. It
 * therefore cannot lose a drum the way WebSid's digi detector does.
 *
 * Why a subclass instead of the public API only: libsidplayfp writes to the
 * SID through c64sid::poke -> sidemu::writeReg -> sidemu::write(), and that
 * last call is the single place every $D400-$D41F write passes. Capturing it
 * is the register stream, with no second emulator in the path.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <new>
#include <vector>

#include <sidplayfp/sidplayfp.h>
#include <sidplayfp/SidTune.h>
#include <sidplayfp/SidTuneInfo.h>
#include <sidplayfp/SidConfig.h>
#include <sidplayfp/sidbuilder.h>
#include <sidplayfp/siddefs.h>
#include <sidplayfp/builders/residfp.h>

#include "sidemu.h"

#include <residfp/residfp.h>

#include "sid_lsfp.h"

/* ---- capture --------------------------------------------------------- */

namespace {

/* libsidplayfp's scheduler counts PHI1/PHI2 half-cycles; getTime(PHI1) gives
 * the visible clock, which is the one the CPU and the SID run at. */
struct capture_t {
    sid_event_t *ev;
    size_t       n, cap;
    int          oom;
    /* Absolute cycle stamps are kept here and converted at the end, because
     * t=0 can only be decided once the init writes are recognisable. */
    double      *cyc;
};

class CaptureSid final : public libsidplayfp::sidemu
{
private:
    capture_t *m_cap;

    /* A real reSIDfp chip sits behind the capture. Tunes read $D41B/$D41C
     * (OSC3/ENV3 noise) and branch on the result; a recorder that answered 0
     * would make the 6502 take a different path and produce a different
     * register stream from the reference player. Only the digital state is
     * advanced here, so clockSilent() is enough. */
    reSIDfp::residfp m_chip;
    SidConfig::sid_model_t m_model = SidConfig::MOS6581;
    bool m_digiboost = false;

    void apply_model()
    {
        if (m_model == SidConfig::MOS8580) {
            m_chip.setChipModel(reSIDfp::CSG8580);
            m_chip.input(m_digiboost ? -32768 : 0);
        } else {
            m_chip.setChipModel(reSIDfp::MOS6581);
            m_chip.input(0);
        }
    }

    void advance()
    {
        const libsidplayfp::event_clock_t now =
            eventScheduler->getTime(libsidplayfp::EVENT_CLOCK_PHI1);
        const libsidplayfp::event_clock_t cycles = now - m_accessClk;
        if (cycles > 0) m_chip.clockSilent((unsigned)cycles);
        m_accessClk += cycles;
    }

    void push(uint_least8_t reg, uint8_t value)
    {
        if (m_cap->oom) return;
        if (m_cap->n == m_cap->cap) {
            size_t want = m_cap->cap ? m_cap->cap * 2 : 4096;
            sid_event_t *ne = (sid_event_t *)realloc(m_cap->ev, want * sizeof *ne);
            if (!ne) { m_cap->oom = 1; return; }
            m_cap->ev = ne;
            double *nc = (double *)realloc(m_cap->cyc, want * sizeof *nc);
            if (!nc) { m_cap->oom = 1; return; }
            m_cap->cyc = nc;
            m_cap->cap = want;
        }
        m_cap->cyc[m_cap->n]     = (double)m_accessClk;
        m_cap->ev[m_cap->n].t_ms = 0.0;
        m_cap->ev[m_cap->n].reg  = (uint8_t)(reg & 0x1f);
        m_cap->ev[m_cap->n].val  = value;
        m_cap->n++;
    }

public:
    CaptureSid(sidbuilder *builder, capture_t *cap)
        : libsidplayfp::sidemu(builder), m_cap(cap) {}

    uint8_t read(uint_least8_t addr) override
    {
        advance();
        return m_chip.read(addr);
    }

    void write(uint_least8_t addr, uint8_t data) override
    {
        advance();
        push(addr, data);
        m_chip.write(addr, data);
    }

    void reset(uint8_t volume) override
    {
        apply_model();
        m_chip.reset();
        m_chip.write(0x18, volume);
    }

    void clock() override { advance(); }

    void model(SidConfig::sid_model_t model, bool digiboost) override
    {
        /* libsidplayfp calls model() after lock() and before the clock starts.
         * Remember the choice and apply it now; reset() re-applies it. */
        m_model = model;
        m_digiboost = digiboost;
        apply_model();
    }

    void sampling(float, float, SidConfig::sampling_method_t) override {}
};

class CaptureBuilder final : public sidbuilder
{
private:
    capture_t *m_cap;

protected:
    libsidplayfp::sidemu *create() override
    {
        return new (std::nothrow) CaptureSid(this, m_cap);
    }

    const char *getCredits() const override { return "capture (no audio)"; }

public:
    CaptureBuilder(capture_t *cap) : sidbuilder("capture"), m_cap(cap) {}
};

/* libsidplayfp resolves PAL/NTSC itself (tune clock, else the default model).
 * Report the same decision so the container header and the renderer agree. */
void resolve_clock(const SidTuneInfo *ti, int *pal, double *clock_hz,
                   double *vsync_hz)
{
    SidTuneInfo::clock_t cs = ti->clockSpeed();
    int is_pal = (cs == SidTuneInfo::CLOCK_NTSC) ? 0 : 1;   /* default model PAL */
    *pal = is_pal;
    *clock_hz  = is_pal ? 985248.0 : 1022730.0;
    *vsync_hz  = is_pal ? 50.125 : 60.0;
}

/* The first play burst is separated from the tail of init by most of a frame.
 * Everything up to that point is state the player must have before t=0, so it
 * is stamped 0 rather than dropped. */
double find_play_origin(const capture_t *cap, double frame_cycles, double total)
{
    if (cap->n == 0) return total;
    for (size_t i = 1; i < cap->n; i++) {
        if (cap->cyc[i] - cap->cyc[i - 1] > frame_cycles * 0.5)
            return cap->cyc[i];
    }
    return cap->cyc[0];
}

void free_capture(capture_t *cap)
{
    free(cap->ev);
    free(cap->cyc);
    cap->ev = NULL;
    cap->cyc = NULL;
    cap->n = cap->cap = 0;
}

} /* namespace */

int sid_lsfp_available(void) { return 1; }

int sid_lsfp_extract(const char *path, int subsong, double seconds,
                     sid_event_t **events, size_t *count,
                     sid_song_info_t *info, char *err, size_t errlen)
{
    if (events) *events = NULL;
    if (count)  *count = 0;
    if (err && errlen) err[0] = 0;

    sid_song_info_t hdr;
    if (sid_song_probe(path, &hdr) != 0) {
        if (err && errlen) snprintf(err, errlen, "not a PSID/RSID file");
        return -1;
    }

    SidTune tune(path);
    if (!tune.getStatus()) {
        if (err && errlen) snprintf(err, errlen, "%s", tune.statusString());
        return -1;
    }

    /* 0 means "the file's default song" to libsidplayfp, which is exactly the
     * -1 convention the other recorders use. */
    tune.selectSong(subsong < 0 ? 0u : (unsigned)subsong + 1u);

    capture_t cap;
    memset(&cap, 0, sizeof cap);

    bool ok = false;
    {
        CaptureBuilder builder(&cap);
        sidplayfp engine;

        SidConfig cfg;
        cfg.frequency = 44100;          /* only feeds sampling(); capture ignores it */
        cfg.samplingMethod = SidConfig::INTERPOLATE;
        cfg.defaultC64Model = SidConfig::PAL;
        cfg.forceC64Model = false;
        cfg.defaultSidModel = SidConfig::MOS6581;
        cfg.forceSidModel = false;
        cfg.sidEmulation = &builder;
        /* A delay above MAX is picked at random from a time-seeded source, and
         * it shifts the phase of every tune that reads a free-running counter.
         * Pin it so recording the same file twice gives the same stream. */
        cfg.powerOnDelay = SidConfig::MAX_POWER_ON_DELAY;

        if (!engine.config(cfg) || !engine.load(&tune)) {
            if (err && errlen) snprintf(err, errlen, "%s", engine.error());
            free_capture(&cap);
            return -1;
        }

        const SidTuneInfo *ti = tune.getInfo();
        /* The container carries one SID socket and every captured write is
         * emitted as WRITE0, so a 2SID/3SID tune would be recorded as if both
         * chips were the same one. Refuse rather than write a corrupt stream. */
        if (ti->sidChips() > 1) {
            if (err && errlen)
                snprintf(err, errlen,
                         "%d SID chips; sidvgm captures one socket only",
                         ti->sidChips());
            free_capture(&cap);
            return -1;
        }

        int pal;
        double clock_hz, vsync_hz;
        resolve_clock(ti, &pal, &clock_hz, &vsync_hz);

        const double frame_cycles = clock_hz / vsync_hz;
        const long long budget = (long long)((seconds + 2.0) * clock_hz);

        /* 5000 cycles per play() call, like lsfpref: small enough for a
         * responsive cycle budget, large enough not to dominate runtime. */
        const unsigned CHUNK = 5000;
        long long run = 0;
        while (run < budget) {
            int res = engine.play(CHUNK);
            if (res < 0) {
                if (err && errlen) snprintf(err, errlen, "%s", engine.error());
                break;
            }
            run += CHUNK;
            if (cap.oom) break;
        }

        if (cap.oom) {
            if (err && errlen) snprintf(err, errlen, "out of memory while recording");
            free_capture(&cap);
            return -1;
        }

        double origin = find_play_origin(&cap, frame_cycles, (double)run);
        double limit  = seconds * clock_hz;
        double cycles_to_ms = 1000.0 / clock_hz;

        size_t kept = 0;
        for (size_t i = 0; i < cap.n; i++) {
            double d = cap.cyc[i] - origin;
            if (d < 0.0) d = 0.0;
            if (d > limit) continue;
            cap.ev[kept] = cap.ev[i];
            cap.ev[kept].t_ms = d * cycles_to_ms;
            kept++;
        }
        cap.n = kept;

        if (info) {
            *info = hdr;
            info->pal      = pal;
            info->clock_hz = clock_hz;
            info->vsync_hz = vsync_hz;
        }
        ok = true;
    }

    if (!ok) {
        free_capture(&cap);
        return -1;
    }

    *events = cap.ev;
    *count  = cap.n;
    free(cap.cyc);
    return 0;
}

/* ---- reSIDfp audio renderer ------------------------------------------ */

struct sid_lsfp_render {
    reSIDfp::residfp *sid;
    double            sample_rate;
    int               filter_on;
};

sid_lsfp_render_t *sid_lsfp_render_new(double clock_hz, int model8580,
                                       double sample_rate)
{
    sid_lsfp_render_t *r = (sid_lsfp_render_t *)calloc(1, sizeof *r);
    if (!r) return NULL;

    r->sid = new (std::nothrow) reSIDfp::residfp();
    if (!r->sid) { free(r); return NULL; }

    r->sid->setChipModel(model8580 ? reSIDfp::CSG8580 : reSIDfp::MOS6581);
    /* DECIMATE is what SidConfig::INTERPOLATE maps to inside reSIDfpEmu. */
    r->sid->setSamplingParameters(clock_hz, reSIDfp::DECIMATE, sample_rate);
    r->sid->reset();
    r->sid->write(0x18, 0x0f);      /* power-on volume, like reSIDfpEmu::reset */
    r->sample_rate = sample_rate;
    r->filter_on = 1;
    return r;
}

void sid_lsfp_render_free(sid_lsfp_render_t *r)
{
    if (!r) return;
    delete r->sid;
    free(r);
}

void sid_lsfp_render_reset(sid_lsfp_render_t *r)
{
    if (!r) return;
    r->sid->reset();
    r->sid->write(0x18, 0x0f);
    r->sid->enableFilter(r->filter_on != 0);
}

void sid_lsfp_render_write(sid_lsfp_render_t *r, uint8_t reg, uint8_t val)
{
    if (!r) return;
    r->sid->write((int)(reg & 0x1f), val);
}

void sid_lsfp_render_render(sid_lsfp_render_t *r, short *buf, size_t frames)
{
    if (!r || !frames) return;
    /* clock(buf, n) runs the SID until exactly n samples exist, so the sample
     * clock stays identical to the stream's 1/44100 tick base. */
    size_t done = 0;
    while (done < frames) {
        int chunk = (frames - done > 8192) ? 8192 : (int)(frames - done);
        r->sid->clock(buf + done, chunk);
        done += (size_t)chunk;
    }
}

void sid_lsfp_render_set_filter(sid_lsfp_render_t *r, int on)
{
    if (!r) return;
    r->filter_on = on;
    r->sid->enableFilter(on != 0);
}

/* ---- direct player (the lsfpref path) -------------------------------- */

struct sid_lsfp_player {
    sidplayfp        *engine;
    SidTune          *tune;
    ReSIDfpBuilder   *builder;
    std::vector<short> buffer;
    /* play() produces whole chunks; the caller asks for block-sized pieces.
     * Whatever is left over from the last chunk lives here, so no sample is
     * ever dropped (dropping them desynchronises the whole tune). */
    std::vector<short> pending;
    size_t            pend_pos;
    double            sample_rate;
    long long         frames;     /* rendered so far */
};

sid_lsfp_player_t *sid_lsfp_player_open(const char *path, int subsong,
                                        double sample_rate, int model8580,
                                        char *err, size_t errlen)
{
    if (err && errlen) err[0] = 0;

    sid_lsfp_player_t *p = new (std::nothrow) sid_lsfp_player();
    if (!p) {
        if (err && errlen) snprintf(err, errlen, "out of memory");
        return NULL;
    }
    p->engine = new (std::nothrow) sidplayfp();
    p->builder = new (std::nothrow) ReSIDfpBuilder("residfp");
    p->tune = new (std::nothrow) SidTune(path);
    if (!p->engine || !p->builder || !p->tune) {
        if (err && errlen) snprintf(err, errlen, "out of memory");
        delete p->engine; delete p->builder; delete p->tune; delete p;
        return NULL;
    }

    if (!p->tune->getStatus()) {
        if (err && errlen) snprintf(err, errlen, "%s", p->tune->statusString());
        delete p->engine; delete p->builder; delete p->tune; delete p;
        return NULL;
    }
    /* libsidplayfp: 0 means "the file's default song", 1..n are explicit.
     * subsong -1 is our "use the default", so it maps to 0. The call must
     * happen even then: skipping it leaves the tune in a different state and
     * changes the very first samples (Turbo Outrun proves it). */
    p->tune->selectSong(subsong < 0 ? 0u : (unsigned)subsong + 1u);

    SidConfig cfg;
    cfg.frequency = (uint_least32_t)sample_rate;
    cfg.samplingMethod = SidConfig::INTERPOLATE;
    cfg.defaultSidModel = model8580 ? SidConfig::MOS8580 : SidConfig::MOS6581;
    cfg.forceSidModel = false;
    cfg.sidEmulation = p->builder;
    /* Anything above MAX gets a random power-on delay, and the delay changes
     * the phase of every tune that reads a free-running counter. Pin it, so
     * two runs of the same file produce the same audio. */
    cfg.powerOnDelay = SidConfig::MAX_POWER_ON_DELAY;

    if (!p->engine->config(cfg) || !p->engine->load(p->tune)) {
        if (err && errlen) snprintf(err, errlen, "%s", p->engine->error());
        delete p->engine; delete p->builder; delete p->tune; delete p;
        return NULL;
    }
    p->engine->initMixer(true);          /* stereo, like lsfpref */

    int buf_samples = p->engine->getBufSize(5000);
    if (buf_samples <= 0) buf_samples = (int)(sample_rate * 0.02) * 2;
    p->buffer.assign((size_t)buf_samples, 0);
    p->sample_rate = sample_rate;
    p->frames = 0;
    return p;
}

void sid_lsfp_player_close(sid_lsfp_player_t *p)
{
    if (!p) return;
    delete p->engine;
    delete p->builder;
    delete p->tune;
    delete p;
}

size_t sid_lsfp_player_render(sid_lsfp_player_t *p, short *buf, size_t frames)
{
    if (!p || !frames) return 0;

    size_t done = 0;

    /* Serve anything left over from the previous chunk first. play() always
     * yields a whole number of samples, so truncating a chunk to the caller's
     * block size would delete samples and shift the rest of the tune. */
    while (done < frames && p->pend_pos < p->pending.size()) {
        buf[done++] = p->pending[p->pend_pos++];
    }
    if (p->pend_pos >= p->pending.size()) {
        p->pending.clear();
        p->pend_pos = 0;
    }

    while (done < frames) {
        int res = p->engine->play(5000);      /* ~5 ms at 1 MHz */
        if (res <= 0) break;
        unsigned mixed = p->engine->mix(p->buffer.data(), (unsigned)res);
        unsigned got = mixed / 2;             /* interleaved stereo -> mono */
        if (got == 0) continue;

        size_t i = 0;
        for (; i < got && done < frames; i++) {
            buf[done++] = p->buffer[i * 2];
            p->frames++;
        }
        if (i < got) {
            p->pending.resize(got - i);
            for (size_t k = i; k < got; k++)
                p->pending[k - i] = p->buffer[k * 2];
            p->pend_pos = 0;
        }
    }
    return done;
}

double sid_lsfp_player_elapsed(const sid_lsfp_player_t *p)
{
    return p ? (double)p->frames / p->sample_rate : 0.0;
}
