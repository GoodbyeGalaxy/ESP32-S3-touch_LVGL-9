# Visuals Screen — Design Spec
**Datum:** 2026-08-27  
**Status:** Approved, ready for implementation planning

---

## Kontext

Das Studio Panel (800×480, ESP32-S3, LVGL 9.5) bekommt einen neuen **Visuals-Screen** als Ersatz für den bisherigen Spectrum-Screen. Spectrum wandert als zusätzliche Ansicht in den Metering-Screen.

Verfügbare Audio-Rohdaten (UDP, 30 Hz): Peak L/R, RMS L/R, Momentary/Short-term/Integrated LUFS, Goniometer-Samples (L/R -1..1), FFT bins[256] (log-skaliert, 20Hz–20kHz).

Verfügbar via WebSocket (Phase 6, bereits implementiert): BPM, Position, Transport-State.

---

## Navigation

### Home-Screen
- Kachel "SPECTRUM" → umbenannt in **"VISUALS"** (neues Icon)

### Visuals Tile-Picker
- **4×2 Grid** (8 Kacheln, eine pro Visual-Mode)
- Jede Kachel: statisches Standbild (pre-rendered C-Array Asset) + Modus-Name
- Stiff images: nach Implementierung aus echtem Frame generiert (LVGL Image Converter → RGB565 C-Array)
- Platzhalter initial: Solid Color + Name
- **Tap** → Fullscreen-Mode

### Fullscreen-Mode
- **Swipe links/rechts** → vorheriger/nächster Modus (zyklisch)
- **Swipe runter** → zurück zum Tile-Picker
- **Touch** → modus-spezifische Interaktion (pro Modus definiert, siehe unten)
- **Touch & Hold 1s** → Morph manuell triggern

### NVS-Persistenz
- Letzter aktiver Modus-Index wird beim Wechseln via `nvs_set_u8` gespeichert
- Beim Start: `nvs_get_u8` → direkt in diesen Modus laden
- ~15 Zeilen Code, NVS bereits initialisiert in wifi.cpp

---

## Morph- und Transitions-System

### State Machine
```
STABLE → MORPHING (crossfade 4–8 Bars) → COOLDOWN (min. 30s) → STABLE
```
Nur aus STABLE heraus kann ein Trigger feuern. Morph-Start ist **BPM-phase-locked** — wartet auf nächsten Bar-1-Beat (BPM kommt aus Studio One WebSocket).

### Mood Score (kontinuierlich, kein Schnitt)
```
mood = lerp(mood, target_mood, alpha)
target_mood = f(loudness_normalized, spectral_centroid, bpm_normalized)
alpha = dt * (0.02 + energy_high * 0.1)  // langsame Musik: alpha klein
```
Steuert Farb-Charakter und Animations-Geschwindigkeit aller Modi kontinuierlich. Kein harter Trigger, nur Drift.

### Morph-Trigger (mehrere, alle in State Machine eingespeist)
1. **Autonome Drift**: alle 16–64 Bars (BPM-getaktet, randomisiert)
2. **Spectral Centroid Shift**: Δcentroid > threshold über 2s → trigger
3. **Silence-to-Entry**: Musik weg >2s, dann Wiedereinsatz → perfekter natürlicher Schnitt
4. **Manual Hold**: Touch 1s → sofortiger Morph-Start

**Nicht too wild weil:** Cooldown verhindert Doppel-Trigger, BPM-Phase-Lock macht es musikalisch, Mood Score ist ohnehin immer aktiv und driftet unabhängig langsam.

---

## Visual Modes — 8 Modi

### Rechenleistungs-Kategorien
- **On-Device**: läuft vollständig auf ESP32, kein Mac nötig
- **Mac-powered**: benötigt Analysis-Packet vom Mac-Helper (Phase 7)

---

### Mode 1: Lissajous XL
**Kategorie:** On-Device  
**Rohdaten:** Goniometer L/R (-1..1, 30Hz)

Vollbildfüllender Lissajous/Goniometer. Phosphor-Trail-Effekt: jeder neue Punkt leuchtet hell auf, faded über mehrere Frames. Canvas in PSRAM, Alpha-Blend per Pixel.

