# PaperS3 / ESP32 E-reader Reference Project Learnings

This is the consolidated reference-learning document for Vink-PaperS3.
It merges and supersedes the scattered lessons from:

- `Competitor-Analysis.md`
- `EDC-Book-Reverse-Analysis.md`
- `PaperS3_Firmware_References.md`
- `market-study-notes.md`
- v0.3 ReadPaper-core rewrite notes

The goal is not to copy another product UI. The goal is to absorb proven engineering patterns for PaperS3 display, touch, input mapping, reader UX, files, fonts, and diagnostics.

## Non-negotiable product boundary

Vink UI should remain Vink/Crosslink-like:

- top status bar + four tab bookmarks
- simple monochrome e-paper cards/list rows
- compact settings rows
- no decorative bottom hint strip
- Chinese-first shell, readable on PaperS3

Reference projects are used for low-level and UX mechanics only:

- display init and refresh discipline
- touch pipeline and gesture handling
- input-state clearing
- reader navigation zones
- font/rendering reliability
- diagnostics and hardware validation
- ebook feature roadmap

---

## Reference projects studied

### 1. `shinemoon/M5ReadPaper`

Role: strongest PaperS3 low-level firmware reference.

Useful areas:

- `src/main.cpp`
- `src/init/setup.cpp`
- `src/device/ui_display.cpp`
- `src/text/bin_font_print.*`
- `src/tasks/display_push_task.*`
- `src/tasks/device_interrupt_task.*`
- `src/ui/ui_control.cpp`
- `src/ui/show_debug.*`

Key lessons:

- Use task split: main/runtime, state machine, input/device, display push.
- Render to a global full-screen canvas first; one display task commits to EPD.
- Call `M5.Display.waitDisplay()` before/around physical pushes.
- Keep an `inDisplayPush` style guard so input/orientation churn does not overlap refresh.
- CJK rendering must use a real font/text path, not `M5.Display.print()` fallback.
- Reader touch can be coarse semantic zones rather than tiny buttons.
- ReadPaper maps the 540×960 screen into a 6×10 grid (`90×96px`) for robust touch zones.

Vink implications:

- Keep `DisplayService` as the single physical writer.
- Keep CJK UI/reader text on canvas-backed font renderers.
- Add a Vink `TouchMapper` layer for coarse zones and state-aware actions.
- Add visible touch/debug screens instead of guessing from code.

---

### 2. `juicecultus/crosspoint-reader-papers3`

Role: best product/architecture reference for a feature-complete PaperS3 reader.

Useful areas:

- `src/MappedInputManager.*`
- `src/activities/Activity.*`
- `src/activities/ActivityManager.*`
- `src/activities/RenderLock.h`
- `lib/hal/HalDisplay.cpp`
- `lib/hal/HalGPIO.cpp`
- `lib/hal/HalPowerManager.cpp`
- `src/components/UITheme.*`

Key lessons:

- Use `MappedInputManager` to convert raw input into semantic app actions.
- Keep raw touch/button reading separate from UI/business logic.
- Store last touch x/y for tap-to-select, but actions go through mapped states.
- Clear stale input on activity transitions: `mappedInput.clearState()` on enter/pop.
- Restore touch orientation when switching reader/non-reader contexts.
- Use `RenderLock` / render task to serialize rendering and avoid TOCTOU races.
- Request full e-ink refresh on major activity transitions to reduce ghosting.
- Footer buttons work as a mode-specific affordance, but Vink should not copy the decorative bottom-bar style.

Vink implications:

- Split `InputService` (raw) from `TouchMapper` (state/action mapping).
- Add explicit input clearing after state transitions and major redraws.
- Treat display rotation and touch orientation as a paired invariant.
- Keep display serialization and expose diagnostics for `displayBusy` / `g_inDisplayPush`.

---

### 3. `omeriko9/M5Paper_PaperS3_eBookReader`

Role: PaperS3 gesture and ESP-IDF device-HAL reference.

Useful areas:

- `main/device_hal.*`
- `main/gesture_detector.*`
- `main/gui.*`
- `main/main.cpp`

Key lessons:

- M5Unified PaperS3 init should set `fallback_board = board_M5PaperS3`, disable unused speaker/mic, enable RTC/IMU, and explicitly set rotation.
- Gesture detector tracks start/current/last point, duration, velocity, tap/double-tap/long-press/swipes.
- Emit most gestures on release; long-press can fire while finger remains down, but only once per sequence.
- Keep HAL boundaries for display size, rotation, SD mount, sleep, buzzer, and IMU.

Vink implications:

- Keep touch detector stateful: down point + last live point + long-press fired flag.
- Do not depend on release-time `getDetail()` coordinates; cache last valid pressed coordinate.
- Future: consider a small device-HAL boundary for PaperS3-specific pins/orientation/power.

---

### 4. `atomic14/diy-esp32-epub-reader`

Role: EPUB architecture and UI loop reference.

Useful areas:

- EPUB ZIP/XML parsing pipeline
- board abstraction
- touch controls / visible feedback pattern

Key lessons:

- EPUB pipeline is ZIP → container.xml → OPF → spine → XHTML → layout blocks → pagination.
- Use `miniz` + XML/HTML parsing instead of ad-hoc text extraction.
- Touch controls can render pressed-state feedback so the user knows touch was received.

Vink implications:

- EPUB is the largest product gap; design it as a pipeline with cacheable parsed metadata/pages.
- Add visible touch feedback or diagnostic overlay for hardware debugging.

---

### 5. `alexcircuits/M5Stack-PaperS3-UniRead`

Role: raw GT911 and watchdog reference.

Key lessons:

- GT911 pins: SDA GPIO 41, SCL GPIO 42.
- Possible GT911 addresses: `0x14`, `0x5D`.
- Registers:
  - status: `0x814E`
  - first point: `0x8150`
- After reading, write `0` to `0x814E` to clear data-ready flag.
- Touch polling around 20 ms; debounce around 300 ms; swipe around 100 px; long-press around 600 ms.
- Heavy EPD work can interact with task watchdog.

Vink implications:

- If M5Unified touch path remains unreliable, add a raw-GT911 diagnostic/fallback path.
- Add diagnostic mode that reports touch count/raw coords before trying more UI fixes.

---

### 6. `Boisti13/papers3-dashboard`

Role: LVGL/e-paper refresh scheduling reference.

Key lessons:

- LVGL flush writes to framebuffer only; physical e-paper refresh is deferred and rate-limited.
- Fast refresh most updates; quality/full refresh every N fast refreshes.
- Always `waitDisplay()` after physical commit.
- Avoid refreshing physically on every internal draw.

Vink implications:

- Continue central display queue.
- Consider quality refresh cadence for shell transitions and diagnostic screens.
- Never reintroduce direct shell display writes.

---

### 7. EDC Book commercial firmware reverse analysis

Role: closed-source commercial feature benchmark.

Key findings:

- 16MB firmware with large partition split:
  - app around 5MB
  - font partition around 10MB
  - LittleFS/Web UI resources
- Supports TXT and EPUB.
- Supports encodings beyond Vink: UTF-8, GBK, UTF-16LE/BE, BIG5.
- Touch reading: left third previous, right area next, plus swipe navigation.
- Bluetooth HID page turner support is mature.
- Web UI supports file management, WiFi config, font config, reading settings.
- Cover/lock-screen system supports `/cover`, `/bg`, `/pic` images and matched book covers.
- Recent reading exists via `recents.ini`.
- Supports subdirectories, cover matching, lock screens, OTA-like distribution via M5Burner.

Vink implications:

- Long-term product bar is higher than TXT-only reader.
- Font strategy matters: a large reliable built-in font resource dramatically improves first-run quality.
- Web file/config UI should evolve beyond basic upload.
- Add cover/lock-screen/recent-reading polish after touch/display reliability is stable.

---

## Feature gap synthesis

### Already strong or partially strong in Vink

- Chinese TXT path, UTF-8/GBK support.
- Chapter detection and caches.
- Full 16MB image with bundled SPIFFS fonts.
- Reader/body font separated from UI font.
- Display queue and canvas-first v0.3 architecture.
- Legado service scaffold.
- Local smoke/build/manifest validation.

