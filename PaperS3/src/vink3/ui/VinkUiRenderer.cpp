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
// PaperS3 canvas: 1080×960. E-paper has 4 tones (DU4).
// Palette maps to varied grayscale that stays legible after partial refresh.
constexpr uint16_t kInk           = 0x0000;  // black – primary text & key borders
constexpr uint16_t kInkMid        = 0x630C;  // ~40% – secondary text, subtle borders
constexpr uint16_t kInkLight      = 0xB5B6;  // ~70% – dividers, disabled text
constexpr uint16_t kSurface       = 0xFFFF;  // white – page & card backgrounds
constexpr uint16_t kSurfaceAlt    = 0xF7BE;  // ~95% – alternating row bg, subtle fill
constexpr uint16_t kSurfaceDeep   = 0xE73C;  // ~90% – pressed/selected fill

// Layout
constexpr int16_t kMarginX        = 28;       // horizontal page margin
constexpr int16_t kContentW       = kPaperS3Width - kMarginX * 2;

constexpr int16_t kStatusH        = 64;       // status bar height
constexpr int16_t kStatusTextY    = 22;       // baseline for time/title/battery

constexpr int16_t kTabsY          = 80;       // tab row top
constexpr int16_t kTabsH          = 62;       // tab height
constexpr int16_t kTabW           = 128;      // tab width
constexpr int16_t kTabGap         = 12;       // gap between tabs
constexpr int16_t kTabCount       = 4;
constexpr int16_t kTabsCenter     = kPaperS3Width / 2;
constexpr int16_t kTabsLeft       = kTabsCenter - (kTabW * kTabCount + kTabGap * (kTabCount - 1)) / 2;

constexpr int16_t kContentY       = 162;      // first content row below tabs

constexpr int16_t kCardRadius     = 16;       // card corner radius
constexpr int16_t kButtonMinH     = 52;       // minimum button height
constexpr int16_t kRowH           = 58;       // standard touch row height
constexpr int16_t kSettingsPad    = 20;       // settings group internal padding

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

    g_cjkText.drawText(kMarginX, kStatusTextY, timeText, kInkMid);
    g_cjkText.drawCentered(kTabsCenter - 200, 0, 400, kStatusH - 4, title ? title : "Vink", kInk);
    g_cjkText.drawRight(kPaperS3Width - kMarginX, kStatusTextY, batteryText, kInkMid);

    // Hairline below status bar
    canvas_->drawFastHLine(kMarginX, kStatusH - 1, kContentW, kInkLight);
}

void VinkUiRenderer::drawTabs(SystemState active) {
    canvas_->setTextSize(1);
    for (int i = 0; i < 4; ++i) {
        const int16_t x = kTabsLeft + i * (kTabW + kTabGap);
        const bool selected = active == kTabs[i].state;

        if (selected) {
            // Subtle filled pill — stands out cleanly on e-paper
            canvas_->fillRoundRect(x, kTabsY + 2, kTabW, kTabsH - 4, kCardRadius, kSurfaceAlt);
        }

        canvas_->drawRoundRect(x, kTabsY, kTabW, kTabsH, kCardRadius, kInk);

        if (selected) {
            // Bold underline bar for unambiguous selection
            constexpr int16_t kUnderlineW = 48;
            const int16_t ux = x + (kTabW - kUnderlineW) / 2;
            canvas_->fillRoundRect(ux, kTabsY + kTabsH - 8, kUnderlineW, 4, 2, kInk);
        }

        g_cjkText.drawCentered(x, kTabsY + 4, kTabW, kTabsH - 8, kTabs[i].label,
                               selected ? kInk : kInkMid);
    }
    canvas_->setTextColor(kInk, kSurface);
}

void VinkUiRenderer::drawCard(int16_t x, int16_t y, int16_t w, int16_t h,
                               const char* title, const char* body) {
    // Card: clean white surface, black outline, subtle left accent line for
    // visual depth on e-paper.
    canvas_->fillRoundRect(x, y, w, h, kCardRadius, kSurface);
    canvas_->drawRoundRect(x, y, w, h, kCardRadius, kInk);
    // Left accent
    canvas_->fillRoundRect(x + 4, y + 18, 3, h - 36, 2, kInkLight);

    constexpr int16_t kPad = 22;
    if (title && title[0]) {
        g_cjkText.drawText(x + kPad + 6, y + kPad - 4, title, kInk);
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
                g_cjkText.drawText(x + kPad + 6, by, line, kInkMid);
                by += static_cast<int16_t>(g_cjkText.fontSize()) + 6;
                lines++;
            }
        }
    }
}