**Mood-Reaktion:** Trail-Länge wächst mit Loudness. Farbe (Hue) dreht sich mit Spectral Centroid.  
**Touch-Interaktion:** Tap freezt den Trail für 2s (schöner Standmoment).  
**Stilbild:** Typisches Lissajous-Ellipsoid, cyan-to-white Phosphor.

---

### Mode 2: Circular FFT
**Kategorie:** On-Device  
**Rohdaten:** bins[256]

FFT-Bins auf Polarkoordinaten gemappt: 256 Frequenzbänder als radiale Balken auf einem Kreis. Niedriger Ausschlag = Ring liegt flach, Peak = Stachel nach außen. Kreis rotiert langsam (Geschwindigkeit = BPM/4).

**Mood-Reaktion:** Radius der Basis-Ringline wächst mit Loudness. Farbgradient (Hue) über die Frequenzbänder, Sättigung = Spectral Centroid.  
**Touch-Interaktion:** Tap kehrt Rotation um (CW/CCW toggle).  
**Stilbild:** Dornenkranz-artig, grün-blau Verlauf.

---

### Mode 3: Aurora Waves
**Kategorie:** On-Device  
**Rohdaten:** bins[256], Peak L/R

Mehrere überlagerte horizontale Sinuswellen, jede durch ein Frequenzband moduliert. Bass-Welle (dick, langsam), Mid-Welle (mittel), Treble-Welle (dünn, schnell). Wellen transluzent, überlagern sich zu Farbfeldern.

**Mood-Reaktion:** Amplituden durch Band-Energie. Wellengeschwindigkeit = BPM-synchron. Farbpalette durch Mood Score (warm bei Bass-lastiger Musik, kühl bei Treble-lastiger).  
**Touch-Interaktion:** Drücken erzeugt lokale Wellen-Disruption (Ripple vom Finger weg).  
**Stilbild:** Nordlicht-artig, blau-lila-grün Schichtung.

---

### Mode 4: Bass Pulse
**Kategorie:** On-Device  
**Rohdaten:** bins[0..20] (Bass-Energie), Peak L/R

Einfache geometrische Figuren (Kreis, Sechseck, Rhombus) die auf Kick/Transient pulsieren. Die Form selbst ist ruhig und zentriert; auf Energie-Peaks schneller Expand → Fade-out. Mehrere konzentrische Ringe auf stärkeren Peaks.

**Mood-Reaktion:** Anzahl aktiver Ringe = f(Loudness). Form rotiert (Sechseck → Rhombus → Kreis) per Mood Score.  
**Touch-Interaktion:** Tap wechselt Grundform manuell.  
**Stilbild:** Pulsierendes Sechseck, weiß mit Glow, schwarzer Hintergrund.

---

### Mode 5: Terrain Wave
**Kategorie:** On-Device  
**Rohdaten:** bins[256]

Horizontale scrollende Höhenkarte: FFT-Bins als Terrain-Profil von links nach rechts. Neue Kolumne wird rechts angehängt, alte links verdrängt. Entferntere Terrain-Schichten dunkler/kleiner (Tiefenwirkung, 3–4 Schichten).

**Mood-Reaktion:** Scroll-Geschwindigkeit = BPM. Farbtiefe durch Loudness. Ältere Schichten färben sich im Mood-Ton.  
**Touch-Interaktion:** Touch-X-Position verschiebt die "Kamera" (welcher Frequenzbereich zentriert ist).  
**Stilbild:** Bergketten-Silhouette, dunkelblau-cyan Layering.

---

### Mode 6: Particle Storm
**Kategorie:** Mac-powered (für Instrument-Trigger) / On-Device (vereinfacht)  
**Rohdaten On-Device:** bins[256], Peak L/R  
**Rohdaten Mac:** kick, snare, hat, tom (Bool), energy_bass/mid/high

Partikel-System mit Frequenzband-Charakter:
- Bass → große, träge Partikel, aufsteigend aus dem unteren Bildschirmrand
- Mid → mittelgroße, spiralförmig
- Treble → kleine, schnelle Funken von oben

Mac-powered: Kick spawnt Partikel-Burst (Explosions-Muster), Snare spawnt horizontale Welle, Hat = schnelle Funken-Shower.

**Touch-Interaktion:** Touch-Position = Gravitations-Zentrum (Partikel werden angezogen).  
**Stilbild:** Partikel-Explosion in Gold-Orange, schwarzer Grund.

