/*
 * sid6581.c - MOS 6581 / 8580 driver for the SPFM parallel bus.
 *
 * Every register write goes out as one Path A command. See sid6581.h for why
 * that is the whole interface: the 74LS374 + 74HCT02 module turns the two bus
 * cycles Path A generates into "latch address" then "select and write".
 */
#include "sid6581.h"
#include "spfm_transport.h"

#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* ADSR timing, MOS 6581 datasheet (8580 numbers differ, mostly faster).  */
/* Index 0 is the shortest time, not "envelope disabled".                */
/* ------------------------------------------------------------------ */
const uint16_t sid_adsr_attack_ms[16] = {
       2,    8,   16,   24,   38,   56,   68,   80,
     100,  250,  500,  800, 1000, 3000, 5000, 8000
};

const uint16_t sid_adsr_decay_ms[16] = {
       6,   24,   48,   72,  114,  168,  204,  240,
     300,  750, 1500, 2400, 3000, 9000, 15000, 24000
};

/* ------------------------------------------------------------------ */
/* per-slot shadow state                                               */
/* ------------------------------------------------------------------ */

#define SID_MAX_SLOTS 4

static sid_voice_t s_voices[SID_MAX_SLOTS][3];
static uint8_t     s_volume[SID_MAX_SLOTS];
static uint8_t     s_mode[SID_MAX_SLOTS];

static sid_write_hook_t s_hook = NULL;
static void            *s_hook_user = NULL;

/* Hardware is 74LS374 + 74HCT02 + 74HCT32 (see docs/接线表_374_02_min.md).
 * The 74HC32 encodes the write condition into TRIG_n = !A0 | /WR | CSn, which
 * drives the SID's /CS directly; the SID's R/W is the SPFM /WR line itself.
 * One write per register is enough.
 *
 * Two earlier designs are superseded and kept only as history (see
 * docs/写入时序调试记录.md §9): a PHI2-counted 13 us window (the RTL-derived
 * "write must span a PHI2 edge" rule turned out to be wrong) and a 74HC74
 * reshaping stage (what actually caused the garbled single writes was one
 * gate driving two CMOS loads, not the missing buffer). The old timing knobs
 * stay reachable from the CLI to reproduce those setups. */
static sid_write_mode_t s_mode_w = SID_WRITE_TWO_PHASE;
static uint32_t         s_phase_gap = 0;
static uint32_t         s_write_delay_us = 0;
static uint32_t         s_write_repeat = 1;

/* Split-mode counters. Defaults keep the phases symmetric so a bare
 * `--write-mode split` behaves like the NOP mode above. */
static uint32_t         s_addr_rep = 4;
static uint32_t         s_data_rep = 8;
static uint32_t         s_gap_random = 0;

/* See sid6581.h: off for the console commands (one USB flush per write, so a
 * typed register takes effect at once), on for stream playback (one flush per
 * frame). */
static bool             s_deferred = false;

void sid_set_deferred_flush(bool on)
{
    s_deferred = on;
}

bool sid_get_deferred_flush(void)
{
    return s_deferred;
}

void sid_set_write_hook(sid_write_hook_t hook, void *user)
{
    s_hook = hook;
    s_hook_user = user;
}

void sid_set_write_mode(sid_write_mode_t mode)
{
    s_mode_w = mode;
}

sid_write_mode_t sid_get_write_mode(void)
{
    return s_mode_w;
}

void sid_set_phase_gap(uint32_t ticks)
{
    s_phase_gap = ticks;
}

uint32_t sid_get_phase_gap(void)
{
    return s_phase_gap;
}

void sid_set_write_delay(uint32_t us)
{
    s_write_delay_us = us;
}

uint32_t sid_get_write_delay(void)
{
    return s_write_delay_us;
}

void sid_set_write_repeat(uint32_t n)
{
    s_write_repeat = n ? n : 1;
}

uint32_t sid_get_write_repeat(void)
{
    return s_write_repeat;
}

void sid_set_addr_repeat(uint32_t n)
{
    s_addr_rep = n ? n : 1;
}

uint32_t sid_get_addr_repeat(void)
{
    return s_addr_rep;
}

void sid_set_data_repeat(uint32_t n)
{
    s_data_rep = n ? n : 1;
}

uint32_t sid_get_data_repeat(void)
{
    return s_data_rep;
}

void sid_set_gap_random(uint32_t max_extra)
{
    s_gap_random = max_extra;
}

uint32_t sid_get_gap_random(void)
{
    return s_gap_random;
}

