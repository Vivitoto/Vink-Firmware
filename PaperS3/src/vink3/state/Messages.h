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
};

} // namespace vink3
