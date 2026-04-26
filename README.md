# Vink-PaperS3

Vink 项目中面向 M5Stack PaperS3 的电子书阅读固件，支持 TXT 格式、自定义灰度字库和触摸屏翻页。

**默认字体:霞鹜文楷 (LXGW WenKai)** -- 一款开源可商用的优雅中文字体。

## 功能特性

- **TXT 阅读**:支持 UTF-8 / GBK 编码的 TXT 文件（GBK 自动转 UTF-8）
- **点阵字库**:自定义二进制字库,支持任意 TTF 字体转换
- **灰度抗锯齿**:4bpp 灰度字库(16级灰度),显示效果接近印刷体
- **默认字体**:霞鹜文楷 (LXGW WenKai),开源可商用
- **智能分页**:自动排版、分页、进度保存
- **触摸操作**:点击/滑动翻页,长按菜单
- **文件浏览**:SD 卡文件列表,自动扫描 `/books/` 目录
- **自定义排版**：字号、行间距、段间距、页边距、首行缩进、两端对齐，全部可调
- **三档刷新**:低/中/高三档全刷频率，平衡残影与速度
- **预读加速**:后台预读下一页到 PSRAM，减少 SD 卡延迟
- **章节识别**:自动识别章节标题，支持缓存
- **电量显示**:状态栏实时显示电量
- **休眠省电**:可配置时长，触摸唤醒
- **WiFi 传书**:局域网 HTTP 上传，手机/电脑直接传 txt
- **字体切换**:多字体文件支持，菜单一键切换
- **阅读统计**:累计时长、翻页数、每日统计
- **蓝牙翻页**:BLE 遥控器支持
- **内存优化**:利用 ESP32-S3 的 8MB PSRAM 存储分页表和索引

## 项目结构

```
Vink-PaperS3/
├── platformio.ini          # PlatformIO 配置
├── src/
│   ├── main.cpp            # 程序入口
│   ├── Config.h            # 配置常量
│   ├── App.h/cpp           # 应用状态机和主循环
│   ├── FontManager.h/cpp   # 点阵字库加载与渲染
│   ├── EbookReader.h/cpp   # 阅读器核心(分页、渲染、进度、书签)
│   ├── FileBrowser.h/cpp   # SD 卡文件浏览器
│   ├── ChapterDetector.h/cpp # 章节自动识别
│   ├── TextCodec.h/cpp     # 编码检测(UTF-8/GBK)
│   ├── WiFiUploader.h/cpp  # WiFi 传书 HTTP 服务器
│   ├── ReadingStats.h/cpp   # 阅读统计
│   ├── BlePageTurner.h/cpp # 蓝牙翻页器
│   └── GBKTable.h          # GBK→Unicode 转换表
    └── generate_font.py    # 字库生成工具(TTF → .fnt)
```

## 快速开始

### 1. 准备 SD 卡

在 SD 卡根目录创建以下结构:

```
SD Card/
├── books/              # 放入 .txt 电子书
│   ├── 三体.txt
│   └── 明朝那些事.txt
├── fonts/              # 放入生成的字库文件
│   ├── wenkai24_gray.fnt   # 24px 霞鹜文楷灰度字库(默认)
│   ├── wenkai16_gray.fnt   # 16px 霞鹜文楷灰度字库(小号)
│   └── simsun24_gray.fnt   # 其他字体可选
├── progress/           # 阅读进度自动保存(自动创建)
└── ebook_config.json   # 全局设置(自动创建)
```

## 生成字库(默认:霞鹜文楷)

本固件默认使用 **霞鹜文楷 (LXGW WenKai)** 作为阅读字体。这是一款开源可商用的中文字体,显示效果优雅。

### 1. 安装依赖

```bash
cd tools
pip install freetype-py numpy
```

### 2. 下载字体

