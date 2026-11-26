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
    spfm_bus_write_program_init(s_bus_pio, s_bus_sm, offset, PIN_BUS_BASE);

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
#define LED_FULL_HITS   6       /* writes per tick that mean "full brightness" */

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
static uint16_t s_ws_hits[4];        /* writes seen since the last tick */
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

/* Called by write_reg_sid() for every write that reached the chip. Only counts;
 * the picture is composed in led_update(). */
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
    if (s_ws_hits[led] < 0xFFFF) s_ws_hits[led]++;
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

/* SID 6581 register write, modelled exactly on ym2413_pio_write_reg() which is
 * verified on hardware: six PIO words, a 20 us gap after each, idle word last.
 *
 *   word 1  A0=0 D=addr WR#=1   address setup
 *   word 2  A0=0 D=addr WR#=0   LE = NOR(A0,/WR) rises -> 374 latches addr
 *   word 3  A0=0 D=addr WR#=1
 *   word 4  A0=1 D=data WR#=1   data setup
 *   word 5  A0=1 D=data WR#=0   TRIG_n low AND R/W low -> SID latches
 *   word 6  A0=1 D=data WR#=1
 *   idle    bus released
 *
 * CS0# is held low for the whole session, so TRIG_n = !A0 | /WR | 0 needs both
 * A0 and /WR low, i.e. only the data-phase pulse writes the chip. */
#define SID_STEP_US 20

static inline void write_reg_sid(uint8_t addr, uint8_t data) {
    uint8_t a = (uint8_t)(addr & 0x1F);

    cs_select(0);

    pio_bus_put(s_bus_pio, s_bus_sm, pack_bus(a, 0, true, true));
    busy_wait_us_32(SID_STEP_US);
    pio_bus_put(s_bus_pio, s_bus_sm, pack_bus(a, 0, false, true));
    busy_wait_us_32(SID_STEP_US);
    pio_bus_put(s_bus_pio, s_bus_sm, pack_bus(a, 0, true, true));
    busy_wait_us_32(SID_STEP_US);

    pio_bus_put(s_bus_pio, s_bus_sm, pack_bus(data, 1, true, true));
    busy_wait_us_32(SID_STEP_US);
    pio_bus_put(s_bus_pio, s_bus_sm, pack_bus(data, 1, false, true));
    busy_wait_us_32(SID_STEP_US);
    pio_bus_put(s_bus_pio, s_bus_sm, pack_bus(data, 1, true, true));
    busy_wait_us_32(SID_STEP_US);

    pio_bus_put(s_bus_pio, s_bus_sm, BUS_IDLE);
    busy_wait_us_32(SID_STEP_US);

    /* Only record, never drive the LEDs here: this runs up to ~800 times a
     * second and each frame costs ~120 us of PIO time. led_update() composes
     * the picture on its own tick. */
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

// ========== SID event queue (host -> playback) ==========
//
// The host sends .sid register writes already stamped with a microsecond delta.
// Applying them against an absolute deadline on this side is what keeps the
// timing independent of USB jitter.

#define SID_Q_SIZE 8192   // power of two; 4 bytes each = 32 KiB

typedef struct {
    uint16_t dt_us;
    uint8_t  addr;
    uint8_t  data;
} sid_ev_t;

static sid_ev_t s_sid_q[SID_Q_SIZE];
static volatile uint32_t s_sid_q_head = 0;
static volatile uint32_t s_sid_q_tail = 0;
static bool     s_sid_host_mode = false;   // true once the host streamed events
static uint64_t s_sid_deadline  = 0;
static bool     s_sid_need_anchor = true;  // next event becomes the clock origin

/* addr 0xFF is a wait-only event: the host could not express a gap longer than
 * the 16-bit delta field, so it splits it into a chain of these. They advance
 * the deadline and write nothing. */
#define SID_EV_WAIT_ONLY 0xFF

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

/* Drop everything queued. A track change must not replay the previous tune's
 * backlog, and a reset must not leave the deadline pointing at stale time. */
static void sid_q_clear(void) {
    s_sid_q_tail = s_sid_q_head;
    s_sid_need_anchor = true;
    s_sid_deadline = time_us_64();
}

// ========== IC# Reset ==========

static void bus_ic_reset(void) {
    gpio_put(PIN_IC, 0);
    absolute_time_t t = make_timeout_time_ms(50);
    while (!time_reached(t)) { tud_task(); tight_loop_contents(); }
    gpio_put(PIN_IC, 1);
    t = make_timeout_time_ms(50);
    while (!time_reached(t)) { tud_task(); tight_loop_contents(); }
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
            /* Clear the event queue first: the host is changing tunes, and a
             * backlog from the old one must not replay into the new one. */
            sid_q_clear();
            bus_ic_reset();
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
        s_sid_host_mode = true;
        break;
    }
    case CMD_SID_EVENTS: {
        // [dt_lo,dt_hi,addr,data] x N
        for (uint8_t i = 0; i + 3 < len; i += 4) {
            uint16_t dt = (uint16_t)(payload[i] | ((uint16_t)payload[i + 1] << 8));
            sid_q_push(dt, payload[i + 2], payload[i + 3]);
        }
        s_sid_host_mode = true;
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

    // Launch VGM player on Core 1 (MegaGRRL-style cycle counter loop)
    multicore_launch_core1(core1_vgm_main);

    s_ws_ready = true;
    cs_select(0);
    bus_ic_reset();

    /* Clear the registers so the SID cannot hold whatever the previous run
     * left latched. Volume stays at 0 here - the host sets it when a tune
     * starts, so a bare board is silent. */
    sid_clear_all();

    s_led_next = make_timeout_time_ms(500);

    /* 每次主循环都跑 tud_task()，否则 USB/HID 会死，BOOTSEL 就发不进来。
     * 板子自己不出声：所有寄存器写都来自主机事件。 */
    while (true) {
        tud_task();
        led_update();

        // Apply due host events against an absolute deadline.
        if (s_sid_host_mode) {
            sid_ev_t ev;
            while (sid_q_pop(&ev)) {
                /* The first event after a reset anchors the clock: the host's
                 * deltas are relative to its start stamp, which is whenever it
                 * began pushing. Anchoring once per stream - not per drained
                 * batch - is what keeps the board's clock monotonic; the old
                 * code reset the anchor whenever the queue happened to run dry,
                 * so any underrun shortened the tune by the wait that followed. */
                if (s_sid_need_anchor) {
                    s_sid_deadline = time_us_64();
                    s_sid_need_anchor = false;
                }

                if (ev.dt_us) {
                    s_sid_deadline += ev.dt_us;
                    while (time_us_64() < s_sid_deadline) {
                        /* led_update() has to run here too. The queue is fed
                         * hundreds of ms ahead, so this wait is where the loop
                         * spends most of its time during playback; leaving it
                         * out froze the LEDs on whatever picture was composed
                         * before the first batch drained. That was the
                         * "stuck on the first LED" report. */
                        tud_task();
                        led_update();
                    }
                }
                if (ev.addr != SID_EV_WAIT_ONLY)
                    write_reg_sid(ev.addr, ev.data);
            }
        }

        // Execute deferred register writes
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
