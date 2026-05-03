# Vink-PaperS3 Local Test Environment

This folder contains local smoke tests for firmware changes that can be checked without a physical PaperS3.

## What this can test

- PlatformIO compile/link success.
- SPIFFS build success as an internal full-image resource step.
- Full 16MB image merge success.
- `releases.json` parses and the top release is full-only for new builds.
- Source-level invariants for the bugs that repeatedly broke real devices:
  - App does not call legacy `_browser.render()` direct display path.
  - Shell commits wait for EPD readiness before physical display submission.
  - Non-tab shell pages are dirty-gated instead of repainting every loop.
  - Tab/back/wake transitions suppress stale touches and wait for finger release.
  - Shell canvas is preallocated during display init.
  - Font/WiFi paths do not use `SPIFFS.begin(true)` and accidentally format SPIFFS.

## What this cannot test

- Real EPD physical ghosting/waveform quality.
- GT911 touch controller electrical behavior.
- PaperS3 PMIC/power-off behavior.
- Actual user-perceived refresh speed on hardware.

Those still require a real PaperS3 burn test. This local gate is meant to catch regressions before asking for another real-device test.

## Fast check using existing artifacts

From `PaperS3/`:

```bash
python3 tests/local_firmware_smoke.py
```

## Full-only local build + artifact check

From `PaperS3/`:

```bash
python3 tests/local_firmware_smoke.py --build
```

The full mode runs:

```bash
tools/build_full_firmware.sh /home/vito/.openclaw/workspace/artifacts/Vink-PaperS3/Vink-PaperS3-<version>-<slug>-full-16MB.bin
```

Internally this still runs `pio run` and `pio run -t buildfs`, because the full image embeds the SPIFFS resource partition. It does **not** copy or publish standalone OTA/app or SPIFFS binaries. Vink PaperS3 should be flashed with the merged 16MB full image from offset `0x0` every time.

## Current artifact slug

The default slug is:

```text
official-profile
```

Override if a future release changes asset names:

```bash
python3 tests/local_firmware_smoke.py --build --slug some-new-slug
```
