#include "VinkUiRenderer.h"
#include "../ReadPaper176.h"
#include "../display/DisplayService.h"
#include "../reader/ReaderTextRenderer.h"
#include "../sync/WifiService.h"
#include "../text/CjkTextRenderer.h"

namespace vink3 {

VinkUiRenderer g_uiRenderer;

namespace {
// ── Design system ────────────────────────────────────────────────────────────
// PaperS3 canvas: 540×960. E-paper has 4 tones.
// Keep the structure flat; use 1) rich blacks and 2) richer gray planes
// to avoid a flat/“dead” look without adding heavy imagery.
constexpr uint16_t kInk           = 0x0000;  // black – primary text & key borders
constexpr uint16_t kInkMid        = 0x4208;  // second-level text
constexpr uint16_t kInkLight      = 0x8410;  // subtle divider / mute text
constexpr uint16_t kInkWash       = 0xA514;  // slightly lighter line weight
constexpr uint16_t kSurface       = 0xFFFF;  // white – page & card backgrounds
constexpr uint16_t kSurfaceAlt    = 0xF79E;  // very light paper tone (~94%)
constexpr uint16_t kSurfaceDeep   = 0xDEDB;  // used for inactive/secondary surfaces
constexpr uint16_t kSurfacePressed= 0xCE59;  // slightly darker tone for tactile feedback

// Layout
constexpr int16_t kMarginX        = 28;       // horizontal page margin
constexpr int16_t kContentW       = kPaperS3Width - kMarginX * 2;

constexpr int16_t kStatusH        = 64;       // status bar height
constexpr int16_t kStatusTextY    = 22;       // baseline for time/title/battery

constexpr int16_t kTabsY          = 76;       // tab row top
constexpr int16_t kTabsH          = 56;       // tab height
constexpr int16_t kTabW           = 112;      // tab width (4×112+3×12=484=540-56)
constexpr int16_t kTabGap         = 12;       // gap between tabs
constexpr int16_t kTabCount       = 4;
constexpr int16_t kTabsLeft       = kMarginX; // 28px margin each side

constexpr int16_t kContentY       = 158;      // first content row below tabs

constexpr int16_t kCornerR        = 3;        // minimal corner radius – clean flat look
constexpr int16_t kButtonMinH     = 52;       // minimum button height
constexpr int16_t kRowH           = 58;       // standard touch row height
constexpr int16_t kSettingsPad    = 18;       // settings group internal padding

struct TabDef {
    SystemState state;
    UiAction action;
    const char* label;
};

constexpr TabDef kTabs[] = {
    {SystemState::Reader,   UiAction::TabReader,   "阅读"},
    {SystemState::Library,  UiAction::TabLibrary,  "书架"},
    {SystemState::Transfer, UiAction::TabTransfer, "同步"},
    {SystemState::Settings, UiAction::TabSettings, "设置"},
};

bool inRect(int16_t x, int16_t y, int16_t rx, int16_t ry, int16_t rw, int16_t rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

void formatStatusTime(char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    m5::rtc_time_t rtc;
    if (M5.Rtc.isEnabled() && M5.Rtc.getTime(&rtc) &&
        rtc.hours >= 0 && rtc.hours < 24 && rtc.minutes >= 0 && rtc.minutes < 60) {
        snprintf(out, outSize, "%02d:%02d", rtc.hours, rtc.minutes);
        return;
    }
    snprintf(out, outSize, "--:--");
}

float readOfficialBatteryVoltage() {
    // Matches M5PaperS3-UserDemo factory firmware: ADC raw * 3.5 / 4096 * 2.
    const int raw = analogRead(static_cast<int>(kBatteryAdcPin));
    if (raw <= 0) return 0.0f;
    return static_cast<float>(raw) * 3.5f / 4096.0f * 2.0f;
}

bool isOfficialUsbConnected() {
    return digitalRead(static_cast<int>(kUsbDetectPin)) == HIGH;
}

bool isOfficialChargeStateActive() {
    // Factory firmware names GPIO4 PIN_CHG_STATE: 0 charging, 1 full/not charging.
    return digitalRead(static_cast<int>(kChargeStatePin)) == LOW;
}

const char* touchCoordModeLabel() {
    return "official-raw";
}

void drawSurfacePanel(M5Canvas* canvas, int16_t x, int16_t y, int16_t w, int16_t h) {
    if (!canvas || w <= 0 || h <= 0) return;
    constexpr int16_t kThick = 2;

    canvas->fillRect(x, y, w, h, kSurfaceDeep);
    if (w > 8 && h > 8) {
        canvas->fillRect(x + 3, y + 3, w - 6, h - 6, kSurface);
        // Subtle top rule and bottom shade for richer paper-like contrast.
        canvas->drawFastHLine(x + 3, y + 3, w - 6, kInkLight);
        canvas->fillRect(x + 4, y + h - 4, w - 8, 2, kInkLight);
    }
    canvas->fillRect(x, y, w, kThick, kInk);
    canvas->fillRect(x, y + h - kThick, w, kThick, kInk);
    canvas->fillRect(x, y + kThick, kThick, h - 2 * kThick, kInk);
    canvas->fillRect(x + w - kThick, y + kThick, kThick, h - 2 * kThick, kInk);
}

void formatBatteryPercent(char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    int level = M5.Power.getBatteryLevel();
    if (level > 0 && level <= 100) {
        snprintf(out, outSize, "%s%d%%", isOfficialUsbConnected() ? "USB " : "", level);
        return;
    }
    const float voltage = readOfficialBatteryVoltage();
    if (voltage > 0.1f) {
        snprintf(out, outSize, "%s%.2fV", isOfficialUsbConnected() ? "USB " : "", voltage);
        return;
    }
    snprintf(out, outSize, "%s--%%", isOfficialUsbConnected() ? "USB " : "");
}
} // namespace

bool VinkUiRenderer::begin(M5Canvas* canvas) {
    if (!canvas) return false;
    canvas_ = canvas;
    g_cjkText.begin(canvas_);
    return true;
}

void VinkUiRenderer::clear() {
    canvas_->fillSprite(kSurface);
    canvas_->setTextColor(kInk, kSurface);
    canvas_->setTextDatum(top_left);
}

void VinkUiRenderer::drawStatusBar(const char* title) {
    canvas_->fillRect(0, 0, kPaperS3Width, kStatusH, kSurface);

    char timeText[12];
    char batteryText[12];
    formatStatusTime(timeText, sizeof(timeText));
    formatBatteryPercent(batteryText, sizeof(batteryText));

    // Time, title and battery share the exact same text top so the notification
    // bar reads as one horizontal line instead of a vertically-mixed row.
    const char* titleText = title ? title : "Vink";
    const int16_t titleX = (kPaperS3Width - g_cjkText.textWidth(titleText)) / 2;
    g_cjkText.drawText(kMarginX, kStatusTextY, timeText, kInkMid);
    g_cjkText.drawText(titleX, kStatusTextY, titleText, kInk);
    g_cjkText.drawRight(kPaperS3Width - kMarginX, kStatusTextY, batteryText, kInkMid);

    // Hairline below status bar
    canvas_->drawFastHLine(kMarginX, kStatusH - 1, kContentW, kInkLight);
}

void VinkUiRenderer::drawTabs(SystemState active) {
    for (int i = 0; i < 4; ++i) {
        const int16_t x = kTabsLeft + i * (kTabW + kTabGap);
        const bool selected = active == kTabs[i].state;

        canvas_->fillRect(x, kTabsY, kTabW, kTabsH,
                          selected ? kSurfaceAlt : kSurfaceDeep);
        // Outlines keep tabs readable on busy backgrounds.
        canvas_->drawRect(x, kTabsY, kTabW, kTabsH, selected ? kInk : kInkLight);
        // subtle top rule on every tab
        canvas_->drawFastHLine(x + 2, kTabsY + 2, kTabW - 4, kInkLight);
        if (selected) drawThickBorder(x, kTabsY, kTabW, kTabsH, kInk);

        g_cjkText.drawCentered(x, kTabsY + 4, kTabW, kTabsH - 12,
                               kTabs[i].label, selected ? kInk : kInkMid);

        if (selected) {
            // Thick underline bar
            constexpr int16_t kUnderlineW = 56;
            const int16_t ux = x + (kTabW - kUnderlineW) / 2;
            canvas_->fillRect(ux, kTabsY + kTabsH - 7, kUnderlineW, 4, kInk);
        }
    }

    // Full-width separator below tabs
    canvas_->drawFastHLine(kMarginX, kTabsY + kTabsH + 2, kContentW, kInkLight);
}

void VinkUiRenderer::drawCard(int16_t x, int16_t y, int16_t w, int16_t h,
                               const char* title, const char* body, bool smallBody) {
    // Card with a slightly warm, paper-like layered look so the block reads less flat.
    canvas_->fillRect(x, y, w, h, kSurfaceDeep);
    canvas_->fillRect(x + 3, y + 3, w - 6, h - 6, kSurface);
    drawThickBorder(x, y, w, h, kInk);

    // Fine top rule for rhythm.
    canvas_->drawFastHLine(x + 6, y + 6, w - 12, kInkLight);
    // Left accent line for depth.
    canvas_->fillRect(x + 8, y + 18, 3, h - 36, kInkLight);

    constexpr int16_t kPad = 22;
    if (title && title[0]) {
        g_cjkText.drawText(x + kPad + 8, y + 12, title, kInk);
    }
    if (body && body[0]) {
        // Wrap long body text across multiple lines within the card
        int16_t by = y + kPad + (title && title[0] ? static_cast<int16_t>(g_cjkText.fontSize()) + 12 : 0);
        const char* p = body;
        int lines = 0;
        constexpr int kMaxBodyLines = 4;
        while (*p && lines < kMaxBodyLines) {
            char line[128];
            size_t n = 0;
            while (*p && *p != '\n' && n < sizeof(line) - 1) line[n++] = *p++;
            if (*p == '\n') p++;
            line[n] = '\0';
            if (line[0]) {
                if (smallBody && g_cjkText.hasSmallFont()) g_cjkText.drawTextSmall(x + kPad + 8, by, line, kInkMid); else g_cjkText.drawText(x + kPad + 8, by, line, kInkMid);
                by += static_cast<int16_t>(g_cjkText.fontSize()) + 6;
                lines++;
            }
        }
    }
}

void VinkUiRenderer::drawThickBorder(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    constexpr int16_t kThick = 2;
    canvas_->fillRect(x, y, w, kThick, color);
    canvas_->fillRect(x, y + h - kThick, w, kThick, color);
    canvas_->fillRect(x, y + kThick, kThick, h - 2 * kThick, color);
    canvas_->fillRect(x + w - kThick, y + kThick, kThick, h - 2 * kThick, color);
}

void VinkUiRenderer::drawButton(int16_t x, int16_t y, int16_t w, int16_t h,
                                 const char* label, bool primary) {
    if (primary) {
        canvas_->fillRect(x, y, w, h, kInk);
        // Highlight strip at the top to avoid dead black block.
        canvas_->fillRect(x + 2, y + 2, w - 4, 4, kInkWash);
        g_cjkText.drawCentered(x, y + 2, w, h - 4, label ? label : "", kSurface);
    } else {
        canvas_->fillRect(x, y, w, h, kSurfaceDeep);
        canvas_->fillRect(x + 2, y + 2, w - 4, h - 4, kSurfacePressed);
        // Thin inset top line suggests pressable area.
        canvas_->drawFastHLine(x + 3, y + 3, w - 6, kInkWash);
        drawThickBorder(x, y, w, h, kInk);
        g_cjkText.drawCentered(x, y + 2, w, h - 4, label ? label : "", kInk);
    }
}

void VinkUiRenderer::drawSettingsRow(int16_t rowX, int16_t y, int16_t rowW,
                                      const char* label, const char* value) {
    // Single row: label left, value right, chevron at far right.
    // All vertically centred on the row.
    static constexpr int16_t kValueRight = 460;
    static constexpr int16_t kChevronX   = 486;
    const int16_t cy = y + kRowH / 2;
    const int16_t lineH = static_cast<int16_t>(g_cjkText.fontSize());

    g_cjkText.drawText(rowX, y + (kRowH - lineH) / 2, label ? label : "", kInk);

    if (value && value[0]) {
        g_cjkText.drawRight(kValueRight, y + (kRowH - lineH) / 2, value, kInkMid);
    }

    // Clean chevron: small right-pointing caret
    canvas_->drawLine(kChevronX,     cy - 5,
                      kChevronX + 8, cy,     kInkMid);
    canvas_->drawLine(kChevronX + 8, cy,
                      kChevronX,     cy + 5, kInkMid);
}

void VinkUiRenderer::drawSettingsGroup(int16_t x, int16_t y,
                                        const char* title,
                                        const char* const* rowLabels,
                                        const char* const* rowValues,
                                        int rowCount) {
    // Dynamic-height settings card. Title at top-left inside, then rows with
    // dividers between them. Height is calculated from rowCount.
    const int16_t cardH = kSettingsPad + 16 + rowCount * kRowH;
    drawSurfacePanel(canvas_, x, y, kContentW, cardH);

    if (title && title[0]) {
        g_cjkText.drawText(x + 22, y + 12, title, kInkMid);
        canvas_->drawFastHLine(x + 22, y + 34, kContentW - 44, kInkLight);
    }

    const int16_t rowX = x + 28;  // matches hitTest x=56 when x=kMarginX=28
    const int16_t rowW = kContentW - 56;
    for (int i = 0; i < rowCount; ++i) {
        const int16_t ry = y + 38 + i * kRowH;
        if (i > 0) {
            canvas_->drawFastHLine(rowX, ry - 2, rowW, kInkLight);
        }
        drawSettingsRow(rowX, ry, rowW,
                        rowLabels[i] ? rowLabels[i] : "",
                        rowValues[i] ? rowValues[i] : "");
    }
}


void VinkUiRenderer::renderBoot() {
    if (!canvas_) return;
    clear();

    // Keep boot plain and centered. The previous hourglass was visually noisy
    // and made the first screen feel like a debug/prototype page.
    constexpr int16_t kCenterY = kPaperS3Height / 2;
    g_cjkText.drawCentered(0, kCenterY - 54, kPaperS3Width, 52, "Vink 加载中", kInk);
    g_cjkText.drawCentered(0, kCenterY + 8, kPaperS3Width, 34, "正在准备书架与阅读器", kInkMid);
    g_cjkText.drawCentered(0, kPaperS3Height - 88, kPaperS3Width, 28,
                           kVinkPaperS3FirmwareVersion, kInkLight);
}

void VinkUiRenderer::renderHome(SystemState state) {
    renderReaderHome();
    (void)state;
}

void VinkUiRenderer::drawBookCard(int16_t x, int16_t y, int16_t w, int16_t h,
                                  const char* title, const char* subtitle, bool isEmpty) {
    // ── Book card shadow & paper layers ───────────────────────────────
    constexpr int16_t kShadowOff = 4;
    canvas_->fillRect(x + kShadowOff, y + kShadowOff, w, h, kInkLight);

    canvas_->fillRect(x, y, w, h, kSurfaceDeep);
    canvas_->fillRect(x + 3, y + 3, w - 6, h - 6, kSurface);

    // ── Book spine (left edge, proportional width) ───────────────────
    const int16_t spineW = max<int16_t>(6, w / 18);
    canvas_->fillRect(x + 1, y + 1, spineW - 1, h - 2, kInk);
    // spine highlight + binding texture
    canvas_->fillRect(x + 2, y + 10, max<int16_t>(1, spineW - 3), h - 20, kInkMid);
    for (int i = 0; i < 4; ++i) {
        const int16_t ry = y + 14 + i * (h / 5);
        canvas_->fillRect(x + 2, ry, max<int16_t>(1, spineW - 3), 1, kInkLight);
    }

    // ── Right edge: page stack lines (book-like) ──────────────────────
    constexpr int kPageLines = 3;
    const uint16_t pageColors[] = {kInkMid, kInkWash, kInkMid};
    for (int i = 0; i < kPageLines; ++i) {
        const int16_t px = x + w - 6 + i;
        canvas_->drawFastVLine(px, y + 10, h - 20, pageColors[i]);
    }

    // ── Soft edge / depth separators ─────────────────────────────────
    canvas_->drawFastHLine(x + 1, y + 1, w - 2, kInkLight);
    canvas_->fillRect(x + 4, y + h - 3, w - 8, 3, kInkLight);
    drawThickBorder(x, y, w, h, kInk);

    // ── Spine/cover divider line ─────────────────────────────────────
    canvas_->drawFastVLine(x + spineW, y, h, kInk);

    if (isEmpty) {
        g_cjkText.drawCentered(x + spineW, y, w - spineW, h, "暂无", kInkMid);
        return;
    }

    // ── Title: truly centered within the cover area, up to 3 lines ──
    const int16_t kPadX = 14;
    const int16_t bodyX = x + spineW;
    const int16_t bodyW = w - spineW;
    const int16_t textW = bodyW - kPadX * 2;
    const int16_t lineH = static_cast<int16_t>(g_cjkText.fontSize()) + 4;
    const int16_t maxLines = 3;

    // Pre-count actual title lines for accurate centering
    int actualLines = 0;
    const char* tp = title ? title : "";
    while (*tp && actualLines < maxLines) {
        int16_t accum = 0;
        while (*tp) {
            if ((static_cast<uint8_t>(*tp) & 0x80) == 0) {
                const char* asciiStart = tp;
                while (*tp && (static_cast<uint8_t>(*tp) & 0x80) == 0) tp++;
                accum += static_cast<int16_t>(tp - asciiStart) * (g_cjkText.fontSize() / 2);
                break;
            } else {
                uint8_t c = static_cast<uint8_t>(*tp);
                size_t adv = 1;
                if ((c & 0xE0) == 0xC0) adv = 2;
                else if ((c & 0xF0) == 0xE0) adv = 3;
                else if ((c & 0xF8) == 0xF0) adv = 4;
                if (accum + g_cjkText.fontSize() > textW) break;
                tp += adv;
                accum += g_cjkText.fontSize();
            }
        }
        actualLines++;
        if (actualLines >= maxLines) break;
    }

    // Center title + subtitle as a group
    const int16_t subtitleH = (subtitle && subtitle[0]) ? (lineH + 8) : 0;
    const int16_t totalContentH = actualLines * lineH + subtitleH;
    int16_t ty = y + (h - totalContentH) / 2;

    const char* p = title ? title : "";
    for (int line = 0; line < maxLines && *p; ++line) {
        char lineBuf[128];
        size_t n = 0;
        int16_t accumW = 0;
        while (*p && n < sizeof(lineBuf) - 1) {
            size_t adv = 1;
            int16_t cw = g_cjkText.fontSize();
            if ((static_cast<uint8_t>(*p) & 0x80) == 0) {
                const char* asciiStart = p;
                while (*p && (static_cast<uint8_t>(*p) & 0x80) == 0) { p++; n++; }
                size_t alen = p - asciiStart;
                accumW += static_cast<int16_t>(alen) * (g_cjkText.fontSize() / 2);
                if (n >= sizeof(lineBuf)) n = sizeof(lineBuf) - 1;
                break;
            } else {
                uint8_t c = static_cast<uint8_t>(*p);
                if ((c & 0xE0) == 0xC0) adv = 2;
                else if ((c & 0xF0) == 0xE0) adv = 3;
                else if ((c & 0xF8) == 0xF0) adv = 4;
                if (accumW + cw > textW) break;
                for (size_t k = 0; k < adv && *p && n < sizeof(lineBuf) - 1; ++k) lineBuf[n++] = *p++;
                accumW += cw;
            }
        }
        lineBuf[n] = '\0';
        if (lineBuf[0]) {
            int16_t lx = bodyX + (bodyW - accumW) / 2;
            if (lx < bodyX + kPadX) lx = bodyX + kPadX;
            g_cjkText.drawText(lx, ty, lineBuf, kInk);
            ty += lineH;
            if (line == maxLines - 1 && *p) {
                g_cjkText.drawText(bodyX + bodyW - kPadX - g_cjkText.fontSize(), ty - lineH, "…", kInkMid);
            }
        }
    }

    // ── Subtitle / progress right after title, centered ────────────
    if (subtitle && subtitle[0]) {
        if (g_cjkText.hasSmallFont()) {
            g_cjkText.drawCenteredSmall(bodyX, ty + 4, bodyW, 16, subtitle, kInkMid);
        } else {
            g_cjkText.drawCentered(bodyX, ty + 4, bodyW, g_cjkText.fontSize(), subtitle, kInkMid);
        }
    }
}

void VinkUiRenderer::renderReaderHome(const char* bookTitle, const char* bookPath,
                                      const char* progressText, bool hasLastBook,
                                      const char* const* recentTitles,
                                      const char* const* recentSubs,
                                      int recentCount) {
    if (!canvas_) return;
    clear();
    drawStatusBar("Vink");
    drawTabs(SystemState::Reader);

    (void)bookPath; // path no longer displayed directly

    // ── Top section: left large book card + right info ─────────────
    // Book card ~1:1.78 portrait ratio (natural book proportions).
    constexpr int16_t kTopY = kContentY;
    constexpr int16_t kTopH = 320;
    constexpr int16_t kCoverW = 180;
    constexpr int16_t kInfoX = kMarginX + kCoverW + 12;
    constexpr int16_t kInfoW = kContentW - kCoverW - 12;
    const int16_t lineH = static_cast<int16_t>(g_cjkText.fontSize()) + 4;

    // Left: large book card using drawBookCard (lighter + spine, book-like shape)
    if (hasLastBook) {
        drawBookCard(kMarginX, kTopY, kCoverW, kTopH,
                     bookTitle, progressText, false);
    } else {
        drawBookCard(kMarginX, kTopY, kCoverW, kTopH,
                     nullptr, nullptr, true);
    }

    // Right: info panel – clean, split-box layout (no outer frame)
    // ── "最近阅读" box, centered ──────────────────────────────────────
    {
        constexpr int16_t kLabelBoxW = 180;
        constexpr int16_t kLabelBoxH = 36;
        constexpr int16_t kLabelBoxX = kInfoX + (kInfoW - kLabelBoxW) / 2;
        constexpr int16_t kLabelBoxY = kTopY + 8;
        if (hasLastBook) {
            canvas_->fillRect(kLabelBoxX, kLabelBoxY, kLabelBoxW, kLabelBoxH, kSurface);
            drawThickBorder(kLabelBoxX, kLabelBoxY, kLabelBoxW, kLabelBoxH, kInkLight);
            g_cjkText.drawCentered(kLabelBoxX, kLabelBoxY, kLabelBoxW, kLabelBoxH, "最近阅读", kInkMid);
        } else {
            g_cjkText.drawText(kInfoX + 10, kTopY + 14, "还没有打开过书籍", kInk);
            g_cjkText.drawText(kInfoX + 10, kTopY + 44, "去书架浏览吧", kInkMid);
        }
    }

    // ── Book title with 《》 ───────────────────────────────────────────
    if (hasLastBook) {
        constexpr int16_t kTitleY = kTopY + 60;
        char titled[256];
        snprintf(titled, sizeof(titled), "《%s》", bookTitle);
        g_cjkText.fitTextToWidth(titled, titled, sizeof(titled),
                                 static_cast<int16_t>(kInfoW - 20));
        g_cjkText.drawText(kInfoX + 10, kTitleY, titled, kInk);
    }

    // ── Progress box ──────────────────────────────────────────────────
    if (hasLastBook && progressText && progressText[0]) {
        constexpr int16_t kProgBoxH = 36;
        constexpr int16_t kProgBoxY = kTopY + kTopH - kProgBoxH - 10;
        constexpr int16_t kProgBoxW = kInfoW - 20;
        constexpr int16_t kProgBoxX = kInfoX + 10;
        canvas_->fillRect(kProgBoxX, kProgBoxY, kProgBoxW, kProgBoxH, kSurface);
        drawThickBorder(kProgBoxX, kProgBoxY, kProgBoxW, kProgBoxH, kInkLight);
        g_cjkText.drawCentered(kProgBoxX, kProgBoxY, kProgBoxW, kProgBoxH, progressText, kInk);
    }

    // ── Action buttons below the top card ─────────────────────────────
    constexpr int16_t kBtnY = kTopY + kTopH + 18;
    constexpr int16_t kBtnGap = 14;
    constexpr int16_t kBtnW = (kContentW - kBtnGap * 2) / 3;
    drawButton(kMarginX, kBtnY, kBtnW, kButtonMinH, hasLastBook ? "继续" : "打开", true);
    if (hasLastBook) {
        drawButton(kMarginX + kBtnW + kBtnGap, kBtnY, kBtnW, kButtonMinH, "目录", false);
    }
    drawButton(kMarginX + (kBtnW + kBtnGap) * 2, kBtnY, kBtnW, kButtonMinH, "从头开始", false);

    // ── 3 recent book cards below, right-edge aligned ──────────────────
    constexpr int16_t kRecentY = kBtnY + kButtonMinH + 18;
    constexpr int16_t kRecentCardW = 140;
    constexpr int16_t kRecentCardH = 204;
    constexpr int16_t kRecentGap = (kContentW - kRecentCardW * 3) / 2;

    for (int i = 0; i < 3; ++i) {
        const int16_t cx = kMarginX + i * (kRecentCardW + kRecentGap);
        const bool empty = i >= recentCount || !recentTitles || !recentTitles[i];
        drawBookCard(cx, kRecentY, kRecentCardW, kRecentCardH,
                     empty ? nullptr : recentTitles[i],
                     nullptr,
                     empty);
    }
}

void VinkUiRenderer::renderShelfGrid(const char* const* titles, const char* const* subs,
                                     int bookCount, uint16_t page, uint16_t totalPages,
                                     int cols, int rows, bool showBrowserEntry) {
    if (!canvas_) return;
    clear();
    drawStatusBar("书架");
    drawTabs(SystemState::Library);

    // ── File browser entry ──────────────────────────────────────────────
    if (showBrowserEntry) {
        constexpr int16_t kEntryY = kContentY;
        constexpr int16_t kEntryH = 52;
        canvas_->fillRect(kMarginX, kEntryY, kContentW, kEntryH, kSurface);
        drawThickBorder(kMarginX, kEntryY, kContentW, kEntryH, kInk);
        g_cjkText.drawText(kMarginX + 22, kEntryY + (kEntryH - static_cast<int16_t>(g_cjkText.fontSize())) / 2,
                           "浏览书籍文件", kInk);
        g_cjkText.drawRight(kMarginX + kContentW - 22, kEntryY + (kEntryH - static_cast<int16_t>(g_cjkText.fontSize())) / 2,
                            ">", kInkMid);
    }

    // ── Book cards grid ─────────────────────────────────────────────────
    constexpr int16_t kCardW = 148;
    constexpr int16_t kCardH = 216;
    constexpr int16_t kGap = 20;
    constexpr int16_t kGridY = 228;

    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
            const int index = row * cols + col;
            const int16_t cx = kMarginX + col * (kCardW + kGap);
            const int16_t cy = kGridY + row * (kCardH + kGap);
            const bool empty = index >= bookCount || !titles || !titles[index];
            drawBookCard(cx, cy, kCardW, kCardH,
                         empty ? nullptr : titles[index],
                         empty ? nullptr : (subs ? subs[index] : nullptr),
                         empty);
        }
    }

