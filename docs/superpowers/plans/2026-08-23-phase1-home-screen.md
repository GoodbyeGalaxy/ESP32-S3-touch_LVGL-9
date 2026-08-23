# Studio Panel – Phase 1: Home Screen + Navigation

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Home Screen mit 6 Modul-Kacheln, konsistentem Dark-Theme-System, Statusleiste und Slide-Navigation zu Platzhalter-Screens.

**Architecture:** Theme-Konstanten in `theme.h` → Screen-Factory-Funktion → 6 Tile-Objekte + 6 Modul-Screens → Statusleiste auf `lv_layer_top()` (persists across transitions). Navigation: `lv_screen_load_anim` mit MOVE_LEFT/MOVE_RIGHT und `auto_del = true`.

**Tech Stack:** C++17, ESP-IDF v5.5, LVGL 9.x via `esp_lvgl_adapter`, ESP32-S3 (800×480)

**Spec:** `../../kickstarter.md` §13 (Design), §10–11 (Phasen), `../../docs/development-notes.md` (kritische Erkenntnisse)

---

## Global Constraints

- Target: ESP32-S3 (Waveshare ESP32-S3-Touch-LCD-7, 800×480)
- Sprache: C++17, kein RTTI, kein Exceptions
- LVGL: 9.x via `esp_lvgl_adapter` — Lock immer mit `esp_lv_adapter_lock(-1)` / `esp_lv_adapter_unlock()`
- **Screen-Erstellung:** Immer `lv_obj_create(nullptr)` + `lv_screen_load()` — NIEMALS `lv_screen_active()` als Basis-Screen (grüner Hintergrund Bug, development-notes.md)
- **Screen-Navigation:** `lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, true)` — `auto_del = true` immer setzen, nie stale Pointer halten
- Design (§13 kickstarter.md): `#0A0A0A` Background, hoher Kontrast, min. 60×60 px Touch-Ziele, Montserrat-Fonts, kein Hobby-Look
- GPIO-Konstanten: nur in `board.h`, nirgendwo sonst

---

## Dateistruktur nach Phase 1

```
main/
├── theme.h                    # NEU: alle Design-Konstanten
├── screens/
│   ├── home.h / home.cpp      # ÄNDERN: 6 Kacheln statt Hello-Text
│   ├── statusbar.h / statusbar.cpp  # NEU: persistente Statusleiste
│   ├── metering.h / metering.cpp    # NEU: Platzhalter
│   ├── studio_one.h / studio_one.cpp  # NEU: Platzhalter
│   ├── usb_midi.h / usb_midi.cpp     # NEU: Platzhalter
│   ├── routing.h / routing.cpp       # NEU: Platzhalter
│   ├── dev_control.h / dev_control.cpp  # NEU: Platzhalter
│   └── settings.h / settings.cpp     # NEU: Platzhalter
└── ui.cpp                     # ÄNDERN: statusbar_init() hinzufügen
```

---

## Task 1: Theme-System

**Files:** `main/theme.h`

**Interfaces:**
- Consumes: nichts
- Produces: `THEME_*` Konstanten, `theme_make_screen()` Helper

- [ ] **Step 1: `main/theme.h` erstellen**

