# Phase 3: WiFi + UDP + Spectrum Screen — Design Spec
**Datum:** 2026-08-23
**Status:** Bereit für Implementierung

---

## Ziel

WiFi-Konnektivität und einen UDP-Datenempfänger auf dem ESP32-S3 einrichten. Ein Ubuntu/Mac-Companion-Script analysiert Audio in Echtzeit und schickt Metering-Werte + FFT-Bins per UDP ans Panel. Der bestehende Metering-Screen wechselt von Demo-Daten auf echte Daten (Demo bleibt als Fallback). Ein neuer Spectrum-Screen mit drei Visualisierungsviews (Swipe-Navigation) wird hinzugefügt.

---

## Nicht in dieser Phase

- 2D-Navigationsraster (eigene Phase)
- USB CDC-ECM (Kabel-Alternative zu WiFi, eigene Phase)
- Touch-basiertes Tap-to-Freeze (GT911 noch nicht initialisiert — BOOT-Button als Fallback)
- Konfigurierbare WiFi-Credentials ohne Rebuild (kommt mit Phase 7 Config-Server)
- Multi-Touch-Gesten

---

## UDP-Paketformat

Binär-Struct, kein JSON-Overhead. Größe: **1076 Bytes**. Rate: ~30 Hz.

```
Offset  Bytes  Typ       Inhalt
────────────────────────────────────────────────────────
0       1      uint8     Magic = 0xAB
1       1      uint8     Version = 1
2       2      uint16    Flags: Bit0=FFT present, Bit1=Gonio present
4       4      uint32    Sequence number (für Drop-Erkennung)
── Metering (36 Bytes) ──────────────────────────────────
8       4      float32   peak_l       (dBFS)
12      4      float32   peak_r       (dBFS)
16      4      float32   rms_l        (dBFS)
20      4      float32   rms_r        (dBFS)
24      4      float32   momentary    (LKFS, 400ms)
28      4      float32   short_term   (LKFS, 3s)
32      4      float32   integrated   (LKFS, kumulativ)
36      4      float32   gonio_l      (raw sample -1..1)
40      4      float32   gonio_r      (raw sample -1..1)
── FFT (1028 Bytes) ─────────────────────────────────────
44      4      uint32    fft_bins     (= 256, Validierung)
48      1024   float32×256  bins[256] (Magnitude 0.0..1.0, log-skaliert)
────────────────────────────────────────────────────────
Total: 1076 Bytes
```

**Paket-Validierung auf ESP32:**
- `buf[0] == 0xAB` (Magic)
- `buf[1] == 1` (Version)
- `len == 1076`
- Bei Fehler: verwerfen, Counter erhöhen

---

## Neue Dateien (ESP32)

| Datei | Verantwortung |
|---|---|
| `main/audio_data.h` | `AudioPacket` Struct + `QueueHandle_t g_audio_queue` (extern) |
| `main/wifi.cpp / .h` | WiFi-Init, Event-Handler, Auto-Reconnect, Statusbar-Update |
| `main/net_receiver.cpp / .h` | UDP-Task, Paket-Parser, Queue-Write |
| `main/screens/spectrum.cpp / .h` | Spectrum-Screen: 3 Views, Swipe, Context-Menu, Freeze |

## Geänderte Dateien (ESP32)

| Datei | Änderung |
|---|---|
| `main/audio_data.cpp` | `QueueHandle_t g_audio_queue` Definition |
| `main/main.cpp` | `wifi_init()` + `net_receiver_start()` im Boot |
| `main/screens/metering.cpp` | Demo-Tick gegen Queue-Read ersetzen (Demo als Fallback) |
| `main/screens/home.cpp` | Spectrum-Tile verlinken (`spectrum_screen_create`) |
| `main/ui.cpp` | Boot-Screen wieder auf Home (Touch-Workaround rückgängig) |
| `sdkconfig.defaults` | WiFi-Config + lwIP-Stack-Größe |

## Neues Companion-Script

| Datei | Inhalt |
|---|---|
| `tools/studio-panel-sender.py` | Python 3, `sounddevice` + `numpy` + `struct` + `socket` |
| `tools/requirements.txt` | `sounddevice`, `numpy` |

