#include "App.h"
#include "UITheme.h"
#include <M5Unified.h>

using namespace UITheme;

static FontManager* gUiFont = nullptr;

static constexpr int PAPER_S3_DISPLAY_ROTATION = 0;  // user-facing portrait, handle at top
static constexpr uint32_t SHELL_FULL_REFRESH_EVERY = 20;
static uint32_t gShellCommitCount = 0;

static void prepareShellFrame();
static void commitShellFrame();
static void drawBackHeader(const char* title, const char* hint = "点 < / 右滑 / 上滑返回");
static void drawUiText(int16_t x, int16_t y, const char* text, uint16_t color = TEXT_BLACK, uint16_t bg = BG_LIGHT);
static void drawUiTextCentered(int16_t x, int16_t y, int16_t w, int16_t h, const char* text, uint16_t color = TEXT_BLACK);
static void drawUiSectionTitle(int16_t x, int16_t y, const char* title);
static void drawUiCapsuleSwitch(int16_t x, int16_t y, int16_t w, bool on);
static void drawUiSlider(int16_t x, int16_t y, int16_t w, int16_t minVal, int16_t maxVal, int16_t current, const char* unit);
static void drawCrosslinkStatusBar();
static void drawBookCoverCard(int16_t x, int16_t y, int16_t w, int16_t h, const char* title, const char* type, int progress, bool dark = false);
static void drawSmallProgress(int16_t x, int16_t y, int16_t w, int percent);
static void drawRowIcon(int16_t x, int16_t y, const char* kind);

App::App() : _state(AppState::INIT), _reader(_font), _touching(false), _touchLongPressFired(false),
             _menuIndex(0), _layoutEditorIndex(0), _chapterMenuIndex(0), _chapterMenuScroll(0),
             _bookmarkMenuIndex(0), _bookmarkMenuScroll(0),
             _fontMenuIndex(0), _fontMenuScroll(0), _settingsScroll(0),
             _activeTab(0), _libraryPage(0), _libraryTotalPages(1), _librarySelected(0),
             _gotoPageCursor(0),
             _lastActivityTime(0), _sleepPending(false), _powerButtonArmed(false),
             _shutdownInProgress(false), _powerButtonPressStart(0),
             _sleepTimeoutMin(AUTO_SLEEP_DEFAULT_MIN),
             _fontCount(0) {
    memset(_gotoPageInput, 0, sizeof(_gotoPageInput));
    memset(_wifiSsid, 0, sizeof(_wifiSsid));
    memset(_wifiPass, 0, sizeof(_wifiPass));
    memset(_legadoHost, 0, sizeof(_legadoHost));
    memset(_legadoUser, 0, sizeof(_legadoUser));
    memset(_legadoPass, 0, sizeof(_legadoPass));
    _wifiConfigured = false;
    _legadoConfigured = false;
    _legadoPort = 80;
    _touchStartX = _touchStartY = _touchLastX = _touchLastY = 0;
    for (int i = 0; i < 4; i++) _tabNeedsRender[i] = true;
}

bool App::init() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n[Vink-PaperS3] Starting...");
    Serial.printf("[Boot] PSRAM size=%u free=%u\n", ESP.getPsramSize(), ESP.getFreePsram());
    
    auto cfg = M5.config();
    // Follow the proven ReadPaper/PaperS3 startup path: use M5Unified's
    // built-in PaperS3 board support and keep the e-paper panel stable across
    // wake/boot. The fallback board prevents occasional autodetect misses.
    cfg.clear_display = false;
    cfg.output_power = true;
    cfg.internal_imu = true;
    cfg.internal_rtc = true;
    cfg.internal_spk = false;
    cfg.internal_mic = false;
    cfg.fallback_board = m5::board_t::board_M5PaperS3;
    M5.begin(cfg);
    delay(50);
    
    if (!initDisplay()) {
        Serial.println("[App] Display init failed");
        return false;
    }
    
    bool sdReady = initSD();
    if (!sdReady) {
        showMessage("SD卡异常，继续启动", 3000);
        Serial.println("[App] SD init failed, continuing for boot diagnostics");
    }
    
    if (!initFont()) {
        showMessage("字体异常，使用备用字体", 2000);
        Serial.println("[App] Font init failed, continuing with built-in fallback if available");
    }
    // 主 UI 也必须走 FontManager，否则 M5GFX 内置字体无法完整显示中文。
    gUiFont = &_font;
    
    if (sdReady) {
        if (!SD.exists(BOOKS_DIR)) SD.mkdir(BOOKS_DIR);
        if (!SD.exists(PROGRESS_DIR)) SD.mkdir(PROGRESS_DIR);
    }
    
    // 扫描可用字体
    if (sdReady) scanFonts();
    
    if (sdReady) _browser.scan(BOOKS_DIR);
    _state = AppState::TAB_READING;
    _activeTab = 0;
    for (int i = 0; i < 4; i++) _tabNeedsRender[i] = true;
    
    loadGlobalSettings();
    _lastActivityTime = millis();
    _sleepPending = false;
    _powerButtonArmed = false;
    
    _stats.load();
    _recent.load();
    loadWiFiConfig();
    loadLegadoConfig();
    
    Serial.println("[App] Init complete");
    return true;
}

bool App::initDisplay() {
    auto& display = M5.Display;
    display.powerSaveOff();
    display.setEpdMode(epd_mode_t::epd_fastest);
    display.setColorDepth(4); // ReadPaper TEXT_COLORDEPTH
    // User's PaperS3 enclosure is physically used with the handle at the top.
    // Rotation 2 was upside down on the real device; flip 180° to the other
    // ReadPaper-supported portrait direction.
    display.setRotation(PAPER_S3_DISPLAY_ROTATION);
    delay(20);

    if (display.width() == 0 || display.height() == 0) {
        Serial.println("[Display] Display not detected");
        return false;
    }
    Serial.printf("[Display] %dx%d\n", display.width(), display.height());

    // ReadPaper-style draw path: render into a 4bpp M5Canvas and pushSprite,
    // instead of drawing directly to M5.Display then calling display().
    M5Canvas canvas(&display);
    canvas.setColorDepth(4);
    canvas.createSprite(SCREEN_WIDTH, SCREEN_HEIGHT);
    canvas.fillSprite(15);
    canvas.setTextColor(0, 15);
    canvas.setTextSize(2);
    canvas.setCursor(24, 32);
    canvas.println("Vink-PaperS3");
    canvas.setTextSize(1);
    canvas.setCursor(24, 76);
    canvas.println("v0.2.9 中文显示修复");
    canvas.setCursor(24, 104);
    canvas.printf("Display %dx%d", display.width(), display.height());
    canvas.setCursor(24, 132);
    canvas.printf("PSRAM %u KB", ESP.getPsramSize() / 1024);
    canvas.drawRect(24, 180, 180, 80, 0);
    canvas.fillRect(230, 180, 180, 80, 0);
    display.waitDisplay();
    canvas.pushSprite(0, 0);
    display.waitDisplay();
    delay(4000);
    return true;
}

bool App::initSD() {
    Serial.println("[SD] Initializing PaperS3 SD SPI bus...");
    // PaperS3 matches ReadPaper's verified pinout:
    // CS=47, SCK=39, MOSI=38, MISO=40. The old CS=4/SCK=14 path is for
    // other ESP32 boards and can make startup look dead after flashing.
    constexpr int SD_CS = 47;
    constexpr int SD_SCK = 39;
    constexpr int SD_MOSI = 38;
    constexpr int SD_MISO = 40;
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    const uint32_t freqs[] = {25000000u, 8000000u, 4000000u};
    for (uint32_t freq : freqs) {
        Serial.printf("[SD] Trying SPI freq=%u...\n", freq);
        if (SD.begin(SD_CS, SPI, freq)) {
            Serial.println("[SD] OK");
            return true;
        }
        delay(50);
    }
    Serial.println("[SD] PaperS3 SPI config failed");
    return false;
}

bool App::initFont() {
    if (_font.loadFont(FONT_FILE_24)) {
        if (strncmp(_font.getCurrentFontPath(), "builtin://", 10) == 0) {
            showMessage("使用内置字体", 1000);
        } else {
            Serial.printf("[Font] Loaded: %s\n", _font.getCurrentFontPath());
        }
        return true;
    }
    if (_font.loadFont(FONT_FILE_16)) {
        if (strncmp(_font.getCurrentFontPath(), "builtin://", 10) == 0) {
            showMessage("使用内置字体", 1000);
        } else {
            Serial.printf("[Font] Loaded: %s\n", _font.getCurrentFontPath());
        }
        return true;
    }
    Serial.println("[Font] No font available!");
    return false;
}

void App::scanFonts() {
    _fontCount = FontManager::scanFonts(_fontPaths, _fontNames, 10);
    Serial.printf("[Font] Scanned %d fonts\n", _fontCount);
}

void App::run() {
    while (true) {
        M5.update();
        handlePowerButton();
        processTouch();
        checkAutoSleep();
        checkBleCommands();
        _uploader.handleClient();
        
        switch (_state) {
            case AppState::INIT:
                handleInit();
                break;
            case AppState::TAB_READING:
                handleTabReading();
                break;
            case AppState::TAB_LIBRARY:
                handleTabLibrary();
                break;
            case AppState::TAB_TRANSFER:
                handleTabTransfer();
                break;
            case AppState::TAB_SETTINGS:
                handleTabSettings();
                break;
            case AppState::READER:
                handleReader();
                break;
            case AppState::READER_MENU:
                handleReaderMenu();
                break;
            case AppState::CHAPTER_LIST:
                handleChapterList();
                break;
            case AppState::BOOKMARK_LIST:
                handleBookmarkList();
                break;
            case AppState::GOTO_PAGE:
                handleGotoPage();
                break;
            case AppState::WIFI_UPLOAD:
                handleWiFiUpload();
                break;
            case AppState::READING_STATS:
                handleReadingStats();
                break;
            case AppState::SETTINGS_LAYOUT:
                handleSettingsLayout();
                break;
            case AppState::SETTINGS_REFRESH:
                handleSettingsRefresh();
                break;
            case AppState::SETTINGS_FONT:
                handleSettingsFont();
                break;
            case AppState::SETTINGS_WIFI:
                handleSettingsWiFi();
                break;
            case AppState::SETTINGS_LEGADO:
                handleSettingsLegado();
                break;
            default:
                break;
        }
        delay(50);
    }
}

void App::handleInit() {
    _browser.scan(BOOKS_DIR);
    _state = AppState::TAB_READING;
    _activeTab = 0;
    for (int i = 0; i < 4; i++) _tabNeedsRender[i] = true;
}

void App::handleFileBrowser() {
    static bool needsRender = true;
    if (needsRender) {
        auto& display = M5.Display;
        display.clear();
        
        // 标题
        display.setTextSize(2);
        display.setCursor(380, 15);
        display.println("书架");
        
        // 最近阅读
        int recentCount = _recent.getCount();
        if (recentCount > 0) {
            display.setTextSize(1);
            display.setCursor(40, 55);
            display.println("最近阅读:");
            
            for (int i = 0; i < recentCount && i < 3; i++) {
                const RecentBook* book = _recent.getBook(i);
                if (!book) continue;
                int y = 75 + i * 35;
                display.setCursor(60, y);
                int pct = _recent.getProgressPercent(i);
                String name = book->name;
                if (name.length() > 25) name = name.substring(0, 22) + "...";
                display.printf("%d. %s (%d%%)", i + 1, name.c_str(), pct);
            }
        }
        
        // 分隔线
        int fileStartY = recentCount > 0 ? 190 : 70;
        display.drawLine(30, fileStartY - 10, SCREEN_WIDTH - 30, fileStartY - 10, 0);
        
        display.setTextSize(1);
        display.setCursor(40, fileStartY);
        display.println("所有书籍:");
        
        // 文件列表
        _browser.renderAt(fileStartY + 25);
        
        display.display();
        needsRender = false;
    }
}

void App::handleReader() {
    // 阅读器由触摸事件驱动
}

// ===== 主菜单（分组式扁平列表）=====
void App::handleMenu() {
    auto& display = M5.Display;
    display.clear();
    
    display.setTextSize(2);
    display.setCursor(380, 20);
    display.println("阅读菜单");
    
    const char* items[] = {
        "继续阅读",
        "[阅读] 章节目录",
        "[阅读] 页码跳转",
        "[阅读] 添加书签",
        "[设置] 排版设置",
        "[设置] 字号调大",
        "[设置] 字号调小",
        "[设置] 残影控制",
        "[工具] 我的书签",
        "[工具] 字体切换",
        "[工具] WiFi传书",
        "[工具] 阅读统计",
        "[系统] 蓝牙翻页",
        "[系统] 返回书架",
        "[系统] 关闭设备",
        "WiFi配置",
        "Legado同步"
    };
    int numItems = 17;
    
    display.setTextSize(1);
    for (int i = 0; i < numItems; i++) {
        int y = 50 + i * 32;
        if (i == _menuIndex) {
            display.fillRect(150, y - 2, 660, 28, 2);
        }
        display.setCursor(170, y + 4);
        display.println(items[i]);
    }
    
    display.display();
}

// ===== 章节目录菜单 =====
void App::handleChapterMenu() {
    auto& display = M5.Display;
    display.clear();
    
    display.setTextSize(2);
    display.setCursor(360, 20);
    display.println("章节目录");
    
    int chapterCount = _reader.getChapterCount();
    if (chapterCount <= 0) {
        display.setTextSize(1);
        display.setCursor(300, 250);
        display.println("未检测到章节");
        display.setCursor(250, 280);
        display.println("（返回阅读后长按菜单识别）");
        display.display();
        return;
    }
    
    const ChapterInfo* chapters = _reader.getChapterList();
    int currentChapter = _reader.getCurrentChapterIndex();
    
    int itemsPerPage = 10;
    int totalPages = (chapterCount + itemsPerPage - 1) / itemsPerPage;
    int currentPage = _chapterMenuScroll / itemsPerPage;
    int startIdx = currentPage * itemsPerPage;
    int endIdx = min(startIdx + itemsPerPage, chapterCount);
    
    display.setTextSize(1);
    for (int i = startIdx; i < endIdx; i++) {
        int y = 70 + (i - startIdx) * 40;
        if (i == _chapterMenuIndex) {
            display.fillRect(150, y - 3, 660, 35, 2);
        }
        if (i == currentChapter) {
            display.drawRect(145, y - 5, 670, 39, 0);
        }
        display.setCursor(170, y + 5);
        String title = chapters[i].title;
        if (title.length() > 30) title = title.substring(0, 27) + "...";
        display.printf("%d. %s", i + 1, title.c_str());
    }
    
    display.setCursor(400, 480);
    display.printf("%d/%d 页", currentPage + 1, totalPages);
    
    display.display();
}