---

### Mode 7: Game of Life
**Kategorie:** Mac-powered  
**Rohdaten:** kick, snare, hat, tom, BPM, energy_bass/mid/high

Conways Game of Life, audio-getrieben:
- **Grid:** 80×50 Zellen, dargestellt als 10×10px Blöcke (800×500, leicht gecroppt auf 800×480)
- **Kick** → 5–8 zufällige lebende Zellen im unteren Drittel des Grids
- **Snare** → horizontaler Flip-Streifen auf Höhe 60% des Grids
- **Hi-Hat** → 2–3 scatter-Zellen im oberen Bereich
- **Tom** → kompaktes 3×3 "Glider"-Pattern in zufälliger Position
- **Evolutions-Rate:** BPM-synchron (120 BPM = 2 Steps/s)
- **Farbe:** lebende Zellen: Hue = f(Spectral Centroid), Helligkeit = f(Alter der Zelle). Alte Zellen leuchten heller.

Ohne Mac: Evolution läuft autonom, nur Bass-Energie triggert gelegentliche neue Seeds.

**Touch-Interaktion:** Finger zeichnet lebende Zellen (wie klassischer GOL-Editor).  
**Stilbild:** Grid mit leuchtend grünen/blauen Zellen verschiedener Generationen, schwarzer Grund.

---

### Mode 8: Julia Set / Mandelbrot
**Kategorie:** On-Device (Haupt-Variante)  
**Rohdaten:** bins[256], RMS L/R, Momentary Loudness, BPM

**Kernarchitektur:**
Escape-Time-Array wird einmalig im Background-Task berechnet und in PSRAM gespeichert (uint8[200×120] = 24KB). Pro Frame: Palette-Remap via LUT → upscale 4× → 800×480 Display. Das ist marginal rechenintensiv.

**Julia Set mit FFT-getriebenem Parameter c:**
```
c.real = map(bass_energy, 0..1, -0.8..0.8)    // bins[0..20] normiert
c.imag = map(treble_energy, 0..1, -0.4..0.4)  // bins[150..255] normiert
```
Kleine Änderungen in c erzeugen dramatisch andere Julia-Mengen. Bass-lastige Musik → breite reale Achse, offene Strukturen. Treble-reich → hohe imaginäre Komponente, dichte Spiral-Strukturen.

**Wenn c sich ändert:** neuen Escape-Time-Array im Background queuen. Smooth-Übergang: alter Array bleibt sichtbar bis neuer fertig, dann crossfade.

**Zoom-System:**
- Zoom-Level kriecht langsam mit Loudness-Integral (je lauter die letzten 10s, desto tiefer)
- Auf Peak-Transient: kleiner Zoom-Sprung (×1.1) in Richtung interessanter Region
- Tiefes Zoom → neue Render nötig → Mac-powered für Echtzeit-Deep-Dive

**Farbe:**
- Escape-Time → Hue-Mapping via HSV-Palette
- Palette-Phase rotiert mit BPM (exakt getaktet)
- Sättigung = Spectral Flatness (tonal → hoch, noise-artig → gering)
- Helligkeit = Momentary Loudness

**Mandelbrot-Modus** (Touch-Hold 2s aus Julia → toggle): feste Ansicht der Mandelbrot-Menge, Farb-Animation durch oben genannte Palette-Rotation. Kein c-Parameter, dafür langsamer autonomer Zoom in bekannt schöne Regionen (vorher definierte Koordinaten-Liste).

**Touch-Interaktion:** Tap auf Bildschirm → Zoom-Zentrum zu Touch-Position verschieben (neues Render queued).

**Stilbild:** Julia-Spiralen in blau-gold Palette, Mandelbrot-typische Apfelmännchen-Silhouette.

---

## Analysis Packet — Mac-Helper Erweiterung (Phase 7)

Für Game of Life und erweiterten Particle Storm braucht der ESP32 interpretierte Events, nicht nur Rohaudio. Der Mac-Helper (Python) wird um librosa/aubio-basierte Analyse erweitert.

