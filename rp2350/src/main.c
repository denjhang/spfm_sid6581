/*
 * RPFM Firmware — HID Protocol
 * RP2350A → PIO → SPFM bus → YM2413/YM2151/AY8910
 *
 * USB HID 64B 帧通信，天然流控，解决 CDC 数据溢出死机问题。
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/bootrom.h"
#include "pico/multicore.h"
#include "tusb.h"
#include "spfm_bus.pio.h"
#include "pio_cs.pio.h"
#include "ws2812.pio.h"
#include "phi2.pio.h"
#include "hardware/timer.h"
#include "protocol.h"
#include "command_buffer.h"

// ========== Pin Definitions ==========
#define PIN_BUS_BASE  0
#define PIN_CS_BASE   12    /* CS0# = GPIO12, CS1# = GPIO13 (spare) */
#define PIN_DIR       11    /* 74LVC245 U1 pin1: 1 = MCU -> bus */
#define PIN_PHI2      14    /* SID pin 8, PIO-generated 985248 Hz */
#define PIN_IC        21
#define PIN_LED       25
#define PIN_WS2812    16    /* measured: 22 has no LED on this board */
#define NUM_WS_LEDS   4

// ========== PIO State ==========
static PIO s_bus_pio = pio0;
static uint s_bus_sm = 0;
static uint s_ay_sm = 0;      // AY8910 dedicated SM (slower clock)
static PIO s_cs_pio = pio1;
static uint s_cs_sm = 0;
static PIO s_ws_pio = pio1;
static uint s_ws_sm = 0;
static PIO s_phi_pio = pio0;
static uint s_phi_sm = 0;
static bool s_ws_ready = false;
static uint32_t s_ws_leds[NUM_WS_LEDS] = {0};
static bool s_led_state = false;
static absolute_time_t s_led_next = {0};

// ========== Command Buffer ==========
static cmd_buf_t s_cmd;

// ========== Protocol State ==========
static volatile uint8_t s_last_seq = 0;
static volatile uint8_t s_status = 0;
volatile uint8_t s_ay_delay_100ns = 10;  // AY8910 /WR pulse delay (100ns units), default 1µs

// VGM player (needs s_status, cs_select, write_reg_ay — declared below via forward decls in vgm_player.h)
#include "vgm_player.h"

// Deferred write queue (HID callback → main loop)
#define DEFERRED_Q_SIZE 16
#define DQF_AY 0x01  // AY8910 fast timing
static volatile uint8_t s_deferred_q[DEFERRED_Q_SIZE][3];
static volatile uint8_t s_deferred_flags[DEFERRED_Q_SIZE];
static volatile uint8_t s_deferred_head = 0;
static volatile uint8_t s_deferred_tail = 0;

static inline void deferred_push(uint8_t slot, uint8_t addr, uint8_t data, uint8_t flags) {
    uint8_t next = (s_deferred_head + 1) % DEFERRED_Q_SIZE;
    if (next == s_deferred_tail) return;
    s_deferred_q[s_deferred_head][0] = slot;
    s_deferred_q[s_deferred_head][1] = addr;
    s_deferred_q[s_deferred_head][2] = data;
    s_deferred_flags[s_deferred_head] = flags;
    s_deferred_head = next;
}

static inline bool deferred_pop(uint8_t *slot, uint8_t *addr, uint8_t *data, uint8_t *flags) {
    if (s_deferred_head == s_deferred_tail) return false;
    *slot = s_deferred_q[s_deferred_tail][0];
    *addr = s_deferred_q[s_deferred_tail][1];
    *data = s_deferred_q[s_deferred_tail][2];
    *flags = s_deferred_flags[s_deferred_tail];
    s_deferred_tail = (s_deferred_tail + 1) % DEFERRED_Q_SIZE;
    return true;
}

// ========== PIO Init ==========

static void pio_bus_init(void) {
    uint offset = pio_add_program(s_bus_pio, &spfm_bus_write_program);
    s_bus_sm = pio_claim_unused_sm(s_bus_pio, true);
    spfm_bus_write_program_init(s_bus_pio, s_bus_sm, offset, PIN_BUS_BASE, PIN_PHI2);

    // The AY8910 state machine is deliberately NOT started: it runs the same
    // program on the same 11 pins, so a second enabled SM would fight this one
    // for the bus. SID writes all go through s_bus_sm.
}

#define CS_IDLE 0x03

static inline uint32_t cs_pack(uint8_t slot) {
    return CS_IDLE & ~(1u << slot);
}

static inline void pio_cs_put(PIO pio, uint sm, uint32_t val) {
    pio_sm_put_blocking(pio, sm, val);
    while (!pio_sm_is_tx_fifo_empty(pio, sm)) {}
}

static void pio_cs_init(void) {
    uint offset = pio_add_program(s_cs_pio, &cs_out_program);
    s_cs_sm = pio_claim_unused_sm(s_cs_pio, true);
    cs_out_program_init(s_cs_pio, s_cs_sm, offset, PIN_CS_BASE);
    pio_cs_put(s_cs_pio, s_cs_sm, CS_IDLE);
}