// ===== 残影控制菜单 =====
void App::handleRefreshMenu() {
    auto& display = M5.Display;
    display.clear();
    
    display.setTextSize(2);
    display.setCursor(360, 20);
    display.println("残影控制");
    
    RefreshStrategy strategy = _reader.getRefreshStrategy();
    const char* items[] = {
        "极速 - DU4快刷 / 每20页全刷",
        "均衡 - DU快刷 / 每10页全刷",
        "清晰 - GL16文本 / 每5页全刷"
    };
    int selected = (int)strategy.frequency;
    
    display.setTextSize(1);
    for (int i = 0; i < 3; i++) {
        int y = 100 + i * 60;
        if (i == selected) {
            display.fillRect(150, y - 5, 660, 50, 2);
        }
        display.setCursor(170, y + 10);
        display.println(items[i]);
    }
    
    display.setCursor(200, 400);
    display.println("Vink 刷新策略：速度优先 / 均衡 / 显示优先");
    display.setCursor(200, 450);
    display.println("← → 切换 | 点击确认 | 上滑返回");
    
    display.display();
}

// ===== 书签菜单 =====
void App::handleBookmarkMenu() {
    auto& display = M5.Display;
    display.clear();
    
    display.setTextSize(2);
    display.setCursor(360, 20);
    display.println("我的书签");
    
    int bmCount = _reader.getBookmarkCount();
    if (bmCount <= 0) {
        display.setTextSize(1);
        display.setCursor(320, 250);
        display.println("暂无书签");
        display.setCursor(250, 280);
        display.println("（阅读时点击菜单 → 添加书签）");
        display.display();
        return;
    }
    
    const Bookmark* bookmarks = _reader.getBookmarks();
    int itemsPerPage = 10;
    int totalPages = (bmCount + itemsPerPage - 1) / itemsPerPage;
    int currentPage = _bookmarkMenuScroll / itemsPerPage;
    int startIdx = currentPage * itemsPerPage;
    int endIdx = min(startIdx + itemsPerPage, bmCount);
    
    display.setTextSize(1);
    for (int i = startIdx; i < endIdx; i++) {
        int y = 70 + (i - startIdx) * 40;
        if (i == _bookmarkMenuIndex) {
            display.fillRect(150, y - 3, 660, 35, 2);
        }
        display.setCursor(170, y + 5);
        String name = bookmarks[i].name;
        if (name.length() > 30) name = name.substring(0, 27) + "...";
        display.printf("%d. %s (P%d)", i + 1, name.c_str(), bookmarks[i].pageNum + 1);
    }
    
    display.setCursor(400, 480);
    display.printf("%d 个书签 | %d/%d 页", bmCount, currentPage + 1, totalPages);
    
    display.display();
}

// ===== 排版设置子菜单 =====
void App::handleLayoutMenu() {
    auto& display = M5.Display;
    display.clear();
    
    display.setTextSize(2);
    display.setCursor(360, 20);
    display.println("排版设置");
    
    LayoutConfig layout = _reader.getLayoutConfig();
    
    struct LayoutItem {
        const char* name;
        int value;
        const char* unit;
        int minVal;
        int maxVal;
        int step;
    };
    
    LayoutItem items[] = {
        {"字号", layout.fontSize, "px", 12, 48, 1},
        {"行间距", layout.lineSpacing, "%", 50, 200, 10},
        {"段间距", layout.paragraphSpacing, "%", 0, 100, 10},
        {"左页边", layout.marginLeft, "px", 0, 120, 5},
        {"右页边", layout.marginRight, "px", 0, 120, 5},
        {"上页边", layout.marginTop, "px", 0, 100, 5},
        {"下页边", layout.marginBottom, "px", 0, 100, 5},
        {"首行缩进", layout.indentFirstLine, "字", 0, 4, 1},
        {"休眠时长", (int)_sleepTimeoutMin, "分", AUTO_SLEEP_MIN_MIN, AUTO_SLEEP_MAX_MIN, 1},
    };
    int numItems = 9;
    
    display.setTextSize(1);
    for (int i = 0; i < numItems; i++) {
        int y = 70 + i * 45;
        if (i == _layoutEditorIndex) {
            display.fillRect(150, y - 3, 660, 38, 2);
        }
        display.setCursor(170, y + 5);
        display.printf("%s: %d %s", items[i].name, items[i].value, items[i].unit);
    }
    
    display.setCursor(200, 480);
    display.println("← → 调整 | 点击确认 | 上滑返回");
    
    display.display();
}

static void normalizePaperS3TouchPoint(int rawX, int rawY, int& outX, int& outY) {
    // M5Unified normally returns display-rotation-aware coordinates. Keep this
    // guard defensive: if a backend reports landscape/raw coordinates, normalize
    // them into Vink's logical 540x960 portrait space.
    int x = rawX;
    int y = rawY;
    if (x >= 0 && y >= 0 && x < SCREEN_WIDTH && y < SCREEN_HEIGHT) {
        outX = x;
        outY = y;
        return;
    }

    // Raw GT911/PaperS3 coordinates may be reported in portrait and then need
    // a transform when display rotation changes. Current requested orientation
    // is rotation 0, so this is identity; the other cases are kept here for the
    // next rotation change instead of scattering math through the UI.
    switch (PAPER_S3_DISPLAY_ROTATION) {
        case 1:
            outX = SCREEN_HEIGHT - 1 - rawY;
            outY = rawX;
            break;
        case 2:
            outX = SCREEN_WIDTH - 1 - rawX;
            outY = SCREEN_HEIGHT - 1 - rawY;
            break;
        case 3:
            outX = rawY;
            outY = SCREEN_WIDTH - 1 - rawX;
            break;
        case 0:
        default:
            outX = rawX;
            outY = rawY;
            break;
    }

    outX = constrain(outX, 0, SCREEN_WIDTH - 1);
    outY = constrain(outY, 0, SCREEN_HEIGHT - 1);
}

void App::processTouch() {
    auto& touch = M5.Touch;
    constexpr int TAP_THRESHOLD = 26;
    constexpr int SWIPE_THRESHOLD = 64;
    constexpr int LONG_PRESS_MS = 750;
    constexpr int LONG_PRESS_MOVE = 28;
    constexpr int MAX_TAP_MS = 650;

    if (touch.getCount() > 0) {
        _lastActivityTime = millis();
        _sleepPending = false;
        auto t = touch.getDetail(0);
        int x = 0, y = 0;
        normalizePaperS3TouchPoint(t.x, t.y, x, y);

        if (!_touching) {
            _touching = true;
            _touchLongPressFired = false;
            _touchStartX = _touchLastX = x;
            _touchStartY = _touchLastY = y;
            _touchStartTime = millis();
            return;
        }

        _touchLastX = x;
        _touchLastY = y;
        int dx = _touchLastX - _touchStartX;
        int dy = _touchLastY - _touchStartY;
        unsigned long dt = millis() - _touchStartTime;

        if (!_touchLongPressFired && dt >= LONG_PRESS_MS && abs(dx) < LONG_PRESS_MOVE && abs(dy) < LONG_PRESS_MOVE) {
            _touchLongPressFired = true;
            onLongPress(_touchStartX, _touchStartY);
        }
        return;
    }

    if (_touching) {
        _touching = false;
        int dx = _touchLastX - _touchStartX;
        int dy = _touchLastY - _touchStartY;
        unsigned long dt = millis() - _touchStartTime;

        if (_touchLongPressFired) {
            _touchLongPressFired = false;
            return;
        }

        int absDx = abs(dx);
        int absDy = abs(dy);
        if (absDx < TAP_THRESHOLD && absDy < TAP_THRESHOLD && dt <= MAX_TAP_MS) {
            onTap(_touchStartX, _touchStartY);
        } else if (absDx >= SWIPE_THRESHOLD || absDy >= SWIPE_THRESHOLD) {
            if (absDx > absDy) onSwipe(dx, dy);
            else onVerticalSwipe(dy);
        }
    }
}