    // ── Page indicator at bottom center ──────────────────────────────────
    // Always show page indicator at bottom center for shelf grid
    if (bookCount > 0) {
        char pageText[32];
        snprintf(pageText, sizeof(pageText), "%u / %u",
                 static_cast<unsigned>(page), static_cast<unsigned>(totalPages));
        g_cjkText.drawCentered(0, kPaperS3Height - 36, kPaperS3Width, 28, pageText, kInkMid);
    } else {
        g_cjkText.drawCentered(0, kGridY + 260, kPaperS3Width, 28, "从文件浏览器打开书籍即可加入书架", kInkLight);
    }
}

void VinkUiRenderer::renderLibrary() {
    if (!canvas_) return;
    clear();
    drawStatusBar("书架");
    drawTabs(SystemState::Library);

    // Empty-state placeholder grid with refined book icons
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const int16_t x = kMarginX + col * 165;
            const int16_t y = kContentY + row * 144;
            const bool alt = (row + col) % 2 == 0;
            canvas_->fillRect(x, y, 154, 118, alt ? kSurfaceAlt : kSurface);
            canvas_->drawRect(x, y, 154, 118, kInkLight);
            // Book icon: simple spine + pages
            const int16_t bx = x + 52;
            const int16_t by = y + 28;
            canvas_->drawFastVLine(bx,     by, 40, kInkMid);
            canvas_->drawFastVLine(bx + 8, by, 40, kInkMid);
            canvas_->drawFastHLine(bx, by,      8, kInkMid);
            canvas_->drawFastHLine(bx, by + 39, 8, kInkMid);
            g_cjkText.drawCentered(x, y + 74, 154, 22, "书籍", kInkMid);
        }
    }

    drawCard(kMarginX, 640, kContentW, 142,
             "书架来源", "SD / 最近阅读 / Legado 远程书架");
}

