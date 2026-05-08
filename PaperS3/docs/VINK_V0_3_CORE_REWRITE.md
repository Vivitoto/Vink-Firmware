# Vink-PaperS3 v0.3.0 Vink-Core Rewrite

Status: design baseline for new `v0.3.0-vink-core` branch.

## 2026-05-08 Source Separation Correction

Do **not** conflate these firmware/code sources:

1. **Vink current code** — the code in this repository. This is the implementation we own and evolve.
2. **M5ReadPaper / ReadPaper source** — optional generic / known-good reference only. It can inform PaperS3 hardware bring-up, display/input/task patterns, file handling, or pagination ideas, but it is **not** the 梦西游 / EDCBook source and must not be treated as the target kernel to port wholesale.
3. **梦西游 / EDCBook binary** — the target UX/layout/feature reference only, based on reverse evidence from the shipped binary and extracted assets/strings. If actual EDCBook source is unavailable, implement EDCBook-derived behavior as a clean-room Vink feature spec; do **not** rebuild Vink as M5ReadPaper under another name.

Updated integration rule: Vink remains the product/kernel boundary; M5ReadPaper is a technical reference; EDCBook/Dream is the UX/layout target. Any copied/adapted source must come only from legally available reference projects with license review, while EDCBook binary observations should become clean-room requirements and tests.

## Decision

The `v0.2.x` architecture is abandoned for the next line. Do not keep patching the old `App::run()` + ad-hoc Shell Canvas state machine.

`v0.3.0` should rebuild the PaperS3 firmware around Vink's proven low-level architecture, then layer Vink UI and Legado reading sync on top.

Vink is Apache-2.0 licensed in the local reference mirror. If code is copied/adapted, preserve license notices where appropriate.

## Hardware / Framework Context

- Board: M5Stack PaperS3
- Chip: ESP32-S3
- Framework: current Vink uses Arduino + M5Unified, same broad runtime family as Vink.
- Display: PaperS3 e-paper, logical portrait 540x960.
- Touch: GT911 via M5Unified touch path / PaperS3 INT line.
- Storage: 16MB flash, SPIFFS resources, SD `/books`.
- PSRAM: OPI PSRAM required for global full-screen canvas.

## Non-negotiable v0.3.0 Architecture

### 1. Vink service-style task split

Current Vink v0.2 has one giant `App::run()` loop. Replace it with task/message ownership like Vink:

- Main task initializes hardware and starts services.
- State machine task owns app state and handles messages.
- Device/input task polls touch/power/battery and posts messages.
- Display push task is the only physical e-paper writer.
- Optional background task handles indexing/cache/sync work.

Vink references:

- `external PaperS3 reference firmware/src/main.cpp`
  - `MainTask()` creates app runtime tasks.
  - Starts `initializeStateMachine()`, `initializeTimerInterrupt()`, `initializeDeviceInterrupt()`, `initializeDisplayPushTask()`.
- `external PaperS3 reference firmware/src/tasks/state_machine_task.cpp`
  - `StateMachineTask::initialize()` creates queue/task.
  - `StateMachineTask::sendMessage()` is the state mutation gate.
  - `StateMachineTask::taskFunction()` blocks on queue and dispatches by current state.
- `external PaperS3 reference firmware/src/tasks/device_interrupt_task.cpp`
  - Dedicated polling task driven by timer notification.
  - Calls `M5.update()`, touch, battery, orientation checks.
- `external PaperS3 reference firmware/src/tasks/display_push_task.cpp`
  - Dedicated display queue and push task.
  - Calls `M5.Display.waitDisplay()` before pushing.

### 2. Global canvas-first rendering

All Vink UI and reader pages should draw to a full-screen canvas. No UI screen should draw directly to `M5.Display` except boot/shutdown emergency screens.

Vink references:

- `external PaperS3 reference firmware/src/main.cpp`
  - global `M5Canvas *g_canvas = nullptr;`
- `external PaperS3 reference firmware/src/init/setup.cpp`
  - creates `g_canvas = new M5Canvas(&M5.Display);`
  - `g_canvas->createSprite(PAPER_S3_WIDTH, PAPER_S3_HEIGHT);`
  - initializes display push task early.
- `external PaperS3 reference firmware/src/device/ui_display.cpp`
  - `display_print()` and wrapped text render into `g_canvas`, not directly into display.
- `external PaperS3 reference firmware/src/tasks/display_push_task.cpp`
  - display task consumes canvas clone / global canvas and performs physical push.

Vink v0.3 rule:

