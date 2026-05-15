#!/usr/bin/env python3
"""Local smoke tests for Vink-PaperS3 firmware.

This is not a PaperS3 hardware emulator. It is a deterministic local gate for
things that repeatedly caused real-device regressions:

- source-level display/touch invariants
- active src/ contains only the v0.3 runtime plus shared utilities
- releases.json top asset sizes match existing release artifacts
- optional PlatformIO build/buildfs/full-merge with full-image size checks
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
REPO = PROJECT.parent
WORKSPACE = Path("/home/vito/.openclaw/workspace")
ARTIFACTS = WORKSPACE / "artifacts" / "Vink-PaperS3"
DEFAULT_SLUG = "ui-restore"
APP_SLOT_SIZE = 0xC00000  # v0.3 single-app layout for full ReadPaper PROGMEM font
SPIFFS_SIZE = 0x3F0000
FULL_FLASH_SIZE = 0x1000000


class CheckFailed(Exception):
    pass


def run(cmd: list[str], cwd: Path = PROJECT) -> None:
    print("$", " ".join(cmd))
    subprocess.run(cmd, cwd=str(cwd), check=True)


def ok(msg: str) -> None:
    print(f"[OK] {msg}")


def fail(msg: str) -> None:
    raise CheckFailed(msg)


def read(path: str) -> str:
    return (PROJECT / path).read_text(encoding="utf-8")


def assert_contains(text: str, needle: str, label: str) -> None:
    if needle not in text:
        fail(f"missing invariant: {label} ({needle})")
    ok(label)


def assert_not_contains(text: str, needle: str, label: str) -> None:
    if needle in text:
        fail(f"forbidden pattern present: {label} ({needle})")
    ok(label)


def assert_gray_font_covers(path: str, chars: str, label: str) -> None:
    data = (PROJECT / path).read_bytes()
    if data[:4] != b"GRAY" or len(data) < 16:
        fail(f"invalid GRAY font for {label}: {path}")
    count = struct.unpack_from("<I", data, 8)[0]
    codepoints = {struct.unpack_from("<I", data, 16 + i * 16)[0] for i in range(count)}
    missing = "".join(ch for ch in chars if ord(ch) not in codepoints)
    if missing:
        fail(f"font coverage missing for {label}: {missing[:80]}")
    ok(label)


def source_invariants() -> None:
    main_cpp = read("src/main.cpp")
    assert_contains(main_cpp, "vink3/runtime/VinkRuntime.h", "firmware entrypoint uses the v0.3 runtime")

    removed_legacy = [
        "App.cpp", "App.h",
        "BlePageTurner.cpp", "BlePageTurner.h",
        "EbookReader.cpp", "EbookReader.h",
        "EpubParser.cpp", "EpubParser.h",
        "FileBrowser.cpp", "FileBrowser.h",
        "JsonHelper.h",
        "LegadoSync.cpp", "LegadoSync.h",
        "ReadingStats.cpp", "ReadingStats.h",
        "RecentBooks.cpp", "RecentBooks.h",
        "UITheme.cpp", "UITheme.h",
        "WebDavClient.cpp", "WebDavClient.h",
        "WiFiUploader.cpp", "WiFiUploader.h",
        "ZipFile.cpp", "ZipFile.h",
    ]
    leftovers = [name for name in removed_legacy if (PROJECT / "src" / name).exists()]
    if leftovers:
        fail(f"obsolete v0.2 monolithic source files should not live in active src/: {', '.join(leftovers)}")
    ok("obsolete v0.2 monolithic source files are removed from active src/")

    platformio = read("platformio.ini")
    assert_contains(platformio, "pre:tools/apply_m5gfx_patches.py", "PlatformIO applies the reproducible M5GFX Panel_EPD patch")
    assert_not_contains(platformio, "ElegantOTA", "unused legacy OTA dependency is not listed")
    assert_not_contains(platformio, "ArduinoJson", "unused legacy JSON dependency is not listed")

    font = read("src/FontManager.cpp")
    assert_not_contains(font, "SPIFFS.begin(true", "Font manager must not format SPIFFS on mount failure")

    return vink3_source_invariants(main_cpp)


def vink3_source_invariants(main_cpp: str) -> None:
    display_h = read("src/vink3/display/DisplayService.h")
    display_cpp = read("src/vink3/display/DisplayService.cpp")
    panel_epd_cpp = read(".pio/libdeps/m5papers3/M5GFX/src/lgfx/v1/platforms/esp32/Panel_EPD.cpp")
    panel_epd_h = read(".pio/libdeps/m5papers3/M5GFX/src/lgfx/v1/platforms/esp32/Panel_EPD.hpp")
    runtime_cpp = read("src/vink3/runtime/VinkRuntime.cpp")
    input_cpp = read("src/vink3/input/InputService.cpp")
    state_cpp = read("src/vink3/state/StateMachine.cpp")
    legado_cpp = read("src/vink3/sync/LegadoService.cpp")
    ui_cpp = read("src/vink3/ui/VinkUiRenderer.cpp")
    webui_cpp = read("src/vink3/webui/WebUiService.cpp")
    system_log_cpp = read("src/vink3/system/SystemLog.cpp")
    cjk_cpp = read("src/vink3/text/CjkTextRenderer.cpp")
    ui_font_cpp = read("src/vink3/text/VinkUiFont24.cpp")
    reader_cpp = read("src/vink3/reader/ReaderTextRenderer.cpp")
    reader_book_h = read("src/vink3/reader/ReaderBookService.h")
    reader_book_cpp = read("src/vink3/reader/ReaderBookService.cpp")
    chapter_cpp = read("src/ChapterDetector.cpp")
    codec_cpp = read("src/TextCodec.cpp")
    toc_tool = read("tools/detect_txt_toc.py")
    m5gfx_patch_tool = read("tools/apply_m5gfx_patches.py")
    partitions_csv = read("custom_16MB.csv")
    full_font_h = read("src/vink3/text/ReadPaperFullFont.h")
    gbk_table_h = read("src/vink3/text/GbkUnicodeTable.h")
    upstream = read("src/vink3/VinkPaperS3.h")
    platformio = read("platformio.ini")
    manifest = json.loads((PROJECT / "releases.json").read_text(encoding="utf-8"))
    latest_version = manifest["releases"][0]["version"]

    assert_contains(main_cpp, "xTaskCreatePinnedToCore", "v0.3 main starts a ReadPaper-style pinned MainTask")
    assert_contains(upstream, f"kVinkPaperS3FirmwareVersion = \"{latest_version}\"", "single firmware version constant matches the manifest top version")
    assert_contains(main_cpp, "kVinkPaperS3FirmwareVersion", "main task init log uses the shared firmware version")
    assert_contains(runtime_cpp, "kVinkPaperS3FirmwareVersion", "runtime boot logs use the shared firmware version")
    assert_not_contains(main_cpp, "v0.3.2-rc", "main task must not show stale firmware version")
    assert_not_contains(ui_cpp, "v0.3.4-rc", "settings/about must not show stale firmware version")
    assert_not_contains(reader_cpp, "v0.3.2-rc", "reader management pages must not show stale firmware version")
    assert_contains(runtime_cpp, "[Vink ", "v0.3 runtime prints Vink firmware identity on boot")
    assert_contains(runtime_cpp, "applyOfficialPaperS3DisplaySetup", "v0.3 official baseline uses the official UserDemo display setup")
    assert_contains(runtime_cpp, "M5.Display.setRotation(kPaperS3DisplayRotation)", "v0.3 PaperS3 display rotation starts from the official touch-profile constant")
    assert_contains(upstream, "kPaperS3DisplayRotation = 0", "official touch-example rotation 0 is the Vink diagnostic baseline")
    assert_contains(upstream, "gPaperS3ActiveDisplayRotation", "active rotation is exposed for diagnostics after runtime verification")
    assert_contains(upstream, "kGt911SdaPin = GPIO_NUM_41", "official GT911 SDA pin is recorded")
    assert_contains(upstream, "kGt911SclPin = GPIO_NUM_42", "official GT911 SCL pin is recorded")
    assert_contains(upstream, "kGt911IntPin = GPIO_NUM_48", "official GT911 INT pin is recorded")
    assert_contains(upstream, "kSdCsPin = 47", "official PaperS3 SD CS pin is recorded")
    assert_contains(upstream, "kSdSckPin = 39", "official PaperS3 SD SCK pin is recorded")
    assert_contains(upstream, "kSdMosiPin = 38", "official PaperS3 SD MOSI pin is recorded")
    assert_contains(upstream, "kSdMisoPin = 40", "official PaperS3 SD MISO pin is recorded")
    assert_contains(upstream, "kBatteryAdcPin = GPIO_NUM_3", "official PaperS3 battery ADC pin is recorded")
    assert_contains(upstream, "kChargeStatePin = GPIO_NUM_4", "official factory charge-state pin is recorded")
    assert_contains(upstream, "kUsbDetectPin = GPIO_NUM_5", "official PaperS3 USB detect pin is recorded")
    assert_contains(upstream, "kBuzzerPin = GPIO_NUM_21", "official PaperS3 buzzer pin is recorded")
    assert_contains(upstream, "kLegacyM5PaperTouchIntPin = GPIO_NUM_36", "GPIO36 is documented as legacy/non-PaperS3 power-key audit note")
    assert_contains(upstream, "kPowerOffPulsePin = GPIO_NUM_44", "PaperS3 PMIC power-off pulse pin is recorded")
    assert_contains(runtime_cpp, "configureOfficialPaperS3Gpios", "runtime initializes official PaperS3 battery/USB/charge/buzzer GPIOs")
    assert_contains(platformio, "epdiy=https://github.com/vroland/epdiy.git#d84d26ebebd780c4c9d4218d76fbe2727ee42b47", "PlatformIO documents the official PaperS3 EPDIY reference pin")
    assert_contains(display_h, "DisplayRequest", "v0.3 display queue uses a typed request struct")
    assert_contains(display_cpp, "cloneCanvas()", "v0.3 display queue snapshots canvas before physical push")
    assert_contains(display_cpp, "enqueue skipped: canvas snapshot allocation failed", "display service drops/retries instead of pushing mutable canvas if snapshot fails")
    assert_not_contains(display_cpp, "canvasToPush ? canvasToPush : canvas_", "display service must not fall back to mutable global canvas")
    assert_contains(display_cpp, "M5.Display.waitDisplay()", "v0.3 display task serializes physical EPD pushes")
    assert_contains(display_cpp, "return kQualityRefresh", "display service uses official-baseline quality refresh until real-device boot is stable")
    assert_contains(display_cpp, "g_inDisplayPush", "v0.3 display task exposes in-push guard")
    assert_contains(input_cpp, "g_inDisplayPush", "v0.3 input task suppresses events during display push")
    assert_contains(input_cpp, "M5.update();", "v0.3 input task owns M5.update polling")
    assert_contains(input_cpp, "side key is handled by the AXP2101 PMIC", "input task documents that PaperS3 side key is PMIC-backed")
    assert_contains(input_cpp, "wasClicked()/wasHold()", "input task uses PMIC PKEY events instead of GPIO-level polling")
    assert_contains(input_cpp, "pollPowerButton", "input task has the requested side-key graceful shutdown bridge")
    assert_contains(input_cpp, "M5.BtnPWR.wasClicked()", "side-key bridge uses M5Unified PMIC click events")
    assert_contains(input_cpp, "M5.BtnPWR.wasHold()", "side-key bridge uses M5Unified PMIC hold events")
    assert_not_contains(input_cpp, "M5.BtnPWR.isPressed()", "side-key bridge must not use GPIO-style isPressed for PMIC PKEY")
    assert_not_contains(input_cpp, "digitalRead(static_cast<int>(kPowerKeyPin))", "power input must not read unverified GPIO36 as PaperS3 side key")
    assert_contains(input_cpp, "lastPoint_ = currentPoint", "touch service caches last valid pressed coordinate")
    assert_contains(input_cpp, "lastRawPoint_ = rawPoint", "touch service preserves raw PaperS3 coordinates for diagnostics")
    assert_contains(input_cpp, "const TouchPoint releasePoint = lastPoint_", "touch service must not use release-time invalid coordinates for taps")
    assert_contains(input_cpp, "normalizeTouchPoint", "touch service normalizes/clamps raw PaperS3 coordinates before hit-test")
    assert_contains(input_cpp, "transformRawPaperS3Point", "touch service has explicit physical-to-portrait transform fallback")
    assert_contains(input_cpp, "gPaperS3TouchCoordMode", "touch transform uses a persistent coordinate mode instead of per-point guessing")
    assert_contains(input_cpp, "suppressUntilRelease", "touch service can suppress stale wake/transition touches until release")
    assert_contains(input_cpp, "gesture cancelled", "touch service cancels drag movement too large for tap but too small for swipe")
    assert_contains(input_cpp, "no coordinate-mode guessing", "touch transform does not infer or remap official raw coordinates")
    assert_contains(state_cpp, "xQueueReceive", "v0.3 state machine is queue-driven")
    assert_contains(state_cpp, "v0.3.7-rc", "BootComplete documents the v0.3.8 bootable baseline behavior")
    assert_contains(state_cpp, "state_ = SystemState::Reader;", "BootComplete enters normal reader home after the validated diagnostic RC")
    assert_contains(state_cpp, "OpenDiagnostics", "diagnostic page remains available from normal UI")
    assert_contains(state_cpp, "ReadPaper-style shutdown", "shutdown path follows ReadPaper-style save/wait/powerOff flow")
    assert_contains(state_cpp, "renderPowerOffReady", "shutdown path draws a final retained power-off page before power cut")
    assert_contains(upstream, "pulsePaperS3PowerOffPin", "PaperS3 shutdown has an explicit GPIO44 PWROFF pulse helper")
    assert_contains(state_cpp, "pulsePaperS3PowerOffPin", "shutdown path pulses PaperS3 GPIO44 after drawing the retained power-off page")
    assert_contains(runtime_cpp, "power-on reset after running session -> side-key shutdown path", "side-key reset while running is converted into shutdown")
    assert_contains(runtime_cpp, "cfg.clear_display = false", "M5Unified must not clear the EPD during startup")
    assert_contains(runtime_cpp, "ESP_RST_POWERON", "handleSideKeyResetShutdown must accept ESP_RST_POWERON for PMS150G power-cycle reset")
    assert_contains(runtime_cpp, "skipping boot page for side-key unlock resume", "side-key lock resume must not flash the normal boot page")
    assert_contains(runtime_cpp, "if (!isPaperS3SideKeyUnlockRequested())", "drawBoot must be gated for side-key lock resume")
    assert_contains(state_cpp, "esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL)", "shutdown fallback disables wake sources before deep sleep")
    assert_contains(state_cpp, "esp_deep_sleep_start", "shutdown path enters ESP32-S3 deep sleep")
    assert_not_contains(state_cpp, "esp_sleep_enable_ext0_wakeup(kPowerKeyPin", "shutdown fallback must not arm unverified GPIO36 wake source")
    assert_contains(legado_cpp, "LegadoService", "v0.3 Legado integration is isolated as a service")
    assert_contains(ui_cpp, "CjkTextRenderer", "v0.3 UI routes text through CJK renderer")
    assert_contains(ui_cpp, "renderDiagnostics", "official PaperS3 touch/display diagnostic page exists")
    assert_contains(ui_cpp, "\"诊断\", \"关于\"", "settings page exposes a visible diagnostics menu entry")
    assert_contains(ui_cpp, "scrollSettings", "settings pages support vertical swipe scrolling when rows exceed the screen")
    assert_contains(ui_cpp, "setClipRect(0, kContentY", "settings main page clips scrollable content below the tabs")
    assert_contains(ui_cpp, "readerSettingsScrollY_", "reader settings sub-page has independent scroll offset")
    assert_contains(state_cpp, "g_uiRenderer.scrollSettings(+1)", "settings page swipe-up scrolls down without numbered pages")
    assert_contains(state_cpp, "g_uiRenderer.scrollSettings(-1)", "settings page swipe-down scrolls up without numbered pages")
    assert_contains(ui_cpp, "return UiAction::OpenDiagnostics", "settings diagnostics menu routes to the diagnostic page")
    assert_contains(ui_cpp, "orientationFromAccel", "diagnostic page classifies BMI270 IMU orientation")
    assert_contains(ui_cpp, "M5.Imu.getImuData", "diagnostic page can show live IMU acceleration/gyro data")
    assert_contains(ui_cpp, "renderSystemLogs", "internal persistent system log page still exists for emergency diagnostics")
    assert_not_contains(ui_cpp, "{\"电源\", \"系统日志\", \"关于\"}", "normal settings page hides the manual system-log entry in this RC")
    assert_contains(system_log_cpp, "Preferences", "system logs persist across reboot in NVS")
    assert_contains(system_log_cpp, "kMaxLines", "system logs keep only recent records")
    assert_contains(state_cpp, "ClearSystemLogs", "system logs can be manually cleared")
    assert_contains(ui_cpp, "buildWrappedSystemLogRows", "system log page wraps long records instead of truncating them")
    assert_contains(state_cpp, "scrollSystemLogs(+1)", "system log page supports swipe-up pagination")
    assert_contains(state_cpp, "scrollSystemLogs(-1)", "system log page supports swipe-down pagination")
    assert_contains(runtime_cpp, "boot reset=", "boot reset reason is recorded for on-device diagnostics")
    assert_contains(runtime_cpp, "boot fw=", "boot firmware and memory snapshot are recorded")
    assert_contains(ui_cpp, "ESP.getFreeHeap", "system log page shows live heap state")
    assert_contains(ui_cpp, "readOfficialBatteryVoltage", "system log page shows live power state")
    assert_contains(ui_cpp, "SD.cardType", "system log page shows live SD state")
    assert_contains(ui_cpp, "返回", "diagnostic page has a visible return button")
    assert_contains(state_cpp, "diagAction >= UiAction::TabReader", "diagnostic page return button exits back to settings")
    assert_contains(ui_cpp, "renderShutdownConfirm", "settings power entry shows a shutdown confirmation page first")
    assert_contains(ui_cpp, "确认关机", "shutdown confirmation has an explicit confirm button")
    assert_contains(ui_cpp, "侧边键单击也会进入关机流程", "shutdown confirmation documents the side-key shutdown path")
    assert_contains(ui_cpp, "renderShutdown", "official side power-key shutdown page exists")
    assert_contains(ui_cpp, "renderPowerOffReady", "final retained shutdown page exists")
    assert_contains(ui_cpp, "Vink 已关机", "final retained shutdown page confirms completed shutdown path")
    assert_contains(ui_cpp, "后续可替换为 SD 卡关机图片", "final retained shutdown page keeps room for future SD-card image replacement")
    assert_contains(ui_cpp, "单击侧边键也会显示此关机页", "shutdown UI documents official PaperS3 side-key behavior")
    assert_contains(ui_cpp, "RequestShutdown", "settings page has touch shutdown fallback when physical side key is unreadable")
    assert_contains(ui_cpp, "ConfirmShutdown", "shutdown requires a second explicit confirmation action")
    assert_contains(state_cpp, "SystemState::ShutdownConfirm", "state machine has a shutdown confirmation state")
    assert_contains(ui_cpp, "raw:", "diagnostic page shows raw GT911/M5Unified coordinates")
    assert_contains(ui_cpp, "norm:", "diagnostic page shows normalized Vink coordinates")
    assert_contains(ui_cpp, "USB:%s CHG:%s BAT:%.2fV", "diagnostic page shows official power/USB/battery signals")
    assert_contains(ui_cpp, "drawSettingsRow", "settings rows align label, value, and arrow explicitly")
    assert_contains(ui_cpp, "页边距", "settings tab exposes page margin")
    assert_contains(ui_cpp, "行间距", "settings tab exposes line spacing")
    assert_contains(ui_cpp, "排版优化", "settings tab exposes reader layout optimization")
    assert_contains(ui_cpp, "g_readerText.antiAliasLabel()", "settings tab exposes anti-alias state")
    assert_contains(ui_cpp, "g_displayService.readerRefreshStrategyLabel()", "settings tab exposes refresh strategy state")
    assert_contains(ui_cpp, "g_readerText.pageTurnEffectLabel()", "settings tab exposes page-turn effect state")
    assert_contains(ui_cpp, "UiAction::CycleReaderPageMargin", "settings tab can cycle page margin")
    assert_contains(ui_cpp, "UiAction::CycleReaderLineSpacing", "settings tab can cycle line spacing")
    assert_contains(ui_cpp, "UiAction::CycleReaderLayoutPreset", "settings tab can cycle layout optimization")
    assert_contains(ui_cpp, "UiAction::ToggleReaderAntiAlias", "settings tab can toggle anti-alias")
    assert_contains(ui_cpp, "UiAction::CycleReaderRefreshStrategy", "settings tab can cycle refresh strategy")
    assert_contains(ui_cpp, "UiAction::ToggleReaderPageTurnEffect", "settings tab can toggle page-turn effect")
    assert_contains(ui_cpp, "kRowH / 2", "settings row label/value/arrow share one computed centerline")
    assert_contains(cjk_cpp, "lineTopForBox", "boxed UI text uses glyph visual bounds for vertical centering")
    assert_contains(ui_cpp, "lineTopForBox", "buttons/tabs/cards/settings rows use the shared UI vertical-centering helper")
    assert_contains(ui_cpp, "同一水平线", "settings page documents row alignment intent")
    assert_contains(ui_cpp, "formatStatusTime", "status bar shows system time at the left")
    assert_contains(ui_cpp, "formatBatteryPercent", "status bar shows battery percentage at the right")
    assert_contains(ui_cpp, "readOfficialBatteryVoltage", "diagnostic/status path can read factory-style battery voltage")
    assert_contains(ui_cpp, "isOfficialUsbConnected", "diagnostic/status path can read official USB detect")
    assert_contains(ui_cpp, "isOfficialChargeStateActive", "diagnostic page can read official charge-state pin")
    assert_contains(ui_cpp, "Action buttons below the top card", "reader home action buttons are explicitly documented")
    assert_contains(ui_cpp, "lastReaderHomeHasBook_", "reader home hit-test only exposes TOC when the visible TOC button exists")
    assert_not_contains(ui_cpp, "繁简", "v0.3 UI must not show Traditional/Simplified toggle wording")
    assert_contains(ui_font_cpp, "g_vink_ui_font24_available = true", "compiled 24px SC UI font is available without SPIFFS")
    assert_contains(cjk_cpp, "PROGMEM Bold 24px UI font loaded", "v0.3 UI uses compiled Simplified Chinese SC font as the primary renderer")
    assert_contains(cjk_cpp, "SPIFFS Bold 16px small font loaded", "CJK renderer loads optional SPIFFS small font for secondary text")
    assert_contains(cjk_cpp, "PROGMEM Bold 24px UI font loaded", "compiled UI font is the normal UI font dependency")
    assert_contains(cjk_cpp, "PROGMEM UI font unavailable", "CJK renderer logs if the compiled UI font is unavailable")
    assert_contains(cjk_cpp, "Missing glyphs should be visible", "CJK renderer must not silently skip missing UI glyphs")
    assert_contains(cjk_cpp, "Layout follows the compiled UI font", "CJK layout metrics must use the compiled UI font")
    assert_not_contains(cjk_cpp, "ReadPaperUiFont", "redundant ReadPaper UI subset must not be linked into the shell UI")
    assert_contains(cjk_cpp, "one common baseline", "UI gray font rendering must not baseline-stagger Latin letters")
    assert_contains(cjk_cpp, "g/p/y", "UI baseline must preserve Latin descenders")
    assert_contains(reader_cpp, "compiled full Wenkai font", "reader body stays on the embedded Wenkai path")
    assert_contains(reader_cpp, "Wenkai32 PROGMEM", "reader Wenkai glyph rendering is the primary body-font path")
    assert_contains(read("src/vink3/text/WenkaiFullFont.h"), "g_wenkai_font32", "compiled reader body font is linked")
    assert_contains(read("src/vink3/text/WenkaiFullFont.cpp"), "GB2312完整汉字", "reader body font covers full GB2312 instead of a sparse contiguous subset")
    assert_gray_font_covers(
        "tools/fonts/wenkai_32.fnt",
        "的第章测试一是在不了有和人这中大为上个国我以要他时来用们生到作地于出就分对成会可主发年动同工也能下过子说产种面而方后多定行学法所民得经十三之进着等部度家电力里如水化高自二理起小物现实加量都两体制机当使点从业本去把性好应开它合还因由其些然前外天政四日那社义事平形相全表间样与关各重新线内数正心反你明看原又么利比或但质气",
        "reader body Wenkai font covers common Chinese text characters",
    )
    assert_contains(reader_cpp, "fontSize()) - glyph.bearingY", "reader Wenkai body font aligns glyphs to a shared baseline")
    reader_aa_policy_h = read("src/vink3/reader/ReaderAaPolicy.h")
    assert_contains(reader_aa_policy_h, "Vink nibble semantics", "reader AA policy documents Vink 4bpp direction")
    assert_contains(reader_aa_policy_h, "rgb565ForNibble", "reader body text uses shared AA palette mapping")
    assert_contains(reader_cpp, "never let a missing body glyph silently disappear", "reader body missing glyphs render visible tofu instead of blank pages")
    assert_contains(reader_cpp, "v0.4.15 shipped with a sparse Wenkai font", "smoke test documents the v0.4.15 blank-body root cause")
    assert_contains(reader_cpp, "ReaderTextRenderer", "v0.3 has a separate reader body renderer")
    assert_contains(reader_cpp, "beginWenkai32Font", "reader body renderer uses full Wenkai PROGMEM font")
    assert_contains(reader_book_cpp, "ReaderBookService", "v0.3 has reader book service for opening TXT books")
    assert_contains(reader_book_cpp, "saveCurrentProgress", "power shutdown can save current reader progress before power-off")
    assert_contains(reader_book_cpp, "SD is initialized lazily", "reader book service does not block boot on SD initialization")
    assert_contains(reader_book_cpp, "scanBooks", "reader book service scans /books into a library list")
    assert_contains(reader_book_h, "kMaxTocEntries = 2000", "reader TOC capacity has headroom above 1000+ chapter novels")
    assert_contains(reader_book_h, "kMaxBooks = 160", "reader library capacity has practical headroom")
    assert_contains(reader_book_h, "currentLibraryDir_[160] = \"/books\"", "reader file browser starts at the bookshelf /books directory")
    assert_contains(reader_book_cpp, "normalizeChildPath", "reader file browser normalizes SD child paths under /books")
    assert_contains(reader_book_cpp, "dir + raw", "reader handles SD child names like /name returned inside /books")
    assert_contains(reader_book_cpp, "s.endsWith(\".epub\")", "reader bookshelf recognizes epub names without pretending parsing is ready")
    assert_not_contains(reader_book_cpp, "clearPageCache", "obsolete page-cache cleanup action removed")
    assert_contains(reader_book_cpp, "重新生成目录", "book entry page exposes TOC rebuild")
    assert_contains(reader_book_cpp, "SD.remove(path)", "book cache maintenance removes stale sidecar files")
    assert_contains(reader_book_cpp, "library browser", "reader book service uses file-browser mode")
    assert_contains(reader_book_cpp, "Subdirectories are", "reader library enters subdirectories instead of flattening everything")
    assert_contains(reader_book_cpp, "setDisplayNameFromPath", "reader library strips file extensions from visible book names")
    assert_contains(reader_book_cpp, "□  %s", "reader library renders a minimal directory icon")
    assert_contains(reader_book_cpp, "▤  %s", "reader library renders a minimal book icon before book files")
    assert_not_contains(reader_book_cpp, "▤  [%s]", "reader library must not show cache flags in book rows")
    assert_contains(reader_book_cpp, "kConfigRoot = \"/config\"", "reader creates an SD-side config root for future settings")
    assert_contains(reader_book_cpp, "kVinkCacheRoot = \"/config/vink-cache\"", "reader cache files live under config/vink-cache")
    assert_contains(reader_book_cpp, "kSidecarRoot = \"/config/vink-cache/books\"", "reader metadata/cache files are stored outside the /books library")
    assert_contains(reader_book_cpp, "hashBookPath", "reader book sidecar paths use deterministic hash names instead of mirroring long book paths")
    assert_contains(reader_book_cpp, "FNV-1a 64-bit", "reader hash naming uses a cheap deterministic path hash")
    assert_contains(reader_book_cpp, "%s/%c%c/%s%s", "reader hash cache paths are sharded by first two hex chars")
    assert_contains(reader_book_cpp, "getSidecarPathForBook", "reader book sidecar paths are derived from the source book path")
    assert_contains(reader_book_cpp, "getLegacySidecarPathForBook", "reader can still read/delete legacy beside-book sidecars after migration")
    assert_contains(reader_book_cpp, "kLastBookRecordPath", "reader tab keeps a separate last-read book record")
    assert_not_contains(reader_book_cpp, "kLastBookRecordPath = \"/books/.vink-last-book\"", "last-read record must not pollute /books")
    assert_contains(reader_book_cpp, "saveLastBookPath", "opening/reading a book updates the last-read record")
    assert_contains(state_cpp, "g_readerBook.renderReaderHome", "Reader tab home is rendered from reader service state, not a static shell")
    assert_contains(reader_book_cpp, "readProgressForBook", "Reader tab can show last book progress without entering body reading")
    assert_contains(reader_book_cpp, "sortBooks", "reader library order is stable across SD directory iteration")
    assert_contains(reader_book_cpp, "detectBookFlags", "reader book service shows library progress/cache flags")
    assert_contains(reader_book_cpp, "formatBytes", "reader book entry shows source file size")
    assert_contains(reader_book_cpp, "renderBookLoadingPage", "reader shows blocking status while first-opening large books")
    assert_contains(reader_book_cpp, "renderChapterLoadingPage", "reader shows blocking status while first-paginating chapters")
    assert_contains(reader_book_cpp, "正在分析目录", "reader explains first-open TOC analysis wait")
    assert_contains(reader_book_cpp, "正在分页", "reader explains first-open chapter pagination wait")
    assert_not_contains(reader_book_cpp, "文件浏览器 %s · %d 项 · 读/目/页", "library summary must stay minimal on e-paper")
    assert_contains(reader_book_cpp, "handleLibraryTap", "reader book service opens selected library entries")
    assert_contains(reader_book_cpp, "left third=prev", "reader page uses large official-friendly 3-zone tap navigation")
    assert_contains(reader_cpp, "renderListPage", "reader text renderer can draw list rows aligned with tap zones")
    assert_contains(reader_cpp, "drawShellTabs", "reader management pages show the same four-tab shell")
    assert_contains(ui_cpp, "Thick underline bar", "reader tabs use underline bar indicator for selected tab")
    assert_contains(reader_book_h, "kListFirstRowY = 222", "reader list touch rows start below the 64px browser summary row")
    assert_contains(reader_book_h, "kShelfBrowserEntryH = 64", "shelf browser entry matches the settings row height")
    assert_contains(reader_book_h, "kShelfCardH = 206", "shelf book cards are slightly compressed to preserve the 3x3 layout")
    assert_contains(ui_cpp, "kLabelBoxH = kRowH", "reader home recent-reading label uses the shared cell height")
    assert_contains(ui_cpp, "Progress text: no box", "reader home progress text is unboxed and visually centered")
    assert_contains(ui_cpp, "renderUiListPage", "UI list renderer exists for tab/list pages")
    assert_contains(reader_book_cpp, "renderUiListPage(SystemState::Library", "library tab uses UI font renderer, not reading font")
    assert_contains(reader_book_cpp, "renderUiListPage(SystemState::Reader", "TOC/navigation list uses UI font renderer, not reading font")
    assert_contains(reader_cpp, "renderActionPage", "reader text renderer can still draw body-font action pages if needed")
    assert_contains(ui_cpp, "renderUiActionPage", "UI action renderer exists for pre-reading pages")
    assert_contains(reader_book_cpp, "renderUiActionPage(SystemState::Reader, \"书籍入口\"", "book entry uses UI font action rendering before reading starts")
    assert_not_contains(reader_book_cpp, "g_readerText.renderActionPage(\"书籍入口\"", "book entry must not use reading font before reading starts")
    assert_contains(reader_book_cpp, "kEntryContinueY", "book entry tap zones share fixed button geometry")
    assert_contains(reader_book_cpp, "renderBookEntryPage", "reader book service shows a book entry action page")
    assert_contains(reader_book_cpp, "renderReaderMenuPage", "reading center tap opens a formal reader menu")
    assert_contains(reader_book_cpp, "renderReaderMenuOverlay", "reader menu uses UI font renderer via overlay, not body reading font")
    assert_not_contains(reader_book_cpp, "g_readerText.renderActionPage(\"阅读菜单\"", "reader menu must not use body reading font")
    assert_contains(ui_cpp, "kCardH = 596", "reader menu has roomier 64px-cell layout instead of cramped overlay")
    assert_contains(ui_cpp, "返回首页", "reader menu return button clearly means Reader tab home")
    assert_contains(ui_cpp, "body pages use SystemState::ReaderMenu internally and have no visible tab", "body reading pages must not accept invisible tab hits")
    assert_contains(reader_book_cpp, "No zone covers the TAB bar", "body reading tap zones must not include the hidden tab strip")
    assert_contains(reader_book_cpp, "Inside the visible menu card but outside concrete controls", "reader menu chrome consumes taps instead of falling through to the body/tab layer")
    assert_contains(reader_book_cpp, "lastTapBackHome_ = true", "reader menu return button goes back to Reader tab home, not body text")
    assert_contains(reader_book_cpp, "cycleRefreshStrategy", "reader menu can trigger refresh strategy setting")
    assert_contains(reader_book_cpp, "toggleAntiAlias", "reader menu can trigger anti-alias setting")
    assert_contains(reader_book_cpp, "cycleLayoutPreset", "reader menu can trigger book layout optimization setting")
    assert_contains(reader_book_cpp, "cycleRefreshStrategy", "reader menu can cycle display refresh strategy")
    assert_contains(reader_book_cpp, "cycleLayoutPreset", "reader menu can cycle layout presets")
    assert_contains(ui_cpp, "抗锯齿", "reader menu exposes anti-alias setting")
    assert_contains(ui_cpp, "全刷频率", "reader menu exposes decoupled full-clean frequency setting")
    assert_contains(webui_cpp, "低=20页，中=10页，高=5页", "WebUI exposes the same low/medium/high full-clean frequency as device settings")
    assert_contains(webui_cpp, "翻页档位", "WebUI exposes page-turn profile tuning without reflashing")
    assert_contains(webui_cpp, "pageTurnProfile", "WebUI saves page-turn profile tuning")
    assert_not_contains(webui_cpp, "中间清影页数", "WebUI must not expose the confusing middle cleanup interval")
    assert_not_contains(webui_cpp, "全屏清影页数", "WebUI must not expose the confusing full cleanup interval")
    assert_not_contains(webui_cpp, "readerMiddleRefreshEvery", "WebUI must not keep legacy middle cleanup form/API fields")
    assert_not_contains(webui_cpp, "readerFullRefreshEvery", "WebUI must not keep legacy full cleanup form/API fields")
    assert_contains(ui_cpp, "下划线", "reader menu exposes underline setting")
    assert_contains(ui_cpp, "排版优化", "reader menu exposes layout optimization setting")
    assert_contains(ui_cpp, "翻页动画", "reader menu exposes page-turn animation setting")
    assert_contains(ui_cpp, "翻页档位", "device settings page exposes page-turn profile setting")
    assert_contains(ui_cpp, "CycleReaderPageTurnProfile", "device settings page can cycle page-turn profile")
    assert_contains(ui_cpp, "页边距", "reader menu exposes page-margin setting")
    assert_contains(reader_book_cpp, "toggleUnderline", "reader menu can toggle underline")
    assert_contains(reader_book_cpp, "togglePageTurnEffect", "reader menu can toggle page-turn effect")
    assert_contains(reader_book_h, "kEntryPageTurnY", "reader menu has a touch row for the sixth action")
    assert_contains(reader_book_cpp, "pageCount_ <= 0 && currentTocIndex_ >= 0) return continueReading()", "reader menu continue rebuilds after layout invalidation")
    assert_contains(reader_cpp, "render_opt1", "reader text renderer models Vink packed render settings")
    assert_contains(reader_cpp, "setAntiAlias", "anti-alias follows the Vink soft-edge toggle model")
    assert_contains(reader_cpp, "indentFirstLine", "reader layout optimization supports first-line indent")
    assert_contains(reader_cpp, "compactBlankLines", "reader layout optimization supports blank-line compaction")
    assert_contains(reader_cpp, "dynamicLineHeight", "reader layout optimization supports dynamic line height")
    assert_contains(reader_cpp, "breakLineOpt", "reader layout optimization supports conservative punctuation line breaking")
    assert_contains(reader_cpp, "startsAtParagraph", "reader layout distinguishes real paragraph starts from page continuations")
    assert_contains(reader_cpp, "firstLineIndentPx", "reader layout uses bounded pixel indent instead of hardcoded two body-font widths")
    assert_contains(reader_book_cpp, "currentRenderOptionsForOffset(offset, start)", "pagination uses chapter-offset-aware paragraph starts")
    assert_contains(reader_book_cpp, "currentRenderOptionsForOffset(start, chapterContentStart(currentTocIndex_))", "page rendering uses chapter-offset-aware paragraph starts")
    assert_contains(reader_book_cpp, "fileOffsetStartsParagraph", "reader inspects source text before indenting page starts")
    assert_contains(reader_book_h, "kTocEntriesPerPage", "TOC uses paginated display")
    assert_contains(display_cpp, "ReaderRefreshStrategy::Speed", "display service preserves legacy low full-clean frequency value")
    assert_contains(display_cpp, "fullEvery = 20", "low full-clean frequency means every 20 pages")
    assert_contains(display_cpp, "ReaderRefreshStrategy::Balanced", "display service has a medium full-clean frequency")
    assert_contains(display_cpp, "fullEvery = 10", "medium full-clean frequency means every 10 pages")
    assert_contains(display_cpp, "ReaderRefreshStrategy::Clear", "display service has a high full-clean frequency")
    assert_contains(display_cpp, "fullEvery = 5", "high full-clean frequency means every 5 pages")
    assert_not_contains(display_cpp, "readerFullRefreshEvery_ > 0", "legacy numeric full cleanup override must not supersede low/medium/high frequency")
    assert_not_contains(display_cpp, "readerMiddleRefreshEvery_ > 0", "legacy numeric middle cleanup override must not supersede low/medium/high frequency")
    assert_contains(display_cpp, "request.effect != DisplayEffect::None", "page-turn effect changes the actual display refresh path")
    assert_contains(display_cpp, "Panel_EPD, not", "page-turn animation documents the actual PaperS3 panel driver")
    assert_contains(display_cpp, "M5DisplayStripSweep(canvas, effect, mode)", "page-turn animation uses the current portrait strip path")
    assert_not_contains(display_cpp, "static_cast<lgfx::Panel_EPDiy*>", "page-turn animation must not reinterpret PaperS3 Panel_EPD as Panel_EPDiy")
    assert_not_contains(display_h, "EpdiyHighlevelState", "display service must not expose fake epdiy highlevel state on PaperS3")
    assert_contains(display_cpp, "displayScroll", "page-turn animation enters the patched Panel_EPD scroll scheduler")
    assert_contains(display_cpp, "pageTurnScrollStripWidth", "page-turn line sweep uses an explicit narrow strip width")
    assert_contains(display_cpp, "ReaderPageTurnProfile::Clean", "page-turn profile has a clean 64px runtime-selectable strip width")
    assert_contains(display_cpp, "ReaderPageTurnProfile::Balanced", "page-turn profile has a balanced 108px runtime-selectable strip width")
    assert_contains(display_cpp, "ReaderPageTurnProfile::Fast", "page-turn profile has a fast 180px runtime-selectable strip width")
    assert_contains(display_cpp, "turnprof", "page-turn profile persists in display NVS to reduce reflashes during tuning")
    assert_contains(display_cpp, "the old high-speed DU-like mode is intentionally not used", "page-turn animation no longer exposes the unusable high-speed DU mode")
    assert_contains(display_cpp, "Stable baseline", "normal page turns use the stable standard-LUT baseline")
    assert_contains(display_cpp, "pageTurnScrollMode", "page-turn cleanup is routed through the strip scheduler")
    assert_contains(display_cpp, "scheduledMode == kQualityRefresh", "page-turn animation honors full-clean intervals inside the scroll scheduler")
    assert_contains(display_cpp, "decoupled\n    // from page-turn animation speed", "full-clean frequency is decoupled from page-turn animation")
    assert_contains(display_cpp, "ghost=disabled-standard-lut", "ghosting compensation UI must not be claimed active on the standard-LUT baseline")
    assert_not_contains(display_cpp, "settle refresh", "page-turn cleanup must not be a post-animation settle flash")
    assert_contains(panel_epd_h, "displayScroll", "patched Panel_EPD exposes a scroll scheduler entrypoint")
    assert_contains(panel_epd_h, "scroll_waveform", "Panel_EPD update data can request the private page-turn waveform")
    assert_contains(panel_epd_cpp, "lut_page_turn", "Panel_EPD has a private short page-turn LUT")
    assert_contains(panel_epd_cpp, "preserves anti-aliased", "page-turn LUT preserves anti-aliased gray edges instead of overdriving them black")
    assert_contains(panel_epd_cpp, "kPrivatePageTurnMode", "Panel_EPD indexes the private page-turn LUT without exposing a public epd_mode_t")
    assert_contains(panel_epd_cpp, "flg_page_turn", "Panel_EPD scroll worker can use the private page-turn waveform")
    assert_contains(panel_epd_cpp, "Keep these coordinates in logical display space", "Panel_EPD scroll receives logical portrait strips")
    assert_contains(panel_epd_cpp, "_display_busy = false", "Panel_EPD clears busy state if an update queue send fails")
    assert_contains(panel_epd_cpp, "4-pixel aligned", "Panel_EPD scroll widths are documented as 4-pixel aligned")
    assert_contains(panel_epd_cpp, "(strip_width + 3u) & ~3u", "Panel_EPD displayScroll rounds public strip widths up to 4-pixel alignment")
    assert_contains(panel_epd_cpp, "new_data.scroll_strip_width & ~3u", "Panel_EPD scroll worker preserves 4-pixel strip alignment")
    assert_contains(panel_epd_cpp, "((xe + me->_cfg.offset_x) | 3u) - strip.x + 1", "Panel_EPD scroll strips keep packed-byte loops 4-pixel aligned")
    assert_contains(panel_epd_cpp, "run_cycle", "Panel_EPD scroll advances strips inside the scan-cycle worker")
    assert_contains(m5gfx_patch_tool, "m5gfx-panel-epd-scroll.patch", "M5GFX Panel_EPD patch is reproducible from source")
    assert_contains(display_cpp, "M5.Display.setAutoDisplay(false)", "page-turn animation stages the full next-page framebuffer before scrolling")
    assert_contains(display_cpp, "waveform/scan-cycle layer", "page-turn animation moved below public clip/push timing")
    assert_not_contains(display_cpp, "kWavefrontStageDelayMs", "page-turn animation must not rely on public delay-tuned strip pushes")
    assert_contains(state_cpp, "shouldQualityRefreshTabSwitch", "tab switching balances fast GL16 refresh with periodic quality cleanup")
    assert_contains(state_cpp, "kQualityEveryTabSwitches = 8", "tab switching does not quality-refresh every tap")
    assert_contains(state_cpp, "g_readerText.pageTurnEffectEnabled()", "state machine routes page turns through the page-turn effect toggle")
    assert_contains(state_cpp, "cycleReaderPageTurnProfile", "state machine cycles page-turn profile separately from the page-turn animation toggle")
    assert_contains(state_cpp, "Lock-screen exit is a state restore, not a page turn", "lock resume must not use page-turn strip animation")
    assert_contains(state_cpp, "(void)g_readerBook.consumeReadingPageRendered();\n        g_displayService.enqueueFull(false, 100);", "lock resume consumes stale reader-page marker and refreshes directly")
    assert_contains(state_cpp, "next page  -> DisplayEffect::VerticalShutter", "page-turn direction contract documents next-page effect")
    assert_contains(state_cpp, "prev page  -> DisplayEffect::HorizontalShutter", "page-turn direction contract documents previous-page effect")
    assert_contains(state_cpp, "? DisplayEffect::VerticalShutter", "tap-driven next-page animation uses the corrected wipe direction")
    assert_contains(state_cpp, "enqueueReaderAwareRefresh(DisplayEffect::VerticalShutter);", "swipe/up-driven next-page animation uses the corrected wipe direction")
    assert_contains(state_cpp, "enqueueReaderAwareRefresh(DisplayEffect::HorizontalShutter);", "previous-page animation uses the corrected wipe direction")
    assert_contains(reader_book_cpp, "ChapterDetector", "reader book service detects TXT table of contents")
    assert_contains(reader_book_cpp, "no TOC found, using whole-book fallback", "reader falls back to whole-book reading when TOC detection fails")
    assert_contains(reader_book_cpp, "the \"第\" in \"第一章 你好\"", "chapter pagination starts at the TOC title byte offset")
    assert_not_contains(reader_book_cpp, "Skip the chapter title line itself", "reader must not skip chapter title line before pagination")
    assert_contains(reader_book_cpp, "TextCodec::convertToUTF8", "reader book service converts GBK TXT before TOC detection")
    assert_contains(reader_book_cpp, ".vink-toc", "reader book service stores TOC cache in the metadata directory")
    assert_contains(reader_book_cpp, "nextTocPage", "reader book service supports TOC paging")
    assert_contains(reader_book_cpp, "buildChapterPages", "reader book service builds chapter page tables")
    assert_contains(reader_book_cpp, ".vink-progress", "reader book service stores progress in the metadata directory")
    assert_contains(reader_book_cpp, "sampleFileFingerprint", "reader cache identity samples only the book head/tail instead of hashing the whole file")
    assert_contains(reader_book_cpp, "kFingerprintSampleBytes = 512", "reader fingerprint uses a small bounded head/tail sample")
    assert_contains(reader_book_cpp, "VCT5", "reader TOC cache validates file size, fingerprint, and schema")
    assert_contains(reader_book_cpp, "cachedSize != activeTextSize()", "reader TOC cache is invalidated when a book file is replaced at the same path")
    assert_contains(reader_book_cpp, "cachedFingerprint != activeTextFingerprint_", "reader caches are invalidated when sampled book content changes")
    assert_contains(reader_book_cpp, "VPR3", "reader progress cache is schema-versioned and fingerprinted")
    assert_contains(reader_book_cpp, ".vink-pages", "reader can still identify legacy/generated page cache files for cleanup")
    assert_not_contains(reader_book_cpp, "clearPageCache", "stale clearPageCache method removed")
    assert_contains(reader_book_cpp, "rebuildTocCache", "reader keeps manual TOC index rebuild action")
    assert_contains(reader_book_cpp, "removeSidecarForCurrentBook(\".vink-toc\")", "reader TOC rebuild clears the old TOC index")
    assert_not_contains(reader_book_cpp, ".vink-page-%04d", "reader must not create one pagination cache file per chapter")
    assert_contains(reader_book_cpp, "Persistent page caches are no longer written", "reader no longer writes persistent page caches in streaming mode")
    assert_contains(reader_book_cpp, "buildPreviousChapterTailPages", "reader can locate previous chapter tail on demand without persistent page cache")
    assert_contains(reader_book_cpp, "previous chapter tail ready", "reader logs on-demand previous-chapter tail paging")
    assert_contains(reader_book_cpp, "delay(1)", "reader yields while locating long previous-chapter tails")
    assert_contains(reader_book_cpp, "No synchronous/eager pre-pagination", "reader avoids synchronous next-chapter preheat while reading")
    assert_contains(reader_book_cpp, "new (&toc_[i]) ChapterDetectResult()", "TOC buffer constructs String entries instead of using raw calloc memory")
    assert_contains(state_cpp, "\"vink3-state\", 16384", "state task has enough stack for reader page buffers")
    assert_contains(chapter_cpp, "file.read(reinterpret_cast<uint8_t*>(chunk)", "chapter detector reads lines in buffered chunks instead of byte-by-byte SD reads")
    assert_contains(chapter_cpp, "matchLine", "chapter detector matches candidate chapter lines after buffered reads")
    assert_contains(chapter_cpp, "lineTooLong", "chapter detector discards long non-heading lines in buffered chunks")
    assert_contains(chapter_cpp, "(linesScanned & 0xFF) == 0", "chapter detector yields periodically during large-book scans")
    assert_contains(reader_book_cpp, "v0.4.5-on-0.4.2 intentionally ignores persistent .vink-pages caches", "reader ignores stale persistent page caches in streaming mode")
    assert_contains(reader_book_cpp, "currentTocIndex_ + 1", "reader book service advances across chapter boundaries")
    assert_not_contains(reader_book_cpp, "*为当前章节", "TOC must not use star text to mark the current chapter")
    assert_not_contains(reader_book_cpp, "%c%03d  %s", "TOC rows must not prefix original chapter titles with star/index")
    assert_not_contains(reader_book_cpp, "%03d %s", "reading headers must use original chapter titles without numeric prefix")
    assert_contains(ui_cpp, "readerList = active == SystemState::Reader", "TOC page uses clean plain rows instead of boxed rows")
    assert_contains(ui_cpp, "Keep active-row rhythm similar to selected tab", "TOC marks current chapter with a clean active-row accent")
    assert_not_contains(ui_cpp, "drawRoundRect(x + 3", "TOC current marker must not draw a dirty extra frame")
    assert_contains(reader_cpp, "measurePageBytes", "reader text renderer exposes page-fit measurement")
    assert_contains(reader_cpp, "drawReadingChrome", "reader text pages render a compact UI-font header/footer chrome")
    assert_contains(reader_cpp, "two-level target palette", "reader text page chrome uses the same ink color as the body for simpler page-turn waveform targets")
    assert_contains(reader_cpp, "g_cjkText.drawRight", "reader chrome uses the UI font renderer for right-aligned time/percent text")
    assert_contains(reader_cpp, "progressPermille", "reader text page footer uses whole-book progress instead of current page number")
    assert_contains(reader_cpp, "nextLineEnd", "reader measurement and rendering share the same line wrapping path")
    assert_contains(reader_cpp, "measured page consumes more bytes than the renderer draws", "reader guards against last-line missing characters")
    assert_contains(reader_cpp, "reader blank body", "reader body renderer logs suspicious all-blank layout cases")
    assert_contains(reader_book_cpp, "A complete Chinese character/punctuation at the page boundary must stay", "UTF-8 page-tail trimming keeps complete final glyphs")
    assert_contains(reader_cpp, "kReaderFooterReserve", "reader body keeps a footer-safe bottom bound after adding chrome")
    assert_contains(reader_cpp, "migrate old defaults in RAM only", "reader layout migration does not write NVS during boot")
    assert_not_contains(reader_cpp, "第 %u 页", "reader text page must not show current page number in the footer")
    assert_contains(reader_cpp, "if (!ready()) loadDefaultFont();", "reader renderer must load a default font on fresh NVS boot")
    assert_contains(reader_cpp, "applyReaderFontSize(fontSizeSetting_, false)", "reader renderer must not write NVS while applying boot-time font settings")
    assert_contains(reader_book_cpp, "openTocEntry", "reader book service can open a TOC entry preview")
    assert_contains(state_cpp, "SystemState::ReaderMenu", "state machine routes reader menu interactions")
    assert_contains(state_cpp, "case UiAction::CycleReaderRefreshStrategy", "state machine applies refresh strategy from settings")
    assert_contains(state_cpp, "case UiAction::ToggleReaderAntiAlias", "state machine applies anti-alias from settings")
    assert_contains(state_cpp, "case UiAction::CycleReaderPageMargin", "state machine applies page margin from settings")
    assert_contains(state_cpp, "case UiAction::CycleReaderLineSpacing", "state machine applies line spacing from settings")
    assert_contains(state_cpp, "case UiAction::CycleReaderLayoutPreset", "state machine applies layout optimization from settings")
    assert_contains(state_cpp, "case UiAction::ToggleReaderPageTurnEffect", "state machine applies page-turn effect from settings")
    assert_contains(state_cpp, "renderLibraryPage", "state machine routes Library tab through reader book list")
    assert_contains(state_cpp, "g_readerBook.handleShelfTap", "state machine routes library/shelf taps through the shelf tap handler")
    assert_contains(state_cpp, "lastLibraryTapOpenedBook", "state machine stays in Library when a directory row is tapped")
    assert_contains(state_cpp, "state_ == SystemState::Library && g_readerBook.handleShelfTap", "state machine only reacts after a valid library row tap")
    assert_contains(full_font_h, "g_readpaper_full_font_data", "Vink full reader font is compiled as PROGMEM")
    assert_contains(partitions_csv, "0xC00000", "Vink partition table has a large single app slot for full reader font")
    assert_not_contains(partitions_csv, "app1", "v0.3 partition table drops dual OTA app1 to fit full ReadPaper font")
    assert_contains(gbk_table_h, "gbkToUnicode", "Vink includes full GBK Unicode lookup")
    assert_contains(codec_cpp, "vink3::gbkToUnicode", "TextCodec uses full GBK lookup before legacy table")
    assert_contains(chapter_cpp, "四", "chapter detector parses full Chinese numerals")
    assert_contains(chapter_cpp, "lastChapterNumber", "chapter detector suppresses duplicate/outlier chapter headings")
    assert_contains(chapter_cpp, "lastChapterNumber", "chapter detector documents web TXT duplicate heading cleanup")
    assert_contains(chapter_cpp, "0xE3", "chapter detector trims ideographic leading spaces")
    assert_contains(toc_tool, "last_number", "host TXT TOC detector suppresses duplicate/outlier chapter headings")
    assert_contains(toc_tool, "detect_toc", "host TXT TOC detector exists for large novel validation")
    assert_contains(ui_cpp, "Vink 加载中", "boot page uses the normal Vink UI font instead of diagnostic probes")
    assert_contains(ui_cpp, "Keep boot plain and centered", "boot page intentionally stays plain and centered")
    assert_not_contains(runtime_cpp, "drawOfficialBootProbe", "runtime must not show the old direct M5.Display boot diagnostic page")
    assert_not_contains(ui_cpp, "VINK CANVAS PROBE", "boot page must not show the old canvas diagnostic probe")
    assert_not_contains(ui_cpp, "drawString(\"阅读", "v0.3 UI renderer must not use M5GFX drawString for Chinese")
    assert_not_contains(ui_cpp, "drawString(\"书", "v0.3 UI renderer must not use M5GFX drawString for Chinese")
    assert_not_contains(ui_cpp, "drawString(\"设置", "v0.3 UI renderer must not use M5GFX drawString for Chinese")

    ui_sources = [
        "src/vink3/ui/VinkUiRenderer.cpp",
        "src/vink3/state/StateMachine.cpp",
        "src/vink3/input/InputService.cpp",
        "src/vink3/runtime/VinkRuntime.cpp",
        "src/vink3/sync/LegadoService.cpp",
        "src/vink3/text/CjkTextRenderer.cpp",
    ]
    for rel in ui_sources:
        text = read(rel)
        if "M5.Display.display(" in text or "M5.Display.pushSprite(" in text:
            fail(f"v0.3 non-display service directly writes physical display: {rel}")
    ok("v0.3 physical display writes are isolated to DisplayService")


def manifest_and_artifacts(slug: str, strict_artifacts: bool = False) -> None:
    data = json.loads((PROJECT / "releases.json").read_text(encoding="utf-8"))
    releases = data.get("releases") or []
    if not releases:
        fail("releases.json has no releases")
    top = releases[0]
    version = top.get("version")
    if not version:
        fail("top release has no version")
    ok(f"Top manifest release is {version}: {top.get('name')}")

    assets = top.get("assets") or {}
    full_asset = assets.get("full") or {}
    if "ota" in assets or "spiffs" in assets:
        fail("top release must be full-only; do not publish OTA/SPIFFS assets for new Vink PaperS3 builds")

    manifest_size = full_asset.get("size")
    if manifest_size != FULL_FLASH_SIZE:
        fail(f"full manifest size must be exactly 16MB: {manifest_size}")
    full_offset = full_asset.get("flashOffset")
    if full_offset == "0x0":
        full_offset = 0
    if full_offset != 0:
        fail(f"full flashOffset must be 0, got {full_offset}")

    path = ARTIFACTS / f"Vink-PaperS3-{version}-{slug}-full-16MB.bin"
    if not path.exists():
        if strict_artifacts:
            fail(f"missing full artifact: {path}")
        ok(f"full-only manifest is declared: {manifest_size} bytes @ offset 0")
        return

    actual_size = path.stat().st_size
    if actual_size != manifest_size:
        if strict_artifacts:
            fail(f"full size mismatch: artifact={actual_size}, manifest={manifest_size}")
        ok(f"full manifest size is declared: {manifest_size} (cached artifact is local build: {actual_size})")
        return
    ok(f"full artifact size matches manifest: {actual_size}")


def built_artifacts_smoke(slug: str) -> None:
    data = json.loads((PROJECT / "releases.json").read_text(encoding="utf-8"))
    version = data["releases"][0]["version"]
    path = ARTIFACTS / f"Vink-PaperS3-{version}-{slug}-full-16MB.bin"
    if not path.exists():
        fail(f"missing built full artifact: {path}")
    size = path.stat().st_size
    if size != FULL_FLASH_SIZE:
        fail(f"full image must be exactly 16MB, got {size}")
    ok(f"built full image is 16MB: {size}")

    forbidden = [
        ARTIFACTS / f"Vink-PaperS3-{version}-{slug}-ota.bin",
        ARTIFACTS / f"Vink-PaperS3-{version}-{slug}-spiffs.bin",
    ]
    for artifact in forbidden:
        if artifact.exists():
            fail(f"full-only workflow should not create standalone artifact: {artifact}")
    ok("full-only workflow did not create OTA/SPIFFS deliverables")


def json_valid() -> None:
    manifest = json.loads((PROJECT / "releases.json").read_text(encoding="utf-8"))
    ok("releases.json parses")
    version = manifest["releases"][0]["version"]
    full = manifest["releases"][0].get("assets", {}).get("full", {})
    flash_offset = full.get("flashOffset")
    if flash_offset == "0x0":
        flash_offset = 0
    if full.get("size") != FULL_FLASH_SIZE or flash_offset != 0:
        fail(f"releases.json must point at the current {version} full-only 16MB image at offset 0")
    if not full.get("name", "").endswith("full-16MB.bin"):
        fail("releases.json full asset must be the user-facing full 16MB image")
    ok("releases.json points at the current full-only RC image")


def build_all(slug: str) -> None:
    if not shutil.which("pio"):
        fail("PlatformIO `pio` not found in PATH")
    version = json.loads((PROJECT / "releases.json").read_text(encoding="utf-8"))["releases"][0]["version"]
    ARTIFACTS.mkdir(parents=True, exist_ok=True)
    out = ARTIFACTS / f"Vink-PaperS3-{version}-{slug}-full-16MB.bin"

    run(["tools/build_full_firmware.sh", str(out)])
    if not out.exists():
        fail(f"full image missing after build: {out}")
    ok(f"full-only artifact ready: {out}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Run local Vink-PaperS3 firmware smoke tests")
    parser.add_argument("--build", action="store_true", help="run PlatformIO build/buildfs as internal steps, merge one full 16MB image, and copy only that full artifact")
    parser.add_argument("--slug", default=DEFAULT_SLUG, help="artifact slug used in workspace/artifacts filenames")
    parser.add_argument("--strict-artifacts", action="store_true", help="require cached artifacts to exactly match releases.json")
    args = parser.parse_args()

    try:
        json_valid()
        source_invariants()
        if args.build:
            build_all(args.slug)
            built_artifacts_smoke(args.slug)
        else:
            manifest_and_artifacts(args.slug, strict_artifacts=args.strict_artifacts)
    except subprocess.CalledProcessError as e:
        print(f"[FAIL] command failed with exit code {e.returncode}", file=sys.stderr)
        return e.returncode or 1
    except CheckFailed as e:
        print(f"[FAIL] {e}", file=sys.stderr)
        return 1

    print("\nAll local firmware smoke checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
