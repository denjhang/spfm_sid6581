/*
 * sid6581.h - MOS 6581 / 8580 driver for the SPFM parallel bus.
 *
 * Hardware: 74LS374 latches A0-A4 during the address phase, 74HCT02 derives
 * LE and /CS_sid so the SID is only selected during the data phase. The host
 * therefore does not touch any control line: one register write is one Path A
 * command, 4 bytes:
 *
 *     [slot, 0x00, reg & 0x1F, data]
 *
 * The firmware expands that into
 *
 *     BusWrite(slot, A0=0, D=reg&0x1F)   -> 374 CLK rising, address latched
 *     BusWrite(slot, A0=1, D=data)       -> /CS_sid low, SID samples the bus
 *
 * PHI2 comes from the module's own oscillator, so pitch calibration is a
 * compile-time constant here (SID_PHI2_HZ).
 */
#ifndef SID6581_H
#define SID6581_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* bus / board constants                                               */
/* ------------------------------------------------------------------ */

/* RE-SPFM puts the SID on the last of the four slots. */
#define SID_DEFAULT_SLOT     3u

/* 1.000 MHz can oscillator on the module (8580 boards use 0.985 MHz). */
#define SID_PHI2_HZ_DEFAULT  1000000.0

/* Register write cost: 4 bytes at 1.5 Mbaud ≈ 21 µs, two bus cycles. */
#define SID_WRITE_US         21u

/* ------------------------------------------------------------------ */
/* register map                                                        */
/* ------------------------------------------------------------------ */

#define SID_REG_COUNT        0x19   /* 0x00 - 0x18 */

/* Per-voice block: voice n base = n * 7 */
#define SID_V1_BASE          0x00
#define SID_V2_BASE          0x07
#define SID_V3_BASE          0x0E

#define SID_REG_FREQ_LO      0x00   /* +base, 8 bits */
#define SID_REG_FREQ_HI      0x01   /* +base, 8 bits (16 bit total) */
#define SID_REG_PW_LO        0x02   /* +base, 8 bits */
#define SID_REG_PW_HI        0x03   /* +base, low 4 bits */
#define SID_REG_CONTROL      0x04   /* +base */
#define SID_REG_ATTACK_DECAY 0x05   /* +base, AD = (A<<4)|D */
#define SID_REG_SUSTAIN_REL  0x06   /* +base, SR = (S<<4)|R */

#define SID_REG_FC_LO        0x15   /* filter cutoff bits 2..0 */
#define SID_REG_FC_HI        0x16   /* filter cutoff bits 10..3 */
#define SID_REG_RES_FILT     0x17   /* (resonance<<4) | routing */
#define SID_REG_MODE_VOL     0x18   /* filter mode | volume */

/* Control register bits */
#define SID_CTL_GATE         0x01
#define SID_CTL_SYNC         0x02
#define SID_CTL_RING         0x04
#define SID_CTL_TEST         0x08
#define SID_CTL_TRI          0x10
#define SID_CTL_SAW          0x20
#define SID_CTL_PULSE        0x40
#define SID_CTL_NOISE        0x80
#define SID_CTL_WAVE_MASK    0xF0

/* 0x17 low bits: which sources enter the filter */
#define SID_FILT_V1          0x01
#define SID_FILT_V2          0x02
#define SID_FILT_V3          0x04
#define SID_FILT_EXT         0x08

/* 0x18 bits 4..7: filter mode and voice-3 mute.
 * NOTE: bit4 is the low-pass mode bit, not a volume bit. */
#define SID_MODE_LP          0x10
#define SID_MODE_BP          0x20
#define SID_MODE_HP          0x40
#define SID_MODE_V3OFF       0x80
#define SID_VOLUME_MASK      0x0F

#define SID_PW_MAX           4095u

/* ------------------------------------------------------------------ */
/* pitch                                                               */
/* ------------------------------------------------------------------ */

/* 128 entries, MIDI note 0..127, computed for SID_PHI2_HZ_DEFAULT.
 * freq_reg = round(440 * 2^((note-69)/12) * 2^24 / PHI2) */
extern const uint16_t sid_freq_table[128];

uint16_t sid_freq_from_note(uint8_t midi_note);

