# Deploy Agent Brief — ESP32-S3 Studio Panel

Self-contained deploy task. Follow these steps exactly. Report only: Flash OK/FAIL and APP_BUILD timestamp.

## Prerequisites

Binary must already be built at:
`/mnt/source/data/coding/ESP32-S3/studio-panel/build/studio-panel.bin`

Board connected at `/dev/ttyACM0`.

## Step 1 — Flash

```bash
source ~/esp/esp-idf-5.5/export.sh 2>/dev/null
idf.py -p /dev/ttyACM0 -b 115200 flash
```

Expected output ends with: `Hard resetting via RTS pin...`

If flash fails, try Download Mode: hold BOOT → press RESET → release BOOT → retry flash.

## Step 2 — Verify

```bash
strings /mnt/source/data/coding/ESP32-S3/studio-panel/build/studio-panel.bin | grep "APP_BUILD"
```

Extracts compile timestamp from binary. `idf.py monitor` requires TTY and cannot be used in subagent context.

## Step 3 — Report

Reply with exactly:
- Flash: OK or FAIL (+ error message if FAIL)
- APP_BUILD: [timestamp from Step 2]

## Notes

- `idf.py flash` exit code 0 = flash reported success (OPI-Flash can silently fail — binary timestamp is the reliable check)
- `I (32) boot: compile time` in monitor = bootloader timestamp, NOT app build time — ignore it
- Binary at `/dev/ttyACM0` — if Permission denied: `newgrp dialout`