void VinkUiRenderer::renderUiListPage(SystemState active, const char* title, const char* summary,
                                      const char* const* rows, int rowCount, int16_t rowY, int16_t rowH,
                                      uint16_t page, uint16_t totalPages, int activeRow) {
    if (!canvas_) return;
    clear();
    drawStatusBar(title ? title : "Vink");
    drawTabs(active);
    if (summary && summary[0]) {
        g_cjkText.drawText(kMarginX + 6, kContentY + 8, summary, kInkMid);
    }

    const int16_t x = kMarginX;
    const int16_t w = kContentW;
    const bool readerList = active == SystemState::Reader;

    for (int i = 0; rows && i < rowCount; ++i) {
        const int16_t y = rowY + i * rowH;
        const bool isActive = i == activeRow;

        if (readerList) {
            if (isActive) {
                canvas_->fillRect(x, y + 2, w, rowH - 6, kSurfaceAlt);
                drawThickBorder(x, y + 2, w, rowH - 6, kInk);
            }
        } else {
            // Library-like card row: subtle bg for active, clean divider between rows.
            if (isActive) {
                canvas_->fillRect(x, y + 2, w, rowH - 6, kSurfaceAlt);
            }
            canvas_->drawRect(x, y + 2, w, rowH - 6, kInkLight);
        }

        char line[160];
        g_cjkText.fitTextToWidth(rows[i] ? rows[i] : "", line, sizeof(line), w - 36);
        const int16_t textX = x + 22;
        const int16_t lineH = static_cast<int16_t>(g_cjkText.fontSize());
        const int16_t textY = y + (rowH - lineH) / 2;
        g_cjkText.drawText(textX, textY, line, isActive ? kInk : kInkMid);

        if (isActive) {
            // Keep active-row rhythm similar to selected tab: edge accent + stronger ink.
            canvas_->fillRect(x + 4, y + 14, 4, rowH - 28, kInk);
        }
    }

    if (totalPages > 1) {
        char footer[48];
        snprintf(footer, sizeof(footer), "%u / %u", static_cast<unsigned>(page), static_cast<unsigned>(totalPages));
        if (readerList) {
            g_cjkText.drawCentered(0, kPaperS3Height - 40, kPaperS3Width, 28, footer, kInkMid);
        } else {
            g_cjkText.drawRight(kPaperS3Width - kMarginX, kPaperS3Height - 40, footer, kInkMid);
        }
    }
}

