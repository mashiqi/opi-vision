# 在 Ubuntu 主机编译 YOLO26 A733 程序

本目录是 `orangepi-vision` 使用的 YOLO26s、ByteTrack、运动分类、schema v3
JSONL 和中文位图标注源码。目标程序是：

```text
yolo26_a733
```

Vision的实时管道使用紧密排列的NV12输入和输出。YOLO程序在内部完成
NV12/BGR转换、旋转、推理、跟踪和标注，避免在进程间来回传输BGR帧。

目标平台是 A733、ARM64、Debian 11。编译必须在 x86_64 Ubuntu 主机上的
Allwinner Model Zoo 环境中完成。

## 1. 为什么不能在本目录直接编译

`CMakeLists.txt` 使用以下相对关系：

```cmake
set(MODEL_ZOO_HOME_DIR ${CMAKE_SOURCE_DIR}/../..)
```

因此，编译时 `CMAKE_SOURCE_DIR` 必须是 Model Zoo 的
`examples/yolo26/`，它的上两级才是 Model Zoo 根目录。CMake 会从根目录
读取：

- `common/npuruntime/`：Allwinner NPU Runtime 源码、头文件和 A733 库。
- `3rdparty/opencv/opencv-4.9.0-aarch64-linux-sunxi-glibc/`：ARM64 OpenCV。
- `0-toolchains/` 和 `cmake_toolchain/`：AArch64 交叉工具链配置。
- `examples/build_linux.sh`：Model Zoo 官方 Linux 构建入口。

直接执行 `cmake orangepi-vision/src/src-yolo26` 会得到错误的
`MODEL_ZOO_HOME_DIR`，也可能误用 Ubuntu 主机的 x86-64 OpenCV。

## 2. Ubuntu 主机准备

建议使用 Model Zoo 支持的 x86_64 Ubuntu 版本，并安装基础工具：

```bash
sudo apt update
sudo apt install -y build-essential cmake make rsync file binutils pkg-config
```

准备好 Allwinner Model Zoo 后，确保 A733 Debian 11 工具链已经解压并有
执行权限，ARM64 OpenCV 4.9.0 也已解压。不同 Model Zoo 版本的工具链压缩包
名称可能不同，应优先遵循该版本根目录的说明，不要把 Ubuntu 主机的
`/usr/bin/g++` 当成目标编译器。

设置路径：

```bash
export MODEL_ZOO_ROOT="$HOME/Allwinner_Model_Zoo"
export VISION_ROOT="$HOME/imx219/orangepi-vision"
export YOLO_EXAMPLE="$MODEL_ZOO_ROOT/examples/yolo26"
```

检查依赖：

```bash
test -x "$MODEL_ZOO_ROOT/examples/build_linux.sh"
test -d "$MODEL_ZOO_ROOT/common/npuruntime"
test -d "$MODEL_ZOO_ROOT/common/npuruntime/lib_linux_aarch64/A733" || \
test -d "$MODEL_ZOO_ROOT/common/npuruntime/lib_linux_aarch64/a733"
test -d "$MODEL_ZOO_ROOT/3rdparty/opencv/opencv-4.9.0-aarch64-linux-sunxi-glibc/lib/cmake/opencv4"
test -f "$VISION_ROOT/src/src-yolo26/CMakeLists.txt"
```

如果 NPU 库目录检查失败，先查看实际平台目录：

```bash
find "$MODEL_ZOO_ROOT/common/npuruntime/lib_linux_aarch64" \
  -maxdepth 1 -mindepth 1 -type d -printf '%f\n'
```

应存在 A733 对应目录。不要通过复制其他芯片平台的库来绕过检查。

## 3. 同步全部源码

创建或复用 Model Zoo 的示例目录：

```bash
mkdir -p "$YOLO_EXAMPLE"
```

复制本目录中的全部 C/C++ 编译输入：

```bash
cp -av "$VISION_ROOT/src/src-yolo26/"*.cpp \
       "$VISION_ROOT/src/src-yolo26/"*.h \
       "$VISION_ROOT/src/src-yolo26/CMakeLists.txt" \
       "$YOLO_EXAMPLE/"
```

不要漏掉下列文件：

```bash
for file in \
  CMakeLists.txt main.cpp pipe_mode.cpp \
  detection_event_logger.cpp detection_event_logger.h \
  bytetrack.cpp bytetrack.h movement_classifier.h \
  yolo26_postprocess.h yolov26_6_pre.cpp yolov26_6_post.cpp model_config.h
do
  test -f "$YOLO_EXAMPLE/$file" || {
    echo "缺少源码：$file" >&2
    exit 1
  }
done

grep -Fx 'project(yolo26)' "$YOLO_EXAMPLE/CMakeLists.txt"
```

不要把 `tests/`、日志、MediaMTX 或整个 `orangepi-vision/bin/` 复制进示例
源码目录。它们不参与 YOLO 程序编译。

## 4. 执行 A733 Debian 11 交叉编译

```bash
cd "$YOLO_EXAMPLE"
../build_linux.sh -t a733 -s debian11
```

构建脚本应向 CMake 传入 A733 目标和 AArch64 Debian 11 工具链。
`project(yolo26)` 与 `TARGET_NAME=a733` 最终形成程序名 `yolo26_a733`，安装
目录由 CMake 形成：