/* Small xorshift PRNG: we only need phase dithering, not statistical quality. */
static uint32_t gap_rand(void)
{
    static uint32_t s = 0x2545F491u;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

static uint32_t gap_for_this_write(void)
{
    if (s_gap_random == 0) return s_phase_gap;
    return s_phase_gap + (gap_rand() % (s_gap_random + 1));
}

/* ------------------------------------------------------------------ */
/* register access                                                     */
/* ------------------------------------------------------------------ */
void sid_write(uint8_t slot, uint8_t reg, uint8_t data)
{
    uint8_t r = (uint8_t)(reg & 0x1F);   /* 25 registers, bit7 must stay 0 */

    if (s_hook) s_hook(slot, r, data, s_hook_user);

    switch (s_mode_w) {
    case SID_WRITE_BURST:
        /* Path A, no host interaction at all: the firmware fires both bus
         * cycles itself. Cheapest possible sequence, 4 bytes per write. */
        for (uint32_t i = 0; i < s_write_repeat; i++)
            spfm_write_path_a(slot, 0x00, r, data);
        break;

    case SID_WRITE_PATH_A_SOFT:
        /* Path A plus a host sleep after each command. Same bus cycles as
         * BURST, only the spacing between consecutive register writes
         * changes, so it isolates "USB pacing" from "two-phase gap". */
        for (uint32_t i = 0; i < s_write_repeat; i++) {
            spfm_write_path_a(slot, 0x00, r, data);
            spfm_flush();
            if (s_write_delay_us) spfm_delay_us(s_write_delay_us);
        }
        break;

    case SID_WRITE_TWO_PHASE_NOP:
        /* Two Path B records with firmware NOP ticks in between. The gap is
         * measured in STM32 loop cycles, so it cannot be disturbed by USB
         * scheduling. This is the precise one. */
        for (uint32_t i = 0; i < s_write_repeat; i++)
            spfm_write_phased(slot, r, data, gap_for_this_write());
        break;

    case SID_WRITE_SPLIT:
        /* Address repeated A times, then D data writes with a *different* gap
         * before each one. The address phase latches once and the 374 holds
         * it, so only the data phases need phase diversity. */
        {
            uint32_t gaps[64];
            uint32_t nd = s_data_rep > 64 ? 64 : s_data_rep;
            for (uint32_t d = 0; d < nd; d++) {
                gaps[d] = s_gap_random
                          ? s_phase_gap + (gap_rand() % (s_gap_random + 1))
                          : s_phase_gap + d;   /* ramp when not randomised */
            }
            for (uint32_t i = 0; i < s_write_repeat; i++)
                spfm_write_split_gaps(slot, r, data, s_addr_rep, gaps, nd);
        }
        break;

    case SID_WRITE_TWO_PHASE:
    default:
        /* YM7129 pattern: one 6-byte record, flush, then host sleep. */
        for (uint32_t i = 0; i < s_write_repeat; i++) {
            spfm_write_two_phase(slot, r, data);
            if (!s_deferred) spfm_flush();
            if (s_write_delay_us) spfm_delay_us(s_write_delay_us);
        }
        break;
    }
}

void sid_write_batch(uint8_t slot, const sid_reg_t *regs, size_t count)
{
    for (size_t i = 0; i < count; i++)
        sid_write(slot, regs[i].reg, regs[i].data);
}

bool sid_flush(void)
{
    return spfm_flush();
}

/* ------------------------------------------------------------------ */
/* pitch                                                               */
/* ------------------------------------------------------------------ */
uint16_t sid_freq_from_hz(double hz, double phi2_hz)
{
    if (phi2_hz <= 0.0 || hz <= 0.0) return 0;
    double v = hz * 16777216.0 / phi2_hz;
    if (v > 65535.0) v = 65535.0;
    return (uint16_t)(v + 0.5);
}

uint16_t sid_freq_from_note(uint8_t midi_note)
{
    return sid_freq_table[midi_note & 0x7F];
}

/* ------------------------------------------------------------------ */
/* chip setup                                                          */
/* ------------------------------------------------------------------ */
void sid_clear_all_regs(uint8_t slot)
{
    for (uint8_t r = 0; r < SID_REG_COUNT; r++)
        sid_write(slot, r, 0x00);
}

void sid_set_volume(uint8_t slot, uint8_t volume)
{
    uint8_t v = (uint8_t)(volume & SID_VOLUME_MASK);
    s_volume[slot & 3] = v;
    sid_write(slot, SID_REG_MODE_VOL, (uint8_t)(s_mode[slot & 3] | v));
}

void sid_set_mode(uint8_t slot, uint8_t mode)
{
    s_mode[slot & 3] = mode;
    sid_write(slot, SID_REG_MODE_VOL, (uint8_t)(mode | s_volume[slot & 3]));
}

void sid_set_filter_cutoff(uint8_t slot, uint16_t cutoff)
{
    uint16_t c = (uint16_t)(cutoff & 0x07FF);
    sid_write(slot, SID_REG_FC_LO, (uint8_t)(c & 0x07));
    sid_write(slot, SID_REG_FC_HI, (uint8_t)(c >> 3));
}

void sid_set_filter(uint8_t slot, uint16_t cutoff, uint8_t resonance, uint8_t routing)
{
    sid_set_filter_cutoff(slot, cutoff);
    sid_write(slot, SID_REG_RES_FILT,
              (uint8_t)(((resonance & 0x0F) << 4) | (routing & 0x0F)));
}

void sid_init(uint8_t slot, uint8_t volume)
{
    memset(s_voices[slot & 3], 0, sizeof(s_voices[slot & 3]));
    s_mode[slot & 3] = 0;
    s_volume[slot & 3] = 0;

    /* 1. Clear every register: the 374 outputs are undefined until the first
     *    address phase, so the SID may be holding garbage. */
    sid_clear_all_regs(slot);

    /* 2. Volume. Without this the chip is completely silent no matter what
     *    else is written. */
    sid_set_volume(slot, volume);

    /* 3. A neutral, audible voice setup so the first note does not depend on
     *    stale gate/decay values: triangle, PW 50%, mid ADSR. */
    for (uint8_t v = 0; v < 3; v++) {
        uint8_t base = (uint8_t)(v * 7);
        sid_write(slot, (uint8_t)(base + SID_REG_ATTACK_DECAY), 0x09);  /* A=0,D=9 */
        sid_write(slot, (uint8_t)(base + SID_REG_SUSTAIN_REL), 0xF0);  /* S=F,R=0 */
        sid_write(slot, (uint8_t)(base + SID_REG_PW_LO), 0x00);
        sid_write(slot, (uint8_t)(base + SID_REG_PW_HI), 0x08);        /* 0x800 = 50% */
        sid_write(slot, (uint8_t)(base + SID_REG_CONTROL), 0x00);      /* gate off */
        s_voices[slot & 3][v].wave = SID_CTL_TRI;
        s_voices[slot & 3][v].ad   = 0x09;
        s_voices[slot & 3][v].sr   = 0xF0;
        s_voices[slot & 3][v].pw   = 0x800;
    }

    /* 4. Filter fully open (no routing, no resonance) so nothing is hidden. */
    sid_set_filter(slot, 0x7FF, 0x00, 0x00);

    sid_flush();
}

/* ------------------------------------------------------------------ */
/* voices                                                              */
/* ------------------------------------------------------------------ */
static void voice_store(uint8_t slot, uint8_t voice, uint8_t note, uint16_t freq,
                        uint16_t pw, uint8_t wave, uint8_t ad, uint8_t sr,
                        bool active)
{
    sid_voice_t *v = &s_voices[slot & 3][voice & 3];
    v->note   = note;
    v->pw     = pw;
    v->wave   = (uint8_t)(wave & SID_CTL_WAVE_MASK);
    v->ad     = ad;
    v->sr     = sr;
    v->active = active;
    (void)freq;
}

void sid_voice_config(uint8_t slot, uint8_t voice, uint16_t freq, uint16_t pw,
                      uint8_t wave, uint8_t ad, uint8_t sr)
{
    if (voice > 2) return;
    uint8_t base = (uint8_t)(voice * 7);

    /* Frequency, low byte then high byte. */
    sid_write(slot, (uint8_t)(base + SID_REG_FREQ_LO), (uint8_t)(freq & 0xFF));
    sid_write(slot, (uint8_t)(base + SID_REG_FREQ_HI), (uint8_t)(freq >> 8));

    /* Pulse width: low 8 bits, then the high 4 bits live in the low nibble. */
    uint16_t p = (uint16_t)(pw & SID_PW_MAX);
    sid_write(slot, (uint8_t)(base + SID_REG_PW_LO), (uint8_t)(p & 0xFF));
    sid_write(slot, (uint8_t)(base + SID_REG_PW_HI), (uint8_t)((p >> 8) & 0x0F));

    /* Envelope. */
    sid_write(slot, (uint8_t)(base + SID_REG_ATTACK_DECAY), ad);
    sid_write(slot, (uint8_t)(base + SID_REG_SUSTAIN_REL), sr);

    /* Waveform with gate still off: the note starts when gate is raised. */
    sid_write(slot, (uint8_t)(base + SID_REG_CONTROL), (uint8_t)(wave & SID_CTL_WAVE_MASK));

    voice_store(slot, voice, s_voices[slot & 3][voice].note, freq, p, wave, ad, sr, false);
}

void sid_note_on(uint8_t slot, uint8_t voice, uint8_t midi_note, uint8_t wave,
                 uint8_t ad, uint8_t sr)
{
    if (voice > 2) return;
    uint8_t base = (uint8_t)(voice * 7);
    uint16_t freq = sid_freq_from_note(midi_note);

    sid_write(slot, (uint8_t)(base + SID_REG_FREQ_LO), (uint8_t)(freq & 0xFF));
    sid_write(slot, (uint8_t)(base + SID_REG_FREQ_HI), (uint8_t)(freq >> 8));
    sid_write(slot, (uint8_t)(base + SID_REG_ATTACK_DECAY), ad);
    sid_write(slot, (uint8_t)(base + SID_REG_SUSTAIN_REL), sr);
    /* Gate last: envelope restarts from the freshly written frequency. */
    sid_write(slot, (uint8_t)(base + SID_REG_CONTROL),
              (uint8_t)((wave & SID_CTL_WAVE_MASK) | SID_CTL_GATE));

    sid_voice_t *v = &s_voices[slot & 3][voice];
    v->active = true;
    v->note   = midi_note;
    v->wave   = (uint8_t)(wave & SID_CTL_WAVE_MASK);
    v->ad     = ad;
    v->sr     = sr;
}

void sid_note_off(uint8_t slot, uint8_t voice)
{
    if (voice > 2) return;
    uint8_t base = (uint8_t)(voice * 7);
    uint8_t wave = s_voices[slot & 3][voice].wave;

    /* Gate down, waveform preserved: the release phase runs. */
    sid_write(slot, (uint8_t)(base + SID_REG_CONTROL), (uint8_t)(wave & SID_CTL_WAVE_MASK));
    s_voices[slot & 3][voice].active = false;
}

void sid_note_kill(uint8_t slot, uint8_t voice)
{
    if (voice > 2) return;
    uint8_t base = (uint8_t)(voice * 7);

    /* Gate down and waveform cleared: output goes to DC 0 immediately instead
     * of waiting for the release envelope. */
    sid_write(slot, (uint8_t)(base + SID_REG_CONTROL), 0x00);
    s_voices[slot & 3][voice].active = false;
}

void sid_set_pulse(uint8_t slot, uint8_t voice, uint16_t pw)
{
    if (voice > 2) return;
    uint8_t base = (uint8_t)(voice * 7);
    uint16_t p = (uint16_t)(pw & SID_PW_MAX);
    sid_write(slot, (uint8_t)(base + SID_REG_PW_LO), (uint8_t)(p & 0xFF));
    sid_write(slot, (uint8_t)(base + SID_REG_PW_HI), (uint8_t)((p >> 8) & 0x0F));
    s_voices[slot & 3][voice].pw = p;
}

void sid_set_freq(uint8_t slot, uint8_t voice, uint16_t freq_reg)
{
    if (voice > 2) return;
    uint8_t base = (uint8_t)(voice * 7);
    sid_write(slot, (uint8_t)(base + SID_REG_FREQ_LO), (uint8_t)(freq_reg & 0xFF));
    sid_write(slot, (uint8_t)(base + SID_REG_FREQ_HI), (uint8_t)(freq_reg >> 8));
}

void sid_set_wave(uint8_t slot, uint8_t voice, uint8_t wave)
{
    if (voice > 2) return;
    uint8_t base = (uint8_t)(voice * 7);
    bool was_active = s_voices[slot & 3][voice].active;
    uint8_t w = (uint8_t)(wave & SID_CTL_WAVE_MASK);

    sid_write(slot, (uint8_t)(base + SID_REG_CONTROL),
              (uint8_t)(was_active ? (w | SID_CTL_GATE) : w));
    s_voices[slot & 3][voice].wave = w;
}

const sid_voice_t *sid_get_voice(uint8_t slot, uint8_t voice)
{
    if (voice > 2) return NULL;
    return &s_voices[slot & 3][voice];
}
