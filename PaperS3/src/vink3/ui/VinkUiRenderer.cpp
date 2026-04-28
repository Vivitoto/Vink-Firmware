#include "VinkUiRenderer.h"
#include "../ReadPaper176.h"
#include "../text/CjkTextRenderer.h"

namespace vink3 {

VinkUiRenderer g_uiRenderer;

namespace {
constexpr int16_t kMargin = 24;
constexpr int16_t kStatusH = 62;
constexpr int16_t kTabsY = 76;
constexpr int16_t kTabsH = 64;
constexpr int16_t kContentY = 160;
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

void formatBatteryPercent(char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    int level = M5.Power.getBatteryLevel();
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    snprintf(out, outSize, "%d%%", level);
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
        if (selected) {
            canvas_->fillRoundRect(x, kTabsY, kTabW, kTabsH, 16, TFT_BLACK);
            canvas_->setTextColor(TFT_WHITE, TFT_BLACK);
        } else {
            const uint16_t fill = (i % 2 == 0) ? kGrayMid : kGrayLight;
            canvas_->fillRoundRect(x, kTabsY, kTabW, kTabsH, 16, fill);
            canvas_->drawRoundRect(x, kTabsY, kTabW, kTabsH, 16, TFT_BLACK);
            canvas_->setTextColor(TFT_BLACK, fill);
        }
        g_cjkText.drawCentered(x, kTabsY, kTabW, kTabsH, kTabs[i].label, selected ? TFT_WHITE : TFT_BLACK);
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
    static constexpr int16_t kRowH = 42;
    const int16_t cy = y + kRowH / 2;
    const int16_t textY = cy - static_cast<int16_t>(g_cjkText.fontSize()) / 2 - 2;

    // Row label, right value and arrow all derive from the same row center.
    // Do not tune these as separate baselines; that is what made settings rows
    // look stepped on PaperS3.
    g_cjkText.drawText(kRowX, textY, label ? label : "", TFT_BLACK);
    if (value && value[0]) {
        g_cjkText.drawRight(kValueRight, textY, value, kGrayText);
    }
    canvas_->drawLine(kArrowX, cy - 9, kArrowX + 9, cy, TFT_BLACK);
    canvas_->drawLine(kArrowX + 9, cy, kArrowX, cy + 9, TFT_BLACK);
}

void VinkUiRenderer::drawSettingsGroup(int16_t y, const char* title, const char* row1, const char* row1Value, const char* row2, const char* row2Value) {
    canvas_->fillRoundRect(28, y, 484, 136, 18, TFT_WHITE);
    canvas_->drawRoundRect(28, y, 484, 136, 18, TFT_BLACK);
    g_cjkText.drawText(56, y + 14, title ? title : "", kGrayText);
    drawSettingsRow(y + 42, row1, row1Value);
    canvas_->drawFastHLine(56, y + 84, 424, kGrayMid);
    drawSettingsRow(y + 84, row2, row2Value);
}

void VinkUiRenderer::drawFooterHint(const char* hint) {
    g_cjkText.drawCentered(0, kPaperS3Height - 42, kPaperS3Width, 28, hint ? hint : "点击标签或卡片", kGrayText);
}

void VinkUiRenderer::renderBoot() {
    if (!canvas_) return;
    clear();
    g_cjkText.drawCentered(0, 390, kPaperS3Width, 48, "Vink", TFT_BLACK);
    g_cjkText.drawCentered(0, 460, kPaperS3Width, 28, "v0.3.0 · ReadPaper V1.7.6 底层", kGrayText);
}

void VinkUiRenderer::renderHome(SystemState state) {
    renderReaderHome();
    (void)state;
}

void VinkUiRenderer::renderReaderHome() {
    if (!canvas_) return;
    clear();
    drawStatusBar("Vink 阅读");
    drawTabs(SystemState::Reader);
    drawCard(28, kContentY, 484, 190, "当前书籍", "选择 TXT 后进入书籍入口");
    drawButton(56, 292, 180, 48, "打开");
    drawButton(304, 292, 180, 48, "书架");
    drawCard(28, 382, 224, 132, "目录", "章节 / 跳页");
    drawCard(288, 382, 224, 132, "标注", "书签 / 截图");
    drawCard(28, 544, 484, 188, "正文设置", "自动翻页 / 刷新 / 字号 / 竖排 / 简体中文");
    drawFooterHint("当前书设置留在正文页，设置页只管默认值");
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
    drawSettingsGroup(310, "显示", "刷新策略", "均衡", "深色模式", "关闭");
    drawSettingsGroup(460, "连接", "WiFi", "配置", "Legado", "地址");
    drawSettingsGroup(610, "系统", "电池与休眠", "自动", "关于", "v0.3.1");
    drawFooterHint("设置项文字、右侧值和箭头保持同一水平线");
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
            if (inRect(x, y, 304, 292, 180, 48)) return UiAction::OpenLibrary;
            if (inRect(x, y, 56, 292, 180, 48)) return UiAction::OpenCurrentBook;
            if (inRect(x, y, 28, kContentY, 484, 190)) return UiAction::OpenCurrentBook;
            break;
        case SystemState::Library:
            if (y >= kContentY && y < 610) return UiAction::OpenCurrentBook;
            break;
        case SystemState::Transfer:
            if (inRect(x, y, 56, 278, 180, 48)) return UiAction::StartLegadoSync;
            break;
        case SystemState::Settings:
            if (y >= kContentY) return UiAction::OpenSettings;
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
