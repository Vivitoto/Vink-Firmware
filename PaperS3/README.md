# Vink-PaperS3

Vink 系列面向 M5Stack PaperS3 的电子书阅读固件。

当前主线是 `v0.3` ReadPaper-style 架构：Arduino `setup()` 只启动一个 pinned supervisor task，运行期拆成 display / input / state / reader / sync 服务，避免旧版单体 `App::run()` 继续膨胀。

## 当前状态

- 设备：M5Stack PaperS3 / ESP32-S3 / 16MB Flash / 8MB PSRAM
- 显示：PaperS3 EPD，Vink 逻辑竖屏 `540×960`
- 触摸：M5Unified `M5.update()` + `M5.Touch.getDetail()` raw 坐标路径
- 发布：只面向用户发布完整 `full-16MB.bin`，从 `0x0` 烧录
- App 清单：`releases.json` 是 Vink Flasher 当前唯一读取入口

## 目录结构

```text
PaperS3/
├── platformio.ini              # PlatformIO / Arduino 构建配置
├── custom_16MB.csv             # 16MB full-only 分区表
├── releases.json               # Vink Flasher 固件发布清单
├── data/fonts/                 # SPIFFS 内置字体资源
├── src/
│   ├── main.cpp                # v0.3 入口，只启动 MainTask
│   ├── Config.h                # 共享常量 / PaperS3 管脚 / 字体路径
│   ├── FontManager.*           # SPIFFS/SD 字库加载与绘制
│   ├── ChapterDetector.*       # TXT 章节识别
│   ├── TextCodec.*             # UTF-8 / GBK 检测与转换
│   ├── GBKTable.h              # legacy GBK 表
│   └── vink3/                  # 当前 v0.3 主架构
│       ├── display/            # EPD 推送队列与刷新策略
│       ├── input/              # 触摸 / 电源键输入服务
│       ├── reader/             # 书库、章节、正文渲染
│       ├── runtime/            # 服务初始化与主循环
│       ├── state/              # 消息与状态机
│       ├── sync/               # Legado 同步服务骨架
│       ├── text/               # CJK / ReadPaper 字体资源
│       └── ui/                 # Shell / 诊断 / 设置 UI
├── tools/                      # 字体、GBK 表、full 镜像构建工具
├── tests/                      # 本地 smoke gate
└── docs/                       # 硬件、架构、验证记录
```

## 构建完整烧录包

```bash
cd Vink/firmware/Vink-Firmware/PaperS3
./tools/build_full_firmware.sh
```

输出完整 16MB 镜像到：

```text
/home/vito/.openclaw/workspace/artifacts/Vink-PaperS3/
```

用户侧烧录只使用 `*-full-16MB.bin`，flash offset 为 `0x0`。`buildfs`/SPIFFS 镜像只是合包中间产物，不单独发布。

## 本地验证

```bash
cd Vink/firmware/Vink-Firmware/PaperS3
python3 tests/local_firmware_smoke.py --strict-artifacts
```

如需同时构建：

```bash
python3 tests/local_firmware_smoke.py --build --strict-artifacts
```

本地验证只能确认源码、构建、合包和 manifest；PaperS3 触摸方向、EPD 实际刷新、字体观感仍必须真机确认。

## SD 卡约定

```text
SD Card/
├── books/              # 放入 .txt 电子书
├── fonts/              # 可选外部 .fnt 字体
├── progress/           # 阅读进度 / TOC / page cache 自动生成
└── ebook_config.json   # 全局设置，后续功能使用
```

内置 UI 字体优先从 SPIFFS full 镜像中的 `data/fonts/` 加载，避免依赖 SD 卡。

## 发布清单规则

- `releases.json` 顶部版本必须是最新用户可烧录版本。
- 新版本默认只维护 `assets.full`。
- `assets.full.size` 必须是 `16777216`。
- `assets.full.flashOffset` 必须是 `0`。
- 不再维护旧的 `oxflash.json` 兼容清单，避免重复数据源。
