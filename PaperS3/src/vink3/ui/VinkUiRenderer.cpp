#include "VinkUiRenderer.h"
#include "../ReadPaper176.h"
#include "../config/ConfigService.h"
#include "../sync/LegadoService.h"
#include "../sync/WifiService.h"
#include "../text/CjkTextRenderer.h"
#include "../../FontManager.h"

namespace vink3 {

VinkUiRenderer g_uiRenderer;

namespace {
// ── Layout constants ──────────────────────────────────────────────────────────
constexpr int16_t kMargin      = 24;
constexpr int16_t kStatusH     = 62;
constexpr int16_t kTabsY       = 76;
constexpr int16_t kTabsH        = 60;
constexpr int16_t kContentY    = 154;
constexpr int16_t kTabW        = 120;
constexpr int16_t kTabGap      = 8;
constexpr int16_t kTabX0       = 18;

// Settings card / row geometry
constexpr int16_t kCardX       = 28;
constexpr int16_t kCardW       = 484;
constexpr int16_t kCardRound   = 18;
constexpr int16_t kGroupH      = 186;   // 2-row group height
constexpr int16_t kGroupGap    = 12;
constexpr int16_t kRowX        = 56;
constexpr int16_t kRowH        = 60;
constexpr int16_t kRowDividerY = 118;   // relative to group top
constexpr int16_t kValueRight  = 448;
constexpr int16_t kArrowX      = 474;

constexpr uint16_t kGrayLight  = 0xEF7D; // ~#EFEFEF
constexpr uint16_t kGrayMid   = 0xD69A;  // ~#D6D6D6
constexpr uint16_t kGrayText  = 0x8410;  // ~#888888

// Settings main page group Y positions
constexpr int16_t kGrpY_Reading  = kContentY;             // 154
constexpr int16_t kGrpY_Display   = kGrpY_Reading + kGroupH + kGroupGap;   // 352
constexpr int16_t kGrpY_Connect   = kGrpY_Display + kGroupH + kGroupGap;  // 550
constexpr int16_t kGrpY_System    = kGrpY_Connect + kGroupH + kGroupGap;   // 748

struct TabDef {
    SystemState state;
    UiAction action;
    const char* label;
};

constexpr TabDef kTabs[] = {
    {SystemState::Reader,     UiAction::TabReader,     "阅读"},
    {SystemState::Library,    UiAction::TabLibrary,    "书架"},
    {SystemState::Transfer,   UiAction::TabTransfer,   "同步"},
    {SystemState::Settings,   UiAction::TabSettings,   "设置"},
};

// ── Helpers ───────────────────────────────────────────────────────────────────

bool inRect(int16_t x, int16_t y, int16_t rx, int16_t ry, int16_t rw, int16_t rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

void formatStatusTime(char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    m5::rtc_time_t rtc;
    if (M5.Rtc.isEnabled() && M5.Rtc.getTime(&rtc) &&
        rtc.hours < 24 && rtc.minutes < 60) {
        snprintf(out, outSize, "%02d:%02d", rtc.hours, rtc.minutes);
        return;
    }
    snprintf(out, outSize, "--:--");
}

float readOfficialBatteryVoltage() {
    const int raw = analogRead(static_cast<int>(kBatteryAdcPin));
    if (raw <= 0) return 0.0f;
    return static_cast<float>(raw) * 3.5f / 4096.0f * 2.0f;
}

bool isOfficialUsbConnected() {
    return digitalRead(static_cast<int>(kUsbDetectPin)) == HIGH;
}

bool isOfficialChargeStateActive() {
    return digitalRead(static_cast<int>(kChargeStatePin)) == LOW;
}

void formatBatteryPercent(char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    int level = M5.Power.getBatteryLevel();
    if (level > 0 && level <= 100) {
        snprintf(out, outSize, "%d%%", level);
        return;
    }
    snprintf(out, outSize, "--%%");
}

void formatBatteryPercentSimple(char* out, size_t outSize) {
    // Simple percentage only (no USB prefix, no voltage fallback)
    if (!out || outSize == 0) return;
    int level = M5.Power.getBatteryLevel();
    if (level > 0 && level <= 100) {
        snprintf(out, outSize, "%d%%", level);
    } else {
        snprintf(out, outSize, "--%%");
    }
}

} // anonymous namespace

// ── VinkUiRenderer inline helpers (need canvas_) ─────────────────────────────

void VinkUiRenderer::formatTimeStr(char* out, size_t outSize) {
    m5::rtc_time_t rtc;
    if (M5.Rtc.isEnabled() && M5.Rtc.getTime(&rtc) &&
        rtc.hours < 24 && rtc.minutes < 60) {
        snprintf(out, outSize, "%02d:%02d", rtc.hours, rtc.minutes);
    } else {
        snprintf(out, outSize, "--:--");
    }
}

void VinkUiRenderer::formatBatterySimple(char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    int level = M5.Power.getBatteryLevel();
    if (level > 0 && level <= 100) {
        snprintf(out, outSize, "%d%%", level);
    } else {
        snprintf(out, outSize, "--%%");
    }
}

void VinkUiRenderer::drawSettingsRowRaw(int16_t rowTopY, const char* label, const char* value) {
    // Keep label/value/arrow on 同一水平线; all derive from the row center.
    const int16_t cy = rowTopY + kRowH / 2;
    const int16_t lineH = 24 + 6;
    const int16_t textY = cy - lineH / 2;
    canvas_->setTextColor(TFT_BLACK, TFT_WHITE);
    g_cjkText.drawText(kRowX, textY, label ? label : "", TFT_BLACK);
    if (value && value[0]) {
        g_cjkText.drawRight(kValueRight, textY, value, kGrayText);
    }
    canvas_->drawLine(kArrowX, cy - 9, kArrowX + 9, cy, TFT_BLACK);
    canvas_->drawLine(kArrowX + 9, cy, kArrowX, cy + 9, TFT_BLACK);
}

void VinkUiRenderer::drawCyclingRow(int16_t rowTopY, const char* label, const char* value) {
    const int16_t cy = rowTopY + kRowH / 2;
    const int16_t lineH = 24 + 6;
    const int16_t textY = cy - lineH / 2;
    canvas_->setTextColor(TFT_BLACK, TFT_WHITE);
    g_cjkText.drawText(kRowX, textY, label ? label : "", TFT_BLACK);
    if (value && value[0]) {
        g_cjkText.drawRight(kValueRight, textY, value, kGrayText);
    }
}

// ── Public renderer API ───────────────────────────────────────────────────────

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
            canvas_->drawRoundRect(x + 2, kTabsY + 2, kTabW - 4, kTabsH - 4, 14, TFT_BLACK);
            canvas_->fillRoundRect(x + 28, kTabsY + kTabsH - 9, kTabW - 56, 4, 2, TFT_BLACK);
        }
        canvas_->setTextColor(TFT_BLACK, TFT_WHITE);
        g_cjkText.drawCentered(x, kTabsY, kTabW, kTabsH - 4, kTabs[i].label, TFT_BLACK);
    }
    canvas_->setTextColor(TFT_BLACK, TFT_WHITE);
}