- UI code receives a `M5Canvas&` or uses a single renderer API backed by `g_canvas`.
- Physical commit is `DisplayService::enqueue(...)` only.
- No `display.display()` from UI state handlers.
- No per-character display writes.

### 3. Display push service copied from Vink model

Physical e-paper refresh must be serialized and queued.

Required behavior:

- Queue message includes flags: transparent, invert, quality/full, effect, rect.
- Display service waits for prior EPD work before push.
- Refresh mode is selected centrally based on counters/config.
- Quality cleanup happens periodically.
- `inDisplayPush` flag prevents input/orientation churn during physical refresh.

Vink references:

- `external PaperS3 reference firmware/src/tasks/display_push_task.h`
  - `DisplayPushMessage`
  - `enqueueDisplayPush(...)`
  - `waitDisplayPushIdle(...)`
  - `enqueueCanvasCloneBlocking(...)`
- `external PaperS3 reference firmware/src/tasks/display_push_task.cpp`
  - `displayTaskFunction()`
  - `M5.Display.waitDisplay()`
  - `s_pushCount`
  - `QUALITY_REFRESH`, `MIDDLE_REFRESH`, `LOW_REFRESH`, `NORMAL_REFRESH`

Vink simplification for first v0.3.0 RC:

- Keep effects minimal: `NOEFFECT`, full-screen push, maybe rect push later.
- Do not copy complex shutter effects until base is stable.
- Keep queue and single writer exactly.

### 4. State machine instead of ad-hoc flags

Vink v0.3 states should be message-driven:

- `BOOT`
- `HOME`
- `LIBRARY`
- `READER`
- `READER_MENU`
- `TRANSFER`
- `SETTINGS`
- `LEGADO_SYNC`
- `SLEEPING` / `SHUTDOWN`

Messages:

- `MSG_TOUCH_DOWN`
- `MSG_TOUCH_UP`
- `MSG_TAP`
- `MSG_SWIPE_LEFT/RIGHT/UP/DOWN`
- `MSG_LONG_PRESS`
- `MSG_DISPLAY_DONE`
- `MSG_OPEN_BOOK`
- `MSG_PAGE_NEXT/PREV`
- `MSG_LEGADO_SYNC_START`
- `MSG_LEGADO_SYNC_DONE/FAILED`
- `MSG_SLEEP_TIMEOUT`
- `MSG_POWER_BUTTON`

Vink reference:

- `external PaperS3 reference firmware/src/tasks/state_machine_task.cpp`
- `external PaperS3 reference firmware/src/tasks/state_*.cpp`

Vink rule:

- State changes only happen inside state machine task.
- UI render is triggered by state transition or explicit dirty event.
- Input task never directly renders.

### 5. Touch/input model copied from Vink style

Current Vink touch handling was repeatedly fragile. v0.3 should use input service + queue.

Required behavior:

- Device task calls `M5.update()` on a fixed tick.
- Track touch press/release, start coordinate, last coordinate, timing.
- Convert to semantic events before state machine sees them.
- Debounce and transition lockout live in input service/state machine, not UI render code.
- During `inDisplayPush`, avoid emitting orientation or repeated UI actions.

Vink references:

- `external PaperS3 reference firmware/src/tasks/device_interrupt_task.cpp`
  - `DEVICE_INTERRUPT_TICK`
  - `checkTouchStatus()`
  - `TOUCH_PRESS_GAP_MS`
  - `inDisplayPush` guard for orientation.

Vink additions:

- Keep PaperS3 logical portrait transform.
- Reader tap zones: left/center/right thirds.
- UI uses hitboxes from Vink layout renderer.

### 6. Font/text path should follow Vink, not ad-hoc tiny built-in UI font

v0.2 failed because Shell UI fonts were hacked through a partial built-in glyph table. v0.3 should use a real text engine / font cache path similar to Vink.

Vink references:

- `external PaperS3 reference firmware/src/text/bin_font_print.cpp`
- `external PaperS3 reference firmware/src/text/bin_font_print.h`
- `external PaperS3 reference firmware/src/device/ui_display.cpp`
  - `display_print()` uses `bin_font_print(..., g_canvas, ...)`.

Vink v0.3 direction:

- Use one robust CJK-capable font path for both UI and reader.
- Font data should come from bundled SPIFFS resource or compiled resource with complete UI coverage.
- UI must never fall back to incomplete glyph sets silently.
- Add a test that every UI string is renderable through chosen font engine.

First implementation preference:

- Port/adapt Vink `bin_font_print` stack and required font cache files.
- Use a known-good bundled font resource in SPIFFS/full image.
- Keep SD custom fonts only for reader正文 later.

