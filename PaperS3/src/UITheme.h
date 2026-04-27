#pragma once
#include "Config.h"
#include <M5Unified.h>

// ===== Crosslink 风格 UI 主题 =====
// 配色：浅灰白纸质感，墨水屏原生风格

namespace UITheme {
    // PaperS3's 4bpp gray fill is rendered as a visible halftone pattern.
    // Keep the shell UI pure black/white until the full ReadPaper canvas path
    // is adopted; otherwise large gray backgrounds look like screen noise.
    // Direct M5.Display drawing expects RGB565 colors, not 4bpp canvas indexes.
    // Using 15/0 directly on M5.Display caused the PaperS3 panel to dither large
    // areas into a noisy gray mesh. Keep the shell UI pure black/white.
    constexpr uint16_t BG_WHITE     = TFT_WHITE;  // 纯白
    constexpr uint16_t BG_LIGHT     = TFT_WHITE;  // 主背景保持纯白
    constexpr uint16_t BG_MID       = TFT_WHITE;  // 卡片背景保持纯白
    constexpr uint16_t BG_DARK      = TFT_BLACK;  // 深色区域
    
    // 文字色
    constexpr uint16_t TEXT_BLACK   = TFT_BLACK;  // 纯黑（标题）
    constexpr uint16_t TEXT_DARK    = TFT_BLACK;  // 正文
    constexpr uint16_t TEXT_MID     = TFT_BLACK;  // 次要文字先保持高对比
    constexpr uint16_t TEXT_LIGHT   = TFT_BLACK;  // 禁用/提示也先保证可读
    
    // 强调色
    constexpr uint16_t ACCENT       = TFT_BLACK;  // 选中态黑色
    constexpr uint16_t ACCENT_LIGHT = TFT_BLACK;
    
    // 边框/分隔线
    constexpr uint16_t BORDER       = TFT_BLACK;
    constexpr uint16_t BORDER_LIGHT = TFT_BLACK;
    
    // 尺寸常量
    constexpr int16_t TOP_TAB_H    = 88;   // Crosslink 顶部状态栏 + 标签栏高度
    constexpr int16_t BOTTOM_NAV_H = 0;    // M5PaperS3 无物理按键，无底部导航栏
    constexpr int16_t CARD_RADIUS  = 10;   // 卡片圆角
    constexpr int16_t ITEM_H       = 76;   // 列表项高度
    
    // 安全区域（减去顶部标签和底部导航）
    inline int16_t contentTop()    { return TOP_TAB_H + 16; }
    inline int16_t contentBottom() { return SCREEN_HEIGHT - BOTTOM_NAV_H - 18; }
    inline int16_t contentHeight() { return contentBottom() - contentTop(); }
    inline int16_t contentWidth()  { return SCREEN_WIDTH - 40; }
    inline int16_t contentLeft()   { return 20; }
    inline int16_t contentRight()  { return SCREEN_WIDTH - 20; }
    
    // 绘制工具函数
    void drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color);
    void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color);
    void drawCard(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t bgColor = BG_LIGHT, uint16_t borderColor = BORDER_LIGHT);
    void drawTabBookmark(int16_t x, int16_t y, int16_t w, int16_t h, bool active, const char* label, const char* icon);
    void drawCapsuleSwitch(int16_t x, int16_t y, int16_t w, bool on);
    void drawSlider(int16_t x, int16_t y, int16_t w, int16_t minVal, int16_t maxVal, int16_t current, const char* unit);
    void drawBottomNavItem(int16_t x, int16_t y, int16_t w, bool active, const char* icon, const char* label);
    void drawBookCover(int16_t x, int16_t y, int16_t w, int16_t h, const char* title, const char* author, int progressPercent);
    void drawSectionTitle(int16_t x, int16_t y, const char* title);
    void drawSeparator(int16_t x, int16_t y, int16_t w);
    void drawIconButton(int16_t x, int16_t y, int16_t size, const char* icon, uint16_t bgColor);
    
    // 辅助函数
    int16_t textWidth(const char* text, uint8_t textSize = 1);
    void drawTextCentered(int16_t x, int16_t y, int16_t w, int16_t h, const char* text, uint8_t textSize, uint16_t color);
    void drawTextRight(int16_t x, int16_t y, int16_t w, const char* text, uint8_t textSize, uint16_t color);
}
