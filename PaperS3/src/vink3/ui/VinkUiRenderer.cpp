#include "VinkUiRenderer.h"
#include "../ReadPaper176.h"

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

struct TabDef {
    SystemState state;
    UiAction action;
    const char* label;
};

constexpr TabDef kTabs[] = {
    {SystemState::Reader, UiAction::TabReader, "Read"},
    {SystemState::Library, UiAction::TabLibrary, "Shelf"},
    {SystemState::Transfer, UiAction::TabTransfer, "Sync"},
    {SystemState::Settings, UiAction::TabSettings, "Set"},
};

bool inRect(int16_t x, int16_t y, int16_t rx, int16_t ry, int16_t rw, int16_t rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}
} // namespace

bool VinkUiRenderer::begin(M5Canvas* canvas) {
    if (!canvas) return false;
    canvas_ = canvas;
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
    canvas_->setTextDatum(middle_left);
    canvas_->setTextSize(2);
    canvas_->drawString(title ? title : "Vink", kMargin, 32);
    canvas_->setTextDatum(middle_right);
    canvas_->setTextSize(1);
    canvas_->drawString("v0.3.0", kPaperS3Width - kMargin, 32);
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
            canvas_->drawRoundRect(x, kTabsY, kTabW, kTabsH, 16, TFT_BLACK);
            canvas_->setTextColor(TFT_BLACK, TFT_WHITE);
        }
        canvas_->setTextDatum(middle_center);
        canvas_->drawString(kTabs[i].label, x + kTabW / 2, kTabsY + kTabsH / 2);
    }
    canvas_->setTextColor(TFT_BLACK, TFT_WHITE);
}

void VinkUiRenderer::drawCard(int16_t x, int16_t y, int16_t w, int16_t h, const char* title, const char* body) {
    canvas_->drawRoundRect(x, y, w, h, 18, TFT_BLACK);
    canvas_->setTextDatum(top_left);
    canvas_->setTextSize(2);
    canvas_->drawString(title ? title : "", x + 22, y + 18);
    canvas_->setTextSize(1);
    if (body && body[0]) {
        canvas_->drawString(body, x + 22, y + 58);
    }
}

void VinkUiRenderer::drawButton(int16_t x, int16_t y, int16_t w, int16_t h, const char* label) {
    canvas_->drawRoundRect(x, y, w, h, h / 2, TFT_BLACK);
    canvas_->setTextDatum(middle_center);
    canvas_->setTextSize(1);
    canvas_->drawString(label ? label : "", x + w / 2, y + h / 2);
}

void VinkUiRenderer::drawFooterHint(const char* hint) {
    canvas_->setTextDatum(middle_center);
    canvas_->setTextSize(1);
    canvas_->drawString(hint ? hint : "Tap tab or card", kPaperS3Width / 2, kPaperS3Height - 28);
}

void VinkUiRenderer::renderBoot() {
    if (!canvas_) return;
    clear();
    canvas_->setTextDatum(middle_center);
    canvas_->setTextSize(3);
    canvas_->drawString("Vink", 270, 410);
    canvas_->setTextSize(1);
    canvas_->drawString("v0.3.0 / ReadPaper V1.7.6 core", 270, 470);
}

void VinkUiRenderer::renderHome(SystemState state) {
    renderReaderHome();
    (void)state;
}

void VinkUiRenderer::renderReaderHome() {
    if (!canvas_) return;
    clear();
    drawStatusBar("Vink Reader");
    drawTabs(SystemState::Reader);
    drawCard(28, kContentY, 484, 180, "Current book", "No book opened yet");
    drawButton(56, 292, 180, 44, "Open");
    drawButton(304, 292, 180, 44, "Library");
    drawCard(28, 370, 224, 132, "Recent", "ReadPaper-like events");
    drawCard(288, 370, 224, 132, "Progress", "Legado ready");
    drawCard(28, 532, 484, 220, "Vink UI", "Cards, tabs and hitboxes are owned by UI renderer");
    drawFooterHint("Reader tab");
}

void VinkUiRenderer::renderLibrary() {
    if (!canvas_) return;
    clear();
    drawStatusBar("Library");
    drawTabs(SystemState::Library);
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const int16_t x = 34 + col * 164;
            const int16_t y = kContentY + row * 150;
            canvas_->drawRoundRect(x, y, 130, 118, 12, TFT_BLACK);
            canvas_->setTextDatum(middle_center);
            canvas_->setTextSize(1);
            canvas_->drawString("Book", x + 65, y + 59);
        }
    }
    drawCard(28, 648, 484, 132, "Import", "USB / WiFi / SD book sources");
    drawFooterHint("Tap a book later; current scaffold focuses on routing");
}

void VinkUiRenderer::renderTransfer() {
    if (!canvas_) return;
    clear();
    drawStatusBar("Transfer & Sync");
    drawTabs(SystemState::Transfer);
    drawCard(28, kContentY, 484, 150, "Legado", "Remote reading progress sync service");
    drawButton(56, 268, 180, 48, "Sync now");
    drawButton(304, 268, 180, 48, "Config");
    drawCard(28, 360, 224, 132, "WiFi", "Uploader/API");
    drawCard(288, 360, 224, 132, "USB", "MSC later");
    drawCard(28, 524, 484, 170, "Status", "No remote sync has run in this scaffold");
    drawFooterHint("Transfer tab");
}

void VinkUiRenderer::renderSettings() {
    if (!canvas_) return;
    clear();
    drawStatusBar("Settings");
    drawTabs(SystemState::Settings);
    drawCard(28, kContentY, 484, 132, "Display", "Refresh, rotation, theme");
    drawCard(28, 318, 484, 132, "Reading", "Font, spacing, layout");
    drawCard(28, 476, 484, 132, "Network", "WiFi and Legado endpoint");
    drawCard(28, 634, 484, 132, "System", "Battery, sleep, about");
    drawFooterHint("Settings tab");
}

void VinkUiRenderer::renderLegadoSync(const char* status) {
    if (!canvas_) return;
    clear();
    drawStatusBar("Legado Sync");
    drawTabs(SystemState::Transfer);
    drawCard(28, kContentY, 484, 160, "Legado", status ? status : "Waiting");
    drawButton(56, 346, 180, 48, "Back");
    drawButton(304, 346, 180, 48, "Retry");
    drawFooterHint("Service screen; HTTP sync comes after reader core");
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
