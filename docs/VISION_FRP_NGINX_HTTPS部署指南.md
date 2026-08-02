# 香橙派 HLS 通过 FRP、Nginx 和 HTTPS 公网访问

> **旧方案提示（2026-08-01）**：V2 已切换至 SRT → 云端 MediaMTX → WebRTC，`frpc` 和 `frps` 当前已关闭但未卸载，板端 HLS 也已关闭。本文件仅保留作 HLS 回退与 FRP 运维参考；当前公网播放请使用 `/vision/`，详见 [V2 WebRTC 部署与验收](V2_WebRTC部署与验收.md)。

本文说明如何在香橙派执行：

```bash
cd ~/orangepi-vision
./vision --start
```

之后，通过以下地址观看视频：

```text
https://<your-personal-website>/vision/
```

当前 MediaMTX 输出为 MPEG-TS HLS、2 秒分片，优先保证连续播放。

本方案使用 HLS，不向公网直接暴露 MediaMTX、RTSP 或 WebRTC 端口。

## 一、方案设计

### 1.1 完整访问链路

```text
浏览器
  |
  | HTTPS，TCP 443
  v
阿里云 Nginx（在这里处理 <your-personal-website> 的 SSL）
  |
  | HTTP，仅服务器本机访问
  v
127.0.0.1:18888（frps 创建的本地映射端口）
  |
  | FRP TLS 隧道，控制连接使用 TCP 7000
  v
香橙派 frpc
  |
  | HTTP，仅香橙派本机访问
  v
127.0.0.1:8888/vision（MediaMTX HLS）
```

香橙派项目当前的 MediaMTX 配置为：

```text
HLS：http://板卡IP:8888/vision
RTSP：rtsp://板卡IP:8554/vision
WebRTC：http://板卡IP:8889/vision
```

本方案只转发 HLS 的 `8888/TCP`。HLS 基于 HTTP，最容易通过 Nginx 和现有 HTTPS 域名稳定提供服务。

### 1.2 端口规划

| 端口 | 所在设备 | 用途 | 是否对公网开放 |
|---|---|---|---|
| `443/TCP` | 阿里云服务器 | Nginx HTTPS 入口 | 是 |
| `80/TCP` | 阿里云服务器 | HTTP 跳转 HTTPS（如现有配置需要） | 是 |
| `7000/TCP` | 阿里云服务器 | frpc 连接 frps | 是，但可限制来源 IP |
| `18888/TCP` | 阿里云服务器 | FRP 映射后的 HLS 端口 | 否，只绑定 `127.0.0.1` |
| `8888/TCP` | 香橙派 | MediaMTX HLS | 否，只通过 FRP 使用 |

不需要转发以下端口：

- `8554/TCP`：RTSP。
- `8889/TCP`：WebRTC HTTP 信令。
- `8189/UDP`：WebRTC 媒体端口。

### 1.3 SSL 的处理位置

现有 `<your-personal-website>` SSL 证书继续由 Nginx 使用，不需要把证书复制到香橙派，也不需要让 frps 监听 `443`。

链路的加密边界如下：

1. 浏览器到阿里云 Nginx：使用现有域名证书和 HTTPS。
2. Nginx 到 `127.0.0.1:18888`：服务器内部回环 HTTP，不经过公网。
3. frpc 到 frps：启用 FRP TLS，保护穿透隧道。
4. 香橙派 frpc 到 `127.0.0.1:8888`：板卡内部回环 HTTP。

因此，公网用户始终访问：

```text
https://<your-personal-website>/vision/
```

不应直接访问 `http://服务器IP:18888`。

## 二、准备参数

准备以下两个值：

```text
SERVER_IP    阿里云服务器公网 IPv4
FRP_TOKEN    frpc 和 frps 共用的随机令牌
```

在阿里云服务器生成令牌：

```bash
openssl rand -hex 32
```

不要把真实 token 写入公开文档、聊天记录或截图。

frpc 与 frps 应使用相同版本。本文配置按项目现有指南使用的 FRP `v0.70.0` 编写。

## 三、配置阿里云 frps

### 3.1 创建 token 文件

在阿里云服务器执行：

```bash
sudo install -d -m 755 /etc/frp
sudo nano /etc/frp/server_token
```

文件中只写入 `FRP_TOKEN` 的真实值，然后设置权限：

```bash
sudo chmod 600 /etc/frp/server_token
```

### 3.2 配置 `/etc/frp/frps.toml`

```toml
bindAddr = "0.0.0.0"
bindPort = 7000

# frpc 创建的远程代理端口只绑定服务器回环地址。
# 因此 18888 只能被服务器本机的 Nginx 访问。
proxyBindAddr = "127.0.0.1"

auth.tokenSource.type = "file"
auth.tokenSource.file.path = "/etc/frp/server_token"

# 强制 frpc 使用 TLS 连接。
transport.tls.force = true
```

