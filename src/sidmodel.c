/*
 * sidmodel.c - software MOS 6581/8580 model for the sidvgm preview player.
 *
 * The shape of the emulation follows the cycle-by-cycle references rather than
 * a "get it roughly right" shortcut, because the point of this player is to
 * judge whether a recorded stream is correct:
 *
 *   - oscillator: 24-bit accumulator, one step per PHI2 cycle, output taken
 *     from the top 12 bits;
 *   - envelope: WebSid's LFSR-threshold model (src/envelope.cpp), including the
 *     per-value exponential delay on decay/release and the zero-lock that makes
 *     a release-to-attack restart behave like the real thing;
 *   - period table: the same 16 COUNTER_PERIOD values WebSid uses, which come
 *     from reSID's analysis of the chip;
 *   - combined waveforms: bitwise AND of the selected waveform outputs, which
 *     is how reSID models the 6581's behaviour;
 *   - filter: Chamberlin state-variable, cutoff from the 11-bit register, with
 *     the resonance nibble mapped onto the damping term.
 *
 * What it does not do: the 6581 filter's nonlinear transfer curve, the exact
 * combined-waveform tables per chip revision, and the DAC's nonlinearity. Those
 * matter for timbre A/B against the real chip, not for "are the notes and
 * envelopes in the right place".
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "sidmodel.h"

/* -std=c11 hides M_PI, which the filter coefficient needs. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* WebSid's COUNTER_PERIOD: the LFSR value that must be hit before the envelope
 * moves one step, indexed by the 4-bit rate. */
static const unsigned short COUNTER_PERIOD[16] = {
    8, 32, 63, 95, 150, 220, 267, 314, 393, 942, 1961, 3138, 3922, 11765, 19608, 31373
};

/* Exponential step delay for decay/release, indexed by the current envelope
 * level: the chip walks the lower half of its range more slowly. Same table
 * WebSid derives in the comment above its definition. */
static unsigned char EXPONENTIAL_DELAYS[256];
static int s_tables_ready = 0;

static void build_tables(void)
{
    static const unsigned char from[6] = { 93, 54, 26, 14, 6, 0 };
    static const unsigned char val[6]  = { 1, 2, 4, 8, 16, 30 };
    for (unsigned i = 0; i < 256; i++) {
        unsigned char q = 1;
        for (unsigned j = 0; j < 6; j++) {
            if (i > from[j]) { q = val[j]; break; }
        }
        EXPONENTIAL_DELAYS[i] = q;
    }
    s_tables_ready = 1;
}

#define LFSR_LIMIT 0x8000u

enum { PH_ATTACK = 0, PH_DECAY = 1, PH_SUSTAIN = 2, PH_RELEASE = 3 };

typedef struct {
    /* registers */
    unsigned freq;
    unsigned pw;
    uint8_t  ctrl;
    uint8_t  ad, sr;

    /* oscillator */
    unsigned acc;
    unsigned noise;
    unsigned prev_msb;          /* for ring mod / sync */

    /* envelope */
    unsigned lfsr;
    unsigned char phase;
    unsigned char level;
    unsigned char exp_counter;
    unsigned char zero_lock;
    unsigned attack, decay, sustain, release;

    /* pulse/voice output */
    int out;
} voice_t;

struct sid_model {
    double   clock_hz;
    double   sample_rate;
    int      model8580;
    int      filter_on;

    voice_t  v[3];

    uint8_t  fc_lo, fc_hi, res_filt, mode_vol;
    int      master_vol;

    /* output accumulator for decimation */
    double   cycle_acc;

    /* filter state (per chip cycle) */
    double   flp, fbp;
    double   f, q;

    /* DC blocker */
    double   dc_x1, dc_y1;

    short    last;
};

/* ---- envelope (WebSid's model) ---------------------------------------- */

static void env_sync(voice_t *v)
{
    v->attack  = COUNTER_PERIOD[v->ad >> 4];
    v->decay   = COUNTER_PERIOD[v->ad & 0x0F];
    v->release = COUNTER_PERIOD[v->sr & 0x0F];
    unsigned s = v->sr >> 4;
    v->sustain = (s << 4) | s;
}

