# Vink-PaperS3 Local Test Environment

This folder contains local smoke tests for firmware changes that can be checked without a physical PaperS3.

## What this can test

- PlatformIO compile/link success.
- SPIFFS build success.
- Full 16MB image merge success.
- `releases.json` parses and top asset sizes match generated artifacts.
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

## Full local build + artifact check

From `PaperS3/`:

```bash
python3 tests/local_firmware_smoke.py --build
```

The full mode runs:

```bash
pio run -e m5papers3
pio run -e m5papers3 -t buildfs
tools/merge_full_firmware.sh
```

Then it copies generated binaries into `/home/vito/.openclaw/workspace/artifacts/` using the current top manifest version and verifies their sizes against `releases.json`.

## Current artifact slug

The default slug is:

```text
shell-commit-guard
```

Override if a future release changes asset names:

```bash
python3 tests/local_firmware_smoke.py --build --slug some-new-slug
```
