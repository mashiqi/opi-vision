# SRT 公网 WebRTC 部署

> **实测状态（2026-08-01）**：已完成板端 SRT 发布、云端 MediaMTX 接收、Nginx WHEP 反代，以及公网/局域网 WebRTC 播放验证。FRP 已关闭但未删除；`/vision-hls/` 仅保留为未来回退路径。

板端将 `config/stream.opus`（48 kHz、双声道）与 H.264 合流为 MPEG-TS。FFmpeg 只编码一次，并用 `tee` 同时发往本地 MediaMTX 和加密 SRT：

```text
OrangePi ──H.264 + Opus / MPEG-TS──┬── UDP 127.0.0.1:8891 ── 本地 MediaMTX（RTSP/WebRTC）
                                  └── SRT/AES UDP 8890 ── 云端 MediaMTX ── WebRTC ── 浏览器
```

SRT 分支使用 FFmpeg `fifo`：云端不可用时本地 UDP 不受影响，远端积压包会被丢弃，并每 5 秒自动重连。

当前实现还会将 H.264 输入队列提升至 1024 包，并将 SRT FIFO 队列设为 512 包，避免云端连接建立或恢复时影响本地 WebRTC 音频连续性。

## 1. 板端配置

```bash
cp config/vision.env.example config/vision.env
```

在 `config/vision.env` 填写以下五项；全部留空即不启用公网转发，填写任意一项则必须全填。

```bash
VISION_SRT_CLOUD_HOST=example.com
VISION_SRT_PASSPHRASE=replace_with_24_char_secret
VISION_SRT_USERNAME=replace_with_publish_user
VISION_SRT_PASSWORD=replace_with_publish_password
VISION_SRT_LATENCY_MS=2000
```

用户名、发布密码和 AES 密码均使用 16–32 位字母、数字、`_`、`-`，例如：

```bash
tr -dc 'A-Za-z0-9_-' </dev/urandom | head -c 24; echo
```

启动前脚本会检查 Opus 编码、48 kHz、双声道和 SRT 参数。`./vision --status` 仅显示“未配置”或“已启用（由推流进程转发）”；实际连接/重连错误看 `./vision --log`。

## 2. 云端 MediaMTX v1.19.3

从 `config/mediamtx.yml.ToBeUploadToOnlineServer.example` 复制配置到云端 `~/mediamtx/mediamtx.yml`。将示例中的占位符替换为与板端一致的值；不要提交真实文件。

```yaml
logLevel: info
rtsp: false
rtmp: false
hls: false
moq: false
api: false

srt: true
srtAddress: :8890

webrtc: true
webrtcAddress: 127.0.0.1:8889
webrtcLocalUDPAddress: :8189
webrtcAdditionalHosts: [example.com]

authMethod: internal
authInternalUsers:
  - user: board_publisher_001
    pass: replace_with_publish_password
    permissions:
      - action: publish
        path: vision
  - user: any
    pass:
    permissions:
      - action: read
        path: vision

paths:
  vision:
    srtPublishPassphrase: replace_with_24_char_secret
```

`webrtcAdditionalHosts` 只声明公网域名，避免向浏览器泄露私网候选地址。SRT 的 `streamid=publish:vision:<用户名>:<密码>` 由板端脚本生成。

创建 systemd 服务：

```ini
# /etc/systemd/system/mediamtx-cloud.service
[Unit]
Description=MediaMTX SRT to WebRTC
After=network-online.target
Wants=network-online.target

[Service]
User=<cloud-user>
WorkingDirectory=/home/<cloud-user>/mediamtx
ExecStart=/home/<cloud-user>/mediamtx/mediamtx /home/<cloud-user>/mediamtx/mediamtx.yml
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

将 `<cloud-user>` 改为实际登录用户，再执行：

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now mediamtx-cloud
sudo systemctl status mediamtx-cloud
```

## 3. Nginx 播放页与 WHEP

`/vision/` 是全浏览器窗口播放页；`/vision/whep` 仅反代本机 MediaMTX。保留原 FRP HLS 时，请迁移其公开地址至 `/vision-hls/`，不要与 `/vision/` 冲突。