/* Same formula with an arbitrary PHI2, for boards with a different can. */
uint16_t sid_freq_from_hz(double hz, double phi2_hz);

/* 6581/8580 nominal ADSR times in milliseconds, index 0..15. Index 0 is the
 * fastest, not "off". */
extern const uint16_t sid_adsr_attack_ms[16];
extern const uint16_t sid_adsr_decay_ms[16];    /* also used for release */

/* ------------------------------------------------------------------ */
/* register access                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t reg;
    uint8_t data;
} sid_reg_t;

/* How a register write is turned into bus cycles.
 *
 * SID_WRITE_TWO_PHASE (default) - the pattern that is actually proven on this
 *   hardware, copied from the YM7129 driver:
 *       spfm_write_raw([slot,0x80,addr, slot,0x81,data])
 *       spfm_flush()
 *       sleep(write_delay_us)          // default 90 us, like YM7129
 *   Address phase and data phase are two separate firmware commands, so the
 *   host fully controls the gap and nothing overlaps in the USB pipe.
 *
 * SID_WRITE_TWO_PHASE_NOP - same 6 bytes, but the gap is filled with NOP ticks
 *   inside the same batch instead of a host sleep. Only useful for probing.
 *
 * SID_WRITE_PATH_A - one 4-byte command, firmware generates both bus cycles
 *   back to back. Cheapest, but it is the mode that produced only crackle on
 *   the real board, so it is not the default. */
typedef enum {
    SID_WRITE_BURST = 0,        /* one Path A record: firmware emits both
                                 * cycles back to back, no host timing */
    SID_WRITE_TWO_PHASE,        /* two records, host sleep between them */
    SID_WRITE_TWO_PHASE_NOP,    /* two records, hardware NOP ticks between */
    SID_WRITE_PATH_A_SOFT,      /* Path A + host sleep after every write */
    SID_WRITE_SPLIT             /* address repeated A times, then gap, then
                                 * data repeated D times, all in one flush */
} sid_write_mode_t;

/* Address-phase and data-phase repeat counts for SID_WRITE_SPLIT.
 *
 * The two phases have different failure modes, so they get separate counters:
 *   address repeats guard against the 374 not latching the register number
 *   data repeats guard against the SID not seeing a PHI2 edge
 * Repeating the address is harmless (the latch just reloads), and repeating
 * the data is harmless too (the last write wins), so both can be pushed high
 * without corrupting anything. */
void     sid_set_addr_repeat(uint32_t n);
uint32_t sid_get_addr_repeat(void);
void     sid_set_data_repeat(uint32_t n);
uint32_t sid_get_data_repeat(void);

/* Per-write host delay in microseconds for SID_WRITE_TWO_PHASE. Default 90,
 * matching the YM7129 driver (its chip needs 89.2 us between writes). */
void     sid_set_write_delay(uint32_t us);
uint32_t sid_get_write_delay(void);

/* Send every register write N times in a row. Default 1.
 *
 * Why this exists: the SID only latches data on a PHI2 rising edge while /CS
 * and R/W are both low. The /WR pulse generated by the firmware is short
 * (~100 ns), while PHI2 at 1 MHz has a rising edge only once per microsecond,
 * so a single pulse lands in the sampling window maybe one time in ten. Every
 * other write is silently lost, which sounds like crackle rather than a note.
 *
 * Repeating an identical write is harmless (SID registers are plain latches)
 * and stretches the wall-clock window until it covers several PHI2 edges, so
 * at least one of the N attempts is guaranteed to land. */
void     sid_set_write_repeat(uint32_t n);
uint32_t sid_get_write_repeat(void);

void sid_set_write_mode(sid_write_mode_t mode);
sid_write_mode_t sid_get_write_mode(void);

/* NOP ticks inserted between the two phases in PHASED modes (1 tick is one
 * firmware loop iteration, roughly a byte time at 1.5 Mbaud). */
void     sid_set_phase_gap(uint32_t ticks);
uint32_t sid_get_phase_gap(void);

