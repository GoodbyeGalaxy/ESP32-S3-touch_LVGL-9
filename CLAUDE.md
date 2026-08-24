# ESP32-S3 Studio Panel — Claude Instructions

**PFLICHT beim Sessionstart:** Lies zuerst `docs/development-notes.md` — dort stehen alle kritischen Erkenntnisse aus dem Hardware-Bringup. Ohne dieses Dokument wirst du dieselben Fehler wiederholen.

**Projekt-Skills:** `docs/skills/` — Schritt-für-Schritt-Workflows für wiederkehrende Aufgaben.
- `docs/skills/flash-verify.md` — Flash + Verifikation (OPI-Flash meldet Erfolg auch bei Fehler!)

Phase 0+1+2+3+4(Metering Pro)+Touch abgeschlossen. Nächstes Ziel: **Phase 5 (USB HID)** oder **VU-Meter Skin**.

## Kurzübersicht

- **Board:** Waveshare ESP32-S3-Touch-LCD-7 (800×480, GT911, CH422G)
- **Stack:** C++17, ESP-IDF v5.5, LVGL 9.5 via espressif/esp_lvgl_adapter 0.5.2
- **Projekt:** `studio-panel/` — bitte immer von dort aus arbeiten
- **ESP-IDF aktivieren:** `source ~/esp/esp-idf-5.5/export.sh` (oder `source ~/.bashrc`)
- **Serial-Port:** `/dev/ttyACM0` — `newgrp dialout` falls Permission denied
- **WiFi-Credentials:** NUR in `studio-panel/sdkconfig.defaults.local` (gitignored) — NIEMALS in Kconfig.projbuild
- **Audio-Script:** `tools/studio-panel-sender.py --host <ESP32-IP>` (Linux + macOS)

## Stand: ABGESCHLOSSEN (2026-08-23)

✅ Phase 0: Display, LVGL, Hintergrundfarbe.
✅ Phase 1: Home Screen (6 Kacheln 3×2, Slide-Navigation, Statusleiste auf lv_layer_top).
✅ Phase 2: Metering Screen (L/R Balken, Goniometer/Lissajous, Loudness-History 60s EBU R128, Numerik I/S/M/Peak).
✅ Phase 3: WiFi + UDP (30Hz, 1072 Bytes), Metering mit echten Daten, Spectrum Screen (Balken/Kurve/Wasserfall, 4 Farbpresets, BOOT-Button Navigation).
✅ Phase 4 (Metering Pro): Engine/Skin-Architektur, Phosphor-Goniometer, Ballistic Modes (dBFS/VU/PPM I+II), Spectral Balance Strip, Fonts, IP in Statusbar.
🎯 Nächstes Ziel: Phase 5 — USB HID (TinyUSB) oder Touch-Fix.

**Dev-Workaround aktiv:** `ui.cpp` bootet direkt in Metering. Für Home-Screen: `home_screen_load()` in ui.cpp wiederherstellen.

**Hintergrund FINAL:** H=0, S=0, V=38 in `theme.h`. Alle Kompensationen exhaustiv getestet (Magenta H=300, Blau H=220, Navy H=240, V=0–36) — alle schlimmer als neutrales Grau. Nicht weiter experimentieren.

**Alle Farben ≥ 38% Luminanz** — IPS-Panel zeigt darunter grünen Tint. Ausnahme: reine Visualisierungsflächen (Waterfall-Canvas, FFT-Kurve). Statusleiste: 0x686868, BG: 0x606060, Cards: 0x747474.

## Offene Probleme (Stand: 2026-08-23)

