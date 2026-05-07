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
    assert_not_contains(platformio, "ElegantOTA", "unused legacy OTA dependency is not listed")
    assert_not_contains(platformio, "ArduinoJson", "unused legacy JSON dependency is not listed")

    font = read("src/FontManager.cpp")
    assert_not_contains(font, "SPIFFS.begin(true", "Font manager must not format SPIFFS on mount failure")

    return vink3_source_invariants(main_cpp)


def vink3_source_invariants(main_cpp: str) -> None:
    display_h = read("src/vink3/display/DisplayService.h")
    display_cpp = read("src/vink3/display/DisplayService.cpp")
    runtime_cpp = read("src/vink3/runtime/VinkRuntime.cpp")
    input_cpp = read("src/vink3/input/InputService.cpp")
    state_cpp = read("src/vink3/state/StateMachine.cpp")
    legado_cpp = read("src/vink3/sync/LegadoService.cpp")
    ui_cpp = read("src/vink3/ui/VinkUiRenderer.cpp")
    cjk_cpp = read("src/vink3/text/CjkTextRenderer.cpp")
    ui_font_cpp = read("src/vink3/text/VinkUiFont24.cpp")
    reader_cpp = read("src/vink3/reader/ReaderTextRenderer.cpp")
    reader_book_h = read("src/vink3/reader/ReaderBookService.h")
    reader_book_cpp = read("src/vink3/reader/ReaderBookService.cpp")
    chapter_cpp = read("src/ChapterDetector.cpp")
    codec_cpp = read("src/TextCodec.cpp")
    toc_tool = read("tools/detect_txt_toc.py")
    partitions_csv = read("custom_16MB.csv")
    full_font_h = read("src/vink3/text/ReadPaperFullFont.h")
    gbk_table_h = read("src/vink3/text/GbkUnicodeTable.h")
    upstream = read("src/vink3/ReadPaper176.h")
    platformio = read("platformio.ini")

    assert_contains(main_cpp, "xTaskCreatePinnedToCore", "v0.3 main starts a ReadPaper-style pinned MainTask")
    assert_contains(upstream, "kVinkPaperS3FirmwareVersion = \"v0.3.19\"", "single firmware version constant matches the manifest top version")
    assert_contains(main_cpp, "kVinkPaperS3FirmwareVersion", "main task init log uses the shared firmware version")
    assert_contains(runtime_cpp, "kVinkPaperS3FirmwareVersion", "runtime boot logs use the shared firmware version")
    assert_not_contains(main_cpp, "v0.3.2-rc", "main task must not show stale firmware version")
    assert_not_contains(ui_cpp, "v0.3.4-rc", "settings/about must not show stale firmware version")
    assert_not_contains(reader_cpp, "v0.3.2-rc", "reader management pages must not show stale firmware version")
    assert_contains(runtime_cpp, "kReadPaperUpstreamVersion", "v0.3 runtime records ReadPaper upstream baseline")
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
    assert_contains(upstream, "V1.7.6", "v0.3 baseline is ReadPaper V1.7.6")
    assert_contains(upstream, "e910d29", "v0.3 baseline records latest remote commit")
    assert_contains(display_h, "DisplayRequest", "v0.3 display queue has ReadPaper-style request struct")
    assert_contains(display_cpp, "cloneCanvas()", "v0.3 display queue snapshots canvas before physical push")
    assert_contains(display_cpp, "enqueue skipped: canvas snapshot allocation failed", "display service drops/retries instead of pushing mutable canvas if snapshot fails")
    assert_not_contains(display_cpp, "canvasToPush ? canvasToPush : canvas_", "display service must not fall back to mutable global canvas")
    assert_contains(display_cpp, "M5.Display.waitDisplay()", "v0.3 display task serializes physical EPD pushes")
    assert_contains(display_cpp, "return kQualityRefresh", "display service uses official-baseline quality refresh until real-device boot is stable")
    assert_contains(display_cpp, "g_inDisplayPush", "v0.3 display task exposes in-push guard")
    assert_contains(input_cpp, "g_inDisplayPush", "v0.3 input task suppresses events during display push")
    assert_contains(input_cpp, "M5.update();", "v0.3 input task owns M5.update polling")
    assert_contains(input_cpp, "side power key is hardware-managed", "input task documents that PaperS3 side key is hardware-managed")
    assert_contains(input_cpp, "does not expose that PMS150G", "input task must not pretend M5Unified exposes PaperS3 side-key reads")
    assert_not_contains(input_cpp, "pollPowerButton", "input task must not poll fake PaperS3 BtnPWR state")
    assert_not_contains(input_cpp, "M5.BtnPWR.isPressed()", "input task must not use unsupported PaperS3 BtnPWR reads")
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
    assert_contains(state_cpp, "M5.Power.powerOff();", "shutdown path uses official M5.Power.powerOff")
    assert_not_contains(state_cpp, "pulsePaperS3PowerOffPin", "shutdown path must not duplicate M5Unified's PaperS3 GPIO44 pulse")
    assert_contains(state_cpp, "esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL)", "shutdown fallback disables wake sources before deep sleep")
    assert_contains(state_cpp, "esp_deep_sleep_start", "shutdown path enters ESP32-S3 deep sleep")
    assert_not_contains(state_cpp, "esp_sleep_enable_ext0_wakeup(kPowerKeyPin", "shutdown fallback must not arm unverified GPIO36 wake source")
    assert_contains(legado_cpp, "LegadoService", "v0.3 Legado integration is isolated as a service")
    assert_contains(ui_cpp, "CjkTextRenderer", "v0.3 UI routes text through CJK renderer")
    assert_contains(ui_cpp, "renderDiagnostics", "official PaperS3 touch/display diagnostic page exists")
    assert_contains(ui_cpp, "返回", "diagnostic page has a visible return button")
    assert_contains(state_cpp, "diagAction == UiAction::TabSettings", "diagnostic page return button exits back to settings")
    assert_contains(ui_cpp, "renderShutdownConfirm", "settings power entry shows a shutdown confirmation page first")
    assert_contains(ui_cpp, "确认关机", "shutdown confirmation has an explicit confirm button")
    assert_contains(ui_cpp, "侧边键双击仍保留硬件关机", "shutdown confirmation documents the hardware double-click path")
    assert_contains(ui_cpp, "renderShutdown", "official side power-key shutdown page exists")
    assert_contains(ui_cpp, "renderPowerOffReady", "final retained shutdown page exists")
    assert_contains(ui_cpp, "Vink 已关机", "final retained shutdown page confirms completed shutdown path")
    assert_contains(ui_cpp, "后续可替换为 SD 卡关机图片", "final retained shutdown page keeps room for future SD-card image replacement")
    assert_contains(ui_cpp, "官方侧键：双击硬件关机", "shutdown UI documents official PaperS3 side-key behavior")
    assert_contains(ui_cpp, "RequestShutdown", "settings page has touch shutdown fallback when physical side key is unreadable")
    assert_contains(ui_cpp, "ConfirmShutdown", "shutdown requires a second explicit confirmation action")
    assert_contains(state_cpp, "SystemState::ShutdownConfirm", "state machine has a shutdown confirmation state")
    assert_contains(ui_cpp, "raw:", "diagnostic page shows raw GT911/M5Unified coordinates")
    assert_contains(ui_cpp, "norm:", "diagnostic page shows normalized Vink coordinates")
    assert_contains(ui_cpp, "USB:%s CHG:%s BAT:%.2fV", "diagnostic page shows official power/USB/battery signals")
    assert_contains(ui_cpp, "drawSettingsRow", "settings rows align label, value, and arrow explicitly")
    assert_contains(ui_cpp, "kRowH / 2", "settings row label/value/arrow share one computed centerline")
    assert_contains(ui_cpp, "同一水平线", "settings page documents row alignment intent")
    assert_contains(ui_cpp, "formatStatusTime", "status bar shows system time at the left")
    assert_contains(ui_cpp, "formatBatteryPercent", "status bar shows battery percentage at the right")
    assert_contains(ui_cpp, "readOfficialBatteryVoltage", "diagnostic/status path can read factory-style battery voltage")
    assert_contains(ui_cpp, "isOfficialUsbConnected", "diagnostic/status path can read official USB detect")
    assert_contains(ui_cpp, "isOfficialChargeStateActive", "diagnostic page can read official charge-state pin")
    assert_contains(ui_cpp, "Buttons sit inside the current-book card", "reader home hit-test checks visible buttons before surrounding card")
    assert_contains(ui_cpp, "drawButton(304, 286, 180, 48, \"书架\")", "reader home bookshelf button geometry is explicit")
    assert_not_contains(ui_cpp, "繁简", "v0.3 UI must not show Traditional/Simplified toggle wording")
    assert_contains(ui_font_cpp, "g_vink_ui_font24_available = true", "compiled 24px SC UI font is available without SPIFFS")
    assert_contains(cjk_cpp, "PROGMEM SC 24px UI font loaded", "v0.3 UI uses compiled Simplified Chinese SC font as the primary renderer")
    assert_contains(cjk_cpp, "filesystem", "CJK renderer documents that shell readability must not depend on SPIFFS")
    assert_contains(cjk_cpp, "bundled SC 16px UI font fallback loaded", "SPIFFS SC font is fallback only, not the primary UI dependency")
    assert_contains(cjk_cpp, "secondary fallback active", "CJK renderer must log when SPIFFS SC font is not actually available")
    assert_contains(cjk_cpp, "ReadPaper V3 UI subset last-resort fallback armed", "ReadPaper UI subset is only a last-resort fallback for missing UI Chinese")
    assert_contains(cjk_cpp, "leaving blank Chinese labels", "CJK renderer must not silently skip missing bundled glyphs")
    assert_contains(cjk_cpp, "Layout must follow the primary renderer", "CJK layout metrics must use the active primary font, not fallback height")
    assert_contains(cjk_cpp, "one common baseline", "UI gray font rendering must not baseline-stagger Latin letters")
    assert_contains(cjk_cpp, "g/p/y", "UI baseline must preserve Latin descenders")
    assert_contains(reader_cpp, "ReadPaper V3 glyph entries already store a visual yOffset", "reader ReadPaper glyph rendering must not center punctuation/lowercase in prompts")
    assert_contains(reader_cpp, "visual top coordinate", "reader gray fallback rendering must not baseline-stagger Latin letters")
    assert_contains(reader_cpp, "ReaderTextRenderer", "v0.3 has a separate reader body renderer")
    assert_contains(reader_cpp, "beginReadPaperFullFont", "reader body renderer uses full ReadPaper PROGMEM font")
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
    assert_contains(reader_book_cpp, "清除分页缓存", "book entry page exposes page-cache cleanup")
    assert_contains(reader_book_cpp, "重新生成目录", "book entry page exposes TOC rebuild")
    assert_contains(reader_book_cpp, "SD.remove(path)", "book cache maintenance removes stale sidecar files")
    assert_contains(reader_book_cpp, "library browser", "reader book service uses file-browser mode")
    assert_contains(reader_book_cpp, "Subdirectories are", "reader library enters subdirectories instead of flattening everything")
    assert_contains(reader_book_cpp, "setDisplayNameFromPath", "reader library strips file extensions from visible book names")
    assert_contains(reader_book_cpp, "□  %s", "reader library renders a minimal directory icon")
    assert_contains(reader_book_cpp, "▤  %s", "reader library renders a minimal book icon before book files")
    assert_not_contains(reader_book_cpp, "▤  [%s]", "reader library must not show cache flags in book rows")
    assert_contains(reader_book_cpp, "getSidecarPathForBook", "reader book sidecar files stay beside nested source books")
    assert_contains(reader_book_cpp, "kLastBookRecordPath", "reader tab keeps a separate last-read book record")
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
    assert_contains(reader_book_cpp, "left third = previous page", "reader page uses large official-friendly 3-zone tap navigation")
    assert_contains(reader_cpp, "renderListPage", "reader text renderer can draw list rows aligned with tap zones")
    assert_contains(reader_cpp, "drawShellTabs", "reader management pages show the same four-tab shell")
    assert_contains(reader_cpp, "outline + underline", "reader tabs use the same no-black-fill selected style as shell tabs")
    assert_contains(reader_book_h, "kListFirstRowY = 204", "reader list touch rows start below visible top tabs")
    assert_contains(ui_cpp, "renderUiListPage", "UI list renderer exists for tab/list pages")
    assert_contains(reader_book_cpp, "renderUiListPage(SystemState::Library", "library tab uses UI font renderer, not reading font")
    assert_contains(reader_book_cpp, "renderUiListPage(SystemState::Reader", "TOC/navigation list uses UI font renderer, not reading font")
    assert_contains(reader_cpp, "renderActionPage", "reader text renderer can still draw body-font action pages if needed")
    assert_contains(ui_cpp, "renderUiActionPage", "UI action renderer exists for pre-reading pages")
    assert_contains(reader_book_cpp, "renderUiActionPage(SystemState::Reader, \"书籍入口\"", "book entry uses UI font action rendering before reading starts")
    assert_not_contains(reader_book_cpp, "g_readerText.renderActionPage(\"书籍入口\"", "book entry must not use reading font before reading starts")
    assert_contains(reader_book_cpp, "kEntryContinueY", "book entry tap zones share fixed button geometry")
    assert_contains(reader_book_cpp, "renderBookEntryPage", "reader book service shows a book entry action page")
    assert_contains(reader_book_cpp, "ChapterDetector", "reader book service detects TXT table of contents")
    assert_contains(reader_book_cpp, "no TOC found, using whole-book fallback", "reader falls back to whole-book reading when TOC detection fails")
    assert_contains(reader_book_cpp, "the “第” in “第一章 你好”", "chapter pagination starts at the TOC title byte offset")
    assert_not_contains(reader_book_cpp, "Skip the chapter title line itself", "reader must not skip chapter title line before pagination")
    assert_contains(reader_book_cpp, "TextCodec::convertToUTF8", "reader book service converts GBK TXT before TOC detection")
    assert_contains(reader_book_cpp, ".vink-toc", "reader book service stores TOC cache beside the source book")
    assert_contains(reader_book_cpp, "nextTocPage", "reader book service supports TOC paging")
    assert_contains(reader_book_cpp, "buildChapterPages", "reader book service builds chapter page tables")
    assert_contains(reader_book_cpp, ".vink-progress", "reader book service stores progress beside the source book")
    assert_contains(reader_book_cpp, "VPR2", "reader progress cache is schema-versioned")
    assert_contains(reader_book_cpp, ".vink-pages", "reader stores all chapter pagination records in one aggregate page cache file")
    assert_not_contains(reader_book_cpp, ".vink-page-%04d", "reader must not create one pagination cache file per chapter")
    assert_contains(reader_book_cpp, "readerLayoutKey", "reader page cache is keyed by layout so TOC index survives font/spacing changes")
    assert_contains(reader_book_cpp, "FILE_APPEND", "reader aggregate page cache appends chapter records instead of overwriting")
    assert_contains(reader_book_cpp, "preheatChapterPageCache", "reader preheats next chapter pagination before boundary turns")
    assert_contains(reader_book_cpp, "maybePreheatNextChapter", "reader invokes next-chapter pagination preheat while reading")
    assert_contains(reader_book_cpp, "VPG2", "reader page cache validates file size/schema")
    assert_contains(reader_book_cpp, "currentTocIndex_ + 1", "reader book service advances across chapter boundaries")
    assert_not_contains(reader_book_cpp, "*为当前章节", "TOC must not use star text to mark the current chapter")
    assert_not_contains(reader_book_cpp, "%c%03d  %s", "TOC rows must not prefix original chapter titles with star/index")
    assert_not_contains(reader_book_cpp, "%03d %s", "reading headers must use original chapter titles without numeric prefix")
    assert_contains(ui_cpp, "plainRows = active == SystemState::Reader", "TOC page uses clean plain rows instead of boxed rows")
    assert_contains(ui_cpp, "Current TOC item: match the tab style with a clean underline only", "TOC marks current chapter with underline only")
    assert_not_contains(ui_cpp, "drawRoundRect(x + 3", "TOC current marker must not draw a dirty extra frame")
    assert_contains(reader_cpp, "measurePageBytes", "reader text renderer exposes page-fit measurement")
    assert_contains(reader_book_cpp, "openTocEntry", "reader book service can open a TOC entry preview")
    assert_contains(state_cpp, "SystemState::ReaderMenu", "state machine routes reader menu interactions")
    assert_contains(state_cpp, "renderLibraryPage", "state machine routes Library tab through reader book list")
    assert_contains(state_cpp, "fromLibrary", "state machine preserves library tap selection before opening reader")
    assert_contains(state_cpp, "lastLibraryTapOpenedBook", "state machine stays in Library when a directory row is tapped")
    assert_contains(state_cpp, "if (!g_readerBook.handleLibraryTap", "state machine only reacts after a valid library row tap")
    assert_contains(full_font_h, "g_readpaper_full_font_data", "v0.3 full ReadPaper font is compiled as PROGMEM")
    assert_contains(partitions_csv, "0xC00000", "v0.3 partition table has a large single app slot for full ReadPaper font")
    assert_not_contains(partitions_csv, "app1", "v0.3 partition table drops dual OTA app1 to fit full ReadPaper font")
    assert_contains(gbk_table_h, "gbkToUnicode", "v0.3 includes full ReadPaper-derived GBK Unicode lookup")
    assert_contains(codec_cpp, "vink3::gbkToUnicode", "TextCodec uses full GBK lookup before legacy table")
    assert_contains(chapter_cpp, "U'四'", "chapter detector parses full Chinese numerals")
    assert_contains(chapter_cpp, "lastChapterNumber", "chapter detector suppresses duplicate/outlier chapter headings")
    assert_contains(chapter_cpp, "免费阅读", "chapter detector documents web TXT duplicate heading cleanup")
    assert_contains(chapter_cpp, "0xE3", "chapter detector trims ideographic leading spaces")
    assert_contains(toc_tool, "last_number", "host TXT TOC detector suppresses duplicate/outlier chapter headings")
    assert_contains(toc_tool, "detect_toc", "host TXT TOC detector exists for large novel validation")
    assert_contains(ui_cpp, "Vink 加载中", "boot page uses the normal Vink UI font instead of diagnostic probes")
    assert_contains(ui_cpp, "hourglass loading mark", "boot page shows a simple vector hourglass loading icon")
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
    if full.get("size") != FULL_FLASH_SIZE or full.get("flashOffset") != 0:
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
