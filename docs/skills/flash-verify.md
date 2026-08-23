# Skill: Flash & Verify (ESP32-S3 OPI Flash)

Dieses Board meldet Flash-Erfolg auch wenn nichts geschrieben wurde.
Immer diesen Workflow benutzen.

## Vorbedingungen

```bash
cd /mnt/source/data/coding/ESP32-S3/studio-panel
source ~/esp/esp-idf-5.5/export.sh 2>/dev/null
```

## Schritt 1: Build

```bash
idf.py build 2>&1 | grep -E " error:|fatal error" | head -20
# → Muss sauber sein (kein Output = kein Fehler)
```

## Schritt 2: Flash (primärer Weg)

```bash
idf.py -p /dev/ttyACM0 -b 115200 flash
```

## Schritt 3: Verifikation (PFLICHT)

### 3a. Binary im Flash prüfen

```bash
python3 ~/esp/esp-idf-5.5/components/esptool_py/esptool/esptool.py \
  --chip esp32s3 --port /dev/ttyACM0 -b 115200 \
  read_flash 0x10000 256 /tmp/flash_check.bin && \
  strings /tmp/flash_check.bin | head -10
# → Muss den aktuellen git-Hash und Datum/Uhrzeit zeigen
```

### 3b. App-Build-Zeit im Monitor prüfen

```bash
timeout 8 idf.py -p /dev/ttyACM0 monitor 2>&1 | grep "APP_BUILD"
# → I (xxx) main: Studio Panel booting... APP_BUILD=<Datum> <Zeit>
```

**ACHTUNG:** `I (32) boot: compile time` = Bootloader-Zeit, ändert sich NICHT bei App-Updates. Nur APP_BUILD ist aussagekräftig.

## Fallback-Reihenfolge bei Problemen

### F1: Flash scheitert mit Port-Fehler

```bash
# BOOT halten → RESET kurz → BOOT loslassen → sofort:
idf.py -p /dev/ttyACM0 -b 115200 flash
```

### F2: Flash meldet Erfolg, aber falsches Binary im Flash

```bash
# Direkt nur App-Binary schreiben (--no-stub, --verify)
python3 ~/esp/esp-idf-5.5/components/esptool_py/esptool/esptool.py \
  --chip esp32s3 --port /dev/ttyACM0 -b 115200 --no-stub \
  write_flash --verify 0x10000 build/studio-panel.bin
```

### F3: Board komplett unsichtbar nach erase-flash

```bash
# USB abstecken → BOOT gedrückt halten → USB einstecken → BOOT loslassen
ls /dev/ttyACM*   # sollte wieder erscheinen
idf.py -p /dev/ttyACM0 flash
```

## WiFi-Credentials

**NIEMALS in Kconfig.projbuild.** Nur in `sdkconfig.defaults.local` (gitignored):

```
CONFIG_ESP_WIFI_SSID="DeinNetzwerk"
CONFIG_ESP_WIFI_PASSWORD="DeinPasswort"
```

Diese Datei wird NIE neu gebaut, nie committed, nie von `idf.py build` überschrieben.
