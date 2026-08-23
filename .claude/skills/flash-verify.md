---
name: flash-verify
description: Flash ESP32-S3 firmware and verify it actually wrote. OPI-Flash boards silently report success without writing. Use this instead of bare idf.py flash.
---

# Flash & Verify — ESP32-S3 OPI Flash

**Problem:** `idf.py flash` meldet Erfolg auch wenn nichts geschrieben wurde (OPI-Flash-Bug).
**Lösung:** Binary schreiben + verifizieren + APP_BUILD im Monitor prüfen.

## Schritt 1: Build sauber?

```bash
cd /mnt/source/data/coding/ESP32-S3/studio-panel && source ~/esp/esp-idf-5.5/export.sh 2>/dev/null && idf.py build 2>&1 | grep -E " error:|fatal error" | head -10
```

→ Kein Output = sauber. Sonst Fehler beheben.

## Schritt 2: Flash

```bash
idf.py -p /dev/ttyACM0 -b 115200 flash
```

## Schritt 3: Verifikation (PFLICHT)

```bash
timeout 8 idf.py -p /dev/ttyACM0 monitor 2>&1 | grep "APP_BUILD"
```

→ Muss `APP_BUILD=<Datum> <Zeit>` zeigen. **`boot: compile time` ist der Bootloader — nicht aussagekräftig.**

## Schritt 4: Falls Flash scheitert

**F1 — Download-Mode:**
BOOT halten → RESET kurz → BOOT loslassen → dann Schritt 2

**F2 — Direktes Write mit Verify:**
```bash
python3 ~/esp/esp-idf-5.5/components/esptool_py/esptool/esptool.py --chip esp32s3 --port /dev/ttyACM0 -b 115200 --no-stub write_flash --verify 0x10000 /mnt/source/data/coding/ESP32-S3/studio-panel/build/studio-panel.bin
```

Dann APP_BUILD prüfen (Schritt 3).

**F3 — Board komplett weg:**
USB abstecken → BOOT halten → USB einstecken → BOOT loslassen → Schritt 2
