#pragma once
#include "driver/gpio.h"
#include "driver/i2c.h"

// ── Auflösung ─────────────────────────────────────────────────
#define LCD_H_RES    800
#define LCD_V_RES    480

// ── RGB Pixel Clock ───────────────────────────────────────────
// 16 MHz → ~39 fps mit den u.g. Blank-Werten (ausreichend für Touch-UI)
#define LCD_PCLK_HZ  (16 * 1000 * 1000)

// ── RGB Steuerleitungen ──────────────────────────────────────
#define LCD_PIN_PCLK   GPIO_NUM_7
#define LCD_PIN_HSYNC  GPIO_NUM_46
#define LCD_PIN_VSYNC  GPIO_NUM_3
#define LCD_PIN_DE     GPIO_NUM_5

// ── RGB Datenpins [bit0..bit15] = B3–B7, G2–G7, R3–R7 ────────
// Reihenfolge: LSB (B3) → MSB (R7) – so wie esp_lcd_rgb_panel_config_t.data_gpio_nums erwartet
#define LCD_DATA_PINS  { 14, 38, 18, 17, 10, \
                         39,  0, 45, 48, 47, 21, \
                          1,  2, 42, 41, 40 }

// ── RGB Blanking-Timing ──────────────────────────────────────
#define LCD_HSYNC_PULSE  4
#define LCD_HSYNC_BP     8
#define LCD_HSYNC_FP     8
#define LCD_VSYNC_PULSE  4
#define LCD_VSYNC_BP     8
#define LCD_VSYNC_FP     8

// ── I2C Bus (Touch + IO-Expander teilen denselben Bus) ────────
#define BSP_I2C_PORT  I2C_NUM_0
#define BSP_I2C_SDA   GPIO_NUM_8
#define BSP_I2C_SCL   GPIO_NUM_9
#define BSP_I2C_FREQ  400000

// ── GT911 Touch ──────────────────────────────────────────────
#define TOUCH_INT_PIN  GPIO_NUM_4
// RST läuft über CH422G EXIO1 (kein direkter GPIO)

// ── CH422G IO-Expander (I2C-Adresse 0x24) ────────────────────
// Jedes Bit entspricht einem EXIO-Ausgang
#define CH422G_I2C_ADDR   0x24
#define CH422G_TOUCH_RST  (1 << 1)   // EXIO1
#define CH422G_LCD_BL     (1 << 2)   // EXIO2 – Backlight
#define CH422G_LCD_RST    (1 << 3)   // EXIO3 – Display Reset
