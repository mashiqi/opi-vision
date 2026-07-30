# OrangePi Vision

**An all-in-one edge-vision service for OrangePi ZERO 3W and IMX219.** It combines camera capture, YOLO26s + ByteTrack tracking, A733 hardware acceleration, and multi-protocol streaming in a deployment-ready ARM64 repository.

[Quick start](#quick-start) · [Architecture](#architecture) · [Documentation](#documentation) · [中文介绍](README.zh-CN.md)

## Highlights

- **Two modes:** lightweight Camera streaming or YOLO26s + ByteTrack real-time detection and tracking.
- **A733 acceleration:** NPU inference with Cedarc/VE2 H.264 hardware encoding by default.
- **One input, three outputs:** RTSP, HLS, and WebRTC are served from the same encoded input; protocols do not trigger duplicate video encoding.
- **Deployment-ready assets:** ARM64 binaries, the NPU model, audio, Cedarc runtime material, and hardware PDFs are included.
- **Configurable video:** 640×360, 1280×720, or 1920×1080; 1–30 FPS; 0°/90°/180°/270° rotation.

## Quick start

Use an OrangePi ZERO 3W (A733/ARM64) with a working IMX219 camera driver, then place this repository at `~/orangepi-vision`:

```bash
cd ~/orangepi-vision
chmod +x vision vision-withyolo bin/* src/src-camera/build-board-tools.sh
./src/src-camera/build-board-tools.sh   # first deployment only
./vision --start                         # Camera mode
```

Start detection and tracking with `./vision-withyolo --size 1280x720 --fps 20`. Inspect or stop the service with `./vision --status`, `./vision --log`, and `./vision --stop`. Hardware encoding never silently falls back; select `--encoder software` explicitly when needed.

## Access and usage

| Protocol | Endpoint | Typical use |
|---|---|---|
| WebRTC | `http://<board-ip>:8889/vision` | Low-latency browser preview |
| HLS | `http://<board-ip>:8888/vision` | Broad browser/player compatibility |
| RTSP | `rtsp://<board-ip>:8554/vision` | VLC, NVR, and RTSP clients |

```bash
./vision --start --size 1280x720 --fps 20 --encoder hardware --rotate 180
./vision --start --encoder software
```

Stop the service before changing its mode, resolution, frame rate, encoder, or rotation.

## Architecture

```text
IMX219 → ISP/VIN (NV12) → Camera or YOLO26s + ByteTrack → H.264 + AAC → MediaMTX
                                                                  ├─ RTSP
                                                                  ├─ HLS → FRP → Nginx HTTPS → Browser
                                                                  └─ WebRTC
```

HLS uses MPEG-TS with 2-second segments and retains seven segments to prioritize stable playback.

## Configuration

Create local configuration only when Dashboard or LLM event features are required:

```bash
cp config/vision.env.example config/vision.env
```

Keep keys in `config/vision.env`; it is intentionally ignored by Git. Runtime audio is `config/stream.m4a` (AAC-LC, 48 kHz, stereo); retain the source `stream.mp3` and confirm redistribution rights before replacing either file.

## Repository layout

```text
bin/          ARM64 MediaMTX, YOLO, and encoding tools
config/       stream configuration, sample environment, and audio
docs/         hardware PDFs and public-HLS deployment guide
lib/          A733 NPU runtime libraries
src/          camera, Cedarc, LLM, and YOLO26 source
yolo-models/  A733 NPU model
vision*       service control entry points
```

## Documentation

- [Public HLS with FRP, Nginx, and HTTPS](docs/VISION_FRP_NGINX_HTTPS%E9%83%A8%E7%BD%B2%E6%8C%87%E5%8D%97.md)
- [YOLO26 cross-compilation and board deployment](src/src-yolo26/README.md)
- [Cedarc runtime and hardware encoding](src/src-cedarc/vendor/CEDARC_HARDWARE_ENCODING.md)
- [Model notes](yolo-models/README.md) · [Operations handoff](HANDOFF.md)

Camera tools build on the board with `./src/src-camera/build-board-tools.sh`. YOLO26 cross-compiles on x86_64 Ubuntu in the Allwinner Model Zoo environment with `../build_linux.sh -t a733 -s debian11`; see the YOLO guide for the complete procedure.

## Contributing and license

Issues and pull requests are welcome. Do not commit `config/vision.env`, certificates, logs, or runtime files; preserve deployment assets and update [LICENCES](LICENCES) when changing third-party or media assets.

This is a self-contained deployment repository, so MediaMTX, ARM64 binaries, the NPU model, audio, and hardware PDFs are intentionally large. Project, third-party, font, and media notices are consolidated in [LICENCES](LICENCES); there is no separate `LICENSE` file.
