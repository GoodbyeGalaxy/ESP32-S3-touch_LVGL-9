# Development Notes — ESP32-S3 Studio Panel

Erkenntnisse aus dem Hardware-Bringup. Jeder dieser Punkte hat Zeit gekostet.

---

## ESP-IDF + LVGL Stack

### esp_lvgl_port Version muss gepinnt werden

`espressif/esp_lvgl_port: "^2.4.0"` zieht je nach Zeitpunkt 2.9.0 — und das bricht auf dem ESP32-S3.

**Problem:** Version 2.9.0 prüft `SOC_LCDCAM_RGB_LCD_SUPPORTED`, das auf ESP32-S3 nicht existiert. ESP32-S3 hat `SOC_LCD_RGB_SUPPORTED`. Ergebnis: `lvgl_port_add_disp_rgb()` gibt immer NULL zurück mit der Meldung "This target does not support RGB."

**Lösung in `idf_component.yml`:**
```yaml
espressif/esp_lvgl_port: ">=2.3.0,<2.5.0"
```

**Wichtig:** Nach Versionsänderung `dependencies.lock` löschen und `managed_components/` löschen, dann `idf.py fullclean && idf.py build`.

---

### LVGL 8 oder 9?

`esp_lvgl_port >=2.3.0,<2.5.0` zieht aktuell `lvgl/lvgl 9.5.0` — nicht LVGL 8.3 wie oft in Tutorials gezeigt. LVGL 9 hat Breaking Changes.

**LVGL 9 API-Änderungen die hier relevant sind:**

| LVGL 8 | LVGL 9 |
|---|---|
| `lv_scr_act()` | `lv_screen_active()` |
| `lv_disp_t` | `lv_display_t` |
| `lvgl_port_cfg_t.task_stack_size` | `lvgl_port_cfg_t.task_stack` |
| `lv_disp_set_default()` | `lv_display_set_default()` |

---

### LVGL Task Stack zu klein → stiller Absturz

8192 Bytes reichen für LVGL 9 + RGB-Panel nicht aus. Der LVGL-Task startet, druckt "Starting LVGL task" und hängt dann — kein Fehler, kein Reboot, nur Schweigen.

**Lösung:** `task_stack = 16384` in `lvgl_port_cfg_t`.

---

### LVGL Default-Theme überschreibt alles

Auch nach `lv_obj_remove_style_all()` und `lv_obj_set_style_bg_color()` ist der Hintergrund noch grün. Das liegt am Default-Theme das beim Display-Init auf alle Objekte angewendet wurde.

**Lösung: Theme deaktivieren BEVOR Screens erstellt werden:**
```cpp
s_disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
lv_display_set_default(s_disp);
lv_display_set_theme(s_disp, nullptr);  // Theme aus
```

**Und dann einen frischen Screen erstellen** (nicht `lv_screen_active()` verwenden — der hat bereits Theme-Styles):
```cpp
lv_obj_t *scr = lv_obj_create(nullptr);  // neues Screen-Objekt
lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A0A0A), 0);
lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
// ... Widgets hinzufügen ...
lv_screen_load(scr);  // am Ende laden
```

---

## ESP-IDF I2C + C++

### `esp_lcd_new_panel_io_i2c()` ist in C++ nicht verwendbar

ESP-IDF v5.2 definiert `esp_lcd_new_panel_io_i2c` als `_Generic`-Macro (C11-Feature). In C++17 kompiliert das nicht.

**Fehlermeldung:**
```
error: '_Generic' was not declared in this scope
```

**Lösung:** Die zugrundeliegende Funktion direkt aufrufen:
```cpp
// Statt:
esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)BSP_I2C_PORT, &io_cfg, &tp_io)
// So:
esp_lcd_new_panel_io_i2c_v1(BSP_I2C_PORT, &io_cfg, &tp_io)
```

---

### `scl_speed_hz` im Legacy-I2C-Treiber nicht setzen

Beim Legacy-I2C-Treiber (`i2c_driver_install` / `i2c_param_config`) wird die Taktfrequenz in `i2c_param_config` gesetzt. In der `esp_lcd_panel_io_i2c_config_t` darf `scl_speed_hz` NICHT gesetzt werden — der Treiber gibt sonst `ESP_ERR_INVALID_ARG` zurück.

**Fehlermeldung:**
```
E: scl_speed_hz is not need to set in legacy i2c_lcd driver
```

---

## Hardware: Waveshare ESP32-S3-Touch-LCD-7

### CH422G IO-Expander

Der CH422G ist kein Standard-I2C-Slave. Er antwortet bei einem normalen I2C-Scanner (`i2c_master_write_to_device` mit einem Byte) nicht zuverlässig — der Scanner findet ihn möglicherweise nicht, obwohl er vorhanden ist.

Das Display-Backlight kann dennoch über den Hardware-Default-Zustand des CH422G aktiv sein, auch wenn Software-Schreibzugriffe fehlschlagen.

**Wenn CH422G nicht antwortet:**
- Espressif-Komponent `espressif/esp_io_expander_ch422g` verwenden statt Raw-I2C
- Alternativ: Logikanalysator am I2C-Bus
- Alternative Adressen prüfen: 0x24 ist für Output, aber CH422G hat mehrere Adressen für verschiedene Operationen