void VinkUiRenderer::drawCard(int16_t x, int16_t y, int16_t w, int16_t h, const char* title, const char* body) {
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
    drawSettingsRowRaw(y, label, value);
}

void VinkUiRenderer::drawSettingsGroup(int16_t y, const char* title,
                                        const char* row1, const char* row1Value,
                                        const char* row2, const char* row2Value) {
    canvas_->fillRoundRect(kCardX, y, kCardW, kGroupH, kCardRound, TFT_WHITE);
    canvas_->drawRoundRect(kCardX, y, kCardW, kGroupH, kCardRound, TFT_BLACK);
    g_cjkText.drawText(56, y + 16, title ? title : "", kGrayText);
    drawSettingsRowRaw(y + 58, row1, row1Value);
    canvas_->drawFastHLine(56, y + kRowDividerY, 424, kGrayMid);
    drawSettingsRowRaw(y + 118, row2, row2Value);
}

void VinkUiRenderer::drawFooterHint(const char* hint) {
    g_cjkText.drawCentered(0, kPaperS3Height - 42, kPaperS3Width, 28, hint ? hint : "点击标签或卡片", kGrayText);
}

// ── Boot ───────────────────────────────────────────────────────────────────────

void VinkUiRenderer::renderBoot() {
    if (!canvas_) return;
    clear();
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

// ── Reader Home ───────────────────────────────────────────────────────────────

void VinkUiRenderer::renderReaderHome() {
    if (!canvas_) return;
    clear();
    drawStatusBar("Vink");
    drawTabs(SystemState::Reader);
    drawCard(28, kContentY, 484, 184, "当前书籍", "TXT 书籍入口");
    drawButton(56, 286, 180, 48, "打开");
    drawButton(304, 286, 180, 48, "书架");
    drawCard(28, 374, 224, 126, "目录", "识别章节 / 跳转");
    drawCard(288, 374, 224, 126, "书签", "标注 / 截图");
    drawCard(28, 524, 484, 176, "正文设置", "字体 · 字号 · 刷新 · 简体中文");
    // drawFooterHint("点卡片进入，滑动切换");
}

// ── Library ───────────────────────────────────────────────────────────────────

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
    // drawFooterHint("选书、目录导航、最近记录和远程书架");
}

// ── Transfer ──────────────────────────────────────────────────────────────────

static const char* labelForWifiMode(WifiOpMode m) {
    switch (m) {
        case WifiOpMode::Off:    return "关闭";
        case WifiOpMode::Sta:    return "STA 模式";
        case WifiOpMode::Ap:     return "AP 热点";
        case WifiOpMode::ApWebUi: return "AP + Web UI";
    }
    return "未知";
}

static const char* labelForLegadoStatus(bool connected, bool enabled) {
    if (!enabled) return "未启用";
    return connected ? "已连接" : "未连接";
}

void VinkUiRenderer::renderTransfer() {
    if (!canvas_) return;
    clear();
    drawStatusBar("传输与同步");
    drawTabs(SystemState::Transfer);

    const auto& cfg = g_configService.get();
    bool lc = g_legadoService.isConnected();

    bool la = g_legadoService.isConfigured();

    // Legado card: shows connection status
    {
        canvas_->fillRoundRect(28, kContentY, 484, 172, kCardRound, TFT_WHITE);
        canvas_->drawRoundRect(28, kContentY, 484, 172, kCardRound, TFT_BLACK);
        g_cjkText.drawText(56, kContentY + 16, "Legado 阅读同步", kGrayText);
        g_cjkText.drawText(56, kContentY + 50,
            cfg.legadoHost.isEmpty()
                ? "地址未配置"
                : cfg.legadoHost.c_str(),
            kGrayText);

        // Status row: enabled + connected
        {
            static char buf[48];
            snprintf(buf, sizeof(buf), "%s | %s",
                     labelForLegadoStatus(lc, la),
                     la ? cfg.legadoHost.c_str() : "请先在设置中配置");
            g_cjkText.drawText(56, kContentY + 84, buf, kGrayText);
        }
    }

    drawButton(56, 334, 180, 48, "立即同步");
    drawButton(304, 334, 180, 48, "连接配置");

    // WiFi card
    {
        const char* wifiLabel = labelForWifiMode(g_wifiService.mode());
        canvas_->fillRoundRect(28, 392, 224, 120, kCardRound, TFT_WHITE);
        canvas_->drawRoundRect(28, 392, 224, 120, kCardRound, TFT_BLACK);
        g_cjkText.drawText(56, 392 + 16, "WiFi 传书", TFT_BLACK);
        g_cjkText.drawText(56, 392 + 50, wifiLabel, kGrayText);
        if (g_wifiService.isApActive() || g_wifiService.mode() == WifiOpMode::Sta) {
            static char ipbuf[32];
            snprintf(ipbuf, sizeof(ipbuf), "IP: %s", g_wifiService.getLocalIp().c_str());
            g_cjkText.drawText(56, 392 + 82, ipbuf, kGrayText);
        }
    }

    // USB MSC card
    {
        canvas_->fillRoundRect(288, 392, 224, 120, kCardRound, TFT_WHITE);
        canvas_->drawRoundRect(288, 392, 224, 120, kCardRound, TFT_BLACK);
        g_cjkText.drawText(316, 392 + 16, "USB 存储", TFT_BLACK);
        g_cjkText.drawText(316, 392 + 50, "SD 卡 USB 模式", kGrayText);
        g_cjkText.drawText(316, 392 + 82, "需确认后接管", kGrayText);
    }

    // WebDAV + Export card
    {
        canvas_->fillRoundRect(28, 532, 484, 120, kCardRound, TFT_WHITE);
        canvas_->drawRoundRect(28, 532, 484, 120, kCardRound, TFT_BLACK);
        g_cjkText.drawText(56, 532 + 16, "数据导出", TFT_BLACK);
        g_cjkText.drawText(56, 532 + 50, "截图导出 / 备份配置", kGrayText);
    }

    // drawFooterHint("点击卡片进入详细设置");
}