/* Random extra NOP ticks added on top of the phase gap, in [0, max_extra].
 *
 * Why this is not cosmetic: one byte at 1.5 Mbaud takes 6.667 us, and PHI2 is
 * 1 us, so a record of 3+gap+3 bytes is an exact multiple of the PHI2 period
 * whenever (6+gap) is divisible by 3. gap=0 and gap=3 both lock every retry
 * onto the same PHI2 phase, so N retries behave like a single retry: either
 * all of them land or none do. That is why repeat=12 produced one fixed wrong
 * pitch while repeat=1 occasionally got it right.
 *
 * Randomising the gap walks the phase, so successive attempts sample
 * different points of the PHI2 period and the hit rate behaves like N
 * independent tries instead of one. */
void     sid_set_gap_random(uint32_t max_extra);
uint32_t sid_get_gap_random(void);

/* One SID register write. `reg` is masked to 0x1F; the driver never emits an
 * address outside the 25 real registers, but the mask is also what keeps bit7
 * of the address byte at 0. */
void sid_write(uint8_t slot, uint8_t reg, uint8_t data);

/* Append several writes; same as calling sid_write in a loop but allows the
 * caller to reason about the whole burst. */
void sid_write_batch(uint8_t slot, const sid_reg_t *regs, size_t count);

/* Write 0 to 0x00..0x18. Needed at power-up because the 374 outputs are
 * undefined until the first address phase and the SID itself has no reset
 * value guarantee for the control registers. */
void sid_clear_all_regs(uint8_t slot);

/* ---- write tracing (for the smoke test and the GUI log) ---- */
typedef void (*sid_write_hook_t)(uint8_t slot, uint8_t reg, uint8_t data, void *user);
void sid_set_write_hook(sid_write_hook_t hook, void *user);

/* ---- buffering ---- */
/* Nothing reaches USB until this is called (or the 64 KiB buffer fills). */
bool sid_flush(void);

/* ------------------------------------------------------------------ */
/* chip-level setup                                                    */
/* ------------------------------------------------------------------ */

/* Clear all registers, set volume, and leave voice 3 muted-free.
 * After this the chip is silent but ready: volume != 0, no gate set. */
void sid_init(uint8_t slot, uint8_t volume);

void sid_set_volume(uint8_t slot, uint8_t volume);       /* 0..15 */

/* mode = combination of SID_MODE_* / SID_MODE_V3OFF, or 0 for "no filter". */
void sid_set_mode(uint8_t slot, uint8_t mode);

/* cutoff is 11 bits (0..2047). The datasheet range is split as
 * 0x15 = cutoff & 7, 0x16 = cutoff >> 3. */
void sid_set_filter_cutoff(uint8_t slot, uint16_t cutoff);

/* resonance 0..15, routing = combination of SID_FILT_*. */
void sid_set_filter(uint8_t slot, uint16_t cutoff, uint8_t resonance, uint8_t routing);

/* ------------------------------------------------------------------ */
/* voices                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    bool     active;
    uint8_t  note;
    uint8_t  wave;      /* SID_CTL_* waveform bits only, no gate */
    uint8_t  ad;
    uint8_t  sr;
    uint16_t pw;
    const char *name;
} sid_voice_t;

/* Voice index 0..2. */
const sid_voice_t *sid_get_voice(uint8_t slot, uint8_t voice);

/* Set frequency/PWM/ADSR/control without gating: prepares a voice. */
void sid_voice_config(uint8_t slot, uint8_t voice, uint16_t freq, uint16_t pw,
                      uint8_t wave, uint8_t ad, uint8_t sr);

/* Full note start: frequency, PWM, ADSR, then control = wave|GATE.
 * Ordering matters: gate must be the last write so the envelope starts from a
 * defined frequency and pulse width. */
void sid_note_on(uint8_t slot, uint8_t voice, uint8_t midi_note, uint8_t wave,
                 uint8_t ad, uint8_t sr);

void sid_note_off(uint8_t slot, uint8_t voice);
void sid_note_kill(uint8_t slot, uint8_t voice);     /* gate=0 and waveform=0 */

void sid_set_pulse(uint8_t slot, uint8_t voice, uint16_t pw);
void sid_set_freq(uint8_t slot, uint8_t voice, uint16_t freq_reg);
void sid_set_wave(uint8_t slot, uint8_t voice, uint8_t wave);

#ifdef __cplusplus
}
#endif

#endif /* SID6581_H */
