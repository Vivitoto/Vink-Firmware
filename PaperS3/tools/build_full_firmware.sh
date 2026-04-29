#!/usr/bin/env bash
set -euo pipefail

# Full-only Vink-PaperS3 build helper.
# The user-facing/flasher artifact is always the merged 16MB image at offset 0x0.
# `buildfs` is still an internal step because the full image embeds the SPIFFS
# partition resources, but this script does not copy/publish standalone OTA or
# SPIFFS binaries.

ENV_NAME="${ENV_NAME:-m5papers3}"
OUT="${1:-.pio/build/${ENV_NAME}/Vink-PaperS3-full-16MB.bin}"

pio run -e "$ENV_NAME"
pio run -e "$ENV_NAME" -t buildfs
bash tools/merge_full_firmware.sh "$ENV_NAME" "$OUT"

size="$(stat -c '%s' "$OUT")"
if [[ "$size" != "16777216" ]]; then
  echo "ERROR: full image must be exactly 16777216 bytes, got $size" >&2
  exit 1
fi

ls -lh "$OUT"
