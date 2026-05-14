#!/usr/bin/env python3
"""Apply local M5GFX PaperS3 patches before PlatformIO compiles libdeps.

The PaperS3 page-turn animation needs a small extension in M5GFX's Panel_EPD
worker. PlatformIO keeps M5GFX under .pio/libdeps (gitignored), so this script
makes the source patch reproducible for clean checkouts and CI/local rebuilds.
"""
from __future__ import annotations

import subprocess
from pathlib import Path

try:  # PlatformIO/SCons injects Import; keep script runnable for diagnostics too.
    Import("env")  # type: ignore[name-defined]
except Exception:  # pragma: no cover
    env = None  # type: ignore[assignment]

if "env" in globals() and env is not None:  # type: ignore[name-defined]
    PROJECT = Path(env.subst("$PROJECT_DIR")).resolve()  # type: ignore[name-defined]
else:
    PROJECT = Path(__file__).resolve().parents[1]
PANEL_CPP = PROJECT / ".pio/libdeps/m5papers3/M5GFX/src/lgfx/v1/platforms/esp32/Panel_EPD.cpp"
PANEL_HPP = PROJECT / ".pio/libdeps/m5papers3/M5GFX/src/lgfx/v1/platforms/esp32/Panel_EPD.hpp"
PATCH = PROJECT / "patches/m5gfx-panel-epd-scroll.patch"
MARKER = "displayScroll(uint_fast16_t x, uint_fast16_t y"


def main() -> None:
    if not PATCH.exists():
        raise SystemExit(f"missing M5GFX patch: {PATCH}")
    if not PANEL_CPP.exists() or not PANEL_HPP.exists():
        # libdeps may not be installed on very early PlatformIO phases. In normal
        # project builds, dependency resolution happens before this pre-script.
        print("[vink3][patch] M5GFX Panel_EPD source not present yet; skipping")
        return
    if MARKER in PANEL_HPP.read_text(encoding="utf-8", errors="ignore"):
        print("[vink3][patch] M5GFX Panel_EPD scroll patch already applied")
        return
    print("[vink3][patch] applying M5GFX Panel_EPD scroll patch")
    subprocess.run(["patch", "-p0", "-i", str(PATCH)], cwd=str(PROJECT), check=True)


main()