### GT911 Touch-Controller

GT911 hat zwei mögliche I2C-Adressen: `0x5D` und `0x14`. Welche aktiv ist, hängt vom Pegel des INT-Pins während des Resets ab. Ohne funktionierenden CH422G-Reset ist die Adresse unbestimmt.

**Board-spezifische Pins (ESP32-S3-Touch-LCD-7):**
- SDA: GPIO8, SCL: GPIO9
- Touch INT: GPIO4
- Touch RST: via CH422G EXIO1 (kein direkter GPIO)

### Farb-Kalibrierung

IPS-Panel zeigt bei reinem Rot (0xFF0000) leicht Orange/Mandarin. Das ist die natürliche Farbcharakteristik dieses Panels und kein Software-Problem. Weiß (0xFFFFFF) und Schwarz (0x000000) erscheinen korrekt.

---

## LVGL Hintergrundfarbe (offenes Problem)

Das PSRAM initialisiert sich mit einem zufälligen Muster (oft grün = `0x07E0` in RGB565). LVGL rendert mit einem kleinen Buffer (40 Zeilen) nur "dirty areas" — der Rest des Framebuffers bleibt beim PSRAM-Initialzustand.

**Was NICHT funktioniert:**
- `lv_obj_set_style_bg_color()` direkt auf Screen-Objekte
- Framebuffer-Pre-fill via `esp_lcd_rgb_panel_get_frame_buffer()` + `esp_cache_msync()`
- `esp_lcd_panel_draw_bitmap()` als Pre-fill
- `full_refresh = true` (verursacht Cycling/Flackern)
- `direct_mode = true` (verursacht Artefakte ohne Vsync-Sync)
- `lv_display_set_theme(disp, nullptr)` + `lv_obj_create(nullptr)` als neuer Screen

**Was als nächstes evaluiert wird:**
- LVGL Custom Theme mit dunklem Hintergrund (so machen es alle funktionierenden GitHub-Projekte)
- `esp_lv_adapter` Komponente (Waveshare-eigene Abstraktion, nutzt andere Flush-Logik)

**Merke:** Kein funktionierendes GitHub-Projekt setzt `lv_obj_set_style_bg_color` direkt auf Screen-Objekte für den Haupthintergrund — sie nutzen ausschließlich das LVGL Theme-System.

---

## Entwicklungsworkflow

### Partition-Tabelle bei erstem Flash

Wenn das Board vorher andere Firmware hatte, kann die alte Partition-Tabelle inkompatibel sein:
```
E: invalid segment length 0xffffffff
E: Factory app partition is not bootable
```

**Lösung:** Flash komplett löschen vor erstem Flashen:
```bash
idf.py -p /dev/ttyACM0 erase-flash
idf.py -p /dev/ttyACM0 flash
```

### Serial-Port Zugriff (Linux)

Nach `sudo usermod -aG dialout $USER` muss `newgrp dialout` in jedem neuen Terminal ausgeführt werden (oder neu einloggen). Ohne das: `Permission denied` auf `/dev/ttyACM0`.

### Monitor beenden

In `idf.py monitor`: `Ctrl+]` (nicht `Ctrl+C`).

### Komponenten-Versionen wechseln

Nach Änderung der Versionen in `idf_component.yml`:
```bash
rm dependencies.lock
rm -rf managed_components/
idf.py fullclean
idf.py build
```

---

## Measurement Accuracy

A detailed analysis of what each meter actually computes and how it compares to professional
standards (BS.1770-4 / EBU R128, True Peak, etc.) is in:

→ [`docs/measurement-accuracy.md`](measurement-accuracy.md)

Short version: RMS and M/S are professional grade. Loudness (LUFS) is an approximation
(no K-weighting, no gating). Spectrum is frame-relative, not calibrated to dBFS. Peak
has a sender-side squaring bug (~6 dB off). Integrated loudness is broken (hardcoded constant).

---

## Screen Architecture (UI-Polish / Phase 5+)

### Settings + DevCtrl merged into SYSTEM screen

The two separate home-row entries "Settings" and "Device Ctrl" were merged into a single
"SYSTEM" screen (`screens/system.cpp`). The merged screen contains cards for Network (WiFi
status + RSSI + toggle), Audio In, Chip, Memory, and a full-width System overview line.

Nav grid: Row 2 reduced from 3 columns to 2 columns:
```
Row 2: [SYSTEM col 0] ↔ [Gradient Test col 1]
```

### Spectrum — 6 unified modes

The BOOT-button-based spectrum navigation was replaced with a single unified spectrum screen
(`screens/spectrum.cpp`) containing 6 modes selectable via centered foot pills:
Bars · Curve · LED · Waterfall · Octave · Spectro.

The separate `meter_led.cpp` was removed. The LED mode disclaimer ("Relative display — not
calibrated to dBFS") is shown in amber text below the LED visualization.

Guide lines (−3/−6/−12 dB relative to frame peak) are available via the settings overlay.
A LUFS momentary badge is always visible in the top-right corner of the spectrum screen.