1. **CH422G** antwortet nicht auf I2C (ESP_FAIL) — Backlight läuft über Hardware-Default. Kein Reset möglich, aber Touch läuft trotzdem (siehe unten).
2. **GT911 FUNKTIONIERT** — initialisiert via Adress-Fallback (0x5D→0x14) ohne CH422G-Reset. touch.cpp probiert beide Adressen automatisch.
3. **IPS-Panel Schwarzpunkt** — Grünlicher Tint unterhalb ~38% Luminanz. Minimale Hintergrundfarbe: `0x606060`. NIEMALS `0x0A0A0A` oder ähnlich dunkle Werte. **AUCH Canvas-Flächen betroffen** — Canvas-Hintergrund = `0x630C` (RGB565 von 0x606060), nicht 0x0000!
4. **Drift (Bild verschiebt sich horizontal)** — PSRAM-Bandbreiten-Contention zwischen LCD-DMA und CPU-PSRAM-Zugriffen. Fix: `bounce_buffer_size_px = LCD_H_RES * 4` in display.cpp. Große PSRAM-Arrays in DRAM allozieren wenn möglich.
5. **CH422G SDA stuck-low** — I2C-Bus physisch defekt. Nur Logikanalysator hilft. Backlight läuft über Hardware-Default ohne I2C.

## Fonts (Stand 2026-08-24)

`THEME_FONT_TITLE`=Montserrat 24, `THEME_FONT_LABEL`=Montserrat 18, `THEME_FONT_HINT`=Montserrat 14, `THEME_FONT_NUM`=unscii_16 (Zahlenwerte, pixel-perfect).
Labels: immer `lv_obj_remove_style_all()`. Zentrierte Labels: `lv_label_set_long_mode(LV_LABEL_LONG_CLIP)` + explizite Größe setzen — sonst Flackern/Scrollen.

**Build-Verifikation:** `APP_BUILD` ändert sich nur wenn `main.cpp` rekompiliert wird. Besser: `strings build/studio-panel.bin | grep "APP_BUILD"` oder Binary-Timestamp prüfen (`stat`). Builds dauern 3–5 min — immer background bash (`run_in_background: true`).

**Subagent-Permissions:** `settings.json`-Änderungen brauchen Session-Neustart. Subagenten erben neue Permissions erst danach.

## Wichtig: xQueueOverwrite braucht Queue-Länge = 1
`g_audio_queue = xQueueCreate(1, sizeof(AudioPacket))` — NICHT 2. xQueueOverwrite() asserted bei Länge ≠ 1.

## Kritische LVGL-Regeln (hart gelernt)

- **NIEMALS `lv_screen_active()` als Basis** — hat Theme-Padding, PSRAM-Grün scheint an Rändern durch
- **IMMER `lv_obj_create(NULL)` + `lv_obj_remove_style_all()` + `lv_screen_load(scr)` am Ende**
- **IMMER `lv_display_set_theme(disp, nullptr)`** nach `esp_lv_adapter_register_display()`
- Framebuffer nach `esp_lcd_panel_init()` vorbelegen um PSRAM-Artefakte zu vermeiden

## Build & Flash

```bash
cd /mnt/source/data/coding/ESP32-S3/studio-panel
source ~/esp/esp-idf-5.5/export.sh 2>/dev/null
idf.py build 2>&1 | grep -E " error:|fatal error" | head -20
idf.py -p /dev/ttyACM0 -b 115200 flash
```

## Flash-Verifikation (KRITISCH — OPI-Flash meldet Erfolg auch bei Fehler)

`idf.py flash` kann lautlos scheitern. Immer verifizieren:

```bash
# 1. Nur App-Binary mit Verifikation schreiben (sicherer als idf.py flash)
python3 ~/esp/esp-idf-5.5/components/esptool_py/esptool/esptool.py \
  --chip esp32s3 --port /dev/ttyACM0 -b 115200 --no-stub \
  write_flash --verify 0x10000 build/studio-panel.bin

# 2. Firmware-Version im Monitor prüfen
timeout 8 idf.py -p /dev/ttyACM0 monitor 2>&1 | grep "APP_BUILD"
```

**ACHTUNG:** `I (32) boot: compile time` ist der **Bootloader** — ändert sich nicht bei App-Updates.
**Richtige Verifikation:** `APP_BUILD=Aug 23 2026 HH:MM:SS` im Monitor (aus `app_main` geloggt).

### Flash scheitert → Lösungsreihenfolge
1. `idf.py -p /dev/ttyACM0 -b 115200 flash` (normal)
2. BOOT+RESET → `idf.py -p /dev/ttyACM0 flash` (Download-Mode)
3. `--no-stub write_flash --verify 0x10000 build/studio-panel.bin` (OPI-Flash direkt)
4. `idf.py erase-flash` → USB neu einstecken im BOOT-Mode → flash
