// lcd.c - ST7567 128x64 LCD driver (I2C)
// Yellow/blue dual color display support

#include "lcd.h"
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

// Display buffer (128x64 = 1024 bytes)
static uint8_t s_lcd_buf[LCD_WIDTH * LCD_PAGES];

// ST7567 commands
#define ST7567_SET_DISP          0xAE  // Display off/on
#define ST7567_SET_MEM_MODE      0x20  // Memory addressing mode
#define ST7567_SET_COL_ADDR      0x21  // Column address
#define ST7567_SET_PAGE_ADDR     0x22  // Page address
#define ST7567_SET_DISP_START    0x40  // Display start line
#define ST7567_SET_CONTRAST      0x81  // Contrast control
#define ST7567_SET_CHARGE_PUMP   0x8D  // Charge pump setting
#define ST7567_SET_SEG_REMAP     0xA0  // Segment remap
#define ST7567_SET_ENTIRE_ON     0xA4  // Entire display on
#define ST7567_SET_ALL_ON        0xA5  // All pixels on
#define ST7567_SET_NORM_DISP     0xA6  // Normal display
#define ST7567_SET_INV_DISP      0xA7  // Inverted display
#define ST7567_SET_MUX_RATIO     0xA8  // Mux ratio
#define ST7567_SET_COM_OUT_DIR   0xC0  // COM output direction
#define ST7567_SET_DISP_OFFSET   0xD3  // Display offset
#define ST7567_SET_DISP_CLK_DIV  0xD5  // Display clock divide
#define ST7567_SET_PRECHARGE     0xD9  // Precharge period
#define ST7567_SET_COM_PIN_CFG   0xDA  // COM pin config
#define ST7567_SET_VCOM_DESEL    0xDB  // VCOMH deselect level

// I2C speed (kHz)
#define LCD_I2C_CLK 400

// Send command to LCD
static void lcd_send_cmd(uint8_t cmd) {
    uint8_t buf[2] = {0x80, cmd};  // 0x80 = command mode
    i2c_write_blocking(LCD_I2C, LCD_I2C_ADDR, buf, 2, false);
}

// Send command list
static void lcd_send_cmd_list(const uint8_t *cmds, int num) {
    for (int i = 0; i < num; i++) {
        lcd_send_cmd(cmds[i]);
    }
}

// Send data buffer to LCD
static void lcd_send_buf(const uint8_t *buf, int len) {
    // Allocate temp buffer for control byte prefix
    uint8_t *temp_buf = malloc(len + 1);
    if (!temp_buf) return;

    temp_buf[0] = 0x40;  // 0x40 = data mode
    memcpy(temp_buf + 1, buf, len);

    i2c_write_blocking(LCD_I2C, LCD_I2C_ADDR, temp_buf, len + 1, false);

    free(temp_buf);
}

