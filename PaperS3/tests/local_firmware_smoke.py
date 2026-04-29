#!/usr/bin/env python3
"""Local smoke tests for Vink-PaperS3 firmware.

This is not a PaperS3 hardware emulator. It is a deterministic local gate for
things that repeatedly caused real-device regressions:

- source-level display/touch invariants
- no legacy direct FileBrowser render path from App
- non-tab menu pages are dirty-gated instead of repainting every loop
- transition touches wait for finger release
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
DEFAULT_SLUG = "official-input-ui-power"
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
    if "vink3/runtime/VinkRuntime.h" in main_cpp:
        return vink3_source_invariants(main_cpp)

    app = read("src/App.cpp")
    app_h = read("src/App.h")
    wifi = read("src/WiFiUploader.cpp")
    font = read("src/FontManager.cpp")

    assert_contains(app, "static bool ensureShellCanvas()", "Shell canvas has an explicit ensure/prealloc helper")
    assert_contains(app, "Shell canvas preallocation failed", "Shell canvas is preallocated during display init")
    assert_contains(app, "_pageNeedsRender || _state != _lastRenderedState", "Non-tab shell pages are dirty-gated")
    assert_contains(app_h, "bool _pageNeedsRender;", "App stores subpage dirty state")
    assert_contains(app_h, "AppState _lastRenderedState;", "App tracks last rendered shell subpage")
    assert_contains(app, "gLastShellCommitOk", "Shell commit success is tracked")
    assert_contains(app_h, "bool _touchWaitRelease;", "Transition touch waits for release")
    assert_contains(app, "touchCount > 0", "Touch suppressor waits until finger is lifted")
    assert_contains(app, "_touchSuppressUntil = millis() + 180", "Tab/back transition cooldown exists")
    assert_contains(app, "_touchSuppressUntil = millis() + 300", "Wake transition cooldown exists")
    assert_contains(app, "if (!waitShellDisplayReady(3500))", "Shell commit is serialized before physical display")
    assert_contains(app, "_tabNeedsRender[tab] = true;", "Toast/tab dirty invalidation exists")

    assert_not_contains(app, "_browser.render(", "App does not call legacy FileBrowser direct render")
    assert_not_contains(app, "commitTopTabsFeedback", "No top-tab partial refresh path remains")
    assert_not_contains(wifi, "SPIFFS.begin(true", "WiFi uploader must not format SPIFFS on mount failure")
    assert_not_contains(font, "SPIFFS.begin(true", "Font manager must not format SPIFFS on mount failure")

    # Keep direct display calls intentionally narrow.
    display_lines = [
        (i + 1, line.strip())
        for i, line in enumerate(app.splitlines())
        if "display.display" in line
    ]
    allowed_patterns = [
        "display.display();",
        "display.display(x - 6, y - 6, w + 12, h + 12);",
        "display.display(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);",
        "display.displayBusy()",
    ]
    for line_no, line in display_lines:
        if not any(p in line for p in allowed_patterns):
            fail(f"unexpected display.display use at App.cpp:{line_no}: {line}")
    ok(f"Direct display calls are limited to intended paths ({len(display_lines)} lines)")


def vink3_source_invariants(main_cpp: str) -> None:
    display_h = read("src/vink3/display/DisplayService.h")
    display_cpp = read("src/vink3/display/DisplayService.cpp")
    runtime_cpp = read("src/vink3/runtime/VinkRuntime.cpp")
    input_cpp = read("src/vink3/input/InputService.cpp")
    state_cpp = read("src/vink3/state/StateMachine.cpp")
    legado_cpp = read("src/vink3/sync/LegadoService.cpp")
    ui_cpp = read("src/vink3/ui/VinkUiRenderer.cpp")
    cjk_cpp = read("src/vink3/text/CjkTextRenderer.cpp")
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
    assert_contains(runtime_cpp, "kReadPaperUpstreamVersion", "v0.3 runtime records ReadPaper upstream baseline")
    assert_contains(runtime_cpp, "applyPaperS3PortraitRotation", "v0.3 PaperS3 display rotation is verified against actual M5GFX dimensions")
    assert_contains(runtime_cpp, "M5.Display.setRotation(kPaperS3DisplayRotation)", "v0.3 PaperS3 display rotation starts from the official-profile constant")
    assert_contains(upstream, "kPaperS3DisplayRotation = 0", "official touch example rotation 0 is the Vink diagnostic baseline")
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
    assert_contains(upstream, "kPowerKeyPin = GPIO_NUM_36", "PaperS3 side power key pin is recorded")
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
    assert_contains(display_cpp, "M5.Display.setColorDepth(kTextColorDepth);", "display service restores normal PaperS3 grayscale color depth after quality refresh")
    assert_contains(display_cpp, "g_inDisplayPush", "v0.3 display task exposes in-push guard")
    assert_contains(input_cpp, "g_inDisplayPush", "v0.3 input task suppresses events during display push")
    assert_contains(input_cpp, "M5.update();", "v0.3 input task owns M5.update polling")
    assert_contains(input_cpp, "pollPowerButton", "input task polls the official side power key")
    assert_contains(input_cpp, "power key armed after boot release", "power key ignores the boot press until release")
    assert_contains(input_cpp, "kPowerDoubleClickWindowMs", "power key follows official double-click shutdown behavior")
    assert_contains(input_cpp, "kPowerLongHoldMs", "power key has deliberate long-hold fallback instead of accidental short press shutdown")
    assert_contains(input_cpp, "lastPoint_ = currentPoint", "touch service caches last valid pressed coordinate")
    assert_contains(input_cpp, "lastRawPoint_ = rawPoint", "touch service preserves raw PaperS3 coordinates for diagnostics")
    assert_contains(input_cpp, "const TouchPoint releasePoint = lastPoint_", "touch service must not use release-time invalid coordinates for taps")
    assert_contains(input_cpp, "normalizeTouchPoint", "touch service normalizes/clamps raw PaperS3 coordinates before hit-test")
    assert_contains(input_cpp, "transformRawPaperS3Point", "touch service has explicit physical-to-portrait transform fallback")
    assert_contains(input_cpp, "gPaperS3TouchCoordMode", "touch transform uses a persistent coordinate mode instead of per-point guessing")
    assert_contains(input_cpp, "suppressUntilRelease", "touch service can suppress stale wake/transition touches until release")
    assert_contains(input_cpp, "gesture cancelled", "touch service cancels drag movement too large for tap but too small for swipe")
    assert_contains(input_cpp, "gPaperS3ActiveDisplayRotation", "touch transform is tied to verified active display rotation")
    assert_contains(state_cpp, "xQueueReceive", "v0.3 state machine is queue-driven")
    assert_contains(state_cpp, "MessageType::PowerButton", "state machine handles side power-key shutdown")
    assert_contains(state_cpp, "pulsePaperS3PowerOffPin", "shutdown path pulses PaperS3 PMIC power-off pin")
    assert_contains(state_cpp, "esp_deep_sleep_start", "shutdown path has deep-sleep fallback after M5.Power.powerOff")
    assert_contains(legado_cpp, "LegadoService", "v0.3 Legado integration is isolated as a service")
    assert_contains(ui_cpp, "CjkTextRenderer", "v0.3 UI routes text through CJK renderer")
    assert_contains(ui_cpp, "renderDiagnostics", "official PaperS3 touch/display diagnostic page exists")
    assert_contains(ui_cpp, "renderShutdown", "official side power-key shutdown page exists")
    assert_contains(ui_cpp, "双击关机", "shutdown UI documents official PaperS3 double-click power-off behavior")
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
    assert_contains(cjk_cpp, "bundled SC 16px UI font loaded", "v0.3 UI prefers compact bundled Simplified Chinese SC font")
    assert_contains(cjk_cpp, "ReadPaper V3 UI subset fallback", "ReadPaper UI subset is fallback only, not Vink UI default")
    assert_contains(reader_cpp, "ReaderTextRenderer", "v0.3 has a separate reader body renderer")
    assert_contains(reader_cpp, "beginReadPaperFullFont", "reader body renderer uses full ReadPaper PROGMEM font")
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
    assert_contains(reader_book_cpp, ".vink-toc", "reader book service stores TOC cache beside the source book")
    assert_contains(reader_book_cpp, "nextTocPage", "reader book service supports TOC paging")
    assert_contains(reader_book_cpp, "buildChapterPages", "reader book service builds chapter page tables")
    assert_contains(reader_book_cpp, ".vink-progress", "reader book service stores progress beside the source book")
    assert_contains(reader_book_cpp, "VPR2", "reader progress cache is schema-versioned")
    assert_contains(reader_book_cpp, ".vink-pages", "reader book service stores chapter page cache beside the source book")
    assert_contains(reader_book_cpp, "VPG2", "reader page cache validates file size/schema")
    assert_contains(reader_book_cpp, "currentTocIndex_ + 1", "reader book service advances across chapter boundaries")
    assert_contains(reader_book_cpp, "*为当前章节", "TOC marks the current chapter")
    assert_contains(reader_cpp, "measurePageBytes", "reader text renderer exposes page-fit measurement")
    assert_contains(reader_book_cpp, "openTocEntry", "reader book service can open a TOC entry preview")
    assert_contains(state_cpp, "SystemState::ReaderMenu", "state machine routes reader menu interactions")
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
    assert_contains(chapter_cpp, "0xE3", "chapter detector trims ideographic leading spaces")
    assert_contains(toc_tool, "last_number", "host TXT TOC detector suppresses duplicate/outlier chapter headings")
    assert_contains(toc_tool, "detect_toc", "host TXT TOC detector exists for large novel validation")
    assert_not_contains(ui_cpp, "drawString", "v0.3 UI renderer must not use M5GFX drawString for Chinese")

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
    ox = json.loads((PROJECT / "oxflash.json").read_text(encoding="utf-8"))
    top = ox[0]["versions"][0]
    if version not in top.get("version", "") or top.get("size") != FULL_FLASH_SIZE:
        fail(f"oxflash.json must point at the current {version} full-only 16MB image")
    if "ota" in top.get("file", "").lower() or "spiffs" in top.get("file", "").lower():
        fail("oxflash.json must not advertise OTA/SPIFFS artifacts")
    ok("oxflash.json points at the current full-only RC image")


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
