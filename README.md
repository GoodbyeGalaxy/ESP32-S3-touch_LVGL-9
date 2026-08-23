# ESP32-S3 Studio Control Panel

A standalone 7" touch control panel for a hybrid audio studio, built on the
**Waveshare ESP32-S3-Touch-LCD-7** with **LVGL 9.5** and **ESP-IDF v5.5**.

The panel is being developed as a modular, freely programmable interface layer
over an entire studio environment — DAW control, MIDI/SysEx, audio metering,
and mixer integration — all accessible through large, responsive touch targets
on a professional-looking dark UI.

> **Community note:** Working LVGL 9.x on this board requires several
> non-obvious workarounds. Hard-won findings are documented in
> [`docs/development-notes.md`](docs/development-notes.md).

---

## Hardware

| Component | Detail |
|---|---|
| Board | Waveshare ESP32-S3-Touch-LCD-7 |
| Display | 800 × 480 IPS, RGB parallel interface |
| Touch | GT911 (capacitive, 5-point) via CH422G I/O expander |
| MCU | ESP32-S3 dual-core, 240 MHz |
| Flash | 16 MB |
| PSRAM | 8 MB OPI (octal) |
| Connectivity | Wi-Fi 2.4 GHz, BLE 5 |
| USB | Single USB-C (native ESP32-S3 USB OTG) |

---

## Software Stack

| Layer | Choice |
|---|---|
| Firmware | C++17 |
| SDK | ESP-IDF v5.5 |
| UI | LVGL 9.5 via `espressif/esp_lvgl_adapter ^0.5.2` |
| Display driver | `esp_lcd_rgb_panel` (direct, no SPI) |
| Touch driver | `espressif/esp_lcd_touch_gt911 ^1.1.0` |
| Build | CMake (standard ESP-IDF) |

---

## LVGL 9 on ESP32-S3 — What Works (and What Doesn't)

This project specifically targets **LVGL 9.5**, not LVGL 8. The two versions
are not API-compatible. Key pitfalls encountered during bring-up:

### Component version pinning
`espressif/esp_lvgl_port: "^2.4.0"` can pull in 2.9.0, which checks
`SOC_LCDCAM_RGB_LCD_SUPPORTED` — a macro that does not exist on ESP32-S3
(it uses `SOC_LCD_RGB_SUPPORTED`). Result: `lvgl_port_add_disp_rgb()` always
returns NULL. Fix:

```yaml
# idf_component.yml
espressif/esp_lvgl_adapter: "^0.5.2"   # use the Waveshare-specific adapter
```

### LVGL task stack
8 KB is not enough for LVGL 9 + RGB panel. The task starts silently and hangs.

```cpp
cfg.task_stack_size = 16384;  // minimum for LVGL 9 on this board
```

### Default theme bleeds through
Even after `lv_obj_remove_style_all()` the background shows PSRAM noise
(green, `0x07E0` in RGB565). The fix is to disable the theme before any
screen is created:

```cpp
lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
lv_display_set_theme(disp, nullptr);           // disable theme first

lv_obj_t *scr = lv_obj_create(nullptr);       // always create fresh screen
lv_obj_set_style_bg_color(scr, color, 0);
// ... add widgets ...
lv_screen_load(scr);                          // load last
```

Never use `lv_screen_active()` as a parent — it carries theme padding that
lets PSRAM artifacts show at the edges.

### IPS panel black point
This panel shows a greenish tint below approximately 38% luminance. All
colors in the project are kept at or above `0x606060` for backgrounds.
Do not use near-black values like `0x0A0A0A`.

### CH422G I/O expander
The CH422G does not respond reliably to standard I2C scanning. Backlight
works via hardware default state even when software writes fail. Touch reset
(GT911) routes through CH422G EXIO1, so GT911 is currently not initialized.
**A logic analyzer is needed to debug this.** See
[`docs/development-notes.md`](docs/development-notes.md) for details.

---

## Project Structure

```
ESP32-S3/
├── studio-panel/          ESP-IDF project
│   ├── main/
│   │   ├── main.cpp       Boot sequence
│   │   ├── board.h        Pin definitions
│   │   ├── display.cpp    RGB panel init
│   │   ├── touch.cpp      GT911 init (partially working)
│   │   ├── ch422g.cpp     I/O expander (backlight + touch reset)
│   │   ├── ui.cpp         LVGL init + screen launch
│   │   ├── theme.h        Colors, fonts, geometry constants
│   │   └── screens/
│   │       ├── home.cpp        6-tile home screen
│   │       ├── statusbar.cpp   Persistent top bar on lv_layer_top
│   │       ├── metering.cpp    Audio metering (in progress)
│   │       ├── studio_one.cpp  DAW control (planned)
│   │       ├── usb_midi.cpp    USB MIDI (planned)
│   │       ├── routing.cpp     WING integration (planned)
│   │       ├── dev_control.cpp Device profiles (planned)
│   │       └── settings.cpp    Configuration (planned)
├── docs/
│   ├── development-notes.md   Hard-won bring-up findings
│   └── superpowers/           Design specs and implementation plans
└── README.md
```

