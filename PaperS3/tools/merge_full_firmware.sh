#!/usr/bin/env bash
set -euo pipefail

ENV_NAME="${1:-m5papers3}"
OUT="${2:-.pio/build/${ENV_NAME}/Vink-PaperS3-full-16MB.bin}"
BUILD_DIR=".pio/build/${ENV_NAME}"

# Full image layout only. Standalone OTA/app and SPIFFS binaries are intermediate
# PlatformIO outputs; Vink PaperS3 users should flash the merged 16MB image from
# offset 0x0 so bootloader, partition table, app and resources stay in sync.
BOOT_APP0="${HOME}/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
ESPTOOL="${HOME}/.platformio/packages/tool-esptoolpy/esptool.py"

python3 "$ESPTOOL" --chip esp32s3 merge_bin \
  -o "$OUT" \
  --flash_mode dio \
  --flash_freq 80m \
  --flash_size 16MB \
  0x0 "$BUILD_DIR/bootloader.bin" \
  0x8000 "$BUILD_DIR/partitions.bin" \
  0xe000 "$BOOT_APP0" \
  0x10000 "$BUILD_DIR/firmware.bin" \
  0xC10000 "$BUILD_DIR/spiffs.bin"

ls -lh "$OUT"
