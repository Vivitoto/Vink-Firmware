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
    g_cjkText.drawText(kMargin, 22, title ? title : "Vink", TFT_BLACK);
    g_cjkText.drawRight(kPaperS3Width - kMargin, 22, "v0.3.0", kGrayText);
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
    const uint16_t fill = ((y / 100) % 2 == 0) ? kGrayLight : kGrayMid;
    canvas_->fillRoundRect(x, y, w, h, 18, fill);
    canvas_->drawRoundRect(x, y, w, h, 18, TFT_BLACK);
    g_cjkText.drawText(x + 22, y + 18, title ? title : "", TFT_BLACK);
    if (body && body[0]) {
        g_cjkText.drawText(x + 22, y + 58, body, kGrayText);
    }
}

void VinkUiRenderer::drawButton(int16_t x, int16_t y, int16_t w, int16_t h, const char* label) {
    canvas_->drawRoundRect(x, y, w, h, h / 2, TFT_BLACK);
    g_cjkText.drawCentered(x, y, w, h, label ? label : "", TFT_BLACK);
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
    drawCard(28, kContentY, 484, 180, "当前书籍", "继续阅读 / 上一页 / 下一页 / 中心菜单");
    drawButton(56, 292, 180, 44, "打开");
    drawButton(304, 292, 180, 44, "书架");
    drawCard(28, 370, 224, 132, "目录", "章节 / 跳页");
    drawCard(288, 370, 224, 132, "标注", "书签 / 截图");
    drawCard(28, 532, 484, 220, "正文设置", "自动翻页 / 刷新 / 字号 / 竖排 / 繁简");
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
    drawCard(28, kContentY, 484, 150, "Legado", "HTTP 服务：书架 / 进度同步");
    drawButton(56, 268, 180, 48, "立即同步");
    drawButton(304, 268, 180, 48, "配置");
    drawCard(28, 360, 224, 132, "WiFi 传书", "热点 / Web UI");
    drawCard(288, 360, 224, 132, "USB 存储", "确认后接管 SD");
    drawCard(28, 524, 484, 170, "WebDAV / 导出", "ReadPaper 传输工具");
    drawFooterHint("Legado 不是 WebDAV；默认 1122，可配置");
}

void VinkUiRenderer::renderSettings() {
    if (!canvas_) return;
    clear();
    drawStatusBar("设置");
    drawTabs(SystemState::Settings);
    drawCard(28, kContentY, 484, 132, "显示默认", "刷新策略 / 深色 / 旋转 / 质量");
    drawCard(28, 318, 484, 132, "阅读默认", "默认字体 / 默认字号 / 默认横竖排");
    drawCard(28, 476, 484, 132, "网络", "WiFi / WebDAV / Legado 地址");
    drawCard(28, 634, 484, 132, "系统", "电池 / 睡眠 / 版本 / 调试");
    drawFooterHint("全局默认放这里；正文临场设置放阅读页");
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
            if (inRect(x, y, 28, kContentY, 484, 180)) return UiAction::OpenCurrentBook;
            if (inRect(x, y, 304, 292, 180, 44)) return UiAction::OpenLibrary;
            break;
        case SystemState::Library:
            if (y >= kContentY && y < 610) return UiAction::OpenCurrentBook;
            break;
        case SystemState::Transfer:
            if (inRect(x, y, 56, 268, 180, 48)) return UiAction::StartLegadoSync;
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