// ── Transfer: Legado status / sync ───────────────────────────────────────────

void VinkUiRenderer::renderTransferLegadoStatus() {
    if (!canvas_) return;
    clear();
    drawStatusBar("Legado 同步");
    drawTabs(SystemState::Transfer);

    const auto& cfg = g_configService.get();
    bool lc = g_legadoService.isConnected();

    // Connection info card
    {
        canvas_->fillRoundRect(kCardX, kGrpY_Reading, kCardW, kGroupH, kCardRound, TFT_WHITE);
        canvas_->drawRoundRect(kCardX, kGrpY_Reading, kCardW, kGroupH, kCardRound, TFT_BLACK);
        g_cjkText.drawText(56, kGrpY_Reading + 16, "连接状态", kGrayText);
        drawCyclingRow(kGrpY_Reading + 58, "服务器",
            cfg.legadoHost.isEmpty() ? "未配置" : cfg.legadoHost.c_str());
        canvas_->drawFastHLine(56, kGrpY_Reading + kRowDividerY, 424, kGrayMid);
        drawCyclingRow(kGrpY_Reading + 118, "端口",
            cfg.legadoHost.isEmpty() ? "1122" : String(cfg.legadoPort).c_str());
    }

    // Last sync card
    {
        canvas_->fillRoundRect(kCardX, kGrpY_Display, kCardW, kGroupH, kCardRound, TFT_WHITE);
        canvas_->drawRoundRect(kCardX, kGrpY_Display, kCardW, kGroupH, kCardRound, TFT_BLACK);
        g_cjkText.drawText(56, kGrpY_Display + 16, "上次同步", kGrayText);
        drawCyclingRow(kGrpY_Display + 58, "状态",
            g_legadoService.isConfigured()
                ? (lc ? "已连接" : "未连接")
                : "未配置");
        canvas_->drawFastHLine(56, kGrpY_Display + kRowDividerY, 424, kGrayMid);
        drawCyclingRow(kGrpY_Display + 118, "自动同步", cfg.legadoEnabled ? "开启" : "关闭");
        canvas_->drawFastHLine(56, kGrpY_Display + 178, 424, kGrayMid);
        if (g_legadoService.lastError().length() > 0) {
            drawCyclingRow(kGrpY_Display + 238, "错误",
                g_legadoService.lastError().substring(0, 28).c_str());
        } else {
            drawCyclingRow(kGrpY_Display + 238, "错误", "无");
        }
    }

    drawButton(56, kGrpY_Connect + 20, 180, 48, "← 返回");
    drawButton(304, kGrpY_Connect + 20, 180, 48, "立即同步");
    // drawFooterHint("点击「立即同步」从 Legado 拉取书架和进度");
}

// ── Transfer: WiFi AP ────────────────────────────────────────────────────────

void VinkUiRenderer::renderTransferWifiAp() {
    if (!canvas_) return;
    clear();
    drawStatusBar("WiFi 传书");
    drawTabs(SystemState::Transfer);

    // Current mode card
    {
        canvas_->fillRoundRect(kCardX, kGrpY_Reading, kCardW, kGroupH, kCardRound, TFT_WHITE);
        canvas_->drawRoundRect(kCardX, kGrpY_Reading, kCardW, kGroupH, kCardRound, TFT_BLACK);
        g_cjkText.drawText(56, kGrpY_Reading + 16, "当前模式", kGrayText);
        drawCyclingRow(kGrpY_Reading + 58, "WiFi",
            labelForWifiMode(g_wifiService.mode()));
        canvas_->drawFastHLine(56, kGrpY_Reading + kRowDividerY, 424, kGrayMid);
        drawCyclingRow(kGrpY_Reading + 118, "IP 地址",
            g_wifiService.getLocalIp().isEmpty() ? "—" : g_wifiService.getLocalIp().c_str());
    }

    // Mode selector card — three options
    {
        constexpr int16_t optH = 58;
        const char* opts[3] = { "关闭 WiFi", "AP + Web UI", "STA 连接路由器" };
        for (int i = 0; i < 3; ++i) {
            const int16_t optY = kGrpY_Display + i * (optH + 4);
            canvas_->fillRoundRect(kCardX, optY, kCardW, optH, 12, TFT_WHITE);
            canvas_->drawRoundRect(kCardX, optY, kCardW, optH, 12, TFT_BLACK);
            g_cjkText.drawText(56, optY + 16, opts[i], TFT_BLACK);
        }
    }

    drawButton(56, kGrpY_Connect + 20, 180, 48, "← 返回");
    drawButton(304, kGrpY_Connect + 20, 180, 48, "启动热点");
    // drawFooterHint("AP 模式：设备变身热点，手机直连传书");
}

// ── Transfer: USB MSC ───────────────────────────────────────────────────────

