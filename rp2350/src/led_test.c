/*
 * led_test.c - 板载 WS2812 流水灯 + 心跳灯测试
 *
 * 引脚和 PIO 配置照抄 RPFM 的 src/rpfm/main.c：
 *   WS2812 = GPIO16, 4 颗串联, PIO1, 800 kHz, 官方 pico-examples 时序
 *   LED    = GPIO25 心跳（板载单色灯）
 *
 * 注意：RPFM 的 main.c 里 PIN_WS2812 写的是 22，那是 commit 3cf1d8b 为了
 * 给 I2C 屏让出 GPIO16 改的。本机实测：只驱动 22 时灯全灭，驱动 16 才亮。
 * 所以这块淘宝 RP2350A 核心板的板载 WS2812 在 GPIO16（与 docs/WS2812_LED.md
 * 和 docs/SPFM_BUS_PINOUT.md 一致），main.c 的 22 不适用。
 *
 * 亮度按 RPFM 的实测值：片选指示 ~40%，流水灯压到 0x18 左右不刺眼。
 *
 * USB 侧照抄同一套 HID 描述符，所以 CMD_BOOTSEL (0x20) 依然有效，
 * scripts/flash.sh 可以全自动烧录。
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "tusb.h"
#include "ws2812.pio.h"
#include "protocol.h"

#define PIN_WS2812   16
#define PIN_LED      25
#define NUM_WS_LEDS  4

static PIO  s_ws_pio = pio1;
static uint s_ws_sm;
static uint32_t s_ws_leds[NUM_WS_LEDS];

/* GRB 顺序，和 RPFM 的 urgb_u32 一致 */
static uint32_t urgb_u32(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)g << 16) | ((uint32_t)r << 8) | (uint32_t)b;
}

static void ws2812_update(void) {
    for (int i = 0; i < NUM_WS_LEDS; i++)
        pio_sm_put_blocking(s_ws_pio, s_ws_sm, s_ws_leds[i] << 8u);
}

static void ws2812_init(void) {
    uint offset = pio_add_program(s_ws_pio, &ws2812_program);
    s_ws_sm = pio_claim_unused_sm(s_ws_pio, true);
    ws2812_program_init(s_ws_pio, s_ws_sm, offset, PIN_WS2812, 800000, false);
    for (int i = 0; i < NUM_WS_LEDS; i++) s_ws_leds[i] = 0;
    ws2812_update();
}

/* ------------------------------------------------------------------ */
/* USB HID：只处理 BOOTSEL，其余忽略                                   */
/* ------------------------------------------------------------------ */
static volatile uint8_t s_last_seq = 0;
static volatile uint8_t s_status = 0;

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize) {
    (void)instance; (void)report_id; (void)report_type;

    if (bufsize < 4) return;

    uint8_t cmd = buffer[0];
    uint8_t seq = buffer[1];
    uint8_t len = buffer[2];

    if (len > 60 || bufsize < (uint16_t)(3 + len + 1)) return;
    if (rpfm_crc8(buffer, 63) != buffer[63]) {
        s_status |= STATUS_ERROR;
        return;
    }

    s_last_seq = seq;
    s_status &= ~STATUS_ERROR;

    if (cmd == CMD_BOOTSEL)
        reset_usb_boot(0, 0);
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type;

    if (reqlen < RPFM_FRAME_SIZE) return 0;

    memset(buffer, 0, RPFM_FRAME_SIZE);
    buffer[0] = s_last_seq;
    buffer[1] = s_status;
    return RPFM_FRAME_SIZE;
}

/* ------------------------------------------------------------------ */
int main(void) {
    stdio_init_all();

    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, 0);

    ws2812_init();
    tud_init(BOARD_TUD_RHPORT);

    /* 自检：4 颗依次点亮，暗绿，每颗 150 ms —— 证明整条链在 */
    for (int i = 0; i < NUM_WS_LEDS; i++) {
        for (int j = 0; j < NUM_WS_LEDS; j++)
            s_ws_leds[j] = (j == i) ? urgb_u32(0x00, 0x18, 0x00) : 0;
        ws2812_update();
        sleep_ms(150);
    }

    /* 流水灯：一颗头 + 两级渐暗尾巴 */
    static const uint32_t colors[NUM_WS_LEDS] = {
        0x001800,   /* 绿 */
        0x000018,   /* 蓝 */
        0x181800,   /* 黄 */
        0x180000,   /* 红 */
    };

    int head = 0;
    bool led_on = false;
    absolute_time_t next_toggle = make_timeout_time_ms(250);
    absolute_time_t next_step   = make_timeout_time_ms(150);

    while (true) {
        tud_task();

        /* 心跳灯 250 ms 翻转 */
        if (time_reached(next_toggle)) {
            led_on = !led_on;
            gpio_put(PIN_LED, led_on);
            next_toggle = make_timeout_time_ms(250);
        }

        /* 流水灯每 150 ms 走一格 */
        if (time_reached(next_step)) {
            for (int j = 0; j < NUM_WS_LEDS; j++)
                s_ws_leds[j] = 0;

            s_ws_leds[head] = colors[head];
            int t1 = (head + NUM_WS_LEDS - 1) % NUM_WS_LEDS;
            int t2 = (head + NUM_WS_LEDS - 2) % NUM_WS_LEDS;
            s_ws_leds[t1] = colors[t1] >> 2;
            s_ws_leds[t2] = colors[t2] >> 3;

            ws2812_update();

            head = (head + 1) % NUM_WS_LEDS;
            next_step = make_timeout_time_ms(150);
        }

        tight_loop_contents();
    }

    return 0;
}