static inline void cs_select(uint8_t slot) {
    pio_cs_put(s_cs_pio, s_cs_sm, cs_pack(slot));
}

// ========== WS2812: write-density meter ==========
//
// Four LEDs, one per SID voice, each with a FIXED colour, brightness driven by
// how many register writes that voice received in the last tick:
//
//     LED0 voice 1 green   LED1 voice 2 blue   LED2 voice 3 yellow   LED3 $D418 red
//
// This is RPFM's own assignment (src/rpfm/main.c:42):
//
//     0x006600  0x000066  0x666600  0x660000
//
// How to read those literals: the PIO shifts left / MSB-first with a 24-bit
// autopull threshold (ws2812.pio, `sm_config_set_out_shift(&c, false, true, 24)`),
// and every word is pushed as `value << 8`, so the FIRST byte on the wire is
// bits 23:16 of the value. A WS2812 wants green first, therefore the literal is
// 0x00GGRRBB - high byte green, middle red, low blue - and RPFM's four values
// are green, blue, yellow, red in exactly that order. urgb_u32() below is
// pico-examples' helper and makes the packing explicit.
//
// How RPFM drives them (the thing to copy): ws_led_on(slot) writes
// cs_colors[slot] verbatim and arms a 50 ms timer; ws_led_off_all() clears the
// strip when it expires. So the original has NO brightness control at all - one
// fixed literal per LED, on or off. What is added here is only the middle
// ground: instead of on/off, the same fixed colour is shown at a level that
// follows the write count. Nothing is ever mixed, so no LED can drift toward
// white.
//
// Brightness ceiling: led_test.c in this tree uses 0x18 and says so ("流水灯压到
// 0x18 左右不刺眼"), but that was with one or two LEDs lit at a time. Here all
// four can be lit at once, so the ceiling is LED_MAX below and it is applied
// unconditionally to every channel of every LED on every frame - there is no
// path that can exceed it, and no addition that can stack past it.
//
// Two rules kept from RPFM and from led_test.c:
//
//   1. ws2812_update() pushes four words and returns. It does NOT wait. The
//      frame gap the strip needs (>=280 us) comes from led_update() being called
//      every WS_TICK_MS = 20 ms, sixty times longer than that. Blocking inside
//      the function stalls the main loop, which feeds tud_task() and the SID
//      event queue.
//   2. led_update() must also be called from the deadline wait loop in main(),
//      because during playback that loop is where the CPU actually sits.

#define WS_TICK_MS      20      /* meter refresh = frame gap */
#define LED_MAX         0x10    /* hard per-channel ceiling, all four lit at once */
/* Writes per 20 ms tick that mean "full brightness". The bus is PHI2-clocked
 * now, so a tick can carry hundreds of writes where it used to carry a handful;
 * 24 = 1200 writes/s for one voice is a hard-driven channel, and the densest
 * tune (4500/s across all four) pins the busiest LED at full. */
#define LED_FULL_HITS   24

/* SID register bits the LED layer needs. The firmware does not link
 * src/sid6581.c (that is the SPFM host driver), so the handful of constants it
 * shares are repeated here rather than pulling in the whole header. */
#define SID_REG_FILT_LO 0x15    /* 0x15..0x18 are the filter/volume block */

/* 0x00GGRRBB, pico-examples' urgb_u32(): green in bits 23:16 because that byte
 * goes out first. */
static uint32_t urgb_u32(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)g << 16) | ((uint32_t)r << 8) | (uint32_t)b;
}

/* Fixed hue per LED, RPFM's order: green, blue, yellow, red. */
static uint32_t s_ws_col[4];
/* Incremented on Core 1 (write_reg_sid), read and cleared on Core 0
 * (led_update). A counter that lands mid-clear can lose one count; for a
 * brightness meter that is invisible, and making it exact would cost a lock on
 * the hot path. Everything else in this block is Core 0 only. */
static volatile uint16_t s_ws_hits[4];
static uint8_t  s_ws_level[4];       /* level currently shown, 0..LED_MAX */
static absolute_time_t s_ws_tick = {0};

/* The only place a level becomes a colour. Clamps each channel to LED_MAX, so
 * the ceiling holds no matter what the caller computed. */
static uint32_t ws_frame(uint32_t col, uint32_t level)
{
    if (level > LED_MAX) level = LED_MAX;
    uint32_t g = ((col >> 16) & 0xFF) * level / LED_MAX;
    uint32_t r = ((col >>  8) & 0xFF) * level / LED_MAX;
    uint32_t b = ( col        & 0xFF) * level / LED_MAX;
    if (g > LED_MAX) g = LED_MAX;
    if (r > LED_MAX) r = LED_MAX;
    if (b > LED_MAX) b = LED_MAX;
    return (g << 16) | (r << 8) | b;
}

