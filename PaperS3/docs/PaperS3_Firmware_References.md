# PaperS3 Firmware Reference Notes

This document is the working reference for Vink-PaperS3 hardware/display/touch fixes. The visual UI direction for Vink remains the user-provided Xiaohongshu/Crosslink reference package; the projects below are references for low-level PaperS3 behavior only.

## Design Boundary

- **Use ReadPaper / Crosspoint / other firmware for:** display initialization, refresh policy, font/canvas rendering, touch mapping, SD/power pins, sleep/wake behavior.
- **Do not copy their UI style.** Vink shell UI should stay Crosslink-like: monochrome e-paper, top status + tab bookmarks, card/grid panels, compact settings rows, no decorative bottom hint bar.

## Reference Projects

### shinemoon/M5ReadPaper

- Repo: <https://github.com/shinemoon/M5ReadPaper>
- Local mirror: `Vink/reference-firmware/M5ReadPaper`
- Useful areas:
  - `src/init/setup.cpp`
  - `src/device/ui_display.cpp`
  - `src/text/bin_font_print.*`
  - `src/tasks/display_push_task.*`
  - `src/device/powermgt.cpp`

Key takeaways:

- Initialize M5Unified first, then create a global `M5Canvas` in PaperS3 portrait geometry (`540x960`).
- Render text through its own font stack (`bin_font_print`) rather than relying on `M5.Display.print()` for CJK.
- Use a display push task / canvas-first model for reliable e-paper output.
- Rotation changes should wake the panel first:
  - `M5.Display.powerSaveOff()`
  - short delay
  - `M5.Display.setRotation(rotation)`
  - short delay
  - optional `powerSaveOn()`
- Power off sequence saves config/bookmark, waits for SD writes, shows a shutdown/lock screen, waits for display completion, then calls `M5.Power.powerOff()`.

Vink implication:

- Keep Vink's `FontManager`/canvas-aware CJK path for all shell UI text.
- Wrap rotation changes and keep display awake while changing rotation.
- Before shutdown: save reader progress/stats, stop services, give SD time to flush, then display shutdown screen and power off.

---

### juicecultus/crosspoint-reader-papers3

- Repo: <https://github.com/juicecultus/crosspoint-reader-papers3>
- Especially useful files:
  - `lib/hal/HalDisplay.cpp`
  - `lib/hal/HalGPIO.cpp`
  - `lib/hal/HalPowerManager.cpp`
  - `src/MappedInputManager.*`

Key takeaways:

#### Display

- Uses `EPD_Painter` and owns an 8bpp framebuffer in PSRAM.
- Boot clears the panel white with a high-quality paint to sync physical ink state.
- Uses quality/high refreshes to reduce ghosting.

#### PaperS3 hardware pins

- GT911 touch:
  - SDA: GPIO 41
  - SCL: GPIO 42
  - INT: GPIO 48
  - I2C address: usually `0x14`
- SD SPI:
  - CS: GPIO 47
  - SCK: GPIO 39
  - MOSI: GPIO 38
  - MISO: GPIO 40
- Power off pulse:
  - GPIO 44 (`PWROFF_PULSE_PIN`)
- Battery ADC:
  - GPIO 3 in Crosspoint's HAL

#### Touch coordinate transform

Crosspoint treats GT911 coordinates as logical portrait coordinates (`540x960`) and transforms them when display orientation changes:

```cpp
switch (touchOrientation) {
  case 1: // LandscapeClockwise
    x = 960 - 1 - rawY;
    y = rawX;
    break;
  case 2: // PortraitInverted
    x = 540 - 1 - rawX;
    y = 960 - 1 - rawY;
    break;
  case 3: // LandscapeCounterClockwise
    x = rawY;
    y = 540 - 1 - rawX;
    break;
  case 0: // Portrait
  default:
    x = rawX;
    y = rawY;
    break;
}
```

#### Touch zones

- Reader mode: split screen width into three zones:
  - left third: previous page
  - center third: confirm/menu
  - right third: next page