void lcd_init(void) {
    // Initialize I2C
    i2c_init(LCD_I2C, LCD_I2C_CLK * 1000);
    gpio_set_function(LCD_SDA, GPIO_FUNC_I2C);
    gpio_set_function(LCD_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(LCD_SDA);
    gpio_pull_up(LCD_SCL);

    sleep_ms(100);  // Wait for LCD power up

    // ST7567 initialization sequence
    const uint8_t init_cmds[] = {
        ST7567_SET_DISP,              // Display off
        ST7567_SET_MEM_MODE, 0x00,    // Horizontal addressing mode
        ST7567_SET_DISP_START, 0x00,  // Start line 0
        ST7567_SET_SEG_REMAP | 0x01,  // Segment remap (column 127 -> SEG0)
        ST7567_SET_MUX_RATIO, 0x3F,   // 1/64 duty
        ST7567_SET_COM_OUT_DIR | 0x08,// COM output scan direction
        ST7567_SET_DISP_OFFSET, 0x00, // No offset
        ST7567_SET_COM_PIN_CFG, 0x12, // COM pins config (128x64)
        ST7567_SET_DISP_CLK_DIV, 0x80,// Default frequency
        ST7567_SET_PRECHARGE, 0xF1,   // Precharge period
        ST7567_SET_VCOM_DESEL, 0x30,  // VCOMH deselect level
        ST7567_SET_CONTRAST, 0x7F,    // Contrast (~50%)
        ST7567_SET_ENTIRE_ON,         // Resume RAM content display
        ST7567_SET_NORM_DISP,         // Normal display (not inverted)
        ST7567_SET_CHARGE_PUMP, 0x14, // Enable charge pump
        ST7567_SET_DISP | 0x01,       // Display on
    };

    lcd_send_cmd_list(init_cmds, sizeof(init_cmds));

    // Clear display
    lcd_clear();
}

void lcd_clear(void) {
    memset(s_lcd_buf, 0, sizeof(s_lcd_buf));
    lcd_update(s_lcd_buf);
}

void lcd_update(const uint8_t *buf) {
    // Set column address range (full width)
    lcd_send_cmd(ST7567_SET_COL_ADDR);
    lcd_send_cmd(0);
    lcd_send_cmd(LCD_WIDTH - 1);

    // Set page address range (full height)
    lcd_send_cmd(ST7567_SET_PAGE_ADDR);
    lcd_send_cmd(0);
    lcd_send_cmd(LCD_PAGES - 1);

    // Send entire frame buffer
    lcd_send_buf(buf, LCD_WIDTH * LCD_PAGES);
}

void lcd_setpixel(uint8_t *buf, int x, int y, bool on) {
    if (x < 0 || x >= LCD_WIDTH || y < 0 || y >= LCD_HEIGHT) return;

    int byte_idx = (y / 8) * LCD_WIDTH + x;
    uint8_t byte = buf[byte_idx];

    if (on)
        byte |= (1 << (y % 8));
    else
        byte &= ~(1 << (y % 8));

    buf[byte_idx] = byte;
}

void lcd_hline(uint8_t *buf, int x0, int x1, int y, bool on) {
    if (y < 0 || y >= LCD_HEIGHT) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= LCD_WIDTH) x1 = LCD_WIDTH - 1;
    if (x0 > x1) {
        int t = x0; x0 = x1; x1 = t;
    }

    for (int x = x0; x <= x1; x++) {
        lcd_setpixel(buf, x, y, on);
    }
}

void lcd_vline(uint8_t *buf, int x, int y0, int y1, bool on) {
    if (x < 0 || x >= LCD_WIDTH) return;
    if (y0 < 0) y0 = 0;
    if (y1 >= LCD_HEIGHT) y1 = LCD_HEIGHT - 1;
    if (y0 > y1) {
        int t = y0; y0 = y1; y1 = t;
    }

    for (int y = y0; y <= y1; y++) {
        lcd_setpixel(buf, x, y, on);
    }
}

void lcd_rect(uint8_t *buf, int x0, int y0, int x1, int y1, bool on) {
    lcd_hline(buf, x0, x1, y0, on);
    lcd_hline(buf, x0, x1, y1, on);
    lcd_vline(buf, x0, y0, y1, on);
    lcd_vline(buf, x1, y0, y1, on);
}

// Simple 8x8 ASCII font (limited character set)
static const uint8_t font_8x8[96][8] = {
    // 0x20 (space) to 0x7F (~)
    // ... (font data would go here)
    // For now, just provide a few basic characters
    [' ' - 0x20] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['A' - 0x20] = {0x3C,0x66,0x66,0x7E,0x66,0x66,0x66,0x00},
    ['B' - 0x20] = {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00},
    ['C' - 0x20] = {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00},
    // ... more characters would be added
};

void lcd_text(uint8_t *buf, int x, int y, const char *str) {
    if (y < 0 || y >= LCD_HEIGHT - 8) return;

    while (*str && x < LCD_WIDTH - 8) {
        char ch = toupper(*str++);
        if (ch < 0x20 || ch > 0x7F) ch = ' ';  // Replace non-ASCII with space

        const uint8_t *glyph = font_8x8[ch - 0x20];
        int page = y / 8;

        for (int col = 0; col < 8; col++) {
            if (x + col >= LCD_WIDTH) break;
            buf[page * LCD_WIDTH + x + col] = glyph[col];
        }
        x += 8;
    }
}

uint8_t* lcd_get_buffer(void) {
    return s_lcd_buf;
}