/* Called by write_reg_sid() on Core 1 for every write that reached the chip.
 * Only counts; the picture is composed on Core 0 in led_update(). */
static void ws_sid_activity(uint8_t addr, uint8_t data)
{
    (void)data;
    if (addr > 0x18) return;

    int led;
    if (addr >= SID_REG_FILT_LO) {
        led = 3;                   /* filter + volume block */
    } else {
        led = addr / 7;            /* 0x00-0x06 v1, 0x07-0x0D v2, 0x0E-0x14 v3 */
        if (led > 2) led = 2;
    }
    uint16_t h = s_ws_hits[led];
    if (h < 0xFFFF) s_ws_hits[led] = (uint16_t)(h + 1);
}

/* Four words, then return - byte for byte RPFM's ws2812_update(). See rule 1
 * above for why nothing waits here. */
static void ws2812_update(void) {
    for (int i = 0; i < NUM_WS_LEDS; i++)
        pio_sm_put_blocking(s_ws_pio, s_ws_sm, s_ws_leds[i] << 8u);
}

static inline void ws_led_off_all(void);

static void ws2812_init(void) {
    uint offset = pio_add_program(s_ws_pio, &ws2812_program);
    s_ws_sm = pio_claim_unused_sm(s_ws_pio, true);
    ws2812_program_init(s_ws_pio, s_ws_sm, offset, PIN_WS2812, 800000, false);

    s_ws_col[0] = urgb_u32(0, LED_MAX, 0);              /* green  */
    s_ws_col[1] = urgb_u32(0, 0, LED_MAX);              /* blue   */
    s_ws_col[2] = urgb_u32(LED_MAX, LED_MAX, 0);        /* yellow */
    s_ws_col[3] = urgb_u32(LED_MAX, 0, 0);              /* red    */

    for (int i = 0; i < NUM_WS_LEDS; i++) {
        s_ws_leds[i] = 0;
        s_ws_hits[i] = 0;
        s_ws_level[i] = 0;
    }
    ws2812_update();
    s_ws_tick = make_timeout_time_ms(WS_TICK_MS);
}

/* PHI2 source: PIO on GPIO14, 985248 Hz (PAL) / 1022727 Hz (NTSC).
 *
 * The clock comes from the emulator's own master clock, not from a 1 MHz can.
 * A 1 MHz crystal would put every PAL tune 25.7 cents sharp; a PIO divider of
 * 150e6/(2*985248) = 76.123 lands on 985272 Hz, which is +0.04 cents. */
#define USE_PIO_PHI2 1

/* Switch the clock without re-adding the program.
 *
 * pio_add_program() appends to the PIO's instruction memory and has no
 * counterpart that frees it while an SM is running, so calling it on every
 * CMD_PHI2 would exhaust the 32-instruction space after a dozen track changes
 * and then hand back a garbage offset. Re-initialising the same offset only
 * reloads the divider and the pin mapping. */
static uint s_phi_off = 0;
static bool s_phi_ready = false;

static void phi2_set(float hz) {
    if (!s_phi_ready) return;
    pio_sm_set_enabled(s_phi_pio, s_phi_sm, false);
    phi2_program_init(s_phi_pio, s_phi_sm, s_phi_off, PIN_PHI2, hz);
}

/* Start the clock. The GPIO-first blink that used to live here has been
 * removed: it ran for six seconds before tud_init(), so USB never enumerated
 * until it finished and the host player could not find the board. The pin is
 * proven; the blink belongs in the bring-up notes, not in the boot path. */
static void phi2_init(void) {
#if !USE_PIO_PHI2
    return;
#endif
    s_phi_off = pio_add_program(s_phi_pio, &phi2_gen_program);
    s_phi_sm = pio_claim_unused_sm(s_phi_pio, true);
    phi2_program_init(s_phi_pio, s_phi_sm, s_phi_off, PIN_PHI2, PHI2_HZ_PAL);
    s_phi_ready = true;
}

static void led_update(void)
{
    /* Heartbeat on the plain LED, unchanged. */
    if (time_reached(s_led_next)) {
        s_led_state = !s_led_state;
        gpio_put(PIN_LED, s_led_state);
        s_led_next = make_timeout_time_ms(500);
    }

    if (!time_reached(s_ws_tick)) return;
    s_ws_tick = make_timeout_time_ms(WS_TICK_MS);

    /* Level is this tick's write count, scaled so LED_FULL_HITS writes means
     * full. It holds for one tick and then drops: a note that is still being
     * driven keeps its brightness because the voice keeps receiving writes,
     * and an idle voice goes dark on the next tick. No accumulation, so there
     * is nothing that can creep upward. */
    int changed = 0;
    for (int i = 0; i < NUM_WS_LEDS; i++) {
        uint32_t lvl = (uint32_t)s_ws_hits[i] * LED_MAX / LED_FULL_HITS;
        if (lvl > LED_MAX) lvl = LED_MAX;
        s_ws_hits[i] = 0;
        s_ws_level[i] = (uint8_t)lvl;

        uint32_t want = lvl ? ws_frame(s_ws_col[i], lvl) : 0;
        if (s_ws_leds[i] != want) { s_ws_leds[i] = want; changed = 1; }
    }
    if (changed && s_ws_ready) ws2812_update();
}