void VinkUiRenderer::renderTransferUsb() {
    if (!canvas_) return;
    clear();
    drawStatusBar("USB 存储模式");
    drawTabs(SystemState::Transfer);

    // Warning / info card
    canvas_->fillRoundRect(kCardX, kGrpY_Reading, kCardW, kGroupH, kCardRound, TFT_WHITE);
    canvas_->drawRoundRect(kCardX, kGrpY_Reading, kCardW, kGroupH, kCardRound, TFT_BLACK);
    g_cjkText.drawText(56, kGrpY_Reading + 16, "说明", kGrayText);
    g_cjkText.drawText(56, kGrpY_Reading + 58,
        "切换到 USB 存储后，SD 卡将以磁盘模式挂载到电脑，", kGrayText);
    g_cjkText.drawText(56, kGrpY_Reading + 84,
        "阅读器功能在此期间暂停。断开 USB 连接后恢复。", kGrayText);
    canvas_->drawFastHLine(56, kGrpY_Reading + kRowDividerY, 424, kGrayMid);
    g_cjkText.drawText(56, kGrpY_Reading + 118, "⚠ 进入此模式会中断当前阅读", kGrayText);


    // Confirm button
    canvas_->fillRoundRect(56, kGrpY_Display + 20, 384, 64, 24, TFT_BLACK);
    g_cjkText.drawCentered(56, kGrpY_Display + 20, 384, 64, "确认进入 USB 存储模式", TFT_WHITE);


    drawButton(56, kGrpY_Connect + 20, 180, 48, "← 返回");
    // drawFooterHint("确认后设备将重启为 USB 磁盘模式");
}

// WebDAV UI removed — kept as empty stub to avoid breaking the header declaration
// and any future revival of this feature.
void VinkUiRenderer::renderTransferWebDav() {
    // intentionally empty: feature was removed as dead code
}

void VinkUiRenderer::renderTransferExport() {
    if (!canvas_) return;
    clear();
    drawStatusBar("导出与截图");
    drawTabs(SystemState::Transfer);


    // Export options
    {
        constexpr int16_t optH = 58;
        const char* opts[3] = { "截图当前页", "导出阅读进度", "导出书架清单" };
        for (int i = 0; i < 3; ++i) {
            const int16_t optY = kGrpY_Reading + i * (optH + 4);
            canvas_->fillRoundRect(kCardX, optY, kCardW, optH, 12, TFT_WHITE);
            canvas_->drawRoundRect(kCardX, optY, kCardW, optH, 12, TFT_BLACK);
            g_cjkText.drawText(56, optY + 16, opts[i], TFT_BLACK);
        }
    }

    // Info
    drawCard(kCardX, kGrpY_Display, kCardW, kGroupH,
        "说明",
        "截图保存到 SD 卡 /screenshots/；进度和书架导出为 JSON 文件");

    drawButton(56, kGrpY_Connect + 20, 180, 48, "← 返回");
    // drawFooterHint("截图和导出文件保存在 SD 卡根目录");
}

// ── Settings main ─────────────────────────────────────────────────────────────

static const char* labelForRefresh(RefreshFrequency f) {
    switch (f) {
        case RefreshFrequency::FREQ_LOW:    return "极速";
        case RefreshFrequency::FREQ_MEDIUM: return "均衡";
        case RefreshFrequency::FREQ_HIGH:   return "清晰";
    }
    return "均衡";
}

static const char* labelForFontSize(uint8_t s) {
    static char buf[8];
    snprintf(buf, sizeof(buf), "%dpx", s);
    return buf;
}

static const char* labelForLineSpacing(uint8_t ls) {
    static char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", ls);
    return buf;
}

static const char* labelForAutoSleep(uint8_t mins) {
    static char buf[12];
    snprintf(buf, sizeof(buf), "%d分钟", mins);
    return buf;
}

void VinkUiRenderer::renderSettings() {
    if (!canvas_) return;
    clear();
    drawStatusBar("设置");
    drawTabs(SystemState::Settings);

    const auto& cfg = g_configService.get();

    // 阅读 group: 正文字体 / 字号与行距
    drawSettingsGroup(kGrpY_Reading, "阅读",
        "正文字体", "霞鹜文楷",
        "字号与行距", labelForFontSize(cfg.fontSize));

    // 显示 group: 刷新策略 / 触摸校准
    drawSettingsGroup(kGrpY_Display, "显示",
        "刷新策略", labelForRefresh(cfg.refreshFrequency),
        "触摸校准", "诊断");

    // 连接 group: WiFi / Legado
    drawSettingsGroup(kGrpY_Connect, "连接",
        "WiFi", cfg.wifiSsid.isEmpty() ? "未配置" : cfg.wifiSsid.c_str(),
        "Legado", cfg.legadoHost.isEmpty() ? "未配置" : cfg.legadoHost.c_str());

    // 系统 group: 电源 / 关于
    drawSettingsGroup(kGrpY_System, "系统",
        "自动休眠", cfg.autoSleepEnabled ? labelForAutoSleep(cfg.autoSleepMinutes) : "关闭",
        "关于", kVinkPaperS3FirmwareVersion);

    // drawFooterHint("点击卡片进入详细设置");
}

// ── Settings: Layout ─────────────────────────────────────────────────────────

