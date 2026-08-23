#!/bin/bash
# Flash & Verify für dieses Board (OPI-Flash, idf.py flash ist unzuverlässig)
set -e
cd "$(dirname "$0")"
source ~/esp/esp-idf-5.5/export.sh 2>/dev/null
idf.py build 2>&1 | grep -E " error:|fatal error" | head -10
python3 ~/esp/esp-idf-5.5/components/esptool_py/esptool/esptool.py --chip esp32s3 --port /dev/ttyACM0 -b 115200 --no-stub write_flash --verify --no-progress 0x10000 build/studio-panel.bin
echo "Flash OK — prüfe Display oder Monitor für APP_BUILD:"
echo "  timeout 8 idf.py -p /dev/ttyACM0 monitor 2>&1 | grep APP_BUILD"