void VinkUiRenderer::drawButton(int16_t x, int16_t y, int16_t w, int16_t h,
                                 const char* label, bool primary) {
    const int16_t r = h / 2;  // pill-shaped button
    canvas_->fillRoundRect(x, y, w, h, r, primary ? kInk : kSurface);
    canvas_->drawRoundRect(x, y, w, h, r, kInk);
    if (label && label[0]) {
        g_cjkText.drawCentered(x, y + 2, w, h - 4, label,
                               primary ? kSurface : kInk);
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
    canvas_->fillRoundRect(x, y, kContentW, cardH, kCardRadius, kSurface);
    canvas_->drawRoundRect(x, y, kContentW, cardH, kCardRadius, kInk);

    if (title && title[0]) {
        g_cjkText.drawText(x + 22, y + 16, title, kInkMid);
    }

    const int16_t rowX = x + 22;
    const int16_t rowW = kContentW - 44;
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
    drawStatusBar("启动");

    const int16_t cx = kPaperS3Width / 2;
    const int16_t top = 260;
    const int16_t bottom = 440;
    const int16_t left = cx - 58;
    const int16_t right = cx + 58;

    // Clean hourglass mark — vector-only, no SD/SPIFFS dependency
    canvas_->drawRoundRect(cx - 90, top - 60, 180, 280, 32, kInk);
    canvas_->drawLine(left,  top,    right, top,    kInk);
    canvas_->drawLine(left,  bottom, right, bottom, kInk);
    canvas_->drawLine(left,  top,    right, bottom, kInk);
    canvas_->drawLine(right, top,    left,  bottom, kInk);
    canvas_->fillTriangle(left + 16,  top + 16,    right - 16, top + 16,    cx, top + 86,    kSurfaceAlt);
    canvas_->fillTriangle(left + 16,  bottom - 16, right - 16, bottom - 16, cx, bottom - 86, kSurfaceAlt);
    canvas_->drawFastHLine(cx - 26, top + 108, 52, kInk);

    g_cjkText.drawCentered(0, 534, kPaperS3Width, 52, "Vink 加载中", kInk);
    g_cjkText.drawCentered(0, 600, kPaperS3Width, 30, "正在准备书架与阅读器", kInkMid);
    g_cjkText.drawCentered(0, kPaperS3Height - 88, kPaperS3Width, 28,
                           kVinkPaperS3FirmwareVersion, kInkLight);
}

void VinkUiRenderer::renderHome(SystemState state) {
    renderReaderHome();
    (void)state;
}

void VinkUiRenderer::renderReaderHome(const char* bookTitle, const char* bookPath,
                                      const char* progressText, bool hasLastBook) {
    if (!canvas_) return;
    clear();
    drawStatusBar("Vink");
    drawTabs(SystemState::Reader);

    // Current book card
    drawCard(kMarginX, kContentY, kContentW, 170,
             hasLastBook ? "最近阅读" : "当前书籍", nullptr);
    const int16_t bodyX = kMarginX + 28;
    if (hasLastBook) {
        char titleLine[160];
        g_cjkText.fitTextToWidth(bookTitle && bookTitle[0] ? bookTitle : "TXT",
                                 titleLine, sizeof(titleLine), kContentW - 64);
        g_cjkText.drawText(bodyX, kContentY + 52, titleLine, kInk);
        if (progressText && progressText[0]) {
            char progLine[96];
            g_cjkText.fitTextToWidth(progressText, progLine, sizeof(progLine), kContentW - 64);
            g_cjkText.drawText(bodyX, kContentY + 86, progLine, kInkMid);
        }
        if (bookPath && bookPath[0]) {
            char pathLine[160];
            g_cjkText.fitTextToWidth(bookPath, pathLine, sizeof(pathLine), kContentW - 64);
            g_cjkText.drawText(bodyX, kContentY + 118, pathLine, kInkLight);
        }
    } else {
        g_cjkText.drawText(bodyX, kContentY + 56, "还没有最近阅读记录", kInkMid);
        g_cjkText.drawText(bodyX, kContentY + 94, "先从书架选择一本 TXT", kInkLight);
    }

    // Action buttons
    drawButton(56, 350, 192, kButtonMinH, hasLastBook ? "继续" : "打开", true);
    drawButton(296, 350, 192, kButtonMinH, "书架", false);

    // Quick-access cards row 1
    constexpr int16_t kQuickY1 = 432;
    constexpr int16_t kQuickW  = 248;
    constexpr int16_t kQuickH  = 136;
    drawCard(kMarginX,   kQuickY1, kQuickW, kQuickH, "目录", "章节浏览 / 跳页");
    drawCard(kQuickW + 44, kQuickY1, kQuickW, kQuickH, "书签", "标注 / 截图");

    // Quick-access cards row 2
    drawCard(kMarginX, 592, kContentW, 174,
             "正文设置", "字体 · 字号 · 刷新 · 简体中文");
}

void VinkUiRenderer::renderLibrary() {
    if (!canvas_) return;
    clear();
    drawStatusBar("书架");
    drawTabs(SystemState::Library);

    // Empty-state placeholder grid with refined book icons
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const int16_t x = kMarginX + 8 + col * 170;
            const int16_t y = kContentY + row * 144;
            const bool alt = (row + col) % 2 == 0;
            canvas_->fillRoundRect(x, y, 154, 118, 14, alt ? kSurfaceAlt : kSurface);
            canvas_->drawRoundRect(x, y, 154, 118, 14, kInkLight);
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
        g_cjkText.drawText(kMarginX + 6, kContentY - 24, summary, kInkMid);
    }

    const int16_t x = kMarginX;
    const int16_t w = kContentW;
    const bool plainRows = active == SystemState::Reader;
    for (int i = 0; rows && i < rowCount; ++i) {
        const int16_t y = rowY + i * rowH;
        const bool isActive = i == activeRow;
        if (!plainRows) {
            // Card-style row: subtle bg for active, clean divider between rows
            if (isActive) {
                canvas_->fillRoundRect(x, y + 2, w, rowH - 6, kCardRadius, kSurfaceAlt);
            }
            canvas_->drawRoundRect(x, y + 2, w, rowH - 6, kCardRadius, kInkLight);
        } else if (i > 0) {
            canvas_->drawFastHLine(x + 18, y - 3, w - 36, kInkLight);
        }

        char line[160];
        g_cjkText.fitTextToWidth(rows[i] ? rows[i] : "", line, sizeof(line), w - 36);
        const int16_t textX = x + 22;
        const int16_t lineH = static_cast<int16_t>(g_cjkText.fontSize());
        const int16_t textY = y + (rowH - lineH) / 2;
        g_cjkText.drawText(textX, textY, line, isActive ? kInk : kInkMid);

        if (isActive && !plainRows) {
            // Active indicator: left bar instead of underline for card-style
            canvas_->fillRoundRect(x + 4, y + 14, 4, rowH - 28, 2, kInk);
        } else if (isActive) {
            int16_t underlineW = min<int16_t>(g_cjkText.textWidth(line), w - 36);
            if (underlineW < 64) underlineW = 64;
            canvas_->fillRoundRect(textX, y + rowH - 8, underlineW, 3, 2, kInk);
        }
    }

    if (totalPages > 1) {
        char footer[48];
        snprintf(footer, sizeof(footer), "%u / %u", static_cast<unsigned>(page), static_cast<unsigned>(totalPages));
        g_cjkText.drawRight(kPaperS3Width - kMarginX, kPaperS3Height - 40, footer, kInkMid);
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

    // Button area — positioned after info, flexible if many buttons
    constexpr int16_t kBtnX = 64;
    constexpr int16_t kBtnW = 416;
    constexpr int16_t kBtnGap = 16;
    const int drawCount = min(actionCount, 8);
    static constexpr int16_t kBtnH[] = {kButtonMinH, 48, 48, 48, 48, 48, 48, 48};

    int16_t btnY = y + 24;
    const int16_t totalBtnH = drawCount > 0 ? drawCount * (kBtnH[0] + kBtnGap) - kBtnGap : 0;
    const int16_t maxBottom = kPaperS3Height - 32;
    if (drawCount > 0 && btnY + totalBtnH > maxBottom) {
        btnY = maxBottom - totalBtnH;
        if (btnY < 500) btnY = 500;
    }

    for (int i = 0; actions && i < drawCount; ++i) {
        const int16_t bh = kBtnH[min(i, (int)(sizeof(kBtnH)/sizeof(kBtnH[0]) - 1))];
        const int16_t by = btnY + i * (bh + kBtnGap);
        drawButton(kBtnX, by, kBtnW, bh, actions[i] ? actions[i] : "", i == 0);
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

    drawCard(kMarginX, kContentY, kContentW, 220, "WiFi 文件传输", ipLine);

    drawButton(56, 402, 192, kButtonMinH,
               running ? "关闭热点" : "启动热点", running);
    drawButton(296, 402, 192, kButtonMinH, "刷新状态", false);

    drawCard(kMarginX, 480, kContentW, 176,
             "WebUI 功能",
             "文件浏览器\n上传 TXT\n新建目录\n重命名 / 删除");
    drawCard(kMarginX, 680, kContentW, 168,
             "使用方式",
             "把 TXT 上传到任意 SD 目录\n设备书架直接读取文件夹");
}

void VinkUiRenderer::renderSettings() {
    if (!canvas_) return;
    clear();
    drawStatusBar("设置");
    drawTabs(SystemState::Settings);

    // Dynamic group layout — each group calculates its own height from rowCount
    static const char* kLayoutLabels[]   = {"页边距", "行间距"};
    static const char* kReadingLabels[]  = {"排版优化", "抗锯齿"};
    static const char* kDisplayLabels[]  = {"刷新策略", "翻页动画"};
    static const char* kSystemLabels[]   = {"电源", "关于"};

    const char* layoutValues[]  = {g_readerText.pageMarginLabel(),    g_readerText.lineSpacingLabel()};
    const char* readingValues[] = {g_readerText.layoutPresetLabel(),  g_readerText.antiAliasLabel()};
    const char* displayValues[] = {g_displayService.readerRefreshStrategyLabel(), g_readerText.pageTurnEffectLabel()};
    const char* systemValues[]  = {"点按关机", kVinkPaperS3FirmwareVersion};

    int16_t gy = kContentY;
    drawSettingsGroup(kMarginX, gy, "排版", kLayoutLabels,  layoutValues,  2); gy += 148;
    drawSettingsGroup(kMarginX, gy, "阅读", kReadingLabels, readingValues, 2); gy += 148;
    drawSettingsGroup(kMarginX, gy, "显示", kDisplayLabels, displayValues, 2); gy += 148;
    drawSettingsGroup(kMarginX, gy, "系统", kSystemLabels,  systemValues,  2);
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
    canvas_->drawRoundRect(408, 20, 96, 44, 14, TFT_BLACK);
    g_cjkText.drawCentered(408, 20, 96, 44, "返回", TFT_BLACK);
    canvas_->setTextSize(1);
    canvas_->drawString("rotation 0 / 540x960 / raw touch", 28, 88);
    canvas_->drawFastHLine(24, 114, kPaperS3Width - 48, TFT_BLACK);

    canvas_->drawRoundRect(24, 136, 492, 178, 14, TFT_BLACK);
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

    canvas_->drawRoundRect(24, 342, 492, 178, 14, TFT_BLACK);
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
    canvas_->drawRoundRect(24, 548, 492, 372, 14, TFT_BLACK);
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
    canvas_->fillRoundRect(36, 218, 468, 420, 24, kSurface);
    canvas_->drawRoundRect(36, 218, 468, 420, 24, kInk);
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
    canvas_->fillRoundRect(54, 300, 432, 300, 24, kSurface);
    canvas_->drawRoundRect(54, 300, 432, 300, 24, kInk);
    g_cjkText.drawCentered(54, 350, 432, 48, reason ? reason : "正在关机", kInk);
    g_cjkText.drawCentered(72, 430, 396, 32, "正在保存进度并关闭电源", kInkMid);
    g_cjkText.drawCentered(72, 482, 396, 32, "官方侧键：双击硬件关机", kInkMid);
    g_cjkText.drawCentered(0, 690, kPaperS3Width, 28, "固件内关机请从设置页点电源", kInkMid);
}

void VinkUiRenderer::renderPowerOffReady() {
    if (!canvas_) return;
    clear();
    drawStatusBar("已关机");
    canvas_->fillRoundRect(44, 246, 452, 390, 28, kSurface);
    canvas_->drawRoundRect(44, 246, 452, 390, 28, kInk);
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
            // Buttons: (56,350,192,52) and (296,350,192,52)
            if (inRect(x, y, 296, 350, 192, kButtonMinH)) return UiAction::OpenLibrary;
            if (inRect(x, y, 56, 350, 192, kButtonMinH)) return UiAction::OpenCurrentBook;
            if (inRect(x, y, kMarginX, kContentY, kContentW, 170)) return UiAction::OpenCurrentBook;
            break;
        case SystemState::Library:
            break;
        case SystemState::Transfer:
            if (inRect(x, y, 56, 402, 192, kButtonMinH)) return UiAction::ToggleWifiAp;
            if (inRect(x, y, 296, 402, 192, kButtonMinH)) return UiAction::OpenTransfer;
            break;
        case SystemState::Settings:
            // Groups at kContentY + 0,148,296,444; rows at groupY+38 + i*58
            if (inRect(x, y, 56, kContentY + 38,         424, kRowH)) return UiAction::CycleReaderPageMargin;
            if (inRect(x, y, 56, kContentY + 38 + kRowH, 424, kRowH)) return UiAction::CycleReaderLineSpacing;
            if (inRect(x, y, 56, kContentY + 148 + 38,         424, kRowH)) return UiAction::CycleReaderLayoutPreset;
            if (inRect(x, y, 56, kContentY + 148 + 38 + kRowH, 424, kRowH)) return UiAction::ToggleReaderAntiAlias;
            if (inRect(x, y, 56, kContentY + 296 + 38,         424, kRowH)) return UiAction::CycleReaderRefreshStrategy;
            if (inRect(x, y, 56, kContentY + 296 + 38 + kRowH, 424, kRowH)) return UiAction::ToggleReaderPageTurnEffect;
            if (inRect(x, y, 56, kContentY + 444 + 38,         424, kRowH)) return UiAction::RequestShutdown;
            if (y >= kContentY) return UiAction::OpenSettings;
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
