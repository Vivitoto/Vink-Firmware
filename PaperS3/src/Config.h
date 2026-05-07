#pragma once
#include <Arduino.h>

// ===== 屏幕参数 (M5Stack Paper S3) =====
// Match ReadPaper's proven PaperS3 portrait framebuffer geometry.
#define SCREEN_WIDTH    540
#define SCREEN_HEIGHT   960
#define EINK_GRAY_LEVELS 16

// ===== 目录路径 =====
#define BOOKS_DIR       "/books"
#define FONTS_DIR       "/fonts"
#define PROGRESS_DIR    "/progress"
#define CONFIG_PATH     "/ebook_config.json"

// ===== 字库文件 =====
// 默认字体：霞鹜文楷 (LXGW WenKai)
// 需要先用 tools/generate_gray_font.py 生成字库文件并放到 SD 卡 /fonts/ 目录
#define FONT_FILE_16    "/fonts/wenkai16_gray.fnt"
#define FONT_FILE_20    "/fonts/wenkai20_gray.fnt"
#define FONT_FILE_24    "/fonts/wenkai24_gray.fnt"
#define FONT_FILE_32    "/fonts/wenkai32_gray.fnt"

// ===== 字库类型 =====
enum class FontType {
    BITMAP_1BPP,    // 1bpp 黑白点阵（旧格式）
    GRAY_4BPP       // 4bpp 灰度点阵（推荐，16级灰度抗锯齿）
};

// ===== 排版配置（可在阅读时动态调整）=====
struct LayoutConfig {
    uint8_t fontSize;           // 字号 (12-48)
    uint8_t lineSpacing;        // 行间距，相对于字号的百分比 (50-200)
    uint8_t paragraphSpacing;   // 段落间距，相对于行高的百分比 (0-100)
    uint8_t marginLeft;         // 左边距 (0-120)
    uint8_t marginRight;        // 右边距 (0-120)
    uint8_t marginTop;          // 上边距 (0-100)
    uint8_t marginBottom;       // 下边距 (0-100)
    uint8_t indentFirstLine;    // 首行缩进，字符数 (0-4)
    bool    justify;            // 两端对齐 (true/false)
    
    // 默认配置
    static LayoutConfig Default() {
        return {
            .fontSize = 24,
            .lineSpacing = 60,          // 60% = 1.6倍行距
            .paragraphSpacing = 50,     // 50% = 半行段间距
            .marginLeft = 24,
            .marginRight = 24,
            .marginTop = 20,
            .marginBottom = 20,
            .indentFirstLine = 2,       // 2字符缩进
            .justify = false
        };
    }
    
    // 计算实际行高（像素）
    uint16_t calcLineHeight() const {
        return fontSize + (fontSize * lineSpacing / 100);
    }
    
    // 计算段后额外间距（像素）
    uint16_t calcParagraphExtra() const {
        return (calcLineHeight() * paragraphSpacing / 100);
    }
    
    // 计算内容区域
    uint16_t contentWidth() const { return SCREEN_WIDTH - marginLeft - marginRight; }
    uint16_t contentHeight() const { return SCREEN_HEIGHT - marginTop - marginBottom; }
};

// ===== 页面边距（默认值，会被 LayoutConfig 覆盖）=====
#define MARGIN_LEFT     24
#define MARGIN_RIGHT    24
#define MARGIN_TOP      20
#define MARGIN_BOTTOM   20
#define LINE_SPACING    6

// ===== 触摸区域 =====
#define ZONE_LEFT_X     0
#define ZONE_LEFT_W     180
#define ZONE_CENTER_X   180
#define ZONE_CENTER_W   180
#define ZONE_RIGHT_X    360
#define ZONE_RIGHT_W    180

