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
DEFAULT_SLUG = "performance"
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
    assert_contains(platformio, "bblanchon/ArduinoJson", "active config/Legado/WebUI JSON dependency is listed")

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
    reader_h = read("src/vink3/reader/ReaderTextRenderer.h")
    reader_book_h = read("src/vink3/reader/ReaderBookService.h")
    reader_book_cpp = read("src/vink3/reader/ReaderBookService.cpp")
    config_cpp = read("src/vink3/config/ConfigService.cpp")
    chapter_cpp = read("src/ChapterDetector.cpp")
    codec_cpp = read("src/TextCodec.cpp")
    toc_tool = read("tools/detect_txt_toc.py")
    partitions_csv = read("custom_16MB.csv")
    full_font_h = read("src/vink3/text/ReadPaperFullFont.h")
    gbk_table_h = read("src/vink3/text/GbkUnicodeTable.h")
    upstream = read("src/vink3/ReadPaper176.h")
    platformio = read("platformio.ini")

    assert_contains(main_cpp, "xTaskCreatePinnedToCore", "v0.3 main starts a ReadPaper-style pinned MainTask")
    assert_contains(upstream, "kVinkPaperS3FirmwareVersion = \"v0.3.13-rc-performance\"", "single firmware version constant matches the manifest top version")
    assert_contains(main_cpp, "kVinkPaperS3FirmwareVersion", "main task init log uses the shared firmware version")
    assert_contains(runtime_cpp, "kVinkPaperS3FirmwareVersion", "runtime boot logs use the shared firmware version")
    assert_not_contains(main_cpp, "v0.3.2-rc", "main task must not show stale firmware version")
    assert_not_contains(ui_cpp, "v0.3.4-rc", "settings/about must not show stale firmware version")
    assert_not_contains(reader_cpp, "v0.3.2-rc", "reader management pages must not show stale firmware version")
    assert_contains(runtime_cpp, "kReadPaperUpstreamVersion", "v0.3 runtime records ReadPaper upstream baseline")
    assert_contains(runtime_cpp, "buildLegadoBaseUrlForRuntime", "runtime configures saved Legado host/port on boot")
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
    assert_contains(display_h, "QueuedDisplayRequest", "display request and immutable snapshot travel in one queue item")
    assert_contains(display_h, "queueLen = 3", "display queue length is capped to avoid PSRAM snapshot pileups")
    assert_not_contains(display_h, "canvasQueue_", "display service must not keep a separate snapshot queue that can desync")
    assert_contains(display_cpp, "cloneCanvas()", "v0.3 display queue snapshots canvas before physical push")
    assert_contains(display_cpp, "enqueue skipped: canvas snapshot allocation failed", "display service drops/retries instead of pushing mutable canvas if snapshot fails")
    assert_contains(display_cpp, "enqueue skipped: display queue full", "display service releases snapshots when the bounded queue is full")
    assert_not_contains(display_cpp, "canvasToPush ? canvasToPush : canvas_", "display service must not fall back to mutable global canvas")
    assert_contains(display_cpp, "M5.Display.waitDisplay()", "v0.3 display task serializes physical EPD pushes")
    assert_contains(display_cpp, "return kQualityRefresh", "display service uses official-baseline quality refresh until real-device boot is stable")
    assert_contains(display_cpp, "g_inDisplayPush", "v0.3 display task exposes in-push guard")
    assert_contains(input_cpp, "g_inDisplayPush", "v0.3 input task suppresses events during display push")
    assert_contains(input_cpp, "M5.update();", "v0.3 input task owns M5.update polling")
    assert_contains(input_cpp, "pollPowerButton", "input task polls the official side power key")
    assert_contains(input_cpp, "BtnPWR armed after boot release", "power key ignores the boot press until release")
    assert_contains(input_cpp, "M5.BtnPWR.setDebounceThresh(0)", "power key uses M5Unified BtnPWR rather than unverified GPIO36")
    assert_contains(input_cpp, "one stable press requests shutdown", "power key gives immediate visible single-press shutdown behavior after real-device feedback")
    assert_contains(input_cpp, "kPowerLongHoldMs", "power key has deliberate long-hold fallback")
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
    assert_contains(state_cpp, "v0.3.7-rc", "BootComplete documents the UI-restore RC behavior")
    assert_contains(state_cpp, "state_ = SystemState::Reader;", "BootComplete enters normal reader home after the validated diagnostic RC")
    assert_contains(state_cpp, "OpenDiagnostics", "diagnostic page remains available from normal UI")
    assert_contains(state_cpp, "MessageType::PowerButton", "state machine handles side power-key shutdown")
    assert_contains(state_cpp, "esp_deep_sleep_start", "shutdown path has deep-sleep fallback after M5.Power.powerOff")
    assert_not_contains(state_cpp, "GPIO_NUM_38", "sleep/shutdown must not arm SD MOSI GPIO38 as a wake source")
    assert_not_contains(state_cpp, "esp_sleep_enable_ext0_wakeup", "touch wake source remains disabled until official GT911 INT wake is validated")
    assert_contains(state_cpp, "wake path is not validated", "auto sleep path is guarded until PaperS3 wake validation")
    assert_contains(legado_cpp, "LegadoService", "v0.3 Legado integration is isolated as a service")
    assert_not_contains(legado_cpp, "JsonArray LegadoService::getBookshelf", "Legado service must not return stack-backed JsonArray views")
    assert_contains(legado_cpp, "getBookshelfCount", "Legado bookshelf count uses safe JSON document lifetime")
    assert_contains(legado_cpp, "obj[\"name\"]", "Legado progress push uses official BookProgress.name field")
    assert_contains(legado_cpp, "obj[\"author\"]", "Legado progress push uses official BookProgress.author field")
    assert_not_contains(legado_cpp, "getBookProgress?url=", "Legado progress pull must not call unsupported Web API getBookProgress endpoint")
    assert_contains(legado_cpp, "getBookshelf", "Legado progress pull uses official Web API bookshelf data")
    assert_contains(legado_cpp, "http.addHeader(\"Authorization\"", "Legado GET/POST requests can carry token auth")
    assert_contains(state_cpp, "buildLegadoBaseUrl", "Legado sync combines host and configured port safely")
    assert_contains(ui_cpp, "CjkTextRenderer", "v0.3 UI routes text through CJK renderer")
    assert_contains(ui_cpp, "renderDiagnostics", "official PaperS3 touch/display diagnostic page exists")
    assert_contains(ui_cpp, "renderShutdown", "official side power-key shutdown page exists")
    assert_contains(ui_cpp, "单按侧边键关机", "shutdown UI documents current single-press hotfix behavior")
    assert_contains(ui_cpp, "RequestShutdown", "settings page has touch shutdown fallback when physical side key is unreadable")
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
    assert_contains(reader_h, "GlyphCacheEntry", "reader text renderer has a bounded glyph lookup cache")
    assert_contains(reader_cpp, "GLYPH_L1_CACHE_SIZE", "reader glyph cache size is controlled by Config.h")
    assert_contains(reader_cpp, "using direct PROGMEM lookup", "reader glyph cache allocation has a safe fallback")
    assert_contains(reader_cpp, "· %u%%", "reader footer shows chapter page progress percentage")
    assert_contains(reader_cpp, "Thin visual progress rail", "reader footer adds a visual progress rail")
    assert_contains(reader_cpp, "fillW", "reader progress rail fills according to percentage")
    assert_contains(reader_cpp, "indentFirstLine", "reader body renderer applies first-line paragraph indentation")
    assert_contains(reader_cpp, "paragraphExtra", "reader body renderer applies paragraph spacing consistently with pagination")
    assert_contains(reader_book_cpp, "ReaderBookService", "v0.3 has reader book service for opening TXT books")
    assert_contains(reader_book_cpp, "saveCurrentProgress", "power shutdown can save current reader progress before power-off")
    assert_contains(reader_book_cpp, "SD is initialized lazily", "reader book service does not block boot on SD initialization")
    assert_contains(reader_book_cpp, "scanBooks", "reader book service scans /books into a library list")
    assert_contains(reader_book_cpp, "sortBooks", "reader library order is stable across SD directory iteration")
    assert_contains(reader_book_cpp, "detectBookFlags", "reader book service shows library progress/cache flags")
    assert_contains(reader_book_cpp, "formatBytes", "reader book entry shows source file size")
    assert_contains(reader_book_cpp, "renderBookLoadingPage", "reader shows blocking status while first-opening large books")
    assert_contains(reader_book_cpp, "renderChapterLoadingPage", "reader shows blocking status while first-paginating chapters")
    assert_contains(reader_book_cpp, "正在分析目录", "reader explains first-open TOC analysis wait")
    assert_contains(reader_book_cpp, "正在分页", "reader explains first-open chapter pagination wait")
    assert_contains(reader_book_cpp, "读/目/页", "library explains progress/TOC/page-cache status markers")
    assert_contains(reader_book_cpp, "handleLibraryTap", "reader book service opens selected library entries")
    assert_contains(reader_book_cpp, "left third = previous page", "reader page uses large official-friendly 3-zone tap navigation")
    assert_contains(reader_book_cpp, "renderEndOfBookPage", "reader shows an explicit end-of-book page instead of a dead next-page tap")
    assert_contains(reader_book_cpp, "本书已读完", "reader end-of-book page uses clear Chinese completion wording")
    assert_contains(reader_book_cpp, "ro.dark       = cfg.darkModeDefault", "reader body honors dark-mode config while rendering pages")
    assert_contains(reader_cpp, "renderListPage", "reader text renderer can draw list rows aligned with tap zones")
    assert_contains(reader_cpp, "drawShellTabs", "reader management pages show the same four-tab shell")
    assert_contains(reader_cpp, "outline + underline", "reader tabs use the same no-black-fill selected style as shell tabs")
    assert_contains(reader_book_h, "kListFirstRowY = 204", "reader list touch rows start below visible top tabs")
    assert_contains(reader_book_cpp, "renderListPage", "library and TOC use explicit row geometry instead of free text layout")
    assert_contains(reader_cpp, "renderActionPage", "reader text renderer can draw aligned action buttons")
    assert_contains(reader_book_cpp, "renderActionPage", "book entry uses real action-button rendering")
    assert_contains(reader_book_cpp, "kEntryContinueY", "book entry tap zones share fixed button geometry")
    assert_contains(reader_book_cpp, "renderBookEntryPage", "reader book service shows a book entry action page")
    assert_contains(reader_book_cpp, "ChapterDetector", "reader book service detects TXT table of contents")
    assert_contains(reader_book_cpp, "no TOC found, using whole-book fallback", "reader falls back to whole-book reading when TOC detection fails")
    assert_contains(reader_book_cpp, "Whole-book fallback has no chapter heading to skip", "whole-book fallback keeps text from the first line")
    assert_contains(reader_book_cpp, "TextCodec::convertToUTF8", "reader book service converts GBK TXT before TOC detection")
    assert_contains(codec_cpp, "Removing stale temp file", "GBK conversion invalidates stale UTF-8 temp files before reading")
    assert_contains(reader_book_cpp, ".vink-toc", "reader book service stores TOC cache beside the source book")
    assert_contains(reader_book_cpp, "VCT3", "reader TOC cache invalidates when source size or detector version changes")
    assert_contains(reader_book_cpp, "detectorVersion", "reader TOC cache records chapter detector rule version")
    assert_contains(reader_book_cpp, "nextTocPage", "reader book service supports TOC paging")
    assert_contains(reader_book_cpp, "buildChapterPages", "reader book service builds chapter page tables")
    assert_contains(reader_book_cpp, ".vink-progress", "reader book service stores progress beside the source book")
    assert_contains(reader_book_cpp, "VPR3", "reader progress cache is schema-versioned")
    assert_contains(reader_book_cpp, "savedOffset", "reader progress restore uses page start offset across layout changes")
    assert_contains(reader_book_cpp, ".vink-pages", "reader book service stores chapter page cache beside the source book")
    assert_contains(reader_book_cpp, "VPG3", "reader page cache validates file size/schema and layout")
    assert_contains(reader_book_cpp, "pageLayoutKey", "reader page cache invalidates when PaperS3 text layout changes")
    assert_contains(reader_book_cpp, "lc.indentFirstLine", "reader page cache includes paragraph indentation in layout fingerprint")
    assert_contains(reader_book_cpp, "lc.paragraphSpacing", "reader page cache includes paragraph spacing in layout fingerprint")
    assert_contains(config_cpp, "config_.indentFirstLine", "reader layout indentation is loaded from persisted config")
    assert_contains(config_cpp, "config_.paragraphSpacing", "reader paragraph spacing is loaded from persisted config")
    assert_contains(reader_book_cpp, "char path[192]", "reader sidecar path buffers match long book path support")
    assert_contains(reader_book_cpp, "pagination hit page cap", "reader pagination logs capped long chapters instead of silently caching partial page tables")
    assert_contains(reader_book_cpp, "pageWindowTruncated_", "reader continues capped long chapters without skipping unread content")
    assert_contains(reader_book_h, "kMaxChapterPages = 4096", "reader page table cap is large enough for long chapters on PSRAM")
    assert_contains(reader_book_cpp, "currentTocIndex_ + 1", "reader book service advances across chapter boundaries")
    assert_contains(reader_book_cpp, "char titleBuf[68]", "reader page headers truncate long chapter titles safely")
    assert_contains(reader_book_cpp, "*为当前章节", "TOC marks the current chapter")
    assert_contains(reader_book_cpp, "tocPage_ = currentTocIndex_ / kTocEntriesPerPage", "opening TOC jumps to the page containing the current chapter")
    assert_contains(reader_cpp, "measurePageBytes", "reader text renderer exposes page-fit measurement")
    assert_contains(reader_book_cpp, "openTocEntry", "reader book service can open a TOC entry preview")
    assert_contains(reader_book_cpp, "章节打开失败", "reader chapter jump has a visible failure fallback instead of silent no-op")
    assert_contains(state_cpp, "SystemState::ReaderMenu", "state machine routes reader menu interactions")
    assert_contains(state_cpp, "state_ == SystemState::ReaderMenu) {\n                if (g_readerBook.handleLongPress", "state machine routes reading long-press gestures to bookmark handling")
    assert_contains(state_cpp, "renderLibraryPage", "state machine routes Library tab through reader book list")
    assert_contains(state_cpp, "fromLibrary", "state machine preserves library tap selection before opening reader")
    assert_contains(state_cpp, "if (!g_readerBook.handleLibraryTap", "state machine only enters reader menu after a valid library row tap")
    assert_contains(full_font_h, "g_readpaper_full_font_data", "v0.3 full ReadPaper font is compiled as PROGMEM")
    assert_contains(partitions_csv, "0xC00000", "v0.3 partition table has a large single app slot for full ReadPaper font")
    assert_not_contains(partitions_csv, "app1", "v0.3 partition table drops dual OTA app1 to fit full ReadPaper font")
    assert_contains(gbk_table_h, "gbkToUnicode", "v0.3 includes full ReadPaper-derived GBK Unicode lookup")
    assert_contains(codec_cpp, "vink3::gbkToUnicode", "TextCodec uses full GBK lookup before legacy table")
    assert_contains(chapter_cpp, "U'四'", "chapter detector parses full Chinese numerals")
    assert_contains(chapter_cpp, "lastChapterNumber", "chapter detector suppresses duplicate/outlier chapter headings")
    assert_contains(chapter_cpp, "免费阅读", "chapter detector documents web TXT duplicate heading cleanup")
    assert_contains(chapter_cpp, "lineStartOffset = file.position", "chapter detector records real byte offsets before stripping CR/LF")
    assert_contains(chapter_cpp, "0xE3", "chapter detector trims ideographic leading spaces")
    assert_contains(chapter_cpp, "正文", "chapter detector strips common web TXT正文 prefixes")
    assert_contains(chapter_cpp, "U'０'", "chapter detector parses full-width Arabic digits")
    assert_contains(chapter_cpp, "Prefix form: 卷一", "chapter detector recognizes volume-prefix headings")
    assert_contains(reader_book_cpp, "右侧为位置", "TOC page shows approximate chapter position for jumping")
    assert_contains(toc_tool, "VOLUME_PREFIX_RE", "host TXT TOC detector recognizes volume-prefix headings")
    assert_contains(toc_tool, "last_number", "host TXT TOC detector suppresses duplicate/outlier chapter headings")
    assert_contains(toc_tool, "detect_toc", "host TXT TOC detector exists for large novel validation")
    assert_contains(toc_tool, "len(line.encode(\"utf-8\"))", "host TXT TOC detector reports firmware-compatible byte offsets")
    assert_contains(ui_cpp, "Use built-in ASCII drawing here", "diagnostic/boot probes may use M5GFX drawString only for ASCII visibility checks")
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

    webui_cpp = read("src/vink3/webui/WebUiService.cpp")
    wifi_cpp = read("src/vink3/sync/WifiService.cpp")
    assert_contains(webui_cpp, '"/index.html"', "Web UI is reachable after WiFiService root redirect")
    assert_contains(webui_cpp, '"/api/files/*"', "Web UI registers wildcard file upload/delete routes")
    assert_contains(webui_cpp, "joinPath(currentPath", "Web UI file actions preserve the current directory")
    assert_contains(webui_cpp, "normalizeUserPath", "Web UI uses shared browser/API path normalization")
    assert_contains(webui_cpp, "queryParamValue(req->uri, \"path\", userPath)", "Web UI file listing extracts the exact path query parameter")
    assert_contains(webui_cpp, "Do not match unrelated keys", "Web UI file listing avoids xpath-style query confusion")
    assert_contains(webui_cpp, "normalizeUserPath(urlDecode(tail))", "Web UI normalizes encoded absolute upload/delete paths")
    assert_contains(webui_cpp, "jsonEscape", "Web UI file list JSON escapes filenames safely")
    assert_contains(webui_cpp, "displayNameForEntry", "Web UI strips filesystem-specific directory prefixes from listed entries")
    assert_contains(webui_cpp, "entry.close();", "Web UI closes each SD directory entry while listing files")
    assert_contains(webui_cpp, "file management is SD-only", "Web UI file manager does not expose SPIFFS/system partition")
    assert_contains(webui_cpp, "sdPathForUserPath", "Web UI file APIs normalize only SD-card paths")
    assert_contains(webui_cpp, "if (!path.startsWith(\"/\")) path = \"/\" + path", "Web UI normalizes malformed relative file paths to SD absolute paths")
    assert_contains(webui_cpp, "while (path.indexOf(\"//\") >= 0) path.replace(\"//\", \"/\")", "Web UI collapses repeated slashes before SD operations")
    assert_contains(webui_cpp, "hasUnsafePathChars", "Web UI rejects control characters and backslashes in file paths")
    assert_contains(webui_cpp, "path[i] == '\\\\'", "Web UI rejects backslash path separators")
    assert_contains(webui_cpp, "isSdRootPath", "Web UI uses normalized SD-root checks for mutating file operations")
    assert_contains(webui_cpp, "realPath == \"/\" || realPath.endsWith(\"/\")", "Web UI upload rejects root and directory-like file targets")
    assert_not_contains(webui_cpp, "SPIFFS.open", "Web UI file manager must not open SPIFFS/system files")
    assert_not_contains(webui_cpp, "SPIFFS.rename", "Web UI file manager must not rename SPIFFS/system files")
    assert_not_contains(webui_cpp, "SPIFFS.remove", "Web UI file manager must not delete SPIFFS/system files")
    assert_not_contains(webui_cpp, "spiffsTotal", "Web UI system info does not expose system partition details")
    assert_not_contains(webui_cpp, "chdir('/spiffs')", "Web UI does not offer a SPIFFS file-management shortcut")
    assert_contains(webui_cpp, "apiFileDownload", "Web UI can download files from browser")
    assert_contains(webui_cpp, "Incomplete upload", "Web UI upload rejects truncated request bodies")
    if webui_cpp.count('return jsonErr(req, 400, "Incomplete read")') < 3:
        fail("Web UI config/mkdir/rename JSON handlers must reject incomplete request bodies")
    ok("Web UI JSON mutation handlers reject incomplete request bodies")
    assert_contains(webui_cpp, "Cannot create directory", "Web UI mkdir reports real failures")
    assert_contains(webui_cpp, "apiFileRename", "Web UI can rename files in place")
    assert_contains(webui_cpp, "ensureBooks", "Web UI exposes a one-tap /books shortcut")
    assert_contains(webui_cpp, "multiple>", "Web UI supports multi-file upload")
    assert_contains(webui_cpp, "sdCardSize", "Web UI system info reports SD card capacity")
    assert_contains(wifi_cpp, "httpd_uri_match_wildcard", "HTTP server enables wildcard route matching for file paths")
    assert_contains(wifi_cpp, "softAP start failed", "WiFi AP start checks softAP failure")
    assert_contains(wifi_cpp, "WebUI server start failed", "WiFi AP+WebUI rolls back if HTTP server fails")
    assert_contains(wifi_cpp, "deprecated", "legacy placeholder book API is marked deprecated")
    assert_contains(wifi_cpp, "max_uri_handlers = 24", "HTTP server allows enough route slots for legacy and Web UI handlers")
    assert_contains(webui_cpp, "kVinkPaperS3FirmwareVersion", "Web UI system info uses shared firmware version")
    assert_contains(webui_cpp, "paragraphSpacing", "Web UI exposes paragraph spacing in reading layout config")
    assert_contains(webui_cpp, "indentFirstLine", "Web UI exposes first-line indentation in reading layout config")
    assert_contains(webui_cpp, "marginLeft", "Web UI exposes reader margins in layout config")
    assert_contains(ui_cpp, "SetWifiApWebUi", "WiFi mode UI maps visible rows to explicit actions")
    assert_not_contains(webui_cpp, 'v0.3.0', "Web UI must not show stale firmware version")


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

    asset_name = full_asset.get("name") or f"Vink-PaperS3-{version}-{slug}-full-16MB.bin"
    path = ARTIFACTS / asset_name
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
    release = data["releases"][0]
    version = release["version"]
    full_asset = (release.get("assets") or {}).get("full") or {}
    path = ARTIFACTS / (full_asset.get("name") or f"Vink-PaperS3-{version}-{slug}-full-16MB.bin")
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
    release = json.loads((PROJECT / "releases.json").read_text(encoding="utf-8"))["releases"][0]
    version = release["version"]
    full_asset = (release.get("assets") or {}).get("full") or {}
    ARTIFACTS.mkdir(parents=True, exist_ok=True)
    out = ARTIFACTS / (full_asset.get("name") or f"Vink-PaperS3-{version}-{slug}-full-16MB.bin")

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