### 7. Legado sync is a service, not UI code

Legado should be a background/service module using state machine messages.

Initial v0.3 target:

- Config: host, port/base URL, username/token if needed.
- Pull remote bookshelf/progress metadata.
- Push current book progress.
- Pull progress for current book before opening.
- Conflict rule: newest timestamp wins, but never silently overwrite local progress without logging.
- UI: Transfer/Sync card shows last sync status and manual sync button.

Integration points:

- `MSG_LEGADO_SYNC_START`
- `MSG_LEGADO_SYNC_DONE`
- `MSG_LEGADO_SYNC_FAILED`
- Reader emits progress-save event to Legado service.

Current code structure:

- v0.3 keeps a lightweight `src/vink3/sync/LegadoService.*` service boundary.
- The old v0.2 `LegadoSync.*` / `WebDavClient.*` active-source files were removed from `src/`; future HTTP/WebDAV work should be rebuilt inside service boundaries instead of reviving UI-coupled legacy code.

### 8. Vink UI on top of Vink core

Do not copy Vink UI style. Copy the bottom architecture only.

Vink UI direction:

- Crosslink/Vink-style monochrome e-paper UI.
- Top status row: time, battery, sync/wifi indicators.
- Main pages: Reading, Library, Transfer/Sync, Settings.
- Cards/panels, not Vink lockscreen/menu aesthetic.
- All drawing goes into global canvas and queues display push.

## Proposed Implementation Phases

### Phase 0 — Repository safety

Done:

- Preserved abandoned v0.2.16 commit on branch `wip/v0.2.16-ui-font-coverage`.
- Created new branch `v0.3.0-vink-core` from `origin/main`.

### Phase 1 — Add v0.3 architecture scaffold

Create new code under `PaperS3/src/vink3/` first, without deleting old code:

- `runtime/VinkRuntime.*`
- `display/DisplayService.*`
- `input/InputService.*`
- `state/StateMachine.*`
- `ui/VinkUiRenderer.*`
- `reader/ReaderController.*`
- `sync/LegadoService.*`

Goal: compile alongside old code, then switch `main.cpp` to new runtime once stable.

### Phase 2 — Port display service

- Port simplified Vink display queue model.
- Full-screen canvas push only at first.
- Add display idle wait.
- Add push counters and quality cleanup.
- Add smoke test: no UI code calls `M5.Display.display()` directly.

### Phase 3 — Port input/state model

- Input task emits semantic events.
- State task owns all state changes.
- No render in input task.
- Add simulated state tests where possible.

### Phase 4 — Replace UI renderer

- Implement Vink UI on global canvas.
- Four main tabs first.
- Keep layout simple and dense.
- No Legado yet.

### Phase 5 — Reader + font path

- Port/adapt Vink text/font path.
- Open local TXT first.
- Page next/prev and progress save.
- Add font coverage/renderability test.

### Phase 6 — Legado service

- Add config and manual sync card.
- Pull/push reading progress.
- Add logs/status page.

### Phase 7 — v0.3.x release candidate

- Version top manifest to the RC/release version.
- Build **full-only** merged 16MB image; do not publish standalone OTA/app or SPIFFS artifacts for new Vink PaperS3 builds.
- Run local smoke tests.
- Only then ask user whether to push/release.

## Local Test Gate for v0.3

Update `tests/local_firmware_smoke.py` to check the new invariants:

- `vink3/display/DisplayService` exists.
- State/input/display services are separate.
- UI files do not call `M5.Display.display()`.
- All physical display calls are inside DisplayService or emergency boot/shutdown path.
- `main.cpp` starts the v0.3 runtime.
- Manifest version is `v0.3.0` when release candidate is prepared.

## Explicit Hardware Risks

Even with Vink architecture copied, local tests cannot prove:

- Real EPD waveform/ghosting quality.
- GT911 electrical/firmware touch behavior.
- PMIC shutdown/wake behavior.
- PSRAM-backed canvas behavior under long rapid interaction on real device.

These require one real burn test per RC.

## 2026-04-28 Update: Remote Vink reference core Baseline

The rewrite baseline is now the remote upstream, not the older local mirror:

```text
repo: https://github.com/shinemoon/external PaperS3 reference firmware
branch: main
commit: e910d29
version file: data/version => V1.7.6
```

The local reference checkout is:

```text
/home/vito/.openclaw/workspace/Vink/reference-firmware/external PaperS3 reference firmware-latest
```

Current v0.3 implementation changes made against that baseline:

- `src/main.cpp` now follows Vink's startup model: Arduino `setup()` creates a pinned `MainTask`, while the runtime owns service startup.
- `src/vink3/VinkPaperS3Core.h` records the upstream repo/version/commit and carries the PaperS3 display constants/refresh thresholds.
- `src/vink3/runtime/VinkRuntime.*` performs Vink service-style hardware init: `clear_display=false`, PaperS3 fallback board, power/IMU/RTC enabled, GPIO48 wake enabled, global 540x960 4bpp canvas allocated early.
- `src/vink3/display/DisplayService.*` now mirrors Vink's display-push model more closely: request queue plus canvas-clone FIFO, `g_inDisplayPush`, `powerSaveOff()`, `waitDisplay()` serialization, push counters, and quality/normal refresh selection.
- `src/vink3/input/InputService.*` remains a polling input task and suppresses event generation while display push is active.
- `src/vink3/state/StateMachine.*` is the state owner and triggers UI renders/DisplayService commits via queued messages.
- `tests/local_firmware_smoke.py` now detects v0.3 mode and checks v0.3 architecture invariants instead of only v0.2 shell guards.

Next work should move beyond the scaffold without confusing the sources:

1. Keep the active implementation in **Vink current code** (`src/vink3/*`) and evolve it in small, testable steps.
2. Use **M5ReadPaper / ReadPaper source** only as optional known-good reference for generic PaperS3 engineering patterns such as timer-driven input polling, battery/power/orientation checks, display queue serialization, font-cache structure, or pagination heuristics. Do not port it wholesale, and do not assume it represents 梦西游 / EDCBook internals.
3. Use **梦西游 / EDCBook binary reverse evidence** as the target UX/layout/feature spec: reader layout, bookshelf interactions, refresh/user settings, Web UI capabilities, encoding support, cover/lock-screen behavior, and the 10MB font-partition strategy.
4. Where EDCBook source is unavailable, perform clean-room reimplementation in Vink: derive requirements from observed strings/assets/behavior, write Vink-native service/UI code, and add tests that assert the behavior without copying unavailable source.
5. Integrate Legado progress sync on top of Vink reader events, not inside UI drawing.

## 2026-04-28 UI / Click Logic Rebuild Note

The first v0.3 Vink shell has been restructured to make UI refactoring closer to Vink's existing click model:

- `VinkUiRenderer` now owns both layout and hit-testing through `UiAction hitTest(SystemState, x, y)`.
- `StateMachine` no longer hard-codes raw UI rectangles directly. It asks the renderer for a semantic `UiAction`, then changes state or starts a service.
- Top-level shell now has four tabs: Reader, Library, Transfer/Sync, Settings.
- Swipe left/right cycles tabs through the state machine.
- Legado starts from a UI action and reports through state-machine messages, keeping service logic out of UI drawing.

Difficulty assessment:

- UI shell refactor itself is moderate/low difficulty now that layout + hit-test are centralized.
- Reusing Vink's click logic is practical at the state-handler/action level, not by copying its exact UI coordinates.
- The harder parts are below/behind the UI: robust CJK font rendering, book/page pipeline, and true Legado progress conflict handling.

## 2026-04-28 CJK UI Text Gate

The v0.3 UI is Simplified Chinese, so it must not depend on M5GFX `drawString()` for Chinese labels.

Findings from Vink reference core font analysis:

- Full Vink `src/text/lite.cpp` embeds a generated font of about 7.3MB (`g_progmem_font_size = 7342411`).
- Current Vink PaperS3 OTA app slot is `0x600000` (6MB), so the full Vink PROGMEM font cannot fit without changing the partition/release strategy.
- Full `lite.bin` also does not fit the current ~4MB SPIFFS partition.
- Therefore the safe path is staged:
  1. UI text first: use a small bundled CJK bitmap renderer and ban `drawString()` in `VinkUiRenderer`.
  2. Generate a Vink-format subset font for Vink UI strings.
  3. Later decide whether the full reader font lives on SD, in a larger non-OTA partition layout, or as a subset/cache pipeline.

Implemented first-stage gate:

- Added `src/vink3/text/CjkTextRenderer.*`.
- `VinkUiRenderer` now routes UI labels through `g_cjkText` instead of `canvas_->drawString()`.
- Smoke test checks that `VinkUiRenderer.cpp` does not contain `drawString` and that CJK rendering uses bundled bitmap font before fallback.

This is not yet the full Vink `bin_font_print` text/page engine. It is the minimal UI-safe bridge needed before porting Vink's larger text subsystem.
