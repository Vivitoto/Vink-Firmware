#pragma once
#include <Arduino.h>
#include <SD.h>

// 支持的编码类型
enum class TextEncoding {
    UTF8,       // UTF-8 (带或不带 BOM)
    GBK,        // GBK / GB2312
    UNKNOWN     // 无法识别，默认按 UTF-8 处理
};

class TextCodec {
public:
    TextCodec();
    
    // 检测文件编码（读取前 4KB 样本）
    static TextEncoding detect(File& file);
    
    // 将 GBK 文件转换为临时 UTF-8 文件
    // 返回转换后的临时文件路径（放在 PROGRESS_DIR 下），失败返回 ""
    static String convertToUTF8(const char* inputPath);
    
    // 检查并删除已存在的临时文件（用于 closeBook 时清理）
    static void cleanupTempFile(const char* path);
    
private:
    // GBK → UTF-8 转换器（状态机）
    class GBKConverter {
    public:
        GBKConverter();
        // 处理一个字节，返回转换后的 UTF-8 字节数，写入 outBuf（至少 4 字节空间）
        // 返回 0 表示该字节尚未形成完整字符（等待下一个字节）
        // 返回 -1 表示无效序列，输出替代字符
        int feedByte(uint8_t byte, uint8_t* outBuf);
        // 重置状态机
        void reset();
    private:
        uint8_t _pending;   // 等待的第二个字节
        bool _hasPending;   // 是否有等待的字节
    };
    
    // UTF-8 验证：检查字节序列是否有效
    static bool isValidUTF8(const uint8_t* buf, size_t len, size_t& consumed);
    
    // GBK 范围检查
    static bool isGBKRange(uint8_t first, uint8_t second);
    
    // 查表：GBK 双字节 → Unicode codepoint
    static uint16_t gbkToUnicode(uint8_t first, uint8_t second);
    
    // Unicode codepoint → UTF-8 编码，返回写入字节数
    static int unicodeToUTF8(uint16_t codepoint, uint8_t* out);
};