void VinkUiRenderer::renderSettingsLayout() {
    if (!canvas_) return;
    clear();
    drawStatusBar("阅读排版");
    drawTabs(SystemState::Settings);

    const auto& cfg = g_configService.get();

    // Group 1: font size + line spacing
    canvas_->fillRoundRect(kCardX, kGrpY_Reading, kCardW, kGroupH, kCardRound, TFT_WHITE);
    canvas_->drawRoundRect(kCardX, kGrpY_Reading, kCardW, kGroupH, kCardRound, TFT_BLACK);
    g_cjkText.drawText(56, kGrpY_Reading + 16, "字号与行距", kGrayText);
    drawCyclingRow(kGrpY_Reading + 58, "字号", labelForFontSize(cfg.fontSize));
    canvas_->drawFastHLine(56, kGrpY_Reading + kRowDividerY, 424, kGrayMid);
    drawCyclingRow(kGrpY_Reading + 118, "行距", labelForLineSpacing(cfg.lineSpacing));

    // Group 2: margins + justify
    canvas_->fillRoundRect(kCardX, kGrpY_Display, kCardW, kGroupH, kCardRound, TFT_WHITE);
    canvas_->drawRoundRect(kCardX, kGrpY_Display, kCardW, kGroupH, kCardRound, TFT_BLACK);
    g_cjkText.drawText(56, kGrpY_Display + 16, "边距与对齐", kGrayText);
    {
        static char buf[16];
        snprintf(buf, sizeof(buf), "左右各%dpx", cfg.marginLeft);
        drawCyclingRow(kGrpY_Display + 58, "页边距", buf);
    }
    canvas_->drawFastHLine(56, kGrpY_Display + kRowDividerY, 424, kGrayMid);
    drawCyclingRow(kGrpY_Display + 118, "两端对齐", cfg.justify ? "开" : "关");

    // Group 3: 简繁 + back
    canvas_->fillRoundRect(kCardX, kGrpY_Connect, kCardW, kGroupH, kCardRound, TFT_WHITE);
    canvas_->drawRoundRect(kCardX, kGrpY_Connect, kCardW, kGroupH, kCardRound, TFT_BLACK);
    g_cjkText.drawText(56, kGrpY_Connect + 16, "文字选项", kGrayText);
    drawCyclingRow(kGrpY_Connect + 58, "简体中文", cfg.simplifiedChinese ? "开启" : "关闭");
    canvas_->drawFastHLine(56, kGrpY_Connect + kRowDividerY, 424, kGrayMid);
    drawCyclingRow(kGrpY_Connect + 118, "返回", "← 设置");

    // Group 4: font family (SD / SPIFFS fonts)
    static char kFontNames[32][64] = {{0}};
    static char kFontPaths[32][128] = {{0}};
    static int sFontCount = -1;
    if (sFontCount < 0) {
        sFontCount = FontManager::scanFonts(kFontPaths, kFontNames, 32);
        if (sFontCount <= 0) {
            strlcpy(kFontNames[0], "无可用字体", 64);
            sFontCount = 1;
        }
    }
    const char* curFontName = (cfg.fontIndex < (uint8_t)sFontCount)
        ? kFontNames[cfg.fontIndex] : kFontNames[0];
    canvas_->fillRoundRect(kCardX, kGrpY_System, kCardW, kGroupH, kCardRound, TFT_WHITE);
    canvas_->drawRoundRect(kCardX, kGrpY_System, kCardW, kGroupH, kCardRound, TFT_BLACK);
    g_cjkText.drawText(56, kGrpY_System + 16, "阅读字体", kGrayText);
    drawCyclingRow(kGrpY_System + 58, "字体", curFontName);
    canvas_->drawFastHLine(56, kGrpY_System + kRowDividerY, 424, kGrayMid);
    drawCyclingRow(kGrpY_System + 118, "返回", "← 设置");

    // Bottom hint
    // drawFooterHint("点击数值切换；左右滑动返回");
}

// ── Settings: Refresh ─────────────────────────────────────────────────────────

void VinkUiRenderer::renderSettingsRefresh() {
    if (!canvas_) return;
    clear();
    drawStatusBar("刷新策略");
    drawTabs(SystemState::Settings);

    const auto& cfg = g_configService.get();

    // Three strategy cards
    const char* strategies[3] = { "极速", "均衡", "清晰" };
    const char* hints[3] = {
        "DU4 快刷，每20页 GC16 全刷",
        "DU 快刷，每10页 GC16 全刷",
        "GL16 文本刷新，每5页 GC16 全刷"
    };
    RefreshFrequency freqs[3] = {
        RefreshFrequency::FREQ_LOW,
        RefreshFrequency::FREQ_MEDIUM,
        RefreshFrequency::FREQ_HIGH
    };

    for (int i = 0; i < 3; ++i) {
        const int16_t cardY = kGrpY_Reading + i * (kGroupH / 2 + 8);
        const bool active = (cfg.refreshFrequency == freqs[i]);
        canvas_->fillRoundRect(kCardX, cardY, kCardW, kGroupH / 2, kCardRound, TFT_WHITE);
        canvas_->drawRoundRect(kCardX, cardY, kCardW, kGroupH / 2, kCardRound, TFT_BLACK);
        if (active) {
            canvas_->drawRoundRect(kCardX + 3, cardY + 3, kCardW - 6, kGroupH / 2 - 6, kCardRound - 2, TFT_BLACK);
        }
        g_cjkText.drawText(56, cardY + 12, strategies[i], active ? TFT_BLACK : kGrayText);
        g_cjkText.drawText(56, cardY + 44, hints[i], kGrayText);
    }

    // drawFooterHint("选择刷新策略；滑动返回");
}

// ── Settings: WiFi ─────────────────────────────────────────────────────────────

void VinkUiRenderer::renderSettingsWifi() {
    if (!canvas_) return;
    clear();
    drawStatusBar("WiFi 配置");
    drawTabs(SystemState::Settings);

    const auto& cfg = g_configService.get();

    // Current status card
    canvas_->fillRoundRect(kCardX, kGrpY_Reading, kCardW, kGroupH, kCardRound, TFT_WHITE);
    canvas_->drawRoundRect(kCardX, kGrpY_Reading, kCardW, kGroupH, kCardRound, TFT_BLACK);
    g_cjkText.drawText(56, kGrpY_Reading + 16, "当前 WiFi", kGrayText);
    {
        static char buf[64];
        if (cfg.wifiSsid.isEmpty()) {
            snprintf(buf, sizeof(buf), "未配置");
        } else {
            snprintf(buf, sizeof(buf), "%s", cfg.wifiSsid.c_str());
        }
        drawCyclingRow(kGrpY_Reading + 58, "SSID", cfg.wifiSsid.isEmpty() ? "未配置" : cfg.wifiSsid.c_str());
        canvas_->drawFastHLine(56, kGrpY_Reading + kRowDividerY, 424, kGrayMid);
        drawCyclingRow(kGrpY_Reading + 118, "密码", cfg.wifiPassword.isEmpty() ? "无" : "已设置");
    }

    // Info card
    drawCard(kCardX, kGrpY_Display, kCardW, kGroupH,
        "说明",
        "SSID/密码可在 Web UI 或从 SD 卡配置文件写入，当前版本暂不支持屏幕内输入");

    // Back button
    drawButton(56, kGrpY_Connect + 30, 180, 48, "← 返回");
    drawButton(304, kGrpY_Connect + 30, 180, 48, "保存");
    // drawFooterHint("WiFi 配置建议在 Transfer → WiFi → 启动热点后，通过 Web UI 设置");
}