从 [LXGW WenKai GitHub Releases](https://github.com/lxgw/LxgwWenKai/releases) 下载 `LXGWWenKai-Regular.ttf`。

### 3. 生成灰度字库

```bash
# 生成 24px 灰度字库(默认阅读字号)
python generate_gray_font.py \
  --input LXGWWenKai-Regular.ttf \
  --output ../sd_card/fonts/wenkai24_gray.fnt \
  --size 24 \
  --chars common_chinese.txt

# 生成 16px 灰度字库(小号)
python generate_gray_font.py \
  --input LXGWWenKai-Regular.ttf \
  --output ../sd_card/fonts/wenkai16_gray.fnt \
  --size 16 \
  --chars common_chinese.txt
```

**字符列表文件**(`common_chinese.txt`)示例:

```
# 常用汉字 3500
的一是在不了有和人这中大为上个国我以要他时来用们生到作地于出就分对成
可主发年动同工也能下过子说产种面而方后多定行学法所民得经十三之进着
...
# 英文和数字
abcdefghijklmnopqrstuvwxyz
ABCDEFGHIJKLMNOPQRSTUVWXYZ
0123456789
# 标点
,。!?、;:""''()《》【】-...·
```

> **注意**:如果不提供字符列表,默认只生成少量常用字。完整中文支持建议提供 3500+ 常用汉字。

### 4. 使用其他字体

你可以使用任意 TTF/OTF 字体:

```bash
python generate_gray_font.py \
  --input /path/to/your/font.ttf \
  --output ../sd_card/fonts/myfont24_gray.fnt \
  --size 24 \
  --chars common_chinese.txt
```

然后在 `Config.h` 中修改默认字库路径:
```cpp
#define FONT_FILE_24 "/fonts/myfont24_gray.fnt"
```

## 字库格式说明

本固件支持两种字库格式:

### 1. 4bpp 灰度字库(推荐)

```
[Header] 16 bytes
  magic[4]      = "GRAY"
  version       = 1
  fontSize      = 字号像素高度
  charCount     = 字符数量
  bitmapSize    = 位图数据总大小

[Index Table] charCount * 16 bytes
  unicode       = Unicode codepoint (4 bytes)
  offset        = 位图数据偏移 (4 bytes)
  width         = 字符宽度 (1 byte)
  height        = 字符高度 (1 byte)
  bearingX      = 水平偏移 (1 byte, signed)
  bearingY      = 垂直偏移 (1 byte, signed)
  advance       = 水平步进 (1 byte)
  reserved      = 3 bytes

[Bitmap Data] 变长
  4bpp 灰度数据,每字符 ((width+1)/2) * height bytes
  每字节存储 2 个像素(高4位 + 低4位)
```

**优点**:显示效果极佳,抗锯齿,接近印刷体
**体积**:约 1bpp 黑白字库的 4 倍(24px/3500字 ≈ 18MB)

### 2. 1bpp 黑白字库(兼容旧格式)

```
[Header] 12 bytes
  magic[4]      = "FNT\0"
  ...
```

**优点**:体积小
**缺点**:有锯齿,显示效果一般

> 固件自动检测字库格式,无需额外配置。

### 3. 编译刷入

```bash
cd Vink/firmware/Vink-PaperS3
pio run --target upload
```

## 触摸操作

| 区域 | 动作 | 功能 |
|------|------|------|
| 左侧 1/4 | 点击/右滑 | 上一页 |
| 右侧 1/4 | 点击/左滑 | 下一页 |
| 中间 1/2 | 点击/长按 | 打开菜单 |
| 书架列表 | 上下滑动 | 选择书籍 |
| 书架右侧 | 点击 | 打开选中书籍 |

## 阅读菜单（14项）

点击中间区域或长按打开菜单：

| 菜单项 | 功能 |
|--------|------|
| 继续阅读 | 返回阅读 |
| 排版设置 | 字号/行距/边距等 9 项参数调整 |
| 章节目录 | 自动识别章节，点击跳转 |
| 残影控制 | 低/中/高三档刷新频率 |
| 添加书签 | 标记当前位置 |
| 我的书签 | 查看/跳转书签 |
| **字体切换** | 选择 /fonts/*.fnt 字体 |
| **WiFi 传书** | 开启 HTTP 服务器，浏览器上传 |
| **阅读统计** | 累计时长/翻页/今日统计 |
| **蓝牙翻页** | 开启 BLE 等待手机连接 |
| 字号调大 | 快捷 +2px |
| 字号调小 | 快捷 -2px |
| 返回书架 | 关闭当前书 |
| 关闭设备 | 关机 |

## 核心架构

### 字库格式 (.fnt)

二进制格式,结构紧凑:

```
[Header] 12 bytes
  magic[4]      = "FNT\0"
  version       = 1
  fontSize      = 字号像素高度
  charCount     = 字符数量

[Index Table] charCount * 10 bytes
  unicode       = Unicode codepoint (4 bytes)
  offset        = 位图数据偏移 (4 bytes)
  width         = 字符宽度 (1 byte)
  height        = 字符高度 (1 byte)

[Bitmap Data] 变长
  1bpp 点阵数据,每字符 ((width+7)/8) * height bytes
```

### 分页算法

1. 首次打开书籍时,逐字符扫描文件
2. 根据字库中的字符宽度计算行宽
3. 行满或遇到换行符时换行
4. 页满(达到屏幕可显示行数)时记录分页点
5. 分页表存储在 PSRAM 中,加速翻页

### 内存使用

| 模块 | 存储位置 | 估算大小 |
|------|---------|---------|
| 灰度字库索引 | PSRAM | ~3500字符 × 16B = 56KB |
| 分页表 | PSRAM | ~1000页 × 8B = 8KB |
| 屏幕缓冲区 | 内部 RAM | 960×540 / 2 = 256KB (4bpp) |
| 运行时堆 | 内部 RAM | ~50KB |

## 已知限制

1. **只支持 TXT**:EPUB/PDF 需要 ZIP + HTML 解析器,后续可扩展
2. **字库需预生成**:无法在设备上实时渲染 TTF
3. **SD 卡依赖**:所有资源(书籍、字体、进度)都存储在 SD 卡
4. **WiFi 传书需配置**:首次使用需设置 WiFi SSID/密码(后续支持)
5. **蓝牙翻页器兼容性**:需使用支持自定义指令的 BLE 遥控器或手机 App

## 扩展建议

- **EPUB 支持**:集成 miniz (ZIP) + tinyxml2 解析 HTML
- **TTF 实时渲染**:集成 FreeType(较慢,但无需预生成字库)
- **WiFi 传书**:✅ 已完成
- **多字体切换**:✅ 已完成
- **阅读统计**:✅ 已完成
- **蓝牙翻页**:✅ 已完成
- **笔记批注**:在特定偏移记录用户笔记

## 技术栈

- **平台**:ESP32-S3 (M5Stack Paper S3)
- **框架**:Arduino (PlatformIO)
- **显示**:M5Unified / M5GFX(E-ink 驱动抽象)
- **字库**:自定义二进制点阵格式
- **文件系统**:SD + FATFS
- **配置**:ArduinoJson

## License

MIT
