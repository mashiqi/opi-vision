# AGENTS.md

## 项目

OrangePi Zero 3W + IMX219 视觉推流。Shell 脚本驱动，硬编码 H.264 + Opus 音频 → UDP → MediaMTX → WebRTC/RTSP。

## 项目文档

主要部署和运维文档：`docs/部署与运维指南.md`。

V1 FRP+HLS 归档：`docs/V1_FRP_HLS_部署方案.md`。

## 硬件约束

- SoC: Allwinner A733, LPDDR5，无有线网口，有 Wi-Fi 6（见 `docs/部署与运维指南.md`）
- 无显示器/无音频输出，仅 SSH 操作
- 不可在板端运行 ffplay、aplay 等本地播放/录制工具
- 所有播放验证在用户 PC 上进行（浏览器 WebRTC 或 VLC RTSP）

## 操作约束

- 与 isp3a-daemon 交互时，其源码在 `src/src-camera/isp3a-daemon.c`（闭源 AWIspApi 库的简单封装）
- 修改板端文件前先备份
- 每轮测试后考虑是否需 `./vision --stop` 清理