static inline void ws_led_off_all(void) {
    for (int i = 0; i < NUM_WS_LEDS; i++) s_ws_leds[i] = 0;
    ws2812_update();
}


// ========== Chip Writes ==========

static inline void write_reg_ym(uint8_t slot, uint8_t addr, uint8_t data) {
    cs_select(slot);
    ym2413_pio_write_reg(s_bus_pio, s_bus_sm, addr, data);
}

static inline void write_reg_ay(uint8_t slot, uint8_t addr, uint8_t data) {
    cs_select(slot);
    ay8910_pio_write_reg(s_bus_pio, s_ay_sm, addr, data);
}

/* How many PHI2 periods the data phase is held low. This is the /CS (and R/W)
 * assertion window, and the SID needs it long enough to actually latch.
 *
 * 12 periods = 12.2 us at PAL. The number to beat is the old 20 us busy_wait,
 * whose digi drums were perfect; 12 is the same order and is expressed in chip
 * clock cycles instead of a host constant, so it scales with PAL/NTSC and lands
 * on a whole number of clock periods.
 *
 * The reason this knob exists at all: with a one-period window (the first
 * USBSID-style version) ordinary PSID improved but digi drums degenerated into
 * noise. A drum is 591227 $D418 writes at a median 181 us spacing and the sound
 * is those spacings; every write has to land, so the window cannot be shortened
 * just because the spacing got better. */
#define SID_HOLD_CYCLES_DEFAULT 12
#define SID_HOLD_CYCLES_MAX     32

static volatile uint32_t s_sid_hold = SID_HOLD_CYCLES_DEFAULT;

/* SID 6581 register write.
 *
 *   word 1  A0=0 D=addr WR#=1   address setup
 *   word 2  A0=0 D=addr WR#=0   LE = NOR(A0,/WR) rises -> 374 latches addr
 *   word 3  A0=0 D=addr WR#=1
 *   word 4  A0=1 D=data WR#=1   data setup
 *   word 5..N  A0=1 D=data WR#=0  TRIG_n low AND R/W low -> SID latches,
 *                                 held for s_sid_hold PHI2 periods
 *   word     A0=1 D=data WR#=1   release
 *   idle     bus released
 *
 * CS0# is held low for the whole session, so TRIG_n = !A0 | /WR | 0 needs both
 * A0 and /WR low, i.e. only the data-phase pulse writes the chip. That also
 * means the CS term is doing no work: cs_select(0) is called but never undone,
 * so this pin is a constant. Kept because the board is wired that way;
 * docs/直驱还是锁存_RP2350接SID审计.md measures what removing it would buy.
 *
 * No host sleep anywhere: the state machine waits for a PHI2 edge before
 * releasing each word (spfm_bus.pio), so every interval is a whole number of
 * chip clock periods. */
static inline void write_reg_sid(uint8_t addr, uint8_t data) {
    uint8_t a = (uint8_t)(addr & 0x1F);

    cs_select(0);
    sid_pio_write_reg(s_bus_pio, s_bus_sm, a, data, s_sid_hold);

    /* Counts only: this runs thousands of times a second and a WS2812 frame
     * costs ~120 us of PIO time. led_update() composes the picture. */
    if (s_ws_ready) ws_sid_activity(a, data);
}

/* Reset leaves volume (0x18) at 0. Every C64 tune's init writes $D418, so the
 * host stream supplies the volume; leaving it at 0 here is what keeps a bare
 * board silent. */
static void sid_clear_all(void) {
    for (uint8_t r = 0x00; r <= 0x18; r++) write_reg_sid(r, 0x00);
}

/* There used to be a boot-time arpeggio/chord self-test here. It is gone: the
 * board now boots silent and waits for the host, because a board that starts
 * playing the moment it is powered is unusable next to anything else. The SID
 * still gets cleared so it cannot hold whatever the previous run left in the
 * 374's latches. */

// ========== SID event queue: Core 0 produces, Core 1 consumes ==========
//
// This is RPFM's split, applied to SID events instead of VGM bytes
// (src/rpfm/main.c: the HID callback writes into cmd_buf on Core 0, and
// core1_vgm_main drains it). The reason is a hard number: write_reg_sid()
// spends 7 * 20 us = 140 us on the bus, and the densest tune in this
// collection issues ~4500 writes/s (Turbo Outrun: 733634 writes / 162.6 s).
// That is 63% of one core, and the busy frames go well past 100% - so doing it
// on Core 0 starves tud_task() and the tune drags. Core 1 has nothing else to
// do; moving the playback loop there makes Core 0 purely a USB endpoint, which
// is exactly how RPFM keeps its FM playback smooth.
//
// The queue is the same shape as cmd_buf_t: single producer, single consumer,
// volatile head/tail, power-of-two mask. Head is only written on Core 0, tail
// only on Core 1, so no lock is needed on this chip.

