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

// ========== WS2812: SID activity meter ==========
//
// Four LEDs, one per thing a listener can hear:
//
//     LED0 voice 1    LED1 voice 2    LED2 voice 3    LED3 $D418
//
// A voice's hue is its waveform, so the bass channel on pulse reads yellow and
// an arpeggio on saw reads blue; its brightness is the gate, so a note looks
// like a blink and a rest looks dark. $D418 tracks the volume nibble and
// flashes on every write to it - the nibble barely moves during a digi drum,
// the write rate is what moves, so the flash is what makes the drum visible.
//
// Everything is composed in led_update() on a 20 ms tick rather than inside
// write_reg_sid(). That function runs at up to ~800 writes/s and each
// ws2812_update() blocks the PIO for ~120 us, so pushing a frame per write
// would eat the timing budget the whole design exists to protect.

#define WS_TICK_MS 20

/* SID register bits the LED layer needs. The firmware does not link
 * src/sid6581.c (that is the SPFM host driver), so the handful of constants it
 * shares are repeated here rather than pulling in the whole header. */
#define SID_REG_MODE_VOL   0x18
#define SID_VOLUME_MASK    0x0F
#define SID_CTL_GATE       0x01
#define SID_CTL_TRI        0x10
#define SID_CTL_SAW        0x20
#define SID_CTL_PULSE      0x40
#define SID_CTL_NOISE      0x80

/* Max 0x28 per channel: the 0x66 the RPFM defaults used was described as
 * blinding on this board, and 0x18 was accepted. */
#define WS_LEVEL 0x28u

/* Waveform -> hue. Index order matches wave_index() below. */
static const uint32_t WAVE_RGB[4] = {
    0x002800,   /* triangle: green  */
    0x000028,   /* sawtooth: blue   */
    0x282800,   /* pulse:    yellow */
    0x280000,   /* noise:    red    */
};

#define WS_FLASH_VOICE 0x101010u
#define WS_FLASH_VOL   0x303030u

static uint8_t  s_voice_ctl[3];      /* last CONTROL write per voice */
static uint32_t s_voice_flash[3];    /* addend that decays after each write */
static uint8_t  s_vol_val;           /* last $D418 volume nibble */
static uint32_t s_vol_flash;
static absolute_time_t s_ws_tick = {0};

static uint32_t ws_scale(uint32_t rgb, uint32_t num, uint32_t den)
{
    uint32_t r = ((rgb >> 16) & 0xFF) * num / den;
    uint32_t g = ((rgb >>  8) & 0xFF) * num / den;
    uint32_t b = ( rgb        & 0xFF) * num / den;
    return (r << 16) | (g << 8) | b;
}

static uint32_t ws_add(uint32_t a, uint32_t b)
{
    uint32_t r = ((a >> 16) & 0xFF) + ((b >> 16) & 0xFF);
    uint32_t g = ((a >>  8) & 0xFF) + ((b >>  8) & 0xFF);
    uint32_t bl = (a        & 0xFF) + (b        & 0xFF);
    if (r > 0xFF) r = 0xFF;
    if (g > 0xFF) g = 0xFF;
    if (bl > 0xFF) bl = 0xFF;
    return (r << 16) | (g << 8) | bl;
}

/* SID waveform bits are not exclusive; when two are set the chip combines
 * them, and visually the brighter one wins. Noise and pulse are the two that
 * carry a drum, so they get priority over saw/triangle. */
static int wave_index(uint8_t ctl)
{
    if (ctl & SID_CTL_NOISE) return 3;
    if (ctl & SID_CTL_PULSE) return 2;
    if (ctl & SID_CTL_SAW)   return 1;
    return 0;
}

/* Called by write_reg_sid() for every write that reached the chip. */
static void ws_sid_activity(uint8_t addr, uint8_t data)
{
    if (addr == SID_REG_MODE_VOL) {
        s_vol_val = (uint8_t)(data & SID_VOLUME_MASK);
        s_vol_flash = WS_FLASH_VOL;
        return;
    }
    /* Voice n's CONTROL register is n*7 + 4 -> 0x04, 0x0B, 0x12. */
    if ((addr & 0x07) == 0x04 && (addr >> 3) < 3) {
        int v = addr >> 3;
        s_voice_ctl[v] = data;
        s_voice_flash[v] = WS_FLASH_VOICE;
    }
}