// ── Settings: Legado ─────────────────────────────────────────────────────────

void VinkUiRenderer::renderSettingsLegado() {
    if (!canvas_) return;
    clear();
    drawStatusBar("Legado 配置");
    drawTabs(SystemState::Settings);

    const auto& cfg = g_configService.get();

    // Connection card
    canvas_->fillRoundRect(kCardX, kGrpY_Reading, kCardW, kGroupH, kCardRound, TFT_WHITE);
    canvas_->drawRoundRect(kCardX, kGrpY_Reading, kCardW, kGroupH, kCardRound, TFT_BLACK);
    g_cjkText.drawText(56, kGrpY_Reading + 16, "服务器", kGrayText);
    drawCyclingRow(kGrpY_Reading + 58, "地址",
        cfg.legadoHost.isEmpty() ? "未配置" : cfg.legadoHost.c_str());
    canvas_->drawFastHLine(56, kGrpY_Reading + kRowDividerY, 424, kGrayMid);
    {
        static char buf[16];
        snprintf(buf, sizeof(buf), "%d", cfg.legadoPort);
        drawCyclingRow(kGrpY_Reading + 118, "端口", cfg.legadoHost.isEmpty() ? "1122" : buf);
    }

    // Status card
    canvas_->fillRoundRect(kCardX, kGrpY_Display, kCardW, kGroupH, kCardRound, TFT_WHITE);
    canvas_->drawRoundRect(kCardX, kGrpY_Display, kCardW, kGroupH, kCardRound, TFT_BLACK);
    g_cjkText.drawText(56, kGrpY_Display + 16, "状态", kGrayText);
    drawCyclingRow(kGrpY_Display + 58, "启用",
        cfg.legadoEnabled ? "已启用" : "未启用");
    canvas_->drawFastHLine(56, kGrpY_Display + kRowDividerY, 424, kGrayMid);
    drawCyclingRow(kGrpY_Display + 118, "Token",
        cfg.legadoToken.isEmpty() ? "未设置" : "已设置");

    // Info card
    drawCard(kCardX, kGrpY_Connect, kCardW, kGroupH,
        "说明",
        "地址格式：http://手机IP，默认端口 1122；Token 在 Legado App 设置中查看");

    // Back button
    drawButton(56, kGrpY_System, 180, 48, "← 返回");
    drawButton(304, kGrpY_System, 180, 48, "保存");
    // drawFooterHint("配置后可从「同步」标签发起 Legado 书架与进度同步");
}

// ── Settings: System / About ──────────────────────────────────────────────────

void VinkUiRenderer::renderSettingsSystem() {
    if (!canvas_) return;
    clear();
    drawStatusBar("系统信息");
    drawTabs(SystemState::Settings);

    auto stats = g_configService.storageStats();

    // Version card
    canvas_->fillRoundRect(kCardX, kGrpY_Reading, kCardW, kGroupH, kCardRound, TFT_WHITE);
    canvas_->drawRoundRect(kCardX, kGrpY_Reading, kCardW, kGroupH, kCardRound, TFT_BLACK);
    g_cjkText.drawText(56, kGrpY_Reading + 16, "版本", kGrayText);
    drawCyclingRow(kGrpY_Reading + 58, "固件", kVinkPaperS3FirmwareVersion);
    canvas_->drawFastHLine(56, kGrpY_Reading + kRowDividerY, 424, kGrayMid);
    drawCyclingRow(kGrpY_Reading + 118, "阅读内核", kReadPaperUpstreamVersion);

    // Storage card
    canvas_->fillRoundRect(kCardX, kGrpY_Display, kCardW, kGroupH, kCardRound, TFT_WHITE);
    canvas_->drawRoundRect(kCardX, kGrpY_Display, kCardW, kGroupH, kCardRound, TFT_BLACK);
    g_cjkText.drawText(56, kGrpY_Display + 16, "存储", kGrayText);
    {
        static char buf[32];
        snprintf(buf, sizeof(buf), "%u / %u KB",
            stats.usedBytes / 1024, stats.totalBytes / 1024);
        drawCyclingRow(kGrpY_Display + 58, "SPIFFS", buf);
    }
    canvas_->drawFastHLine(56, kGrpY_Display + kRowDividerY, 424, kGrayMid);
    {
        static char buf[16];
        uint32_t s = stats.uptimeSeconds;
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", s / 3600, (s % 3600) / 60, s % 60);
        drawCyclingRow(kGrpY_Display + 118, "运行时长", buf);
    }

    // Auto-sleep card
    const auto& cfg = g_configService.get();
    canvas_->fillRoundRect(kCardX, kGrpY_Connect, kCardW, kGroupH, kCardRound, TFT_WHITE);
    canvas_->drawRoundRect(kCardX, kGrpY_Connect, kCardW, kGroupH, kCardRound, TFT_BLACK);
    g_cjkText.drawText(56, kGrpY_Connect + 16, "自动休眠", kGrayText);
    drawCyclingRow(kGrpY_Connect + 58, "状态",
        cfg.autoSleepEnabled ? "开启" : "关闭");
    canvas_->drawFastHLine(56, kGrpY_Connect + kRowDividerY, 424, kGrayMid);
    drawCyclingRow(kGrpY_Connect + 118, "时间",
        cfg.autoSleepEnabled ? labelForAutoSleep(cfg.autoSleepMinutes) : "—");

    // Back
    drawButton(56, kGrpY_System, 180, 48, "← 返回");
    drawButton(304, kGrpY_System, 180, 48, "保存");
    // drawFooterHint("运行时长和存储占用实时刷新");
}

// ── Diagnostics ───────────────────────────────────────────────────────────────