void VinkUiRenderer::renderUiActionPage(SystemState active, const char* title,
                                        const char* const* infoLines, int infoCount,
                                        const char* const* actions, int actionCount) {
    if (!canvas_) return;
    clear();
    drawStatusBar(title ? title : "操作");
    drawTabs(active);

    // Info section: auto-wrap long lines, reserve space for buttons below
    const int16_t lineH = static_cast<int16_t>(g_cjkText.fontSize()) + 8;
    const int16_t maxW = kContentW - 12;
    int16_t y = kContentY + 4;
    for (int i = 0; infoLines && i < infoCount; ++i) {
        const char* text = infoLines[i] ? infoLines[i] : "";
        size_t pos = 0;
        const size_t len = strlen(text);
        // Stop info text at ~480px so buttons always have room below
        while (pos < len && y < 480) {
            char line[160];
            // Approximate chars that fit in maxW (CJK: ~2 chars per fontSize px)
            const size_t charsPerLine = maxW / (g_cjkText.fontSize() / 2);
            size_t approx = len - pos;
            if (approx > charsPerLine) approx = charsPerLine;
            size_t end = pos + approx;
            if (end > pos) {
                size_t n = min(end - pos, sizeof(line) - 1);
                memcpy(line, text + pos, n);
                line[n] = '\0';
                const bool isFirstLine = (i == 0) && (pos == 0);
                g_cjkText.drawText(kMarginX + 6, y, line, isFirstLine ? kInk : kInkMid);
                y += lineH;
                pos = end;
            } else break;
        }
    }

    // Button area uses fixed geometry so display and touch mapping cannot drift
    // when info text wraps differently. ReaderBookService mirrors these values.
    constexpr int16_t kBtnX = 64;
    constexpr int16_t kBtnW = 416;
    constexpr int16_t kBtnH = kButtonMinH;
    constexpr int16_t kBtnGap = 16;
    constexpr int16_t kBtnY0 = 500;
    const int drawCount = min(actionCount, 6);

    for (int i = 0; actions && i < drawCount; ++i) {
        const int16_t by = kBtnY0 + i * (kBtnH + kBtnGap);
        drawButton(kBtnX, by, kBtnW, kBtnH, actions[i] ? actions[i] : "", i == 0);
    }
}