#define SID_Q_SIZE 8192   // power of two; 4 bytes each = 32 KiB

typedef struct {
    uint16_t dt_us;
    uint8_t  addr;
    uint8_t  data;
} sid_ev_t;

static sid_ev_t s_sid_q[SID_Q_SIZE];
static volatile uint32_t s_sid_q_head = 0;   // written by Core 0
static volatile uint32_t s_sid_q_tail = 0;   // written by Core 1
static uint64_t s_sid_deadline  = 0;
static bool     s_sid_need_anchor = true;    // next event becomes the clock origin

/* addr 0xFF is a wait-only event: the host could not express a gap longer than
 * the 16-bit delta field, so it splits it into a chain of these. They advance
 * the deadline and write nothing. */
#define SID_EV_WAIT_ONLY 0xFF

/* How far wall time may run past the deadline on an empty queue before the
 * timeline is re-anchored onto the present. One PAL video frame is 19.95 ms and
 * a digi burst is one write every ~137 us, so a gap of 20 ms is silence, never
 * a note. See the starved branch in core1_sid_main(). */
#define SID_STARVE_GRACE_US 20000

static inline bool sid_q_push(uint16_t dt, uint8_t addr, uint8_t data) {
    uint32_t next = (s_sid_q_head + 1) & (SID_Q_SIZE - 1);
    if (next == s_sid_q_tail) return false;
    s_sid_q[s_sid_q_head].dt_us = dt;
    s_sid_q[s_sid_q_head].addr  = addr;
    s_sid_q[s_sid_q_head].data  = data;
    s_sid_q_head = next;
    return true;
}

static inline bool sid_q_pop(sid_ev_t *out) {
    if (s_sid_q_head == s_sid_q_tail) return false;
    *out = s_sid_q[s_sid_q_tail];
    s_sid_q_tail = (s_sid_q_tail + 1) & (SID_Q_SIZE - 1);
    return true;
}

static inline uint32_t sid_q_used(void) {
    return (s_sid_q_head - s_sid_q_tail) & (SID_Q_SIZE - 1);
}

/* Playback gate. RPFM's host fills a quarter of the firmware buffer before it
 * sends CMD_VGM_START, and only then does Core 1 begin consuming; the buffer is
 * refilled before it can run dry. That prefill is why the original has no
 * start-of-tune instability at all.
 *
 * Here the same idea, sized in events: the host pushes PREFILL events before
 * CMD_SID_START, so when consumption begins there is already a full queue to
 * play out and the host has a wide margin to keep refilling. Without it the
 * board starts on an almost empty queue, plays the first burst at whatever rate
 * events happen to arrive, and only settles once the queue is deep enough -
 * heard as the first second or two running fast. */
static volatile bool s_sid_running = false;

/* Reset request, raised by the HID callback on Core 0 and serviced by Core 1. */
static volatile bool s_sid_reset_req = false;

/* Stop request: shut the gate, drop what is still queued, forget the timeline.
 * Lightweight - no IC# pulse - so it can run at every track boundary.
 *
 * The event head is written by Core 0 only, so Core 0 records where the queue
 * ended at the moment the stop was parsed and Core 1 truncates *to that mark*.
 * Core 1 must not simply cut to its own current head: the host starts pushing
 * the next tune's prefill immediately after the stop frame, and cutting to the
 * live head would throw that prefill away. */
static volatile bool     s_sid_stop_req = false;
static volatile uint32_t s_sid_stop_mark = 0;

/* Start request. This used to be applied by Core 0 inside the HID callback,
 * which was a race: STOP is only a flag that Core 1 services later, so if
 * START ran first Core 1 would then take the stop branch and set
 * s_sid_running back to false - closing the gate it had just opened and
 * stranding whatever the host pushed after the stop. That is the "press q and
 * the notes keep ringing" report: the host's mute is STOP, clear, START, and
 * the clear is what got stranded.
 *
 * Making START a request too puts every control transition on Core 1, which
 * services reset -> stop -> start in that order in a single pass. The host's
 * frames always arrive in that order, so the end state is well defined. */
static volatile bool s_sid_start_req = false;

/* Is a control transition pending? Checked in the deadline wait so a stop is
 * not delayed by a long inter-event gap (a tune can leave tens of ms between
 * two writes, and Core 1 would otherwise sit in that spin loop). */
static inline bool sid_ctrl_pending(void) {
    return s_sid_reset_req || s_sid_stop_req || s_sid_start_req;
}

static void sid_q_request_clear(void) {
    s_sid_reset_req = true;
}

// ========== IC# Reset ==========

