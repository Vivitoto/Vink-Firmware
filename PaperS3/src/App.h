#pragma once
#include <Arduino.h>
#include "Config.h"
#include "FontManager.h"
#include "EbookReader.h"
#include "FileBrowser.h"
#include "WiFiUploader.h"
#include "ReadingStats.h"
#include "BlePageTurner.h"
#include "RecentBooks.h"
#include "LegadoSync.h"
#include "WebDavClient.h"

class App {
public:
    App();
    
    // 初始化
    bool init();
    
    // 主循环
    void run();
    
private:
    AppState _state;
    FontManager _uiFont;   // Shell/UI 固定内置字体，不受阅读字体切换影响
    FontManager _font;     // 阅读正文使用，可在阅读设置里切换
    EbookReader _reader;
    FileBrowser _browser;
    WiFiUploader _uploader;
    ReadingStats _stats;
    BlePageTurner _ble;
    RecentBooks _recent;
    LegadoSync _legado;
    
    // 空闲计时（休眠）/ 电源键
    unsigned long _lastActivityTime;
    bool _sleepPending;
    bool _powerButtonArmed;
    bool _toastVisible;
    bool _toastDirty;
    bool _toastClearDirty;
    bool _toastDrawn;
    AppState _toastDrawState;
    unsigned long _toastUntil;
    char _toastText[96];
    bool _shutdownInProgress;
    bool _sdReady;
    bool _pageNeedsRender;
    AppState _lastRenderedState;
    unsigned long _powerButtonPressStart;
    
    // 触摸状态
    bool _touching;
    bool _touchLongPressFired;
    bool _touchConsumed;
    bool _touchWaitRelease;
    int _touchStartX, _touchStartY;
    int _touchLastX, _touchLastY;
    unsigned long _touchStartTime;
    unsigned long _touchSuppressUntil;
    
    // 菜单状态
    int _menuIndex;           // 主菜单选中项
    int _layoutEditorIndex;   // 排版设置菜单选中项
    int _chapterMenuIndex;    // 章节菜单选中项
    int _chapterMenuScroll;   // 章节菜单滚动偏移
    int _bookmarkMenuIndex;   // 书签菜单选中项
    int _bookmarkMenuScroll;  // 书签菜单滚动偏移
    int _fontMenuIndex;       // 字体菜单选中项
    int _fontMenuScroll;      // 字体菜单滚动偏移
    int _settingsScroll;      // 设置页面滚动偏移
    
    // 当前激活标签页
    int _activeTab;           // 0=读书 1=书架 2=传输 3=设置
    bool _tabNeedsRender[4];  // 各标签页是否需要重绘
    
    // 书架9宫格
    int _libraryPage;         // 书架当前页
    int _libraryTotalPages;   // 书架总页数
    int _librarySelected;     // 书架选中索引
    
    // 页码跳转
    char _gotoPageInput[8];
    int _gotoPageCursor;
    
    // WiFi 配置
    char _wifiSsid[32];
    char _wifiPass[32];
    bool _wifiConfigured;
    
    // Legado 配置
    char _legadoHost[64];
    int _legadoPort;
    char _legadoUser[32];
    char _legadoPass[32];
    bool _legadoConfigured;
    
    // 全局设置
    uint8_t _sleepTimeoutMin;  // 休眠时长（分钟）
    
    // 可用字体列表
    char _fontPaths[10][128];
    char _fontNames[10][64];
    int _fontCount;
    
    // 初始化各模块
    bool initDisplay();
    bool initSD();
    bool initFont();
    void scanFonts();
    
    // 状态处理
    void handleInit();
    void handleTabReading();     // 📖 读书主页
    void handleTabLibrary();     // 📚 书架9宫格
    void handleTabTransfer();    // 🛜 传输中心
    void handleTabSettings();    // ⚙️ 设置页
    void handleReader();
    void handleReaderMenu();     // 阅读时弹出菜单
    void handleChapterList();    // 章节目录
    void handleBookmarkList();   // 书签列表
    void handleGotoPage();       // 页码跳转
    void handleReadingStats();   // 阅读统计
    void handleWiFiUpload();     // WiFi传书界面
    void handleSettingsLayout(); // 排版设置
    void handleSettingsRefresh();// 残影控制
    void handleSettingsFont();   // 字体切换
    void handleSettingsWiFi();   // WiFi配置
    void handleSettingsLegado(); // Legado同步
    void handleEndOfBook();      // 书末处理
    void handleWiFiConfig();     // WiFi配置简化界面
    void handleLegadoSync();     // Legado同步界面
    void syncLegadoProgress();   // 执行同步
    void loadWiFiConfig();       // 加载WiFi配置
    void saveWiFiConfig();       // 保存WiFi配置
    void loadLegadoConfig();     // 加载Legado配置
    void saveLegadoConfig();     // 保存Legado配置
    void drawSleepScreen();      // 休眠屏
    
    // 标签页导航
    void renderTopTabs();
    void renderBottomNav();
    void switchTab(int tab);
    void navigateBack();
    bool isTabState(AppState state);
    
    // 旧函数名兼容
    void handleFileBrowser();    // → handleTabReading
    void handleMenu();           // → 旧菜单，保留兼容
    void handleChapterMenu();    // → handleChapterList
    void handleLayoutMenu();     // → handleSettingsLayout
    void handleRefreshMenu();    // → handleSettingsRefresh
    void handleBookmarkMenu();   // → handleBookmarkList
    void handleFontMenu();       // → handleSettingsFont
    
    // 触摸处理
    void processTouch();
    void onTap(int x, int y);
    void onSwipe(int dx, int dy);
    void onVerticalSwipe(int dy);  // 垂直滑动（菜单选择）
    void onLongPress(int x, int y);
    
    // 菜单执行
    void executeMenuItem(int index);
    
    // 排版参数调整
    void adjustLayoutParameter(int delta);
    int clamp(int val, int minVal, int maxVal);
    
    // 全局设置
    void loadGlobalSettings();
    void saveGlobalSettings();
    
    // 电量显示
    void drawBatteryIcon(int x, int y);
    
    // 电源 / 休眠管理
    void handlePowerButton();
    void shutdownDevice(const char* reason = "正在关机...");
    void checkAutoSleep();
    void enterSleep();
    void wakeUp();
    
    // BLE 翻页器
    void checkBleCommands();
    
    // 显示消息
    void showMessage(const char* msg, int durationMs = 2000);
    void showToast(const char* msg, int durationMs = 1600);
    void serviceToast();
    bool drawToastNow();
    bool clearToastNow();
};
