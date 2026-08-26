# Skill: Flash & Verify (ESP32-S3 OPI Flash)

Dieses Board meldet Flash-Erfolg auch wenn nichts geschrieben wurde.
Immer `flash.sh` benutzen — nie `idf.py flash` direkt.

## Vorbedingungen

```bash
cd /mnt/source/data/coding/ESP32-S3/studio-panel
source ~/esp/esp-idf-5.5/export.sh 2>/dev/null
```

## Schritt 1: Build

```bash
idf.py build 2>&1 | grep -E " error:|fatal error" | head -20 || true
# || true ist nötig: grep gibt exit code 1 wenn keine Fehler gefunden werden
# → sauber = kein Output. Mit Fehler = Fehlermeldungen erscheinen.
```

Binary prüfen (zuverlässiger als Exit-Code):

```bash
ls -lh build/studio-panel.bin   # Muss existieren und aktuelles Timestamp haben
```

## Schritt 2: Flash (IMMER flash.sh verwenden)

```bash
bash flash.sh
```

`flash.sh` macht intern: `esptool --no-stub write_flash --verify` — das einzige
was auf OPI-Flash zuverlässig funktioniert. `idf.py flash` meldet Erfolg auch
bei fehlgeschlagenem Write.

## Schritt 3: Verifikation (PFLICHT)

```bash
timeout 8 idf.py -p /dev/ttyACM0 monitor 2>&1 | grep "APP_BUILD"
# → I (xxx) main: Studio Panel booting... APP_BUILD=<Datum> <Zeit>
```

**ACHTUNG:** `I (32) boot: compile time` = Bootloader-Kompilierzeit — ändert
sich NICHT bei App-Updates. Nur `APP_BUILD` aus `app_main` ist aussagekräftig.

## Fallback-Reihenfolge bei Problemen

### F1: Flash scheitert mit Port-Fehler

```bash
# BOOT halten → RESET kurz → BOOT loslassen → sofort:
bash flash.sh
```

### F2: flash.sh meldet Erfolg, aber falsches Binary

```bash
# Binary direkt verifizieren:
python3 ~/esp/esp-idf-5.5/components/esptool_py/esptool/esptool.py \
  --chip esp32s3 --port /dev/ttyACM0 -b 115200 --no-stub \
  verify_flash 0x10000 build/studio-panel.bin
```

### F3: Board nach erase-flash unsichtbar

```bash
# USB abstecken → BOOT gedrückt halten → USB einstecken → BOOT loslassen
ls /dev/ttyACM*   # sollte wieder erscheinen
bash flash.sh
```

## sdkconfig.defaults — Source of Truth

**KRITISCH:** `sdkconfig` ist gitignored und darf nie committed werden.
Alle Board-spezifischen Einstellungen MÜSSEN in `sdkconfig.defaults` stehen,
sonst gehen sie beim nächsten `rm sdkconfig` verloren:

```
# Flash
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y

# PSRAM (ohne das: kein Frame Buffer, nur Backlight sichtbar)
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y

# Fonts (ohne das: lv_font_montserrat_24 nicht deklariert)
CONFIG_LV_FONT_MONTSERRAT_14=y
CONFIG_LV_FONT_MONTSERRAT_18=y
CONFIG_LV_FONT_MONTSERRAT_24=y
CONFIG_LV_FONT_UNSCII_16=y
```

Wenn sdkconfig neu generiert werden muss (z.B. nach neuen Komponenten):

```bash
rm sdkconfig
idf.py build   # generiert sdkconfig neu aus sdkconfig.defaults
```

## WiFi-Credentials

**NIEMALS in Kconfig.projbuild.** Nur in `sdkconfig.defaults.local` (gitignored):

```
CONFIG_ESP_WIFI_SSID="DeinNetzwerk"
CONFIG_ESP_WIFI_PASSWORD="DeinPasswort"
```