---

## Komponente 1: `audio_data.h`

```cpp
#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

struct AudioPacket {
    uint8_t  magic;
    uint8_t  version;
    uint16_t flags;
    uint32_t seq;
    float    peak_l, peak_r;
    float    rms_l, rms_r;
    float    momentary, short_term, integrated;
    float    gonio_l, gonio_r;
    uint32_t fft_bins;
    float    bins[256];
} __attribute__((packed));

static_assert(sizeof(AudioPacket) == 1076, "AudioPacket size mismatch");

extern QueueHandle_t g_audio_queue;  // capacity=2, xQueueOverwrite für neuestes Paket
```

---

## Komponente 2: `wifi.cpp`

**WiFi-Konfiguration in `sdkconfig.defaults`:**
```
CONFIG_ESP_WIFI_SSID="MeinNetzwerk"
CONFIG_ESP_WIFI_PASSWORD="MeinPasswort"
CONFIG_ESP_WIFI_AUTH_WPA2_PSK=y
CONFIG_LWIP_SO_RCVBUF=y
```

**Verhalten:**
- `wifi_init()`: Station-Mode, Event-Loop starten, `esp_wifi_connect()`
- Bei `WIFI_EVENT_STA_DISCONNECTED`: automatisch `esp_wifi_connect()` retry
- Bei `IP_EVENT_STA_GOT_IP`: `net_receiver_start()` aufrufen + `statusbar_update_wifi(true)`
- Bei Disconnect: `statusbar_update_wifi(false)`

**IO-Contracts:**
- `wifi_init()` — muss vor `app_main` Event-Loop aufgerufen werden; blockiert nicht
- `wifi_is_connected()` — thread-safe, liest atomic flag

---

## Komponente 3: `net_receiver.cpp`

**UDP-Task** (Core 0, Stack 4096, Priorität 5):
```
Socket öffnen auf 0.0.0.0:4210 (SO_REUSEADDR)
Loop:
  recvfrom() → buf[1076]
  Validierung: Magic + Version + Länge
  xQueueOverwrite(g_audio_queue, &pkt)   ← immer neuestes Paket
  Drop-Counter wenn seq nicht monoton steigt
  Alle 300 Pakete (~10s): ESP_LOGI mit Drop-Rate
```

**`net_receiver_start()`** — erstellt Task; idempotent (zweiter Aufruf no-op wenn Task läuft).

**Screens lesen Queue** im LVGL-Timer:
```cpp
AudioPacket pkt;
if (xQueuePeek(g_audio_queue, &pkt, 0) == pdTRUE) {
    // echte Daten übernehmen
} else {
    // Demo-Fallback
}
```
`xQueuePeek` (nicht `xQueueReceive`) — Paket bleibt in Queue für parallel lesende Screens.

---

## Komponente 4: Metering-Screen Update

`metering_demo_tick()` bleibt erhalten. Im LVGL-Timer-Callback:

```cpp
AudioPacket pkt;
if (xQueuePeek(g_audio_queue, &pkt, 0) == pdTRUE) {
    data->state.peak_l     = pkt.peak_l;
    data->state.peak_r     = pkt.peak_r;
    data->state.rms_l      = pkt.rms_l;
    data->state.rms_r      = pkt.rms_r;
    data->state.momentary  = pkt.momentary;
    data->state.short_term = pkt.short_term;
    data->state.integrated = pkt.integrated;
    data->state.l_sample   = pkt.gonio_l;
    data->state.r_sample   = pkt.gonio_r;
    // history_tick + history ring buffer update wie bisher
} else {
    metering_demo_tick(data->state, DT);
}
```

History-Ring-Buffer und Goniometer-Ring-Buffer laufen weiter unverändert — sie konsumieren `l_sample`/`r_sample` und `short_term` wie bisher.

---

## Komponente 5: Spectrum-Screen

### SpectrumScreenData