/* Only Core 1 calls this: it is a 100 ms pulse and Core 1 is the core that owns
 * the bus. Core 0 must keep running tud_task() meanwhile, which is exactly why
 * this became a Core 1 job - the old version sat on Core 0 and stopped the USB
 * stack for 100 ms on every track change. */
static void bus_ic_reset(void) {
    gpio_put(PIN_IC, 0);
    sleep_ms(50);
    gpio_put(PIN_IC, 1);
    sleep_ms(50);
}

// ========== GPIO Init ==========

static void gpio_init_all(void) {
    /* 74LVC245 data bus direction: high = RP2350A drives the 5 V side.
     * Kept high for the whole session; only a read would flip it. */
    gpio_init(PIN_DIR);
    gpio_set_dir(PIN_DIR, GPIO_OUT);
    gpio_put(PIN_DIR, 1);

    gpio_init(PIN_IC);
    gpio_set_dir(PIN_IC, GPIO_OUT);
    gpio_put(PIN_IC, 1);
    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, 0);
}

// ========== HID Callbacks ==========

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize) {
    (void)instance; (void)report_id; (void)report_type;

    if (bufsize < 4) return;

    uint8_t cmd = buffer[0];
    uint8_t seq = buffer[1];
    uint8_t len = buffer[2];

    if (len > 60 || bufsize < (uint16_t)(3 + len + 1)) return;

    // CRC check
    if (rpfm_crc8(buffer, 63) != buffer[63]) {
        s_status |= STATUS_ERROR;
        return;
    }

    s_last_seq = seq;
    s_status &= ~STATUS_ERROR;

    const uint8_t *payload = buffer + 3;

    switch (cmd) {
    case CMD_WRITE_REG: {
        for (uint8_t i = 0; i + 2 < len; i += 3) {
            deferred_push(payload[i], payload[i+1], payload[i+2], 0);
        }
        break;
    }
    case CMD_WRITE_AY: {
        for (uint8_t i = 0; i + 2 < len; i += 3) {
            deferred_push(payload[i], payload[i+1], payload[i+2], DQF_AY);
        }
        break;
    }
    case CMD_WRITE_TICK: {
        for (uint8_t i = 0; i < len; i++) {
            cmd_buf_write(&s_cmd, payload[i]);
        }
        break;
    }
    case CMD_VGM_DATA: {
        cmd_buf_write_buf(&s_cmd, payload, len);
        break;
    }
    case CMD_RESET: {
        if (len >= 1) {
            /* The host is changing tunes. Hand the reset to Core 1, which owns
             * the bus and the queue's tail; doing it here would touch state the
             * other core is reading. Core 1 clears the backlog, pulses IC# and
             * re-clears the registers. */
            s_sid_reset_req = true;
        }
        break;
    }
    case CMD_VGM_START: {
        // payload: [loop_offset_lo, loop_offset_hi] (relative to cmd_buf start)
        uint32_t loop_off = 0;
        if (len >= 2) loop_off = payload[0] | ((uint32_t)payload[1] << 8);
        vgm_player_start(loop_off);
        break;
    }
    case CMD_VGM_STOP: {
        vgm_player_stop();
        cmd_buf_init(&s_cmd);
        break;
    }
    case CMD_BOOTSEL: {
        reset_usb_boot(0, 0);
        break;
    }
    case CMD_SET_DELAY: {
        if (len >= 1 && payload[0] <= 200)
            s_ay_delay_100ns = payload[0];
        break;
    }
    case CMD_SET_MUTE: {
        if (len >= 2) {
            uint8_t mask = payload[0];
            int8_t solo = (int8_t)payload[1];
            vgm_player_set_mute(mask, solo);
        }
        break;
    }
    case CMD_SET_ACTIVE_CHIPS: {
        // bitmask: bit0 = SCC (0xD2)
        if (len >= 4) {
            uint32_t mask = payload[0] | ((uint32_t)payload[1] << 8) |
                            ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);
            vgm_player_set_active_chips(mask);
        }
        break;
    }
    case CMD_SET_EMU_VOLUME: {
        // [chip_id(1B), volume(1B)] — chip_id: 0xD2=SCC, 0x52=DAC
        if (len >= 2) {
            vgm_player_set_emu_volume(payload[0], payload[1]);
        }
        break;
    }
    case CMD_WRITE_SCC: {
        if (len == 0) {
            vgm_player_reset_scc();
        }
        for (uint8_t i = 0; i + 2 < len; i += 3) {
            vgm_player_write_scc(payload[i], payload[i+1], payload[i+2]);
        }
        break;
    }
    case CMD_SID_WRITE: {
        // [addr,data] x N, written immediately, in order
        for (uint8_t i = 0; i + 1 < len; i += 2)
            sid_q_push(0, payload[i], payload[i + 1]);
        break;
    }
    case CMD_SID_EVENTS: {
        // [dt_lo,dt_hi,addr,data] x N
        for (uint8_t i = 0; i + 3 < len; i += 4) {
            uint16_t dt = (uint16_t)(payload[i] | ((uint16_t)payload[i + 1] << 8));
            sid_q_push(dt, payload[i + 2], payload[i + 3]);
        }
        break;
    }
    case CMD_SID_START: {
        /* Release the gate. RPFM's CMD_VGM_START equivalent: the host has filled
         * part of the queue, so playback can begin with a full buffer behind it
         * instead of racing the first few hundred events.
         *
         * It also starts a NEW timeline, because the host only sends this after
         * pre-filling the beginning of a tune (t=0). Without that, track 2 of a
         * playlist inherits track 1's absolute deadline: by the time the new
         * prefill is popped, wall time is already seconds past it, so not one
         * of the ~1500 prefilled events waits - the opening 1.2 s of music gets
         * dumped at bus speed in ~30 ms. That is the "first second or two is
         * played too fast" report, and it only ever appeared in a playlist (a
         * single file is preceded by CMD_RESET, which anchors for it).
         *
         * Anchoring works exactly as on a fresh tune: dt==0 events are init and
         * run immediately, the first non-zero delta sets the origin to "now".
         *
         * Raised as a request rather than applied here so it cannot overtake a
         * pending STOP (see s_sid_start_req). */
        s_sid_start_req = true;
        break;
    }
    case CMD_SID_STOP: {
        /* Track boundary: close the gate, drop whatever is still queued and
         * forget the timeline. Core 1 owns the queue tail, so it does the
         * work; the mark tells it how far to cut. */
        s_sid_stop_mark = s_sid_q_head;
        s_sid_stop_req = true;
        break;
    }
    case CMD_SID_STEP: {
        /* The data-phase hold, in PHI2 periods (1..32). This is the /CS low
         * window, so it is the knob that decides whether a write lands - see the
         * comment at SID_HOLD_CYCLES_DEFAULT. */
        if (len >= 1) {
            uint32_t v = payload[0];
            if (v < 1) v = 1;
            if (v > SID_HOLD_CYCLES_MAX) v = SID_HOLD_CYCLES_MAX;
            s_sid_hold = v;
        }
        break;
    }
    case CMD_PHI2: {
#if USE_PIO_PHI2
        if (len >= 1)
            phi2_set(payload[0] ? PHI2_HZ_NTSC : PHI2_HZ_PAL);
#endif
        break;
    }
    case CMD_NOP:
    default:
        break;
    }
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type;

    if (reqlen < RPFM_FRAME_SIZE) return 0;

    memset(buffer, 0, RPFM_FRAME_SIZE);
    buffer[0] = s_last_seq;
    uint32_t used = sid_q_used();
    if (used > (SID_Q_SIZE * 3 / 4))
        s_status |= STATUS_BUF_HIGH;
    else
        s_status &= ~STATUS_BUF_HIGH;
    buffer[1] = s_status;
    buffer[2] = (uint8_t)(used & 0xFF);
    buffer[3] = (uint8_t)((used >> 8) & 0xFF);
    // Tick: current playback sample position (32-bit LE)
    uint32_t tick = s_vgm_tick;
    buffer[4] = (uint8_t)(tick & 0xFF);
    buffer[5] = (uint8_t)((tick >> 8) & 0xFF);
    buffer[6] = (uint8_t)((tick >> 16) & 0xFF);
    buffer[7] = (uint8_t)((tick >> 24) & 0xFF);

    return RPFM_FRAME_SIZE;
}

