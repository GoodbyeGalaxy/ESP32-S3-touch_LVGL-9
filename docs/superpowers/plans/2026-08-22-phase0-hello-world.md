# Studio Panel – Phase 0: Toolchain + Hello World

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** ESP32-S3 bootet, Display zeigt "Hello Studio" auf professionellem Dark-Theme, Touch-Events erscheinen im Serial Log.

**Architecture:** RGB-Panel (ST7262/EK9716) über 16-bit Parallel-Interface, gesteuert via `esp_lcd_new_rgb_panel`. GT911 Touch via I2C. CH422G IO-Expander (I2C, selber Bus) für Backlight und Reset-Leitungen. LVGL 8.3 via `esp_lvgl_port` im eigenen FreeRTOS-Task. Framebuffer liegt im PSRAM.

**Tech Stack:** C++17, ESP-IDF v5.2, LVGL 8.3 (espressif/esp_lvgl_port ^2.4), espressif/esp_lcd_touch_gt911 ^1.1

**Spec:** `../../kickstarter.md` (Abschnitte 2, 10, 11, 13, 14 – Phase 0 + Design-Ziel)

## Global Constraints

- Target-SoC: ESP32-S3 (Waveshare ESP32-S3-Touch-LCD-7, 800×480 IPS, GT911 Touch, CH422G IO-Expander)
- Framework: ESP-IDF v5.2 (keine Arduino-Abhängigkeiten)
- Sprache: C++17, kein RTTI, kein Exceptions
- Display: RGB parallel 16-bit, 2 Framebuffer im PSRAM, `direct_mode = true`
- LVGL: Version 8.3.x – **kein** LVGL 9 (API-Bruch)
- Design (aus kickstarter.md Abschnitt 13): dunkle Oberfläche (#0A0A0A Hintergrund), hoher Kontrast, sehr reduzierte Farbpalette, klare Typografie – das Panel soll wie ein eigenständiges professionelles Studiogerät wirken, **nicht** wie eine Hobby-Mikrocontroller-App
- Alle Board-spezifischen GPIO-Konstanten gehören in `main/board.h` – nirgendwo sonst dürfen GPIO-Nummern stehen
- Kein HAL-Overhead: direkte ESP-IDF APIs verwenden
- PSRAM-Zugriff: `CONFIG_SPIRAM=y`, Framebuffer via `heap_caps_malloc(…, MALLOC_CAP_SPIRAM)`

---

## Dateistruktur

```
studio-panel/
├── CMakeLists.txt              # Top-level CMake
├── sdkconfig.defaults          # ESP32-S3 Build-Konfiguration
├── main/
│   ├── CMakeLists.txt          # Komponenten-Registrierung + REQUIRES
│   ├── idf_component.yml       # Externe Abhängigkeiten (LVGL, GT911)
│   ├── board.h                 # ALLE GPIO-Konstanten, Timing, LCD-Auflösung
│   ├── main.cpp                # app_main: Initialisierungsreihenfolge
│   ├── ch422g.h / ch422g.cpp   # IO-Expander: Backlight, RST-Leitungen
│   ├── display.h / display.cpp # esp_lcd RGB-Panel, gibt panel_handle zurück
│   ├── touch.h / touch.cpp     # GT911 Init, gibt touch_handle zurück
│   ├── ui.h / ui.cpp           # lvgl_port Init, Display + Touch registrieren, Screen laden
│   └── screens/
│       ├── home.h
│       └── home.cpp            # "Hello Studio" – Dark-Theme, Phase-0-Endzustand
└── config/
    ├── devices/
    ├── layouts/
    └── macros/
```

**Interface-Übersicht:**
- `ch422g_init()` → I2C Bus + CH422G bereit; `ch422g_set(mask)` → setzt EXIO-Bits
- `display_init()` → RGB Panel läuft; `display_get_panel()` → `esp_lcd_panel_handle_t`
- `touch_init()` → GT911 bereit; `touch_get_handle()` → `esp_lcd_touch_handle_t`
- `ui_init()` → LVGL läuft, Home Screen sichtbar
- `home_screen_create()` → zeichnet Phase-0-Screen auf `lv_scr_act()`

---

## Task 0: Toolchain-Check & ESP-IDF Setup

**Files:** keine neuen Projektdateien

**Interfaces:** Consumes: nichts. Produces: lauffähiges `idf.py` + `esptool.py` auf dem System.

- [ ] **Step 1: Prüfen ob ESP-IDF bereits installiert ist**

```bash
idf.py --version
```
Expected: `ESP-IDF v5.2.x` oder höher → Tasks 2–4 überspringen.
Falls Fehler → weiter mit Step 2.

- [ ] **Step 2: ESP-IDF v5.2 klonen**

```bash
mkdir -p ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git \
  --branch v5.2 --depth 1 ~/esp/esp-idf
```

- [ ] **Step 3: Toolchain installieren (Xtensa + esptool)**

```bash
cd ~/esp/esp-idf
./install.sh esp32s3
```

Dauert ~5–10 Minuten beim ersten Mal (Download der Xtensa-Toolchain).

- [ ] **Step 4: Umgebung dauerhaft aktivieren**

```bash
echo '. ~/esp/esp-idf/export.sh' >> ~/.bashrc
source ~/.bashrc
idf.py --version
```

Expected: `ESP-IDF v5.2.x`

- [ ] **Step 5: Waveshare-Referenz-Repo lokal spiegeln (zum Nachschlagen)**

```bash
git clone --depth 1 https://github.com/waveshareteam/ESP32-S3-Touch-LCD-7.git \
  /tmp/waveshare-ref
```

Dieses Repo wird **nicht** ins Projekt kopiert. Es dient als Referenz für Timing-Werte und Pin-Bestätigung. Nicht anfassen, nur lesen.

---

## Task 1: Projektgerüst

**Files:** `CMakeLists.txt`, `sdkconfig.defaults`, `main/CMakeLists.txt`, `main/idf_component.yml`, `main/board.h`, `main/main.cpp`, `config/`

**Interfaces:**
- Consumes: funktionierende ESP-IDF-Umgebung (Task 0)
- Produces: `idf.py build` läuft durch (mit Linker-Fehlern wegen fehlender Symbole – das ist OK)

- [ ] **Step 1: Projektverzeichnis anlegen**

```bash
cd /mnt/source/data/coding/ESP32-S3
idf.py create-project --path studio-panel studio-panel
cd studio-panel
mkdir -p config/devices config/layouts config/macros main/screens
```

- [ ] **Step 2: `CMakeLists.txt` schreiben**

```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(studio-panel)
```

- [ ] **Step 3: `sdkconfig.defaults` schreiben**

```ini
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240=y

# 16 MB Flash
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y

# 8 MB PSRAM (OPI/Octal)
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y

# FreeRTOS 1 kHz Tick
CONFIG_FREERTOS_HZ=1000

# Watchdog aus (erleichtert Debug)
CONFIG_ESP_TASK_WDT_EN=n

# LVGL Fonts (werden in Phase 1 erweitert)
CONFIG_LV_FONT_MONTSERRAT_24=y
CONFIG_LV_FONT_MONTSERRAT_14=y
```

- [ ] **Step 4: `main/idf_component.yml` schreiben**

```yaml
dependencies:
  idf: ">=5.2"
  espressif/esp_lvgl_port: "^2.4.0"
  espressif/esp_lcd_touch_gt911: "^1.1.0"
```

- [ ] **Step 5: `main/board.h` schreiben**

Alle GPIO-Nummern für den Waveshare ESP32-S3-Touch-LCD-7 (800×480, GT911, CH422G).
Pin-Quelle: https://docs.waveshare.com/ESP32-S3-Touch-LCD-7

```cpp
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
```

- [ ] **Step 6: `main/CMakeLists.txt` schreiben**

```cmake
idf_component_register(
    SRCS
        "main.cpp"
        "ch422g.cpp"
        "display.cpp"
        "touch.cpp"
        "ui.cpp"
        "screens/home.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        esp_lcd
        esp_lcd_touch
        driver
        freertos
        log
)
```

- [ ] **Step 7: `main/main.cpp` Grundgerüst schreiben**

```cpp
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "board.h"
#include "ch422g.h"
#include "display.h"
#include "touch.h"
#include "ui.h"

static const char *TAG = "main";

extern "C" void app_main()
{
    ESP_LOGI(TAG, "Studio Panel booting...");

    ch422g_init();
    // Backlight + Resets aktivieren
    ch422g_set(CH422G_LCD_RST | CH422G_LCD_BL | CH422G_TOUCH_RST);

    display_init();
    touch_init();
    ui_init();

    ESP_LOGI(TAG, "Boot complete.");
}
```

- [ ] **Step 8: Stub-Implementierungen anlegen (damit CMake durchläuft)**

`main/ch422g.h`:
```cpp
#pragma once
#include <cstdint>
void ch422g_init();
void ch422g_set(uint8_t mask);
```

`main/ch422g.cpp`:
```cpp
#include "ch422g.h"
void ch422g_init() {}
void ch422g_set(uint8_t) {}
```

`main/display.h`:
```cpp
#pragma once
#include "esp_lcd_panel_ops.h"
void display_init();
esp_lcd_panel_handle_t display_get_panel();
```

`main/display.cpp`:
```cpp
#include "display.h"
static esp_lcd_panel_handle_t s_panel = nullptr;
void display_init() {}
esp_lcd_panel_handle_t display_get_panel() { return s_panel; }
```

`main/touch.h`:
```cpp
#pragma once
#include "esp_lcd_touch.h"
void touch_init();
esp_lcd_touch_handle_t touch_get_handle();
```

`main/touch.cpp`:
```cpp
#include "touch.h"
static esp_lcd_touch_handle_t s_touch = nullptr;
void touch_init() {}
esp_lcd_touch_handle_t touch_get_handle() { return s_touch; }
```

`main/ui.h`:
```cpp
#pragma once
void ui_init();
```

`main/ui.cpp`:
```cpp
#include "ui.h"
void ui_init() {}
```

`main/screens/home.h`:
```cpp
#pragma once
void home_screen_create();
```

`main/screens/home.cpp`:
```cpp
#include "home.h"
void home_screen_create() {}
```

- [ ] **Step 9: Ziel setzen + einmal bauen**

```bash
idf.py set-target esp32s3
idf.py build
```

Expected: Kompiliert durch, linkt. Keine Warnungen wegen fehlender Symbole (alles ist gestubbt).

- [ ] **Step 10: Git initialisieren + committen**

```bash
git init
git add .
git commit -m "feat: project scaffold, board.h, stub implementations"
```

---

## Task 2: CH422G IO-Expander – Backlight + Resets

**Files:** `main/ch422g.h` (Interface bleibt), `main/ch422g.cpp` (Implementierung)

**Interfaces:**
- Consumes: `BSP_I2C_*`, `CH422G_*` aus `board.h`
- Produces: `ch422g_init()`, `ch422g_set(mask)` – nach `ch422g_init()` leuchtet das Backlight

Der CH422G verwendet ein einfaches I2C-Write-Protokoll: ein Byte an Adresse 0x24 setzt alle 8 EXIO-Ausgänge auf einmal.

- [ ] **Step 1: `main/ch422g.cpp` implementieren**

```cpp
#include "ch422g.h"
#include "board.h"
#include "driver/i2c.h"
#include "esp_log.h"

static const char *TAG = "ch422g";

void ch422g_init()
{
    i2c_config_t cfg = {};
    cfg.mode             = I2C_MODE_MASTER;
    cfg.sda_io_num       = BSP_I2C_SDA;
    cfg.scl_io_num       = BSP_I2C_SCL;
    cfg.sda_pullup_en    = GPIO_PULLUP_ENABLE;
    cfg.scl_pullup_en    = GPIO_PULLUP_ENABLE;
    cfg.master.clk_speed = BSP_I2C_FREQ;

    ESP_ERROR_CHECK(i2c_param_config(BSP_I2C_PORT, &cfg));
    ESP_ERROR_CHECK(i2c_driver_install(BSP_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0));

    ch422g_set(0);   // alle Ausgänge low (Backlight aus, Reset aktiv-low)
    ESP_LOGI(TAG, "I2C + CH422G initialized");
}

void ch422g_set(uint8_t mask)
{
    esp_err_t err = i2c_master_write_to_device(
        BSP_I2C_PORT, CH422G_I2C_ADDR,
        &mask, 1,
        pdMS_TO_TICKS(20));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CH422G write failed: %s", esp_err_to_name(err));
    }
}
```

- [ ] **Step 2: Bauen + flashen**

```bash
idf.py build && idf.py -p $(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -1) flash monitor
```

Expected im Log:
```
I (xxx) main: Studio Panel booting...
I (xxx) ch422g: I2C + CH422G initialized
I (xxx) main: Boot complete.
```

Das Display-Backlight sollte hell aufleuchten (weiß/grau – kein Inhalt, aber Beleuchtung an).

Falls Backlight **nicht** an: CH422G-Adresse in `board.h` auf `0x20` oder `0x22` ändern und erneut testen.

- [ ] **Step 3: Committen**

```bash
git add main/ch422g.cpp
git commit -m "feat: CH422G IO expander, I2C init, backlight enable"
```

---

## Task 3: RGB Display Driver

**Files:** `main/display.h` (anpassen), `main/display.cpp` (implementieren)

**Interfaces:**
- Consumes: `LCD_*` Konstanten aus `board.h`, `esp_lcd_new_rgb_panel`
- Produces: `display_init()` bringt Panel zum Laufen; `display_get_panel()` → `esp_lcd_panel_handle_t` für LVGL

- [ ] **Step 1: `main/display.h` finalisieren**

```cpp
#pragma once
#include "esp_lcd_panel_ops.h"

void display_init();
esp_lcd_panel_handle_t display_get_panel();
```

- [ ] **Step 2: `main/display.cpp` implementieren**

```cpp
#include "display.h"
#include "board.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

static const char *TAG = "display";
static esp_lcd_panel_handle_t s_panel = nullptr;

void display_init()
{
    const int data_pins[] = LCD_DATA_PINS;

    esp_lcd_rgb_panel_config_t cfg = {};
    cfg.clk_src                      = LCD_CLK_SRC_DEFAULT;
    cfg.timings.pclk_hz              = LCD_PCLK_HZ;
    cfg.timings.h_res                = LCD_H_RES;
    cfg.timings.v_res                = LCD_V_RES;
    cfg.timings.hsync_pulse_width    = LCD_HSYNC_PULSE;
    cfg.timings.hsync_back_porch     = LCD_HSYNC_BP;
    cfg.timings.hsync_front_porch    = LCD_HSYNC_FP;
    cfg.timings.vsync_pulse_width    = LCD_VSYNC_PULSE;
    cfg.timings.vsync_back_porch     = LCD_VSYNC_BP;
    cfg.timings.vsync_front_porch    = LCD_VSYNC_FP;
    cfg.timings.flags.pclk_active_neg = 0;
    cfg.data_width                   = 16;
    cfg.bits_per_pixel               = 16;
    cfg.num_fbs                      = 2;
    cfg.bounce_buffer_size_px        = 10 * LCD_H_RES;
    cfg.psram_trans_align            = 64;
    cfg.hsync_gpio_num               = LCD_PIN_HSYNC;
    cfg.vsync_gpio_num               = LCD_PIN_VSYNC;
    cfg.de_gpio_num                  = LCD_PIN_DE;
    cfg.pclk_gpio_num                = LCD_PIN_PCLK;
    cfg.disp_gpio_num                = -1;  // Backlight via CH422G
    cfg.flags.fb_in_psram            = 1;

    for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = data_pins[i];

    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_LOGI(TAG, "RGB panel ready %dx%d @ %lu Hz", LCD_H_RES, LCD_V_RES, (unsigned long)LCD_PCLK_HZ);
}

esp_lcd_panel_handle_t display_get_panel()
{
    return s_panel;
}
```

- [ ] **Step 3: Rauch-Test in `main.cpp` – roten Streifen zeichnen**

Temporär nach `display_init()` in `app_main()` einfügen:
```cpp
void *fb = nullptr;
esp_lcd_rgb_panel_get_frame_buffer(display_get_panel(), 1, &fb);
if (fb) {
    uint16_t *buf = static_cast<uint16_t *>(fb);
    for (int i = 0; i < LCD_H_RES * 40; i++) buf[i] = 0xF800;  // RGB565 Rot
}
```

- [ ] **Step 4: Bauen + flashen + prüfen**

```bash
idf.py build && idf.py -p $(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -1) flash monitor
```

Expected: Obere ~40 Zeilen des Displays leuchten rot, Rest schwarz.

**Falls kein Bild:**
- PCLK auf `8 * 1000 * 1000` reduzieren und neu flashen
- `pclk_active_neg = 1` setzen (invertierter Clock)
- Timing-Werte mit Waveshare-Referenzcode unter `/tmp/waveshare-ref` vergleichen

- [ ] **Step 5: Rauch-Test-Code entfernen, committen**

```bash
# Temporären Test-Code in main.cpp löschen
git add main/display.cpp main/display.h main/main.cpp
git commit -m "feat: RGB panel driver, solid-color smoke test passed"
```

---

## Task 4: LVGL Integration

**Files:** `main/CMakeLists.txt` (REQUIRES erweitern), `main/ui.cpp`, `main/screens/home.cpp`

**Interfaces:**
- Consumes: `display_get_panel()`, `esp_lvgl_port`
- Produces: `ui_init()` startet LVGL-Task, Home Screen ist sichtbar; `lvgl_port_lock/unlock` für alle LVGL-Zugriffe aus anderen Tasks

- [ ] **Step 1: `main/CMakeLists.txt` REQUIRES erweitern**

```cmake
idf_component_register(
    SRCS
        "main.cpp"
        "ch422g.cpp"
        "display.cpp"
        "touch.cpp"
        "ui.cpp"
        "screens/home.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        esp_lcd
        esp_lcd_touch
        driver
        freertos
        log
        esp_lvgl_port
)
```

- [ ] **Step 2: `main/ui.cpp` implementieren**

```cpp
#include "ui.h"
#include "board.h"
#include "display.h"
#include "touch.h"
#include "screens/home.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "ui";
static lv_display_t *s_disp = nullptr;

void ui_init()
{
    const lvgl_port_cfg_t port_cfg = {
        .task_priority     = 4,
        .task_stack_size   = 8192,
        .task_affinity     = -1,
        .task_max_sleep_ms = 500,
        .timer_period_ms   = 5,
    };
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = nullptr,
        .panel_handle  = display_get_panel(),
        .buffer_size   = LCD_H_RES * LCD_V_RES,
        .double_buffer = true,
        .hres          = LCD_H_RES,
        .vres          = LCD_V_RES,
        .monochrome    = false,
        .color_format  = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy  = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma     = false,
            .buff_spiram  = true,
            .sw_rotate    = false,
            .full_refresh = false,
            .direct_mode  = true,   // Framebuffer direkt – kein Zwischenkopieren
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (!s_disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return;
    }

    lvgl_port_lock(0);
    home_screen_create();
    lvgl_port_unlock();

    ESP_LOGI(TAG, "LVGL ready");
}
```

- [ ] **Step 3: `main/screens/home.cpp` – "Hello Studio" Dark-Theme**

```cpp
#include "home.h"
#include "lvgl.h"

void home_screen_create()
{
    lv_obj_t *scr = lv_scr_act();

    // Hintergrund: fast schwarz (#0A0A0A)
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A0A0A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    // Titelzeile oben (subtiles Grau)
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "STUDIO CONTROL PANEL");
    lv_obj_set_style_text_color(title, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    // Haupttext zentriert (helles Weiß)
    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "Hello Studio");
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    // Version unten rechts
    lv_obj_t *ver = lv_label_create(scr);
    lv_label_set_text(ver, "v0.1");
    lv_obj_set_style_text_color(ver, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(ver, LV_ALIGN_BOTTOM_RIGHT, -16, -12);
}
```

- [ ] **Step 4: Bauen + flashen**

```bash
idf.py build && idf.py -p $(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -1) flash monitor
```

Expected: Fast-schwarzer Hintergrund, "STUDIO CONTROL PANEL" oben in Dunkelgrau, "Hello Studio" weiß zentriert, "v0.1" unten rechts in Dunkelgrau.

Falls `lv_font_montserrat_24` nicht gefunden: `idf.py menuconfig` → Component Config → LVGL → Fonts → Montserrat 24 aktivieren, dann `idf.py build`.

- [ ] **Step 5: Committen**

```bash
git add main/ui.cpp main/ui.h main/screens/home.cpp main/CMakeLists.txt
git commit -m "feat: LVGL + esp_lvgl_port, Hello Studio dark-theme screen"
```

---

## Task 5: GT911 Touch – Events + visuelles Feedback

**Files:** `main/touch.cpp` (implementieren), `main/ui.cpp` (Touch registrieren), `main/screens/home.cpp` (Event-Handler)

**Interfaces:**
- Consumes: `BSP_I2C_PORT` (bereits von ch422g_init initialisiert), `CH422G_TOUCH_RST`, `TOUCH_INT_PIN`
- Produces: `touch_init()`, `touch_get_handle()` → `esp_lcd_touch_handle_t`; LVGL empfängt Touch-Events; Serial Log zeigt Koordinaten

- [ ] **Step 1: `main/CMakeLists.txt` REQUIRES erweitern**

```cmake
    REQUIRES
        esp_lcd
        esp_lcd_touch
        esp_lcd_touch_gt911     # neu
        driver
        freertos
        log
        esp_lvgl_port
```

- [ ] **Step 2: `main/touch.cpp` implementieren**

```cpp
#include "touch.h"
#include "board.h"
#include "ch422g.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_panel_io.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "touch";
static esp_lcd_touch_handle_t s_touch = nullptr;

void touch_init()
{
    // GT911 Reset-Sequenz: RST low → warten → RST high
    ch422g_set(CH422G_LCD_RST | CH422G_LCD_BL);                         // Touch RST = low
    vTaskDelay(pdMS_TO_TICKS(10));
    ch422g_set(CH422G_LCD_RST | CH422G_LCD_BL | CH422G_TOUCH_RST);     // Touch RST = high

    // I2C Panel-IO für GT911 (I2C-Bus läuft bereits durch ch422g_init)
    esp_lcd_panel_io_handle_t tp_io = nullptr;
    esp_lcd_panel_io_i2c_config_t io_cfg = {};
    io_cfg.dev_addr             = 0x5D;   // GT911 Standard; bei Fehler: 0x14 versuchen
    io_cfg.scl_speed_hz         = BSP_I2C_FREQ;
    io_cfg.control_phase_bytes  = 1;
    io_cfg.dc_bit_offset        = 0;
    io_cfg.lcd_cmd_bits         = 16;
    io_cfg.lcd_param_bits       = 8;
    io_cfg.flags.disable_control_phase = 1;

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(
        (esp_lcd_i2c_bus_handle_t)BSP_I2C_PORT, &io_cfg, &tp_io));

    esp_lcd_touch_config_t tp_cfg = {};
    tp_cfg.x_max         = LCD_H_RES;
    tp_cfg.y_max         = LCD_V_RES;
    tp_cfg.rst_gpio_num  = GPIO_NUM_NC;   // Reset via CH422G, nicht per GPIO
    tp_cfg.int_gpio_num  = TOUCH_INT_PIN;
    tp_cfg.levels.reset      = 0;
    tp_cfg.levels.interrupt  = 0;
    tp_cfg.flags.swap_xy  = 0;
    tp_cfg.flags.mirror_x = 0;
    tp_cfg.flags.mirror_y = 0;

    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &s_touch));
    ESP_LOGI(TAG, "GT911 initialized");
}

esp_lcd_touch_handle_t touch_get_handle()
{
    return s_touch;
}
```

- [ ] **Step 3: Touch in `ui_init()` registrieren**

In `main/ui.cpp`, nach `lvgl_port_add_disp()`:
```cpp
    // Touch zu LVGL-Display binden
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp   = s_disp,
        .handle = touch_get_handle(),
    };
    lvgl_port_add_touch(&touch_cfg);
```

- [ ] **Step 4: Touch-Feedback im Home Screen**

In `main/screens/home.cpp` am Ende von `home_screen_create()` ergänzen:
```cpp
    // Touch-Koordinaten im Serial Log + visuelles Feedback
    static lv_obj_t *touch_dot = nullptr;
    touch_dot = lv_obj_create(scr);
    lv_obj_set_size(touch_dot, 20, 20);
    lv_obj_set_style_radius(touch_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(touch_dot, lv_color_hex(0xFF4444), LV_PART_MAIN);
    lv_obj_set_style_border_width(touch_dot, 0, LV_PART_MAIN);
    lv_obj_add_flag(touch_dot, LV_OBJ_FLAG_HIDDEN);

    // Lambda kann LVGL-Callback nicht direkt; statische Funktion verwenden
    struct Ctx { lv_obj_t *dot; };
    static Ctx ctx;
    ctx.dot = touch_dot;

    lv_obj_add_event_cb(scr, [](lv_event_t *e) {
        lv_indev_t *indev = lv_indev_get_act();
        if (!indev) return;
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        ESP_LOGI("home", "Touch: x=%d y=%d", (int)p.x, (int)p.y);
        Ctx *c = static_cast<Ctx *>(lv_event_get_user_data(e));
        lv_obj_align(c->dot, LV_ALIGN_TOP_LEFT, p.x - 10, p.y - 10);
        lv_obj_clear_flag(c->dot, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_PRESSING, &ctx);
```

- [ ] **Step 5: Bauen + flashen**

```bash
idf.py build && idf.py -p $(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -1) flash monitor
```

Expected:
- Display zeigt "Hello Studio" auf schwarzem Hintergrund
- Beim Antippen: roter Kreis an der Berührungsposition
- Serial Log: `I (xxx) home: Touch: x=NNN y=NNN`

Falls GT911 nicht antwortet:
```
# I2C-Scanner: prüft welche Adressen auf dem Bus antworten
# (Temporär in ch422g_init() nach dem Install einfügen)
for (uint8_t addr = 1; addr < 127; addr++) {
    uint8_t dummy;
    if (i2c_master_read_from_device(BSP_I2C_PORT, addr, &dummy, 1, pdMS_TO_TICKS(10)) == ESP_OK)
        ESP_LOGI("scan", "I2C device at 0x%02X", addr);
}
```
Wenn 0x14 statt 0x5D erscheint: `io_cfg.dev_addr = 0x14` in `touch.cpp`.

- [ ] **Step 6: Kickstarter-Erfolgskriterium aus Abschnitt 16 bestätigen**

```
ESP32 bootet                 ✓  Serial Log: "Studio Panel booting..."
Display zeigt Oberfläche     ✓  Hello Studio, Dark Theme
Touch Button reagiert        ✓  Roter Dot + Serial Log
```

- [ ] **Step 7: Committen**

```bash
git add main/touch.cpp main/touch.h main/ui.cpp main/screens/home.cpp main/CMakeLists.txt
git commit -m "feat: GT911 touch driver, LVGL touch input, Phase 0 complete"
```

---

## Phasen-Übersicht — Folgende Pläne

Jede Phase bekommt einen eigenen Plan. WING kommt zuletzt (wie gewünscht).

| Plan-Datei | Phase | Inhalt |
|---|---|---|
| `phase0` | 0 | **Dieser Plan** – Toolchain, Display, Touch, Hello World |
| `phase1` | 1 | Home-Screen mit 6 Modul-Kacheln, Screen-Navigation (`lv_scr_load_anim`), Dark-Theme-System, Statusleiste |
| `phase2` | 2 | USB HID via TinyUSB: Touch-Button → Keyboard-Shortcut auf dem Mac (CMD+S, CMD+Z etc.) |
| `phase3` | 3 | USB MIDI (TinyUSB MIDI Class), CC-Fader (`lv_slider`) → Nord Lead 2X |
| `phase4` | 4 | JSON-Geräteprofile (cJSON), SysEx-Template-Engine, abstrahierte Device Pages |
| `phase5` | 5 | WebSocket-Client (`esp_websocket_client`), Peak/RMS/LUFS/Goniometer empfangen und in LVGL visualisieren |
| `phase7` | 7 | HTTP Config-Server (`esp_http_client`), JSON-Layouts vom NAS laden, OTA optional |
| `phase6` | 6 | WING-Integration (OSC/Netzwerk), Status, Snapshots, Mutes – **zuletzt** |

> **Design-Gebot für alle Phasen** (kickstarter.md §13): Jede neue Seite muss wirken wie ein eigenständiges professionelles Studiogerät — kein Hobby-Look. Dark Theme, hoher Kontrast, große Touch-Ziele (min. 60×60 px), klare Typografie (Montserrat), Warnfarben nur bei Bedarf. Optik hat höchste Priorität.
