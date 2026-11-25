// lcd.h - ST7567 128x64 LCD driver (I2C)
// Supports yellow/blue dual color displays

#ifndef LCD_H
#define LCD_H

#include <stdint.h>
#include <stdbool.h>

// Display configuration
#define LCD_WIDTH   128
#define LCD_HEIGHT  64
#define LCD_PAGES   (LCD_HEIGHT / 8)  // 8 pages

// I2C configuration
#define LCD_I2C     i2c0
#define LCD_SDA     15
#define LCD_SCL     16
#define LCD_I2C_ADDR 0x3C  // Typical ST7567 I2C address

// Initialize LCD (I2C + ST7567)
void lcd_init(void);

// Clear entire display
void lcd_clear(void);

// Update display from buffer (128x64 = 1024 bytes)
void lcd_update(const uint8_t *buf);

// Set pixel in buffer (x: 0-127, y: 0-63)
void lcd_setpixel(uint8_t *buf, int x, int y, bool on);

// Draw horizontal line
void lcd_hline(uint8_t *buf, int x0, int x1, int y, bool on);

// Draw vertical line
void lcd_vline(uint8_t *buf, int x, int y0, int y1, bool on);

// Draw rectangle
void lcd_rect(uint8_t *buf, int x0, int y0, int x1, int y1, bool on);

// Draw text (ASCII only, 8x8 font)
void lcd_text(uint8_t *buf, int x, int y, const char *str);

// Get display buffer pointer (allocated in driver)
uint8_t* lcd_get_buffer(void);

#endif // LCD_H
