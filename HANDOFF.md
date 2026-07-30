# OrangePi Vision 发布交接

## 已验证架构

```text
IMX219 → ISP/VIN → Camera 或 YOLO26s + ByteTrack → H.264 + AAC → MediaMTX HLS
MediaMTX HLS → FRP → Nginx HTTPS → https://<your-personal-website>/vision/
```

- MediaMTX 提供 RTSP `8554`、HLS `8888` 与 WebRTC `8889`。
- 公网仅经 FRP 映射板端 `127.0.0.1:8888` 到云端 `127.0.0.1:18888`，由 Nginx 提供 HTTPS。
- HLS 为 MPEG-TS、2 秒分片、保留 7 段；音频为预编码 AAC-LC、48 kHz、双声道。

## 运维状态

- 板端与公网 HLS 已验证可读取 H.264 + AAC。
- 硬件编码依赖 Cedarc 运行时；安装、验证和重建步骤见 [Cedarc 文档](src/src-cedarc/vendor/CEDARC_HARDWARE_ENCODING.md)。
- 启动、状态、日志、停止依次使用 `./vision --start`、`./vision --status`、`./vision --log`、`./vision --stop`。

## 发布前检查

- 保留 `bin/`、`yolo-models/`、`config/stream.mp3`、`config/stream.m4a` 和 `docs/` 中的 PDF。
- 仅提交 `config/vision.env.example`，不得提交 `config/vision.env` 或证书私钥。
- 许可证、第三方归属和背景音乐的再分发声明集中在根目录 [LICENCES](LICENCES)。
