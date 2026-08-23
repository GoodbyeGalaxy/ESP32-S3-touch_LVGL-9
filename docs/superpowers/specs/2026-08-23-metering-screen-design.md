# Metering Screen — Design Spec
**Datum:** 2026-08-23
**Phase:** 2 (ursprünglich USB HID — verschoben, Metering vorgezogen)
**Status:** Bereit für Implementierung

---

## Ziel

Den Metering-Screen (`main/screens/metering.cpp`) von einem Placeholder zu einem vollständigen Broadcast-Metering-Display ausbauen. Datenquelle: simulierte Demo-Daten über einen LVGL-Timer. Die Datenquelle ist als austauschbare Schicht ausgelegt — echte Audio-Daten (z.B. via WiFi/UDP) können später durch Ersatz einer einzigen Funktion eingesteckt werden.

---

## Screen-Layout (800×480)

```
┌────────────────────────────────────────────────────────────────┐
│                       Status Bar (40px)                        │
├────────────┬───────────────────────────────┬───────────────────┤
│  L   R     │   Goniometer (250×250)        │  I: −14.2 LKFS   │
│  │▓▓│ │▓▓│ │   (Lissajous)                 │  S: −13.8 LKFS   │
│  │▓▓│ │▓▓│ │                              │  M: −12.1 LKFS   │
│  │▓▓│ │▓▓│ ├───────────────────────────────┤  Peak: −5.9 dBFS │
│            │  Short-term Loudness / 60s   │                   │
│            │  [scrollender Balken-Graph]  │                   │
│            │  Target: −23 LKFS ─────────  │                   │
├────────────┴───────────────────────────────┴───────────────────┤
│ ◁ Home                                                         │
└────────────────────────────────────────────────────────────────┘
```

**Grobe Maße:**
- L/R Balken: je ~90px breit, ~340px hoch, links mit 16px Rand
- Goniometer: 250×250px, oben Mitte
- Loudness-History: ~430px breit, ~140px hoch, unten Mitte
- Numerik: ~170px breit, rechts mit 16px Rand
- Back-Button: unten links, 100×44px

---

## Datenmodell

### Struct `MeteringState` (intern in metering.cpp)

```cpp
struct MeteringState {
    // Rohdaten (normalisiert, -1.0 .. 1.0)
    float l_sample;
    float r_sample;

    // Peak mit Hold
    float peak_l;           // aktueller Peak (dBFS)
    float peak_r;
    float peak_hold_l;      // eingefrorener Peak-Hold-Wert
    float peak_hold_r;
    float peak_hold_timer;  // Sekunden bis Decay-Start (max 3.0)

    // RMS (300ms gleitend, in dBFS)
    float rms_l;
    float rms_r;

    // Loudness (alle in LKFS / dBFS)
    float momentary;        // M: 400ms Integration
    float short_term;       // S: 3s Integration
    float integrated;       // I: kumulativ

    // Loudness-History für 60s-Graph
    float short_term_history[60];  // Ring-Buffer, 1 Wert/s
    int   history_head;            // aktueller Schreibindex
    float history_tick;            // Akkumulator für 1s-Takt
};
```

### Demo-Datengenerator

Wird von einem `lv_timer_t` mit 30ms Intervall (≈30 Hz) aufgerufen. Die Funktion `metering_demo_tick(MeteringState&, float dt)`:

1. **L/R Sample:** `l = sin(2π · t · 0.7)`, `r = sin(2π · t · 0.7 + phase_offset)` wobei `phase_offset` langsam zwischen 0 und π/2 driftet (~0.3 rad/s). Amplitude variiert mit einer langsamen Hüllkurve (~0.1 Hz Sinus).
2. **Peak:** `peak = max(peak · decay^dt, |sample_db|)` mit `decay = pow(0.001, 1/2.0)` (2s auf −60 dBFS). Attack: sofort.
3. **Peak-Hold:** Wenn neuer Peak > Hold-Wert → Hold aktualisieren, Timer reset auf 3.0s. Sonst Timer dekrementieren; bei 0 → Hold folgt Decay.
4. **RMS:** Gleitender Mittelwert der letzten 300ms (ca. 9 Samples bei 30 Hz): `rms = sqrt(α · sample² + (1−α) · rms_sq)` → in dBFS.
5. **Momentary (M):** Analog zu RMS, aber 400ms Fenster, K-Weighted (in Demo: vereinfacht = gewichteter RMS).
6. **Short-term (S):** 3s-Fenster, träge.
7. **Integrated (I):** Kumulativer Durchschnitt seit Screen-Start, träge um −14 LKFS.
8. **History-Tick:** Alle 1s wird `short_term_history[history_head] = short_term` gesetzt, `history_head = (history_head + 1) % 60`.

**Austauschpunkt:** `metering_demo_tick()` ist die einzige Funktion die Messwerte schreibt. Um echte Daten einzustecken, wird diese Funktion durch eine ersetzt die Daten aus einem Queue liest (z.B. von einem WiFi/UDP-Task).

---

## LVGL-Widgets

### 1. L/R Pegelbalken