- Footer/non-reader mode can split a footer into four virtual buttons, but Vink should not draw a decorative bottom hint bar; use content-level touch affordances instead.
- Lock the tap zone to the touch-down coordinate, not drifting finger position.
- Suppress touch input briefly after activity transitions (`~200ms`) to avoid stale taps.

Vink implication:

- Add explicit PaperS3 touch normalization/transform tied to Vink's display rotation.
- Store touch-down and last live coordinates; do not query `M5.Touch.getDetail()` after release and assume it still contains valid final coordinates.
- Reader taps should remain 3-zone; shell pages should use content hitboxes.
- Add transition cooldown after major page/state changes where practical.

---

### omeriko9/M5Paper_PaperS3_eBookReader

- Repo: <https://github.com/omeriko9/M5Paper_PaperS3_eBookReader>
- Useful files:
  - `main/main.cpp`
  - `main/device_hal.cpp`
  - `main/gesture_detector.cpp`

Key takeaways:

#### M5Unified init

For PaperS3 it uses:

```cpp
auto cfg = M5.config();
cfg.clear_display = !is_wake_from_sleep;
cfg.output_power = true;
cfg.internal_imu = true;
cfg.internal_rtc = true;
cfg.internal_spk = false;
cfg.internal_mic = false;
cfg.fallback_board = m5::board_t::board_M5PaperS3;
M5.begin(cfg);
M5.Display.wakeup();
M5.BtnPWR.setDebounceThresh(0);
M5.BtnPWR.setHoldThresh(0);
M5.Display.setRotation(0);
```

#### Gesture detector

- Track touch start, current/last point, duration, and velocity.
- Detect:
  - tap
  - double tap
  - long press
  - swipe up/down/left/right
- Use thresholds:
  - tap movement threshold
  - swipe distance threshold
  - minimum swipe velocity
  - long-press duration
- Emit events on release; long press can emit while finger is still down.

#### Hardware HAL

- Abstracts device name, rotation, display size, SD mount, buzzer, IMU auto-rotation, sleep.
- SD mount uses SDSPI host, PaperS3 pins, max frequency around 20 MHz.
- Touch wake waits for the touch interrupt line to return inactive before enabling wake.

Vink implication:

- Add `cfg.fallback_board = board_M5PaperS3` if supported by current M5Unified version.
- Keep the touch detector stateful: down point + last live point + long-press fired flag.
- Avoid immediate repeated long-press triggers.
- Consider future IMU auto-rotation, but keep fixed rotation now because user requested handle-up orientation.

---

### alexcircuits/M5Stack-PaperS3-UniRead

- Repo: <https://github.com/alexcircuits/M5Stack-PaperS3-UniRead>
- Useful file:
  - `src/controllers/event_mgr_paper_s3.cpp`

Key takeaways:

#### Raw GT911 path

- I2C pins:
  - SDA GPIO 41
  - SCL GPIO 42
- I2C port: `I2C_NUM_0`
- Addresses tried:
  - `0x14`
  - `0x5D`
- Registers:
  - touch status: `0x814E`
  - first point coordinates: `0x8150`
- After reading status/point, write `0` to `0x814E` to clear the data-ready flag.

#### Event task

- Dedicated touch task polls every ~20 ms.
- Gesture thresholds:
  - swipe threshold around `100 px`
  - long-press duration around `600 ms`
  - long-press move threshold around `30 px`
  - debounce around `300 ms`
- Emits queued events: TAP, HOLD, RELEASE, SWIPE_LEFT, SWIPE_RIGHT, SWIPE_DOWN.

#### Watchdog note

- epdiy grayscale render threads can occupy CPU long enough to trip task watchdog.
- UniRead disables or relaxes TWDT around PaperS3 display-heavy operation.

Vink implication:

- If M5Unified touch becomes unreliable, fall back to raw GT911 read using these registers.
- Add a debounced event model before adding more complex touch gestures.
- Watch for watchdog symptoms during full-screen grayscale refreshes.

---

### Boisti13/papers3-dashboard

