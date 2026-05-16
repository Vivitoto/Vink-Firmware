#include "EpdiyPaperS3Backend.h"

#include <algorithm>
#include <cstring>
#include <lgfx/v1/platforms/esp32/Panel_EPD.hpp>

extern "C" {
#include <epd_board.h>
#include <epd_highlevel.h>
#include <epdiy.h>
}

namespace vink3 {

EpdiyPaperS3Backend g_epdiyPaperS3Backend;

namespace {
EpdiyHighlevelState s_hl;
bool s_hlInitialized = false;

static enum EpdDrawMode epdiyModeFor(bool quality) {
    return quality ? MODE_GC16 : MODE_GL16;
}

static uint8_t buildPhysicalOffsets(int* offsets, uint16_t width, uint8_t effectSteps) {
    const int n = std::max(1, std::min(24, static_cast<int>(effectSteps) / 2));
    const int alignedStep = ((width + n * 16 - 1) / (n * 16)) * 16;
    uint8_t count = 0;
    offsets[count++] = 0;
    for (int x = alignedStep; x < width && count < 24; x += alignedStep) {
        offsets[count++] = x;
    }
    offsets[count++] = width;
    return count;
}
} // namespace

bool EpdiyPaperS3Backend::begin() {
    if (ready_) return true;

    Serial.println("[vink3][epdiy] init PaperS3 architecture backend: ED047TC1 + LCD renderer");
    // M5Unified must run first for PaperS3 board/touch/PMIC setup, but its
    // Panel_EPD worker owns the same esp_lcd i80 bus/RMT/GDMA path that epdiy
    // needs.  Stop and release it before epdiy init; otherwise two drivers
    // fight the ED047TC1 bus and the strict validation build can boot to a
    // retained/blank screen.
    if (M5.Display.getBoard() == m5gfx::board_t::board_M5PaperS3) {
        if (auto* panel = static_cast<lgfx::Panel_EPD*>(M5.Display.panel())) {
            panel->releaseBusForExternalDriver();
        }
    } else {
        Serial.println("[vink3][epdiy] pure epdiy display owner; M5GFX Panel_EPD was not initialized");
    }
    epd_init(&epd_board_m5papers3, &ED047TC1, static_cast<EpdInitOptions>(EPD_OPTIONS_DEFAULT | EPD_FEED_QUEUE_32));
    epd_set_rotation(EPD_ROT_LANDSCAPE);
    s_hl = epd_hl_init(nullptr);
    s_hlInitialized = true;
    epd_poweron();
    powered_ = true;
    ready_ = true;
    return true;
}

uint8_t EpdiyPaperS3Backend::rgb565ToGray4(uint16_t rgb565) {
    const uint8_t r5 = (rgb565 >> 11) & 0x1F;
    const uint8_t g6 = (rgb565 >> 5) & 0x3F;
    const uint8_t b5 = rgb565 & 0x1F;
    const uint16_t r = (r5 * 255u + 15u) / 31u;
    const uint16_t g = (g6 * 255u + 31u) / 63u;
    const uint16_t b = (b5 * 255u + 15u) / 31u;
    const uint16_t y = (77u * r + 150u * g + 29u * b) >> 8;
    return static_cast<uint8_t>((y * 15u + 127u) / 255u);
}

bool EpdiyPaperS3Backend::copyCanvasToFrontFramebuffer(M5Canvas* canvas) {
    if (!ready_ || !s_hlInitialized || !canvas) return false;
    if (canvas->width() != kPaperS3Width || canvas->height() != kPaperS3Height) {
        Serial.printf("[vink3][epdiy] canvas geometry mismatch: %dx%d expected %dx%d\n",
                      canvas->width(), canvas->height(), kPaperS3Width, kPaperS3Height);
        return false;
    }
    if (canvas->getColorDepth() != 16) {
        Serial.printf("[vink3][epdiy] unsupported canvas depth=%u; expected RGB565/16bpp\n",
                      canvas->getColorDepth());
        return false;
    }

    uint8_t* fb = epd_hl_get_framebuffer(&s_hl);
    if (!fb) return false;
    std::memset(fb, 0xFF, (kPaperS3PhysicalWidth / 2) * kPaperS3PhysicalHeight);

    const auto* src = static_cast<const uint16_t*>(canvas->getBuffer());
    if (!src) return false;

    // Vink renders logical portrait 540x960. ED047TC1 is native landscape
    // 960x540. This mapping matches the M5GFX PaperS3 rotation-0 portrait view:
    // logical (x,y) -> physical (y, 539-x). Keep this as the single conversion
    // boundary for the epdiy backend.
    constexpr int srcStride = kPaperS3Width;
    constexpr int dstStrideBytes = kPaperS3PhysicalWidth / 2;
    for (int y = 0; y < kPaperS3Height; ++y) {
        for (int x = 0; x < kPaperS3Width; ++x) {
            const uint16_t rgb = src[y * srcStride + x];
            const uint8_t gray = rgb565ToGray4(rgb);
            const int px = y;
            const int py = kPaperS3Width - 1 - x;
            uint8_t* cell = fb + py * dstStrideBytes + (px >> 1);
            if ((px & 1) == 0) {
                *cell = static_cast<uint8_t>((*cell & 0x0F) | (gray << 4));
            } else {
                *cell = static_cast<uint8_t>((*cell & 0xF0) | gray);
            }
        }
    }
    return true;
}

bool EpdiyPaperS3Backend::pushCanvas(M5Canvas* canvas, bool quality) {
    if (!begin()) return false;
    if (!copyCanvasToFrontFramebuffer(canvas)) return false;

    const EpdRect full = epd_full_screen();
    const enum EpdDrawError err = epd_hl_update_area(&s_hl, epdiyModeFor(quality), 21, full);
    if (err != EPD_DRAW_SUCCESS) {
        Serial.printf("[vink3][epdiy] update_area failed err=0x%x\n", static_cast<int>(err));
        return false;
    }
    return true;
}

bool EpdiyPaperS3Backend::pushPageTurn(M5Canvas* canvas, DisplayEffect effect, bool quality,
                                       uint8_t effectSteps, uint8_t residueCompensation) {
    (void)residueCompensation; // epdiy scroll uses waveform/diff state; residue tuning comes later.
    if (!begin()) return false;
    if (!copyCanvasToFrontFramebuffer(canvas)) return false;

    int offsets[25] = {0};
    const uint8_t offsetCount = buildPhysicalOffsets(offsets, kPaperS3PhysicalWidth, effectSteps);
    const int scrollCount = std::max(0, static_cast<int>(offsetCount) - 1);
    const int direction = (effect == DisplayEffect::VerticalShutter) ? 1 : 0;
    Serial.printf("[vink3][epdiy] update_area_ex mode=%s steps=%u strips=%d direction=%s\n",
                  quality ? "GC16" : "GL16", effectSteps, scrollCount, direction ? "rtl" : "ltr");

    const EpdRect full = epd_full_screen();
    const enum EpdDrawError err = epd_hl_update_area_ex(&s_hl, epdiyModeFor(quality), 21, full,
                                                        offsets, scrollCount, direction);
    if (err != EPD_DRAW_SUCCESS) {
        Serial.printf("[vink3][epdiy] update_area_ex failed err=0x%x\n", static_cast<int>(err));
        return false;
    }
    return true;
}

} // namespace vink3
