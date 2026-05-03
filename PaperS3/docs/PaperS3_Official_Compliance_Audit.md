# PaperS3 Official Compliance Audit

Status: local firmware/code audit for Vink-PaperS3 `v0.3.2-rc`.

Official sources used:

- M5Stack PaperS3 product/developer page: <https://docs.m5stack.com/zh_CN/core/PaperS3>
- M5Stack Arduino PaperS3 quick-start pages: touch, microSD, battery, wake/sleep, RTC/IMU/buzzer notes
- Official factory firmware: <https://github.com/m5stack/M5PaperS3-UserDemo>
- Local fetched snapshot: `/home/vito/.openclaw/workspace/Vink/reference-firmware/M5PaperS3-UserDemo`

## Firmware profile checklist

| Area | Official requirement / reference | Vink status |
|---|---|---|
| SoC | ESP32-S3R8 | `platformio.ini` uses `esp32-s3-devkitm-1`; build flags define `ESP32S3` |
| Flash | 16MB external flash | `board_upload.flash_size = 16MB`, full image exactly 16MB |
| PSRAM | enabled, Octal/OPI | `BOARD_HAS_PSRAM`, `qio_opi`, `board_build.psram_type = opi` |
| Display | `EPD_ED047TC1`, 960x540 physical, 16 gray | constants record physical 960x540; Vink uses 540x960 portrait canvas, 4bpp shell |
| Touch | GT911, SDA G41, SCL G42, INT G48 | constants and diagnostics record G41/G42/G48; input caches raw + normalized points |
| RTC | BM8563, I2C address 0x51 | enabled through `cfg.internal_rtc = true`; status bar reads `M5.Rtc` |
| IMU | BMI270, I2C address 0x68 | enabled through `cfg.internal_imu = true`; no product IMU feature yet |
| Battery ADC | G3 | constants + diagnostics read raw ADC using factory formula; still requires real voltage calibration |
| Charge state | factory firmware uses GPIO4 `PIN_CHG_STATE`, 0 charging | constants + diagnostics expose GPIO4 state |
| USB detect | G5, factory firmware treats HIGH as USB-IN | constants + diagnostics/status use GPIO5 |
| Buzzer | G21 | initialized low as official hardware pin; no product sound UI yet |
| Power key | side key powers on; firmware should ignore boot press residue and treat a later deliberate press as shutdown | input task now arms only after boot-release, requires stable press, saves progress, draws shutdown page, pulses GPIO44, calls `M5.Power.powerOff()`, then deep-sleep fallback on GPIO36 |
| microSD | CS47/SCK39/MOSI38/MISO40 | explicit SPI pins and 25MHz→8MHz→4MHz fallback |
| Wake/sleep | official examples redraw after wake | automatic idle sleep is deliberately not enabled until real-device wake validation; shutdown fallback uses GPIO36 wake only after key release |
| Release image | user requires full-only | build/smoke/release manifest now full-only; OTA/SPIFFS are internal intermediates only |

## Official factory firmware deltas reviewed

The factory firmware is an ESP-IDF self-test app, not a reader product, so Vink should not copy its Mooncake app/UI. Relevant low-level patterns were checked instead:

- `main/hal/hal.cpp`
  - `M5.begin()` then display rotation and device app init.
  - RTC/power/SD/Wi-Fi/buzzer/ext-port init are centralized in HAL.
  - Battery uses GPIO3 ADC with `raw * 3.5 / 4096 * 2`.
  - USB detect uses GPIO5 HIGH = USB connected.
  - charge-state GPIO4 is tracked separately.
  - SD uses SDSPI pins: MISO40/MOSI38/SCLK39/CS47.
  - power off sleeps display, waits, then calls `M5.Power.powerOff()`.
- `main/main.cpp`
  - Uses quality EPD refresh for full-screen test/periodic clean refresh.
  - Calls `M5.update()` continuously.
  - Periodically forces full refresh to avoid stale ink state.

Vink adaptations:

- Vink centralizes physical EPD pushes in `DisplayService`, with quality refresh after repeated fast pushes.
- Vink keeps `M5.update()` in the input task and does not mix random direct display writes into UI code.
- Vink now initializes and exposes official battery/USB/charge/buzzer pins, even though not all are user-facing features yet.
- Vink keeps SD lazy-init to avoid boot appearing stuck when no SD card exists, but uses official pins and logs frequency fallback.
- Vink now refuses to push a mutable live canvas if the immutable display snapshot cannot be allocated; this avoids reintroducing render-vs-EPD races under PSRAM pressure.
- Vink now includes an explicit physical 960x540 → logical 540x960 touch transform fallback tied to the runtime-selected active rotation, while still preserving raw coordinates for diagnosis.
- Vink now implements the v0.3 side power-key path: boot press is ignored until release, a later stable press requests shutdown, current reader progress is saved, a shutdown page is refreshed, GPIO44 is pulsed, `M5.Power.powerOff()` is called, and deep sleep is only a fallback.
- `releases.json` is the single current firmware manifest consumed by Vink Flasher; the obsolete `oxflash.json` compatibility manifest was removed to avoid duplicate sources of truth.

## Remaining hardware-only validation

These cannot be closed locally:

1. GT911 raw coordinate direction and normalized 540x960 mapping, even with the added rotation fallback.
2. Whether the active M5GFX rotation selected at boot matches handle-up usage on the user's physical unit.
3. EPD ghosting/quality-refresh cadence under real reading/tapping.
4. Battery ADC calibration accuracy versus actual voltage.
5. GPIO4 charge-state polarity on the user's hardware revision.
6. SD stability across different cards at 25MHz/8MHz/4MHz.
7. Side power-key shutdown on real PaperS3: confirm boot-press arming, shutdown screen, GPIO44 power-off pulse, and no immediate reboot.
8. Wake/sleep behavior after long idle on real PaperS3; v0.3 still intentionally defers automatic idle sleep until side-key shutdown is confirmed.

## Required real-device test path

1. Flash the full 16MB image from offset `0x0`.
2. Open Settings → Touch calibration/diagnostics.
3. Tap all 9 grid regions; confirm raw and normalized coordinates move in the same physical direction.
4. Check diagnostics line: `USB`, `CHG`, `BAT`, active `rotation`, and panel dimensions.
5. Open bookshelf from SD and confirm `/books/*.txt` scan.
6. Open a GBK/UTF-8 TXT, page forward/back, then return to shell.
7. Let the device idle long enough to test wake/redraw/touch suppression.