```text
install/yolo26_linux_a733/yolo26_a733
```

若修改源码后怀疑仍在使用旧缓存，只清理本示例的目标安装目录后重编译：

```bash
cd "$YOLO_EXAMPLE"
EXPECTED_EXAMPLE=$(realpath "$MODEL_ZOO_ROOT/examples/yolo26")
ACTUAL_EXAMPLE=$(realpath "$YOLO_EXAMPLE")
test "$ACTUAL_EXAMPLE" = "$EXPECTED_EXAMPLE" || {
  echo "拒绝清理非预期目录：$ACTUAL_EXAMPLE" >&2
  exit 1
}
rm -rf -- "$YOLO_EXAMPLE/install/yolo26_linux_a733"
../build_linux.sh -t a733 -s debian11
```

## 5. 验证构建产物

```bash
export YOLO_OUTPUT="$YOLO_EXAMPLE/install/yolo26_linux_a733/yolo26_a733"

test -s "$YOLO_OUTPUT"
test -x "$YOLO_OUTPUT"
file "$YOLO_OUTPUT"
readelf -h "$YOLO_OUTPUT" | grep -E 'Class:|Machine:'
sha256sum "$YOLO_OUTPUT"
```

正确结果应包含：

```text
ELF 64-bit
ARM aarch64
```

确认新名称、ByteTrack 和中文标注源码确实进入产物：

```bash
LEGACY_NAME='yolo26_'"demo"'_a733'
if strings "$YOLO_OUTPUT" | grep -Fq "$LEGACY_NAME"; then
  echo '错误：仍包含旧程序名。请确认 CMakeLists.txt 和 main.cpp 已更新，并清理旧产物。' >&2
  exit 1
fi

strings "$YOLO_OUTPUT" | grep -F 'YOLO_TRACK_CLASSES'
strings "$YOLO_OUTPUT" | grep -F 'schema_version'
strings "$YOLO_OUTPUT" | grep -F 'raw-nv12-pipe'
```

中文 UTF-8 文本可能因编译器或 `strings` 的区域设置而不便直接检查，最终仍
须在板端通过实际画面确认没有乱码或方框。

## 6. 部署到板卡

先停止视觉服务：

```bash
ssh orangepi@板卡IP 'cd ~/orangepi-vision && ./vision --stop'
```

上传临时文件：

```bash
scp "$YOLO_OUTPUT" \
  orangepi@板卡IP:~/orangepi-vision/bin/yolo26_a733.new
```

板端备份、安装和依赖检查：

```bash
ssh orangepi@板卡IP '
  set -e
  cd ~/orangepi-vision
  cp -a bin/yolo26_a733 bin/yolo26_a733.bak
  install -m 0755 bin/yolo26_a733.new bin/yolo26_a733
  rm -f bin/yolo26_a733.new
  file bin/yolo26_a733
  if ldd bin/yolo26_a733 | grep "not found"; then
    echo "存在缺失的动态库" >&2
    exit 1
  fi
'
```

项目运行时会把 `orangepi-vision/lib/` 加入 `LD_LIBRARY_PATH`。至少应保留与
NPU Runtime 匹配的 `libNBGlinker.so` 和 `libVIPhal.so`。

## 7. 板端验收

```bash
ssh -t orangepi@板卡IP '
  cd ~/orangepi-vision
  ./vision-withyolo.sh --size 1280x720
  ./vision --status
  tail -n 50 log/publisher.log
  tail -n 5 log/yolo-detections.jsonl
'
```

检查：

- `/vision` 的 WebRTC、HLS、RTSP 均能播放。
- `publisher.log` 没有 NPU、模型或动态库错误。
- JSONL 为 `schema_version: 3`，包含 `track_id` 和运动状态。
- 画面显示 `person #编号` 等英文信息及“移动中、静止、待确认”等中文状态。
- 摄像头整体移动时显示“摄像头移动中”。
- `publisher.log`显示YOLO使用 `raw-nv12-pipe`。
- 0°和180°方向、尺寸、检测框及文字正确。
- 新增90°和270°必须分别确认竖屏尺寸、方向、检测框和文字。

若新程序失败，停止服务后恢复：

```bash
cd ~/orangepi-vision
./vision --stop
cp -a bin/yolo26_a733.bak bin/yolo26_a733
chmod +x bin/yolo26_a733
```

## 常见错误

### 找不到 OpenCVConfig.cmake

确认已解压 AArch64 OpenCV 4.9.0，并存在：

```text
3rdparty/opencv/opencv-4.9.0-aarch64-linux-sunxi-glibc/lib/cmake/opencv4/OpenCVConfig.cmake
```

### 输出是 x86-64

说明绕过了 Model Zoo 构建脚本或工具链未生效。重新使用：

```bash
../build_linux.sh -t a733 -s debian11
```

### 找不到 A733 NPU Runtime

检查 `common/npuruntime/lib_linux_aarch64/` 中是否有 A733 目录，并确认当前
Model Zoo 版本支持 A733。不要链接 T527、T536 或其他平台的库。

### 程序仍显示旧名称或没有中文状态

检查复制到 `examples/yolo26/` 的 `CMakeLists.txt`、`main.cpp`、
`yolov26_6_post.cpp` 是否来自本目录，然后删除本示例的
`install/yolo26_linux_a733/` 并重新编译、重新部署。
