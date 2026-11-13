#!/bin/sh
# Packs an ESP-Hosted coprocessor image for the ESP32-C6 and writes it to the P4's "c6fw" partition.
# Usage: tools/flash_c6fw.sh <c6-image.bin> [port]
# Then on the device: Apps > Setup > Diagnostics > Update Wi-Fi chip.
set -e
BIN=${1:?usage: flash_c6fw.sh <c6-image.bin> [port]}
PORT=${2:-$(ls /dev/cu.usbmodem* | head -1)}
OUT=$(mktemp -t c6fw).bin
python3 - "$BIN" "$OUT" <<'PY'
import struct, sys, zlib
data = open(sys.argv[1], "rb").read()
assert data[0] == 0xE9, "not an ESP image"
open(sys.argv[2], "wb").write(b"C6FW" + struct.pack("<III", len(data), zlib.crc32(data) & 0xffffffff, 0) + data)
print("packed %d bytes" % len(data))
PY
python -m esptool --chip esp32p4 -p "$PORT" -b 460800 --after hard_reset write_flash 0xC20000 "$OUT"
rm -f "$OUT"