void App::onTap(int x, int y) {
    Serial.printf("[Touch] Tap at (%d, %d)\n", x, y);

    // 子页面左上角返回按钮：所有设置/统计/菜单页统一可退回上一级。
    if (!isTabState(_state) && _state != AppState::READER && y < 92 && x < 120) {
        navigateBack();
        return;
    }

    if (_state == AppState::SETTINGS_REFRESH) {
        int startY = 116;
        int itemH = 128;
        int gap = 20;
        int idx = (y - startY) / (itemH + gap);
        if (idx >= 0 && idx < 3) {
            _reader.setRefreshFrequency((RefreshFrequency)idx);
            handleSettingsRefresh();
        }
        return;
    }

    if (_state == AppState::SETTINGS_FONT) {
        int idx = (y - 112) / 92;
        if (idx >= 0 && idx < _fontCount) {
            if (_font.loadFont(_fontPaths[idx])) showMessage("字体已切换", 800);
            _state = AppState::TAB_SETTINGS;
            _tabNeedsRender[3] = true;
        }
        return;
    }

    if (_state == AppState::SETTINGS_LAYOUT) {
        _layoutEditorIndex = ((_layoutEditorIndex + 1) % 5);
        handleSettingsLayout();
        return;
    }

    if (_state == AppState::SETTINGS_WIFI || _state == AppState::SETTINGS_LEGADO) {
        navigateBack();
        return;
    }
    
    // ===== 新 UI 标签页处理 =====
    if (_state == AppState::TAB_READING || _state == AppState::TAB_LIBRARY ||
        _state == AppState::TAB_TRANSFER || _state == AppState::TAB_SETTINGS) {
        
        // 顶部标签切换：与 Crosslink 书签式 Tab 的实际绘制区域一致。
        if (y < TOP_TAB_H) {
            const int tabBaseX = 20;
            const int tabW = (SCREEN_WIDTH - 40) / 4;
            int clickedTab = (x - tabBaseX) / tabW;
            if (x >= tabBaseX && x <= SCREEN_WIDTH - 20 && clickedTab >= 0 && clickedTab < 4) {
                switchTab(clickedTab);
            }
            return;
        }
        
        // 底部导航区域（不做处理，只是提示）
        if (y > SCREEN_HEIGHT - BOTTOM_NAV_H) {
            return;
        }
        
        // 根据标签页处理
        switch (_state) {
            case AppState::TAB_READING: {
                int recentCount = _recent.getCount();
                int cardY = contentTop() + 42;
                if (x >= contentLeft() && x <= contentRight() && y >= cardY && y <= cardY + 178) {
                    if (recentCount > 0) {
                        const RecentBook* book = _recent.getBook(0);
                        if (book && _reader.openBook(book->path)) {
                            _stats.startReading(book->name);
                            _state = AppState::READER;
                            _reader.renderPage();
                        }
                    } else {
                        switchTab(1);
                    }
                    return;
                }
                return;
            }
            
            case AppState::TAB_LIBRARY: {
                if (_browser.getItemCount() == 0) {
                    if (x >= contentLeft() + 80 && x <= contentRight() - 80 && y >= contentTop() + 260 && y <= contentTop() + 328) {
                        _browser.scan(BOOKS_DIR);
                        _tabNeedsRender[1] = true;
                        showMessage("已重新扫描SD卡", 800);
                    }
                    return;
                }
                const int cols = 3;
                const int booksPerPage = 9;
                const int gapX = 12;
                const int gapY = 18;
                const int cardW = (contentWidth() - 12 - gapX * 2) / 3;
                const int cardH = 168;
                int startX = contentLeft() + 6;
                int cy = contentTop() + 50;
                
                int clickedCol = (x - startX) / (cardW + gapX);
                int clickedRow = (y - cy) / (cardH + gapY);
                if (clickedCol >= 0 && clickedCol < cols && clickedRow >= 0 && clickedRow < 3) {
                    int localIdx = clickedRow * cols + clickedCol;
                    int idx = _libraryPage * booksPerPage + localIdx;
                    if (idx < _browser.getItemCount()) {
                        _librarySelected = idx;
                        const FileItem* item = _browser.getItem(idx);
                        if (item && item->isDirectory) {
                            if (_browser.enterDirectory(idx)) {
                                _libraryPage = 0;
                                _librarySelected = 0;
                                _tabNeedsRender[1] = true;
                            }
                        } else if (item && _reader.openBook(item->path)) {
                            _stats.startReading(item->name);
                            _recent.addBook(item->path, item->name, 0, _reader.getTotalPages());
                            _state = AppState::READER;
                            _reader.renderPage();
                        }
                    }
                }
                return;
            }
            
            case AppState::TAB_TRANSFER: {
                const int rowH = 74;
                int panelY = contentTop() + 54;
                int rowStart = panelY + 116;
                int clickedIdx = -1;
                if (x >= contentLeft() + 6 && x <= contentRight() - 6 && y >= rowStart && y <= rowStart + rowH * 6) {
                    clickedIdx = (y - rowStart) / rowH;
                }

                switch (clickedIdx) {
                    case 0:
                        if (_uploader.isRunning()) {
                            _uploader.stop();
                            showMessage("WiFi传书已关闭", 1500);
                        } else {
                            _uploader.start(_wifiSsid, _wifiPass);
                            showMessage("WiFi传书已开启", 1500);
                        }
                        _tabNeedsRender[2] = true;
                        break;
                    case 1:
                        _state = AppState::SETTINGS_WIFI;
                        break;
                    case 2:
                        _state = AppState::SETTINGS_LEGADO;
                        break;
                    case 3:
                        if (_ble.isRunning()) {
                            _ble.stop();
                            showMessage("蓝牙翻页已关闭", 1500);
                        } else {
                            _ble.start();
                            showMessage("蓝牙翻页已开启", 1500);
                        }
                        _tabNeedsRender[2] = true;
                        break;
                    case 4:
                        _state = AppState::READING_STATS;
                        break;
                    case 5:
                        switchTab(1);
                        break;
                }
                return;
            }
            
            case AppState::TAB_SETTINGS: {
                const int rowH = 64;
                int panelY = contentTop() + 26;
                int drawY = panelY + 74 - _settingsScroll;
                int clickedIdx = -1;
                const int groupBefore[] = {0, 2, 4, 6};
                for (int i = 0; i < 8; i++) {
                    bool newGroup = (i == groupBefore[0] || i == groupBefore[1] || i == groupBefore[2] || i == groupBefore[3]);
                    if (newGroup) drawY += 34;
                    if (y >= drawY && y <= drawY + rowH) {
                        clickedIdx = i;
                        break;
                    }
                    drawY += rowH;
                }

                if (clickedIdx >= 0 && clickedIdx < 8) {
                    switch (clickedIdx) {
                        case 0: _fontMenuIndex = 0; _fontMenuScroll = 0; _state = AppState::SETTINGS_FONT; break;
                        case 1: _layoutEditorIndex = 0; _state = AppState::SETTINGS_LAYOUT; break;
                        case 2: _layoutEditorIndex = 0; _state = AppState::SETTINGS_LAYOUT; break;
                        case 3: _state = AppState::SETTINGS_REFRESH; break;
                        case 4: _state = AppState::SETTINGS_WIFI; break;
                        case 5: _state = AppState::SETTINGS_LEGADO; break;
                        case 6:
                            _sleepTimeoutMin = clamp(_sleepTimeoutMin + 5, AUTO_SLEEP_MIN_MIN, AUTO_SLEEP_MAX_MIN);
                            if (_sleepTimeoutMin > AUTO_SLEEP_MAX_MIN) _sleepTimeoutMin = AUTO_SLEEP_MIN_MIN;
                            saveGlobalSettings();
                            _tabNeedsRender[3] = true;
                            break;
                        case 7: showMessage("Vink-PaperS3 v0.2.9", 3000); break;
                    }
                }
                return;
            }
            
            default:
                break;
        }
        return;
    }
    
    // ===== 原有状态处理 =====
    switch (_state) {
        case AppState::FILE_BROWSER: {
            // 点击最近阅读区域
            if (y < 190 && _recent.getCount() > 0) {
                int recentIdx = (y - 75) / 35;
                if (recentIdx >= 0 && recentIdx < _recent.getCount() && recentIdx < 3) {
                    const RecentBook* book = _recent.getBook(recentIdx);
                    if (book && _reader.openBook(book->path)) {
                        _stats.startReading(book->name);
                        _state = AppState::READER;
                        _reader.renderPage();
                        return;
                    }
                }
            }
            // 右侧打开选中项目
            if (x >= ZONE_RIGHT_X) {
                const FileItem* item = _browser.getSelectedItem();
                if (item && item->isDirectory) {
                    if (_browser.enterDirectory(_browser.getSelectedIndex())) {
                        _browser.render();
                    } else {
                        showMessage("进入目录失败!");
                    }
                } else if (item && _reader.openBook(item->path)) {
                    _stats.startReading(item->name);
                    _recent.addBook(item->path, item->name, 0, _reader.getTotalPages());
                    _state = AppState::READER;
                    _reader.renderPage();
                } else {
                    showMessage("打开失败!");
                }
            }
            break;
        }
        
        case AppState::READER: {
            if (x < ZONE_LEFT_X + ZONE_LEFT_W) {
                _reader.prevPage();
                _stats.recordPageTurn();
            } else if (x >= ZONE_RIGHT_X) {
                if (!_reader.nextPage()) {
                    // 书末
                    handleEndOfBook();
                    return;
                }
                _stats.recordPageTurn();
                _recent.updateProgress(_reader.getBookPath(), _reader.getCurrentPage(), _reader.getTotalPages());
            } else {
                _menuIndex = 0;
                _state = AppState::MENU;
                handleMenu();
            }
            break;
        }
        
        case AppState::MENU: {
            int clickedIndex = (y - 50) / 32;
            if (clickedIndex < 0) clickedIndex = 0;
            if (clickedIndex > 16) clickedIndex = 16;
            _menuIndex = clickedIndex;
            executeMenuItem(clickedIndex);
            break;
        }
        
        case AppState::MENU_CHAPTER: {
            if (_reader.getChapterCount() > 0) {
                _reader.gotoChapter(_chapterMenuIndex);
                _state = AppState::READER;
            }
            break;
        }
        
        case AppState::MENU_REFRESH: {
            _state = AppState::READER;
            _reader.renderPage();
            break;
        }
        
        case AppState::MENU_BOOKMARK: {
            if (_reader.getBookmarkCount() > 0) {
                _reader.gotoBookmark(_bookmarkMenuIndex);
                _state = AppState::READER;
            }
            break;
        }
        
        case AppState::MENU_FONT: {
            if (_fontCount > 0 && _fontMenuIndex < _fontCount) {
                if (_font.loadFont(_fontPaths[_fontMenuIndex])) {
                    showMessage("字体已切换", 1000);
                    if (_reader.isOpen()) {
                        _reader.renderPage();
                    }
                }
            }
            _state = AppState::READER;
            break;
        }
        
        case AppState::MENU_GOTO_PAGE: {
            // 数字键盘区域: 300-620 x, 300-450 y
            if (y >= 300 && y < 450 && x >= 300 && x < 660) {
                int col = (x - 300) / 120;
                int row = (y - 300) / 50;
                int keyIdx = row * 3 + col;
                
                if (keyIdx == 9) { // 删除
                    if (_gotoPageCursor > 0) {
                        _gotoPageCursor--;
                        _gotoPageInput[_gotoPageCursor] = '\0';
                    }
                } else if (keyIdx == 11) { // 跳转
                    if (_gotoPageCursor > 0) {
                        int page = atoi(_gotoPageInput) - 1; // 用户输入1-based
                        if (page >= 0 && page < _reader.getTotalPages()) {
                            _reader.gotoPage(page);
                            _state = AppState::READER;
                        } else {
                            showMessage("页码超出范围");
                        }
                    }
                } else if (keyIdx >= 0 && keyIdx < 9) { // 1-9
                    if (_gotoPageCursor < 6) {
                        _gotoPageInput[_gotoPageCursor++] = '1' + keyIdx;
                        _gotoPageInput[_gotoPageCursor] = '\0';
                    }
                } else if (keyIdx == 10) { // 0
                    if (_gotoPageCursor < 6) {
                        _gotoPageInput[_gotoPageCursor++] = '0';
                        _gotoPageInput[_gotoPageCursor] = '\0';
                    }
                }
                handleGotoPage();
            } else if (y < 200) {
                // 点击输入框上方=取消
                _state = AppState::READER;
                _reader.renderPage();
            }
            break;
        }
        
        case AppState::WIFI_UPLOAD:
        case AppState::READING_STATS: {
            _state = AppState::READER;
            _reader.renderPage();
            break;
        }
        
        case AppState::MENU_LAYOUT: {
            _state = AppState::READER;
            _reader.renderPage();
            break;
        }
        
        default:
            break;
    }
}

void App::onSwipe(int dx, int dy) {
    Serial.printf("[Touch] Swipe dx=%d, dy=%d\n", dx, dy);

    // 子页面右滑返回上一级；比“上滑返回”更符合电子书/手机习惯。
    if (!isTabState(_state) && _state != AppState::READER && dx > 90) {
        navigateBack();
        return;
    }
    
    // 新UI标签页：左右滑动切换标签
    if (isTabState(_state)) {
        if (dx > 80) {
            // 右滑：上一个标签
            switchTab(_activeTab - 1);
        } else if (dx < -80) {
            // 左滑：下一个标签
            switchTab(_activeTab + 1);
        }
        return;
    }

    if (_state == AppState::SETTINGS_LAYOUT) {
        adjustLayoutParameter(dx > 0 ? 1 : -1);
        handleSettingsLayout();
        return;
    }
    
    switch (_state) {
        case AppState::FILE_BROWSER: {
            if (dy > 30) {
                _browser.selectNext();
                _browser.render();
            } else if (dy < -30) {
                _browser.selectPrev();
                _browser.render();
            }
            break;
        }
        
        case AppState::READER: {
            if (dx < -50) {
                if (!_reader.nextPage()) {
                    handleEndOfBook();
                    return;
                }
                _stats.recordPageTurn();
                _recent.updateProgress(_reader.getBookPath(), _reader.getCurrentPage(), _reader.getTotalPages());
            } else if (dx > 50) {
                _reader.prevPage();
                _stats.recordPageTurn();
                _recent.updateProgress(_reader.getBookPath(), _reader.getCurrentPage(), _reader.getTotalPages());
            }
            break;
        }
        
        case AppState::MENU_LAYOUT: {
            adjustLayoutParameter(dx > 0 ? 1 : -1);
            handleLayoutMenu();
            break;
        }
        
        case AppState::MENU_CHAPTER: {
            if (dx < -50 && _chapterMenuScroll < _reader.getChapterCount() - 1) {
                _chapterMenuScroll += 10;
                handleChapterMenu();
            } else if (dx > 50 && _chapterMenuScroll > 0) {
                _chapterMenuScroll -= 10;
                handleChapterMenu();
            }
            break;
        }
        
        case AppState::MENU_FONT: {
            if (dx < -50 && _fontMenuScroll < _fontCount - 1) {
                _fontMenuScroll += 10;
                handleFontMenu();
            } else if (dx > 50 && _fontMenuScroll > 0) {
                _fontMenuScroll -= 10;
                handleFontMenu();
            }
            break;
        }
        
        case AppState::MENU_BOOKMARK: {
            int bmCount = _reader.getBookmarkCount();
            if (dx < -50 && _bookmarkMenuScroll < bmCount - 1) {
                _bookmarkMenuScroll += 10;
                handleBookmarkMenu();
            } else if (dx > 50 && _bookmarkMenuScroll > 0) {
                _bookmarkMenuScroll -= 10;
                handleBookmarkMenu();
            }
            break;
        }
        
        case AppState::MENU_REFRESH: {
            RefreshStrategy current = _reader.getRefreshStrategy();
            RefreshFrequency newFreq;
            if (dx > 0) {
                newFreq = (current.frequency == RefreshFrequency::FREQ_LOW) ? RefreshFrequency::FREQ_MEDIUM :
                          (current.frequency == RefreshFrequency::FREQ_MEDIUM) ? RefreshFrequency::FREQ_HIGH :
                          RefreshFrequency::FREQ_HIGH;
            } else {
                newFreq = (current.frequency == RefreshFrequency::FREQ_HIGH) ? RefreshFrequency::FREQ_MEDIUM :
                          (current.frequency == RefreshFrequency::FREQ_MEDIUM) ? RefreshFrequency::FREQ_LOW :
                          RefreshFrequency::FREQ_LOW;
            }
            _reader.setRefreshFrequency(newFreq);
            handleRefreshMenu();
            break;
        }
        
        default:
            break;
    }
}

void App::onVerticalSwipe(int dy) {
    if (!isTabState(_state) && _state != AppState::READER && dy < -90) {
        navigateBack();
        return;
    }

    // 新UI：设置页面滚动
    if (_state == AppState::TAB_SETTINGS) {
        if (dy > 30) {
            _settingsScroll += 20;
            _tabNeedsRender[3] = true;
        } else if (dy < -30) {
            _settingsScroll -= 20;
            if (_settingsScroll < 0) _settingsScroll = 0;
            _tabNeedsRender[3] = true;
        }
        return;
    }
    
    // 新UI：阅读器菜单
    if (_state == AppState::READER_MENU) {
        if (dy < -30 && _menuIndex > 0) {
            _menuIndex--;
            handleReaderMenu();
        } else if (dy > 30 && _menuIndex < 11) {
            _menuIndex++;
            handleReaderMenu();
        } else if (dy > 100) {
            _state = AppState::READER;
            _reader.renderPage();
        }
        return;
    }
    
    switch (_state) {
        case AppState::MENU: {
            if (dy < -30 && _menuIndex > 0) {
                _menuIndex--;
                handleMenu();
            } else if (dy > 30 && _menuIndex < 16) {
                _menuIndex++;
                handleMenu();
            }
            break;
        }
        
        case AppState::MENU_CHAPTER: {
            int chapterCount = _reader.getChapterCount();
            if (dy < -30 && _chapterMenuIndex > 0) {
                _chapterMenuIndex--;
                if (_chapterMenuIndex < _chapterMenuScroll) {
                    _chapterMenuScroll = (_chapterMenuIndex / 10) * 10;
                }
                handleChapterMenu();
            } else if (dy > 30 && _chapterMenuIndex < chapterCount - 1) {
                _chapterMenuIndex++;
                if (_chapterMenuIndex >= _chapterMenuScroll + 10) {
                    _chapterMenuScroll = (_chapterMenuIndex / 10) * 10;
                }
                handleChapterMenu();
            } else if (dy > 100) {
                _state = AppState::READER;
                _reader.renderPage();
            }
            break;
        }
        
        case AppState::MENU_BOOKMARK: {
            int bmCount = _reader.getBookmarkCount();
            if (dy < -30 && _bookmarkMenuIndex > 0) {
                _bookmarkMenuIndex--;
                if (_bookmarkMenuIndex < _bookmarkMenuScroll) {
                    _bookmarkMenuScroll = (_bookmarkMenuIndex / 10) * 10;
                }
                handleBookmarkMenu();
            } else if (dy > 30 && _bookmarkMenuIndex < bmCount - 1) {
                _bookmarkMenuIndex++;
                if (_bookmarkMenuIndex >= _bookmarkMenuScroll + 10) {
                    _bookmarkMenuScroll = (_bookmarkMenuIndex / 10) * 10;
                }
                handleBookmarkMenu();
            } else if (dy > 100) {
                _state = AppState::READER;
                _reader.renderPage();
            }
            break;
        }
        
        case AppState::MENU_FONT: {
            if (dy < -30 && _fontMenuIndex > 0) {
                _fontMenuIndex--;
                handleFontMenu();
            } else if (dy > 30 && _fontMenuIndex < _fontCount - 1) {
                _fontMenuIndex++;
                handleFontMenu();
            } else if (dy > 100) {
                _state = AppState::READER;
                _reader.renderPage();
            }
            break;
        }
        
        case AppState::MENU_LAYOUT: {
            if (dy < -30 && _layoutEditorIndex > 0) {
                _layoutEditorIndex--;
                handleLayoutMenu();
            } else if (dy > 30) {
                if (_layoutEditorIndex < 8) {
                    _layoutEditorIndex++;
                    handleLayoutMenu();
                } else {
                    _state = AppState::READER;
                    _reader.renderPage();
                }
            }
            break;
        }
        
        default:
            break;
    }
}

