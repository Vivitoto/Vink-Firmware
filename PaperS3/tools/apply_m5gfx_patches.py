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
MARKER = "releaseBusForExternalDriver(void)"


def main() -> None:
    if not PATCH.exists():
        raise SystemExit(f"missing M5GFX patch: {PATCH}")
    if not PANEL_CPP.exists() or not PANEL_HPP.exists():
        # libdeps may not be installed on very early PlatformIO phases. In normal
        # project builds, dependency resolution happens before this pre-script.
        print("[vink3][patch] M5GFX Panel_EPD source not present yet; skipping")
        return
    panel_h_text = PANEL_HPP.read_text(encoding="utf-8", errors="ignore")
    if MARKER in panel_h_text:
        print("[vink3][patch] M5GFX Panel_EPD scroll/release patch already applied")
        return
    if "displayScroll(uint_fast16_t x, uint_fast16_t y" not in panel_h_text:
        print("[vink3][patch] applying M5GFX Panel_EPD scroll patch")
        subprocess.run(["patch", "-p0", "-i", str(PATCH)], cwd=str(PROJECT), check=True)
    apply_recovery_patch()


def replace_once(path: Path, old: str, new: str, label: str) -> None:
    text = path.read_text(encoding="utf-8", errors="ignore")
    if new in text:
        return
    if old not in text:
        raise SystemExit(f"[vink3][patch] cannot apply {label}: expected source block missing in {path}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


def apply_recovery_patch() -> None:
    bus_h = PROJECT / ".pio/libdeps/m5papers3/M5GFX/src/lgfx/v1/platforms/esp32/Bus_EPD.h"
    bus_cpp = PROJECT / ".pio/libdeps/m5papers3/M5GFX/src/lgfx/v1/platforms/esp32/Bus_EPD.cpp"

    replace_once(bus_h,
                 "  bool init(void) override;\n\n  bool busy(void) const override { return _bus_busy; }",
                 "  bool init(void) override;\n  void release(void) override;\n\n  bool busy(void) const override { return _bus_busy; }",
                 "Bus_EPD release declaration")
    replace_once(bus_cpp,
                 "#include <esp_lcd_panel_ops.h>\n",
                 "#include <esp_lcd_panel_ops.h>\n#include <esp_lcd_panel_io.h>\n",
                 "Bus_EPD panel_io include")
    replace_once(bus_cpp,
                 "bool Bus_EPD::init(void)\n{\n  _bus_busy = false;\n  _pwr_on = false;\n",
                 "void Bus_EPD::release(void)\n{\n  wait();\n  powerControl(false);\n  wait();\n  if (_io_handle) {\n    esp_lcd_panel_io_del(_io_handle);\n    _io_handle = nullptr;\n  }\n  if (_i80_bus_handle) {\n    esp_lcd_del_i80_bus(_i80_bus_handle);\n    _i80_bus_handle = nullptr;\n  }\n  _bus_busy = false;\n  _pwr_on = false;\n}\n\nbool Bus_EPD::init(void)\n{\n  _bus_busy = false;\n  _pwr_on = false;\n",
                 "Bus_EPD release implementation")

    replace_once(PANEL_HPP,
                 "    void displayScroll(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h,\n                       uint_fast16_t strip_width, bool reverse, uint8_t compensation = 1,\n                       uint8_t effect_steps = 0);\n\n    void writeFillRectPreclipped",
                 "    void displayScroll(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h,\n                       uint_fast16_t strip_width, bool reverse, uint8_t compensation = 1,\n                       uint8_t effect_steps = 0);\n    // Vink PaperS3 epdiy bring-up: M5Unified initializes Panel_EPD first for\n    // board/touch/PMIC detection.  A real epdiy backend must then stop this\n    // worker and release the esp_lcd i80 bus before epdiy configures LCD/RMT/GDMA.\n    void releaseBusForExternalDriver(void);\n\n    void writeFillRectPreclipped",
                 "Panel_EPD external-driver release declaration")
    replace_once(PANEL_CPP,
                 "    return true;\n  }\n\n  void Panel_EPD::beginTransaction(void)\n",
                 "    return true;\n  }\n\n  void Panel_EPD::releaseBusForExternalDriver(void)\n  {\n    waitDisplay();\n    _display_busy = false;\n    if (_task_update_handle) {\n      vTaskDelete(_task_update_handle);\n      _task_update_handle = nullptr;\n    }\n    if (_update_queue_handle) {\n      vQueueDelete(_update_queue_handle);\n      _update_queue_handle = nullptr;\n    }\n    auto bus = getBusEPD();\n    if (bus) {\n      bus->release();\n      printf(\"[vink3][epd] M5GFX Panel_EPD bus released for external driver\\\\n\");\n    }\n  }\n\n  void Panel_EPD::beginTransaction(void)\n",
                 "Panel_EPD external-driver release implementation")
    replace_once(PANEL_CPP,
                 "      const int signed_span = reverse ? -span : span;\n      const float ratio = (float)local_y / (float)height;\n      int off = (int)((ratio - 0.5f) * (float)signed_span);\n      if (off < min_off) { off = min_off; }\n      if (off > max_off) { off = max_off; }\n      return (int16_t)off;\n",
                 "      (void)local_y;\n      (void)height;\n      (void)width;\n      (void)n_steps;\n      (void)lead;\n      (void)reverse;\n      (void)min_off;\n      (void)max_off;\n      (void)span;\n      // v0.4.38 real-device feedback showed the row-source offset creates a\n      // visible diagonal wavefront.  Keep the EDCBook-style bucket reveal, but\n      // make the first recovery build strictly vertical so scan geometry can be\n      // validated independently from any future phase-level row-offset work.\n      return 0;\n",
                 "Panel_EPD straight wavefront row offset")
    replace_once(PANEL_CPP,
                 "          const uint8_t active_frames = new_data.scroll_effect_steps >= 24 ? 3 : 2;\n",
                 "          const uint8_t active_frames = new_data.scroll_effect_steps >= 24 ? 4 : 3;\n",
                 "Panel_EPD recovery active frame count")
    print("[vink3][patch] M5GFX Panel_EPD recovery release/straight-wavefront patch applied")


main()