```cpp
#pragma once
#include "lvgl.h"

// ── Farben ────────────────────────────────────────────────────
#define THEME_BG_PRIMARY      lv_color_hex(0x0A0A0A)   // Screen-Hintergrund
#define THEME_BG_CARD         lv_color_hex(0x141414)   // Kacheln / Cards
#define THEME_BG_CARD_HOVER   lv_color_hex(0x1E1E1E)   // Pressed-State
#define THEME_ACCENT          lv_color_hex(0x3B82F6)   // Primärfarbe (Blau)
#define THEME_ACCENT_DIM      lv_color_hex(0x1E3A5F)   // Gedämpftes Akzent
#define THEME_TEXT_PRIMARY    lv_color_hex(0xF0F0F0)   // Haupttext
#define THEME_TEXT_SECONDARY  lv_color_hex(0x666666)   // Sekundärtext
#define THEME_TEXT_HINT       lv_color_hex(0x333333)   // Phase-Hinweise
#define THEME_SEPARATOR       lv_color_hex(0x1A1A1A)   // Trennlinien

// ── Geometrie ─────────────────────────────────────────────────
#define THEME_TILE_W          236    // Kachelbreite (3 × 236 + 2 × 36 = 800)
#define THEME_TILE_H          196    // Kachelhöhe   (2 × 196 + 3 × 29 = 479)
#define THEME_TILE_GAP        36     // Horizontaler Abstand
#define THEME_TILE_GAP_V      29     // Vertikaler Abstand
#define THEME_STATUSBAR_H     32     // Höhe der Statusleiste
#define THEME_RADIUS          8      // Globaler Border-Radius

// ── Fonts ─────────────────────────────────────────────────────
#define THEME_FONT_TITLE      (&lv_font_montserrat_24)
#define THEME_FONT_LABEL      (&lv_font_montserrat_14)
#define THEME_FONT_HINT       (&lv_font_montserrat_14)

// ── Helper: neuen Screen mit Dark-Background erstellen ────────
// Gibt IMMER lv_obj_create(nullptr) zurück — nie lv_screen_active() verwenden.
static inline lv_obj_t *theme_make_screen()
{
    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, THEME_BG_PRIMARY, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}
```

- [ ] **Step 2: Prüfen ob Fonts in `sdkconfig.defaults` aktiviert**

```ini
CONFIG_LV_FONT_MONTSERRAT_24=y
CONFIG_LV_FONT_MONTSERRAT_14=y
```

Falls nicht → `idf.py menuconfig` → Component Config → LVGL → Fonts.

---

## Task 2: Statusleiste

**Files:** `main/screens/statusbar.h`, `main/screens/statusbar.cpp`

**Interfaces:**
- Consumes: LVGL (lv_layer_top), `THEME_*`
- Produces: `statusbar_init()`, `statusbar_update_wifi(bool)`, `statusbar_update_time(const char*)`

Die Statusleiste lebt auf `lv_layer_top()` — überlagert alle Screens und überlebt Navigationsübergänge.

- [ ] **Step 1: `main/screens/statusbar.h`**

```cpp
#pragma once

void statusbar_init();
void statusbar_update_wifi(bool connected);
void statusbar_update_time(const char *time_str);
```

- [ ] **Step 2: `main/screens/statusbar.cpp`**

```cpp
#include "statusbar.h"
#include "theme.h"
#include "lvgl.h"

static lv_obj_t *s_bar   = nullptr;
static lv_obj_t *s_title = nullptr;
static lv_obj_t *s_wifi  = nullptr;
static lv_obj_t *s_time  = nullptr;

void statusbar_init()
{
    lv_obj_t *top = lv_layer_top();

    s_bar = lv_obj_create(top);
    lv_obj_set_size(s_bar, LV_HOR_RES, THEME_STATUSBAR_H);
    lv_obj_set_pos(s_bar, 0, 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x080808), 0);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_bar, 0, 0);
    lv_obj_set_style_pad_all(s_bar, 0, 0);
    lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);

    // Titel links
    s_title = lv_label_create(s_bar);
    lv_label_set_text(s_title, "STUDIO PANEL");
    lv_obj_set_style_text_color(s_title, THEME_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_font(s_title, THEME_FONT_HINT, 0);
    lv_obj_align(s_title, LV_ALIGN_LEFT_MID, 12, 0);

    // Zeit rechts
    s_time = lv_label_create(s_bar);
    lv_label_set_text(s_time, "--:--");
    lv_obj_set_style_text_color(s_time, THEME_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_font(s_time, THEME_FONT_HINT, 0);
    lv_obj_align(s_time, LV_ALIGN_RIGHT_MID, -12, 0);

    // WiFi-Status
    s_wifi = lv_label_create(s_bar);
    lv_label_set_text(s_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(s_wifi, THEME_TEXT_HINT, 0);
    lv_obj_align(s_wifi, LV_ALIGN_RIGHT_MID, -50, 0);
}

void statusbar_update_wifi(bool connected)
{
    if (!s_wifi) return;
    lv_obj_set_style_text_color(s_wifi,
        connected ? THEME_ACCENT : THEME_TEXT_HINT, 0);
}

void statusbar_update_time(const char *time_str)
{
    if (!s_time) return;
    lv_label_set_text(s_time, time_str);
}
```