void VinkUiRenderer::renderTransfer() {
    if (!canvas_) return;
    clear();
    drawStatusBar("传输");
    drawTabs(SystemState::Transfer);

    const bool running = g_wifiService.httpServerRunning();
    char ipLine[256];
    if (running) {
        snprintf(ipLine, sizeof(ipLine),
                 "SSID: %s\n访问地址: http://%s",
                 g_wifiService.getActiveSsid(),
                 g_wifiService.getLocalIp().toString().c_str());
    } else {
        snprintf(ipLine, sizeof(ipLine),
                 "点击启动热点后\n用手机连接 Vink-PaperS3");
    }

    drawCard(kMarginX, kContentY, kContentW, 160, "WiFi 文件传输", ipLine);

    // Action area: two standalone buttons like book entry style
    constexpr int16_t kBtnY2 = 332;
    constexpr int16_t kBtnW2 = 210;
    constexpr int16_t kBtnGap2 = 16;
    constexpr int16_t kBtnPad = (kContentW - kBtnW2 * 2 - kBtnGap2) / 2;
    drawButton(kMarginX + kBtnPad, kBtnY2, kBtnW2, kButtonMinH,
               running ? "关闭热点" : "启动热点", running);
    drawButton(kMarginX + kBtnPad + kBtnW2 + kBtnGap2, kBtnY2, kBtnW2, kButtonMinH,
               "刷新状态", false);

    drawCard(kMarginX, 412, kContentW, 166,
             "WebUI 功能",
             "文件浏览器\n上传 TXT\n新建目录\n重命名 / 删除", true);
    drawCard(kMarginX, 596, kContentW, 168,
             "使用方式",
             "把 TXT 上传到任意 SD 目录\n设备书架直接读取文件夹", true);
}

void VinkUiRenderer::drawMenuItem(int16_t x, int16_t y, int16_t w, int16_t h,
                                   const char* label, bool isToggle, bool isOn, const char* altText) {
    canvas_->fillRect(x, y, w, h, kSurface);
    drawThickBorder(x, y, w, h, kInk);

    const int16_t lineH = static_cast<int16_t>(g_cjkText.fontSize());
    const int16_t textY = y + (h - lineH) / 2;
    constexpr int16_t kLabelX = 46;
    constexpr int16_t kPlainLabelX = 22;
    constexpr int16_t kValueRightPad = 20;

    if (isToggle) {
        // Filled circle = ON, hollow circle = OFF. The label always starts at a
        // fixed x, so toggling does not make the item text jump.
        constexpr int16_t kDotR = 8;
        const int16_t dotX = x + 26;
        const int16_t dotY = y + h / 2;
        if (isOn) {
            canvas_->fillCircle(dotX, dotY, kDotR, kInk);
        } else {
            canvas_->fillCircle(dotX, dotY, kDotR, kSurface);
            canvas_->drawCircle(dotX, dotY, kDotR, kInk);
        }
        g_cjkText.drawText(x + kLabelX, textY, label ? label : "", kInk);
    } else if (altText && altText[0]) {
        // Cycle item: current value on the left (prominent), config name on the right.
        g_cjkText.drawText(x + kPlainLabelX, textY, altText, kInk);
        g_cjkText.drawRight(x + w - kValueRightPad, textY, label ? label : "", kInkMid);
    } else {
        const int16_t tw = g_cjkText.textWidth(label ? label : "");
        g_cjkText.drawText(x + (w - tw) / 2, y + (h - lineH) / 2, label ? label : "", kInk);
    }
}

