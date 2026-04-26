# Vink-Firmware

Vink 系列设备固件仓库。

## 目录结构

```text
Vink-Firmware/
└── PaperS3/       # Vink-PaperS3：面向 M5Stack PaperS3 的电子书阅读固件
```

## 当前固件

- `PaperS3/`：Vink-PaperS3，M5Stack PaperS3 电子书阅读固件。

## 设备发布清单

每个设备目录维护自己的发布清单，App 只读取对应设备目录下的清单：

```text
PaperS3/releases.json
```

GitHub Release 资产仍由仓库 Release 承载，但设备版本、更新说明、下载地址、默认烧录资产都归档在对应设备目录中。后续新增设备时，新增独立目录和独立 `releases.json`。

## 发布资产命名

固件 Release 资产使用以下格式：

```text
Vink-PaperS3-vX.Y.Z-full-16MB.bin
Vink-PaperS3-vX.Y.Z-ota.bin
Vink-PaperS3-vX.Y.Z-spiffs.bin
```

其中 `full-16MB` 是 Vink Flasher 默认烧录的完整镜像。
