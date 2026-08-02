# OrangePi Vision 交接记录

## V2 完成状态（2026-08-01）

- 已实测：板端 H.264 + Opus → 加密 SRT → 云端 MediaMTX → Nginx `/vision/` → 公网 WebRTC；局域网 WebRTC 与 RTSP 同时正常。
- 已修复：SRT FIFO 参数层级、启动阶段音频队列抖动。
- FRP：`frpc`、`frps` 已关闭且禁止开机启动，程序和配置保留，暂不卸载。
- Git 发布注意：`config/mediamtx.yml.ToBeUploadToOnlineServer` 与 `config/nginx.conf.ToBeUploadToOnlineServer` 含真实部署信息，不得提交。

## 任务目标

板端将 H.264 与 `config/stream.opus` 合流，通过加密 SRT 推往云端 MediaMTX，并由云端 WebRTC 对外播放；保留 FRP，待公网验收完成后再单独卸载。

## 已完成

- `vision-common.sh`：固定 Opus 音频；FFmpeg 单次合流，`tee + fifo` 同时输出本地 UDP 与 SRT；FIFO 参数已修正为显式 `f=fifo`，远端队列为 512 包，云端不可用时每 5 秒恢复并丢弃远端积压包。
- `vision`：Camera/YOLO 统一加载 `vision.env`，校验五项 SRT 配置与 Opus 规格，状态不虚报 SRT 已连接。
- `config/mediamtx.yml`：板端 HLS 已关闭，保留 RTSP/WebRTC。
- 新增 `config/stream.opus`，已验证为 48 kHz、双声道 Opus。
- 部署方案位于 `docs/SRT公网部署方案.md`；中英文 README 已同步。
- 已准备云端 `nginx.conf`：`/vision/` 为 WebRTC 静态播放页，`/vision/whep` 反代本机 MediaMTX，`/vision-hls/` 保留 FRP HLS 回退；未启用观看登录。

## 主输出文件

`vision`、`vision-common.sh`、`config/vision.env.example`、`config/mediamtx.yml`、`config/stream.opus`、`docs/SRT公网部署方案.md`。

## 验证状态

- Git Bash 已通过 `bash -n vision` 与 `bash -n vision-common.sh`。
- SRT 配置静态测试：全空通过、部分配置拒绝、完整配置通过。
- FFmpeg 本机支持 SRT；Opus 文件探测通过。
- 已在真实板端与云端验证：SRT 发布成功，公网 WebRTC 与局域网 WebRTC 均可播放 H.264 + Opus。
- 尚未完成连续 15 分钟、云端中断自动恢复和音频连续性验收。

## 下一步

1. 按部署文档配置云端 MediaMTX、Nginx、UFW/安全组。
2. 在板端写入真实的五项 SRT 配置并做全链路与断链隔离测试。
3. 公网 WebRTC 稳定后，确认 FRP 无其他用途，再单独卸载 FRP。
