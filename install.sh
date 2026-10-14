#!/bin/bash
# Install the BBEN MNC100 integrated camera stack on Debian 13 (Linux 6.12).
#   sudo bash install.sh
# Builds a DKMS package (atomisp ISP + gmin platform + AR0330 sensor), keeps udev from
# loading it early, and loads it from a systemd timer 60 s after boot with a crash guard.
# Undo: sudo bash uninstall.sh
set -eu
PKG=atomisp-bben
VER=1.0
# The atomisp ISP driver (kernel staging code, packaged for out-of-tree builds) is fetched
# from its upstream repository at a fixed commit instead of being copied into this one.
ATOMISP_REPO=https://github.com/EasyNetDev/atomisp-6.10-dkms.git
ATOMISP_COMMIT=8505242fa32b6bd1cf72db442e89322484cd108d
FW=/lib/firmware/intel/ipu/shisp_2401a0_v21.bin
FW_SHA256=e89359f4e4934c410c83d525e283f34c5fcce9cb5caa75ad8a32d66d3842d95c
D="$(cd "$(dirname "$0")" && pwd)"
K="$(uname -r)"

die() { echo "error: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run with sudo"

echo "=== 1. checks ==="
case "$K" in 6.12.*) ;; *) die "kernel $K: the atomisp sources used here only cover 6.12" ;; esac
[ -e /sys/bus/acpi/devices/APTA0330:00 ] || die "no AR0330 sensor (ACPI APTA0330) on this machine"
lspci -n 2>/dev/null | grep -q '8086:22b8' || die "no Intel Atom ISP2401 (PCI 8086:22b8)"
echo "  kernel $K, AR0330 sensor and ISP2401 present"

echo "=== 2. ISP firmware ==="
if [ ! -f "$FW" ] || [ "$(sha256sum "$FW" | cut -d' ' -f1)" != "$FW_SHA256" ]; then
  die "missing or wrong $FW (need sha256 $FW_SHA256). See README, section Firmware."
fi
echo "  $FW ok"

echo "=== 3. packages ==="
apt-get install -y dkms "linux-headers-$K" git python3-pil v4l-utils

echo "=== 4. atomisp sources ($ATOMISP_COMMIT) ==="
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
git clone -q "$ATOMISP_REPO" "$T/atomisp-dkms"
git -C "$T/atomisp-dkms" checkout -q "$ATOMISP_COMMIT"
[ "$(git -C "$T/atomisp-dkms" rev-parse HEAD)" = "$ATOMISP_COMMIT" ] || die "atomisp commit mismatch"

echo "=== 5. DKMS package $PKG/$VER ==="
# Blacklist first: once depmod indexes the modules, udev could otherwise load them by alias.
install -m644 "$D/system/atomisp-bben.conf" /etc/modprobe.d/atomisp-bben.conf
S="/usr/src/$PKG-$VER"
dkms remove -m "$PKG" -v "$VER" --all >/dev/null 2>&1 || true
rm -rf "$S"
mkdir -p "$S/sensor" "$S/atomisp-dkms/atomisp"
install -m644 "$D/dkms/dkms.conf" "$S/"
install -m755 "$D/dkms/dkms-build.sh" "$S/"
install -m644 "$D/dkms/sensor/Kbuild" "$S/sensor/"
install -m644 "$D/driver/atomisp-ar0330.c" "$S/sensor/"
A="$T/atomisp-dkms"
cp "$A/Makefile" "$A/LICENSE" "$A/Kbuild-CFLAGS-6.12.mk" "$A/Symbols-6.12.mk" "$S/atomisp-dkms/"
cp "$A/atomisp/Kbuild" "$S/atomisp-dkms/atomisp/"
cp -r "$A/atomisp/6.12" "$S/atomisp-dkms/atomisp/"
dkms add -m "$PKG" -v "$VER"
echo "  building (about 4 minutes on the Atom)"
dkms install -m "$PKG" -v "$VER" -k "$K"

echo "=== 6. loader, service, udev rule ==="
install -m755 "$D/system/atomisp-camera" /usr/local/sbin/atomisp-camera
install -m644 "$D/system/atomisp-camera.service" "$D/system/atomisp-camera.timer" /etc/systemd/system/
install -m644 "$D/system/61-atomisp-video.rules" /etc/udev/rules.d/
udevadm control --reload
systemctl daemon-reload
systemctl enable atomisp-camera.timer

echo "=== done ==="
dkms status -m "$PKG"
echo "The camera loads by itself about 60 s after each boot."
echo "To load it now: sudo systemctl start atomisp-camera"
echo "Your user needs the video group: sudo usermod -aG video \$USER (then log in again)"
