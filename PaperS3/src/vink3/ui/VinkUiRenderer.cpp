#include "VinkUiRenderer.h"
#include "../ReadPaper176.h"
#include "../text/CjkTextRenderer.h"

namespace vink3 {

VinkUiRenderer g_uiRenderer;

namespace {
constexpr int16_t kMargin = 24;
constexpr int16_t kStatusH = 62;
constexpr int16_t kTabsY = 76;
constexpr int16_t kTabsH = 60;
constexpr int16_t kContentY = 154;
constexpr int16_t kTabW = 120;
constexpr int16_t kTabGap = 8;
constexpr int16_t kTabX0 = 18;
constexpr uint16_t kGrayLight = 0xEF7D; // ~#EFEFEF
constexpr uint16_t kGrayMid = 0xD69A;   // ~#D6D6D6
constexpr uint16_t kGrayText = 0x8410;  // ~#888888

struct TabDef {
    SystemState state;
    UiAction action;
    const char* label;
};

constexpr TabDef kTabs[] = {
    {SystemState::Reader, UiAction::TabReader, "阅读"},
    {SystemState::Library, UiAction::TabLibrary, "书架"},
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
    canvas_->fillSprite(TFT_WHITE);
    canvas_->setTextColor(TFT_BLACK, TFT_WHITE);
    canvas_->setTextDatum(top_left);
}

void VinkUiRenderer::drawStatusBar(const char* title) {
    canvas_->fillRect(0, 0, kPaperS3Width, kStatusH, TFT_WHITE);
    canvas_->drawFastHLine(kMargin, kStatusH - 1, kPaperS3Width - kMargin * 2, TFT_BLACK);

    char timeText[12];
    char batteryText[12];
    formatStatusTime(timeText, sizeof(timeText));
    formatBatteryPercent(batteryText, sizeof(batteryText));

    // Status bar corners are reserved for live device state: system time on the
    // left, battery percentage on the right. Page title stays centered so it no
    // longer competes with the notification/status content.
    g_cjkText.drawText(kMargin, 20, timeText, kGrayText);
    g_cjkText.drawCentered(112, 20, 316, 26, title ? title : "Vink", TFT_BLACK);
    g_cjkText.drawRight(kPaperS3Width - kMargin, 20, batteryText, kGrayText);
}

void VinkUiRenderer::drawTabs(SystemState active) {
    canvas_->setTextSize(1);
    for (int i = 0; i < 4; ++i) {
        const int16_t x = kTabX0 + i * (kTabW + kTabGap);
        const bool selected = active == kTabs[i].state;
        canvas_->fillRoundRect(x, kTabsY, kTabW, kTabsH, 16, TFT_WHITE);
        canvas_->drawRoundRect(x, kTabsY, kTabW, kTabsH, 16, TFT_BLACK);
        if (selected) {
            // Avoid filled black tabs: on real PaperS3 the white CJK label can
            // look swallowed after partial refresh/photo compression. Use a
            // strong outline + underline so the text always remains black.
            canvas_->drawRoundRect(x + 2, kTabsY + 2, kTabW - 4, kTabsH - 4, 14, TFT_BLACK);
            canvas_->fillRoundRect(x + 28, kTabsY + kTabsH - 9, kTabW - 56, 4, 2, TFT_BLACK);
        }
        canvas_->setTextColor(TFT_BLACK, TFT_WHITE);
        g_cjkText.drawCentered(x, kTabsY, kTabW, kTabsH - 4, kTabs[i].label, TFT_BLACK);
    }
    canvas_->setTextColor(TFT_BLACK, TFT_WHITE);
}

void VinkUiRenderer::drawCard(int16_t x, int16_t y, int16_t w, int16_t h, const char* title, const char* body) {
    // Keep Vink's shell visually calm: white cards, black frame, consistent
    // 22px inner padding. Avoid alternating gray blocks that make PaperS3 look
    // like misaligned boxes after partial refreshes.
    canvas_->fillRoundRect(x, y, w, h, 18, TFT_WHITE);
    canvas_->drawRoundRect(x, y, w, h, 18, TFT_BLACK);
    g_cjkText.drawText(x + 22, y + 18, title ? title : "", TFT_BLACK);
    if (body && body[0]) {
        g_cjkText.drawText(x + 22, y + 58, body, kGrayText);
    }
}

void VinkUiRenderer::drawButton(int16_t x, int16_t y, int16_t w, int16_t h, const char* label) {
    canvas_->fillRoundRect(x, y, w, h, h / 2, TFT_WHITE);
    canvas_->drawRoundRect(x, y, w, h, h / 2, TFT_BLACK);
    g_cjkText.drawCentered(x, y, w, h, label ? label : "", TFT_BLACK);
}

void VinkUiRenderer::drawSettingsRow(int16_t y, const char* label, const char* value) {
    static constexpr int16_t kRowX = 56;
    static constexpr int16_t kValueRight = 448;
    static constexpr int16_t kArrowX = 474;
    static constexpr int16_t kRowH = 60;
    const int16_t cy = y + kRowH / 2;
    const int16_t lineH = static_cast<int16_t>(g_cjkText.fontSize()) + 6;
    const int16_t textY = cy - lineH / 2;

    // Row label, right value and arrow all derive from the same row center.
    // Do not tune these as separate baselines; that is what made settings rows
    // look stepped on PaperS3. Keep label/value/arrow on 同一水平线.
    g_cjkText.drawText(kRowX, textY, label ? label : "", TFT_BLACK);
    if (value && value[0]) {
        g_cjkText.drawRight(kValueRight, textY, value, kGrayText);
    }
    canvas_->drawLine(kArrowX, cy - 9, kArrowX + 9, cy, TFT_BLACK);
    canvas_->drawLine(kArrowX + 9, cy, kArrowX, cy + 9, TFT_BLACK);
}

void VinkUiRenderer::drawSettingsGroup(int16_t y, const char* title, const char* row1, const char* row1Value, const char* row2, const char* row2Value) {
    // Experience-based, needs PaperS3 confirmation: 24px Simplified Chinese UI
    // font needs generous line boxes on e-paper; the old compact rows made the
    // settings page look squeezed. Use 60px rows and taller groups.
    canvas_->fillRoundRect(28, y, 484, 186, 18, TFT_WHITE);
    canvas_->drawRoundRect(28, y, 484, 186, 18, TFT_BLACK);
    g_cjkText.drawText(56, y + 16, title ? title : "", kGrayText);
    drawSettingsRow(y + 58, row1, row1Value);
    canvas_->drawFastHLine(56, y + 118, 424, kGrayMid);
    drawSettingsRow(y + 118, row2, row2Value);
}

void VinkUiRenderer::drawFooterHint(const char* hint) {
    g_cjkText.drawCentered(0, kPaperS3Height - 42, kPaperS3Width, 28, hint ? hint : "点击标签或卡片", kGrayText);
}

void VinkUiRenderer::renderBoot() {
    if (!canvas_) return;
    clear();
    // Use built-in ASCII drawing here, not the bundled CJK font path. The goal
    // is to prove that the Vink canvas + display-service takeover is visible
    // immediately after the official direct M5.Display boot probe.
    canvas_->setTextColor(TFT_BLACK, TFT_WHITE);
    canvas_->setTextDatum(middle_center);
    canvas_->setTextSize(3);
    canvas_->drawString("VINK CANVAS PROBE", kPaperS3Width / 2, 210);
    canvas_->setTextSize(2);
    canvas_->drawString("after official M5.Display probe", kPaperS3Width / 2, 260);
    canvas_->drawString("960x540 rotation=1", kPaperS3Width / 2, 300);
    canvas_->drawRect(16, 16, kPaperS3Width - 32, kPaperS3Height - 32, TFT_BLACK);
    canvas_->drawFastHLine(16, kPaperS3Height / 2, kPaperS3Width - 32, TFT_BLACK);
    canvas_->drawFastVLine(kPaperS3Width / 2, 16, kPaperS3Height - 32, TFT_BLACK);
    canvas_->setTextDatum(top_left);
    canvas_->setTextSize(1);
}

void VinkUiRenderer::renderHome(SystemState state) {
    renderReaderHome();
    (void)state;
}

void VinkUiRenderer::renderReaderHome() {
    if (!canvas_) return;
    clear();
    drawStatusBar("Vink");
    drawTabs(SystemState::Reader);
    drawCard(28, kContentY, 484, 184, "当前书籍", "TXT 书籍入口");
    drawButton(56, 286, 180, 48, "打开");
    drawButton(304, 286, 180, 48, "书架");
    drawCard(28, 374, 224, 126, "目录", "章节 / 跳页");
    drawCard(288, 374, 224, 126, "书签", "标注 / 截图");
    drawCard(28, 524, 484, 176, "正文设置", "字体 · 字号 · 刷新 · 简体中文");
    drawFooterHint("点卡片进入，滑动切换");
}

void VinkUiRenderer::renderLibrary() {
    if (!canvas_) return;
    clear();
    drawStatusBar("书架");
    drawTabs(SystemState::Library);
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const int16_t x = 34 + col * 164;
            const int16_t y = kContentY + row * 150;
            canvas_->fillRoundRect(x, y, 130, 118, 12, (row + col) % 2 == 0 ? kGrayMid : kGrayLight);
            canvas_->drawRoundRect(x, y, 130, 118, 12, TFT_BLACK);
            g_cjkText.drawCentered(x, y, 130, 118, "书籍", TFT_BLACK);
        }
    }
    drawCard(28, 648, 484, 132, "书架来源", "SD / 最近阅读 / Legado 远程书架");
    drawFooterHint("选书、目录导航、最近记录和远程书架");
}