void App::onLongPress(int x, int y) {
    Serial.printf("[Touch] Long press at (%d, %d)\n", x, y);
    if (_state == AppState::READER) {
        _menuIndex = 0;
        _state = AppState::READER_MENU;
        handleReaderMenu();
    }
}

void App::executeMenuItem(int index) {
    // 新UI：阅读器弹出菜单处理
    if (_state == AppState::READER_MENU) {
        switch (index) {
            case 0: { // 章节目录
                if (_reader.getChapterCount() <= 0) _reader.detectChapters();
                _chapterMenuIndex = max(0, _reader.getCurrentChapterIndex());
                _chapterMenuScroll = (_chapterMenuIndex / 10) * 10;
                _state = AppState::CHAPTER_LIST;
                break;
            }
            case 1: // 添加书签
                if (_reader.addBookmark()) showMessage("书签已添加", 1500);
                else showMessage("添加失败", 1500);
                _state = AppState::READER;
                _reader.renderPage();
                break;
            case 2: // 我的书签
                _bookmarkMenuIndex = 0;
                _bookmarkMenuScroll = 0;
                _state = AppState::BOOKMARK_LIST;
                break;
            case 3: // 页码跳转
                memset(_gotoPageInput, 0, sizeof(_gotoPageInput));
                _gotoPageCursor = 0;
                _state = AppState::GOTO_PAGE;
                handleGotoPage();
                break;
            case 4: // 排版设置
                _layoutEditorIndex = 0;
                _state = AppState::SETTINGS_LAYOUT;
                break;
            case 5: // 字体切换
                _fontMenuIndex = 0;
                _fontMenuScroll = 0;
                _state = AppState::SETTINGS_FONT;
                break;
            case 6: // 残影控制
                _state = AppState::SETTINGS_REFRESH;
                break;
            case 7: // WiFi传书
                if (_uploader.isRunning()) {
                    _uploader.stop();
                    showMessage("WiFi传书已关闭", 1500);
                } else {
                    _uploader.start(_wifiSsid, _wifiPass);
                    showMessage("WiFi传书已开启", 1500);
                }
                _state = AppState::READER;
                _reader.renderPage();
                break;
            case 8: // 阅读统计
                _state = AppState::READING_STATS;
                break;
            case 9: // 蓝牙翻页
                if (_ble.isRunning()) {
                    _ble.stop();
                    showMessage("蓝牙翻页已关闭", 1500);
                } else {
                    _ble.start();
                    showMessage("蓝牙翻页已开启", 1500);
                }
                _state = AppState::READER;
                _reader.renderPage();
                break;
            case 10: // 返回书架
                _stats.stopReading();
                _stats.save();
                _reader.closeBook();
                _browser.scan(BOOKS_DIR);
                _state = AppState::TAB_READING;
                _activeTab = 0;
                for (int i = 0; i < 4; i++) _tabNeedsRender[i] = true;
                break;
            case 11: // 关闭设备
                shutdownDevice("正在关机...");
                break;
        }
        return;
    }
    
    switch (index) {
        case 0: // 继续阅读
            _state = AppState::READER;
            _reader.renderPage();
            break;
            
        case 1: { // 章节目录
            if (_reader.getChapterCount() <= 0) {
                _reader.detectChapters();
            }
            _chapterMenuIndex = max(0, _reader.getCurrentChapterIndex());
            _chapterMenuScroll = (_chapterMenuIndex / 10) * 10;
            _state = AppState::MENU_CHAPTER;
            handleChapterMenu();
            break;
        }
        
        case 2: { // 页码跳转
            memset(_gotoPageInput, 0, sizeof(_gotoPageInput));
            _gotoPageCursor = 0;
            _state = AppState::MENU_GOTO_PAGE;
            handleGotoPage();
            break;
        }
        
        case 3: { // 添加书签
            if (_reader.addBookmark()) {
                showMessage("书签已添加", 1500);
            } else {
                showMessage("添加失败", 1500);
            }
            _state = AppState::READER;
            break;
        }
        
        case 4: // 排版设置
            _layoutEditorIndex = 0;
            _state = AppState::MENU_LAYOUT;
            handleLayoutMenu();
            break;
            
        case 5: // 字号调大
            _reader.changeFontSize(+2);
            _state = AppState::READER;
            break;
            
        case 6: // 字号调小
            _reader.changeFontSize(-2);
            _state = AppState::READER;
            break;
            
        case 7: // 残影控制
            _state = AppState::MENU_REFRESH;
            handleRefreshMenu();
            break;
            
        case 8: // 我的书签
            _bookmarkMenuIndex = 0;
            _bookmarkMenuScroll = 0;
            _state = AppState::MENU_BOOKMARK;
            handleBookmarkMenu();
            break;
            
        case 9: // 字体切换
            _fontMenuIndex = 0;
            _fontMenuScroll = 0;
            _state = AppState::MENU_FONT;
            handleFontMenu();
            break;
            
        case 10: { // WiFi传书
            if (_uploader.isRunning()) {
                _uploader.stop();
                showMessage("WiFi传书已关闭", 1500);
            } else {
                showMessage("请在配置中设置WiFi", 2000);
            }
            _state = AppState::READER;
            break;
        }
        
        case 11: // 阅读统计
            _state = AppState::READING_STATS;
            handleReadingStats();
            break;
            
        case 12: { // 蓝牙翻页
            if (_ble.isRunning()) {
                _ble.stop();
                showMessage("蓝牙翻页已关闭", 1500);
            } else {
                _ble.start();
                showMessage("蓝牙翻页已开启", 1500);
            }
            _state = AppState::READER;
            break;
        }
        
        case 13: // 返回书架
            _stats.stopReading();
            _stats.save();
            _reader.closeBook();
            _browser.scan(BOOKS_DIR);
            _state = AppState::TAB_READING;
            _activeTab = 0;
            for (int i = 0; i < 4; i++) _tabNeedsRender[i] = true;
            break;
            
        case 14: // 关闭设备
            shutdownDevice("正在关机...");
            break;
            
        case 15: // WiFi配置
            handleWiFiConfig();
            break;
            
        case 16: // Legado同步
            handleLegadoSync();
            break;
    }
}

void App::adjustLayoutParameter(int delta) {
    if (_layoutEditorIndex < 8) {
        LayoutConfig layout = _reader.getLayoutConfig();
        switch (_layoutEditorIndex) {
            case 0: layout.fontSize = clamp(layout.fontSize + delta, 12, 48); break;
            case 1: layout.lineSpacing = clamp(layout.lineSpacing + delta * 10, 50, 200); break;
            case 2: layout.paragraphSpacing = clamp(layout.paragraphSpacing + delta * 10, 0, 100); break;
            case 3: layout.marginLeft = clamp(layout.marginLeft + delta * 5, 0, 120); break;
            case 4: layout.marginRight = clamp(layout.marginRight + delta * 5, 0, 120); break;
            case 5: layout.marginTop = clamp(layout.marginTop + delta * 5, 0, 100); break;
            case 6: layout.marginBottom = clamp(layout.marginBottom + delta * 5, 0, 100); break;
            case 7: layout.indentFirstLine = clamp(layout.indentFirstLine + delta, 0, 4); break;
        }
        _reader.setLayoutConfig(layout);
    } else if (_layoutEditorIndex == 8) {
        // 休眠时长
        _sleepTimeoutMin = clamp((int)_sleepTimeoutMin + delta, AUTO_SLEEP_MIN_MIN, AUTO_SLEEP_MAX_MIN);
        saveGlobalSettings();
    }
}

int App::clamp(int val, int minVal, int maxVal) {
    if (val < minVal) return minVal;
    if (val > maxVal) return maxVal;
    return val;
}

void App::showMessage(const char* msg, int durationMs) {
    auto& display = M5.Display;
    display.clear();
    display.fillScreen(BG_LIGHT);
    drawUiTextCentered(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, msg, TEXT_BLACK);
    display.display();
    
    if (durationMs > 0) {
        delay(durationMs);
    }
}

// ===== 全局设置 =====

void App::loadGlobalSettings() {
    File f = SD.open(CONFIG_PATH, FILE_READ);
    if (!f) {
        Serial.println("[Settings] No config file, using defaults");
        _sleepTimeoutMin = AUTO_SLEEP_DEFAULT_MIN;
        return;
    }
    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        _sleepTimeoutMin = AUTO_SLEEP_DEFAULT_MIN;
        return;
    }
    _sleepTimeoutMin = doc["sleepTimeoutMin"] | AUTO_SLEEP_DEFAULT_MIN;
    if (_sleepTimeoutMin < AUTO_SLEEP_MIN_MIN) _sleepTimeoutMin = AUTO_SLEEP_MIN_MIN;
    if (_sleepTimeoutMin > AUTO_SLEEP_MAX_MIN) _sleepTimeoutMin = AUTO_SLEEP_MAX_MIN;
    Serial.printf("[Settings] Loaded: sleep=%d min\n", _sleepTimeoutMin);
}

void App::saveGlobalSettings() {
    File f = SD.open(CONFIG_PATH, FILE_WRITE);
    if (!f) return;
    DynamicJsonDocument doc(2048);
    doc["sleepTimeoutMin"] = _sleepTimeoutMin;
    doc["timestamp"] = millis();
    serializeJson(doc, f);
    f.close();
    Serial.printf("[Settings] Saved: sleep=%d min\n", _sleepTimeoutMin);
}

// ===== 电量显示 =====

void App::drawBatteryIcon(int x, int y) {
#if BATTERY_ICON_ENABLED
    auto& display = M5.Display;
    BatteryInfo bat = BatteryInfo::read();
    if (!bat.valid) return;
    
    display.drawRect(x, y, 24, 12, 0);
    display.drawRect(x + 24, y + 3, 3, 6, 0);
    
    int fillW = (bat.level * 20) / 100;
    uint16_t color = TFT_BLACK;
    
    if (fillW > 0) {
        display.fillRect(x + 2, y + 2, fillW, 8, color);
    }
    
    if (bat.charging) {
        display.drawPixel(x + 12, y + 6, 15);
    }
#endif
}

// BatteryInfo::read() 外部实现（Config.h 中只是声明，因 M5 尚未 include）
BatteryInfo BatteryInfo::read() {
    BatteryInfo info;
    info.level = M5.Power.getBatteryLevel();
    info.charging = M5.Power.isCharging();
    info.valid = (info.level >= 0 && info.level <= 100);
    return info;
}

// ===== 电源 / 休眠管理 =====

static constexpr int PAPER_S3_POWER_OFF_PIN = 44;
static constexpr int PAPER_S3_POWER_KEY_PIN = 36;

static void pulsePaperS3PowerOffPin() {
    // CrossPoint/ReadPaper 类 PaperS3 关机路径：GPIO44 给 PMIC 一个高脉冲。
    // M5Unified 的 powerOff 在部分情况下会退到 deep sleep/重启观感；这里显式
    // 先走 PaperS3 硬件关机脉冲，再让 M5Unified 做兜底。
    pinMode(PAPER_S3_POWER_OFF_PIN, OUTPUT);
    digitalWrite(PAPER_S3_POWER_OFF_PIN, HIGH);
    delay(150);
    digitalWrite(PAPER_S3_POWER_OFF_PIN, LOW);
    delay(100);
}

void App::handlePowerButton() {
    if (_shutdownInProgress) return;

    // 目标行为：按一次硬件开机；系统起来后，再按一次才关机。
    // 因此启动后的前几秒必须忽略“开机那次按键”的残留状态，等电源键
    // 释放后再正式接管，避免刚开机就被固件误判为关机请求。
    if (!_powerButtonArmed) {
        if (millis() > 3000 && !M5.BtnPWR.isPressed()) {
            _powerButtonArmed = true;
            _powerButtonPressStart = 0;
            Serial.println("[Power] BtnPWR armed after boot release");
        }
        return;
    }

    // 不等 wasClicked() 释放后才处理：PaperS3 的 PMIC 长按可能先触发硬件重启。
    // 接管后只要检测到一次稳定按下，就立即进入保存+关机流程。
    if (M5.BtnPWR.isPressed()) {
        if (_powerButtonPressStart == 0) {
            _powerButtonPressStart = millis();
        }
        if (millis() - _powerButtonPressStart > 80) {
            Serial.println("[Power] BtnPWR pressed, shutting down immediately");
            shutdownDevice("正在关机...");
        }
        return;
    }

    _powerButtonPressStart = 0;
    if (M5.BtnPWR.wasClicked() || M5.BtnPWR.wasHold()) {
        Serial.println("[Power] BtnPWR click/hold event, shutting down");
        shutdownDevice("正在关机...");
    }
}

void App::shutdownDevice(const char* reason) {
    if (_shutdownInProgress) return;
    _shutdownInProgress = true;

    if (_reader.isOpen()) {
        _reader.saveProgress();
    }
    _stats.save();
    if (_uploader.isRunning()) {
        _uploader.stop();
    }
    if (_ble.isRunning()) {
        _ble.stop();
    }

    // 参考 ReadPaper：先完整刷新关机提示并等待电子纸完成刷新，再断电。
    showMessage(reason ? reason : "正在关机...", 1200);
    delay(300);
    M5.Display.waitDisplay();
    delay(500);

    // 若用户仍按着电源键，先等释放，避免 deep sleep 兜底被 GPIO36 立即唤醒，
    // 看起来像“按电源键重启”。最多等 3 秒，防止异常卡死。
    unsigned long waitStart = millis();
    while (M5.BtnPWR.isPressed() && millis() - waitStart < 3000) {
        M5.update();
        delay(30);
    }

    pulsePaperS3PowerOffPin();
    M5.Display.waitDisplay();
    M5.Power.powerOff();

    // 正常不会走到这里。若 PMIC 未真正断电，退回深睡；进入深睡前再次确保
    // 电源键已释放，否则 ext0 低电平会立刻唤醒，表现成重启。
    waitStart = millis();
    while (digitalRead(PAPER_S3_POWER_KEY_PIN) == LOW && millis() - waitStart < 3000) {
        delay(30);
    }
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PAPER_S3_POWER_KEY_PIN, 0);
    esp_deep_sleep_start();
}