本方案不使用 FRP 的 `vhostHTTPPort` 或 `vhostHTTPSPort`。服务器的 `80/443` 继续完全由现有 Nginx 管理，避免端口冲突。

### 3.3 校验并启动 frps

```bash
sudo /usr/local/bin/frps verify -c /etc/frp/frps.toml
sudo systemctl restart frps
sudo systemctl status frps --no-pager
```

确认 `7000` 正在监听：

```bash
sudo ss -lntp | grep ':7000'
```

### 3.4 阿里云安全组和服务器防火墙

阿里云安全组入方向至少允许：

```text
TCP 443    HTTPS
TCP 80     HTTP/跳转 HTTPS（如果现有网站使用）
TCP 7000   frpc 连接 frps
```

如果香橙派出口公网 IP 固定，建议把 `7000/TCP` 的来源限制为该 IP；否则暂时允许连接，并依赖长随机 token 和 FRP TLS 鉴权。

不要在安全组或 UFW 中开放 `18888`。由于 `proxyBindAddr` 设置为 `127.0.0.1`，该端口本身也不会监听公网地址。

## 四、配置香橙派 frpc

### 4.1 创建 token 文件

```bash
sudo install -d -m 755 /etc/frp
sudo nano /etc/frp/client_token
```

写入与服务器 `/etc/frp/server_token` 完全相同的 token，然后设置权限：

```bash
sudo chmod 600 /etc/frp/client_token
```

### 4.2 配置 `/etc/frp/frpc.toml`

把 `SERVER_IP` 替换为阿里云服务器真实公网 IPv4：

```toml
serverAddr = "SERVER_IP"
serverPort = 7000

auth.tokenSource.type = "file"
auth.tokenSource.file.path = "/etc/frp/client_token"

transport.tls.enable = true

[[proxies]]
name = "orangepi-vision-hls"
type = "tcp"
localIP = "127.0.0.1"
localPort = 8888
remotePort = 18888
```

这项代理的含义是：

```text
阿里云 127.0.0.1:18888
    -> FRP TLS 隧道
    -> 香橙派 127.0.0.1:8888
```

### 4.3 校验并启动 frpc

```bash
sudo /usr/local/bin/frpc verify -c /etc/frp/frpc.toml
sudo systemctl restart frpc
sudo systemctl status frpc --no-pager
```

实时查看日志：

```bash
sudo journalctl -u frpc -f
```

frpc 可以一直运行。即使 `vision` 尚未启动，FRP 隧道也可以保持在线；此时因为 `8888` 没有视频服务，访问 `/vision/` 会暂时失败。

## 五、配置 Nginx

### 5.1 找到现有 HTTPS server 块

在阿里云服务器查找 `<your-personal-website>` 当前使用的 Nginx 配置：

```bash
sudo nginx -T | grep -n -B 5 -A 20 'server_name <your-personal-website>'
```

应当已经存在类似配置：

```nginx
server {
    listen 443 ssl http2;
    server_name <your-personal-website>;

    ssl_certificate     /现有证书路径/fullchain.pem;
    ssl_certificate_key /现有证书路径/privkey.pem;

    # 现有网站配置……
}
```

不要删除或替换现有证书路径，也不要新建另一个同时监听相同域名和 `443` 的冲突 server 块。

### 5.2 添加 `/vision` 代理

在 `http { ... }` 内加入上游连接池：

```nginx
upstream vision_hls_backend {
    server 127.0.0.1:18888;
    keepalive 8;
}
```

再在现有 `<your-personal-website>` 的 HTTPS `server { ... }` 内加入：

```nginx
location = /vision {
    return 301 /vision/;
}

location ^~ /vision/ {
    proxy_pass http://vision_hls_backend;

    proxy_http_version 1.1;
    proxy_set_header Connection "";
    proxy_set_header Host $host;
    proxy_set_header X-Real-IP $remote_addr;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    proxy_set_header X-Forwarded-Proto $scheme;

    proxy_buffering off;
    proxy_request_buffering off;
    proxy_cache off;

    proxy_connect_timeout 10s;
    proxy_read_timeout 300s;
    proxy_send_timeout 300s;

    add_header Cache-Control "no-cache" always;
}
```

注意：

- `proxy_pass http://vision_hls_backend;` 后面不能添加 `/`。
- 浏览器请求 `/vision/index.m3u8` 时，MediaMTX 也必须收到 `/vision/index.m3u8`。
- 网站原有的 `location /` 和首页配置保持不变。
- `/vision` 会统一跳转到 `/vision/`，避免相对资源路径错误。

