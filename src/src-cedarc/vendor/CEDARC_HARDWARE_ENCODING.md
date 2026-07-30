# Cedarc/VE2硬编码运行时

本目录保存 `orangepi-vision` 使用的Cedarc开发文件、原始归档和系统安装工具。
这些材料来自瑞莎A733系统镜像，并已获得提取与分发许可。

## 设计原则

- `orangepi-vision` 不依赖外部的 `vpu-development-kit`。
- Cedarc动态库安装到系统标准目录 `/usr/lib/aarch64-linux-gnu`。
- Vision运行时不设置Cedarc专用的 `LD_LIBRARY_PATH`，避免与FFmpeg、GStreamer
  或YOLO进程加载的其他库发生混用。
- 项目内保存可复现的头文件、rootfs副本、原始归档、清单与SHA256。
- 硬编码不可用时不会自动退回软编码；用户必须显式选择
  `--encoder software`。

## 目录

```text
src/src-cedarc/
├── aw-h264-encoder.c
├── Makefile
└── vendor/
    ├── CEDARC_HARDWARE_ENCODING.md
    ├── include/
    ├── rootfs/
    ├── archive/
    └── scripts/
```

`include/` 保存构建编码器所需的12个Cedarc头文件。

`rootfs/` 镜像保存瑞莎运行时的目标路径，例如：

```text
rootfs/etc/cedarc.conf
rootfs/etc/udev/rules.d/99-zz-cedar-ve.rules
rootfs/usr/bin/demoVencoder
rootfs/usr/lib/aarch64-linux-gnu/libvencoder.so
```

`archive/` 保存未经改写的 `vpu-userspace.tar.gz`、manifest和SHA256。

## 安装与验证

在香橙派板端执行：

```bash
cd ~/orangepi-vision/src/src-cedarc/vendor
sudo ./scripts/install-runtime.sh
./scripts/verify-runtime.sh
```

安装脚本将项目中的rootfs副本复制到系统标准路径，然后运行 `ldconfig` 并
刷新udev规则。执行前会检查架构和关键文件。

编译项目内编码器：

```bash
cd ~/orangepi-vision/src/src-cedarc
make
../bin/aw-h264-encoder --version
```

## Vision用法

默认使用硬编码：

```bash
cd ~/orangepi-vision
./vision
```

显式选择软编码：

```bash
./vision --start --encoder software
```

旋转参数按顺时针角度表示：

```bash
./vision --start --rotate 0
./vision --start --rotate 90
./vision --start --rotate 180
./vision --start --rotate 270
```

0°和180°已有历史实测基础；新增的90°和270°必须完成板端验收后才能标记为
稳定支持。当前不提供水平或垂直镜像。

## 必需运行库

H.264编码链路至少依赖：

```text
libcdc_base.so
libMemAdapter.so
libVE.so
libvenc_base.so
libvenc_codec.so
libvenc_common.so
libvenc_h264.so
libvencoder.so
```

完整归档还包括解码器、H.265、JPEG、OMX和诊断程序，方便以后扩展；Vision
当前只调用项目内的 `aw-h264-encoder`。

## 系统要求

- AArch64；
- 兼容的A733/sun60iw2 Cedar驱动；
- `/dev/cedar_dev_ve2` 和 `/dev/dma_heap/system`；
- 当前用户具有设备读写权限，通常需要属于 `video` 组；
- FFmpeg包含 `h264_metadata` 与 `setts` 位流过滤器。