void App::checkAutoSleep() {
#if AUTO_SLEEP_ENABLED
    unsigned long timeoutMs = (unsigned long)_sleepTimeoutMin * 60 * 1000;
    unsigned long idle = millis() - _lastActivityTime;
    
    if (idle > timeoutMs) {
        if (!_sleepPending) {
            _sleepPending = true;
            Serial.printf("[Sleep] Idle %d min, entering sleep...\n", _sleepTimeoutMin);
            enterSleep();
        }
    }
#endif
}

void App::enterSleep() {
    if (_reader.isOpen()) {
        _reader.saveProgress();
    }
    
    // 绘制休眠屏
    drawSleepScreen();
    
    delay(500);
    
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_36, 0);
    esp_light_sleep_start();
    
    wakeUp();
}

void App::drawSleepScreen() {
    prepareShellFrame();
    auto& display = M5.Display;

    UITheme::drawCard(60, 250, SCREEN_WIDTH - 120, 300, BG_WHITE, BORDER_LIGHT);
    drawUiTextCentered(60, 280, SCREEN_WIDTH - 120, 42, "休眠中", TEXT_BLACK);

    if (_reader.isOpen()) {
        String name = _reader.getBookPath();
        int lastSlash = name.lastIndexOf('/');
        if (lastSlash >= 0) name = name.substring(lastSlash + 1);
        if (name.length() > 22) name = name.substring(0, 20) + "..";
        drawUiTextCentered(78, 340, SCREEN_WIDTH - 156, 36, name.c_str(), TEXT_BLACK);

        int pct = (_reader.getTotalPages() > 0) ? (_reader.getCurrentPage() * 100 / _reader.getTotalPages()) : 0;
        char progress[40];
        snprintf(progress, sizeof(progress), "阅读进度：%d%%", pct);
        drawUiTextCentered(78, 392, SCREEN_WIDTH - 156, 32, progress, TEXT_MID);
        drawSmallProgress(110, 440, SCREEN_WIDTH - 220, pct);
    } else {
        drawUiTextCentered(78, 350, SCREEN_WIDTH - 156, 36, "Vink-PaperS3", TEXT_BLACK);
    }

    BatteryInfo bat = BatteryInfo::read();
    if (bat.valid) {
        char batLine[32];
        snprintf(batLine, sizeof(batLine), "电量：%d%%", bat.level);
        drawUiTextCentered(78, 480, SCREEN_WIDTH - 156, 32, batLine, TEXT_MID);
    }

    drawUiTextCentered(0, 600, SCREEN_WIDTH, 36, "轻触屏幕唤醒", TEXT_MID);
    commitShellFrame();
}

void App::handleEndOfBook() {
    prepareShellFrame();
    UITheme::drawCard(60, 260, SCREEN_WIDTH - 120, 260, BG_WHITE, BORDER_LIGHT);
    drawUiTextCentered(60, 310, SCREEN_WIDTH - 120, 48, "本书已读完", TEXT_BLACK);
    drawUiTextCentered(60, 380, SCREEN_WIDTH - 120, 36, "感谢阅读", TEXT_MID);
    drawUiTextCentered(60, 450, SCREEN_WIDTH - 120, 36, "点击返回书架，左滑继续停留", TEXT_MID);
    commitShellFrame();
    
    // 等待用户操作
    unsigned long start = millis();
    while (millis() - start < 10000) {
        M5.update();
        auto& touch = M5.Touch;
        if (touch.getCount() > 0) {
            auto t = touch.getDetail(0);
            int tx = 0, ty = 0;
            normalizePaperS3TouchPoint(t.x, t.y, tx, ty);
            if (tx >= ZONE_RIGHT_X) {
                // 返回书架
                _stats.stopReading();
                _stats.save();
                _reader.closeBook();
                _browser.scan(BOOKS_DIR);
                _state = AppState::TAB_READING;
                _activeTab = 0;
                for (int i = 0; i < 4; i++) _tabNeedsRender[i] = true;
                return;
            } else if (tx < ZONE_LEFT_X + ZONE_LEFT_W) {
                // 左侧触摸=停留
                _state = AppState::READER;
                _reader.renderPage();
                return;
            }
        }
        delay(50);
    }
    
    // 超时自动返回书架
    _stats.stopReading();
    _stats.save();
    _reader.closeBook();
    _browser.scan(BOOKS_DIR);
    _state = AppState::TAB_READING;
    _activeTab = 0;
    for (int i = 0; i < 4; i++) _tabNeedsRender[i] = true;
}

void App::handleGotoPage() {
    auto& display = M5.Display;
    display.clear();
    
    display.setTextSize(2);
    display.setCursor(340, 60);
    display.println("页码跳转");
    
    display.setTextSize(1);
    display.setCursor(300, 140);
    display.printf("共 %d 页", _reader.getTotalPages());
    
    // 输入框
    display.drawRect(350, 200, 260, 60, 0);
    display.setTextSize(2);
    display.setCursor(370, 220);
    display.printf("%s", _gotoPageInput[0] ? _gotoPageInput : "_");
    
    // 数字键盘
    display.setTextSize(1);
    const char* keys[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "删除", "0", "跳转"};
    for (int i = 0; i < 12; i++) {
        int row = i / 3;
        int col = i % 3;
        int kx = 300 + col * 120;
        int ky = 300 + row * 50;
        display.drawRect(kx, ky, 100, 40, 0);
        display.setCursor(kx + 35, ky + 15);
        display.println(keys[i]);
    }
    
    display.display();
}

void App::wakeUp() {
    Serial.println("[Sleep] Woken up");
    _lastActivityTime = millis();
    _sleepPending = false;
    
    M5.Touch.begin(&M5.Display);
    
    if (_state == AppState::READER && _reader.isOpen()) {
        _reader.renderPage();
    } else if (isTabState(_state)) {
        _browser.render();
    } else {
        auto& display = M5.Display;
        display.clear();
        display.display();
    }
}

// ===== 字体切换菜单 =====

void App::handleFontMenu() {
    auto& display = M5.Display;
    display.clear();
    
    display.setTextSize(2);
    display.setCursor(360, 20);
    display.println("字体切换");
    
    if (_fontCount <= 0) {
        display.setTextSize(1);
        display.setCursor(300, 250);
        display.println("未找到字体文件");
        display.setCursor(250, 280);
        display.println("（请放入 /fonts/*.fnt）");
        display.display();
        return;
    }
    
    display.setTextSize(1);
    for (int i = 0; i < _fontCount; i++) {
        int y = 70 + i * 40;
        if (i == _fontMenuIndex) {
            display.fillRect(150, y - 3, 660, 35, 2);
        }
        // 当前字体标记
        bool isCurrent = (strcmp(_font.getCurrentFontPath(), _fontPaths[i]) == 0);
        display.setCursor(170, y + 5);
        display.printf("%s %s", _fontNames[i], isCurrent ? "[当前]" : "");
    }
    
    display.setCursor(200, 480);
    display.println("点击切换 | 上滑返回");
    
    display.display();
}

// ===== WiFi传书界面 =====

void App::handleWiFiUpload() {
    auto& display = M5.Display;
    display.clear();
    
    display.setTextSize(2);
    display.setCursor(320, 20);
    display.println("WiFi传书");
    
    display.setTextSize(1);
    if (_uploader.isRunning()) {
        display.setCursor(200, 100);
        display.println("WiFi 已连接");
        display.setCursor(200, 140);
        display.printf("IP: %s", _uploader.getIP().c_str());
        display.setCursor(200, 180);
        display.println("端口: 8080");
        display.setCursor(200, 240);
        display.println("手机/电脑浏览器访问上方地址");
        display.setCursor(200, 280);
        display.println("选择 .txt 文件上传");
    } else {
        display.setCursor(250, 200);
        display.println("WiFi传书未启动");
        display.setCursor(200, 240);
        display.println("请到菜单中开启");
    }
    
    if (_uploader.hasNewUpload()) {
        display.setCursor(200, 350);
        display.printf("新上传: %s", _uploader.getLastUploadName().c_str());
        _uploader.clearNewUpload();
        _browser.scan(BOOKS_DIR);
    }
    
    display.setCursor(250, 450);
    display.println("点击返回阅读");
    
    display.display();
}

// ===== 阅读统计 =====

void App::handleReadingStats() {
    auto& display = M5.Display;
    prepareShellFrame();
    drawBackHeader("阅读统计", "统计当前书籍和累计阅读");

    int x = contentLeft();
    int y = 120;
    UITheme::drawCard(x, y, contentWidth(), 280, BG_WHITE, BORDER_LIGHT);
    char statsLine[72];
    if (_reader.isOpen()) {
        BookStats book = _stats.getCurrentBookStats();
        snprintf(statsLine, sizeof(statsLine), "当前书翻页：%d 页", book.totalPagesRead);
        drawUiText(x + 18, y + 24, statsLine, TEXT_BLACK, BG_WHITE);
        snprintf(statsLine, sizeof(statsLine), "当前书阅读：%s", _stats.formatTime(book.totalSeconds).c_str());
        drawUiText(x + 18, y + 60, statsLine, TEXT_BLACK, BG_WHITE);
        snprintf(statsLine, sizeof(statsLine), "打开次数：%d", book.readCount);
        drawUiText(x + 18, y + 96, statsLine, TEXT_BLACK, BG_WHITE);
    } else {
        drawUiText(x + 18, y + 24, "当前没有打开书籍", TEXT_BLACK, BG_WHITE);
    }
    snprintf(statsLine, sizeof(statsLine), "累计阅读：%s", _stats.formatTime(_stats.getTotalReadingSeconds()).c_str());
    drawUiText(x + 18, y + 154, statsLine, TEXT_BLACK, BG_WHITE);
    snprintf(statsLine, sizeof(statsLine), "累计翻页：%d 页", _stats.getTotalPagesRead());
    drawUiText(x + 18, y + 190, statsLine, TEXT_BLACK, BG_WHITE);
    snprintf(statsLine, sizeof(statsLine), "今日阅读：%s / %d 页", _stats.formatTime(_stats.getTodaySeconds()).c_str(), _stats.getTodayPages());
    drawUiText(x + 18, y + 226, statsLine, TEXT_BLACK, BG_WHITE);
    commitShellFrame();
}

// ===== BLE 翻页器检查 =====

void App::checkBleCommands() {
    if (!_ble.isRunning()) return;
    int cmd = _ble.checkCommand();
    if (cmd == 1) {
        _reader.nextPage();
    } else if (cmd == -1) {
        _reader.prevPage();
    }
}

void App::handleLegadoSync() {
    auto& display = M5.Display;
    prepareShellFrame();
    drawBackHeader("Legado同步", "WebDAV 阅读进度同步");

    int x = contentLeft();
    int y = 124;
    UITheme::drawCard(x, y, contentWidth(), 220, BG_WHITE, BORDER_LIGHT);
    drawUiText(x + 18, y + 22, _legadoConfigured ? "已配置" : "未配置", TEXT_BLACK, BG_WHITE);
    drawUiText(x + 18, y + 76, _legadoConfigured ? _legadoHost : "请在SD卡根目录放置 /legado_config.json", TEXT_BLACK, BG_WHITE);

    if (_legadoConfigured) {
        syncLegadoProgress();
        drawUiText(x + 18, y + 128, "同步完成", TEXT_BLACK, BG_WHITE);
    } else {
        drawUiText(x + 18, y + 128, "用于和阅读/Legado同步阅读进度", TEXT_BLACK, BG_WHITE);
    }
    commitShellFrame();
}

void App::handleWiFiConfig() {
    auto& display = M5.Display;
    prepareShellFrame();
    drawBackHeader("WiFi配置", "在SD卡根目录编辑配置文件");

    int x = contentLeft();
    int y = 124;
    UITheme::drawCard(x, y, contentWidth(), 270, BG_WHITE, BORDER_LIGHT);
    drawUiText(x + 18, y + 20, _wifiConfigured ? "已配置" : "未配置", TEXT_BLACK, BG_WHITE);
    drawUiText(x + 18, y + 72, "请在SD卡根目录创建：", TEXT_BLACK, BG_WHITE);
    drawUiText(x + 18, y + 104, "/wifi_config.json", TEXT_BLACK, BG_WHITE);
    drawUiText(x + 18, y + 148, "{\"ssid\":\"你的WiFi\",", TEXT_BLACK, BG_WHITE);
    drawUiText(x + 18, y + 178, " \"pass\":\"密码\"}", TEXT_BLACK, BG_WHITE);
    drawUiText(x + 18, y + 222, "保存后重启或重新进入页面生效", TEXT_BLACK, BG_WHITE);

    if (_wifiConfigured) {
        UITheme::drawCard(x, y + 300, contentWidth(), 90, BG_WHITE, BORDER_LIGHT);
        char wifiLine[96];
        snprintf(wifiLine, sizeof(wifiLine), "当前WiFi：%s", _wifiSsid);
        drawUiText(x + 18, y + 330, wifiLine, TEXT_BLACK, BG_WHITE);
    }
    commitShellFrame();
}

void App::syncLegadoProgress() {
    if (!_reader.isOpen() || !_legadoConfigured) return;
    // push current progress
    LegadoProgress prog;
    memset(&prog, 0, sizeof(prog));
    strlcpy(prog.bookUrl, _reader.getBookPath(), sizeof(prog.bookUrl));
    prog.durChapterIndex = _reader.getCurrentChapterIndex();
    prog.durChapterPos = _reader.getCurrentPage();
    _legado.pushProgress(prog);
}

void App::loadWiFiConfig() {
    File f = SD.open("/wifi_config.json");
    if (!f) return;
    DynamicJsonDocument doc(512);
    deserializeJson(doc, f);
    f.close();
    strlcpy(_wifiSsid, doc["ssid"] | "", sizeof(_wifiSsid));
    strlcpy(_wifiPass, doc["pass"] | "", sizeof(_wifiPass));
    _wifiConfigured = _wifiSsid[0] != '\0';
}

void App::saveWiFiConfig() {
    DynamicJsonDocument doc(512);
    doc["ssid"] = _wifiSsid;
    doc["pass"] = _wifiPass;
    File f = SD.open("/wifi_config.json", FILE_WRITE);
    if (f) {
        serializeJson(doc, f);
        f.close();
    }
}