// ===== 应用状态 =====
enum class AppState {
    INIT,
    // 主标签页
    TAB_READING,      // 📖 读书主页（最近阅读 + 统计）
    TAB_LIBRARY,      // 📚 书架（9宫格封面）
    TAB_TRANSFER,     // 🛜 传输（WiFi/蓝牙/同步）
    TAB_SETTINGS,     // ⚙️ 设置（卡片式）
    // 阅读状态
    READER,
    READER_MENU,      // 阅读时弹出菜单（半透明遮罩）
    // 设置子页面
    SETTINGS_LAYOUT,  // 排版设置
    SETTINGS_REFRESH, // 残影控制
    SETTINGS_FONT,    // 字体切换
    SETTINGS_WIFI,    // WiFi配置
    SETTINGS_LEGADO,  // Legado同步
    // 阅读工具
    CHAPTER_LIST,     // 章节目录
    BOOKMARK_LIST,    // 书签列表
    GOTO_PAGE,        // 页码跳转
    READING_STATS,    // 阅读统计
    // 其他
    WIFI_UPLOAD,
    MESSAGE,
    // 兼容旧状态名
    FILE_BROWSER = TAB_READING,
    MENU = READER_MENU,
    MENU_CHAPTER = CHAPTER_LIST,
    MENU_GOTO_PAGE = GOTO_PAGE,
    MENU_LAYOUT = SETTINGS_LAYOUT,
    MENU_REFRESH = SETTINGS_REFRESH,
    MENU_BOOKMARK = BOOKMARK_LIST,
    MENU_FONT = SETTINGS_FONT
};

// ===== 休眠省电配置 =====
#define AUTO_SLEEP_ENABLED      1
#define AUTO_SLEEP_DEFAULT_MIN  5     // 默认5分钟
#define AUTO_SLEEP_MIN_MIN      1     // 最短1分钟
#define AUTO_SLEEP_MAX_MIN      60    // 最长60分钟

// ===== 电量显示 =====
#define BATTERY_ICON_ENABLED    1
#define BATTERY_ICON_X          4
#define BATTERY_ICON_Y          4

// ===== 字体大小 =====
enum class FontSize {
    SMALL = 16,
    MEDIUM = 20,
    LARGE = 24
};

// 电池状态
struct BatteryInfo {
    int level;      // 0-100
    bool charging;  // 是否充电中
    bool valid;     // 是否读取成功
    
    static BatteryInfo read();  // 实现在 App.cpp 中
};

// ===== 刷新策略（局刷/全刷控制）=====
// ===== 刷新频率档位 =====
enum class RefreshFrequency {
    FREQ_LOW,      // 极速：DU4 快刷，每20页 GC16 全刷
    FREQ_MEDIUM,   // 均衡：DU 快刷，每10页 GC16 全刷
    FREQ_HIGH      // 清晰：GL16 文本刷新，每5页 GC16 全刷
};

struct RefreshStrategy {
    RefreshFrequency frequency;
    uint8_t fullRefreshEvery;   // 每 N 页做一次全刷
    bool usePartialUpdate;      // 是否启用局刷
    
    static RefreshStrategy FromFrequency(RefreshFrequency freq) {
        switch (freq) {
            case RefreshFrequency::FREQ_LOW:
                return { .frequency = freq, .fullRefreshEvery = 20, .usePartialUpdate = true };
            case RefreshFrequency::FREQ_MEDIUM:
                return { .frequency = freq, .fullRefreshEvery = 10, .usePartialUpdate = true };
            case RefreshFrequency::FREQ_HIGH:
                return { .frequency = freq, .fullRefreshEvery = 5, .usePartialUpdate = true };
            default:
                return { .frequency = RefreshFrequency::FREQ_MEDIUM, .fullRefreshEvery = 5, .usePartialUpdate = true };
        }
    }
    
    const char* getLabel() const {
        switch (frequency) {
            case RefreshFrequency::FREQ_LOW: return "极速";
            case RefreshFrequency::FREQ_MEDIUM: return "均衡";
            case RefreshFrequency::FREQ_HIGH: return "清晰";
        }
        return "中";
    }
};

// ===== 性能优化配置 =====
#define PRELOAD_NEXT_PAGE       1   // 预读下一页文本到内存
#define PRELOAD_BUFFER_SIZE     8192 // 预读缓冲区大小
#define GLYPH_L1_CACHE_SIZE     500  // 字库热缓存字数（PSRAM）