// ========== Core 1: SID playback ==========
//
// Same shape as RPFM's core1_vgm_main(): spin on the shared queue, do the
// hardware work here, never call into the USB stack. Core 0 is left free to
// service HID frames, which is what removes the drag.
//
// Idle is a tight spin, not a sleep: the queue is fed 400 ms ahead so there is
// almost always something ready, and a sleep would have to be woken to gain
// anything. RPFM spins the same way ("Tight idle — don't sleep, just spin").

static void core1_sid_main(void) {
    sid_ev_t ev;

    for (;;) {
        /* Reset requests are serviced here so head/tail stay single-writer. A
         * track change must not replay the previous tune's backlog.
         *
         * A reset supersedes any stop or start still pending: the stop mark was
         * recorded before the reset cleared the queue, so applying it afterwards
         * would move the tail backwards and resurrect discarded events. */
        if (s_sid_reset_req) {
            s_sid_reset_req = false;
            s_sid_stop_req = false;
            s_sid_start_req = false;
            s_sid_q_tail = s_sid_q_head;
            s_sid_need_anchor = true;
            s_sid_running = false;
            bus_ic_reset();
            sid_clear_all();
            continue;
        }

        /* Stop requests: drop the backlog and forget the timeline, but leave the
         * chip alone. This is what a track boundary needs; CMD_RESET also
         * pulses IC# and is far too slow to do between two files of a
         * playlist. */
        if (s_sid_stop_req) {
            s_sid_stop_req = false;
            s_sid_q_tail = s_sid_stop_mark;
            s_sid_need_anchor = true;
            s_sid_running = false;
            continue;
        }

        /* Start requests, always after the stop above: the host sends
         * STOP -> clear -> START, so servicing them in this order means the
         * clear lands on a queue that is already empty and the gate that opens
         * lasts. Applying START on Core 0 instead let it run before this stop,
         * which then closed the gate again and stranded the clear - the chip
         * kept the old volume and the notes rang on after quitting. */
        if (s_sid_start_req) {
            s_sid_start_req = false;
            s_sid_deadline = 0;
            s_sid_need_anchor = true;
            s_sid_running = true;
            continue;
        }

        /* Nothing is consumed until the host says the queue is prefilled. This
         * is RPFM's CMD_VGM_START: fill first, then play. */
        if (!s_sid_running) {
            tight_loop_contents();
            continue;
        }

        if (!sid_q_pop(&ev)) {
            /* Starved: nothing to play. Do not let the music clock run away.
             *
             * A tune can be silent for seconds and then fire a dense $D418 digi
             * burst (Great Giana Sisters RSID writes 79 registers in its first
             * second, nothing much for three more, then 7247 per second). While
             * the queue is empty the deadline cannot advance, so wall time
             * passes it. When the burst finally arrives every event's deadline
             * is already in the past and the board writes each HID batch
             * back-to-back at bus speed (15 writes in 285 us) instead of at the
             * 137 us spacing that is the drum. That is the "digi turns to mush"
             * report, and it is the same failure as a rushed opening.
             *
             * Re-anchor onto the present once the gap is real (> 20 ms, well
             * over one video frame). The next event then gets its full delta
             * measured from now, so the *spacing* the recorder intended is kept
             * even though the absolute origin moved. Only the origin moves: a
             * gap that long is silence, so nothing audible shifts. */
            if (!s_sid_need_anchor &&
                time_us_64() > s_sid_deadline + SID_STARVE_GRACE_US)
                s_sid_deadline = time_us_64();
            tight_loop_contents();
            continue;
        }

        /* Anchor the clock on the first event that carries a non-zero delta,
         * not on the first event at all.
         *
         * A tune's init burst is hundreds of writes all stamped t=0, and each
         * one costs real bus time (~18 us here). Anchoring on the first of them
         * starts the music clock before the init has finished executing, so by
         * the time the first real note arrives the deadline is already that
         * whole burst behind - and the board then rushes to catch up. That is
         * the "first second or two is played too fast" report.
         *
         * Anchoring on the first non-zero delta makes "t=0" mean what the
         * recorder meant by it: init complete. The init burst is free. */
        if (s_sid_need_anchor) {
            if (ev.dt_us == 0) {
                if (ev.addr != SID_EV_WAIT_ONLY)
                    write_reg_sid(ev.addr, ev.data);
                continue;
            }
            s_sid_deadline = time_us_64();
            s_sid_need_anchor = false;
        }

        if (ev.dt_us) {
            s_sid_deadline += ev.dt_us;
            while (time_us_64() < s_sid_deadline) {
                /* A stop or start must not wait out a long gap between two
                 * events - a tune can leave tens of ms of silence and the user
                 * pressing q expects the chip to go quiet now. Drop this event
                 * instead of writing it early: a pending request only ever
                 * means "close the gate", and firing one last note on the way
                 * out is exactly what the mute is trying to avoid. */
                if (sid_ctrl_pending()) goto next_pass;
                tight_loop_contents();
            }
        }
        if (ev.addr != SID_EV_WAIT_ONLY)
            write_reg_sid(ev.addr, ev.data);
    next_pass:;
    }
}

