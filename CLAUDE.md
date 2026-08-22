# ESP32-S3 Studio Panel — Claude Instructions

**PFLICHT beim Sessionstart:** Lies zuerst `docs/development-notes.md` — dort stehen alle kritischen Erkenntnisse aus dem Hardware-Bringup. Ohne dieses Dokument wirst du dieselben Fehler wiederholen.

Danach lies `docs/superpowers/plans/2026-08-22-phase0-hello-world.md` für den Projektstand.

## Kurzübersicht

- **Board:** Waveshare ESP32-S3-Touch-LCD-7 (800×480, GT911, CH422G)
- **Stack:** C++17, ESP-IDF v5.2, LVGL 9.5 via esp_lvgl_port 2.4.4
- **Projekt:** `studio-panel/` — bitte immer von dort aus arbeiten
- **ESP-IDF aktivieren:** `source ~/.bashrc` (export.sh ist eingetragen)
- **Serial-Port:** `/dev/ttyACM0` — `newgrp dialout` falls Permission denied

## Offene Probleme (Stand: 2026-08-22)

1. **Hintergrundfarbe grün** — LVGL rendert nur dirty areas, PSRAM-Initialzustand (grün) scheint durch. Lösung: Custom LVGL Theme evaluieren, ggf. `esp_lv_adapter`
2. **CH422G** antwortet nicht auf I2C — Backlight/Touch-Reset ohne Software-Control
3. **GT911** Touch nicht initialisiert (hängt an CH422G)
4. **Debug-Code** noch in ch422g.cpp (I2C-Scanner) und ui.cpp (Lock-Logging)

## Build

```bash
cd /mnt/source/data/coding/ESP32-S3/studio-panel
idf.py build
idf.py -p /dev/ttyACM0 flash
timeout 20 idf.py -p /dev/ttyACM0 monitor 2>&1 | grep -v "^---"
```