void VinkUiRenderer::renderReaderMenuOverlay(const char* bookTitle, const char* chapterTitle,
                                              const char* refreshLabel, bool antiAliasOn,
                                              const char* layoutLabel, bool underlineOn,
                                              bool pageTurnEffectOn) {
    if (!canvas_) return;
    drawStatusBar("阅读菜单");

    constexpr int16_t kCardX = 16;
    constexpr int16_t kCardY = 56;
    constexpr int16_t kCardW = 508;
    constexpr int16_t kCardH = 508;
    canvas_->fillRect(kCardX, kCardY, kCardW, kCardH, kSurface);
    drawThickBorder(kCardX, kCardY, kCardW, kCardH, kInk);

    constexpr int16_t kPad = 20;
    const int16_t lineH = static_cast<int16_t>(g_cjkText.fontSize());
    const int16_t kLabelX = kCardX + kPad + 2;
    const int16_t kValX   = kCardX + kCardW - kPad - 2;
    const int16_t kDivX = kLabelX;
    const int16_t kDivW = kCardW - kPad * 2 - 4;

    // Book/chapter info
    {
        char titleLine[160];
        snprintf(titleLine, sizeof(titleLine), "%s", bookTitle && bookTitle[0] ? bookTitle : "书籍");
        g_cjkText.drawText(kLabelX, kCardY + 16, titleLine, kInk);
    }
    {
        char chapLine[120];
        snprintf(chapLine, sizeof(chapLine), "%s", chapterTitle && chapterTitle[0] ? chapterTitle : "");
        if (chapLine[0])
            g_cjkText.drawText(kLabelX, kCardY + 40, chapLine, kInkMid);
    }
    canvas_->drawFastHLine(kDivX, kCardY + 64, kDivW, kInkLight);

    // ── Settings grid ────────────────────────────────────────────
    constexpr int16_t kItemW = 220;
    constexpr int16_t kItemH = 64;
    constexpr int16_t kCol0 = kCardX + 24;
    constexpr int16_t kCol1 = kCardX + 264;
    constexpr int16_t kGridY = kCardY + 76;

    drawMenuItem(kCol0, kGridY, kItemW, kItemH, "抗锯齿",   true,  antiAliasOn, nullptr);
    drawMenuItem(kCol1, kGridY, kItemW, kItemH, "翻页刷新", false, false, refreshLabel);
    drawMenuItem(kCol0, kGridY + kItemH + 4, kItemW, kItemH, "下划线",   true,  underlineOn, nullptr);
    drawMenuItem(kCol1, kGridY + kItemH + 4, kItemW, kItemH, "排版优化", false, false, layoutLabel);
    drawMenuItem(kCol0, kGridY + 2 * (kItemH + 4), kItemW, kItemH, "翻页动画", true,  pageTurnEffectOn, nullptr);
    drawMenuItem(kCol1, kGridY + 2 * (kItemH + 4), kItemW, kItemH, "页边距",   false, false, g_readerText.pageMarginLabel());

    // ── Font / size rows (same thickBorder style as grid items) ──
    constexpr int16_t kFullW = 460;
    constexpr int16_t kFullH = 64;
    int16_t fry = kGridY + 3 * kItemH + 2 * 4 + 6;

    // Font source
    canvas_->fillRect(kCol0, fry, kFullW, kFullH, kSurface);
    drawThickBorder(kCol0, fry, kFullW, kFullH, kInk);
    g_cjkText.drawText(kCol0 + 22, fry + (kFullH - lineH) / 2, "字体", kInk);
    g_cjkText.drawRight(kCol0 + kFullW - 20, fry + (kFullH - lineH) / 2, g_readerText.fontSourceLabel(), kInkMid);
    fry += kFullH + 4;

    // Font size stepper
    {
        const bool isSd = g_readerText.isSdFont();
        const uint8_t curSz = isSd ? g_readerText.sdFontSize() : g_readerText.readerFontSizeSetting();
        const bool atMin = curSz <= g_readerText.minSdFontSize();
        const bool atMax = curSz >= g_readerText.maxSdFontSize();
        canvas_->fillRect(kCol0, fry, kFullW, kFullH, kSurface);
        drawThickBorder(kCol0, fry, kFullW, kFullH, kInk);
        g_cjkText.drawText(kCol0 + 22, fry + (kFullH - lineH) / 2, "字号", kInk);
        constexpr int16_t kSegW = 64;
        const int16_t segX = kCol0 + kFullW - 20 - 5 * kSegW;
        const int16_t segY = fry + 8;
        const int16_t segH = kFullH - 16;
        for (int si = 0; si < 5; ++si) {
            const int16_t sx = segX + si * kSegW;
            const bool isBtn = (si != 2);
            const bool grayed = (si <= 1) ? (atMin || !isSd) : (si >= 3) ? (atMax || !isSd) : false;
            const uint16_t fg = grayed ? kInkLight : kInk;
            const char* lb;
            switch (si) { case 0: lb="◀◀"; break; case 1: lb="◀"; break;
                          case 3: lb="▶"; break; case 4: lb="▶▶"; break;
                          default: lb = isSd ? g_readerText.sdFontSizeLabel() : g_readerText.readerFontSizeLabel(); break; }
            if (isBtn) {
                canvas_->fillRect(sx, segY, kSegW - 4, segH, grayed ? kSurface : kSurfaceDeep);
                canvas_->drawRect(sx, segY, kSegW - 4, segH, fg);
            }
            g_cjkText.drawCentered(sx, segY, kSegW - 4, segH, lb, fg);
        }
    }
    fry += kFullH + 6;

    // ── Bottom buttons ─────────────────────────────────────────
    drawMenuItem(kCol0, fry, kItemW, kItemH, "目录", false, false, nullptr);
    drawMenuItem(kCol1, fry, kItemW, kItemH, "返回", false, false, nullptr);
}

void VinkUiRenderer::renderSettings() {
    if (!canvas_) return;
    clear();
    drawStatusBar("设置");
    drawTabs(SystemState::Settings);

    if (showReaderSettings_) {
        renderReaderSettings();
        return;
    }

    // ── Main settings page ────────────────────────────────────────────
    // "阅读设置" card → taps open sub-page
    constexpr int16_t kMainCardY = kContentY;
    constexpr int16_t kMainCardH = 52;
    drawSurfacePanel(canvas_, kMarginX, kMainCardY, kContentW, kMainCardH);
    g_cjkText.drawText(kMarginX + 22, kMainCardY + (kMainCardH - static_cast<int16_t>(g_cjkText.fontSize())) / 2,
                       "阅读设置", kInk);
    // Chevron >
    g_cjkText.drawRight(kMarginX + kContentW - 22, kMainCardY + (kMainCardH - static_cast<int16_t>(g_cjkText.fontSize())) / 2,
                        ">", kInkMid);

    // System group at bottom
    static const char* kSysLabels[] = {"电源", "关于"};
    const char* sysValues[] = {"点按关机", kVinkPaperS3FirmwareVersion};
    drawSettingsGroup(kMarginX, kMainCardY + kMainCardH + 24, "系统", kSysLabels, sysValues, 2);
}

void VinkUiRenderer::showReaderSettings()  { showReaderSettings_ = true; }
void VinkUiRenderer::hideReaderSettings() { showReaderSettings_ = false; }