- [ ] **Step 3: `statusbar_init()` in `ui.cpp` aufrufen**

In `ui_init()`, nach `esp_lv_adapter_lock`:
```cpp
if (esp_lv_adapter_lock(-1) == ESP_OK) {
    statusbar_init();
    home_screen_load();   // Phase 1: lädt statt direkt erstellt
    esp_lv_adapter_unlock();
}
```

---

## Task 3: Sechs Modul-Screens (Platzhalter)

**Files:** je `screens/<name>.h` + `screens/<name>.cpp` für: metering, studio_one, usb_midi, routing, dev_control, settings

**Interface je Screen:**
- `lv_obj_t *<name>_screen_create()` — erstellt Screen, gibt Pointer zurück
- Screen hat HOME-Button → navigiert zurück mit `MOVE_RIGHT`

- [ ] **Step 1: Template für alle 6 Screens**

Beispiel `main/screens/metering.cpp` (alle anderen analog):

```cpp
#include "metering.h"
#include "theme.h"
#include "screens/home.h"
#include "lvgl.h"

static void on_back(lv_event_t *e)
{
    lv_obj_t *home = home_screen_create();
    lv_screen_load_anim(home, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, true);
}

lv_obj_t *metering_screen_create()
{
    lv_obj_t *scr = theme_make_screen();

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "METERING");
    lv_obj_set_style_text_color(title, THEME_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_TITLE, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, THEME_STATUSBAR_H + 24);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "Phase 5 — Peak / RMS / LUFS");
    lv_obj_set_style_text_color(hint, THEME_TEXT_HINT, 0);
    lv_obj_set_style_text_font(hint, THEME_FONT_HINT, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 100, 44);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, 16, -16);
    lv_obj_set_style_bg_color(btn, THEME_BG_CARD, 0);
    lv_obj_add_event_cb(btn, on_back, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, LV_SYMBOL_LEFT " Home");
    lv_obj_set_style_text_color(btn_label, THEME_TEXT_PRIMARY, 0);
    lv_obj_center(btn_label);

    return scr;
}
```

`metering.h`:
```cpp
#pragma once
#include "lvgl.h"
lv_obj_t *metering_screen_create();
```

- [ ] **Step 2: Alle 6 Screens erstellen**

| Datei | Titel | Phase-Hint |
|---|---|---|
| `metering.cpp` | METERING | Phase 5 — Peak / RMS / LUFS |
| `studio_one.cpp` | STUDIO ONE | Phase 5 — WebSocket / Audio-Helper |
| `usb_midi.cpp` | USB MIDI | Phase 3 — CC-Fader / Nord Lead 2X |
| `routing.cpp` | ROUTING | Phase 6 — WING Integration |
| `dev_control.cpp` | DEVICE CONTROL | Phase 4 — JSON Profiles / SysEx |
| `settings.cpp` | SETTINGS | Phase 7 — HTTP Config / OTA |

---

## Task 4: Home Screen — 6 Kacheln

**Files:** `main/screens/home.h` (anpassen), `main/screens/home.cpp` (neu)

**Interface:**
- `lv_obj_t *home_screen_create()` — gibt Screen zurück (aufgerufen von ui.cpp und Modul-Screens)

- [ ] **Step 1: `main/screens/home.h` anpassen**

