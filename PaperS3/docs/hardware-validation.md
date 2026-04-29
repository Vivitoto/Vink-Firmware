# PaperS3 Hardware Validation Playbook

This file exists because local builds and desktop previews are not enough for Vink/PaperS3.
Every firmware RC that touches UI, display, input, sleep, font rendering, or flashing assets must be validated with the levels below.

## Confidence levels

Use these exact labels in summaries:

- `本地已验证`: compile/smoke/build/manifest/static invariants passed locally.
- `需要真机验证`: behavior depends on PaperS3 hardware, EPD, GT911 touch, SD card, RTC/PMIC, side power key, or real display waveform.
- `已真机确认`: user or tester confirmed the behavior on an actual PaperS3.

Do not collapse these levels into “fixed”.

## Minimum RC checks before asking the user to burn

Local gates:

```bash
python3 tests/local_firmware_smoke.py --build --slug <slug>
```

The build gate is **full-only** for Vink-PaperS3: it may generate `firmware.bin` and `spiffs.bin` internally because the merged image needs app + resources, but the only user-facing/flasher artifact is `Vink-PaperS3-<version>-<slug>-full-16MB.bin` flashed from offset `0x0`.

Also inspect:

- UI preview / contact sheet matches actual firmware geometry.
- Hit-test rectangles match visible buttons/tabs.
- Touch code does not use release-time coordinates as the only Tap coordinate source.
- Display service serializes physical EPD pushes with `waitDisplay()` and `g_inDisplayPush`.
- Generated full image is exactly 16MB and includes bootloader + partition table + app + SPIFFS resources.
- No standalone OTA/app or SPIFFS artifacts are copied/published for new Vink-PaperS3 builds.
- Side power-key path ignores the boot press until release, then handles a stable later press as shutdown.

## First real-device smoke path

After flashing a full image, verify in this order:

1. Boot reaches the shell without clipped/rotated framebuffer.
2. Top status bar: time left, title centered, battery right.
3. Top tabs: `阅读 / 书架 / 同步 / 设置` all visibly readable.
4. Touch top tabs one by one; each must visibly navigate.
5. Touch home buttons: `打开` and `书架`.
6. Enter settings and return/back path if available.
7. Open book list with SD inserted and without SD inserted.
8. Open one TXT, page next/prev, open directory/TOC if available.
9. After boot has settled, press the side power key once; confirm the shutdown page appears, the device powers off, and it does not immediately reboot.

## If touch appears dead

Do not guess. Build a diagnostic RC that shows or logs:

- raw `M5.Touch.getDetail()` x/y/pressed/count
- normalized logical x/y
- event type: down/up/tap/swipe/longpress
- current `SystemState`
- `g_inDisplayPush` and/or display busy state
- selected `UiAction` from hit-test

Classify the failure:

- GT911 not initialized / no touch count
- raw coordinates present but transformed incorrectly
- input task not running
- display busy / push guard suppresses input forever
- hit-test geometry mismatch
- state machine receives event but action is wrong or ignored
- side power-key boot press was not ignored, or shutdown press was not converted into a `PowerButton` event
- PMIC GPIO44 power-off pulse does not fully cut power

## If UI photo looks wrong

Treat real photos as higher priority than desktop previews. Check:

- black-filled selected tabs swallowing white CJK text
- CJK font too large for tab/card/button height
- anti-alias gray pixels too light on EPD/photo
- text baselines and card padding
- long Chinese labels/values overflowing card or button
- preview elements that do not correspond to actual firmware controls

## Release rule

Do not push/tag/release a new Vink firmware merely because the local gate passed.
For UI/touch/display changes, first give the user a local RC full image and wait for real-device confirmation.
