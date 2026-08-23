# ESP32-S3 Studio Panel — Claude Instructions

**PFLICHT beim Sessionstart:** Lies zuerst `docs/development-notes.md` — dort stehen alle kritischen Erkenntnisse aus dem Hardware-Bringup. Ohne dieses Dokument wirst du dieselben Fehler wiederholen.

Phase 0+1+2+3 abgeschlossen. Nächstes Ziel: **Phase 4 (USB HID)** oder **Touch-Fix (CH422G/GT911)**.

## Kurzübersicht

- **Board:** Waveshare ESP32-S3-Touch-LCD-7 (800×480, GT911, CH422G)
- **Stack:** C++17, ESP-IDF v5.5, LVGL 9.5 via espressif/esp_lvgl_adapter 0.5.2
- **Projekt:** `studio-panel/` — bitte immer von dort aus arbeiten
- **ESP-IDF aktivieren:** `source ~/esp/esp-idf-5.5/export.sh` (oder `source ~/.bashrc`)
- **Serial-Port:** `/dev/ttyACM0` — `newgrp dialout` falls Permission denied
- **WiFi-Credentials:** `studio-panel/main/Kconfig.projbuild` → `idf.py menuconfig` oder direkt editieren
- **Audio-Script:** `tools/studio-panel-sender.py --host <ESP32-IP>` (Linux + macOS)

## Stand: ABGESCHLOSSEN (2026-08-23)

✅ Phase 0: Display, LVGL, Hintergrundfarbe.
✅ Phase 1: Home Screen (6 Kacheln 3×2, Slide-Navigation, Statusleiste auf lv_layer_top).
✅ Phase 2: Metering Screen (L/R Balken, Goniometer/Lissajous, Loudness-History 60s EBU R128, Numerik I/S/M/Peak).
✅ Phase 3: WiFi + UDP (30Hz, 1072 Bytes), Metering mit echten Daten, Spectrum Screen (Balken/Kurve/Wasserfall, 4 Farbpresets, BOOT-Button Navigation).
🎯 Nächstes Ziel: Phase 4 — USB HID (TinyUSB) oder Touch-Fix.

**Dev-Workaround aktiv:** `ui.cpp` bootet direkt in Metering. Für Home-Screen: `home_screen_load()` in ui.cpp wiederherstellen.

**Alle Farben ≥ 38% Luminanz** — IPS-Panel zeigt darunter grünen Tint. Ausnahme: reine Visualisierungsflächen (Waterfall-Canvas, FFT-Kurve). Statusleiste: 0x686868, BG: 0x606060, Cards: 0x747474.

## Offene Probleme (Stand: 2026-08-23)

1. **CH422G** antwortet nicht auf I2C (ESP_FAIL) — Backlight läuft über Hardware-Default, Touch-Reset schlägt fehl → GT911 nicht initialisiert. Diagnose braucht Logikanalysator.
2. **GT911** Touch nicht initialisiert (hängt an CH422G-Reset)
3. **IPS-Panel Schwarzpunkt** — Grünlicher Tint unterhalb ~38% Luminanz. Minimale Hintergrundfarbe: `0x606060`. NIEMALS `0x0A0A0A` oder ähnlich dunkle Werte (Ausnahme: reine Canvas-Flächen).

## Wichtig: xQueueOverwrite braucht Queue-Länge = 1
`g_audio_queue = xQueueCreate(1, sizeof(AudioPacket))` — NICHT 2. xQueueOverwrite() asserted bei Länge ≠ 1.

## Kritische LVGL-Regeln (hart gelernt)

- **NIEMALS `lv_screen_active()` als Basis** — hat Theme-Padding, PSRAM-Grün scheint an Rändern durch
- **IMMER `lv_obj_create(NULL)` + `lv_obj_remove_style_all()` + `lv_screen_load(scr)` am Ende**
- **IMMER `lv_display_set_theme(disp, nullptr)`** nach `esp_lv_adapter_register_display()`
- Framebuffer nach `esp_lcd_panel_init()` vorbelegen um PSRAM-Artefakte zu vermeiden

## Build

```bash
cd /mnt/source/data/coding/ESP32-S3/studio-panel
idf.py build 2>&1 | grep -E " error:|fatal error" | head -20
idf.py -p /dev/ttyACM0 flash
timeout 20 idf.py -p /dev/ttyACM0 monitor 2>&1 | grep -v "^---"
```