### High-priority gaps

1. **Touch robustness and diagnostics**
   - Need TouchMapper and touch diagnostic page.
   - Must classify failures instead of guessing.

2. **EPUB support**
   - Biggest format gap versus CrossPoint, diy-epub-reader, EDC Book.
   - Needs ZIP/XML/HTML pipeline and cache.

3. **Recent reading / continue reading quality**
   - High daily-use value.
   - Should be prominent on home/library.

4. **Quick jump / TOC UX**
   - Long novels require page/percentage/chapter jump.

5. **Web file/config UI**
   - EDC Book and CrossPoint have much stronger transfer/config surfaces.

6. **Cover / lock screen polish**
   - Important for e-paper product feel.

---

## Architecture rules for Vink going forward

### Input/touch

- `InputService` owns polling and raw event collection.
- `TouchMapper` should own state-specific mapping:
  - shell tabs/buttons/cards
  - reader left/center/right zones
  - settings rows
  - diagnostics
- Cache touch-down point and last valid pressed point.
- Do not use post-release `getDetail()` as the only source for tap coordinates.
- Clear stale input on state transitions.
- Reader mode must use coarse zones, not precise small controls.
- Orientation transform must be paired with display rotation.

### Display/EPD

- One physical writer only: `DisplayService`.
- All normal UI renders to canvas first.
- `waitDisplay()` around physical pushes.
- Guard input during physical push, but diagnostic pages must show if the guard is stuck.
- Full/quality refresh on major transitions; fast/partial refresh only where validated.

### UI

- Vink visual style remains Crosslink-like, not ReadPaper-like.
- Real photos outrank desktop previews.
- Avoid black-filled CJK selected tabs unless verified on hardware; black text on white/outlined controls is safer.
- UI font size must fit PaperS3 tab/card/button geometry.
- Hit-test rectangles must match visible controls.

### Fonts

- Shell UI requires reliable Simplified Chinese bundled font.
- Reader body can use fuller/proven reader font.
- No silent fallback to incomplete glyph tables for user-facing UI.
- Consider a larger built-in font/partition strategy later, inspired by EDC Book.

### Diagnostics

A diagnostic build/page should show:

- raw `M5.Touch.getCount()`
- raw `getDetail()` x/y/pressed
- normalized x/y
- event type: down/up/tap/swipe/longpress
- current `SystemState`
- hit-test/semantic action
- `g_inDisplayPush` and display busy
- last input timestamp

Touch failure categories:

- GT911 not initialized / no touch count
- raw coordinates present but transformed incorrectly
- input task not running
- display busy / push guard suppresses input forever
- hit-test geometry mismatch
- state machine receives event but ignores/wrongly maps action

---

## Implementation roadmap influenced by references

### P0: Stabilize hardware interaction

- Add TouchMapper.
- Add touch/display diagnostic page or diagnostic build.
- Add transition input clear / require release.
- Keep smoke invariants for cached touch coordinates and display serialization.

### P1: Restore reader product basics

- Coarse reader zones: left prev, center menu, right next.
- Better recent-reading home card.
- Strong TOC/page-jump flow.
- Book open/loading diagnostics for SD/large TXT.

### P2: Match market feature expectations

- EPUB pipeline and cache.
- Web file/config UI upgrade.
- Cover thumbnails and lock screen.
- Bluetooth HID polish.
- More encodings: UTF-16LE/BE and BIG5.

### P3: Advanced polish

- OPDS / KOReader-style sync.
- Orientation settings after touch/display transform is tested.
- OTA/self-update once partition/release strategy is solid.

---

## Release/validation rule

For any UI, touch, display, font, sleep, or power change:

1. Run local build/smoke.
2. Generate/inspect preview only as an aid, not as proof.
3. Produce a local RC full image.
4. User verifies on real PaperS3:
   - boot
   - visible UI
   - tabs
   - touch buttons
   - reader open/page
   - settings/back
5. Only after real-device confirmation, prepare GitHub push/release checklist.

Use these labels precisely:

- `本地已验证`
- `需要真机验证`
- `已真机确认`