void VinkUiRenderer::renderTransfer() {
    if (!canvas_) return;
    clear();
    drawStatusBar("传输与同步");
    drawTabs(SystemState::Transfer);
    drawCard(28, kContentY, 484, 180, "Legado", "HTTP 服务：书架 / 进度同步");
    drawButton(56, 278, 180, 48, "立即同步");
    drawButton(304, 278, 180, 48, "配置");
    drawCard(28, 368, 224, 132, "WiFi 传书", "热点 / Web UI");
    drawCard(288, 368, 224, 132, "USB 存储", "确认后接管 SD");
    drawCard(28, 532, 484, 170, "WebDAV / 导出", "ReadPaper 传输工具");
    drawFooterHint("Legado 不是 WebDAV；默认 1122，可配置");
}

void VinkUiRenderer::renderSettings() {
    if (!canvas_) return;
    clear();
    drawStatusBar("设置");
    drawTabs(SystemState::Settings);
    drawSettingsGroup(kContentY, "阅读", "正文字体", "默认", "字号与行距", "默认");
    drawSettingsGroup(352, "显示", "刷新策略", "均衡", "触摸校准", "诊断");
    drawSettingsGroup(550, "连接", "WiFi", "配置", "Legado", "地址");
    drawSettingsGroup(748, "系统", "电源", "点按关机", "关于", kVinkPaperS3FirmwareVersion);
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

void VinkUiRenderer::renderShutdown(const char* reason) {
    if (!canvas_) return;
    clear();
    drawStatusBar("关机");
    canvas_->fillRoundRect(54, 300, 432, 300, 24, TFT_WHITE);
    canvas_->drawRoundRect(54, 300, 432, 300, 24, TFT_BLACK);
    g_cjkText.drawCentered(54, 350, 432, 48, reason ? reason : "正在关机", TFT_BLACK);
    g_cjkText.drawCentered(72, 430, 396, 32, "正在保存进度并关闭电源", kGrayText);
    g_cjkText.drawCentered(72, 482, 396, 32, "请松开侧边电源键", kGrayText);
    g_cjkText.drawCentered(0, 690, kPaperS3Width, 28, "单按侧边键关机；若侧键无效，可从设置页点电源", kGrayText);
}

void VinkUiRenderer::renderLegadoSync(const char* status) {
    if (!canvas_) return;
    clear();
    drawStatusBar("Legado 同步");
    drawTabs(SystemState::Transfer);
    drawCard(28, kContentY, 484, 160, "Legado", status ? status : "等待同步");
    drawButton(56, 346, 180, 48, "返回");
    drawButton(304, 346, 180, 48, "重试");
    drawFooterHint("冲突不自动覆盖，不确定就让用户选");
}

UiAction VinkUiRenderer::hitTestTabs(int16_t x, int16_t y) const {
    if (y < kTabsY || y >= kTabsY + kTabsH) return UiAction::None;
    for (int i = 0; i < 4; ++i) {
        const int16_t tabX = kTabX0 + i * (kTabW + kTabGap);
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
            // Buttons sit inside the current-book card; test them before the
            // surrounding card so the visible "书架" button does not open book.
            if (inRect(x, y, 304, 286, 180, 48)) return UiAction::OpenLibrary;
            if (inRect(x, y, 56, 286, 180, 48)) return UiAction::OpenCurrentBook;
            if (inRect(x, y, 28, kContentY, 484, 184)) return UiAction::OpenCurrentBook;
            break;
        case SystemState::Library:
            // Library rows are owned by ReaderBookService, whose visible/touch
            // geometry starts below the shell summary. Avoid broad hit-testing
            // here; StateMachine transitions only if the reader service accepts
            // the row tap.
            break;
        case SystemState::Transfer:
            if (inRect(x, y, 56, 278, 180, 48)) return UiAction::StartLegadoSync;
            break;
        case SystemState::Settings:
            if (inRect(x, y, 56, 470, 424, 60)) return UiAction::OpenDiagnostics;
            if (inRect(x, y, 56, 806, 424, 60)) return UiAction::RequestShutdown;
            if (y >= kContentY) return UiAction::OpenSettings;
            break;
        case SystemState::Diagnostics:
            if (y >= 316) return UiAction::OpenDiagnostics;
            break;
        case SystemState::LegadoSync:
            if (inRect(x, y, 56, 346, 180, 48)) return UiAction::TabTransfer;
            if (inRect(x, y, 304, 346, 180, 48)) return UiAction::StartLegadoSync;
            break;
        default:
            break;
    }
    return UiAction::None;
}

} // namespace vink3