```cpp
struct SpectrumScreenData {
    lv_obj_t   *view_bars;      // View 1
    lv_obj_t   *view_curve;     // View 2
    lv_obj_t   *view_waterfall; // View 3
    lv_obj_t   *freeze_icon;
    lv_timer_t *timer;

    float       smoothed[256];  // exponential MA über Frames (alle 3 Views)
    bool        frozen;

    // Waterfall
    void       *wf_buf;         // PSRAM canvas buffer
    uint8_t     color_preset;   // 0=Klassisch 1=Grün 2=Warm 3=Lila
    bool        wf_rtl;         // true = Right→Left (Default), false = Top→Bottom

    // Peak hold für Analyzer
    float       peak_hold[256];
    float       peak_hold_timer[256];
};
```

### View 1 — Klassischer Analyzer (`spectrum_bars_draw`)

- 256 Balken auf 800px Breite (~3px/Balken + 0px Gap)
- **Logarithmische Frequenz-Remapping**: bins werden auf log-Skala verteilt (20Hz–20kHz), niedrige Frequenzen bekommen mehr Platz
- Farbe nach Amplitude: `0x205020` (leise) → `0x50A050` → `0xC8A030` → `0xE05050` (laut)
- Peak-Hold: weißer 1px-Marker, 2s Freeze, dann 20dB/s Decay
- `lv_obj_t` mit `LV_EVENT_DRAW_MAIN` custom callback, `lv_obj_invalidate` im Timer

### View 2 — FFT-Kurve (`spectrum_curve_draw`)

- Gefüllte Area-Chart: obere Linie + gefüllte Fläche darunter
- Hintergrund: `0x0A0A0A` (reine Visualisierungsfläche, kein Touch-Target — IPS-Luminanz-Regel gilt nicht)
- Farb-Gradient der Füllung: `0x0A0A2A` (Basis) → `0x1A3A8A` → `0x40A0C0` (Spitze)
- Glättung: α = 0.4 (träger als Analyzer — Kurve fließt statt zu springen)
- Gezeichnet mit `lv_draw_line` Segment-für-Segment über 256 Punkte

### View 3 — Waterfall (`spectrum_waterfall_draw`)

**Default: Right→Left**
- X-Achse: Zeit (rechts = neu, links = alt)
- Y-Achse: Frequenz (unten = Bass, oben = Treble)
- `lv_canvas` 800×448px, PSRAM-Buffer: 800×448×2 = 716,800 Bytes (~700KB) — im PSRAM (8MB vorhanden)
- Pro Frame: `memmove(buf, buf + 2, (W-1)*H*2)` — shift 1 Spalte links, neue Spalte rechts füllen
- Neue Spalte: 448 Pixel, Farbe = `color_preset_lut[preset][bin_to_y(f)] * magnitude`

**Farbpresets (LUTs, je 256 Einträge `lv_color_t`):**
- 0 — Klassisch: Schwarz → Blau → Cyan → Gelb → Rot → Weiß
- 1 — Grün: Schwarz → Dunkelgrün → Hellgrün → Weiß
- 2 — Warm: Schwarz → Dunkelrot → Orange → Gelb → Weiß
- 3 — Lila: Schwarz → Dunkelviolett → Magenta → Weiß

### Context-Menu (Long-Press auf Waterfall-View)

Overlay auf `lv_layer_top`, verschwindet bei Tap außerhalb:

```
┌─────────────────────────────────────────┐
│  Farbschema                             │
│  [████] [████] [████] [████]           │
│  60×30px Swatches, sofort aktiv bei Tap │
│                                         │
│  Richtung   [←→]  [↓↑]                │
└─────────────────────────────────────────┘
```

- Swatch = `lv_obj_t` mit Gradient-Hintergrund (repräsentiert das Preset visuell)
- Aktives Preset bekommt weißen 2px Border
- `LV_EVENT_LONG_PRESSED` auf dem Waterfall-Container (500ms Hold)

### Freeze-Modus

- BOOT-Button (GPIO0): GPIO-Interrupt togglet `data->frozen`
- Wenn `frozen == true`: Timer-Callback liest Queue + updated `smoothed[]`, aber `lv_obj_invalidate()` wird nicht aufgerufen — Display bleibt eingefroren
- Freeze-Icon (❚❚): `lv_label` oben rechts, sichtbar nur wenn `frozen == true`
- Später: `LV_EVENT_CLICKED` auf View als zweiter Trigger (wenn Touch läuft)