static void env_reset(voice_t *v)
{
    v->lfsr = 0;
    v->phase = PH_RELEASE;
    v->level = 0;
    v->exp_counter = 0;
    v->zero_lock = 1;
    env_sync(v);
}

static int env_exp_delay(voice_t *v)
{
    v->exp_counter++;
    if (v->exp_counter >= EXPONENTIAL_DELAYS[v->level]) {
        v->exp_counter = 0;
        return 1;
    }
    return 0;
}

static void env_clock(voice_t *v)
{
    unsigned char prev = v->level;

    if (++v->lfsr >= LFSR_LIMIT) v->lfsr = 0;

    switch (v->phase) {
    case PH_ATTACK:
        if (v->lfsr == v->attack && !v->zero_lock) {
            v->lfsr = 0;
            v->level = (unsigned char)((v->level + 1) & 0xFF);
            v->exp_counter = 0;
            if (v->level == 0xFF) v->phase = PH_DECAY;
        }
        break;
    case PH_DECAY:
        if (v->lfsr == v->decay && !v->zero_lock) {
            v->lfsr = 0;
            if (env_exp_delay(v)) {
                if (v->level != v->sustain) v->level--;
                else v->phase = PH_SUSTAIN;
            }
        }
        break;
    case PH_SUSTAIN:
        if (v->lfsr == v->decay) v->lfsr = 0;
        if (v->level > v->sustain) v->phase = PH_DECAY;
        break;
    case PH_RELEASE:
        if (v->lfsr == v->release && !v->zero_lock) {
            v->lfsr = 0;
            if (env_exp_delay(v)) v->level--;
        }
        break;
    }

    if (v->level == 0 && prev > 0) v->zero_lock = 1;
}

/* ---- oscillator ------------------------------------------------------- */

static unsigned noise_clock(voice_t *v)
{
    /* 23-bit LFSR, tapped at bits 22 and 17. */
    unsigned bit = ((v->noise >> 22) ^ (v->noise >> 17)) & 1u;
    v->noise = ((v->noise << 1) | bit) & 0x7FFFFFu;
    return (v->noise >> 11) & 0xFFFu;
}

/* One PHI2 cycle for one voice. `sync_src_msb` is the next voice's MSB, used
 * for hard sync and ring modulation. */
static void voice_clock(sid_model_t *m, int idx, unsigned prev_acc)
{
    voice_t *v = &m->v[idx];
    uint8_t ctrl = v->ctrl;

    /* TEST resets the accumulator and holds it there. */
    if (ctrl & 0x08) {
        v->acc = 0;
    } else {
        /* hard sync: reset when the previous voice's accumulator wraps */
        if ((ctrl & 0x02) && (prev_acc & 0x800000u) &&
            !(m->v[(idx + 2) % 3].acc & 0x800000u))
            v->acc = 0;
        v->acc = (v->acc + v->freq) & 0xFFFFFFu;
    }

    unsigned msb = v->acc & 0x800000u;
    unsigned acc_msb_used = v->acc;

    /* ring modulation: XOR this voice's MSB with the previous voice's */
    if (ctrl & 0x04) {
        unsigned pmsb = m->v[(idx + 2) % 3].acc & 0x800000u;
        if (pmsb) acc_msb_used ^= 0x800000u;
    }

    unsigned tri  = (acc_msb_used & 0x800000u)
                    ? ((acc_msb_used >> 11) & 0xFFF)
                    : (~(acc_msb_used >> 11) & 0xFFF);
    unsigned saw  = (v->acc >> 12) & 0xFFF;
    unsigned pls  = ((v->acc >> 12) >= (v->pw & 0xFFFu)) ? 0xFFFu : 0u;
    unsigned noi  = noise_clock(v);

    unsigned out = 0;
    int sel = 0;
    uint8_t wf = ctrl & 0xF0;
    if (wf & 0x10) { out = (sel ? (out & tri) : tri); sel = 1; }
    if (wf & 0x20) { out = (sel ? (out & saw) : saw); sel = 1; }
    if (wf & 0x40) { out = (sel ? (out & pls) : pls); sel = 1; }
    if (wf & 0x80) { out = (sel ? (out & noi) : noi); sel = 1; }
    if (!sel) out = 0;

    /* voice output: 12-bit wave times the 8-bit envelope */
    v->out = (int)((out * (unsigned)v->level) >> 8);

    env_clock(v);
    v->prev_msb = msb;
}

