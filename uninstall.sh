#!/bin/bash
# Undo install.sh: sudo bash uninstall.sh
# The ISP firmware and installed Debian packages are left in place.
set -u
PKG=atomisp-bben
VER=1.0
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
systemctl disable --now atomisp-camera.timer 2>/dev/null
systemctl stop atomisp-camera.service 2>/dev/null
rm -f /etc/systemd/system/atomisp-camera.service /etc/systemd/system/atomisp-camera.timer
systemctl daemon-reload
rm -f /usr/local/sbin/atomisp-camera /etc/modprobe.d/atomisp-bben.conf \
      /etc/udev/rules.d/61-atomisp-video.rules
udevadm control --reload
rm -rf /var/lib/atomisp-camera
dkms remove -m "$PKG" -v "$VER" --all
rm -rf "/usr/src/$PKG-$VER"
echo "removed"
