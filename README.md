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
| Touch | GT911 (capacitive, 5-point), I2C address fallback 0x5D → 0x14 |
| MCU | ESP32-S3 dual-core, 240 MHz |
| Flash | 16 MB OPI |
| PSRAM | 8 MB OPI (octal, 80 MHz) |
| Connectivity | Wi-Fi 2.4 GHz, BLE 5 |
| USB | Single USB-C (native ESP32-S3 USB OTG — used for USB MIDI) |

---

## Software Stack

| Layer | Choice |
|---|---|
| Firmware | C++17 |
| SDK | ESP-IDF v5.5 |
| UI | LVGL 9.5 via `espressif/esp_lvgl_adapter ^0.5.2` |
| Display driver | `esp_lcd_rgb_panel` (direct parallel RGB) |
| Touch driver | `espressif/esp_lcd_touch_gt911 ^1.1.0` |
| USB MIDI | `espressif/esp_tinyusb ^1.1` (USB MIDI Class Device) |
| Build | CMake (standard ESP-IDF) |

---

## Current State

| Phase | Description | Status |
|---|---|---|
| 0 | Display, LVGL, background color | ✅ Done |
| 1 | Home screen (6 tiles, swipe navigation, statusbar on `lv_layer_top`) | ✅ Done |
| 2 | Metering screen (L/R bars, goniometer, 60 s loudness history EBU R128, numerics) | ✅ Done |
| 3 | WiFi + UDP audio stream (30 Hz) + Spectrum screen (bars / curve / waterfall) | ✅ Done |
| 4 | Metering Pro: engine/skin architecture, phosphor goniometer, VU/PPM ballistics | ✅ Done |
| Touch | GT911 via I2C address fallback, swipe navigation, touch indicator in statusbar | ✅ Done |
| 5 | USB MIDI Class Device (TinyUSB) — CC output from USB MIDI screen | ✅ Done |
| 6 | Studio One WebSocket integration | Planned |
| 7 | Behringer WING OSC integration | Planned |

---

## Screens

| Screen | Access | Description |
|---|---|---|
| **Home** | Boot | 6-tile grid, swipe left to navigate forward |
| **Metering** | Swipe from Home | L/R bars, phosphor goniometer, loudness history, I/S/M/Peak numerics |
| **Spectrum** | Swipe from Metering | Bars / FFT curve / waterfall, 4 color presets |
| **Studio One** | Home tile | DAW transport mockup (BPM, position, play/stop) |
| **USB MIDI** | Home tile | 8 CC faders (Nord Lead 2X), SEND CC via USB MIDI |
| **Routing** | Home tile | WING mixer mockup, 8 channel strips |
| **Settings** | Home tile | WiFi / audio / system / build info, live 1 Hz update |
| **Dev Control** | Home tile | Developer tools |
| **Gradient Test** | Settings → Gradient → | IPS brightness calibration (diagnostic) |

All screens share a persistent statusbar (top 32 px on `lv_layer_top`) and a foot bar
with a **← Home** button.

---

## Project Structure

```
ESP32-S3/
├── studio-panel/           ESP-IDF project root
│   ├── flash.sh            Flash script (OPI-Flash safe, with verify)
│   ├── main/
│   │   ├── main.cpp            Boot sequence
│   │   ├── board.h             All GPIO pin definitions
│   │   ├── theme.h/cpp         Colors, fonts, geometry — swap at runtime
│   │   ├── ui.cpp              LVGL init + screen launch
│   │   ├── display.cpp         RGB panel init (3 frame buffers in PSRAM)
│   │   ├── touch.cpp           GT911 init (0x5D → 0x14 fallback)
│   │   ├── ch422g.cpp          I/O expander (backlight, I2C scan)
│   │   ├── wifi.cpp/h          WiFi station, auto-reconnect
│   │   ├── net_receiver.cpp/h  UDP task (port 4210, xQueueOverwrite)
│   │   ├── audio_data.h/cpp    Shared AudioPacket struct + FreeRTOS queue
│   │   ├── usb_midi_driver.h/cpp  TinyUSB MIDI Class Device init + send_cc()
│   │   └── screens/
│   │       ├── home.cpp/h
│   │       ├── statusbar.cpp/h
│   │       ├── metering.cpp/h
│   │       ├── meter_engine.cpp/h
│   │       ├── skin_digital.cpp/h
│   │       ├── skin_vu.cpp/h
│   │       ├── spectrum.cpp/h
│   │       ├── studio_one.cpp/h
│   │       ├── usb_midi.cpp/h
│   │       ├── routing.cpp/h
│   │       ├── settings.cpp/h
│   │       ├── dev_control.cpp/h
│   │       ├── touch_nav.cpp/h
│   │       ├── foot.cpp/h
│   │       └── gradient_test.cpp/h
├── tools/
│   ├── studio-panel-sender.py  UDP audio analysis sender (Linux + macOS)
│   └── requirements.txt
├── docs/
│   ├── development-notes.md    Critical bring-up findings (read this first)
│   └── skills/                 Step-by-step workflows (flash-verify, etc.)
└── README.md
```