void VinkUiRenderer::renderReaderSettings() {
    // ── Back button ───────────────────────────────────────────────────
    constexpr int16_t kBackY = kContentY;
    constexpr int16_t kBackH = 32;
    drawSurfacePanel(canvas_, kMarginX, kBackY, 120, kBackH);
    g_cjkText.drawText(kMarginX + 6, kBackY, "< 返回", kInkMid);

    // Determine label strings
    const char* fontSourceVal = g_readerText.fontSourceLabel();
    const char* fontSzLabel = g_readerText.isSdFont() ? g_readerText.sdFontSizeLabel() : g_readerText.readerFontSizeLabel();
    const char* pageMarginVal = g_readerText.pageMarginLabel();
    const char* lineSpacingVal = g_readerText.lineSpacingLabel();
    const char* layoutVal = g_readerText.layoutPresetLabel();
    const char* antiAliasVal = g_readerText.antiAliasLabel();
    const char* refreshVal = g_displayService.readerRefreshStrategyLabel();
    const char* pageTurnVal = g_readerText.pageTurnEffectLabel();

    const int16_t lineH = static_cast<int16_t>(g_cjkText.fontSize());
    const int16_t kRH = 46;
    const int16_t kSH = 52;
    const int16_t kCardX = kMarginX;
    const int16_t kCardW = kContentW;
    const int16_t kLabelX = kCardX + 22;
    const int16_t kValX = kCardX + kCardW - 22;
    const int16_t kDivX = kLabelX;
    const int16_t kDivW = kCardW - 44;
    const int16_t kGG = 14;

    auto cardRow = [&](int16_t ry, const char* l, const char* v) {
        g_cjkText.drawText(kLabelX, ry + (kRH - lineH) / 2, l, kInk);
        g_cjkText.drawRight(kValX, ry + (kRH - lineH) / 2, v, kInkMid);
    };
    auto cardDiv = [&](int16_t y) { canvas_->drawFastHLine(kDivX, y, kDivW, kInkLight); };

    g_cjkText.drawText(kMarginX + 6, kBackY, "< 返回", kInkMid);
    int16_t gy = kBackY + 34 + 12;

    // ══════ 排版 ══════
    {
        const int16_t cardH = 230;
        drawSurfacePanel(canvas_, kCardX, gy, kCardW, cardH);
        g_cjkText.drawText(kLabelX, gy + 14, "排版", kInkMid);
        canvas_->drawFastHLine(kLabelX, gy + 38, kDivW, kInkLight);
        int16_t ry = gy + 42;
        cardRow(ry, "字体", fontSourceVal);
        ry += kRH; cardDiv(ry);
        {
            const bool isSd = g_readerText.isSdFont();
            const uint8_t curSz = isSd ? g_readerText.sdFontSize() : g_readerText.readerFontSizeSetting();
            const bool atMin = curSz <= g_readerText.minSdFontSize();
            const bool atMax = curSz >= g_readerText.maxSdFontSize();
            g_cjkText.drawText(kLabelX, ry + (kSH - lineH) / 2, "字号", kInk);
            constexpr int16_t kSegW = 60;
            const int16_t segX = kValX - 5 * kSegW;
            const int16_t segY = ry + 6;
            const int16_t segH = kSH - 12;
            for (int si = 0; si < 5; ++si) {
                const int16_t sx = segX + si * kSegW;
                const bool isBtn = (si != 2);
                const bool grayed = (si <= 1) ? (atMin || !isSd) : (si >= 3) ? (atMax || !isSd) : false;
                const uint16_t fg = grayed ? kInkLight : kInk;
                const char* lb;
                switch (si) { case 0: lb="◀◀"; break; case 1: lb="◀"; break;
                              case 3: lb="▶"; break; case 4: lb="▶▶"; break;
                              default: lb = isSd ? g_readerText.sdFontSizeLabel() : g_readerText.readerFontSizeLabel(); break; }
                if (isBtn) {
                    canvas_->fillRect(sx, segY, kSegW - 4, segH, grayed ? kSurface : kSurfaceDeep);
                    canvas_->drawRect(sx, segY, kSegW - 4, segH, fg);
                }
                g_cjkText.drawCentered(sx, segY, kSegW - 4, segH, lb, fg);
            }
        }
        ry += kSH; cardDiv(ry);
        cardRow(ry, "页边距", pageMarginVal);
        ry += kRH; cardDiv(ry);
        cardRow(ry, "行间距", lineSpacingVal);
        gy += cardH + kGG;
    }

    // ══════ 阅读 ══════
    {
        const int16_t cardH = 130;
        drawSurfacePanel(canvas_, kCardX, gy, kCardW, cardH);
        g_cjkText.drawText(kLabelX, gy + 14, "阅读", kInkMid);
        canvas_->drawFastHLine(kLabelX, gy + 38, kDivW, kInkLight);
        int16_t ry = gy + 42;
        cardRow(ry, "排版优化", layoutVal);
        ry += kRH; cardDiv(ry);
        cardRow(ry, "抗锯齿", antiAliasVal);
        gy += cardH + kGG;
    }

    // ══════ 显示 ══════
    {
        const int16_t cardH = 130;
        drawSurfacePanel(canvas_, kCardX, gy, kCardW, cardH);
        g_cjkText.drawText(kLabelX, gy + 14, "显示", kInkMid);
        canvas_->drawFastHLine(kLabelX, gy + 38, kDivW, kInkLight);
        int16_t ry = gy + 42;
        cardRow(ry, "刷新策略", refreshVal);
        ry += kRH; cardDiv(ry);
        cardRow(ry, "翻页动画", pageTurnVal);
    }
}

void VinkUiRenderer::renderDiagnostics(const Message& lastTouch, const char* eventName) {
    if (!canvas_) return;
    clear();

    char line[128];
    canvas_->setTextColor(TFT_BLACK, TFT_WHITE);
    canvas_->setTextDatum(top_left);
    canvas_->setTextSize(2);
    canvas_->drawString("VINK DIAGNOSTIC", 24, 22);
    canvas_->drawString("OFFICIAL PORTRAIT", 24, 52);
    canvas_->drawRect(408, 20, 96, 44, TFT_BLACK);
    g_cjkText.drawCentered(408, 20, 96, 44, "返回", TFT_BLACK);
    canvas_->setTextSize(1);
    canvas_->drawString("rotation 0 / 540x960 / raw touch", 28, 88);
    canvas_->drawFastHLine(24, 114, kPaperS3Width - 48, TFT_BLACK);

    canvas_->drawRect(24, 136, 492, 178, TFT_BLACK);
    canvas_->setTextSize(2);
    canvas_->drawString("DISPLAY", 48, 158);
    canvas_->setTextSize(1);
    snprintf(line, sizeof(line), "rotation=%u logical=%dx%d", gPaperS3ActiveDisplayRotation, kPaperS3Width, kPaperS3Height);
    canvas_->drawString(line, 48, 198);
    snprintf(line, sizeof(line), "M5.Display=%dx%d", M5.Display.width(), M5.Display.height());
    canvas_->drawString(line, 48, 226);
    snprintf(line, sizeof(line), "USB:%s CHG:%s BAT:%.2fV", isOfficialUsbConnected() ? "IN" : "--", isOfficialChargeStateActive() ? "ON" : "--", readOfficialBatteryVoltage());
    canvas_->drawString(line, 48, 254);
    canvas_->drawString("If visible: Vink canvas takeover works", 48, 282);

    canvas_->drawRect(24, 342, 492, 178, TFT_BLACK);
    canvas_->setTextSize(2);
    canvas_->drawString("TOUCH RAW", 48, 364);
    canvas_->setTextSize(1);
    snprintf(line, sizeof(line), "event: %s  count:%ld", eventName ? eventName : "wait", static_cast<long>(lastTouch.value));
    canvas_->drawString(line, 48, 404);
    snprintf(line, sizeof(line), "raw: %d, %d", lastTouch.rawTouch.x, lastTouch.rawTouch.y);
    canvas_->drawString(line, 48, 436);
    snprintf(line, sizeof(line), "norm: %d, %d", lastTouch.touch.x, lastTouch.touch.y);
    canvas_->drawString(line, 48, 468);
    canvas_->drawString("Touch: dot should match your finger", 48, 496);

    const int16_t gx = 54;
    const int16_t gy = 580;
    const int16_t gw = 432;
    const int16_t gh = 300;
    canvas_->drawRect(24, 548, 492, 372, TFT_BLACK);
    canvas_->setTextSize(2);
    canvas_->drawString("3x3 HIT GRID", 54, 566);
    canvas_->drawRect(gx, gy + 36, gw, gh, TFT_BLACK);
    canvas_->drawFastVLine(gx + gw / 3, gy + 36, gh, TFT_BLACK);
    canvas_->drawFastVLine(gx + gw * 2 / 3, gy + 36, gh, TFT_BLACK);
    canvas_->drawFastHLine(gx, gy + 36 + gh / 3, gw, TFT_BLACK);
    canvas_->drawFastHLine(gx, gy + 36 + gh * 2 / 3, gw, TFT_BLACK);
    canvas_->setTextSize(1);
    canvas_->drawString("TOP", gx + gw / 2 - 12, gy + 48);
    canvas_->drawString("LEFT", gx + 12, gy + 36 + gh / 2);
    canvas_->drawString("RIGHT", gx + gw - 52, gy + 36 + gh / 2);
    canvas_->drawString("BOTTOM", gx + gw / 2 - 22, gy + 36 + gh - 20);

    if (lastTouch.timestampMs != 0) {
        const int16_t px = gx + (static_cast<int32_t>(lastTouch.touch.x) * gw) / kPaperS3Width;
        const int16_t py = gy + 36 + (static_cast<int32_t>(lastTouch.touch.y) * gh) / kPaperS3Height;
        canvas_->fillCircle(px, py, 10, TFT_BLACK);
        canvas_->drawCircle(px, py, 20, TFT_BLACK);
    }

    canvas_->setTextSize(1);
}

void VinkUiRenderer::renderShutdownConfirm() {
    if (!canvas_) return;
    clear();
    drawStatusBar("关机确认");
    drawTabs(SystemState::Settings);
    canvas_->fillRect(36, 218, 468, 420, kSurface);
    drawThickBorder(36, 218, 468, 420, kInk);
    g_cjkText.drawCentered(60, 270, 420, 44, "确认关闭电源？", kInk);
    g_cjkText.drawCentered(70, 344, 400, 30, "会先保存当前阅读进度", kInkMid);
    g_cjkText.drawCentered(70, 386, 400, 30, "然后调用 M5.Power.powerOff()", kInkMid);
    g_cjkText.drawCentered(70, 428, 400, 30, "侧边键双击仍保留硬件关机", kInkMid);
    drawButton(64, 530, 180, 56, "取消", false);
    drawButton(296, 530, 180, 56, "确认关机", true);
}

