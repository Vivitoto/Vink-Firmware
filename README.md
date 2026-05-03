# Vink-Firmware

Vink 系列设备固件总仓库。

## 目录结构

```text
Vink-Firmware/
└── PaperS3/       # Vink-PaperS3：面向 M5Stack PaperS3 的电子书阅读固件
```

新增设备时按设备单独建目录，不把不同设备源码、资源、构建脚本混在仓库根目录。

## 当前固件

- `PaperS3/`：Vink-PaperS3，M5Stack PaperS3 电子书阅读固件。

## 设备发布清单

每个设备目录维护自己的发布清单，App 读取对应设备目录下的清单：

```text
PaperS3/releases.json
```

GitHub Release 资产仍由仓库 Release 承载，但设备版本、更新说明、下载地址、默认烧录资产都归档在对应设备目录中。后续新增设备时，新增独立目录和独立 `releases.json`。

`PaperS3/oxflash.json` 旧兼容清单已移除；Vink Flasher 当前只读取 `PaperS3/releases.json`，避免维护两个数据源。

## 发布资产命名

当前用户侧只发布完整首刷镜像：

```text
Vink-PaperS3-vX.Y.Z-full-16MB.bin
```

要求：

- 从 flash offset `0x0` 烧录。
- 文件大小为 `16777216` bytes。
- bootloader、分区表、app、SPIFFS 资源都合入完整包。
- OTA/app-only/SPIFFS 单包只作为内部构建中间产物，不作为新版本用户下载资产。
