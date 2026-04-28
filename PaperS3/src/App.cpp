#include "App.h"
#include "UITheme.h"
#include <M5Unified.h>

using namespace UITheme;

static FontManager* gUiFont = nullptr;
static M5Canvas* gShellCanvas = nullptr;
static bool gShellFrameActive = false;
static bool gLastShellCommitOk = false;

static LovyanGFX& uiDrawTarget() {
    return (gShellFrameActive && gShellCanvas) ? static_cast<LovyanGFX&>(*gShellCanvas) : static_cast<LovyanGFX&>(M5.Display);
}

static constexpr int PAPER_S3_DISPLAY_ROTATION = 0;  // user-facing portrait, handle at top
static constexpr uint32_t SHELL_FULL_REFRESH_EVERY = 6;
static uint32_t gShellCommitCount = 0;

static bool ensureShellCanvas();
static void prepareShellFrame();
static bool waitShellDisplayReady(uint32_t timeoutMs);
static bool commitShellFrame();
static void drawBackHeader(const char* title, const char* hint = "点 < / 右滑 / 上滑返回");
static void drawUiText(int16_t x, int16_t y, const char* text, uint16_t color = TEXT_BLACK, uint16_t bg = BG_LIGHT);
static void drawUiTextCentered(int16_t x, int16_t y, int16_t w, int16_t h, const char* text, uint16_t color = TEXT_BLACK);
static void drawUiTextRight(int16_t rightX, int16_t y, const char* text, uint16_t color = TEXT_BLACK, uint16_t bg = BG_LIGHT);
static void drawUiSectionTitle(int16_t x, int16_t y, const char* title);
static void drawUiCapsuleSwitch(int16_t x, int16_t y, int16_t w, bool on);
static void drawUiSlider(int16_t x, int16_t y, int16_t w, int16_t minVal, int16_t maxVal, int16_t current, const char* unit);
static void drawCrosslinkStatusBar();
static void drawBookCoverCard(int16_t x, int16_t y, int16_t w, int16_t h, const char* title, const char* type, int progress, bool dark = false);
static void drawSmallProgress(int16_t x, int16_t y, int16_t w, int percent);
static void drawSlotIcon(int16_t x, int16_t y, const char* kind, uint16_t color = TEXT_BLACK, uint16_t bg = BG_WHITE);
static void drawRowIcon(int16_t x, int16_t y, const char* kind);
static void drawIconLabel(int16_t x, int16_t y, const char* icon, const char* label, uint16_t bg = BG_WHITE);

App::App() : _state(AppState::INIT), _reader(_font), _touching(false), _touchLongPressFired(false), _touchConsumed(false), _touchWaitRelease(false),
             _menuIndex(0), _layoutEditorIndex(0), _chapterMenuIndex(0), _chapterMenuScroll(0),
             _bookmarkMenuIndex(0), _bookmarkMenuScroll(0),
             _fontMenuIndex(0), _fontMenuScroll(0), _settingsScroll(0),
             _activeTab(0), _libraryPage(0), _libraryTotalPages(1), _librarySelected(0),
             _gotoPageCursor(0),
             _lastActivityTime(0), _sleepPending(false), _powerButtonArmed(false),
             _toastVisible(false), _toastDirty(false), _toastClearDirty(false),
             _toastDrawn(false), _toastDrawState(AppState::INIT), _toastUntil(0),
             _shutdownInProgress(false), _sdReady(false), _pageNeedsRender(true), _lastRenderedState(AppState::INIT), _powerButtonPressStart(0),
             _sleepTimeoutMin(AUTO_SLEEP_DEFAULT_MIN),
             _fontCount(0) {
    memset(_gotoPageInput, 0, sizeof(_gotoPageInput));
    memset(_wifiSsid, 0, sizeof(_wifiSsid));
    memset(_wifiPass, 0, sizeof(_wifiPass));
    memset(_legadoHost, 0, sizeof(_legadoHost));
    memset(_legadoUser, 0, sizeof(_legadoUser));
    memset(_legadoPass, 0, sizeof(_legadoPass));
    memset(_toastText, 0, sizeof(_toastText));
    _toastVisible = false;
    _toastDirty = false;
    _toastClearDirty = false;
    _toastDrawn = false;
    _toastDrawState = AppState::INIT;
    _toastUntil = 0;
    _wifiConfigured = false;
    _legadoConfigured = false;
    _sdReady = false;
    _legadoPort = 80;
    _touchStartX = _touchStartY = _touchLastX = _touchLastY = 0;
    _touchSuppressUntil = 0;
    _touchConsumed = false;
    _touchWaitRelease = false;
    _pageNeedsRender = true;
    _lastRenderedState = AppState::INIT;
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
    _sdReady = sdReady;
    if (!sdReady) {
        showMessage("SD卡异常，继续启动", 3000);
        Serial.println("[App] SD init failed, continuing for boot diagnostics");
    }
    
    if (!initFont()) {
        showMessage("字体异常，使用备用字体", 2000);
        Serial.println("[App] Font init failed, continuing with built-in fallback if available");
    }
    // 主 UI 固定走内置 UI 字体；阅读正文才允许后续切换 SD 字体。
    gUiFont = &_uiFont;
    
    if (sdReady) {
        if (!SD.exists(BOOKS_DIR)) SD.mkdir(BOOKS_DIR);
        if (!SD.exists(PROGRESS_DIR)) SD.mkdir(PROGRESS_DIR);
    }
    
    // Do not block or risk watchdog/reset before first UI frame. Font/book scans
    // can be triggered later by the relevant pages/actions. v0.2.13 showed repeated
    // boot diagnostic refreshes on real hardware, which points to an early boot loop.
    _fontCount = 0;

    _state = AppState::TAB_READING;
    _activeTab = 0;
    for (int i = 0; i < 4; i++) _tabNeedsRender[i] = true;
    
    if (_sdReady) {
        loadGlobalSettings();
        _stats.load();
        _recent.load();
        loadWiFiConfig();
        loadLegadoConfig();
    } else {
        _sleepTimeoutMin = AUTO_SLEEP_DEFAULT_MIN;
    }
    _lastActivityTime = millis();
    _sleepPending = false;
    _powerButtonArmed = false;
    
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

    // Allocate the Shell canvas before SD/font/cache work can fragment PSRAM.
    // Reference PaperS3 readers keep a global framebuffer/canvas alive for the
    // UI lifetime; lazy allocation on first menu paint makes the riskiest page
    // transition also depend on a large PSRAM allocation.
    if (!ensureShellCanvas()) {
        Serial.println("[Display] Shell canvas preallocation failed; will retry before first shell frame");
    }

    // Keep boot display non-blocking. v0.2.13 could remain on this diagnostic
    // screen on real PaperS3 because startup called pushSprite() + display() +
    // waitDisplay() before the app state machine was alive. The first real shell
    // frame will be committed by handleTabReading() after init completes.
    display.fillScreen(BG_LIGHT);
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
    // Shell/UI must be boring and robust: use the built-in PROGMEM 1bpp font,
    // not the SPIFFS/SD file-backed gray font. The real device showed that after
    // repeated taps the line/card geometry stayed intact while text turned into
    // dense garbage or disappeared, which points at the file-backed UI font path
    // or its PSRAM/index state being corrupted. Reading content still uses the
    // richer bundled/SD font path below.
    bool uiOk = _uiFont.loadBuiltinFont();
    if (uiOk) {
        Serial.printf("[Font] Fixed built-in safe UI font active: %s\n", _uiFont.getCurrentFontPath());
    }

    bool readerOk = _font.loadBundledFont(FONT_FILE_24) || _font.loadBundledFont(FONT_FILE_20) || _font.loadBundledFont(FONT_FILE_16);
    if (readerOk) {
        Serial.printf("[Font] Reader default font active: %s\n", _font.getCurrentFontPath());
    }

    if (!uiOk || !readerOk) {
        Serial.printf("[Font] Font init incomplete: ui=%d reader=%d\n", uiOk, readerOk);
    }
    return uiOk && readerOk;
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
        serviceToast();

        // Tab pages already have per-tab dirty flags. Non-tab Shell pages used to
        // render every 8ms, which kept the e-paper queue busy and made menus feel
        // frozen after entering them. Render those pages once per state/gesture,
        // and keep dirty if the physical commit was skipped.
        bool isShellSubpage = !isTabState(_state) && _state != AppState::INIT && _state != AppState::READER;
        bool shouldRenderSubpage = _pageNeedsRender || _state != _lastRenderedState;
        if (isShellSubpage && !shouldRenderSubpage) {
            delay(8);
            continue;
        }
        if (isShellSubpage) gLastShellCommitOk = false;
        AppState renderState = _state;
        
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
        if (isShellSubpage) {
            if (gLastShellCommitOk && _state == renderState) {
                _lastRenderedState = _state;
                _pageNeedsRender = false;
            } else {
                _pageNeedsRender = true;
            }
        }
        // Keep touch polling snappy. M5Unified examples update input every loop;
        // a 50ms sleep made tap-release detection feel sticky on PaperS3.
        delay(8);
    }
}