- Repo: <https://github.com/Boisti13/papers3-dashboard>
- Useful files:
  - `src/display/epd_driver.cpp`
  - `src/touch/gt911.cpp`
  - `src/power/*`

Key takeaways:

#### Refresh model

- LVGL flush callback writes pixels into M5GFX framebuffer only.
- Physical e-paper refresh is deferred to `epd_driver_tick()`.
- Refresh is rate-limited:
  - minimum interval around `2000 ms`
  - after ~20 fast refreshes, do a quality/full refresh
- Fast path:
  - `M5.Display.setEpdMode(lgfx::epd_mode_t::epd_fastest)`
  - `M5.Display.display()`
  - `M5.Display.waitDisplay()`
- Full path:
  - `M5.Display.setEpdMode(lgfx::epd_mode_t::epd_quality)`
  - `M5.Display.display()`
  - `M5.Display.waitDisplay()`

#### Touch

- M5Unified initializes GT911; touch input can use `M5.Touch.getCount()` and `M5.Touch.getDetail(0)`.
- Synthetic tap injection after light-sleep wake can make the first wake touch also trigger UI action.

Vink implication:

- For shell UI, use a simple refresh scheduler: fast refresh most page transitions, quality refresh every N shell commits or after explicit full-refresh actions.
- Always `waitDisplay()` after physical commit in code paths that immediately depend on panel state.
- Do not refresh physically on every internal draw operation.

---

### m5stack/bootloader_components

- Repo: <https://github.com/m5stack/bootloader_components>
- Useful file:
  - `boot_hooks/boot_hooks.c`

Key takeaway:

- Boot hook disables USB Serial/JTAG D+ pullup early to avoid PaperS3 power/USB enumeration issues:
  - set `USB_SERIAL_JTAG_PAD_PULL_OVERRIDE`
  - clear `USB_SERIAL_JTAG_DP_PULLUP`
  - clear `USB_SERIAL_JTAG_USB_PAD_ENABLE`

Vink implication:

- Not needed for UI fixes now.
- Keep in mind if PaperS3 shows USB/battery boot instability.

## Vink Fix Checklist

Use this checklist when modifying Vink-PaperS3 firmware:

1. **Display init**
   - `M5.begin(cfg)` with PaperS3-safe config.
   - `M5.Display.powerSaveOff()` before rotation/display setup.
   - `setColorDepth(4)` for PaperS3 shell/read path.
   - fixed rotation `0` for user's handle-up physical orientation.

2. **CJK text**
   - Shell UI Chinese must use `FontManager` drawing, not M5 built-in `print()`.
   - Reader text can keep `EbookReader` font drawing path.

3. **Touch**
   - Normalize coordinates to `540x960` logical portrait.
   - Store start and last live touch points.
   - On release, classify using stored last point, not a fresh post-release `getDetail()`.
   - Tap threshold around 20-30 px.
   - Swipe threshold around 50-100 px.
   - Long press around 600-1000 ms, fire once per touch sequence.
   - Reader uses 3-zone tap navigation.

4. **Refresh**
   - Shell page commits use fast refresh by default.
   - Every ~20 shell commits, use quality refresh to reduce ghosting.
   - Wait for display after commits that are immediately followed by sleep/power actions.

5. **Power**
   - Before shutdown/sleep: save reader progress and stats.
   - Stop WiFi uploader and BLE services.
   - Give SD writes time to flush (`~500 ms`) before power-off.
   - Show shutdown/sleep screen and wait for display.
   - PaperS3 hardware power-off pulse: GPIO44 high briefly, then low; fallback to `M5.Power.powerOff()`/deep sleep.

6. **SD**
   - PaperS3 pins: CS47/SCK39/MOSI38/MISO40.
   - Try multiple SPI frequencies; 20-25 MHz may work, fall back to 8/4 MHz.

7. **UI style**
   - Keep Crosslink/Xiaohongshu visual style.
   - No decorative bottom hint strip.
   - Use page content action cards for touch hints, e.g. empty bookshelf `打开SD卡`.