- Je ein `lv_bar_t` pro Kanal, vertikal (`LV_BAR_MODE_NORMAL`), Wertebereich −60 bis 0 (dBFS × 10 als Integer).
- RMS-Wert steuert den `lv_bar_set_value()`.
- Peak-Hold-Marker: `LV_EVENT_DRAW_TASK_ADDED` custom draw callback auf dem Bar-Parent — zeichnet eine horizontale Linie an der Peak-Hold-Position.
- Farbzonen (via `lv_bar` Indicator-Style oder custom draw):
  - Grün: −60 bis −9 dBFS
  - Gelb: −9 bis −3 dBFS
  - Rot: −3 bis 0 dBFS

### 2. Goniometer

- `lv_canvas_t`, 250×250px, Farbraum `LV_COLOR_FORMAT_ARGB8888`.
- Ring-Buffer mit letzten 80 `(m, s)`-Koordinatenpaaren, wobei:
  - `M = (L + R) / sqrt(2)` (Mono/Mid)
  - `S = (L - R) / sqrt(2)` (Side)
- Jeden Timer-Tick:
  1. `lv_canvas_fill_bg()` mit `lv_color_hex(0x606060)` + niedrigem Opa (≈40) → Nachleuchten
  2. Koordinatentransformation: Bildmitte = (125,125), Skalierung so dass ±1.0 auf ±110px mappt
  3. Neuen Punkt als gefüllten Kreis (2px Radius) zeichnen, Farbe je Abstand vom Zentrum: nah = grün (`THEME_ACCENT`), weit = rot
- Monokompatiblitäts-Referenzlinie: senkrechte Linie durch Bildmitte (dezent, `THEME_TEXT_HINT`)

### 3. Loudness-History-Graph

- `lv_obj_t` mit `LV_EVENT_DRAW_MAIN` custom draw callback.
- Zeichnet 60 vertikale Balken (ein Balken pro Sekunde, links = ältester, rechts = aktuellster).
- Wertebereich: −40 bis −6 LKFS (sinnvoller Broadcast-Bereich).
- Balkenfarbe: grün wenn < −23 LKFS (EBU R128 Target), gelb −23 bis −16, rot > −16.
- Target-Linie: horizontale Linie bei −23 LKFS, Farbe `THEME_TEXT_HINT`, gestrichelt (via wiederholte kleine `lv_draw_rect`).
- Achsenbeschriftung: „−23 LKFS" rechts neben der Linie, „60s" unten rechts.

### 4. Numerik-Panel

Vier `lv_label_t` Widgets, je eine Zeile:
```
I:    −14.2 LKFS
S:    −13.8 LKFS
M:    −12.1 LKFS
Peak: −5.9 dBFS
```
- Font: `THEME_FONT_HINT` für Label, `THEME_FONT_LABEL` für Wert (oder monospace falls verfügbar).
- Werte werden jeden Timer-Tick via `lv_label_set_text_fmt()` aktualisiert.
- Farbe des Peak-Werts wechselt auf Rot (`lv_color_hex(0xE05050)`) wenn > −3 dBFS.

---

## Dateien

| Datei | Änderung |
|---|---|
| `main/screens/metering.cpp` | Komplett neu — Placeholder wird ersetzt |
| `main/screens/metering.h` | Unverändert (`metering_screen_create()` bleibt) |
| `main/idf_component.yml` | Keine neuen Abhängigkeiten |
| `sdkconfig.defaults` | Keine Änderungen |

---

## Architektur-Richtung: modulare Layouts (nicht in dieser Phase implementiert)

Das Metering-System soll langfristig verschiedene Layout-Presets unterstützen, in die austauschbare Module eingesetzt werden können. Beispiele:

- **Broadcast-Layout** (dieses Spec): L/R Balken + Goniometer + Loudness-History + Numerik
- **Compact-Layout**: Nur Balken + Numerik (für kleinere Kacheln oder Overlay)
- **Goniometer-Focus**: Großes Goniometer mit minimaler Numerik

**Designprinzip für diese Implementierung:** Jedes Widget (Balken, Goniometer, History, Numerik) wird als eigenständige Funktion gebaut (`metering_bar_create()`, `metering_gonio_create()`, etc.), die ein `lv_obj_t*` Parent entgegennimmt und ein Widget zurückgibt. Das macht zukünftige Layout-Varianten möglich ohne die Widget-Logik neu zu schreiben.

Die Layout-Auswahl (welche Module wo platziert werden) bleibt für diese Phase hard-coded. Ein konfigurierbares Layout-System kommt erst wenn ein zweites Layout tatsächlich benötigt wird.

---

## Nicht in dieser Phase

- Echte Audio-Daten (WiFi/UDP/OSC) — Phase 3
- K-Weighting-Filter für LUFS (korrekte ITU-R BS.1770 Implementierung) — Phase 3
- Touch-Interaktion (CH422G/GT911 noch offen)
- LRA (Loudness Range) — kann später im Numerik-Panel ergänzt werden
- Konfigurierbare Layout-Auswahl — erst wenn zweites Layout benötigt wird

---

## Farbregeln (aus CLAUDE.md)

- **Alle Farben ≥ 38% Luminanz** — IPS-Panel zeigt darunter grünen Tint
- Goniometer-Hintergrund: `0x606060` (= `THEME_BG`)
- Balken-Grün: Accent-Farbe aus `theme.h`
- Peak-Rot: `0xE05050` (Luminanz ~45%, sicher)
