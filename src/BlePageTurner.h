#pragma once
#include <Arduino.h>

// BLE 翻页器支持
// 使用 NimBLE 库（轻量级 BLE）
// 手机端可用任意 BLE 调试工具发送 "N" (next) / "P" (prev)

class BlePageTurner {
public:
    BlePageTurner();
    
    // 启动/停止 BLE 服务
    bool start();
    void stop();
    bool isRunning() const { return _running; }
    
    // 主循环中调用，检查是否有翻页指令
    // 返回: 0=无, 1=下一页, -1=上一页
    int checkCommand();
    
    // 获取设备名称（用于手机扫描）
    const char* getDeviceName() const { return "Vink-PaperS3"; }
    
public:
    bool _running;
    int _pendingCommand;
    
    void setPendingCommand(int cmd) { _pendingCommand = cmd; }
    
    static void onWriteCallback(const char* data, int len);
    static BlePageTurner* _instance;
};
