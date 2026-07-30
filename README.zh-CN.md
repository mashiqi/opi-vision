# OrangePi Vision

**面向 OrangePi ZERO 3W 与 IMX219 的一体化边缘视觉服务。** 本项目将相机采集、YOLO26s + ByteTrack 追踪、A733 硬件加速和多协议直播整理为可部署的 ARM64 仓库。

[快速开始](#快速开始) · [系统架构](#系统架构) · [文档](#文档) · [English](README.md)

## 功能亮点

- **双模式运行：** 轻量 Camera 直播，或 YOLO26s + ByteTrack 实时检测与追踪。
- **A733 硬件加速：** 使用 NPU 推理，默认采用 Cedarc/VE2 H.264 硬件编码。
- **一次输入，三种输出：** 同一份编码输入提供 RTSP、HLS 和 WebRTC；不会因不同协议重复视频编码。
- **部署资产齐全：** 仓库包含 ARM64 程序、NPU 模型、背景音频、Cedarc 运行时材料和硬件 PDF。
- **画面参数可调：** 支持 640×360、1280×720、1920×1080，1–30 FPS，以及 0°/90°/180°/270° 旋转。

## 快速开始

准备 OrangePi ZERO 3W（A733/ARM64）、已可用的 IMX219 驱动，并将仓库放在 `~/orangepi-vision`：

```bash
cd ~/orangepi-vision
chmod +x vision vision-withyolo bin/* src/src-camera/build-board-tools.sh
./src/src-camera/build-board-tools.sh   # 仅首次部署需要
./vision --start                         # Camera 模式
```

使用 `./vision-withyolo --size 1280x720 --fps 20` 启动检测与追踪；使用 `./vision --status`、`./vision --log`、`./vision --stop` 查看或管理服务。硬件编码不会静默降级；需要时请显式指定 `--encoder software`。

## 访问与使用

| 协议 | 地址 | 常用场景 |
|---|---|---|
| WebRTC | `http://<board-ip>:8889/vision` | 浏览器低延迟预览 |
| HLS | `http://<board-ip>:8888/vision` | 浏览器与播放器兼容访问 |
| RTSP | `rtsp://<board-ip>:8554/vision` | VLC、NVR 与 RTSP 客户端 |

```bash
./vision --start --size 1280x720 --fps 20 --encoder hardware --rotate 180
./vision --start --encoder software
```

改变模式、分辨率、帧率、编码器或旋转角度前，应先停止服务。

## 系统架构

```text
IMX219 → ISP/VIN（NV12）→ Camera 或 YOLO26s + ByteTrack → H.264 + AAC → MediaMTX
                                                                       ├─ RTSP
                                                                       ├─ HLS → FRP → Nginx HTTPS → 浏览器
                                                                       └─ WebRTC
```

HLS 使用 MPEG-TS、2 秒分片并保留 7 个分片，以连续播放为优先。

## 配置

仅在需要 Dashboard 或 LLM 事件功能时创建本地配置：

```bash
cp config/vision.env.example config/vision.env
```

请将密钥保存在 `config/vision.env`，该文件已被 Git 忽略。运行时音频为 `config/stream.m4a`（AAC-LC、48 kHz、双声道）；替换音频前请保留源文件 `stream.mp3` 并确认再分发权。

## 项目结构

```text
bin/          ARM64 MediaMTX、YOLO 和编码工具
config/       推流配置、示例环境变量和音频
docs/         硬件 PDF 与公网 HLS 部署指南
lib/          A733 NPU 运行库
src/          camera、Cedarc、LLM 与 YOLO26 源码
yolo-models/  A733 NPU 模型
vision*       服务控制入口
```

## 文档

- [公网 HLS、FRP、Nginx 与 HTTPS](docs/VISION_FRP_NGINX_HTTPS部署指南.md)
- [YOLO26 交叉编译与板端部署](src/src-yolo26/README.md)
- [Cedarc 运行时与硬件编码](src/src-cedarc/vendor/CEDARC_HARDWARE_ENCODING.md)
- [模型说明](yolo-models/README.md) · [运维交接](HANDOFF.md)

Camera 工具在板端执行 `./src/src-camera/build-board-tools.sh` 构建。YOLO26 需要在 x86_64 Ubuntu 的 Allwinner Model Zoo 环境中使用 `../build_linux.sh -t a733 -s debian11` 交叉编译；完整步骤见 YOLO 文档。

## 贡献与许可证

欢迎提交 Issue 和 Pull Request。请勿提交 `config/vision.env`、证书、日志或运行文件；修改第三方或媒体资产时，请同步更新 [LICENCES](LICENCES)。

这是一个自包含部署仓库，因此 MediaMTX、ARM64 程序、NPU 模型、音频和硬件 PDF 等大文件会被保留。项目、第三方、字体与媒体声明统一收录在 [LICENCES](LICENCES)，不另设 `LICENSE` 文件。