### 5.3 校验并重载 Nginx

```bash
sudo nginx -t
sudo systemctl reload nginx
```

只有 `nginx -t` 显示配置成功后才能重载。测试失败时不要重启 Nginx，应先修正配置。

## 六、启动视频并逐层验证

应从香橙派向公网逐层测试。哪一层第一次失败，问题通常就在该层或它的前一层。

### 6.1 香橙派启动视频

```bash
cd ~/orangepi-vision
./vision --start
./vision --status
```

测试 MediaMTX 本地 HLS：

```bash
curl -i http://127.0.0.1:8888/vision
curl -i http://127.0.0.1:8888/vision/index.m3u8
```

`/vision` 返回 `302` 并跳转到 `/vision/` 属于 MediaMTX 正常行为；`index.m3u8` 应返回 HLS 播放列表。

### 6.2 阿里云测试 FRP 映射

在阿里云服务器执行：

```bash
curl -i http://127.0.0.1:18888/vision/index.m3u8
```

如果香橙派本地访问正常，但这里失败，应检查：

```bash
sudo systemctl status frps --no-pager
sudo journalctl -u frps -n 100 --no-pager
sudo journalctl -u frpc -n 100 --no-pager
sudo ss -lntp | grep ':18888'
```

正常情况下，`18888` 应只显示为：

```text
127.0.0.1:18888
```

不应显示为：

```text
0.0.0.0:18888
```

### 6.3 阿里云测试 Nginx HTTPS

```bash
curl -I https://<your-personal-website>/vision/
curl -i https://<your-personal-website>/vision/index.m3u8
```

最后在浏览器打开：

```text
https://<your-personal-website>/vision/
```

## 七、常见问题

### 7.1 网站首页正常，但 `/vision/` 返回 502

502 表示 Nginx 无法访问 `127.0.0.1:18888`。依次检查：

```bash
sudo ss -lntp | grep ':18888'
sudo systemctl status frps --no-pager
sudo journalctl -u frps -n 100 --no-pager
```

并在香橙派检查：

```bash
sudo systemctl status frpc --no-pager
sudo journalctl -u frpc -n 100 --no-pager
```

### 7.2 FRP 正常，但没有视频

先确认 `vision` 已运行：

```bash
cd ~/orangepi-vision
./vision --status
curl -i http://127.0.0.1:8888/vision/index.m3u8
```

如果本地 `8888` 失败，问题在视频服务本身，不在 FRP 或 Nginx。

### 7.3 frpc 无法连接 frps

在香橙派测试：

```bash
nc -vz SERVER_IP 7000
```

同时检查：

- `SERVER_IP` 是否为阿里云真实公网 IPv4。
- 阿里云安全组是否允许 `7000/TCP`。
- 服务器防火墙是否允许 `7000/TCP`。
- 两端 token 是否完全一致。
- frpc 与 frps 版本是否一致。

可以比较 token 文件的哈希，而不显示 token 内容。

阿里云服务器：

```bash
sudo sha256sum /etc/frp/server_token
```

香橙派：

```bash
sudo sha256sum /etc/frp/client_token
```

两端 SHA256 应完全相同。

### 7.4 `/vision` 可以访问，但页面资源路径错误

确认同时存在：

```nginx
location = /vision {
    return 301 /vision/;
}
```

并确认 `proxy_pass` 末尾没有 `/`：

```nginx
proxy_pass http://127.0.0.1:18888;
```

### 7.5 SSL 证书错误

FRP 不负责公网证书。检查 Nginx 当前证书：

```bash
sudo nginx -T | grep -n -E 'server_name|ssl_certificate'
curl -Iv https://<your-personal-website>/
```

证书必须包含 `<your-personal-website>`，并由现有 Nginx HTTPS server 块加载。不要把证书配置到 frpc 或本方案的 MediaMTX 中。

## 八、日常使用

正常配置完成后，frps、frpc 和 Nginx 都可以常驻运行。

启动视频：

```bash
cd ~/orangepi-vision
./vision --start
```

停止视频：

```bash
cd ~/orangepi-vision
./vision --stop
```

浏览器地址始终为：

```text
https://<your-personal-website>/vision/
```

停止视频不会关闭 FRP 隧道或网站，只会使 `/vision/` 暂时没有可播放的视频。

FRP 常用检查：

```bash
# 阿里云
sudo systemctl status frps --no-pager
sudo journalctl -u frps -n 100 --no-pager

# 香橙派
sudo systemctl status frpc --no-pager
sudo journalctl -u frpc -n 100 --no-pager
```
