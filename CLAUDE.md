# ESP32-S3 Studio Panel — Claude Instructions

**PFLICHT beim Sessionstart:** Lies zuerst `docs/development-notes.md` — dort stehen alle kritischen Erkenntnisse aus dem Hardware-Bringup. Ohne dieses Dokument wirst du dieselben Fehler wiederholen.

**Projekt-Skills:** `docs/skills/` — Schritt-für-Schritt-Workflows für wiederkehrende Aufgaben.
- `docs/skills/flash-verify.md` — Flash + Verifikation (OPI-Flash meldet Erfolg auch bei Fehler!)

Phase 0+1+2+3+4(Metering Pro)+Touch+5(USB MIDI)+UI-Polish abgeschlossen. Nächstes Ziel: **Phase 6 (Studio One WebSocket)**. Danach: Theme-Templating (Laufzeit-Farbwechsel, mehrere ThemeColors-Instanzen).

## Kurzübersicht

- **Board:** Waveshare ESP32-S3-Touch-LCD-7 (800×480, GT911, CH422G)
- **Stack:** C++17, ESP-IDF v5.5, LVGL 9.5 via espressif/esp_lvgl_adapter 0.5.2
- **Projekt:** `studio-panel/` — bitte immer von dort aus arbeiten
- **ESP-IDF aktivieren:** `source ~/esp/esp-idf-5.5/export.sh` (oder `source ~/.bashrc`)
- **Serial-Port:** `/dev/ttyACM0` — `newgrp dialout` falls Permission denied
- **WiFi-Credentials:** NUR in `studio-panel/sdkconfig.defaults.local` (gitignored) — NIEMALS in Kconfig.projbuild
- **Audio-Script:** `python3 /mnt/source/data/coding/ESP32-S3/tools/studio-panel-sender.py --host <ESP32-IP>` (Linux + macOS, IP sichtbar in Settings-Screen)
- **GitHub Remote:** `origin` = `https://github.com/GoodbyeGalaxy/ESP32-S3-touch_LVGL-9.git`

## Stand: ABGESCHLOSSEN (2026-08-27)

✅ Phase 0: Display, LVGL, Hintergrundfarbe.
✅ Phase 1: Home Screen (6 Kacheln 3×2, Slide-Navigation, Statusleiste auf lv_layer_top).
✅ Phase 2: Metering Screen (L/R Balken, Goniometer/Lissajous, Loudness-History 60s EBU R128, Numerik I/S/M/Peak).
✅ Phase 3: WiFi + UDP (30Hz, 1072 Bytes), Metering mit echten Daten, Spectrum Screen (Balken/Kurve/Wasserfall, 4 Farbpresets, BOOT-Button Navigation).
✅ Phase 4 (Metering Pro): Engine/Skin-Architektur, Phosphor-Goniometer, Ballistic Modes (dBFS/VU/PPM I+II), Spectral Balance Strip, Fonts, IP in Statusbar.
✅ Phase 5 (USB MIDI): TinyUSB MIDI Class Device via `espressif/esp_tinyusb`. `usb_midi_driver.h/cpp` erstellt. SEND CC Button in `usb_midi.cpp` funktional.
✅ UI-Polish: 2D-Navigation (nav_controller), Foot-Bar, Gradient-Test, Theme-System, VU-Verbesserungen, SNTP-Zeit.
🎯 Nächstes Ziel: Phase 6 — Studio One WebSocket.

**Hintergrund FINAL:** `0x0A0A0A` (theme.cpp bg_primary + foot_bg) — nach GPIO8/9-Fix bestätigt sicher. Canvas-BG = `0x0841` (RGB565). Nicht weiter experimentieren.

## Offene Probleme (Stand: 2026-08-26)

1. **CH422G** antwortet nicht auf I2C (ESP_FAIL) — Backlight läuft über Hardware-Default. Kein Reset möglich, aber Touch läuft trotzdem (siehe unten).
2. **GT911 FUNKTIONIERT** — initialisiert via Adress-Fallback (0x5D→0x14) ohne CH422G-Reset. touch.cpp probiert beide Adressen automatisch.
3. **IPS-Panel Schwarzpunkt** — Nach GPIO8/9-Fix (war fälschlicherweise I2C): `0x0A0A0A` als BG bestätigt sicher. Canvas-Flächen = `0x0841` (RGB565 von 0x080808). Reine Visualisierungsflächen (FFT, Waterfall) können tiefer gehen.
4. **Drift (Bild verschiebt sich horizontal)** — PSRAM-Bandbreiten-Contention zwischen LCD-DMA und CPU-PSRAM-Zugriffen. Fix: `bounce_buffer_size_px = LCD_H_RES * 4` in display.cpp. Große PSRAM-Arrays in DRAM allozieren wenn möglich.
5. **CH422G SDA stuck-low** — I2C-Bus physisch defekt. Nur Logikanalysator hilft. Backlight läuft über Hardware-Default ohne I2C.

## sdkconfig.defaults — Vollständigkeit kritisch

Nach `rm sdkconfig && idf.py build` werden **nur** Keys aus `sdkconfig.defaults` übernommen — fehlende Keys fallen auf ESP-IDF-Defaults zurück, was zu schwer debuggbaren Fehlern führt. Folgende Keys sind zwingend:

```
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y          # default wäre 2MB → Flash-Fehler
CONFIG_SPIRAM=y                            # ohne PSRAM → nur Backlight sichtbar
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y
CONFIG_LV_FONT_MONTSERRAT_14=y
CONFIG_LV_FONT_MONTSERRAT_24=y
CONFIG_LV_FONT_UNSCII_16=y
CONFIG_TINYUSB_MIDI_COUNT=1                # für Phase 5 USB MIDI
```

## TinyUSB / USB MIDI

Komponentenname für CMakeLists.txt REQUIRES ist **nicht** `tinyusb`. Stattdessen via `idf_component.yml` einbinden:
```yaml
espressif/esp_tinyusb: "^1.1"
```
Nur `CONFIG_TINYUSB_MIDI_COUNT=1` in sdkconfig.defaults nötig. Kein manueller REQUIRES-Eintrag.

## flash.sh — set -e + grep Bug

`set -e` kombiniert mit `grep` ohne Treffer → exit code 1 → Script bricht ab. Fix: `|| true` am Ende jeder grep-Zeile im Script.

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
