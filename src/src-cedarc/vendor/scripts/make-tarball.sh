#!/bin/bash
# Build PowerVR and Cedar userspace tarballs from a Radxa A7S image.
#
# The VPU archive is prepared for an existing ARM64 Debian 12 Bookworm Server
# installation using Orange Pi's 6.6 sun60iw2 kernel and Cedar driver.
#
# Run in a privileged debian:bookworm-slim container with this repository
# mounted at /work.
#
# Input: /work/radxa-a733_bullseye_kde_r2.output_512.img
# Output: /work/pvr-userspace.tar.gz, /work/vpu-userspace.tar.gz,
#         /work/vpu-userspace.manifest and
#         /work/vpu-userspace.tar.gz.sha256
#
# No proprietary binaries are stored in this repository. The tarballs are built
# locally from the Radxa image supplied by the user.

set -euo pipefail

RADXA_IMG=/work/radxa-a733_bullseye_kde_r2.output_512.img

if [ ! -f "$RADXA_IMG" ]; then
  echo "ERROR: $RADXA_IMG not found" >&2
  echo "Download the Radxa A7S Bullseye KDE image" >&2
  echo "and place it (uncompressed) at $RADXA_IMG." >&2
  exit 1
fi

# Read rootfs geometry from the image. Radxa A7S rootfs is GPT part 3.
read -r RADXA_START RADXA_SECTORS < <(partx -g -b -o START,SECTORS -r --nr 3 "$RADXA_IMG")
: "${RADXA_START:?could not read Radxa partition 3 from $RADXA_IMG}"
: "${RADXA_SECTORS:?could not read Radxa partition 3 size from $RADXA_IMG}"

RADXA_PART_OFFSET=$((RADXA_START * 512))
RADXA_PART_SIZE=$((RADXA_SECTORS * 512))

LOOP=$(losetup -o $RADXA_PART_OFFSET --sizelimit $RADXA_PART_SIZE -f --show "$RADXA_IMG")

mkdir -p /mnt/radxa
mount -o ro "$LOOP" /mnt/radxa

cleanup() {
  umount -l /mnt/radxa 2>/dev/null || true
  losetup -d "$LOOP" 2>/dev/null || true
}
trap cleanup EXIT

# pvr-userspace.tar.gz
echo ">> Building pvr-userspace.tar.gz"
PVR_STAGE=/tmp/pvr-stage
rm -rf "$PVR_STAGE"; mkdir -p "$PVR_STAGE"