static void ws2812_update(void) {
    for (int i = 0; i < NUM_WS_LEDS; i++)
        pio_sm_put_blocking(s_ws_pio, s_ws_sm, s_ws_leds[i] << 8u);
}

static inline void ws_led_off_all(void);

static void ws2812_init(void) {
    uint offset = pio_add_program(s_ws_pio, &ws2812_program);
    s_ws_sm = pio_claim_unused_sm(s_ws_pio, true);
    ws2812_program_init(s_ws_pio, s_ws_sm, offset, PIN_WS2812, 800000, false);
    for (int i = 0; i < NUM_WS_LEDS; i++) s_ws_leds[i] = 0;
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

/* SID register bits the LED layer needs. The firmware does not link
 * src/sid6581.c (that is the SPFM host driver), so the handful of constants it
 * shares are repeated here rather than pulling in the whole header. */
#define SID_REG_MODE_VOL   0x18
#define SID_VOLUME_MASK    0x0F
#define SID_CTL_GATE       0x01
#define SID_CTL_TRI        0x10
#define SID_CTL_SAW        0x20
#define SID_CTL_PULSE      0x40
#define SID_CTL_NOISE      0x80

/* Nothing to light when the tune is silent; decay the write flashes instead. */
static void ws_decay(void)
{
    for (int i = 0; i < 3; i++)
        if (s_voice_flash[i]) s_voice_flash[i] -= s_voice_flash[i] / 3 + 1;
    if (s_vol_flash) s_vol_flash -= s_vol_flash / 3 + 1;
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

    ws_decay();

    uint32_t want[NUM_WS_LEDS];

    for (int v = 0; v < 3; v++) {
        uint8_t ctl = s_voice_ctl[v];
        uint32_t c = WAVE_RGB[wave_index(ctl)];
        /* Gate off = dark: a release tail is inaudible on an LED, and a
         * constant glow would make every channel look permanently on. */
        uint32_t body = (ctl & SID_CTL_GATE)
                      ? ws_scale(c, WS_LEVEL, WS_LEVEL)
                      : 0;
        want[v] = ws_add(body, s_voice_flash[v]);
    }

    /* $D418: brightness follows the volume nibble, flashes on each write. The
     * drum barely moves this nibble, so without the flash a digi passage looks
     * like a steady dim light. */
    want[3] = ws_add(ws_scale(0xFFFFFFu, (uint32_t)s_vol_val, 15u), s_vol_flash);

    int changed = 0;
    for (int i = 0; i < NUM_WS_LEDS; i++) {
        if (s_ws_leds[i] != want[i]) { s_ws_leds[i] = want[i]; changed = 1; }
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

/* 肉眼可见的自检：用同一条 PIO 通路，把两次写间隔拉到 250 ms。
 * A0 变成 2 Hz 方波，CS0# 也跟着翻。如果这一段 A0 的灯不动，
 * 才是 PIO 真没到引脚；如果这一段动、播放时不动，那只是 1 µs 太短看不见。 */
static void sid_visible_blink(void) {
    for (int i = 0; i < 8; i++) {
        cs_select(0);                                    /* CS0# 低 */
        pio_bus_put(s_bus_pio, s_bus_sm, pack_bus(0x00, 0, true, true));  /* A0=0 */
        sleep_ms(250);
        pio_bus_put(s_bus_pio, s_bus_sm, pack_bus(0x00, 1, true, true));  /* A0=1 */
        sleep_ms(250);
        cs_select(1);                                    /* CS0# 高 */
        sleep_ms(250);
    }
    pio_bus_put(s_bus_pio, s_bus_sm, pack_bus(0xFF, 0, true, true));
    cs_select(0);
}

/* Reset leaves volume (0x18) at 0, so nothing is audible without this. */
static void sid_clear_all(void) {
    for (uint8_t r = 0x00; r <= 0x18; r++) write_reg_sid(r, 0x00);
    write_reg_sid(0x18, 0x0F);
}

/* Four rising notes on voice 1: proves clock, address latch and data path. */
/* 密集自检音序：不需要主机，上电后一直循环。
 *
 * 声部 1：16 分音符快速琶音（每音 60 ms，含 6 次寄存器写）
 * 声部 2 + 3：同时按住五度和八度，作为持续和弦底
 * 每 8 个音符插一次噪声打击，检验 $D418 附近与快速 gate 往返
 *
 * 这些数字是故意压紧的：一拍 60 ms 内要发 6 条命令，比普通 PSID 更密。
 * 如果这里能连续响几分钟不掉音、不跑调，总线时序就算过关。
 */
static const uint16_t SEMI[12] = {
    0x0D5D, 0x0E2B, 0x0F02, 0x0FE3, 0x10CF, 0x11C6,
    0x12CA, 0x13DA, 0x14F8, 0x1625, 0x1760, 0x18AC
};

static int s_arp_n = 0;

/* 主音下方五度，和 SEMI 一一对应 */
static const uint16_t CHORD_FIFTH[12] = {
    0x08B8, 0x093B, 0x09C5, 0x0A56, 0x0AEF, 0x0B90,
    0x0C39, 0x0CEB, 0x0DA5, 0x0E68, 0x0F34, 0x1009
};

static void sid_arp_chord_test(void) {
    /* 三个声部统一包络：起音快、decay 短、sustain 中等，能听清每个音 */
    for (int v = 0; v < 3; v++) {
        uint8_t b = (uint8_t)(v * 7);
        write_reg_sid((uint8_t)(b + 2), 0x00);
        write_reg_sid((uint8_t)(b + 3), 0x08);   /* 50% 占空比 */
        write_reg_sid((uint8_t)(b + 5), 0x08);   /* attack 2ms, decay 300ms */
        write_reg_sid((uint8_t)(b + 6), 0x66);   /* sustain 6, release 240ms */
    }
    write_reg_sid(0x04, 0x40);   /* 声部1 脉冲，gate 关 */
    write_reg_sid(0x0B, 0x20);   /* 声部2 锯齿，gate 关 */
    write_reg_sid(0x12, 0x10);   /* 声部3 三角，gate 关 */

    s_arp_n = 0;
}

/* One note of the test sequence, called from the main loop so tud_task() keeps
 * running and BOOTSEL stays reachable. Returns the delay to wait afterwards. */
static uint32_t sid_arp_step(void) {
    {
        int n = s_arp_n;
        uint16_t f = SEMI[n % 12];

        /* 声部 1：琶音主音 */
        write_reg_sid(0x00, (uint8_t)(f & 0xFF));
        write_reg_sid(0x01, (uint8_t)(f >> 8));
        write_reg_sid(0x04, 0x41);            /* 脉冲 + gate on */

        /* 声部 2：下方五度，跟着走 */
        write_reg_sid(0x07, (uint8_t)(CHORD_FIFTH[n % 12] & 0xFF));
        write_reg_sid(0x08, (uint8_t)(CHORD_FIFTH[n % 12] >> 8));
        write_reg_sid(0x0B, 0x21);            /* 锯齿 + gate on */

        /* 声部 3：每 4 个音换一次高八度根音 */
        if ((n & 3) == 0) {
            uint16_t o = (uint16_t)(f * 2);
            write_reg_sid(0x0E, (uint8_t)(o & 0xFF));
            write_reg_sid(0x0F, (uint8_t)(o >> 8));
            write_reg_sid(0x12, 0x11);        /* 三角 + gate on */
        }

        /* 关 gate，保证下一个音有干净的 0->1 边沿 */
        write_reg_sid(0x04, 0x40);

        /* 每 8 个音给一次噪声打击 */
        if ((n & 7) == 7) {
            write_reg_sid(0x0E, 0x40);
            write_reg_sid(0x0F, 0x24);        /* 高频嘶声 */
            write_reg_sid(0x13, 0x08);        /* 快起音 */
            write_reg_sid(0x14, 0x33);        /* 短促衰减 */
            write_reg_sid(0x12, 0x81);        /* 噪声 + gate */
            s_arp_n++;
            return 30;
        }

        s_arp_n++;
        return 55;
    }
}

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

    /* 清寄存器 + 音量：这一步之后 SID 才可能出声 */
    sid_clear_all();
    sid_arp_chord_test();

    s_led_next = make_timeout_time_ms(500);
    absolute_time_t next_tune = make_timeout_time_ms(200);

    /* 每次主循环都跑 tud_task()，否则 USB/HID 会死，BOOTSEL 就发不进来。
     * 没有主机事件时，自己循环跑密集琶音 + 和弦 + 噪声鼓。 */
    while (true) {
        tud_task();
        led_update();

        // Host stream takes over as soon as events arrive; until then keep
        // looping the chime so the board is audible on its own.
        if (!s_sid_host_mode && time_reached(next_tune)) {
            next_tune = make_timeout_time_ms(sid_arp_step());
        }

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
                        tud_task();
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