/* ---- filter ----------------------------------------------------------- */

static void filter_update(sid_model_t *m)
{
    /* 11-bit cutoff. The two chips do not share a scale: the 6581's cutoff
     * register is strongly non-linear and its bottom end is around 220 Hz, not
     * near-DC, while the 8580 is close to linear up to ~12 kHz. A linear
     * 6581 mapping makes any tune that parks the cutoff at 0 sound silent,
     * which is how this was noticed (turboutrun writes $D416=$B0 with
     * $D417=$07). Geometric interpolation between the two measured ends. */
    unsigned fc = (unsigned)m->fc_lo | ((unsigned)(m->fc_hi & 0x07) << 3);
    double hz;
    if (m->model8580)
        hz = 30.0 + (double)fc * 5.8;
    else
        hz = 220.0 * pow(12000.0 / 220.0, (double)fc / 2047.0);

    double nyq = m->clock_hz * 0.5;
    if (hz > nyq * 0.45) hz = nyq * 0.45;

    /* Chamberlin SVF coefficient. */
    m->f = 2.0 * sin(M_PI * hz / m->clock_hz);

    unsigned r = m->res_filt & 0x0F;
    /* Q rises with the resonance nibble; keep the loop from ringing apart. */
    m->q = 1.0 - (double)r / 16.0;
    if (m->q < 0.15) m->q = 0.15;
}

/* Chamberlin state-variable. $D418 bits 4/5/6 select low/band/high pass, and
 * like the chip they sum when more than one is set. */
static double filter_step(sid_model_t *m, double in, uint8_t mode)
{
    double hp = in - m->flp - m->q * m->fbp;
    double bp = m->f * hp + m->fbp;
    double lp = m->f * bp + m->flp;
    m->fbp = bp;
    m->flp = lp;

    double out = 0.0;
    if (mode & 0x10) out += lp;
    if (mode & 0x20) out += bp;
    if (mode & 0x40) out += hp;
    if (!(mode & 0x70)) out = lp;      /* no mode selected: pass low */
    return out;
}

/* ---- public ----------------------------------------------------------- */

sid_model_t *sid_model_new(double clock_hz, int model8580, double sample_rate)
{
    if (!s_tables_ready) build_tables();
    if (clock_hz <= 0.0) clock_hz = 985248.0;
    if (sample_rate <= 0.0) sample_rate = 44100.0;

    sid_model_t *m = (sid_model_t *)calloc(1, sizeof *m);
    if (!m) return NULL;
    m->clock_hz = clock_hz;
    m->sample_rate = sample_rate;
    m->model8580 = model8580;
    m->filter_on = 1;
    m->cycle_acc = 0.0;
    sid_model_reset(m);
    return m;
}

void sid_model_free(sid_model_t *m)
{
    free(m);
}

void sid_model_reset(sid_model_t *m)
{
    if (!m) return;
    memset(m->v, 0, sizeof m->v);
    for (int i = 0; i < 3; i++) {
        m->v[i].noise = 0x7FFFF8u;
        env_reset(&m->v[i]);
    }
    m->fc_lo = m->fc_hi = m->res_filt = m->mode_vol = 0;
    m->master_vol = 0;
    m->flp = m->fbp = 0.0;
    m->dc_x1 = m->dc_y1 = 0.0;
    m->last = 0;
    m->cycle_acc = 0.0;
    filter_update(m);
}

void sid_model_set_filter(sid_model_t *m, int on)
{
    if (m) m->filter_on = on ? 1 : 0;
}