```nginx
location = /vision/whep {
    proxy_pass http://127.0.0.1:8889/vision/whep;
    proxy_http_version 1.1;
    proxy_set_header Host $host;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    proxy_request_buffering off;
    proxy_read_timeout 3600;
}

location /vision/ {
    alias /var/www/vision/;
    index index.html;
    try_files $uri $uri/ =404;
}
```

创建 `/var/www/vision/index.html`：

```html
<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Vision</title>
<style>html,body,video{margin:0;width:100%;height:100%;background:#000}video{object-fit:contain}button{position:fixed;z-index:1;bottom:16px}#sound{left:16px}#full{right:16px}</style>
<video id="v" autoplay muted playsinline></video><button id="sound">点击开启声音</button><button id="full">全屏</button>
<script>
const v=document.querySelector('#v'), b=document.querySelector('#sound');
async function start(){const pc=new RTCPeerConnection();pc.addTransceiver('video',{direction:'recvonly'});pc.addTransceiver('audio',{direction:'recvonly'});pc.ontrack=e=>v.srcObject=e.streams[0];const o=await pc.createOffer();await pc.setLocalDescription(o);const a=await fetch('/vision/whep',{method:'POST',headers:{'Content-Type':'application/sdp'},body:o.sdp});if(!a.ok)throw Error(await a.text());await pc.setRemoteDescription({type:'answer',sdp:await a.text()});}
start().catch(e=>{console.error(e);setTimeout(()=>location.reload(),3000)});b.onclick=async()=>{v.muted=false;await v.play();b.remove()};
document.querySelector('#full').onclick=()=>v.requestFullscreen();
</script></html>
```

首次有声播放需要用户点击；“全屏”按钮进入浏览器全屏播放。

可选登录门禁：默认不启用，认证 include 目录保持为空。需要时生成 `htpasswd`，创建独立认证片段，并在两个 `location` 中加入 `include /etc/nginx/snippets/vision-auth.conf;`。认证片段只包含 `auth_basic` 与 `auth_basic_user_file`，无需改播放页或板端。

## 4. 防火墙与验收

云安全组和 UFW 仅放行 UDP `8890`（SRT）与 UDP `8189`（WebRTC ICE）；HTTP(S) 使用现有 TCP `443`。不要开放 `8554`、`8889`。例如：

```bash
sudo ufw allow 8890/udp
sudo ufw allow 8189/udp
sudo nginx -t && sudo systemctl reload nginx
```

验收顺序：

1. `./vision --start` 后本地 `http://<board-ip>:8889/vision` 有 H.264+Opus。
2. 云端确认监听 UDP 8890/8189，打开 `https://<你的域名>/vision/`，点击后有声音和视频。
3. 连续运行 15 分钟，跨过音频循环点检查同步。
4. 暂停云端 MediaMTX 或阻断 UDP 8890：本地 WebRTC 应持续播放；恢复后查看 `./vision --log` 的 SRT 自动恢复记录。

FRP/HLS 仅在公网 WebRTC 稳定验收后迁移或移除；确认 FRP 没有其他用途后，再单独停用 `frps`。

本次验收后，`frpc` 与 `frps` 已停止且禁止开机启动，但程序与配置仍保留；不应卸载，直到确认不再需要 HLS 回退或其他端口转发。

## 发布到 GitHub 前

- 只提交 `config/vision.env.example`，绝不提交真实 `config/vision.env`。
- `config/mediamtx.yml.ToBeUploadToOnlineServer` 可能含真实发布账户、SRT AES 密钥；**不要 `git add` 或提交它**。
- `config/nginx.conf.ToBeUploadToOnlineServer` 可能包含个人域名和证书路径；也不要提交。请使用对应的 `.example` 模板文件。
- 同样不要提交证书私钥、`log/`、`run/` 或其他云端私密配置。

## 5. 故障排查

- 板端启动拒绝：检查 `stream.opus` 是否存在且为 48 kHz、双声道 Opus。
- SRT 无法连通：核对五项配置、AES 密码、用户名/密码和云端 UDP 8890。
- 浏览器无画面：检查 UDP 8189 安全组/UFW、域名 DNS 和 `webrtcAdditionalHosts`。
- 本地画面异常：先查看 `./vision --log`；SRT 故障不应影响本地 UDP/WebRTC。
