# V2 WebRTC 部署与验收记录

日期：2026-08-01

## 已验证架构

```text
IMX219 → H.264 + Opus / MPEG-TS
       ├─ 本地 MediaMTX → RTSP、局域网 WebRTC
       └─ 加密 SRT → 云端 MediaMTX → Nginx WHEP → 公网 WebRTC
```

公网地址格式：`https://<你的域名>/vision/`

## 实测结果

- 板端 Camera 硬件编码：1280×720、30 FPS、H.264。
- 背景音频：`stream.opus`，48 kHz、双声道 Opus。
- 板端 MediaMTX：局域网 WebRTC 与 RTSP 可用。
- 云端 MediaMTX v1.19.3：接收 SRT UDP 8890，提供本机 WHEP 与 UDP 8189 ICE。
- Nginx：`/vision/` 为无登录播放页，`/vision/whep` 反代本机 MediaMTX。
- 公网与局域网 WebRTC 均已验证可播放音视频。

## 可靠性处理

- FFmpeg 单次合流，通过 `tee` 同时输出本地 UDP 与 SRT。
- SRT 使用独立 FIFO：队列 512 包，满队列丢弃远端积压包；任意错误每 5 秒恢复；恢复后等待关键帧。
- H.264 输入队列设为 1024，消除启动阶段的音频断续。

## FRP 与 HLS

- 板端 HLS 已关闭。
- `/vision-hls/` Nginx 路由保留为未来 HLS 回退入口。
- `frpc` 与 `frps` 已关闭且禁止开机启动，但尚未卸载；确认没有其他转发用途后再删除。

## 日常检查

```bash
# 板端
./vision --status
./vision --log

# 云端
sudo systemctl status mediamtx-cloud
sudo journalctl -u mediamtx-cloud -n 50 --no-pager
sudo nginx -t
```

## GitHub 发布清单

- 可提交：脚本、`config/vision.env.example`、`config/stream.opus`、文档。
- 不可提交：`config/vision.env`、`config/mediamtx.yml.ToBeUploadToOnlineServer`、`config/nginx.conf.ToBeUploadToOnlineServer`、证书私钥、日志、运行状态文件。
- 可复用云端配置：使用对应的 `.example` 文件，替换占位符后将真实文件保存为不带 `.example` 的部署文件。
