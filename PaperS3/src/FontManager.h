#pragma once
#include <Arduino.h>
#include <SD.h>
#include "Config.h"

// ===== 1bpp 黑白字库格式 =====
struct FontHeader_1bpp {
    char magic[4];      // "FNT\0"
    uint16_t version;   // 1
    uint16_t fontSize;  // 字号（像素高度）
    uint32_t charCount; // 字符数量
};

struct CharIndex_1bpp {
    uint32_t unicode;   // Unicode codepoint
    uint32_t offset;    // 点阵数据在文件中的偏移
    uint8_t width;      // 字符宽度
    uint8_t height;     // 字符高度
};

// ===== 4bpp 灰度字库格式 =====
struct FontHeader_gray {
    char magic[4];      // "GRAY"
    uint16_t version;   // 1
    uint16_t fontSize;  // 字号（像素高度）
    uint32_t charCount; // 字符数量
    uint32_t bitmapSize;// 位图数据总大小
};

struct CharIndex_gray {
    uint32_t unicode;   // Unicode codepoint
    uint32_t offset;    // 点阵数据在文件中的偏移
    uint8_t width;      // 字符宽度
    uint8_t height;     // 字符高度
    int8_t bearingX;    // 水平偏移（基线对齐）
    int8_t bearingY;    // 垂直偏移
    uint8_t advance;    // 水平步进
    uint8_t reserved;
    uint16_t reserved2;
};

class FontManager {
public:
    FontManager();
    ~FontManager();
    
    // 加载字库文件（自动检测格式）。默认会优先尝试 SD，然后尝试内置 SPIFFS。
    bool loadFont(const char* path);
    // 强制从固件内置 SPIFFS 加载字库，避免 SD 卡上的旧/坏字体覆盖系统 UI 字体。
    bool loadBundledFont(const char* path);
    // 从固件 flash(PROGMEM) 加载内置 1bpp 字库
    bool loadBuiltinFont();
    void unload();
    bool isLoaded() const;
    
    // 获取字库类型
    FontType getFontType() const { return _fontType; }
    
    // 扫描可用字体
    static int scanFonts(char paths[][128], char names[][64], int maxCount);
    
    // 获取当前字体路径
    const char* getCurrentFontPath() const { return _currentPath; }
    
    // ===== 1bpp 黑白字库接口 =====
    const uint8_t* getCharBitmap(uint32_t unicode, uint8_t& outWidth, uint8_t& outHeight);
    
    // ===== 4bpp 灰度字库接口 =====
    // 获取字符灰度位图数据（4bpp，每字节存储2个像素）
    const uint8_t* getCharBitmapGray(uint32_t unicode, uint8_t& outWidth, uint8_t& outHeight,
                                     int8_t& outBearingX, int8_t& outBearingY, uint8_t& outAdvance);
    
    // 通用接口
    uint8_t getCharWidth(uint32_t unicode);
    uint8_t getCharAdvance(uint32_t unicode);  // 水平步进（包含字间距）
    uint16_t getFontSize() const;
    
    // 渲染辅助
    // 将 4bpp 数据解压到 8bpp 缓冲区（用于直接写入 epdiy framebuffer）
    static void unpackGray4To8(const uint8_t* src, uint8_t* dst, int width, int height);
    
private:
    bool loadBitmapFont();
    bool loadGrayFont();
    
    File _file;
    FontType _fontType;
    char _currentPath[128];
    bool _builtinLoaded;
    const uint8_t* _builtinBitmapData;
    
    // 1bpp 数据
    FontHeader_1bpp _header_1bpp;
    CharIndex_1bpp* _index_1bpp;
    
    // 4bpp 灰度数据
    FontHeader_gray _header_gray;
    CharIndex_gray* _index_gray;
    
    uint8_t* _bitmapBuffer; // 临时点阵缓冲区（PSRAM）
    
    int findCharIndex(uint32_t unicode);
};