void sid_model_write(sid_model_t *m, uint8_t reg, uint8_t val)
{
    if (!m || reg > 0x18) return;
    voice_t *v = NULL;
    int vi = -1;

    if (reg < 0x15) {
        vi = reg / 7;
        v = &m->v[vi];
        switch (reg % 7) {
        case 0: v->freq = (v->freq & 0xFF00u) | val; break;
        case 1: v->freq = (v->freq & 0x00FFu) | ((unsigned)val << 8); break;
        case 2: v->pw = (v->pw & 0xFF00u) | val; break;
        case 3: v->pw = (v->pw & 0x00FFu) | ((unsigned)(val & 0x0F) << 8); break;
        case 4: {
            uint8_t old_gate = v->ctrl & 0x01;
            uint8_t new_gate = val & 0x01;
            if (!old_gate && new_gate) {
                v->phase = PH_ATTACK;
                v->zero_lock = 0;
            } else if (old_gate && !new_gate) {
                v->phase = PH_RELEASE;
            }
            v->ctrl = val;
            break;
        }
        case 5:
            v->ad = val;
            v->attack  = COUNTER_PERIOD[val >> 4];
            v->decay   = COUNTER_PERIOD[val & 0x0F];
            break;
        case 6: {
            v->sr = val;
            unsigned s = val >> 4;
            v->sustain = (s << 4) | s;
            v->release = COUNTER_PERIOD[val & 0x0F];
            break;
        }
        }
        return;
    }

    switch (reg) {
    case 0x15: m->fc_lo = val; filter_update(m); break;
    case 0x16: m->fc_hi = val; filter_update(m); break;
    case 0x17: m->res_filt = val; filter_update(m); break;
    case 0x18:
        m->mode_vol = val;
        m->master_vol = val & 0x0F;
        break;
    }
}

void sid_model_render(sid_model_t *m, short *buf, size_t frames)
{
    if (!m || !buf) return;
    double cycles_per_sample = m->clock_hz / m->sample_rate;
    if (cycles_per_sample < 1.0) cycles_per_sample = 1.0;

    for (size_t n = 0; n < frames; n++) {
        m->cycle_acc += cycles_per_sample;
        unsigned cycles = (unsigned)m->cycle_acc;
        m->cycle_acc -= (double)cycles;
        if (cycles == 0) cycles = 1;

        double sum = 0.0;
        for (unsigned c = 0; c < cycles; c++) {
            unsigned prev_acc = m->v[2].acc;
            for (int i = 0; i < 3; i++) {
                voice_clock(m, i, prev_acc);
                prev_acc = m->v[i].acc;
            }

            /* $D417 bits 0-2 route voice 1/2/3 (plus bit 3 external) through
             * the filter; $D418 bits 4-6 pick the filter mode, bit 7 mutes
             * voice 3. Getting the routing wrong here silences the filter
             * path completely, so it is read from res_filt, not mode_vol. */
            double filtered = 0.0, direct = 0.0;
            for (int i = 0; i < 3; i++) {
                if (m->mode_vol & 0x80 && i == 2) continue;   /* voice 3 off */
                if (m->res_filt & (1u << i)) filtered += (double)m->v[i].out;
                else                         direct   += (double)m->v[i].out;
            }
            double sample;
            if (m->filter_on && (m->res_filt & 0x07))
                sample = filter_step(m, filtered, m->mode_vol) + direct;
            else
                sample = filtered + direct;
            sum += sample;
        }

        double val = sum / (double)cycles;

        /* DC blocker: the wave outputs are unipolar, so the raw sum carries a
         * large offset that would eat the whole 16-bit range. */
        double hp = val - m->dc_x1 + 0.995 * m->dc_y1;
        m->dc_x1 = val;
        m->dc_y1 = hp;

        double scaled = hp * (double)m->master_vol / 15.0 * 1.2;
        if (scaled > 32767.0) scaled = 32767.0;
        if (scaled < -32768.0) scaled = -32768.0;
        buf[n] = (short)scaled;
        m->last = buf[n];
    }
}