**Protokoll** (via bestehendem WebSocket, selber Kanal wie transport):
```json
{
  "type": "analysis",
  "kick": true,
  "snare": false,
  "hat": true,
  "tom": false,
  "energy_bass": 0.82,
  "energy_mid": 0.31,
  "energy_high": 0.09,
  "spectral_centroid": 0.44,
  "spectral_flatness": 0.12,
  "loudness_delta": 0.08
}
```
Rate: 30 Hz (synchron mit Audio-Packet). ESP32-Seite: neue AnalysisState-Struct + Queue (analog zu AudioPacket/StudioOneState).

---

## Spectrum → Metering Integration

Spectrum-Screen entfällt als eigenständiger Screen. Spectrum-Ansicht wird **vertikaler Swipe nach oben** innerhalb des Metering-Screens (oder Tab, falls Swipe mit bestehendem Layout kollidiert — zu klären bei Implementierung).

---

## Arbeitspakete

### WP-A: Foundation (Subagent-geeignet, unabhängig)
- Home-Screen: "SPECTRUM" → "VISUALS", neues Icon, Routing auf Visuals-Screen
- Metering-Screen: Spectrum als Swipe-Up-Ansicht integrieren
- Visuals-Screen: Tile-Picker Infrastruktur (4×2 Grid, Tap→Fullscreen, Swipe-Navigation)
- NVS-Persistenz: Modus-Index speichern/laden
- Mood-Score-System: kontinuierlicher float, BPM-getaktet, Inputs aus AudioPacket
- Morph State Machine: STABLE→MORPHING→COOLDOWN, alle Trigger-Quellen
- Placeholder-Kacheln (Solid Color + Name) für alle 8 Modi

### WP-B: On-Device Classics (Subagent-geeignet nach WP-A)
- Mode 1: Lissajous XL (Phosphor-Trail, Mood-Farbe)
- Mode 2: Circular FFT (Polar-Mapping, BPM-Rotation)
- Mode 3: Aurora Waves (Multi-Layer Sinuswellen, FFT-moduliert)
- Mode 4: Bass Pulse (Beat-Detection aus bins[0..20], Geometrie)
- Mode 5: Terrain Wave (Scrollende Höhenkarte, Tiefenwirkung)
- **Performance-Gate:** Nach WP-B auf echter Hardware messen. Trail-Effekte auf PSRAM-Canvas können PSRAM-Contention triggern (bekannter Drift-Bug). Entscheidung: falls zu schwer → Mac übernimmt Rendering, ESP32 zeigt Frame.

### WP-C: Julia Set / Mandelbrot (Subagent-geeignet nach WP-A)
- Background-Render-Task: Escape-Time-Array in PSRAM
- FFT-zu-c-Mapping (bass→real, treble→imag)
- Palette-LUT: Hue via Escape-Time, Phase via BPM, Sättigung via Spectral Flatness
- Zoom-System: Loudness-Integral + Peak-Sprung + Touch-Zoom
- Mandelbrot-Modus toggle (Touch-Hold)
- Crossfade bei c-Änderung

### WP-D: Mac Analysis Extension (Subagent-geeignet, unabhängig von WP-B/C)
- Python-Helper: librosa/aubio für Onset-Detection (Kick, Snare, Hat, Tom)
- Spectral Centroid, Spectral Flatness berechnen
- Analysis-Packet senden (JSON, 30 Hz, selber WebSocket-Kanal)
- ESP32: AnalysisState-Struct + Queue, ws_client.cpp erweitern

### WP-E: Mac-powered Creatives (nach WP-D)
- Mode 6: Particle Storm mit Instrument-Triggern
- Mode 7: Game of Life (Grid-Rendering, alle Trigger)
- Tile-Stills: echte Stills generieren für alle fertigen Modi, als C-Arrays einbetten

---

## Offene Fragen (bei Implementierung klären)

1. **Spectrum in Metering**: Swipe-Up oder Tab? Klärt sich beim Implementieren des Metering-Screens.
2. **4×2 vs. Paginierung**: Falls 8 Kacheln auf 800×480 zu eng — zweite Seite mit Swipe.
3. **PSRAM Performance**: Trail-Effekte und Particle Storm erst nach WP-B-Test einschätzen.
4. **Julia Deep Zoom**: Bei Zoom-Level > 1000× braucht man Double-Precision oder Fixed-Point-Arithmetik — Mac-Offload dann sinnvoller.
