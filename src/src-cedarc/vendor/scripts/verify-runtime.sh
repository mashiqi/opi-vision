#!/usr/bin/env bash
set -u

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
VISION_DIR=$(cd -- "$SCRIPT_DIR/../../.." && pwd)
ENCODER="$VISION_DIR/bin/aw-h264-encoder"
status=0

check() {
    local description=$1
    shift
    if "$@" >/dev/null 2>&1; then
        printf 'OK   %s\n' "$description"
    else
        printf 'FAIL %s\n' "$description"
        status=1
    fi
}

[[ "$(uname -m)" == aarch64 ]] &&
    echo "OK   AArch64架构" ||
    { echo "FAIL 当前系统不是AArch64"; status=1; }

for library in libcdc_base.so libMemAdapter.so libVE.so libvenc_base.so \
    libvenc_codec.so libvenc_common.so libvenc_h264.so libvencoder.so; do
    if /sbin/ldconfig -p 2>/dev/null | grep -Fq "$library"; then
        echo "OK   $library"
    else
        echo "FAIL $library"
        status=1
    fi
done

for device in /dev/cedar_dev_ve2 /dev/dma_heap/system; do
    [[ -e "$device" ]] &&
        echo "OK   $device 存在" ||
        { echo "FAIL $device 不存在"; status=1; }
    [[ -r "$device" && -w "$device" ]] &&
        echo "OK   $device 可读写" ||
        { echo "FAIL $device 当前用户不可读写"; status=1; }
done

if [[ -x "$ENCODER" ]]; then
    "$ENCODER" --version
    if ldd "$ENCODER" 2>/dev/null | grep -q 'not found'; then
        echo "FAIL 编码器存在未解析动态库"
        ldd "$ENCODER" | grep 'not found'
        status=1
    else
        echo "OK   编码器动态库完整"
    fi
else
    echo "FAIL 找不到编码器: $ENCODER"
    status=1
fi

exit "$status"
