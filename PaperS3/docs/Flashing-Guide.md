# Vink-PaperS3 固件烧录指南

## 准备工作

### 1. 安装 PlatformIO

```bash
# 安装 PlatformIO Core
pip install platformio

# 或者安装 VS Code + PlatformIO IDE 插件（推荐）
```

### 2. 连接设备

1. 用 USB-C 线将 PaperS3 连接到电脑
2. **进入下载模式**：**长按电源键**，直到背部状态灯**闪红光**
3. 电脑应识别到串口设备（Linux: `/dev/ttyACM0` 或 `/dev/ttyUSB0`）

### 3. 项目目录

```bash
cd Vink/firmware/Vink-Firmware/PaperS3
```

## 编译烧录

### 方式一：命令行（PlatformIO Core）

```bash
# 编译
pio run

# 编译并烧录（自动进入下载模式后执行）
pio run --target upload

# 查看串口日志
pio device monitor
```

### 方式二：VS Code + PlatformIO IDE

1. 打开 VS Code，安装 PlatformIO IDE 插件
2. 打开 `Vink/firmware/Vink-Firmware/PaperS3` 文件夹
3. 点击左侧 PlatformIO 图标 → `Project Tasks` → `m5papers3` → `Upload`

## 首次烧录后的操作

### SD 卡准备

在 SD 卡根目录创建：

```
SD Card/
├── books/              # 放入 .txt 电子书
│   └── 你的书.txt
├── fonts/              # 放入字库文件
│   └── wenkai24_gray.fnt
└── progress/           # 自动创建，存放阅读进度
```

### 字库生成

```bash
cd tools
pip install freetype-py numpy

# 下载霞鹜文楷字体
# https://github.com/lxgw/LxgwWenKai/releases

# 生成字库
python generate_gray_font.py \
  --input LXGWWenKai-Regular.ttf \
  --output ../sd_card/fonts/wenkai24_gray.fnt \
  --size 24
```

## 硬件操作

| 操作 | 方式 |
|------|------|
| **开机** | 单击侧边按钮 |
| **关机** | 双击侧边按钮 |
| **下载模式** | 长按电源键，背部红灯闪烁 |

## 常见问题

### Q: 电脑识别不到设备
A: 检查是否进入下载模式（背部红灯闪烁）。Linux 可能需要添加 udev 规则。

### Q: 烧录失败/超时
A: 重新进入下载模式再试。确保 USB 线支持数据传输。

### Q: 屏幕不刷新
A: PaperS3 使用并行 EPD 驱动，首次刷入可能需要完整刷新一次。

### Q: 触摸没反应
A: GT911 触摸芯片初始化需要一点时间，重启试试。
