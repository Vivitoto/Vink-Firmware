#pragma once
#include <Arduino.h>

namespace vink3 {

enum class SystemState : uint8_t {
    Boot,
    Home,
    Library,
    Reader,
    ReaderMenu,
    Transfer,
    Settings,
    // Settings sub-pages
    SettingsLayout,     // 阅读排版：字体/字号/行距/边距/两端对齐
    SettingsRefresh,    // 显示刷新：极速/均衡/清晰
    SettingsWifi,        // WiFi 配置
    SettingsLegado,      // Legado 配置
    SettingsSystem,      // 系统信息 / 关于
    // ── Transfer sub-pages ───────────────────────────────────────
    TransferLegadoStatus,  // Legado 连接状态 / 立即同步
    TransferWifiAp,        // WiFi 热点模式
    TransferUsb,           // USB MSC 确认
    TransferExport,        // 导出 / 截图
    // ── Diagnostics ──────────────────────────────────────────────
    Diagnostics,
    LegadoSync,
    Sleeping,
    Shutdown,
};

enum class MessageType : uint8_t {
    None,
    BootComplete,
    TouchDown,
    TouchMove,
    TouchUp,
    Tap,
    SwipeLeft,
    SwipeRight,
    SwipeUp,
    SwipeDown,
    LongPress,
    DisplayDone,
    OpenBook,
    PageNext,
    PagePrev,
    LegadoSyncStart,
    LegadoSyncDone,
    LegadoSyncFailed,
    SleepTimeout,
    PowerButton,
};

struct TouchPoint {
    int16_t x;
    int16_t y;

    TouchPoint() : x(0), y(0) {}
    TouchPoint(int16_t px, int16_t py) : x(px), y(py) {}
};

struct Message {
    MessageType type = MessageType::None;
    uint32_t timestampMs = 0;
    // `touch` is Vink's normalized 540x960 logical portrait coordinate.
    TouchPoint touch{};
    // `rawTouch` preserves the raw coordinate returned by M5Unified/GT911 for
    // hardware diagnostics before Vink hit-testing or clamping.
    TouchPoint rawTouch{};
    int32_t value = 0;
    // Generic extra payload for service→state messages (e.g. book count).
    uint32_t scratch = 0;
};

} // namespace vink3