---

## Build & Flash

```bash
# 1. Activate ESP-IDF (once per shell session)
source ~/esp/esp-idf-5.5/export.sh

# 2. Build
cd studio-panel
idf.py build

# 3. Flash
idf.py -p /dev/ttyACM0 flash

# 4. Monitor
idf.py -p /dev/ttyACM0 monitor
# Exit: Ctrl+]

# If permission denied on /dev/ttyACM0:
newgrp dialout
```

> **First flash only:** If the board had other firmware, erase flash first:
> ```bash
> idf.py -p /dev/ttyACM0 erase-flash
> idf.py -p /dev/ttyACM0 flash
> ```

---

## Current State

| Phase | Description | Status |
|---|---|---|
| 0 | Display, LVGL, background color | ✅ Done |
| 1 | Home screen (6 tiles, slide navigation, statusbar on `lv_layer_top`) | ✅ Done |
| 2 | Metering screen (bars, goniometer, loudness history, demo data) | 🔨 In progress |
| 3 | WiFi + real audio data via UDP/OSC | Planned |
| 4 | USB HID (keyboard shortcuts → DAW) | Planned |
| 5 | USB MIDI (CC/SysEx → synthesizers) | Planned |
| 6 | Studio One WebSocket integration | Planned |
| 7 | Behringer WING integration | Planned |

### Metering Screen (Phase 2 target)

```
┌─────────────────────────────────────────────────────────────────┐
│                    Status Bar                                    │
├──────────┬──────────────────────────────────┬───────────────────┤
│  L   R   │  Goniometer / Lissajous (250×250) │  I:  -14.2 LKFS  │
│  ││  ││  │  (stereo correlation, M/S rotated) │  S:  -13.8 LKFS  │
│  ││  ││  │                                   │  M:  -12.1 LKFS  │
│          ├──────────────────────────────────┤  Peak: -5.9 dBFS │
│          │  Short-term Loudness / 60 s       │                  │
│          │  EBU R128 target: -23 LKFS ────  │                  │
├──────────┴──────────────────────────────────┴───────────────────┤
│ ◁ Home                                                          │
└─────────────────────────────────────────────────────────────────┘
```

---

## Known Issues

1. **CH422G not responding** — I2C writes fail (`ESP_FAIL`). Backlight works
   via hardware default. Touch reset through CH422G not possible → GT911
   uninitialized. Needs logic analyzer to diagnose.

2. **GT911 touch not initialized** — depends on CH422G reset (see above).

3. **IPS greenish tint below ~38% luminance** — all UI colors kept above
   `0x606060`.

---

## Planned Studio Integration

```
                        ┌─────────────────┐
                        │  Touch Panel    │
                        │  (this project) │
                        └────────┬────────┘
                                 │
              ┌──────────────────┼──────────────────┐
              │                  │                  │
        ┌─────▼────┐      ┌──────▼─────┐    ┌──────▼─────┐
        │  USB HID │      │  USB MIDI  │    │  Wi-Fi     │
        │  (DAW    │      │  (CC/SysEx │    │  (OSC/WS/  │
        │  macros) │      │  → synths) │    │  metering) │
        └──────────┘      └────────────┘    └──────────┘
                                 │
                    ┌────────────┼────────────┐
                    │            │            │
             ┌──────▼──┐  ┌─────▼───┐  ┌────▼──────┐
             │Studio One│  │  WING  │  │  mioXL   │
             │  (DAW)  │  │(Mixer) │  │  (MIDI   │
             └─────────┘  └────────┘  │  Router) │
                                      └──────────┘
```

Target studio gear: Studio One, Behringer WING Rack, iConnectivity mioXL,
Nord Lead 2X, Nord Electro, Novation Bass Station, Novation DrumStation,
Arturia KeyStep Pro, Arturia BeatStep Pro, MPC One+.

---

## Design Goals

- Dark studio aesthetic, not a smartphone app
- High contrast, minimal color palette
- Large touch targets (≥ 44 px)
- Smooth animations (LVGL `lv_screen_load_anim`)
- Warning colors only when needed
- Feels like a dedicated hardware device

---

## License

MIT
