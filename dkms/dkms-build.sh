#!/bin/bash
# Build the three camera modules for one kernel and collect them in out/.
# Usage: dkms-build.sh <kernel release> <kernel build dir>
set -eu
K="$1"; KDIR="$2"
cd "$(dirname "$(readlink -f "$0")")"
MM="$(echo "$K" | cut -d. -f1,2)"
TREE="$PWD/atomisp-dkms"
[ -d "$TREE/atomisp/$MM" ] || { echo "no atomisp sources for kernel $MM" >&2; exit 1; }

echo "--- atomisp $MM for $K ---"
make -C "$TREE" KVER="$K" KDIR="$KDIR" -j"$(nproc)" modules

echo "--- AR0330 sensor ---"
make -C "$KDIR" M="$PWD/sensor" -j"$(nproc)" \
     ATOMISP_INC="$TREE/atomisp/$MM/drivers/staging/media/atomisp/include" \
     KBUILD_EXTRA_SYMBOLS="$TREE/Module.symvers" modules

rm -rf out; mkdir out
A="$TREE/atomisp/$MM/drivers/staging/media/atomisp"
cp "$A/atomisp.ko" "$(find "$A" -name atomisp_gmin_platform.ko)" sensor/atomisp-ar0330.ko out/
ls -l out/