void App::loadLegadoConfig() {
    File f = SD.open("/legado_config.json");
    if (!f) return;
    DynamicJsonDocument doc(512);
    deserializeJson(doc, f);
    f.close();
    strlcpy(_legadoHost, doc["host"] | "", sizeof(_legadoHost));
    _legadoPort = doc["port"] | 80;
    strlcpy(_legadoUser, doc["user"] | "", sizeof(_legadoUser));
    strlcpy(_legadoPass, doc["pass"] | "", sizeof(_legadoPass));
    _legadoConfigured = _legadoHost[0] != '\0';
    if (_legadoConfigured) {
        _legado.config(_legadoHost, _legadoPort, _legadoUser, _legadoPass);
    }
}

void App::saveLegadoConfig() {
    DynamicJsonDocument doc(512);
    doc["host"] = _legadoHost;
    doc["port"] = _legadoPort;
    doc["user"] = _legadoUser;
    doc["pass"] = _legadoPass;
    File f = SD.open("/legado_config.json", FILE_WRITE);
    if (f) {
        serializeJson(doc, f);
        f.close();
    }
}

// ===== PaperS3 竖屏 UI =====

static uint32_t decodeUiUTF8(const uint8_t* buf, size_t& pos, size_t len) {
    if (pos >= len) return 0;
    uint8_t c = buf[pos];
    if ((c & 0x80) == 0) { pos++; return c; }
    if ((c & 0xE0) == 0xC0 && pos + 1 < len) {
        uint32_t ch = ((c & 0x1F) << 6) | (buf[pos + 1] & 0x3F);
        pos += 2; return ch;
    }
    if ((c & 0xF0) == 0xE0 && pos + 2 < len) {
        uint32_t ch = ((c & 0x0F) << 12) | ((buf[pos + 1] & 0x3F) << 6) | (buf[pos + 2] & 0x3F);
        pos += 3; return ch;
    }
    if ((c & 0xF8) == 0xF0 && pos + 3 < len) {
        uint32_t ch = ((c & 0x07) << 18) | ((buf[pos + 1] & 0x3F) << 12) | ((buf[pos + 2] & 0x3F) << 6) | (buf[pos + 3] & 0x3F);
        pos += 4; return ch;
    }
    pos++; return c;
}

static int16_t uiTextWidth(const char* text) {
    if (!text || !gUiFont || !gUiFont->isLoaded()) return UITheme::textWidth(text ? text : "", 1);
    int16_t w = 0;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text);
    size_t pos = 0, len = strlen(text);
    while (pos < len) {
        uint32_t ch = decodeUiUTF8(bytes, pos, len);
        uint8_t adv = gUiFont->getCharAdvance(ch);
        w += adv > 0 ? adv : (ch < 128 ? 8 : 24);
    }
    return w;
}

static void drawUiGlyph(uint32_t unicode, int16_t x, int16_t y, uint16_t color) {
    if (!gUiFont || !gUiFont->isLoaded()) return;
    auto& display = M5.Display;
    if (gUiFont->getFontType() == FontType::GRAY_4BPP) {
        uint8_t width, height, advance;
        int8_t bearingX, bearingY;
        const uint8_t* bmp = gUiFont->getCharBitmapGray(unicode, width, height, bearingX, bearingY, advance);
        if (!bmp || width == 0 || height == 0) return;
        int drawX = x + bearingX;
        int drawY = y + (gUiFont->getFontSize() - bearingY);
        for (int row = 0; row < height && drawY + row < SCREEN_HEIGHT; row++) {
            for (int col = 0; col < width && drawX + col < SCREEN_WIDTH; col++) {
                if (drawX + col < 0 || drawY + row < 0) continue;
                int srcIdx = row * ((width + 1) / 2) + col / 2;
                uint8_t nibble = (col % 2 == 0) ? ((bmp[srcIdx] >> 4) & 0x0F) : (bmp[srcIdx] & 0x0F);
                if (nibble > 0) display.drawPixel(drawX + col, drawY + row, color);
            }
        }
    } else {
        uint8_t width, height;
        const uint8_t* bmp = gUiFont->getCharBitmap(unicode, width, height);
        if (!bmp || width == 0 || height == 0) return;
        for (int row = 0; row < height && y + row < SCREEN_HEIGHT; row++) {
            for (int col = 0; col < width && x + col < SCREEN_WIDTH; col++) {
                int byteIdx = row * ((width + 7) / 8) + col / 8;
                int bitIdx = 7 - (col % 8);
                if (bmp[byteIdx] & (1 << bitIdx)) display.drawPixel(x + col, y + row, color);
            }
        }
    }
}

static void drawUiText(int16_t x, int16_t y, const char* text, uint16_t color, uint16_t bg) {
    auto& display = M5.Display;
    if (!text) return;
    if (!gUiFont || !gUiFont->isLoaded()) {
        display.setTextColor(color, bg);
        display.setTextSize(1);
        display.setCursor(x, y);
        display.print(text);
        return;
    }
    int16_t cx = x;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text);
    size_t pos = 0, len = strlen(text);
    while (pos < len && cx < SCREEN_WIDTH) {
        uint32_t ch = decodeUiUTF8(bytes, pos, len);
        if (ch == '\n') { y += gUiFont->getFontSize() + 6; cx = x; continue; }
        drawUiGlyph(ch, cx, y, color);
        uint8_t adv = gUiFont->getCharAdvance(ch);
        cx += adv > 0 ? adv : (ch < 128 ? 8 : 24);
    }
}

static void drawUiTextCentered(int16_t x, int16_t y, int16_t w, int16_t h, const char* text, uint16_t color) {
    int16_t tw = uiTextWidth(text);
    int16_t th = gUiFont && gUiFont->isLoaded() ? gUiFont->getFontSize() : 16;
    drawUiText(x + (w - tw) / 2, y + (h - th) / 2, text, color, BG_LIGHT);
}

static void drawUiSectionTitle(int16_t x, int16_t y, const char* title) {
    drawUiText(x, y, title, TEXT_BLACK, BG_LIGHT);
    int16_t tw = uiTextWidth(title);
    M5.Display.drawLine(x, y + 30, x + tw, y + 30, ACCENT);
    M5.Display.drawLine(x, y + 31, x + tw, y + 31, ACCENT);
}

static void drawUiCapsuleSwitch(int16_t x, int16_t y, int16_t w, bool on) {
    auto& display = M5.Display;
    int16_t h = 28;
    int16_t r = h / 2;
    UITheme::fillRoundRect(x, y, w, h, r, on ? ACCENT : BG_MID);
    int16_t knobX = on ? x + w - h + 2 : x + 2;
    UITheme::fillRoundRect(knobX, y + 2, h - 4, h - 4, r - 2, BG_WHITE);
    drawUiText(on ? x + 8 : x + w - 24, y + 4, on ? "开" : "关", on ? BG_WHITE : TEXT_LIGHT, on ? ACCENT : BG_MID);
}

static void drawUiSlider(int16_t x, int16_t y, int16_t w, int16_t minVal, int16_t maxVal, int16_t current, const char* unit) {
    auto& display = M5.Display;
    int16_t trackH = 6;
    int16_t knobR = 10;
    display.drawRoundRect(x, y + (knobR - trackH / 2) - 1, w, trackH + 2, 4, BORDER_LIGHT);
    int16_t filledW = maxVal == minVal ? 0 : (w * (current - minVal)) / (maxVal - minVal);
    if (filledW > 0) display.fillRoundRect(x, y + (knobR - trackH / 2), filledW, trackH, trackH / 2, ACCENT);
    display.fillCircle(x + filledW, y + knobR, knobR, BG_WHITE);
    display.drawCircle(x + filledW, y + knobR, knobR, ACCENT);
    char buf[32];
    snprintf(buf, sizeof(buf), "%d%s", current, unit ? unit : "");
    drawUiText(x + w + 10, y + 2, buf, TEXT_DARK, BG_LIGHT);
}

static void drawSmallProgress(int16_t x, int16_t y, int16_t w, int percent) {
    auto& display = M5.Display;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    display.drawRect(x, y, w, 6, TEXT_BLACK);
    int fillW = max(1, (w - 2) * percent / 100);
    if (percent > 0) display.fillRect(x + 1, y + 1, fillW, 4, TEXT_BLACK);
}

static void drawRowIcon(int16_t x, int16_t y, const char* kind) {
    auto& display = M5.Display;
    if (!kind) kind = "book";
    if (strcmp(kind, "book") == 0) {
        display.drawRoundRect(x, y, 26, 22, 3, TEXT_BLACK);
        display.drawLine(x + 13, y + 2, x + 13, y + 20, TEXT_BLACK);
        display.drawLine(x + 5, y + 5, x + 10, y + 3, TEXT_BLACK);
        display.drawLine(x + 16, y + 3, x + 21, y + 5, TEXT_BLACK);
    } else if (strcmp(kind, "wifi") == 0) {
        display.drawCircle(x + 13, y + 16, 2, TEXT_BLACK);
        display.drawCircle(x + 13, y + 16, 8, TEXT_BLACK);
        display.drawCircle(x + 13, y + 16, 14, TEXT_BLACK);
        display.fillRect(x - 3, y + 15, 32, 16, BG_LIGHT);
    } else if (strcmp(kind, "stats") == 0) {
        display.drawRect(x + 3, y + 10, 5, 10, TEXT_BLACK);
        display.drawRect(x + 11, y + 5, 5, 15, TEXT_BLACK);
        display.drawRect(x + 19, y + 1, 5, 19, TEXT_BLACK);
    } else {
        display.drawRoundRect(x + 2, y + 2, 22, 18, 4, TEXT_BLACK);
        display.drawLine(x + 7, y + 7, x + 19, y + 7, TEXT_BLACK);
        display.drawLine(x + 7, y + 12, x + 17, y + 12, TEXT_BLACK);
    }
}

static void drawBookCoverCard(int16_t x, int16_t y, int16_t w, int16_t h, const char* title, const char* type, int progress, bool dark) {
    auto& display = M5.Display;
    UITheme::drawCard(x, y, w, h, dark ? BG_DARK : BG_WHITE, BORDER_LIGHT);
    uint16_t fg = dark ? BG_WHITE : TEXT_BLACK;
    uint16_t bg = dark ? BG_DARK : BG_WHITE;
    // A simple monochrome cover texture: border + diagonal/photo blocks.
    display.drawRect(x + 8, y + 8, w - 16, max(36, h - 62), fg);
    if (!dark) {
        display.drawLine(x + 10, y + 12, x + w - 12, y + max(34, h - 66), fg);
        display.drawLine(x + w - 28, y + 14, x + w - 12, y + 38, fg);
        display.fillCircle(x + w / 2, y + max(24, h / 3), 3, fg);
    } else {
        display.drawRect(x + 14, y + 18, w - 28, max(24, h - 82), fg);
    }
    if (type && strlen(type) > 0) {
        drawUiTextCentered(x, y + h - 52, w, 22, type, fg);
    }
    char nameBuf[36];
    strlcpy(nameBuf, title ? title : "书籍", sizeof(nameBuf));
    if (strlen(nameBuf) > 18) {
        nameBuf[16] = '.';
        nameBuf[17] = '.';
        nameBuf[18] = '\0';
    }
    drawUiTextCentered(x + 4, y + h - 30, w - 8, 20, nameBuf, fg);
    if (progress >= 0) drawSmallProgress(x + 8, y + h - 8, w - 16, progress);
}

static void drawCrosslinkStatusBar() {
    auto& display = M5.Display;
    drawUiText(22, 9, "Vink e-reader", TEXT_MID, BG_LIGHT);
#if BATTERY_ICON_ENABLED
    BatteryInfo bat = BatteryInfo::read();
    char batText[12];
    if (bat.valid) {
        snprintf(batText, sizeof(batText), "%d%%", bat.level);
        drawUiText(SCREEN_WIDTH - 78, 9, batText, TEXT_MID, BG_LIGHT);
    }
    display.drawRect(SCREEN_WIDTH - 38, 11, 24, 12, TEXT_BLACK);
    display.drawRect(SCREEN_WIDTH - 14, 14, 3, 6, TEXT_BLACK);
    if (bat.valid) {
        int fillW = (bat.level * 20) / 100;
        if (fillW > 0) display.fillRect(SCREEN_WIDTH - 36, 13, fillW, 8, TEXT_BLACK);
    }
#endif
    display.drawLine(20, 32, SCREEN_WIDTH - 20, 32, BORDER_LIGHT);
}

static void prepareShellFrame() {
    auto& display = M5.Display;
    display.waitDisplay();
    display.powerSaveOff();
    display.setEpdMode(epd_mode_t::epd_fastest);
    display.fillScreen(BG_LIGHT);
}