void VinkUiRenderer::renderShutdown(const char* reason) {
    if (!canvas_) return;
    clear();
    drawStatusBar("关机");
    canvas_->fillRect(54, 300, 432, 300, kSurface);
    drawThickBorder(54, 300, 432, 300, kInk);
    g_cjkText.drawCentered(54, 350, 432, 48, reason ? reason : "正在关机", kInk);
    g_cjkText.drawCentered(72, 430, 396, 32, "正在保存进度并关闭电源", kInkMid);
    g_cjkText.drawCentered(72, 482, 396, 32, "官方侧键：双击硬件关机", kInkMid);
    g_cjkText.drawCentered(0, 690, kPaperS3Width, 28, "固件内关机请从设置页点电源", kInkMid);
}

void VinkUiRenderer::renderPowerOffReady() {
    if (!canvas_) return;
    clear();
    drawStatusBar("已关机");
    canvas_->fillRect(44, 246, 452, 390, kSurface);
    drawThickBorder(44, 246, 452, 390, kInk);
    g_cjkText.drawCentered(64, 306, 412, 52, "Vink 已关机", kInk);
    g_cjkText.drawCentered(76, 398, 388, 32, "阅读进度已保存", kInkMid);
    g_cjkText.drawCentered(76, 448, 388, 32, "屏幕会保留此页面", kInkMid);
    g_cjkText.drawCentered(76, 498, 388, 32, "单击侧边键开机", kInkMid);
    g_cjkText.drawCentered(0, 710, kPaperS3Width, 28, "后续可替换为 SD 卡关机图片", kInkMid);
}

UiAction VinkUiRenderer::hitTestTabs(int16_t x, int16_t y) const {
    if (y < kTabsY || y >= kTabsY + kTabsH) return UiAction::None;
    for (int i = 0; i < 4; ++i) {
        const int16_t tabX = kTabsLeft + i * (kTabW + kTabGap);
        if (inRect(x, y, tabX, kTabsY, kTabW, kTabsH)) return kTabs[i].action;
    }
    return UiAction::None;
}

UiAction VinkUiRenderer::hitTest(SystemState state, int16_t x, int16_t y) const {
    UiAction tab = hitTestTabs(x, y);
    if (tab != UiAction::None) return tab;

    switch (state) {
        case SystemState::Reader:
        case SystemState::Home:
            // Reader home geometry must mirror renderReaderHome() pixel-for-pixel.
            {
                constexpr int16_t kTopCardY = kContentY;
                constexpr int16_t kTopCardW = 180;
                constexpr int16_t kTopCardH = 320;
                constexpr int16_t kBtnY = kTopCardY + kTopCardH + 18;
                constexpr int16_t kBtnGap = 14;
                constexpr int16_t kBtnW = (kContentW - kBtnGap * 2) / 3;

                // Top card → open
                if (inRect(x, y, kMarginX, kTopCardY, kTopCardW, kTopCardH)) return UiAction::OpenCurrentBook;
                // "继续/打开" button
                if (inRect(x, y, kMarginX, kBtnY, kBtnW, kButtonMinH)) return UiAction::OpenCurrentBook;
                // "目录" button
                if (inRect(x, y, kMarginX + kBtnW + kBtnGap, kBtnY, kBtnW, kButtonMinH)) return UiAction::OpenCurrentBookToc;
                // "从头开始" button (kBtnY recalculated below with new kTopH)
                if (inRect(x, y, kMarginX + (kBtnW + kBtnGap) * 2, kBtnY, kBtnW, kButtonMinH)) return UiAction::RestartCurrentBook;
                // 3 recent book cards → handled by handleReaderHomeTap (touch coords passed via None)
            }
            break;
        case SystemState::Library:
            break;
        case SystemState::Transfer:
            if (inRect(x, y, 52, 332, 210, kButtonMinH)) return UiAction::ToggleWifiAp;
            if (inRect(x, y, 278, 332, 210, kButtonMinH)) return UiAction::OpenTransfer;
            break;
        case SystemState::Settings:
            if (showReaderSettings_) {
                if (inRect(x, y, kMarginX, kContentY, 120, 32)) return UiAction::BackToSettings;
                // Layout: back+34+12=204; cards at 204, 204+230+14=448, 448+130+14=592
                const int16_t g0 = kContentY + 34 + 12;
                {
                    // 排版: cardH=230, rows at g0+42, +kRH, +kSH
                    int16_t ry = g0 + 42;
                    if (inRect(x, y, kMarginX + 22, ry, kContentW - 44, 46)) return UiAction::CycleReaderFontSource;
                    ry += 46;
                    // Stepper right-aligned
                    const int16_t segX = kMarginX + kContentW - 22 - 5 * 60;
                    const int16_t segY = ry + 6;
                    constexpr int16_t kW = 52;
                    if (inRect(x, y, segX,              segY, kW, 40)) return UiAction::DecreaseSdFontSizeBig;
                    if (inRect(x, y, segX + 60,         segY, kW, 40)) return UiAction::DecreaseSdFontSize;
                    if (inRect(x, y, segX + 120,        segY, kW, 40)) return UiAction::CycleReaderFontSize;
                    if (inRect(x, y, segX + 180,        segY, kW, 40)) return UiAction::IncreaseSdFontSize;
                    if (inRect(x, y, segX + 240,        segY, kW, 40)) return UiAction::IncreaseSdFontSizeBig;
                    // 页边距 + 行间距
                    ry += 52;
                    if (inRect(x, y, kMarginX + 22, ry, kContentW - 44, 46)) return UiAction::CycleReaderPageMargin;
                    ry += 46;
                    if (inRect(x, y, kMarginX + 22, ry, kContentW - 44, 46)) return UiAction::CycleReaderLineSpacing;
                }
                {
                    // 阅读: g0+244
                    int16_t ry = g0 + 244 + 42;
                    if (inRect(x, y, kMarginX + 22, ry, kContentW - 44, 46)) return UiAction::CycleReaderLayoutPreset;
                    ry += 46;
                    if (inRect(x, y, kMarginX + 22, ry, kContentW - 44, 46)) return UiAction::ToggleReaderAntiAlias;
                }
                {
                    // 显示: g0+244+144
                    int16_t ry = g0 + 388 + 42;
                    if (inRect(x, y, kMarginX + 22, ry, kContentW - 44, 46)) return UiAction::CycleReaderRefreshStrategy;
                    ry += 46;
                    if (inRect(x, y, kMarginX + 22, ry, kContentW - 44, 46)) return UiAction::ToggleReaderPageTurnEffect;
                }
                break;
            }
            // Main page: "阅读设置" card + system group
            if (inRect(x, y, kMarginX, kContentY, kContentW, 52)) return UiAction::OpenReaderSettings;
            if (inRect(x, y, 56, kContentY + 52 + 24 + 38,         424, kRowH)) return UiAction::RequestShutdown;
            if (inRect(x, y, 56, kContentY + 52 + 24 + 38 + kRowH, 424, kRowH)) return UiAction::OpenSettings;
            break;
        case SystemState::ShutdownConfirm:
            if (inRect(x, y, 64, 530, 180, 56)) return UiAction::CancelShutdown;
            if (inRect(x, y, 296, 530, 180, 56)) return UiAction::ConfirmShutdown;
            break;
        case SystemState::Diagnostics:
            if (inRect(x, y, 408, 20, 96, 44)) return UiAction::TabSettings;
            if (y >= 316) return UiAction::OpenDiagnostics;
            break;
        default:
            break;
    }
    return UiAction::None;
}

} // namespace vink3