```cpp
#pragma once
#include "lvgl.h"

// Erstellt den Home-Screen und gibt ihn zurück.
// Aufrufer lädt ihn mit lv_screen_load() oder lv_screen_load_anim().
lv_obj_t *home_screen_create();

// Convenience: Erstellt + lädt sofort (ohne Animation, für Boot)
void home_screen_load();
```

- [ ] **Step 2: `main/screens/home.cpp` neu schreiben**

```cpp
#include "home.h"
#include "theme.h"
#include "screens/metering.h"
#include "screens/studio_one.h"
#include "screens/usb_midi.h"
#include "screens/routing.h"
#include "screens/dev_control.h"
#include "screens/settings.h"
#include "lvgl.h"

struct TileDef {
    const char *symbol;
    const char *label;
    const char *hint;
    lv_obj_t *(*create_screen)();
};

static const TileDef TILES[6] = {
    { LV_SYMBOL_AUDIO,    "METERING",       "Pegel / RMS / LUFS",   metering_screen_create   },
    { LV_SYMBOL_PLAY,     "STUDIO ONE",     "DAW Control",          studio_one_screen_create },
    { LV_SYMBOL_SHUFFLE,  "USB MIDI",       "CC / Nord Lead 2X",    usb_midi_screen_create   },
    { LV_SYMBOL_ROUTE,    "ROUTING",        "WING Integration",     routing_screen_create    },
    { LV_SYMBOL_SETTINGS, "DEVICE CTRL",    "JSON Profiles",        dev_control_screen_create},
    { LV_SYMBOL_SETTINGS, "SETTINGS",       "Config / OTA",         settings_screen_create   },
};

static void on_tile_clicked(lv_event_t *e)
{
    auto *def = static_cast<const TileDef *>(lv_event_get_user_data(e));
    lv_obj_t *target = def->create_screen();
    lv_screen_load_anim(target, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, true);
}

lv_obj_t *home_screen_create()
{
    lv_obj_t *scr = theme_make_screen();

    for (int i = 0; i < 6; i++) {
        int col = i % 3;
        int row = i / 3;

        int x = col * (THEME_TILE_W + THEME_TILE_GAP);
        int y = THEME_STATUSBAR_H + THEME_TILE_GAP_V
              + row * (THEME_TILE_H + THEME_TILE_GAP_V);

        lv_obj_t *tile = lv_obj_create(scr);
        lv_obj_set_size(tile, THEME_TILE_W, THEME_TILE_H);
        lv_obj_set_pos(tile, x, y);
        lv_obj_set_style_bg_color(tile, THEME_BG_CARD, 0);
        lv_obj_set_style_bg_color(tile, THEME_BG_CARD_HOVER, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(tile, 0, 0);
        lv_obj_set_style_radius(tile, THEME_RADIUS, 0);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);

        // Symbol
        lv_obj_t *sym = lv_label_create(tile);
        lv_label_set_text(sym, TILES[i].symbol);
        lv_obj_set_style_text_color(sym, THEME_ACCENT, 0);
        lv_obj_set_style_text_font(sym, THEME_FONT_TITLE, 0);
        lv_obj_align(sym, LV_ALIGN_TOP_MID, 0, 28);

        // Label
        lv_obj_t *lbl = lv_label_create(tile);
        lv_label_set_text(lbl, TILES[i].label);
        lv_obj_set_style_text_color(lbl, THEME_TEXT_PRIMARY, 0);
        lv_obj_set_style_text_font(lbl, THEME_FONT_LABEL, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

        // Hint
        lv_obj_t *hint = lv_label_create(tile);
        lv_label_set_text(hint, TILES[i].hint);
        lv_obj_set_style_text_color(hint, THEME_TEXT_HINT, 0);
        lv_obj_set_style_text_font(hint, THEME_FONT_HINT, 0);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);

        lv_obj_add_event_cb(tile, on_tile_clicked, LV_EVENT_CLICKED,
                            const_cast<TileDef *>(&TILES[i]));
    }

    return scr;
}

void home_screen_load()
{
    lv_obj_t *scr = home_screen_create();
    lv_screen_load(scr);
}
```