void App::handleInit() {
    _browser.scan(BOOKS_DIR);
    _state = AppState::TAB_READING;
    _activeTab = 0;
    for (int i = 0; i < 4; i++) _tabNeedsRender[i] = true;
}

void App::handleFileBrowser() {
    // Compatibility shim: the old file-browser page used direct cursor drawing
    // and did not follow the current 540x960 card grid/alignment rules.
    handleTabLibrary();
}

void App::handleReader() {
    // 阅读器由触摸事件驱动
}

// ===== 旧菜单入口兼容：全部转到新版卡片/居中 UI =====
void App::handleMenu() {
    handleReaderMenu();
}

void App::handleChapterMenu() {
    handleChapterList();
}

void App::handleRefreshMenu() {
    handleSettingsRefresh();
}

void App::handleBookmarkMenu() {
    handleBookmarkList();
}

void App::handleLayoutMenu() {
    handleSettingsLayout();
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
    constexpr uint32_t TRANSITION_TOUCH_SUPPRESS_MS = 180;
    constexpr int TAP_THRESHOLD = 26;
    constexpr int SWIPE_THRESHOLD = 64;
    constexpr int LONG_PRESS_MS = 750;
    constexpr int LONG_PRESS_MOVE = 28;
    constexpr int MAX_TAP_MS = 650;

    int touchCount = touch.getCount();
    if (_touchSuppressUntil || _touchWaitRelease) {
        // Reference PaperS3 projects suppress stale touches after state
        // transitions/wake. A pure time delay is not enough: if the user's finger
        // stays down longer than the cooldown, that same press can become a fresh
        // tap on the newly rendered page. Wait for both the cooldown and a full
        // release before accepting touch input again.
        bool cooldownDone = (!_touchSuppressUntil || millis() >= _touchSuppressUntil);
        if (!cooldownDone || touchCount > 0) {
            _touching = false;
            _touchLongPressFired = false;
            _touchConsumed = false;
            if (cooldownDone) _touchSuppressUntil = 0;
            _touchWaitRelease = true;
            return;
        }
        _touchSuppressUntil = 0;
        _touchWaitRelease = false;
    }

    if (touchCount > 0) {
        if (!_touching && M5.Display.displayBusy()) {
            // Ignore new touches while the EPD queue is still committing the
            // previous frame. Accepting tab/actions here can enqueue overlapping
            // full-screen commits and make glyphs disappear or the panel appear
            // stuck after rapid repeated taps.
            return;
        }
        _lastActivityTime = millis();
        _sleepPending = false;
        auto t = touch.getDetail(0);
        int x = 0, y = 0;
        normalizePaperS3TouchPoint(t.x, t.y, x, y);

        if (!_touching) {
            _touching = true;
            _touchLongPressFired = false;
            _touchConsumed = false;
            _touchStartX = _touchLastX = x;
            _touchStartY = _touchLastY = y;
            _touchStartTime = millis();

            // Start tab switching on touch-down rather than waiting for release.
            // PaperS3 screen refresh is slow enough that saving one release cycle
            // is noticeable, and the top tab zone is not used for vertical swipes.
            if (isTabState(_state) && y < TOP_TAB_H) {
                const int tabBaseX = 20;
                const int tabW = (SCREEN_WIDTH - 40) / 4;
                int clickedTab = (x - tabBaseX) / tabW;
                if (x >= tabBaseX && x <= SCREEN_WIDTH - 20 && clickedTab >= 0 && clickedTab < 4 && clickedTab != _activeTab) {
                    switchTab(clickedTab);
                    // Do not submit a separate top-tab partial refresh here.
                    // Rapid taps could make that partial commit overlap with the
                    // following full-page shell commit. The next main-loop pass
                    // renders the selected tab as one serialized full frame.
                    _touchConsumed = true;
                }
            }
            return;
        }

        _touchLastX = x;
        _touchLastY = y;
        int dx = _touchLastX - _touchStartX;
        int dy = _touchLastY - _touchStartY;
        unsigned long dt = millis() - _touchStartTime;

        if (!_touchLongPressFired && dt >= LONG_PRESS_MS && abs(dx) < LONG_PRESS_MOVE && abs(dy) < LONG_PRESS_MOVE) {
            _touchLongPressFired = true;
            _pageNeedsRender = true;
            onLongPress(_touchStartX, _touchStartY);
        }
        return;
    }

    if (_touching) {
        _touching = false;
        int dx = _touchLastX - _touchStartX;
        int dy = _touchLastY - _touchStartY;
        unsigned long dt = millis() - _touchStartTime;

        if (_touchLongPressFired || _touchConsumed) {
            _touchLongPressFired = false;
            _touchConsumed = false;
            return;
        }

        if (M5.Display.displayBusy()) {
            return;
        }

        int absDx = abs(dx);
        int absDy = abs(dy);
        if (absDx < TAP_THRESHOLD && absDy < TAP_THRESHOLD && dt <= MAX_TAP_MS) {
            _pageNeedsRender = true;
            onTap(_touchStartX, _touchStartY);
        } else if (absDx >= SWIPE_THRESHOLD || absDy >= SWIPE_THRESHOLD) {
            _pageNeedsRender = true;
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
            if (_font.loadFont(_fontPaths[idx])) showToast("字体已切换", 900);
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
                        showToast("已重新扫描SD卡", 900);
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
                int16_t baseX = contentLeft() + 6;
                int16_t baseY = contentTop() - 2;
                int16_t w = contentWidth() - 12;
                int panelY = baseY + 38;

                auto toggleWiFi = [&]() {
                    if (_uploader.isRunning()) {
                        _uploader.stop();
                        showToast("WiFi传书已关闭", 1200);
                    } else {
                        _uploader.start(_wifiSsid, _wifiPass);
                        showToast("WiFi传书已开启", 1200);
                    }
                    _tabNeedsRender[2] = true;
                };

                if (x >= baseX + w - 136 && x <= baseX + w - 18 &&
                    y >= panelY + 26 && y <= panelY + 86) {
                    toggleWiFi();
                    return;
                }

                const int gap = 12;
                const int cardW = (w - gap) / 2;
                const int cardH = 128;
                int gridY = panelY + 148;
                int clickedIdx = -1;
                for (int i = 0; i < 6; i++) {
                    int col = i % 2;
                    int row = i / 2;
                    int bx = baseX + col * (cardW + gap);
                    int by = gridY + row * (cardH + gap);
                    if (x >= bx && x <= bx + cardW && y >= by && y <= by + cardH) {
                        clickedIdx = i;
                        break;
                    }
                }

                switch (clickedIdx) {
                    case 0:
                        toggleWiFi();
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
                            showToast("蓝牙翻页已关闭", 1200);
                        } else {
                            _ble.start();
                            showToast("蓝牙翻页已开启", 1200);
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
                const int baseX = contentLeft() + 4;
                const int baseY = contentTop() - 2;
                const int w = contentWidth() - 8;
                const int cardH = 176;
                const int gap = 14;
                const int headerH = 36;
                const int rowH = 62;
                int clickedIdx = -1;

                for (int group = 0; group < 4; group++) {
                    int cardY = baseY + 38 + group * (cardH + gap);
                    int row1Y = cardY + headerH + 6;
                    int row2Y = row1Y + rowH + 10;
                    if (x >= baseX && x <= baseX + w) {
                        if (y >= row1Y && y <= row1Y + rowH) {
                            clickedIdx = group * 2;
                            break;
                        }
                        if (y >= row2Y && y <= row2Y + rowH) {
                            clickedIdx = group * 2 + 1;
                            break;
                        }
                    }
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
                        case 7: showToast("Vink-PaperS3 v0.2.15", 1800); break;
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
                        _activeTab = 1;
                        _state = AppState::TAB_LIBRARY;
                        _tabNeedsRender[1] = true;
                        handleTabLibrary();
                    } else {
                        showToast("进入目录失败", 1200);
                    }
                } else if (item && _reader.openBook(item->path)) {
                    _stats.startReading(item->name);
                    _recent.addBook(item->path, item->name, 0, _reader.getTotalPages());
                    _state = AppState::READER;
                    _reader.renderPage();
                } else {
                    showToast("打开失败", 1200);
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
                _state = AppState::READER_MENU;
                handleReaderMenu();
            }
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
                    showToast("字体已切换", 1000);
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
                            showToast("页码超出范围", 1200);
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
                _activeTab = 1;
                _state = AppState::TAB_LIBRARY;
                _tabNeedsRender[1] = true;
                handleTabLibrary();
            } else if (dy < -30) {
                _browser.selectPrev();
                _activeTab = 1;
                _state = AppState::TAB_LIBRARY;
                _tabNeedsRender[1] = true;
                handleTabLibrary();
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
                if (_reader.addBookmark()) showToast("书签已添加", 1200);
                else showToast("添加失败", 1200);
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
                    showToast("WiFi传书已关闭", 1200);
                } else {
                    _uploader.start(_wifiSsid, _wifiPass);
                    showToast("WiFi传书已开启", 1200);
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
                    showToast("蓝牙翻页已关闭", 1200);
                } else {
                    _ble.start();
                    showToast("蓝牙翻页已开启", 1200);
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
                showToast("书签已添加", 1200);
            } else {
                showToast("添加失败", 1200);
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
                showToast("WiFi传书已关闭", 1200);
            } else {
                showToast("请在配置中设置WiFi", 1400);
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
                showToast("蓝牙翻页已关闭", 1200);
            } else {
                _ble.start();
                showToast("蓝牙翻页已开启", 1200);
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
    while (display.displayBusy()) {
        M5.update();
        delay(8);
        yield();
    }
    display.waitDisplay();

    bool oldFrameActive = gShellFrameActive;
    gShellFrameActive = false;
    UITheme::setDrawTarget(nullptr);

    display.clear();
    display.fillScreen(BG_LIGHT);
    drawUiTextCentered(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, msg, TEXT_BLACK);
    display.display();

    gShellFrameActive = oldFrameActive;
    UITheme::setDrawTarget((oldFrameActive && gShellCanvas) ? static_cast<LovyanGFX*>(gShellCanvas) : nullptr);
    
    if (durationMs > 0) {
        delay(durationMs);
    }
}

void App::showToast(const char* msg, int durationMs) {
    if (!msg || !msg[0]) return;
    strlcpy(_toastText, msg, sizeof(_toastText));
    _toastUntil = millis() + (durationMs > 0 ? (unsigned long)durationMs : 1600UL);
    _toastVisible = true;
    _toastDirty = true;
    _toastClearDirty = false;
    drawToastNow();
}

bool App::drawToastNow() {
    if (!_toastVisible || !_toastDirty || !_toastText[0]) return false;
    auto& display = M5.Display;
    if (display.displayBusy()) return false;
    display.waitDisplay();

    const int16_t x = 72;
    const int16_t y = SCREEN_HEIGHT - 72;
    const int16_t w = SCREEN_WIDTH - 144;
    const int16_t h = 36;
    display.setEpdMode(epd_mode_t::epd_fastest);
    display.fillRect(x - 4, y - 4, w + 8, h + 8, BG_LIGHT);
    display.fillRoundRect(x, y, w, h, 10, BG_WHITE);
    display.drawRoundRect(x, y, w, h, 10, BORDER_LIGHT);

    bool oldFrameActive = gShellFrameActive;
    gShellFrameActive = false;
    UITheme::setDrawTarget(nullptr);
    drawUiTextCentered(x, y, w, h, _toastText, TEXT_BLACK);
    gShellFrameActive = oldFrameActive;
    UITheme::setDrawTarget((oldFrameActive && gShellCanvas) ? static_cast<LovyanGFX*>(gShellCanvas) : nullptr);

    display.display(x - 6, y - 6, w + 12, h + 12);
    _toastDirty = false;
    _toastDrawn = true;
    _toastDrawState = _state;
    return true;
}

bool App::clearToastNow() {
    if (!_toastClearDirty) return false;
    // Do not erase toast with a blank partial rectangle. On e-paper that can
    // delete underlying card/text pixels and look like the old "text vanished"
    // failure. Instead, ask the active page to redraw itself through its normal
    // Shell/Reader pipeline.
    _toastClearDirty = false;
    if (!_toastDrawn || _state != _toastDrawState) {
        _toastDrawn = false;
        return false;
    }

    _toastDrawn = false;
    if (isTabState(_state)) {
        int tab = constrain(_activeTab, 0, 3);
        _tabNeedsRender[tab] = true;
    } else if (_state == AppState::READER && _reader.isOpen()) {
        _reader.renderPage();
    } else {
        _pageNeedsRender = true;
    }
    return true;
}

void App::serviceToast() {
    if (_toastVisible && _toastDirty) {
        drawToastNow();
    }
    if (_toastVisible && millis() >= _toastUntil) {
        _toastVisible = false;
        _toastDirty = false;
        _toastText[0] = '\0';
        _toastClearDirty = _toastDrawn;
    }
    if (_toastClearDirty) {
        clearToastNow();
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
    auto& display = uiDrawTarget();
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
    auto& display = uiDrawTarget();

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
    prepareShellFrame();
    drawBackHeader("页码跳转", "输入页码后点击跳转");

    char totalLine[48];
    snprintf(totalLine, sizeof(totalLine), "共 %d 页", _reader.getTotalPages());
    drawUiTextCentered(0, 120, SCREEN_WIDTH, 32, totalLine, TEXT_MID);

    UITheme::drawCard(120, 168, SCREEN_WIDTH - 240, 76, BG_WHITE, BORDER_LIGHT);
    drawUiTextCentered(120, 168, SCREEN_WIDTH - 240, 76, _gotoPageInput[0] ? _gotoPageInput : "_", TEXT_BLACK);

    const char* keys[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "删除", "0", "跳转"};
    const int16_t gridX = 72;
    const int16_t gridY = 292;
    const int16_t keyW = 124;
    const int16_t keyH = 54;
    const int16_t gapX = 24;
    const int16_t gapY = 18;
    for (int i = 0; i < 12; i++) {
        int row = i / 3;
        int col = i % 3;
        int kx = gridX + col * (keyW + gapX);
        int ky = gridY + row * (keyH + gapY);
        UITheme::drawCard(kx, ky, keyW, keyH, BG_WHITE, BORDER_LIGHT);
        drawUiTextCentered(kx, ky, keyW, keyH, keys[i], TEXT_BLACK);
    }
    drawUiTextCentered(0, SCREEN_HEIGHT - 54, SCREEN_WIDTH, 28, "右滑返回 · 点击数字输入", TEXT_MID);
    commitShellFrame();
}


void App::wakeUp() {
    Serial.println("[Sleep] Woken up");
    _lastActivityTime = millis();
    _sleepPending = false;
    _pageNeedsRender = true;
    _touchSuppressUntil = millis() + 300;
    _touching = false;
    _touchLongPressFired = false;
    _touchConsumed = false;
    _touchWaitRelease = true;
    
    M5.Touch.begin(&M5.Display);
    
    if (_state == AppState::READER && _reader.isOpen()) {
        _reader.renderPage();
    } else if (isTabState(_state)) {
        if (_activeTab < 0 || _activeTab > 3) _activeTab = 0;
        _tabNeedsRender[_activeTab] = true;
        switch (_activeTab) {
            case 0: _state = AppState::TAB_READING; handleTabReading(); break;
            case 1: _state = AppState::TAB_LIBRARY; handleTabLibrary(); break;
            case 2: _state = AppState::TAB_TRANSFER; handleTabTransfer(); break;
            case 3: _state = AppState::TAB_SETTINGS; handleTabSettings(); break;
        }
    } else {
        _state = AppState::TAB_READING;
        _activeTab = 0;
        _tabNeedsRender[0] = true;
        handleTabReading();
    }
}

// ===== 字体切换菜单 =====

void App::handleFontMenu() {
    handleSettingsFont();
}


// ===== WiFi传书界面 =====

void App::handleWiFiUpload() {
    prepareShellFrame();
    drawBackHeader("WiFi传书", "浏览器上传本地书籍");

    const int16_t x = contentLeft();
    int16_t y = 124;
    const int16_t w = contentWidth();
    UITheme::drawCard(x, y, w, 146, BG_WHITE, BORDER_LIGHT);
    drawIconLabel(x + 20, y + 20, "wifi", _uploader.isRunning() ? "WiFi 已连接" : "WiFi传书未启动", BG_WHITE);
    if (_uploader.isRunning()) {
        char ipLine[96];
        snprintf(ipLine, sizeof(ipLine), "IP: %s", _uploader.getIP().c_str());
        drawUiText(x + 20, y + 72, ipLine, TEXT_BLACK, BG_WHITE);
        drawUiText(x + 20, y + 106, "端口: 8080", TEXT_MID, BG_WHITE);
    } else {
        drawUiText(x + 20, y + 76, "请到传输页点击开启", TEXT_MID, BG_WHITE);
    }

    y += 170;
    UITheme::drawCard(x, y, w, 170, BG_WHITE, BORDER_LIGHT);
    drawUiText(x + 20, y + 18, "使用方式", TEXT_BLACK, BG_WHITE);
    drawUiText(x + 20, y + 58, "1. 手机/电脑连接同一 WiFi", TEXT_MID, BG_WHITE);
    drawUiText(x + 20, y + 92, "2. 浏览器访问上方 IP:8080", TEXT_MID, BG_WHITE);
    drawUiText(x + 20, y + 126, "3. 选择 .txt 文件上传", TEXT_MID, BG_WHITE);

    if (_uploader.hasNewUpload()) {
        y += 194;
        UITheme::drawCard(x, y, w, 84, BG_WHITE, ACCENT);
        drawUiText(x + 20, y + 18, "新上传", TEXT_BLACK, BG_WHITE);
        drawUiText(x + 20, y + 50, _uploader.getLastUploadName().c_str(), TEXT_MID, BG_WHITE);
        _uploader.clearNewUpload();
        _browser.scan(BOOKS_DIR);
    }

    drawUiTextCentered(0, SCREEN_HEIGHT - 54, SCREEN_WIDTH, 28, "点击返回阅读 · 右滑返回", TEXT_MID);
    commitShellFrame();
}


// ===== 阅读统计 =====

void App::handleReadingStats() {
    prepareShellFrame();
    drawBackHeader("阅读统计", "累计数据和当前书籍");

    const int16_t x = contentLeft();
    const int16_t w = contentWidth();
    int16_t y = 120;
    const int16_t gap = 12;
    const int16_t cardW = (w - gap) / 2;
    struct Stat { const char* label; String value; const char* icon; };
    BookStats book = _reader.isOpen() ? _stats.getCurrentBookStats() : BookStats();
    Stat stats[] = {
        {"今日阅读", String(_stats.formatTime(_stats.getTodaySeconds())), "stats"},
        {"累计阅读", String(_stats.formatTime(_stats.getTotalReadingSeconds())), "stats"},
        {"累计翻页", String(_stats.getTotalPagesRead()) + " 页", "book"},
        {"当前书翻页", _reader.isOpen() ? String(book.totalPagesRead) + " 页" : String("未打开"), "book"},
    };
    for (int i = 0; i < 4; i++) {
        int bx = x + (i % 2) * (cardW + gap);
        int by = y + (i / 2) * 142;
        UITheme::drawCard(bx, by, cardW, 126, BG_WHITE, BORDER_LIGHT);
        drawIconLabel(bx + 18, by + 18, stats[i].icon, stats[i].label, BG_WHITE);
        drawUiText(bx + 20, by + 74, stats[i].value.c_str(), TEXT_BLACK, BG_WHITE);
    }

    y += 304;
    UITheme::drawCard(x, y, w, 190, BG_WHITE, BORDER_LIGHT);
    drawUiText(x + 20, y + 18, "当前书籍", TEXT_BLACK, BG_WHITE);
    if (_reader.isOpen()) {
        drawUiText(x + 20, y + 56, _reader.getBookPath(), TEXT_MID, BG_WHITE);
        char line[64];
        snprintf(line, sizeof(line), "打开次数：%d", book.readCount);
        drawUiText(x + 20, y + 96, line, TEXT_BLACK, BG_WHITE);
        snprintf(line, sizeof(line), "当前书阅读：%s", _stats.formatTime(book.totalSeconds).c_str());
        drawUiText(x + 20, y + 132, line, TEXT_BLACK, BG_WHITE);
    } else {
        drawUiText(x + 20, y + 70, "当前没有打开书籍", TEXT_MID, BG_WHITE);
    }
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
    prepareShellFrame();
    drawBackHeader("Legado同步", "WebDAV 阅读进度同步");

    const int16_t x = contentLeft();
    int16_t y = 124;
    const int16_t w = contentWidth();
    struct Row { const char* name; const char* value; };
    char portLine[24];
    snprintf(portLine, sizeof(portLine), "%d", _legadoPort);
    Row rows[] = {
        {"配置状态", _legadoConfigured ? "已配置" : "未配置"},
        {"主机", _legadoConfigured ? _legadoHost : "/legado_config.json"},
        {"端口", portLine},
        {"同步", _legadoConfigured ? "点击后同步阅读进度" : "配置后可用"},
    };
    if (_legadoConfigured) syncLegadoProgress();
    for (int i = 0; i < 4; i++) {
        UITheme::drawCard(x, y, w, 78, BG_WHITE, BORDER_LIGHT);
        drawUiText(x + 20, y + 14, rows[i].name, TEXT_BLACK, BG_WHITE);
        drawUiText(x + 20, y + 44, rows[i].value, TEXT_MID, BG_WHITE);
        y += 94;
    }
    drawUiTextCentered(0, SCREEN_HEIGHT - 54, SCREEN_WIDTH, 28, "右滑返回 · 修改 SD 卡配置文件", TEXT_MID);
    commitShellFrame();
}


void App::handleWiFiConfig() {
    prepareShellFrame();
    drawBackHeader("WiFi配置", "在SD卡根目录编辑配置文件");

    const int16_t x = contentLeft();
    int16_t y = 124;
    const int16_t w = contentWidth();
    struct Row { const char* name; const char* value; };
    Row rows[] = {
        {"配置状态", _wifiConfigured ? "已配置" : "未配置"},
        {"配置文件", "/wifi_config.json"},
        {"SSID", _wifiConfigured ? _wifiSsid : "未配置"},
        {"密码", "不在屏幕显示"},
        {"生效方式", "保存后重启或重新进入页面"},
    };
    for (int i = 0; i < 5; i++) {
        UITheme::drawCard(x, y, w, 76, BG_WHITE, BORDER_LIGHT);
        drawUiText(x + 20, y + 13, rows[i].name, TEXT_BLACK, BG_WHITE);
        drawUiText(x + 20, y + 43, rows[i].value, TEXT_MID, BG_WHITE);
        y += 90;
    }
    drawUiTextCentered(0, SCREEN_HEIGHT - 54, SCREEN_WIDTH, 28, "右滑返回 · 仅本地保存配置", TEXT_MID);
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
    auto& display = uiDrawTarget();
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
    auto& display = uiDrawTarget();
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
    // Gray glyph draw uses the supplied y as a baseline-ish top plus bearing
    // adjustment. Move up slightly so button/pill text is optically centered.
    int16_t ty = y + (h - th) / 2 - 2;
    drawUiText(x + (w - tw) / 2, ty, text, color, BG_LIGHT);
}

static void drawUiTextRight(int16_t rightX, int16_t y, const char* text, uint16_t color, uint16_t bg) {
    int16_t tw = uiTextWidth(text ? text : "");
    drawUiText(rightX - tw, y, text, color, bg);
}

static void drawUiSectionTitle(int16_t x, int16_t y, const char* title) {
    drawUiText(x, y, title, TEXT_BLACK, BG_LIGHT);
    int16_t tw = uiTextWidth(title);
    uiDrawTarget().drawLine(x, y + 30, x + tw, y + 30, ACCENT);
    uiDrawTarget().drawLine(x, y + 31, x + tw, y + 31, ACCENT);
}

static void drawUiCapsuleSwitch(int16_t x, int16_t y, int16_t w, bool on) {
    auto& display = uiDrawTarget();
    const int16_t h = 30;
    const int16_t pad = 4;
    const int16_t knob = h - pad * 2;
    UITheme::fillRoundRect(x, y, w, h, h / 2, on ? ACCENT : BG_WHITE);
    display.drawRoundRect(x, y, w, h, h / 2, BORDER_LIGHT);
    int16_t knobX = on ? x + w - pad - knob : x + pad;
    int16_t knobY = y + (h - knob) / 2;
    display.fillCircle(knobX + knob / 2, knobY + knob / 2, knob / 2, on ? BG_WHITE : BG_LIGHT);
    display.drawCircle(knobX + knob / 2, knobY + knob / 2, knob / 2, on ? BG_WHITE : BORDER_LIGHT);
    drawUiTextCentered(x, y, w, h, on ? "开" : "关", on ? BG_WHITE : TEXT_MID);
}

static void drawUiSlider(int16_t x, int16_t y, int16_t w, int16_t minVal, int16_t maxVal, int16_t current, const char* unit) {
    auto& display = uiDrawTarget();
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
    auto& display = uiDrawTarget();
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    display.drawRect(x, y, w, 6, TEXT_BLACK);
    int fillW = max(1, (w - 2) * percent / 100);
    if (percent > 0) display.fillRect(x + 1, y + 1, fillW, 4, TEXT_BLACK);
}

static bool iconIs(const char* kind, const char* a, const char* b = nullptr) {
    if (!kind) return false;
    return strcmp(kind, a) == 0 || (b && strcmp(kind, b) == 0);
}

static void drawSlotIcon(int16_t x, int16_t y, const char* kind, uint16_t color, uint16_t bg) {
    auto& display = uiDrawTarget();
    if (!kind) kind = "book";
    // Unified icon system: every icon owns a 30x30 slot and draws a smaller
    // 22-24px glyph centered inside it. This keeps tabs, cards and buttons from
    // looking like mixed icon packs with different visual weights.
    const int16_t cx = x + 15;
    const int16_t cy = y + 15;

    if (iconIs(kind, "book", "read")) {
        const int16_t gx = x + 5;
        const int16_t gy = y + 6;
        display.drawRoundRect(gx, gy, 21, 18, 3, color);
        display.drawLine(gx + 10, gy + 2, gx + 10, gy + 16, color);
        display.drawLine(gx + 4, gy + 6, gx + 8, gy + 5, color);
        display.drawLine(gx + 12, gy + 5, gx + 17, gy + 6, color);
    } else if (iconIs(kind, "shelf")) {
        const int16_t gy = y + 6;
        for (int i = 0; i < 3; i++) {
            int16_t gx = x + 5 + i * 7;
            display.drawRect(gx, gy, 5, 18, color);
            display.drawLine(gx + 1, gy + 5, gx + 3, gy + 5, color);
        }
        display.drawLine(x + 4, y + 25, x + 26, y + 25, color);
    } else if (iconIs(kind, "send")) {
        display.drawLine(cx, y + 6, cx, y + 21, color);
        display.drawLine(cx, y + 6, cx - 6, y + 12, color);
        display.drawLine(cx, y + 6, cx + 6, y + 12, color);
        display.drawRoundRect(x + 6, y + 19, 18, 7, 3, color);
        display.fillRect(x + 8, y + 18, 14, 5, bg);
    } else if (iconIs(kind, "settings", "gear")) {
        display.drawCircle(cx, cy, 8, color);
        display.drawCircle(cx, cy, 3, color);
        display.drawLine(cx, y + 3, cx, y + 7, color);
        display.drawLine(cx, y + 23, cx, y + 27, color);
        display.drawLine(x + 3, cy, x + 7, cy, color);
        display.drawLine(x + 23, cy, x + 27, cy, color);
        display.drawLine(x + 7, y + 7, x + 9, y + 9, color);
        display.drawLine(x + 23, y + 7, x + 21, y + 9, color);
        display.drawLine(x + 7, y + 23, x + 9, y + 21, color);
        display.drawLine(x + 23, y + 23, x + 21, y + 21, color);
    } else if (iconIs(kind, "wifi")) {
        display.drawCircle(cx, y + 21, 2, color);
        display.drawCircle(cx, y + 21, 8, color);
        display.drawCircle(cx, y + 21, 13, color);
        display.fillRect(x + 1, y + 21, 28, 10, bg);
    } else if (iconIs(kind, "stats")) {
        display.drawRect(x + 6, y + 15, 4, 9, color);
        display.drawRect(x + 13, y + 10, 4, 14, color);
        display.drawRect(x + 20, y + 6, 4, 18, color);
    } else if (iconIs(kind, "clock")) {
        display.drawCircle(cx, cy, 10, color);
        display.drawLine(cx, cy, cx, y + 8, color);
        display.drawLine(cx, cy, x + 21, y + 18, color);
    } else {
        display.drawRoundRect(x + 5, y + 7, 20, 16, 4, color);
        display.drawLine(x + 10, y + 12, x + 20, y + 12, color);
        display.drawLine(x + 10, y + 17, x + 18, y + 17, color);
    }
}

static void drawRowIcon(int16_t x, int16_t y, const char* kind) {
    drawSlotIcon(x, y, kind, TEXT_BLACK, BG_WHITE);
}

static void drawIconLabel(int16_t x, int16_t y, const char* icon, const char* label, uint16_t bg) {
    drawRowIcon(x, y, icon);
    int16_t fontH = gUiFont && gUiFont->isLoaded() ? gUiFont->getFontSize() : 16;
    drawUiText(x + 40, y + (30 - fontH) / 2 - 2, label, TEXT_BLACK, bg);
}

static void drawBookCoverCard(int16_t x, int16_t y, int16_t w, int16_t h, const char* title, const char* type, int progress, bool dark) {
    auto& display = uiDrawTarget();
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

static void formatStatusTime(char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = '\0';

    m5::rtc_time_t rtc;
    if (M5.Rtc.isEnabled() && M5.Rtc.getTime(&rtc) &&
        rtc.hours >= 0 && rtc.hours < 24 && rtc.minutes >= 0 && rtc.minutes < 60) {
        snprintf(out, outSize, "%02d:%02d", rtc.hours, rtc.minutes);
        return;
    }

    snprintf(out, outSize, "--:--");
}

static void drawCrosslinkStatusBar() {
    auto& display = uiDrawTarget();
    char timeText[12];
    formatStatusTime(timeText, sizeof(timeText));
    drawUiText(22, 8, timeText, TEXT_MID, BG_LIGHT);
#if BATTERY_ICON_ENABLED
    BatteryInfo bat = BatteryInfo::read();
    const int16_t iconX = SCREEN_WIDTH - 44;
    const int16_t iconY = 10;
    char batText[12];
    if (bat.valid) {
        snprintf(batText, sizeof(batText), "%d%%", bat.level);
        int16_t tw = uiTextWidth(batText);
        drawUiText(iconX - tw - 10, 8, batText, TEXT_MID, BG_LIGHT);
    }
    display.drawRect(iconX, iconY + 2, 26, 13, TEXT_BLACK);
    display.drawRect(iconX + 26, iconY + 6, 3, 5, TEXT_BLACK);
    if (bat.valid) {
        int fillW = (bat.level * 22) / 100;
        if (fillW > 0) display.fillRect(iconX + 2, iconY + 4, fillW, 9, TEXT_BLACK);
    }
#endif
    display.drawLine(20, 32, SCREEN_WIDTH - 20, 32, BORDER_LIGHT);
}

static bool ensureShellCanvas() {
    if (gShellCanvas && gShellCanvas->getBuffer()) return true;
    auto& display = M5.Display;
    if (!gShellCanvas) {
        gShellCanvas = new M5Canvas(&display);
    }
    if (!gShellCanvas) return false;
    gShellCanvas->setPsram(true);
    gShellCanvas->setColorDepth(4);
    if (!gShellCanvas->createSprite(SCREEN_WIDTH, SCREEN_HEIGHT)) {
        Serial.println("[Shell] Failed to allocate shell canvas");
        delete gShellCanvas;
        gShellCanvas = nullptr;
        return false;
    }
    return true;
}

static void prepareShellFrame() {
    auto& display = M5.Display;
    // Do not block on every touch before drawing the next shell page. The EPD
    // driver will serialize if it is still busy; avoiding an unconditional
    // wait here makes tab taps feel much less sticky.
    display.powerSaveOff();
    display.setEpdMode(epd_mode_t::epd_fastest);

    ensureShellCanvas();

    gShellFrameActive = (gShellCanvas != nullptr);
    UITheme::setDrawTarget(gShellFrameActive ? static_cast<LovyanGFX*>(gShellCanvas) : nullptr);

    auto& target = uiDrawTarget();
    target.fillScreen(BG_LIGHT);
}

static bool waitShellDisplayReady(uint32_t timeoutMs) {
    auto& display = M5.Display;
    const uint32_t start = millis();
    while (display.displayBusy() && millis() - start < timeoutMs) {
        M5.update();
        delay(8);
        yield();
    }
    if (display.displayBusy()) {
        Serial.printf("[Shell] EPD queue still busy after %lu ms; skip overlapping shell commit\n",
                      (unsigned long)(millis() - start));
        return false;
    }
    display.waitDisplay();
    return true;
}

static bool commitShellFrame() {
    gLastShellCommitOk = false;
    auto& display = M5.Display;
    if (!waitShellDisplayReady(3500)) {
        UITheme::setDrawTarget(nullptr);
        gShellFrameActive = false;
        return false;
    }
    // Draw the whole shell into an off-screen M5Canvas first, then push once.
    // Direct glyph drawing on PaperS3 visibly refreshes one character at a time
    // and later display commits can lose those glyph pixels.
    if (gShellCommitCount > 0 && (gShellCommitCount % SHELL_FULL_REFRESH_EVERY) == 0) {
        display.setEpdMode(epd_mode_t::epd_quality);
    } else {
        // Prefer perceived touch responsiveness for shell/navigation pages.
        // Quality cleanup still happens every few shell commits to control ghosting.
        display.setEpdMode(epd_mode_t::epd_fastest);
    }

    if (gShellFrameActive && gShellCanvas) {
        UITheme::setDrawTarget(nullptr);
        gShellFrameActive = false;
        gShellCanvas->pushSprite(0, 0);
        display.display(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    } else {
        display.display(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
        UITheme::setDrawTarget(nullptr);
        gShellFrameActive = false;
    }
    gShellCommitCount++;
    gLastShellCommitOk = true;
    return true;
}

static void drawBackHeader(const char* title, const char* hint) {
    auto& display = uiDrawTarget();
    const int16_t backX = 20;
    const int16_t topY = 14;
    drawUiTextCentered(backX, topY, 36, 36, "<", TEXT_BLACK);
    int16_t fontH = gUiFont && gUiFont->isLoaded() ? gUiFont->getFontSize() : 16;
    drawUiText(64, topY + (36 - fontH) / 2 - 2, title, TEXT_BLACK, BG_LIGHT);
    drawUiText(20, 58, hint, TEXT_MID, BG_LIGHT);
    display.drawLine(20, 82, SCREEN_WIDTH - 20, 82, BORDER);
    display.drawLine(20, 83, SCREEN_WIDTH - 20, 83, BORDER);
}


void App::navigateBack() {
    _pageNeedsRender = true;
    _touchSuppressUntil = millis() + 180;
    _touching = false;
    _touchLongPressFired = false;
    _touchConsumed = true;
    _touchWaitRelease = true;
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
    auto& display = uiDrawTarget();
    drawCrosslinkStatusBar();

    const char* tabs[] = {"阅读", "书架", "传输", "设置"};
    int16_t gap = 8;
    int16_t tabW = (SCREEN_WIDTH - 48 - gap * 3) / 4;
    int16_t baseX = 24;
    int16_t tabY = 50;
    int16_t tabH = 42;

    for (int i = 0; i < 4; i++) {
        bool active = (i == _activeTab);
        int16_t boxX = baseX + i * (tabW + gap);
        int16_t boxY = tabY;
        int16_t boxH = 32;
        if (active) {
            display.fillRoundRect(boxX, boxY, tabW, boxH, 8, ACCENT);
            display.fillTriangle(boxX + tabW / 2 - 8, boxY + boxH, boxX + tabW / 2 + 8, boxY + boxH, boxX + tabW / 2, boxY + boxH + 7, ACCENT);
        } else {
            display.drawRoundRect(boxX, boxY, tabW, boxH, 8, BORDER_LIGHT);
        }

        const char* icons[] = {"book", "shelf", "send", "settings"};
        uint16_t c = active ? BG_WHITE : TEXT_MID;
        uint16_t iconBg = active ? ACCENT : BG_LIGHT;
        int16_t labelW = uiTextWidth(tabs[i]);
        const int16_t iconW = 30;
        const int16_t innerGap = 7;
        int16_t groupW = iconW + innerGap + labelW;
        int16_t ix = boxX + (tabW - groupW) / 2;
        int16_t iy = boxY + (boxH - iconW) / 2;
        int16_t labelX = ix + iconW + innerGap;
        int16_t labelY = boxY + (boxH - (gUiFont && gUiFont->isLoaded() ? gUiFont->getFontSize() : 16)) / 2 - 2;

        drawSlotIcon(ix, iy, icons[i], c, iconBg);
        // Same vertical math for all four labels; do not hand-offset active tabs.
        drawUiText(labelX, labelY, tabs[i], active ? BG_WHITE : TEXT_MID, active ? ACCENT : BG_LIGHT);
    }
    display.drawLine(20, TOP_TAB_H - 1, SCREEN_WIDTH - 20, TOP_TAB_H - 1, BORDER_LIGHT);
}

void App::renderBottomNav() {
    // M5PaperS3 无物理按键，底部导航栏留空
    // 如需显示电量/时间，可在此添加
}

void App::switchTab(int tab) {
    _pageNeedsRender = true;
    _touchSuppressUntil = millis() + 180;
    _touching = false;
    _touchLongPressFired = false;
    _touchConsumed = true;
    _touchWaitRelease = true;
    if (tab < 0) tab = 0;
    if (tab > 3) tab = 3;
    _activeTab = tab;
    switch (tab) {
        case 0: _state = AppState::TAB_READING; break;
        case 1: _state = AppState::TAB_LIBRARY; break;
        case 2: _state = AppState::TAB_TRANSFER; break;
        case 3: _state = AppState::TAB_SETTINGS; break;
    }
    _tabNeedsRender[tab] = true;
}

bool App::isTabState(AppState state) {
    return state == AppState::TAB_READING || 
           state == AppState::TAB_LIBRARY || 
           state == AppState::TAB_TRANSFER || 
           state == AppState::TAB_SETTINGS;
}

// ===== 📖 读书主页（full-height Crosslink dashboard）=====
void App::handleTabReading() {
    static bool needsRender = true;
    if (!needsRender && !_tabNeedsRender[0]) return;

    prepareShellFrame();
    auto& display = uiDrawTarget();
    renderTopTabs();

    const int16_t x = contentLeft() - 2;
    const int16_t w = contentWidth() + 4;
    int16_t y = contentTop() - 2;
    int recentCount = _recent.getCount();

    drawUiText(x, y, "阅读", TEXT_BLACK, BG_LIGHT);
    drawUiTextRight(x + w, y + 2, recentCount > 0 ? "继续阅读" : "准备开始", TEXT_MID, BG_LIGHT);
    y += 32;

    UITheme::drawCard(x, y, w, 188, BG_WHITE, BORDER_LIGHT);
    const RecentBook* current = recentCount > 0 ? _recent.getBook(0) : nullptr;
    int pct = current ? _recent.getProgressPercent(0) : 0;
    drawBookCoverCard(x + 14, y + 16, 112, 150, current ? current->name : "Vink", current ? "TXT" : "BOOK", pct, false);
    drawUiText(x + 146, y + 20, current ? current->name : "还没有正在阅读的书", TEXT_BLACK, BG_WHITE);
    drawUiText(x + 146, y + 55, current ? "本地书籍 / TXT" : "从书架选择一本书开始阅读", TEXT_MID, BG_WHITE);
    char line[96];
    if (current && current->totalPages > 0) {
        snprintf(line, sizeof(line), "当前进度 %d%%      %d / %d 页", pct, current->currentPage + 1, current->totalPages);
    } else {
        snprintf(line, sizeof(line), "当前进度 %d%%      等待导入", pct);
    }
    drawUiText(x + 146, y + 90, line, TEXT_BLACK, BG_WHITE);
    drawSmallProgress(x + 146, y + 124, w - 174, pct);
    UITheme::drawCard(x + 146, y + 148, 102, 28, BG_WHITE, BORDER_LIGHT);
    drawUiTextCentered(x + 146, y + 148, 102, 28, current ? "打开" : "书架", TEXT_BLACK);
    UITheme::drawCard(x + 260, y + 148, 102, 28, BG_WHITE, BORDER_LIGHT);
    drawUiTextCentered(x + 260, y + 148, 102, 28, "传书", TEXT_BLACK);
    UITheme::drawCard(x + 374, y + 148, 88, 28, BG_WHITE, BORDER_LIGHT);
    drawUiTextCentered(x + 374, y + 148, 88, 28, "详情", TEXT_BLACK);
    y += 208;

    drawUiText(x, y, "最近书籍", TEXT_BLACK, BG_LIGHT);
    char countText[20];
    snprintf(countText, sizeof(countText), "%d 本", min(max(recentCount - 1, 0), 6));
    drawUiTextRight(x + w, y + 1, countText, TEXT_MID, BG_LIGHT);
    y += 28;

    const int cols = 3;
    const int gapX = 10;
    const int gapY = 12;
    const int coverW = (w - gapX * 2) / cols;
    const int coverH = 124;
    for (int i = 0; i < 6; i++) {
        int bx = x + (i % cols) * (coverW + gapX);
        int by = y + (i / cols) * (coverH + gapY);
        const RecentBook* book = (i + 1 < recentCount) ? _recent.getBook(i + 1) : nullptr;
        if (book) {
            drawBookCoverCard(bx, by, coverW, coverH, book->name, "TXT", _recent.getProgressPercent(i + 1), i == 2);
        } else {
            UITheme::drawCard(bx, by, coverW, coverH, BG_WHITE, BORDER_LIGHT);
            display.drawRect(bx + 14, by + 12, coverW - 28, 64, TEXT_MID);
            display.drawLine(bx + 18, by + 22, bx + coverW - 18, by + 70, TEXT_MID);
            drawUiTextCentered(bx + 4, by + 88, coverW - 8, 22, i == 0 ? "打开SD卡" : "空位", TEXT_MID);
        }
    }
    y += coverH * 2 + gapY + 26;

    drawUiText(x, y, "阅读统计", TEXT_BLACK, BG_LIGHT);
    y += 28;
    const int statGap = 10;
    const int statW = (w - statGap * 2) / 3;
    struct StatCard { const char* icon; const char* label; String value; const char* unit; };
    StatCard stats[] = {
        {"clock", "今日", String(_stats.formatTime(_stats.getTodaySeconds())), ""},
        {"stats", "累计", String(_stats.formatTime(_stats.getTotalReadingSeconds())), ""},
        {"book", "翻页", String(_stats.getTotalPagesRead()), "页"},
    };
    for (int i = 0; i < 3; i++) {
        int sx = x + i * (statW + statGap);
        UITheme::drawCard(sx, y, statW, 86, BG_WHITE, BORDER_LIGHT);
        drawRowIcon(sx + 12, y + 10, stats[i].icon);
        drawUiText(sx + 50, y + 13, stats[i].label, TEXT_MID, BG_WHITE);
        drawUiText(sx + 16, y + 50, stats[i].value.c_str(), TEXT_BLACK, BG_WHITE);
        if (stats[i].unit && strlen(stats[i].unit) > 0) {
            int16_t valueW = uiTextWidth(stats[i].value.c_str());
            drawUiText(sx + 20 + valueW, y + 54, stats[i].unit, TEXT_BLACK, BG_WHITE);
        }
    }
    y += 106;

    const char* chips[] = {"书架", "传书", "字体", "刷新"};
    const int chipGap = 10;
    const int chipW = (w - chipGap * 3) / 4;
    for (int i = 0; i < 4; i++) {
        int cx = x + i * (chipW + chipGap);
        UITheme::drawCard(cx, y, chipW, 38, BG_WHITE, BORDER_LIGHT);
        drawUiTextCentered(cx, y, chipW, 38, chips[i], TEXT_BLACK);
    }

    if (commitShellFrame()) {
        needsRender = false;
        _tabNeedsRender[0] = false;
    }
}

// ===== 📚 书架（Crosslink cover grid）=====
void App::handleTabLibrary() {
    static bool needsRender = true;
    if (!needsRender && !_tabNeedsRender[1]) return;

    prepareShellFrame();
    auto& display = uiDrawTarget();
    renderTopTabs();

    static bool libraryScanned = false;
    if (!libraryScanned && _sdReady) {
        _browser.scan(BOOKS_DIR);
        libraryScanned = true;
    }
    const int totalBooks = _browser.getItemCount();
    const int booksPerPage = 9;
    _libraryTotalPages = (totalBooks + booksPerPage - 1) / booksPerPage;
    if (_libraryTotalPages < 1) _libraryTotalPages = 1;
    if (_libraryPage >= _libraryTotalPages) _libraryPage = _libraryTotalPages - 1;

    int16_t x = contentLeft() - 2;
    int16_t y = contentTop() - 2;
    int16_t w = contentWidth() + 4;
    drawUiText(x, y, "书架", TEXT_BLACK, BG_LIGHT);
    drawUiTextRight(x + w, y + 2, "按名打开", TEXT_MID, BG_LIGHT);
    y += 36;

    const int cols = 3;
    const int gapX = 12;
    const int gapY = 14;
    const int cardW = (w - gapX * 2) / 3;
    const int cardH = 142;

    if (totalBooks == 0) {
        for (int i = 0; i < 9; i++) {
            int bx = x + (i % cols) * (cardW + gapX);
            int by = y + (i / cols) * (cardH + gapY);
            UITheme::drawCard(bx, by, cardW, cardH, BG_WHITE, BORDER_LIGHT);
            display.drawRect(bx + 20, by + 14, cardW - 40, 74, TEXT_MID);
            display.drawLine(bx + 24, by + 26, bx + cardW - 24, by + 78, TEXT_MID);
            drawUiTextCentered(bx + 4, by + 102, cardW - 8, 22, i == 0 ? "等待导入" : "书籍", TEXT_MID);
        }
        int actionY = y + 3 * (cardH + gapY) + 6;
        UITheme::drawCard(x, actionY, w, 118, BG_WHITE, BORDER_LIGHT);
        drawIconLabel(x + 24, actionY + 18, "book", "书架为空", BG_WHITE);
        drawUiText(x + 66, actionY + 50, "请将 TXT / EPUB 放入 SD 卡 /books", TEXT_MID, BG_WHITE);
        UITheme::drawCard(x + w - 160, actionY + 66, 136, 38, BG_WHITE, BORDER);
        drawUiTextCentered(x + w - 160, actionY + 66, 136, 38, "打开SD卡", TEXT_BLACK);
        if (commitShellFrame()) {
            needsRender = false;
            _tabNeedsRender[1] = false;
        }
        return;
    }

    int startIdx = _libraryPage * booksPerPage;
    for (int i = 0; i < booksPerPage; i++) {
        int idx = startIdx + i;
        if (idx >= totalBooks) break;
        const FileItem* item = _browser.getItem(idx);
        if (!item) continue;
        int bx = x + (i % cols) * (cardW + gapX);
        int by = y + (i / cols) * (cardH + gapY);
        drawBookCoverCard(bx, by, cardW, cardH, item->name, item->isDirectory ? "DIR" : "TXT", item->isDirectory ? -1 : 0, idx == _librarySelected);
    }

    char pageText[32];
    snprintf(pageText, sizeof(pageText), "%d of %d", _libraryPage + 1, _libraryTotalPages);
    drawUiTextCentered(0, SCREEN_HEIGHT - 58, SCREEN_WIDTH, 30, pageText, TEXT_MID);

    if (commitShellFrame()) {
        needsRender = false;
        _tabNeedsRender[1] = false;
    }
}

// ===== 🛜 传输中心（Crosslink status panel）=====
void App::handleTabTransfer() {
    static bool needsRender = true;
    if (!needsRender && !_tabNeedsRender[2]) return;

    prepareShellFrame();
    renderTopTabs();

    int16_t x = contentLeft() + 6;
    int16_t y = contentTop() - 2;
    int16_t w = contentWidth() - 12;
    drawUiText(x, y, "连接与传输", TEXT_BLACK, BG_LIGHT);
    drawUiTextRight(x + w, y + 2, _uploader.isRunning() ? "服务中" : "待连接", TEXT_MID, BG_LIGHT);

    int panelY = y + 38;
    UITheme::drawCard(x, panelY, w, 122, BG_WHITE, BORDER_LIGHT);
    drawIconLabel(x + 24, panelY + 18, "wifi", "网络状态", BG_WHITE);
    char ipLine[96];
    if (_uploader.isRunning()) snprintf(ipLine, sizeof(ipLine), "IP: %s", _uploader.getIP().c_str());
    else snprintf(ipLine, sizeof(ipLine), "WiFi传书未开启");
    drawUiText(x + 64, panelY + 54, ipLine, TEXT_MID, BG_WHITE);
    UITheme::drawCard(x + w - 118, panelY + 36, 90, 36, BG_WHITE, BORDER_LIGHT);
    drawUiTextCentered(x + w - 118, panelY + 36, 90, 36, _uploader.isRunning() ? "关闭" : "开启", TEXT_BLACK);

    struct TransferItem { const char* title; const char* desc; bool enabled; const char* icon; bool toggle; };
    TransferItem items[] = {
        {"WiFi传书", "浏览器上传", _uploader.isRunning(), "wifi", true},
        {"WiFi配置", _wifiConfigured ? _wifiSsid : "wifi_config", _wifiConfigured, "settings", true},
        {"Legado同步", _legadoConfigured ? _legadoHost : "legado_config", _legadoConfigured, "book", true},
        {"蓝牙翻页", _ble.isRunning() ? "已开启" : "未开启", _ble.isRunning(), "settings", true},
        {"阅读统计", "累计数据", true, "stats", false},
        {"打开书架", "/books", true, "book", false},
    };

    int gridY = panelY + 148;
    const int cols = 2;
    const int gap = 12;
    const int cardW = (w - gap) / 2;
    const int cardH = 128;
    for (int i = 0; i < 6; i++) {
        int col = i % cols;
        int row = i / cols;
        int bx = x + col * (cardW + gap);
        int by = gridY + row * (cardH + gap);
        UITheme::drawCard(bx, by, cardW, cardH, BG_WHITE, BORDER_LIGHT);
        drawIconLabel(bx + 18, by + 14, items[i].icon, items[i].title, BG_WHITE);
        drawUiText(bx + 18, by + 54, items[i].desc, TEXT_MID, BG_WHITE);
        if (items[i].toggle) {
            drawUiCapsuleSwitch(bx + cardW - 76, by + 78, 56, items[i].enabled);
        } else {
            drawUiText(bx + cardW - 34, by + 84, ">", TEXT_BLACK, BG_WHITE);
        }
    }

    int guideY = gridY + 3 * (cardH + gap) + 10;
    UITheme::drawCard(x, guideY, w, 72, BG_WHITE, BORDER_LIGHT);
    drawUiText(x + 22, guideY + 14, "触摸提示", TEXT_BLACK, BG_WHITE);
    drawUiText(x + 22, guideY + 42, "点卡片执行；右滑返回子页面", TEXT_MID, BG_WHITE);

    if (commitShellFrame()) {
        needsRender = false;
        _tabNeedsRender[2] = false;
    }
}

// ===== ⚙️ 设置页（full-height Crosslink settings）=====
void App::handleTabSettings() {
    static bool needsRender = true;
    if (!needsRender && !_tabNeedsRender[3]) return;

    prepareShellFrame();
    renderTopTabs();

    const int16_t x = contentLeft() + 4;
    const int16_t y = contentTop() - 2;
    const int16_t w = contentWidth() - 8;
    drawUiText(x, y, "设置", TEXT_BLACK, BG_LIGHT);
    drawUiTextRight(x + w, y + 2, "轻触修改", TEXT_MID, BG_LIGHT);

    RefreshStrategy rs = _reader.getRefreshStrategy();
    char sleepText[24];
    snprintf(sleepText, sizeof(sleepText), "%d 分钟", _sleepTimeoutMin);

    struct SettingItem {
        const char* group;
        const char* title1;
        const char* value1;
        const char* title2;
        const char* value2;
        bool segmented1;
        bool segmented2;
    };
    SettingItem groups[] = {
        {"阅读", "正文字体", _fontCount > 0 ? _fontNames[0] : "内置字体", "字号", "小    中    大", false, true},
        {"版式", "对齐方式", "左    中    右    齐", "刷新策略", rs.getLabel(), true, false},
        {"连接", "WiFi配置", _wifiConfigured ? _wifiSsid : "未配置", "Legado同步", _legadoConfigured ? _legadoHost : "未配置", false, false},
        {"系统", "休眠时间", sleepText, "关于", "Vink-PaperS3 / v0.2.15", false, false},
    };

    const int cardGap = 14;
    const int cardH = 176;  // deliberately tall: use the PaperS3 vertical space.
    const int headerH = 36;
    const int rowH = 62;
    int cardY = y + 38;

    auto drawSettingRow = [&](int rowX, int rowY, int rowW, const char* title, const char* value, bool segmented, bool arrow) {
        drawUiText(rowX + 18, rowY + 8, title, TEXT_BLACK, BG_WHITE);
        if (segmented) {
            const int pillY = rowY + 34;
            UITheme::drawCard(rowX + 144, pillY - 4, rowW - 190, 34, BG_WHITE, BORDER_LIGHT);
            drawUiTextCentered(rowX + 144, pillY - 4, rowW - 190, 34, value, TEXT_BLACK);
        } else {
            // Value sits on its own lower line. This uses vertical space instead
            // of squeezing long labels and values into one baseline.
            drawUiText(rowX + 18, rowY + 36, value, TEXT_MID, BG_WHITE);
        }
        if (arrow) drawUiText(rowX + rowW - 34, rowY + 22, ">", TEXT_BLACK, BG_WHITE);
    };

    for (int g = 0; g < 4; g++) {
        UITheme::drawCard(x, cardY, w, cardH, BG_WHITE, BORDER_LIGHT);
        drawUiText(x + 18, cardY + 10, groups[g].group, TEXT_BLACK, BG_WHITE);
        uiDrawTarget().drawLine(x + 18, cardY + headerH, x + w - 18, cardY + headerH, BORDER_LIGHT);

        int row1Y = cardY + headerH + 6;
        int row2Y = row1Y + rowH + 10;
        drawSettingRow(x, row1Y, w, groups[g].title1, groups[g].value1, groups[g].segmented1, true);
        uiDrawTarget().drawLine(x + 18, row2Y - 6, x + w - 18, row2Y - 6, BORDER_LIGHT);
        drawSettingRow(x, row2Y, w, groups[g].title2, groups[g].value2, groups[g].segmented2, true);

        cardY += cardH + cardGap;
    }

    // Subtle footer consumes remaining space like the reference pages instead of
    // leaving a visually accidental blank area.
    drawUiTextCentered(0, SCREEN_HEIGHT - 42, SCREEN_WIDTH, 24, "右滑返回 · 点击条目进入", TEXT_MID);

    if (commitShellFrame()) {
        needsRender = false;
        _tabNeedsRender[3] = false;
    }
}

// ===== 阅读弹出菜单（半透明遮罩）=====
void App::handleReaderMenu() {
    prepareShellFrame();
    drawBackHeader("阅读菜单", "点 < / 右滑返回阅读");

    const int16_t x = contentLeft();
    const int16_t w = contentWidth();
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
    const int numItems = 12;
    const int rowH = 50;
    const int gap = 8;
    int y = 106;
    for (int i = 0; i < numItems; i++) {
        uint16_t border = (i == _menuIndex) ? ACCENT : BORDER_LIGHT;
        UITheme::drawCard(x, y, w, rowH, BG_WHITE, border);
        drawUiText(x + 18, y + 14, items[i], i == _menuIndex ? TEXT_BLACK : TEXT_DARK, BG_WHITE);
        if (i == _menuIndex) drawUiTextRight(x + w - 18, y + 14, "当前", TEXT_MID, BG_WHITE);
        y += rowH + gap;
        if (y > SCREEN_HEIGHT - 70) break;
    }
    drawUiTextCentered(0, SCREEN_HEIGHT - 44, SCREEN_WIDTH, 24, "上下滑选择 · 点击执行", TEXT_MID);
    commitShellFrame();
}

// ===== 排版设置（卡片式 + 滑块）=====
void App::handleSettingsLayout() {
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
        drawUiText(contentLeft() + 18, y + 14, rows[i].name, TEXT_BLACK, BG_WHITE);
        if (i == _layoutEditorIndex) drawUiTextRight(contentRight() - 18, y + 14, "当前", TEXT_MID, BG_WHITE);
        drawUiSlider(contentLeft() + 18, y + 64, contentWidth() - 138, rows[i].minVal, rows[i].maxVal, rows[i].value, rows[i].unit);
        y += 126;
    }
    commitShellFrame();
}

// ===== 残影控制（胶囊开关式）=====
void App::handleSettingsRefresh() {
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
    prepareShellFrame();
    drawBackHeader("正文字体", "仅影响阅读正文，点 < 返回");

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
        drawUiText(contentLeft() + 18, y + 22, _fontNames[i], TEXT_BLACK, BG_WHITE);
        if (isCurrent) drawUiTextRight(contentRight() - 18, y + 22, "当前", TEXT_MID, BG_WHITE);
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
    prepareShellFrame();
    drawBackHeader("章节目录", "点击章节跳转，右滑返回");

    int chapterCount = _reader.getChapterCount();
    if (chapterCount <= 0) {
        drawUiTextCentered(0, 260, SCREEN_WIDTH, 80, "未检测到章节", TEXT_MID);
        drawUiTextCentered(0, 330, SCREEN_WIDTH, 50, "长按菜单可自动识别章节", TEXT_MID);
        commitShellFrame();
        return;
    }

    const ChapterInfo* chapters = _reader.getChapterList();
    int currentChapter = _reader.getCurrentChapterIndex();
    int itemsPerPage = 12;
    int totalPages = (chapterCount + itemsPerPage - 1) / itemsPerPage;
    int currentPage = _chapterMenuScroll / itemsPerPage;
    int startIdx = currentPage * itemsPerPage;
    int endIdx = min(startIdx + itemsPerPage, chapterCount);

    const int16_t x = contentLeft();
    const int16_t w = contentWidth();
    const int16_t itemH = 50;
    int16_t y = 104;
    for (int i = startIdx; i < endIdx; i++) {
        bool selected = (i == _chapterMenuIndex);
        UITheme::drawCard(x, y, w, itemH, selected ? BG_MID : BG_WHITE, i == currentChapter ? ACCENT : BORDER_LIGHT);
        String title = chapters[i].title;
        if (title.length() > 28) title = title.substring(0, 25) + "...";
        char chapterLine[160];
        snprintf(chapterLine, sizeof(chapterLine), "%d. %s", i + 1, title.c_str());
        int16_t fontH = gUiFont && gUiFont->isLoaded() ? gUiFont->getFontSize() : 16;
        drawUiText(x + 18, y + (itemH - fontH) / 2 - 2, chapterLine, i == currentChapter ? ACCENT : TEXT_DARK, selected ? BG_MID : BG_WHITE);
        y += itemH + 8;
    }
    char chapterPageText[32];
    snprintf(chapterPageText, sizeof(chapterPageText), "%d/%d 页", currentPage + 1, totalPages);
    drawUiTextCentered(0, SCREEN_HEIGHT - 54, SCREEN_WIDTH, 28, chapterPageText, TEXT_MID);
    commitShellFrame();
}


// ===== 书签列表（改进版）=====
void App::handleBookmarkList() {
    prepareShellFrame();
    drawBackHeader("我的书签", "点击书签跳转，右滑返回");

    int bmCount = _reader.getBookmarkCount();
    if (bmCount <= 0) {
        drawUiTextCentered(0, 260, SCREEN_WIDTH, 80, "暂无书签", TEXT_MID);
        drawUiTextCentered(0, 330, SCREEN_WIDTH, 50, "阅读时点击菜单添加书签", TEXT_MID);
        commitShellFrame();
        return;
    }

    const Bookmark* bookmarks = _reader.getBookmarks();
    int itemsPerPage = 12;
    int totalPages = (bmCount + itemsPerPage - 1) / itemsPerPage;
    int currentPage = _bookmarkMenuScroll / itemsPerPage;
    int startIdx = currentPage * itemsPerPage;
    int endIdx = min(startIdx + itemsPerPage, bmCount);

    const int16_t x = contentLeft();
    const int16_t w = contentWidth();
    const int16_t itemH = 50;
    int16_t y = 104;
    for (int i = startIdx; i < endIdx; i++) {
        bool selected = (i == _bookmarkMenuIndex);
        UITheme::drawCard(x, y, w, itemH, selected ? BG_MID : BG_WHITE, BORDER_LIGHT);
        String name = bookmarks[i].name;
        if (name.length() > 26) name = name.substring(0, 23) + "...";
        char bookmarkLine[160];
        snprintf(bookmarkLine, sizeof(bookmarkLine), "%d. %s (P%d)", i + 1, name.c_str(), bookmarks[i].pageNum + 1);
        int16_t fontH = gUiFont && gUiFont->isLoaded() ? gUiFont->getFontSize() : 16;
        drawUiText(x + 18, y + (itemH - fontH) / 2 - 2, bookmarkLine, TEXT_DARK, selected ? BG_MID : BG_WHITE);
        y += itemH + 8;
    }
    char bookmarkPageText[48];
    snprintf(bookmarkPageText, sizeof(bookmarkPageText), "%d 个书签 | %d/%d", bmCount, currentPage + 1, totalPages);
    drawUiTextCentered(0, SCREEN_HEIGHT - 54, SCREEN_WIDTH, 28, bookmarkPageText, TEXT_MID);
    commitShellFrame();
}