void VinkUiRenderer::renderDiagnostics(const Message& lastTouch, const char* eventName) {
    if (!canvas_) return;
    clear();

    char line[128];
    // Use built-in ASCII drawing here only for diagnostic/boot visibility checks.
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
    snprintf(line, sizeof(line), "rotation=%u logical=%dx%d",
              gPaperS3ActiveDisplayRotation, kPaperS3Width, kPaperS3Height);
    canvas_->drawString(line, 48, 198);
    snprintf(line, sizeof(line), "M5.Display=%dx%d", M5.Display.width(), M5.Display.height());
    canvas_->drawString(line, 48, 226);
    snprintf(line, sizeof(line), "USB:%s CHG:%s BAT:%.2fV",
             isOfficialUsbConnected() ? "IN" : "--",
             isOfficialChargeStateActive() ? "ON" : "--",
             readOfficialBatteryVoltage());
    canvas_->drawString(line, 48, 254);
    canvas_->drawString("If visible: Vink canvas takeover works", 48, 282);

    canvas_->drawRoundRect(24, 342, 492, 178, 14, TFT_BLACK);
    canvas_->setTextSize(2);
    canvas_->drawString("TOUCH RAW", 48, 364);
    canvas_->setTextSize(1);
    snprintf(line, sizeof(line), "event: %s  count:%ld",
             eventName ? eventName : "wait", static_cast<long>(lastTouch.value));
    canvas_->drawString(line, 48, 404);
    snprintf(line, sizeof(line), "raw: %d, %d",
             lastTouch.rawTouch.x, lastTouch.rawTouch.y);
    canvas_->drawString(line, 48, 436);
    snprintf(line, sizeof(line), "norm: %d, %d",
             lastTouch.touch.x, lastTouch.touch.y);
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

// ── Shutdown / Legado sync ────────────────────────────────────────────────────

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
    renderLegadoSync(status, -1, nullptr);
}

void VinkUiRenderer::renderLegadoSync(const char* status, int bookCount, const char* errorMsg) {
    if (!canvas_) return;
    clear();
    drawStatusBar("Legado 同步");
    drawTabs(SystemState::Transfer);

    // Main status card
    canvas_->fillRoundRect(kCardX, kContentY, kCardW, 220, kCardRound, TFT_WHITE);
    canvas_->drawRoundRect(kCardX, kContentY, kCardW, 220, kCardRound, TFT_BLACK);
    g_cjkText.drawText(56, kContentY + 16, "同步状态", kGrayText);
    g_cjkText.drawText(56, kContentY + 58, status ? status : "等待同步", TFT_BLACK);

    if (bookCount >= 0) {
        char buf[48];
        snprintf(buf, sizeof(buf), "书架图书：%d 本", bookCount);
        g_cjkText.drawText(56, kContentY + 100, buf, kGrayText);
    }
    if (errorMsg && errorMsg[0]) {
        g_cjkText.drawText(56, kContentY + 140, errorMsg, TFT_RED);
    }

    // Last sync info
    {
        char buf[64];
        bool lc = g_legadoService.isConnected();
        bool la = g_legadoService.isConfigured();
        snprintf(buf, sizeof(buf), "服务器：%s",
                 la ? (g_configService.get().legadoHost.isEmpty() ? "未配置" : g_configService.get().legadoHost.c_str()) : "未配置");
        g_cjkText.drawText(56, kContentY + 178, buf, kGrayText);
    }

    drawButton(56, 346, 180, 48, "← 返回");
    drawButton(304, 346, 180, 48, "重试");
    // drawFooterHint("冲突时不自动覆盖；不确定时显示状态让用户选择");
}

