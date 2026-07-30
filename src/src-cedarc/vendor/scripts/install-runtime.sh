#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
VENDOR_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd)
ROOTFS_DIR="$VENDOR_DIR/rootfs"

fail() {
    echo "install-runtime: $*" >&2
    exit 1
}

[[ "$(uname -m)" == aarch64 ]] || fail "仅支持AArch64系统"
[[ -d "$ROOTFS_DIR/usr/lib/aarch64-linux-gnu" ]] ||
    fail "缺少rootfs运行库，请先按文档复制瑞莎运行时"
[[ -f "$ROOTFS_DIR/etc/cedarc.conf" ]] ||
    fail "缺少rootfs/etc/cedarc.conf"

for library in libcdc_base.so libMemAdapter.so libVE.so libvenc_base.so \
    libvenc_codec.so libvenc_common.so libvenc_h264.so libvencoder.so; do
    [[ -e "$ROOTFS_DIR/usr/lib/aarch64-linux-gnu/$library" ]] ||
        fail "缺少运行库: $library"
done

if ((EUID != 0)); then
    exec sudo -- "$0" "$@"
fi

echo "正在安装Cedarc运行时到系统标准路径..."
cp -a -- "$ROOTFS_DIR"/. /
/sbin/ldconfig

if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules
    udevadm trigger --subsystem-match=misc || true
fi

echo "Cedarc运行时安装完成。"
echo "请重新登录以应用video组权限，然后运行 scripts/verify-runtime.sh。"