// ========== Main ==========

int main() {
    stdio_init_all();

    gpio_init_all();
    pio_bus_init();
    pio_cs_init();
    phi2_init();
    ws2812_init();
    cmd_buf_init(&s_cmd);
    vgm_player_init(&s_cmd, s_bus_pio, s_ay_sm);

    // Init TinyUSB device stack
    tud_init(BOARD_TUD_RHPORT);

    s_ws_ready = true;
    cs_select(0);

    /* Everything that touches the SID bus happens on Core 1 from here on, so the
     * initial reset/clear goes with it. Core 0 starts the player and then only
     * ever handles USB and the LEDs. */
    multicore_launch_core1(core1_sid_main);
    s_sid_reset_req = true;

    s_led_next = make_timeout_time_ms(500);

    /* 每次主循环都跑 tud_task()，否则 USB/HID 会死，BOOTSEL 就发不进来。
     * 板子自己不出声：所有寄存器写都来自主机事件，由 Core 1 执行。 */
    while (true) {
        tud_task();
        led_update();

        // Execute deferred register writes (still Core 0: they are YM paths)
        uint8_t slot, addr, data, flags;
        while (deferred_pop(&slot, &addr, &data, &flags)) {
            if (flags & DQF_AY)
                write_reg_ay(slot, addr, data);
            else
                write_reg_ym(slot, addr, data);
        }
    }

    return 0;
}
