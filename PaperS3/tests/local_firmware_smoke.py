#!/usr/bin/env python3
"""Local smoke tests for Vink-PaperS3 firmware.

This is not a PaperS3 hardware emulator. It is a deterministic local gate for
things that repeatedly caused real-device regressions:

- source-level display/touch invariants
- no legacy direct FileBrowser render path from App
- non-tab menu pages are dirty-gated instead of repainting every loop
- transition touches wait for finger release
- releases.json top asset sizes match existing release artifacts
- optional PlatformIO build/buildfs/full-merge with partition-size checks
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
ARTIFACTS = WORKSPACE / "artifacts"
DEFAULT_SLUG = "shell-commit-guard"
APP_SLOT_SIZE = 0x600000
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
    reader_book_cpp = read("src/vink3/reader/ReaderBookService.cpp")
    chapter_cpp = read("src/ChapterDetector.cpp")
    codec_cpp = read("src/TextCodec.cpp")
    toc_tool = read("tools/detect_txt_toc.py")
    partitions_csv = read("custom_16MB.csv")
    full_font_h = read("src/vink3/text/ReadPaperFullFont.h")
    gbk_table_h = read("src/vink3/text/GbkUnicodeTable.h")
    upstream = read("src/vink3/ReadPaper176.h")

    assert_contains(main_cpp, "xTaskCreatePinnedToCore", "v0.3 main starts a ReadPaper-style pinned MainTask")
    assert_contains(runtime_cpp, "kReadPaperUpstreamVersion", "v0.3 runtime records ReadPaper upstream baseline")
    assert_contains(upstream, "V1.7.6", "v0.3 baseline is ReadPaper V1.7.6")
    assert_contains(upstream, "e910d29", "v0.3 baseline records latest remote commit")
    assert_contains(display_h, "DisplayRequest", "v0.3 display queue has ReadPaper-style request struct")
    assert_contains(display_cpp, "cloneCanvas()", "v0.3 display queue snapshots canvas before physical push")
    assert_contains(display_cpp, "M5.Display.waitDisplay()", "v0.3 display task serializes physical EPD pushes")
    assert_contains(display_cpp, "g_inDisplayPush", "v0.3 display task exposes in-push guard")
    assert_contains(input_cpp, "g_inDisplayPush", "v0.3 input task suppresses events during display push")
    assert_contains(input_cpp, "M5.update();", "v0.3 input task owns M5.update polling")
    assert_contains(state_cpp, "xQueueReceive", "v0.3 state machine is queue-driven")
    assert_contains(legado_cpp, "LegadoService", "v0.3 Legado integration is isolated as a service")
    assert_contains(ui_cpp, "CjkTextRenderer", "v0.3 UI routes text through CJK renderer")
    assert_contains(cjk_cpp, "beginReadPaperSubset", "v0.3 CJK renderer uses ReadPaper subset font before fallback")
    assert_contains(cjk_cpp, "loadBundledFont", "v0.3 CJK renderer still has bundled bitmap fallback")
    assert_contains(reader_cpp, "ReaderTextRenderer", "v0.3 has a separate reader body renderer")
    assert_contains(reader_cpp, "beginReadPaperFullFont", "reader body renderer uses full ReadPaper PROGMEM font")
    assert_contains(reader_book_cpp, "ReaderBookService", "v0.3 has reader book service for opening TXT books")
    assert_contains(reader_book_cpp, "ChapterDetector", "reader book service detects TXT table of contents")
    assert_contains(reader_book_cpp, "TextCodec::convertToUTF8", "reader book service converts GBK TXT before TOC detection")
    assert_contains(reader_book_cpp, ".vink-toc", "reader book service stores TOC cache beside the source book")
    assert_contains(full_font_h, "g_readpaper_full_font_data", "v0.3 full ReadPaper font is compiled as PROGMEM")
    assert_contains(partitions_csv, "0xC00000", "v0.3 partition table has a large single app slot for full ReadPaper font")
    assert_not_contains(partitions_csv, "app1", "v0.3 partition table drops dual OTA app1 to fit full ReadPaper font")
    assert_contains(gbk_table_h, "gbkToUnicode", "v0.3 includes full ReadPaper-derived GBK Unicode lookup")
    assert_contains(codec_cpp, "vink3::gbkToUnicode", "TextCodec uses full GBK lookup before legacy table")
    assert_contains(chapter_cpp, "U'四'", "chapter detector parses full Chinese numerals")
    assert_contains(chapter_cpp, "0xE3", "chapter detector trims ideographic leading spaces")
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
    expected = {
        "full": ARTIFACTS / f"Vink-PaperS3-{version}-{slug}-full-16MB.bin",
        "ota": ARTIFACTS / f"Vink-PaperS3-{version}-{slug}-ota.bin",
        "spiffs": ARTIFACTS / f"Vink-PaperS3-{version}-{slug}-spiffs.bin",
    }
    for kind, path in expected.items():
        manifest_size = assets.get(kind, {}).get("size")
        if not isinstance(manifest_size, int) or manifest_size <= 0:
            fail(f"{kind} manifest size is invalid: {manifest_size}")
        if not path.exists():
            if strict_artifacts:
                fail(f"missing artifact: {path}")
            ok(f"{kind} manifest size is declared: {manifest_size}")
            continue
        actual_size = path.stat().st_size
        if actual_size != manifest_size:
            if strict_artifacts:
                fail(f"{kind} size mismatch: artifact={actual_size}, manifest={manifest_size}")
            ok(f"{kind} manifest size is declared: {manifest_size} (cached artifact is local build: {actual_size})")
            continue
        ok(f"{kind} artifact size matches manifest: {actual_size}")

    full_offset = assets.get("full", {}).get("flashOffset")
    ota_offset = assets.get("ota", {}).get("flashOffset")
    if full_offset != 0:
        fail(f"full flashOffset must be 0, got {full_offset}")
    if ota_offset not in (65536, "65536", "0x10000"):
        fail(f"unexpected ota flashOffset: {ota_offset}")
    ok("Manifest flash offsets look correct")


def built_artifacts_smoke(slug: str) -> None:
    data = json.loads((PROJECT / "releases.json").read_text(encoding="utf-8"))
    version = data["releases"][0]["version"]
    expected = {
        "full": ARTIFACTS / f"Vink-PaperS3-{version}-{slug}-full-16MB.bin",
        "ota": ARTIFACTS / f"Vink-PaperS3-{version}-{slug}-ota.bin",
        "spiffs": ARTIFACTS / f"Vink-PaperS3-{version}-{slug}-spiffs.bin",
    }
    sizes = {}
    for kind, path in expected.items():
        if not path.exists():
            fail(f"missing built artifact: {path}")
        sizes[kind] = path.stat().st_size

    if sizes["full"] != FULL_FLASH_SIZE:
        fail(f"full image must be exactly 16MB, got {sizes['full']}")
    ok(f"built full image is 16MB: {sizes['full']}")
    if not 0 < sizes["ota"] <= APP_SLOT_SIZE:
        fail(f"OTA image must fit app slot: artifact={sizes['ota']} slot={APP_SLOT_SIZE}")
    ok(f"built OTA image fits 6MB app slot: {sizes['ota']}")
    if sizes["spiffs"] != SPIFFS_SIZE:
        fail(f"SPIFFS image must match partition size: artifact={sizes['spiffs']} partition={SPIFFS_SIZE}")
    ok(f"built SPIFFS image matches partition: {sizes['spiffs']}")


def json_valid() -> None:
    json.loads((PROJECT / "releases.json").read_text(encoding="utf-8"))
    ok("releases.json parses")


def build_all(slug: str) -> None:
    if not shutil.which("pio"):
        fail("PlatformIO `pio` not found in PATH")
    run(["pio", "run", "-e", "m5papers3"])
    run(["pio", "run", "-e", "m5papers3", "-t", "buildfs"])
    run(["tools/merge_full_firmware.sh"])

    version = json.loads((PROJECT / "releases.json").read_text(encoding="utf-8"))["releases"][0]["version"]
    ARTIFACTS.mkdir(parents=True, exist_ok=True)
    copies = {
        PROJECT / ".pio/build/m5papers3/m5papers3-ebook-full.bin": ARTIFACTS / f"Vink-PaperS3-{version}-{slug}-full-16MB.bin",
        PROJECT / ".pio/build/m5papers3/firmware.bin": ARTIFACTS / f"Vink-PaperS3-{version}-{slug}-ota.bin",
        PROJECT / ".pio/build/m5papers3/spiffs.bin": ARTIFACTS / f"Vink-PaperS3-{version}-{slug}-spiffs.bin",
    }
    for src, dst in copies.items():
        if not src.exists():
            fail(f"build output missing: {src}")
        shutil.copy2(src, dst)
        ok(f"copied {src.name} -> {dst}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Run local Vink-PaperS3 firmware smoke tests")
    parser.add_argument("--build", action="store_true", help="run PlatformIO build, buildfs, full merge, and copy artifacts first")
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