---

## Build & Flash

```bash
# 1. Activate ESP-IDF (once per shell session)
source ~/esp/esp-idf-5.5/export.sh

# 2. Navigate to project
cd studio-panel

# 3. Build
idf.py build

# 4. Flash (use flash.sh — raw idf.py flash is unreliable on OPI flash)
bash flash.sh

# 5. Verify the right firmware is running
timeout 8 idf.py -p /dev/ttyACM0 monitor 2>&1 | grep APP_BUILD
# Expected: APP_BUILD=<date> <time>

# If permission denied on /dev/ttyACM0:
newgrp dialout
```

> **OPI Flash warning:** `idf.py flash` reports success even when the write
> fails silently on this board's OPI flash. Always use `flash.sh`, which runs
> esptool with `--no-stub --verify`. Check `APP_BUILD` in the monitor output
> to confirm the new firmware is actually running — **not** the bootloader
> compile time (which never changes).

> **sdkconfig:** Never commit `sdkconfig` — it contains local paths. All
> required settings (PSRAM, flash size, fonts, TinyUSB) are in
> `sdkconfig.defaults` which is committed.

---

## USB MIDI

The ESP32-S3 enumerates as a **USB MIDI 1.0 Class Device** via the native
USB OTG port (USB-C connector). No driver needed on Linux/macOS/Windows 11.

On the **USB MIDI** screen, 8 fader strips map to MIDI CC values for the
Nord Lead 2X. Press **SEND CC** to transmit all 8 current values on MIDI
channel 1. The USB MIDI device appears as "Studio Panel MIDI".

---

## Companion Script

```bash
pip install sounddevice numpy
python3 tools/studio-panel-sender.py --host <ESP32_IP>
python3 tools/studio-panel-sender.py --list   # show audio devices
```

Streams real-time audio analysis (FFT, RMS, loudness, goniometer) to the
panel via UDP on port 4210 at ~30 Hz. Cross-platform (Linux PipeWire/PulseAudio,
macOS CoreAudio). Falls back to animated demo data when no stream is active.

---

## LVGL 9 on ESP32-S3 — Critical Pitfalls

### Component adapter
Use `espressif/esp_lvgl_adapter`, not `espressif/esp_lvgl_port`. The port
adapter checks `SOC_LCDCAM_RGB_LCD_SUPPORTED` which doesn't exist on ESP32-S3.

### Theme must be disabled
```cpp
lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
lv_display_set_theme(disp, nullptr);  // must come before any screen creation
```

### Never use lv_screen_active() as parent
Always create a fresh screen:
```cpp
lv_obj_t *scr = lv_obj_create(nullptr);
lv_obj_remove_style_all(scr);
// ... add content ...
lv_screen_load(scr);
```

### IPS panel minimum brightness
The panel shows a greenish tint below ~38% luminance. Minimum background:
`0x606060`. This applies to canvas backgrounds too (`0x630C` in RGB565).

### Frame buffers must be in PSRAM
`cfg.flags.fb_in_psram = 1` — requires `CONFIG_SPIRAM=y` + `CONFIG_SPIRAM_MODE_OCT=y`
in sdkconfig. Three frame buffers at 800×480×2 = ~2.3 MB total.

### OPI Flash silent failures
`idf.py flash` can report success while writing garbage. Use `flash.sh`.

---

## Known Issues

1. **CH422G SDA stuck-low** — I2C bus physically degraded. Backlight runs
   via hardware default. Needs logic analyzer to diagnose. Does not affect
   touch (GT911 works via I2C address fallback without CH422G reset).

2. **WiFi occasional connect failure** — auto-reconnect active, usually
   connects within 5 s on retry.

3. **IPS greenish tint below 38% luminance** — all UI colors kept ≥ `0x606060`.

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
        │  USB MIDI│      │   Wi-Fi    │    │  Future    │
        │  CC/SysEx│      │  UDP/OSC/  │    │  USB HID   │
        │  → synths│      │  WebSocket │    │  (macros)  │
        └──────────┘      └─────┬──────┘    └────────────┘
                                │
                   ┌────────────┼────────────┐
                   │            │            │
            ┌──────▼──┐  ┌─────▼───┐  ┌────▼──────┐
            │Studio One│  │  WING  │  │  mioXL   │
            │  (DAW)  │  │(Mixer) │  │ (Router) │
            └─────────┘  └────────┘  └──────────┘
```

Target gear: Studio One, Behringer WING Rack, iConnectivity mioXL,
Nord Lead 2X, Nord Electro, Novation Bass Station, Novation DrumStation,
Arturia KeyStep Pro, Arturia BeatStep Pro, MPC One+.

---

## Design Goals

- Dark studio aesthetic — not a smartphone app
- High contrast, minimal color palette
- Large touch targets (≥ 44 px)
- Smooth swipe animations
- Feels like a dedicated hardware device

---

## License

MIT