- [ ] **Step 3: `ui.cpp` anpassen — `home_screen_load()` statt `home_screen_create()`**

```cpp
if (esp_lv_adapter_lock(-1) == ESP_OK) {
    statusbar_init();
    home_screen_load();
    esp_lv_adapter_unlock();
}
```

- [ ] **Step 4: `main/CMakeLists.txt` — neue Dateien eintragen**

```cmake
idf_component_register(
    SRCS
        "main.cpp"
        "ch422g.cpp"
        "display.cpp"
        "touch.cpp"
        "ui.cpp"
        "screens/home.cpp"
        "screens/statusbar.cpp"
        "screens/metering.cpp"
        "screens/studio_one.cpp"
        "screens/usb_midi.cpp"
        "screens/routing.cpp"
        "screens/dev_control.cpp"
        "screens/settings.cpp"
    ...
)
```

---

## Task 5: Build + Verifikation

- [ ] **Step 1: Bauen**

```bash
idf.py build
```

Expected: kein Error, kein Warning für fehlende Symbole.

- [ ] **Step 2: Flashen + Monitor**

```bash
idf.py -p /dev/ttyACM0 flash
```

```bash
timeout 15 idf.py -p /dev/ttyACM0 monitor 2>&1 | grep -v "^---"
```

- [ ] **Step 3: Erfolgs-Kriterien**

```
Home Screen sichtbar             ✓  6 Kacheln auf dunklem Hintergrund
Statusleiste sichtbar            ✓  "STUDIO PANEL" oben links
Kachel antippen → Slide-Left     ✓  Modul-Screen öffnet sich
HOME-Button → Slide-Right        ✓  Home-Screen kommt zurück
Touch-Ziele ≥ 60×60 px          ✓  Kacheln 236×196 px
```

- [ ] **Step 4: Committen**

```bash
git add -A
git commit -m "feat: Phase 1 — Home Screen mit 6 Kacheln, Statusleiste, Screen-Navigation"
```

---

## Risiken & Bekannte Fallstricke

| Risiko | Gegenmassnahme |
|---|---|
| `LV_SYMBOL_AUDIO` / `LV_SYMBOL_ROUTE` nicht in LVGL 9 | Alternativ `LV_SYMBOL_SOUND`, `LV_SYMBOL_SHUFFLE` oder Text-Label statt Symbol |
| Screen-Pointer nach `auto_del=true` ungültig | Nie Screen-Pointer cachen — immer `create_screen()` neu aufrufen |
| Statusleiste nach Screen-Wechsel verschwunden | `lv_layer_top()` ist display-global, kein Re-Init nötig — nur einmalig aufrufen |
| Kacheln scrollen statt clicken | `lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE)` auf Screen UND Kacheln |
| Hintergrundfarbe falsch (bekannter Bug) | `theme_make_screen()` nutzt `lv_obj_create(nullptr)` — NICHT `lv_screen_active()` |

---

## Phasen-Übersicht

| Plan-Datei | Phase | Inhalt |
|---|---|---|
| `phase0` | 0 | ✅ Toolchain, Display, Touch, Hello World |
| **`phase1`** | **1** | **Dieser Plan — 6 Kacheln, Navigation, Statusleiste** |
| `phase2` | 2 | USB HID via TinyUSB: Keyboard-Shortcuts |
| `phase3` | 3 | USB MIDI: CC-Fader → Nord Lead 2X |
| `phase4` | 4 | JSON-Geräteprofile, SysEx-Template-Engine |
| `phase5` | 5 | WebSocket: Peak/RMS/LUFS visualisieren |
| `phase7` | 7 | HTTP Config-Server, JSON-Layouts vom NAS |
| `phase6` | 6 | WING-Integration — **zuletzt** |