// ── Hit test ──────────────────────────────────────────────────────────────────

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
            // Buttons sit inside the current-book card; test visible buttons before the surrounding card.
            if (inRect(x, y, 304, 286, 180, 48)) return UiAction::OpenLibrary;
            if (inRect(x, y, 56, 286, 180, 48)) return UiAction::OpenCurrentBook;
            if (inRect(x, y, 28, kContentY, 484, 184)) return UiAction::OpenCurrentBook;
            break;

        case SystemState::Library:
            break;

        case SystemState::Transfer:
            // Legado card area → Legado status sub-page
            if (inRect(x, y, 28, kContentY, 484, 172)) return UiAction::OpenTransferLegado;
            // "立即同步" button
            if (inRect(x, y, 56, 334, 180, 48)) return UiAction::StartLegadoSync;
            // "连接配置" button → Legado settings sub-page
            if (inRect(x, y, 304, 334, 180, 48)) return UiAction::OpenSettingsLegado;
            // WiFi card → WiFi AP sub-page
            if (inRect(x, y, 28, 392, 224, 120)) return UiAction::OpenTransferWifiAp;
            // USB MSC card → USB confirm sub-page
            if (inRect(x, y, 288, 392, 224, 120)) return UiAction::OpenTransferUsb;
            break;

        // ── Transfer sub-pages ─────────────────────────────────────
        case SystemState::TransferLegadoStatus:
            // Back / Sync buttons
            if (inRect(x, y, 56, kGrpY_Connect + 20, 180, 48)) return UiAction::TabTransfer;
            if (inRect(x, y, 304, kGrpY_Connect + 20, 180, 48)) return UiAction::StartLegadoSync;
            // Auto-sync toggle row in last-sync card
            if (inRect(x, y, kRowX, kGrpY_Display + 118, kValueRight - kRowX + 60, kRowH)) {
                return UiAction::CycleLegadoSyncEnabled;
            }
            break;

        case SystemState::TransferWifiAp:
            // Back button
            if (inRect(x, y, 56, kGrpY_Connect + 20, 180, 48)) return UiAction::TabTransfer;
            // Start hotspot button
            if (inRect(x, y, 304, kGrpY_Connect + 20, 180, 48)) return UiAction::ToggleWifiAp;
            // Three mode option cards
            {
                constexpr int16_t optH = 58;
                for (int i = 0; i < 3; ++i) {
                    const int16_t optY = kGrpY_Display + i * (optH + 4);
                    if (inRect(x, y, kCardX, optY, kCardW, optH)) {
                        if (i == 0) return UiAction::SetWifiOff;
                        if (i == 1) return UiAction::SetWifiApWebUi;
                        return UiAction::SetWifiSta;
                    }
                }
            }
            break;

        case SystemState::TransferUsb:
            // Confirm area (large black button)
            if (inRect(x, y, 56, kGrpY_Display + 20, 384, 64)) return UiAction::OpenTransferUsb;
            // Back button
            if (inRect(x, y, 56, kGrpY_Connect + 20, 180, 48)) return UiAction::TabTransfer;
            break;

        case SystemState::TransferExport:
            // Three export option cards
            {
                constexpr int16_t optH = 58;
                for (int i = 0; i < 3; ++i) {
                    const int16_t optY = kGrpY_Reading + i * (optH + 4);
                    if (inRect(x, y, kCardX, optY, kCardW, optH)) {
                        return UiAction::OpenTransferExport;  // stub
                    }
                }
            }
            if (inRect(x, y, 56, kGrpY_Connect + 20, 180, 48)) return UiAction::TabTransfer;
            break;

        // ── Settings main page ────────────────────────────────────
        case SystemState::Settings:
            // Back nav row (电源/关于 area, below last group)
            if (inRect(x, y, 56, kGrpY_System + 118, 424, kRowH)) return UiAction::OpenSettingsSystem;
            // "关于" row
            if (inRect(x, y, kRowX, kGrpY_System + 118, kValueRight - kRowX + 60, kRowH)) {
                return UiAction::OpenSettingsSystem;
            }
            // System: 电源 row
            if (inRect(x, y, kRowX, kGrpY_System + 58, kValueRight - kRowX + 60, kRowH)) {
                return UiAction::RequestShutdown;
            }
            // Connection group rows
            if (inRect(x, y, kRowX, kGrpY_Connect + 58, kValueRight - kRowX + 60, kRowH)) {
                return UiAction::OpenSettingsWifi;
            }
            if (inRect(x, y, kRowX, kGrpY_Connect + 118, kValueRight - kRowX + 60, kRowH)) {
                return UiAction::OpenSettingsLegado;
            }
            // Display group rows
            if (inRect(x, y, kRowX, kGrpY_Display + 58, kValueRight - kRowX + 60, kRowH)) {
                return UiAction::OpenSettingsRefresh;
            }
            if (inRect(x, y, kRowX, kGrpY_Display + 118, kValueRight - kRowX + 60, kRowH)) {
                return UiAction::OpenDiagnostics;
            }
            // Reading group rows
            if (inRect(x, y, kRowX, kGrpY_Reading + 58, kValueRight - kRowX + 60, kRowH)) {
                return UiAction::OpenSettingsLayout;
            }
            if (inRect(x, y, kRowX, kGrpY_Reading + 118, kValueRight - kRowX + 60, kRowH)) {
                return UiAction::OpenSettingsLayout;
            }
            break;

        // ── Settings sub-pages ─────────────────────────────────────
        case SystemState::SettingsLayout:
            // Font size row (cycling)
            if (inRect(x, y, kRowX, kGrpY_Reading + 58, kValueRight - kRowX + 40, kRowH)) {
                return UiAction::CycleFontSize;
            }
            // Line spacing row (cycling)
            if (inRect(x, y, kRowX, kGrpY_Reading + 118, kValueRight - kRowX + 40, kRowH)) {
                return UiAction::CycleLineSpacing;
            }
            // Margins row (cycles left/right together)
            if (inRect(x, y, kRowX, kGrpY_Display + 58, kValueRight - kRowX + 40, kRowH)) {
                return UiAction::CycleMarginLeft;
            }
            // Justify row (cycling)
            if (inRect(x, y, kRowX, kGrpY_Display + 118, kValueRight - kRowX + 40, kRowH)) {
                return UiAction::CycleJustify;
            }
            // Simplified Chinese (cycling)
            if (inRect(x, y, kRowX, kGrpY_Connect + 58, kValueRight - kRowX + 40, kRowH)) {
                return UiAction::CycleSimplified;
            }
            // Back row (group 3)
            if (inRect(x, y, kRowX, kGrpY_Connect + 118, kValueRight - kRowX + 40, kRowH)) {
                return UiAction::TabSettings;
            }
            // Font family row (group 4)
            if (inRect(x, y, kRowX, kGrpY_System + 58, kValueRight - kRowX + 40, kRowH)) {
                return UiAction::CycleFontFamily;
            }
            // Back row (group 4)
            if (inRect(x, y, kRowX, kGrpY_System + 118, kValueRight - kRowX + 40, kRowH)) {
                return UiAction::TabSettings;
            }
            break;

        case SystemState::SettingsRefresh:
            // Three strategy cards
            for (int i = 0; i < 3; ++i) {
                const int16_t cardY = kGrpY_Reading + i * (kGroupH / 2 + 8);
                if (inRect(x, y, kCardX, cardY, kCardW, kGroupH / 2)) {
                    return UiAction::CycleRefreshFrequency;  // cycle handled in state machine
                }
            }
            break;

        case SystemState::SettingsWifi:
            if (inRect(x, y, 56, kGrpY_Connect + 30, 180, 48)) return UiAction::TabSettings;
            if (inRect(x, y, 304, kGrpY_Connect + 30, 180, 48)) return UiAction::SaveWifiSettings;
            break;

        case SystemState::SettingsLegado:
            // Back button
            if (inRect(x, y, 56, kGrpY_System, 180, 48)) return UiAction::TabSettings;
            // "启用" cycling row in Status card
            if (inRect(x, y, kRowX, kGrpY_Display + 58, kValueRight - kRowX + 40, kRowH))
                return UiAction::CycleLegadoEnabled;
            // Save button
            if (inRect(x, y, 304, kGrpY_System, 180, 48)) return UiAction::SaveLegadoSettings;
            break;

        case SystemState::SettingsSystem:
            // Auto-sleep status cycling
            if (inRect(x, y, kRowX, kGrpY_Connect + 58, kValueRight - kRowX + 40, kRowH)) {
                return UiAction::OpenSettingsSystem; // future: toggle auto-sleep
            }
            if (inRect(x, y, 56, kGrpY_System, 180, 48)) return UiAction::TabSettings;
            if (inRect(x, y, 304, kGrpY_System, 180, 48)) return UiAction::OpenSettingsSystem; // save
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