PVR_LIST=/mnt/radxa/var/lib/dpkg/info/xserver-xorg-img-bxm-1.21.1-2.deb.list
PVR_COPIED=0
while read -r path; do
  [ -z "$path" ] && continue
  case "$path" in
    /usr/bin/Xorg|/etc/X11/*|/usr/lib/xorg/*|/usr/lib/systemd/*|/usr/lib/libxcvt*|/etc/environment)
      continue;;
    # Skip /lib/* as the only PVR content there is firmware. This is re-added under /usr/lib/firmware below.
    /lib|/lib/*) continue;;
  esac

  src="/mnt/radxa$path"

  if [ -L "$src" ] || [ -f "$src" ]; then
    mkdir -p "$PVR_STAGE$(dirname "$path")"
    cp -a "$src" "$PVR_STAGE$path"
    PVR_COPIED=$((PVR_COPIED+1))
  fi
done < "$PVR_LIST"

# Firmware + ICD + ld.so.conf + autoload
mkdir -p "$PVR_STAGE/usr/lib/firmware" "$PVR_STAGE/usr/share/vulkan/icd.d" \
         "$PVR_STAGE/etc/ld.so.conf.d" "$PVR_STAGE/etc/modules-load.d" \
         "$PVR_STAGE/usr/lib/aarch64-linux-gnu/dri"

cp -a /mnt/radxa/lib/firmware/rgx.* "$PVR_STAGE/usr/lib/firmware/"
cp /mnt/radxa/usr/share/vulkan/icd.d/img_icd.json "$PVR_STAGE/usr/share/vulkan/icd.d/"
cp /mnt/radxa/etc/ld.so.conf.d/00_xserver-xorg-img-bxm.conf "$PVR_STAGE/etc/ld.so.conf.d/"

echo pvrsrvkm > "$PVR_STAGE/etc/modules-load.d/pvr.conf"
[ -f /mnt/radxa/usr/local/lib/dri/pvr_dri.so ] && \
  cp -a /mnt/radxa/usr/local/lib/dri/pvr_dri.so "$PVR_STAGE/usr/lib/aarch64-linux-gnu/dri/"

rm -f /work/pvr-userspace.tar.gz
tar -C "$PVR_STAGE" -czf /work/pvr-userspace.tar.gz .
echo "   pvr: $PVR_COPIED files; tarball: $(du -h /work/pvr-userspace.tar.gz | cut -f1)"


# vpu-userspace.tar.gz
echo ">> Building vpu-userspace.tar.gz"
VPU_STAGE=/tmp/vpu-stage
rm -rf "$VPU_STAGE"; mkdir -p "$VPU_STAGE"

VPU_LISTS="
/mnt/radxa/var/lib/dpkg/info/libcedarc-dev-2.0.0-arm64.list
/mnt/radxa/var/lib/dpkg/info/libgstreamer-openmax-allwinner.list
"

VPU_COPIED=0
for L in $VPU_LISTS; do
  if [ ! -f "$L" ]; then
    echo "ERROR: required Radxa package list not found: $L" >&2
    exit 1
  fi
  while read -r path; do
    [ -z "$path" ] && continue
    case "$path" in
      /usr/include/*|/usr/share/doc/*|/usr/share/metainfo/*|/usr/share/man/*) continue;;
      # XVideo output is a desktop/X11 plugin and is not needed on Bookworm Server.
      */gstreamer-1.0/libgstxvimagesink.so) continue;;
    esac
    src="/mnt/radxa$path"
    case "$path" in
      /lib/*) dst_path="/usr$path" ;;
      *) dst_path="$path" ;;
    esac
    if [ -L "$src" ] || [ -f "$src" ]; then
      mkdir -p "$VPU_STAGE$(dirname "$dst_path")"
      cp -a "$src" "$VPU_STAGE$dst_path"
      VPU_COPIED=$((VPU_COPIED+1))
    fi
  done < "$L"
done

# libgstomx.so has a set of workarounds. Radxa doesn't enable them all.
# On the OPi, decode/encode stalls during the OMX Loaded -> Idle transition without
# these extra workarounds enabled. The stall happens because the output port
# keeps the 176x144 placeholder format. Enabling pass-color-format-to-decoder,
# height-multiple-16, and the other supported flags fixes decode and encode.
#
# Valid flag names were taken from the plugin binary:
#   strings /usr/lib/aarch64-linux-gnu/gstreamer-1.0/libgstomx.so | \
#     grep -E "^(no-|event-|signals-|pass-|height-)"
GSTOMX_STAGED="$VPU_STAGE/etc/xdg/gstomx.conf"

if [ -f "$GSTOMX_STAGED" ]; then
  HACKS="event-port-settings-changed-ndata-parameter-swap;event-port-settings-changed-port-0-to-1;no-disable-outport;no-component-reconfigure;no-component-role;no-empty-eos-buffer;pass-color-format-to-decoder;pass-profile-to-decoder;signals-premature-eos;height-multiple-16"
  sed -i "s|^hacks=.*|hacks=$HACKS|" "$GSTOMX_STAGED"

  # The Radxa source currently contains two [omxh264videoenc] sections. Keep
  # the first occurrence of every section so GStreamer registers one factory.
  awk '
    /^\[/ {
      section = $0
      if (seen[section]++)
        skip = 1
      else
        skip = 0
    }
    !skip { print }
  ' "$GSTOMX_STAGED" > "$GSTOMX_STAGED.tmp"
  mv "$GSTOMX_STAGED.tmp" "$GSTOMX_STAGED"

  # This is the final location after extracting the archive on Bookworm.
  sed -i 's|^core-name=.*|core-name=/usr/lib/aarch64-linux-gnu/libOmxCore.so|' \
    "$GSTOMX_STAGED"

  if [ "$(grep -c '^\[omxh264videoenc\]$' "$GSTOMX_STAGED")" -ne 1 ]; then
    echo "ERROR: gstomx.conf must contain exactly one [omxh264videoenc] section" >&2
    exit 1
  fi
  echo "   patched gstomx.conf: $(grep -c '^hacks=' "$GSTOMX_STAGED") entries"
else
  echo "ERROR: gstomx.conf was not copied from the Radxa image" >&2
  exit 1
fi

# Make Cedar device nodes usable on headless boots
mkdir -p "$VPU_STAGE/etc/udev/rules.d"
cat > "$VPU_STAGE/etc/udev/rules.d/99-zz-cedar-ve.rules" <<'EOF'
KERNEL=="cedar_dev*", GROUP="video", MODE="0660"
SUBSYSTEM=="cedar_ve", TAG+="uaccess", GROUP="video", MODE="0660"
SUBSYSTEM=="cedar_ve2", TAG+="uaccess", GROUP="video", MODE="0660"
EOF

rm -f /work/vpu-userspace.tar.gz \
      /work/vpu-userspace.manifest \
      /work/vpu-userspace.tar.gz.sha256

(cd "$VPU_STAGE" && \
  find . \( -type f -o -type l \) -printf '%P\n' | LC_ALL=C sort) \
  > /work/vpu-userspace.manifest

tar -C "$VPU_STAGE" -czf /work/vpu-userspace.tar.gz .
(cd /work && sha256sum vpu-userspace.tar.gz \
  > vpu-userspace.tar.gz.sha256)
echo "   vpu: $VPU_COPIED files; tarball: $(du -h /work/vpu-userspace.tar.gz | cut -f1)"

echo
echo "Done. Tarballs in /work:"
ls -lh /work/pvr-userspace.tar.gz \
       /work/vpu-userspace.tar.gz \
       /work/vpu-userspace.manifest \
       /work/vpu-userspace.tar.gz.sha256
echo
echo "Apply VPU userspace on the tested OPi Bookworm Server with:"
echo "  scp vpu-userspace.tar.gz vpu-userspace.manifest vpu-userspace.tar.gz.sha256 orangepi@<pi-ip>:/tmp/"
echo "  # then on the Pi:"
echo "  cd /tmp && sha256sum -c vpu-userspace.tar.gz.sha256"
echo "  sudo tar xzpf /tmp/vpu-userspace.tar.gz -C /"
echo "  sudo ldconfig"
echo "  sudo udevadm control --reload-rules && sudo udevadm trigger"
echo "  sudo reboot"
echo
echo "After reboot, verify the encoder with:"
echo "  gst-inspect-1.0 omxh264videoenc"
echo "Always set interval-intraframes explicitly; 30 is the tested default."