### Navigation zwischen Views

```cpp
// Swipe Right → nächster View
lv_screen_load_anim(next_view, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
// Swipe Left → vorheriger View
lv_screen_load_anim(prev_view, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, false);
// Back-Button → Metering-Screen
lv_screen_load_anim(metering, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, true);
```

Da Touch noch nicht funktioniert: Navigation vorübergehend über BOOT-Button (kurz = nächster View, lang = zurück zu Metering). Wenn Touch aktiv: Swipe-Gesten via `LV_EVENT_GESTURE`.

---

## Komponente 6: Companion-Script (`tools/studio-panel-sender.py`)

```
Argumente:
  --host     ESP32-IP (default: studio-panel.local oder hardcoded 192.168.x.x)
  --port     4210
  --bins     256 (konfigurierbar)
  --rate     30 (fps)
  --device   Audio-Device (default: System-Monitor automatisch erkannt)
  --list     Verfügbare Devices anzeigen

Ablauf:
  1. sounddevice.query_devices() → System-Monitor finden (loopback/monitor)
  2. sounddevice.InputStream öffnen (stereo, 44100 Hz, blocksize=1470 für ~30Hz)
  3. Pro Block:
     a. numpy FFT (1470 → 256 Bins, Hanning-Fenster, log-Magnitude normiert)
     b. Peak/RMS berechnen (dBFS)
     c. LUFS approximieren (RMS-basiert, K-weighted vereinfacht)
     d. struct.pack → UDP senden
```

**Plattform:** Linux (PipeWire/PulseAudio via PortAudio) + Mac (CoreAudio via PortAudio). Identischer Code, PortAudio abstrahiert die Unterschiede.

**Dependencies:** `pip install sounddevice numpy` (keine weiteren)

---

## sdkconfig.defaults Ergänzungen

```
# WiFi
CONFIG_ESP_WIFI_SSID="MeinNetzwerk"
CONFIG_ESP_WIFI_PASSWORD="MeinPasswort"
CONFIG_ESP_WIFI_AUTH_WPA2_PSK=y

# lwIP / Sockets
CONFIG_LWIP_SO_RCVBUF=y
CONFIG_LWIP_UDP_RECVMBOX_SIZE=16

# Waterfall Canvas braucht mehr PSRAM-Heap
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0
```

---

## Boot-Sequenz (main.cpp)

```
ch422g_init()
display_init()
touch_init()          ← weiterhin versuchen (für späteres Touch)
ch422g_backlight_on()
wifi_init()           ← NEU: non-blocking, startet Event-Loop
ui_init()             ← lädt Home-Screen (kein Metering-Workaround mehr)
                         net_receiver_start() kommt via WiFi-Event-Callback
```

---

## Spec Self-Review

**Placeholder-Scan:** Keine TBDs. WiFi-Credentials als Platzhalter-SSID explizit markiert.

**Konsistenz:**
- `xQueuePeek` (nicht `xQueueReceive`) in beiden Screens — Paket bleibt verfügbar ✓
- PSRAM-Größe: Gonio 125KB + Waterfall 700KB = 825KB von 8MB — unkritisch ✓
- BOOT-Button (GPIO0) für zwei Funktionen (Freeze + View-Navigation): kurzem Druck = View-Navigation, langem Druck (>1s) = Freeze-Toggle. Klar trennbar ✓
- Demo-Fallback in Metering: `metering_demo_tick` bleibt, wird nur übersprungen wenn Queue voll ✓

**Scope:** Eine Implementierungseinheit. WiFi + UDP + Spectrum sind eng gekoppelt (Datenfluss). ✓

**Ambiguität:** `xQueuePeek` ist non-destructive — beide Screens (Metering + Spectrum) können gleichzeitig lesen. Bei gleichzeitig geöffnetem Screen kein Problem da nur ein Screen aktiv ist. ✓