static void commitShellFrame() {
    auto& display = M5.Display;
    // Dashboard reference pattern: shell draws into M5GFX framebuffer freely,
    // then performs one physical e-paper commit. Most commits use fastest mode;
    // every N commits use quality mode to reduce accumulated ghosting.
    if (gShellCommitCount > 0 && (gShellCommitCount % SHELL_FULL_REFRESH_EVERY) == 0) {
        display.setEpdMode(epd_mode_t::epd_quality);
    } else {
        display.setEpdMode(epd_mode_t::epd_fastest);
    }
    display.display(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    display.waitDisplay();
    gShellCommitCount++;
}

static void drawBackHeader(const char* title, const char* hint) {
    auto& display = M5.Display;
    drawUiText(20, 22, "<", TEXT_BLACK, BG_LIGHT);
    drawUiText(64, 22, title, TEXT_BLACK, BG_LIGHT);
    drawUiText(20, 58, hint, TEXT_MID, BG_LIGHT);
    display.drawLine(20, 82, SCREEN_WIDTH - 20, 82, BORDER);
    display.drawLine(20, 83, SCREEN_WIDTH - 20, 83, BORDER);
}

void App::navigateBack() {
    switch (_state) {
        case AppState::SETTINGS_LAYOUT:
        case AppState::SETTINGS_REFRESH:
        case AppState::SETTINGS_FONT:
        case AppState::SETTINGS_WIFI:
        case AppState::SETTINGS_LEGADO:
            _activeTab = 3;
            _state = AppState::TAB_SETTINGS;
            _tabNeedsRender[3] = true;
            break;
        case AppState::WIFI_UPLOAD:
        case AppState::READING_STATS:
            if (_reader.isOpen()) {
                _state = AppState::READER;
                _reader.renderPage();
            } else {
                _activeTab = 2;
                _state = AppState::TAB_TRANSFER;
                _tabNeedsRender[2] = true;
            }
            break;
        case AppState::CHAPTER_LIST:
        case AppState::BOOKMARK_LIST:
        case AppState::GOTO_PAGE:
        case AppState::READER_MENU:
            _state = AppState::READER;
            if (_reader.isOpen()) _reader.renderPage();
            break;
        default:
            if (!isTabState(_state)) {
                _state = AppState::TAB_READING;
                _activeTab = 0;
                _tabNeedsRender[0] = true;
            }
            break;
    }
}

void App::renderTopTabs() {
    auto& display = M5.Display;
    drawCrosslinkStatusBar();

    const char* tabs[] = {"阅读", "书架", "传输", "设置"};
    const char* icons[] = {"book", "book", "wifi", "settings"};
    int16_t tabW = (SCREEN_WIDTH - 40) / 4;
    int16_t baseX = 20;
    int16_t tabY = 34;
    int16_t tabH = 48;

    for (int i = 0; i < 4; i++) {
        bool active = (i == _activeTab);
        int16_t x = baseX + i * tabW;
        if (active) {
            display.fillRoundRect(x, tabY, tabW + 6, tabH - 6, 8, ACCENT);
            display.fillTriangle(x + tabW / 2 - 8, tabY + tabH - 6, x + tabW / 2 + 8, tabY + tabH - 6, x + tabW / 2, tabY + tabH + 2, ACCENT);
        } else {
            display.drawRoundRect(x, tabY, tabW + 6, tabH - 6, 8, BORDER_LIGHT);
        }
        if (i == 0 || i == 1) {
            uint16_t c = active ? BG_WHITE : TEXT_MID;
            int ix = x + 16;
            int iy = tabY + 11;
            display.drawRoundRect(ix, iy, 22, 18, 3, c);
            display.drawLine(ix + 11, iy + 2, ix + 11, iy + 17, c);
        } else if (i == 2) {
            uint16_t c = active ? BG_WHITE : TEXT_MID;
            int ix = x + 16;
            int iy = tabY + 11;
            display.drawCircle(ix + 12, iy + 16, 2, c);
            display.drawCircle(ix + 12, iy + 16, 8, c);
            display.drawCircle(ix + 12, iy + 16, 14, c);
            display.fillRect(ix - 3, iy + 14, 30, 16, active ? ACCENT : BG_LIGHT);
        } else {
            uint16_t c = active ? BG_WHITE : TEXT_MID;
            display.drawCircle(x + 25, tabY + 20, 9, c);
            display.drawLine(x + 25, tabY + 8, x + 25, tabY + 13, c);
            display.drawLine(x + 25, tabY + 27, x + 25, tabY + 32, c);
            display.drawLine(x + 13, tabY + 20, x + 18, tabY + 20, c);
            display.drawLine(x + 32, tabY + 20, x + 37, tabY + 20, c);
        }
        drawUiTextCentered(x + 38, tabY + 8, tabW - 40, 28, tabs[i], active ? BG_WHITE : TEXT_MID);
    }
    display.drawLine(20, TOP_TAB_H - 1, SCREEN_WIDTH - 20, TOP_TAB_H - 1, BORDER_LIGHT);
}

void App::renderBottomNav() {
    // M5PaperS3 无物理按键，底部导航栏留空
    // 如需显示电量/时间，可在此添加
}

void App::switchTab(int tab) {
    if (tab < 0) tab = 0;
    if (tab > 3) tab = 3;
    _activeTab = tab;
    switch (tab) {
        case 0: _state = AppState::TAB_READING; break;
        case 1: _state = AppState::TAB_LIBRARY; break;
        case 2: _state = AppState::TAB_TRANSFER; break;
        case 3: _state = AppState::TAB_SETTINGS; break;
    }
    for (int i = 0; i < 4; i++) _tabNeedsRender[i] = true;
}

bool App::isTabState(AppState state) {
    return state == AppState::TAB_READING || 
           state == AppState::TAB_LIBRARY || 
           state == AppState::TAB_TRANSFER || 
           state == AppState::TAB_SETTINGS;
}

// ===== 📖 读书主页（Crosslink dashboard）=====
void App::handleTabReading() {
    static bool needsRender = true;
    if (!needsRender && !_tabNeedsRender[0]) return;

    auto& display = M5.Display;
    prepareShellFrame();
    renderTopTabs();

    const int16_t x = contentLeft() + 6;
    const int16_t w = contentWidth() - 12;
    int16_t y = contentTop() + 2;
    int recentCount = _recent.getCount();

    drawUiText(x, y, "正在阅读", TEXT_BLACK, BG_LIGHT);
    y += 40;

    if (recentCount > 0) {
        const RecentBook* book = _recent.getBook(0);
        int pct = _recent.getProgressPercent(0);
        UITheme::drawCard(x, y, w, 178, BG_WHITE, BORDER_LIGHT);
        drawBookCoverCard(x + 16, y + 16, 112, 140, book ? book->name : "当前书籍", "TXT", pct, false);
        drawUiText(x + 148, y + 24, book ? book->name : "当前书籍", TEXT_BLACK, BG_WHITE);
        drawUiText(x + 148, y + 58, "本地书籍", TEXT_MID, BG_WHITE);
        char line[64];
        snprintf(line, sizeof(line), "当前进度 %d%%", pct);
        drawUiText(x + 148, y + 94, line, TEXT_BLACK, BG_WHITE);
        if (book && book->totalPages > 0) {
            snprintf(line, sizeof(line), "#%d/%d 页", book->currentPage + 1, book->totalPages);
            drawUiText(x + 330, y + 94, line, TEXT_BLACK, BG_WHITE);
        }
        drawSmallProgress(x + 148, y + 132, w - 180, pct);
    } else {
        UITheme::drawCard(x, y, w, 178, BG_WHITE, BORDER_LIGHT);
        drawRowIcon(x + 26, y + 30, "book");
        drawUiText(x + 70, y + 30, "还没有正在阅读的书", TEXT_BLACK, BG_WHITE);
        drawUiText(x + 70, y + 72, "去书架选择一本书开始阅读", TEXT_MID, BG_WHITE);
        UITheme::drawCard(x + 70, y + 116, 180, 42, BG_WHITE, BORDER);
        drawUiTextCentered(x + 70, y + 116, 180, 42, "打开书架", TEXT_BLACK);
    }
    y += 208;

    if (recentCount > 1) {
        const int coverW = (w - 24) / 3;
        const int coverH = 174;
        for (int i = 0; i < 3; i++) {
            const RecentBook* book = _recent.getBook(i + 1);
            int bx = x + i * (coverW + 12);
            if (book) {
                drawBookCoverCard(bx, y, coverW, coverH, book->name, "TXT", _recent.getProgressPercent(i + 1), i == 2);
            } else {
                UITheme::drawCard(bx, y, coverW, coverH, BG_WHITE, BORDER_LIGHT);
            }
        }
        y += coverH + 36;
    }

    drawUiText(x, y, "阅读统计", TEXT_BLACK, BG_LIGHT);
    y += 42;
    const int statGap = 12;
    const int statW = (w - statGap * 2) / 3;
    struct StatCard { const char* icon; const char* label; String value; const char* unit; };
    StatCard stats[] = {
        {"clock", "上次阅读", String(_stats.formatTime(_stats.getTodaySeconds())), ""},
        {"stats", "累计阅读", String(_stats.formatTime(_stats.getTotalReadingSeconds())), ""},
        {"book", "累计翻页", String(_stats.getTotalPagesRead()), "页"},
    };
    for (int i = 0; i < 3; i++) {
        int sx = x + i * (statW + statGap);
        UITheme::drawCard(sx, y, statW, 124, BG_WHITE, BORDER_LIGHT);
        drawRowIcon(sx + 14, y + 16, stats[i].icon);
        drawUiText(sx + 46, y + 18, stats[i].label, TEXT_MID, BG_WHITE);
        drawUiText(sx + 16, y + 68, stats[i].value.c_str(), TEXT_BLACK, BG_WHITE);
        if (stats[i].unit && strlen(stats[i].unit) > 0) drawUiText(sx + statW - 42, y + 72, stats[i].unit, TEXT_BLACK, BG_WHITE);
    }

    commitShellFrame();
    needsRender = false;
    _tabNeedsRender[0] = false;
}

// ===== 📚 书架（Crosslink 3列封面网格）=====
void App::handleTabLibrary() {
    static bool needsRender = true;
    if (!needsRender && !_tabNeedsRender[1]) return;

    auto& display = M5.Display;
    prepareShellFrame();
    renderTopTabs();

    const int totalBooks = _browser.getItemCount();
    const int booksPerPage = 9;
    _libraryTotalPages = (totalBooks + booksPerPage - 1) / booksPerPage;
    if (_libraryTotalPages < 1) _libraryTotalPages = 1;
    if (_libraryPage >= _libraryTotalPages) _libraryPage = _libraryTotalPages - 1;

    int16_t x = contentLeft() + 6;
    int16_t y = contentTop() + 2;
    int16_t w = contentWidth() - 12;
    drawUiText(x, y, "书架", TEXT_BLACK, BG_LIGHT);
    drawUiText(SCREEN_WIDTH - 128, y + 4, "按名打开书库", TEXT_MID, BG_LIGHT);
    y += 48;

    if (totalBooks == 0) {
        UITheme::drawCard(x, y + 24, w, 330, BG_WHITE, BORDER_LIGHT);
        drawRowIcon(x + w / 2 - 13, y + 76, "book");
        drawUiTextCentered(x, y + 122, w, 42, "书架为空", TEXT_BLACK);
        drawUiTextCentered(x, y + 170, w, 36, "请将 TXT / EPUB 放入 SD 卡 /books", TEXT_MID);
        UITheme::drawCard(x + (w - 210) / 2, y + 236, 210, 54, BG_WHITE, BORDER);
        drawUiTextCentered(x + (w - 210) / 2, y + 236, 210, 54, "打开SD卡", TEXT_BLACK);
        commitShellFrame();
        needsRender = false;
        _tabNeedsRender[1] = false;
        return;
    }

    const int cols = 3;
    const int gapX = 12;
    const int gapY = 18;
    const int cardW = (w - gapX * 2) / 3;
    const int cardH = 168;
    int startIdx = _libraryPage * booksPerPage;

    for (int i = 0; i < booksPerPage; i++) {
        int idx = startIdx + i;
        if (idx >= totalBooks) break;
        const FileItem* item = _browser.getItem(idx);
        if (!item) continue;
        int col = i % cols;
        int row = i / cols;
        int bx = x + col * (cardW + gapX);
        int by = y + row * (cardH + gapY);
        drawBookCoverCard(bx, by, cardW, cardH, item->name, item->isDirectory ? "DIR" : "TXT", item->isDirectory ? -1 : 0, idx == _librarySelected);
    }

    char pageText[32];
    snprintf(pageText, sizeof(pageText), "%d of %d", _libraryPage + 1, _libraryTotalPages);
    drawUiTextCentered(0, SCREEN_HEIGHT - 58, SCREEN_WIDTH, 30, pageText, TEXT_MID);

    commitShellFrame();
    needsRender = false;
    _tabNeedsRender[1] = false;
}

// ===== 🛜 传输中心（Crosslink status panel）=====
void App::handleTabTransfer() {
    static bool needsRender = true;
    if (!needsRender && !_tabNeedsRender[2]) return;

    prepareShellFrame();
    renderTopTabs();

    int16_t x = contentLeft() + 6;
    int16_t y = contentTop() + 2;
    int16_t w = contentWidth() - 12;
    drawUiText(x, y, "连接与传输", TEXT_BLACK, BG_LIGHT);

    int panelY = y + 52;
    UITheme::drawCard(x, panelY, w, 650, BG_WHITE, BORDER_LIGHT);
    drawRowIcon(x + 24, panelY + 24, "wifi");
    drawUiText(x + 64, panelY + 24, "网络状态", TEXT_BLACK, BG_WHITE);
    char ipLine[96];
    if (_uploader.isRunning()) snprintf(ipLine, sizeof(ipLine), "WiFi传书已开启  IP: %s", _uploader.getIP().c_str());
    else snprintf(ipLine, sizeof(ipLine), "WiFi传书未开启");
    drawUiText(x + 24, panelY + 62, ipLine, TEXT_MID, BG_WHITE);

    struct TransferItem { const char* title; const char* desc; bool enabled; const char* icon; };
    TransferItem items[] = {
        {"WiFi传书", _uploader.isRunning() ? "点击关闭浏览器上传服务" : "点击开启浏览器上传服务", _uploader.isRunning(), "wifi"},
        {"WiFi配置", _wifiConfigured ? _wifiSsid : "查看 /wifi_config.json 配置方法", _wifiConfigured, "settings"},
        {"Legado同步", _legadoConfigured ? _legadoHost : "查看 /legado_config.json 配置方法", _legadoConfigured, "book"},
        {"蓝牙翻页", _ble.isRunning() ? "已开启，点击关闭" : "未开启，点击启动", _ble.isRunning(), "settings"},
        {"阅读统计", "查看当前书籍和累计统计", true, "stats"},
        {"打开书架", "浏览 SD 卡 /books 中的书籍", true, "book"},
    };

    int rowY = panelY + 116;
    const int rowH = 74;
    for (int i = 0; i < 6; i++) {
        if (i > 0) M5.Display.drawLine(x + 22, rowY - 10, x + w - 22, rowY - 10, BORDER_LIGHT);
        drawRowIcon(x + 24, rowY + 18, items[i].icon);
        drawUiText(x + 66, rowY + 10, items[i].title, TEXT_BLACK, BG_WHITE);
        drawUiText(x + 66, rowY + 42, items[i].desc, TEXT_MID, BG_WHITE);
        if (i <= 3) drawUiCapsuleSwitch(x + w - 82, rowY + 22, 58, items[i].enabled);
        else drawUiText(x + w - 42, rowY + 24, ">", TEXT_BLACK, BG_WHITE);
        rowY += rowH;
    }

    int guideY = panelY + 584;
    M5.Display.drawLine(x + 22, guideY - 14, x + w - 22, guideY - 14, BORDER_LIGHT);
    drawUiText(x + 24, guideY, "触摸提示", TEXT_BLACK, BG_WHITE);
    drawUiText(x + 24, guideY + 34, "点对应行执行；左上 < 或右滑返回子页面", TEXT_MID, BG_WHITE);

    commitShellFrame();
    needsRender = false;
    _tabNeedsRender[2] = false;
}

// ===== ⚙️ 设置页（Crosslink compact settings panel）=====
void App::handleTabSettings() {
    static bool needsRender = true;
    if (!needsRender && !_tabNeedsRender[3]) return;

    prepareShellFrame();
    renderTopTabs();

    int16_t x = contentLeft() + 28;
    int16_t y = contentTop() + 26;
    int16_t w = contentWidth() - 56;
    UITheme::drawCard(x, y, w, 720, BG_WHITE, BORDER_LIGHT);

    drawUiText(x + 24, y + 24, "设置", TEXT_BLACK, BG_WHITE);

    RefreshStrategy rs = _reader.getRefreshStrategy();
    char sleepText[24];
    snprintf(sleepText, sizeof(sleepText), "%d 分钟", _sleepTimeoutMin);
    struct SettingItem { const char* group; const char* title; const char* value; AppState nextState; };
    SettingItem items[] = {
        {"字体", "预设字体", _fontCount > 0 ? _fontNames[0] : "内置字体", AppState::SETTINGS_FONT},
        {"字体", "字号", "小  中  大", AppState::SETTINGS_LAYOUT},
        {"版式", "对齐方式", "左  中  右  齐", AppState::SETTINGS_LAYOUT},
        {"版式", "刷新策略", rs.getLabel(), AppState::SETTINGS_REFRESH},
        {"连接", "WiFi配置", _wifiConfigured ? _wifiSsid : "未配置", AppState::SETTINGS_WIFI},
        {"连接", "Legado同步", _legadoConfigured ? _legadoHost : "未配置", AppState::SETTINGS_LEGADO},
        {"系统", "休眠时间", sleepText, AppState::TAB_SETTINGS},
        {"系统", "关于", "Vink-PaperS3 v0.2.9", AppState::TAB_SETTINGS},
    };

    const int numItems = 8;
    const int rowH = 64;
    int rowY = y + 74 - _settingsScroll;
    const char* lastGroup = "";
    for (int i = 0; i < numItems; i++) {
        if (strcmp(lastGroup, items[i].group) != 0) {
            drawUiText(x + 24, rowY + 4, items[i].group, TEXT_BLACK, BG_WHITE);
            rowY += 34;
            lastGroup = items[i].group;
        }
        if (rowY + rowH >= y && rowY <= y + 720) {
            if (i == 0 || i == 3 || i == 4 || i == 6) {
                M5.Display.fillRoundRect(x + 18, rowY + 4, w - 36, rowH - 8, 8, BG_WHITE);
                M5.Display.drawRoundRect(x + 18, rowY + 4, w - 36, rowH - 8, 8, BORDER_LIGHT);
            }
            drawUiText(x + 30, rowY + 18, items[i].title, TEXT_BLACK, BG_WHITE);
            drawUiText(x + w - 170, rowY + 18, items[i].value, TEXT_MID, BG_WHITE);
            if (i != 1 && i != 2) drawUiText(x + w - 38, rowY + 18, ">", TEXT_BLACK, BG_WHITE);
        }
        rowY += rowH;
    }

    commitShellFrame();
    needsRender = false;
    _tabNeedsRender[3] = false;
}

// ===== 阅读弹出菜单（半透明遮罩）=====
void App::handleReaderMenu() {
    auto& display = M5.Display;
    
    // 如果菜单已经显示过，不需要重绘（底层阅读页还在）
    // 这里只做一次性绘制
    
    // 半透明遮罩（墨水屏模拟：用浅灰填充区域）
    display.fillRect(100, 60, SCREEN_WIDTH - 200, SCREEN_HEIGHT - 120, BG_LIGHT);
    UITheme::drawCard(100, 60, SCREEN_WIDTH - 200, SCREEN_HEIGHT - 120, BG_WHITE, BORDER);
    
    drawUiTextCentered(100, 75, SCREEN_WIDTH - 200, 32, "菜单", TEXT_BLACK);
    
    const char* items[] = {
        "章节目录",
        "添加书签",
        "我的书签",
        "页码跳转",
        "排版设置",
        "字体切换",
        "残影控制",
        "WiFi传书",
        "阅读统计",
        "蓝牙翻页",
        "返回书架",
        "关闭设备"
    };
    int numItems = 12;
    
    display.setTextSize(1);
    for (int i = 0; i < numItems; i++) {
        int y = 110 + i * 32;
        if (i == _menuIndex) {
            display.fillRect(120, y - 2, SCREEN_WIDTH - 240, 28, BG_MID);
            display.setTextColor(TEXT_BLACK, BG_MID);
        } else {
            display.setTextColor(TEXT_DARK, BG_WHITE);
        }
        drawUiText(140, y + 4, items[i], i == _menuIndex ? TEXT_BLACK : TEXT_DARK, i == _menuIndex ? BG_MID : BG_WHITE);
    }
    display.setTextColor(TEXT_BLACK, BG_LIGHT);
    
    display.display();
}

// ===== 排版设置（卡片式 + 滑块）=====
void App::handleSettingsLayout() {
    auto& display = M5.Display;
    prepareShellFrame();
    drawBackHeader("排版设置", "点 < / 右滑 / 上滑返回");

    LayoutConfig layout = _reader.getLayoutConfig();
    struct Row { const char* name; int minVal; int maxVal; int value; const char* unit; };
    Row rows[] = {
        {"字号", 12, 48, layout.fontSize, "px"},
        {"行距", 50, 200, layout.lineSpacing, "%"},
        {"左右边距", 0, 120, layout.marginLeft, "px"},
        {"顶部边距", 0, 100, layout.marginTop, "px"},
        {"首行缩进", 0, 4, layout.indentFirstLine, "字"},
    };

    int y = 108;
    for (int i = 0; i < 5; i++) {
        UITheme::drawCard(contentLeft(), y, contentWidth(), 112, BG_WHITE, i == _layoutEditorIndex ? ACCENT : BORDER_LIGHT);
        char rowTitle[40];
        snprintf(rowTitle, sizeof(rowTitle), "%s%s", i == _layoutEditorIndex ? "* " : "", rows[i].name);
        drawUiText(contentLeft() + 18, y + 14, rowTitle, TEXT_BLACK, BG_WHITE);
        drawUiSlider(contentLeft() + 18, y + 64, contentWidth() - 130, rows[i].minVal, rows[i].maxVal, rows[i].value, rows[i].unit);
        y += 126;
    }
    commitShellFrame();
}

// ===== 残影控制（胶囊开关式）=====
void App::handleSettingsRefresh() {
    auto& display = M5.Display;
    prepareShellFrame();
    drawBackHeader("刷新策略", "点击档位切换，点 < 返回");

    RefreshStrategy strategy = _reader.getRefreshStrategy();
    const char* labels[] = {"极速", "均衡", "清晰"};
    const char* descs[] = {
        "局部快刷为主，每20页全刷一次",
        "局部快刷为主，每10页全刷一次",
        "文字更清晰，每5页全刷一次"
    };
    int selected = (int)strategy.frequency;

    int y = 116;
    for (int i = 0; i < 3; i++) {
        UITheme::drawCard(contentLeft(), y, contentWidth(), 128, BG_WHITE, i == selected ? ACCENT : BORDER_LIGHT);
        drawUiText(contentLeft() + 18, y + 18, labels[i], TEXT_BLACK, BG_WHITE);
        drawUiText(contentLeft() + 18, y + 64, descs[i], TEXT_MID, BG_WHITE);
        drawUiCapsuleSwitch(contentRight() - 78, y + 44, 58, i == selected);
        y += 148;
    }
    commitShellFrame();
}

// ===== 字体切换（列表式）=====
void App::handleSettingsFont() {
    auto& display = M5.Display;
    prepareShellFrame();
    drawBackHeader("字体切换", "点击字体切换，点 < 返回");

    if (_fontCount <= 0) {
        drawUiTextCentered(0, 260, SCREEN_WIDTH, 80, "未找到字体", TEXT_MID);
        drawUiTextCentered(0, 330, SCREEN_WIDTH, 50, "请将 .fnt 放入 /fonts", TEXT_MID);
        commitShellFrame();
        return;
    }

    int y = 112;
    for (int i = 0; i < _fontCount && i < 9; i++) {
        bool isCurrent = (strcmp(_font.getCurrentFontPath(), _fontPaths[i]) == 0);
        UITheme::drawCard(contentLeft(), y, contentWidth(), 78, BG_WHITE, isCurrent ? ACCENT : BORDER_LIGHT);
        char fontTitle[96];
        snprintf(fontTitle, sizeof(fontTitle), "%s%s", isCurrent ? "* " : "", _fontNames[i]);
        drawUiText(contentLeft() + 18, y + 22, fontTitle, TEXT_BLACK, BG_WHITE);
        y += 92;
    }
    commitShellFrame();
}

// ===== WiFi配置 =====
void App::handleSettingsWiFi() {
    handleWiFiConfig();
}

// ===== Legado同步 =====
void App::handleSettingsLegado() {
    handleLegadoSync();
}

// ===== 章节目录（改进版）=====
void App::handleChapterList() {
    auto& display = M5.Display;
    display.clear();
    display.fillScreen(BG_LIGHT);
    
    drawUiSectionTitle(20, 15, "章节目录");
    
    int chapterCount = _reader.getChapterCount();
    if (chapterCount <= 0) {
        drawUiTextCentered(0, 200, SCREEN_WIDTH, 100, "未检测到章节", TEXT_MID);
        drawUiTextCentered(0, 280, SCREEN_WIDTH, 50, "长按菜单可自动识别章节", TEXT_MID);
        display.display();
        return;
    }
    
    const ChapterInfo* chapters = _reader.getChapterList();
    int currentChapter = _reader.getCurrentChapterIndex();
    
    int itemsPerPage = 12;
    int totalPages = (chapterCount + itemsPerPage - 1) / itemsPerPage;
    int currentPage = _chapterMenuScroll / itemsPerPage;
    int startIdx = currentPage * itemsPerPage;
    int endIdx = min(startIdx + itemsPerPage, chapterCount);
    
    int itemH = 36;
    int cy = 55;
    for (int i = startIdx; i < endIdx; i++) {
        int y = cy + (i - startIdx) * (itemH + 4);
        UITheme::drawCard(10, y, SCREEN_WIDTH - 20, itemH, BG_WHITE, BORDER_LIGHT);
        
        if (i == _chapterMenuIndex) {
            display.fillRoundRect(10, y, SCREEN_WIDTH - 20, itemH, UITheme::CARD_RADIUS, BG_MID);
        }
        if (i == currentChapter) {
            display.setTextColor(ACCENT, i == _chapterMenuIndex ? BG_MID : BG_WHITE);
        } else {
            display.setTextColor(TEXT_DARK, i == _chapterMenuIndex ? BG_MID : BG_WHITE);
        }
        
        String title = chapters[i].title;
        if (title.length() > 35) title = title.substring(0, 32) + "...";
        char chapterLine[160];
        snprintf(chapterLine, sizeof(chapterLine), "%d. %s", i + 1, title.c_str());
        drawUiText(25, y + 10, chapterLine, i == currentChapter ? ACCENT : TEXT_DARK, i == _chapterMenuIndex ? BG_MID : BG_WHITE);
    }
    display.setTextColor(TEXT_BLACK, BG_LIGHT);
    
    char chapterPageText[32];
    snprintf(chapterPageText, sizeof(chapterPageText), "%d/%d 页", currentPage + 1, totalPages);
    drawUiText(400, SCREEN_HEIGHT - 35, chapterPageText, TEXT_MID, BG_LIGHT);
    
    renderBottomNav();
    display.display();
}

// ===== 书签列表（改进版）=====
void App::handleBookmarkList() {
    auto& display = M5.Display;
    display.clear();
    display.fillScreen(BG_LIGHT);
    
    drawUiSectionTitle(20, 15, "我的书签");
    
    int bmCount = _reader.getBookmarkCount();
    if (bmCount <= 0) {
        drawUiTextCentered(0, 200, SCREEN_WIDTH, 100, "暂无书签", TEXT_MID);
        drawUiTextCentered(0, 280, SCREEN_WIDTH, 50, "阅读时点击菜单添加书签", TEXT_MID);
        display.display();
        return;
    }
    
    const Bookmark* bookmarks = _reader.getBookmarks();
    int itemsPerPage = 12;
    int totalPages = (bmCount + itemsPerPage - 1) / itemsPerPage;
    int currentPage = _bookmarkMenuScroll / itemsPerPage;
    int startIdx = currentPage * itemsPerPage;
    int endIdx = min(startIdx + itemsPerPage, bmCount);
    
    int itemH = 36;
    int cy = 55;
    for (int i = startIdx; i < endIdx; i++) {
        int y = cy + (i - startIdx) * (itemH + 4);
        UITheme::drawCard(10, y, SCREEN_WIDTH - 20, itemH, BG_WHITE, BORDER_LIGHT);
        
        if (i == _bookmarkMenuIndex) {
            display.fillRoundRect(10, y, SCREEN_WIDTH - 20, itemH, UITheme::CARD_RADIUS, BG_MID);
        }
        
        String name = bookmarks[i].name;
        if (name.length() > 30) name = name.substring(0, 27) + "...";
        char bookmarkLine[160];
        snprintf(bookmarkLine, sizeof(bookmarkLine), "%d. %s (P%d)", i + 1, name.c_str(), bookmarks[i].pageNum + 1);
        drawUiText(25, y + 10, bookmarkLine, TEXT_DARK, i == _bookmarkMenuIndex ? BG_MID : BG_WHITE);
    }
    display.setTextColor(TEXT_BLACK, BG_LIGHT);
    
    char bookmarkPageText[48];
    snprintf(bookmarkPageText, sizeof(bookmarkPageText), "%d 个书签 | %d/%d", bmCount, currentPage + 1, totalPages);
    drawUiText(350, SCREEN_HEIGHT - 35, bookmarkPageText, TEXT_MID, BG_LIGHT);
    
    renderBottomNav();
    display.display();
}

