# bben-mnc100-camera

Linux support for the integrated camera of the BBEN MNC100 mini PC on Debian 13: an onsemi
(Aptina) AR0330 sensor behind the Intel Atom ISP2401 (Cherry Trail), driven by the staging
`atomisp` driver.

It contains:

- `driver/`: the AR0330 sensor driver for atomisp, its generator, and the base it is
  generated from.
- `dkms/`, `system/`, `install.sh`: a DKMS package that survives kernel updates, loaded by a
  systemd timer after boot with a crash guard.
- `tools/`: native capture with the ISP colour pipeline, a live view in the browser, and a
  feed for viewing the camera from another machine over SSH.
- `docs/REVERSE-ENGINEERING.md`: what was read from the Windows driver and why.

## Status

Tested on one BBEN MNC100 (Atom x5-Z8350) with Debian 13, kernel 6.12.111.

| Output size | Field of view | fps at default | Max fps measured |
|---|---|---|---|
| 1920x1080 | centre crop | 30 | 48 |
| 1408x792 | smaller centre crop | 30 | 63 |
| 1144x758 | full sensor, 2x2 binned | 30 | 68 |
| 704x396 | full sensor, 3x binned | 30 | 72 |
| 660x496 | full sensor 4:3, 3x binned | 30 | 58 |

Colour (black level, white balance, colour matrix) comes from the Windows tuning data and is
applied by `tools/`. Applications that open `/dev/video0` directly get the picture without
that correction. The full 2304x1532 resolution cannot be used: atomisp only streams in a run
mode limited to 1920 pixels wide.

Before this work the community driver delivered about 0.55 fps. The cause was a register
table written for a 24 MHz clock and RAW12, where this board feeds 19.2 MHz and the Windows
driver uses RAW10. See `docs/REVERSE-ENGINEERING.md`.

## AI disclosure

The code in this repository was written with an AI coding assistant (Claude), directed and
tested on real hardware by the repository owner. It has not been independently reviewed line
by line. `atomisp` is staging code: it taints the kernel and has known bugs.

## Firmware

atomisp needs the ISP firmware `shisp_2401a0_v21.bin`, version
`irci_stable_candrpv_0415_20150521_0458`, sha256
`e89359f4e4934c410c83d525e283f34c5fcce9cb5caa75ad8a32d66d3842d95c`. It is Intel's and is not
included here. It is published in the
[intel-aero/meta-intel-aero-base](https://github.com/intel-aero/meta-intel-aero-base)
repository under `recipes-kernel/linux/linux-yocto/`. Other versions do not work. Place it at
`/lib/firmware/intel/ipu/shisp_2401a0_v21.bin`.

## Install

    sudo bash install.sh
    sudo usermod -aG video $USER    # then log in again

`install.sh` checks for the sensor, the ISP, kernel 6.12 and the firmware, installs `dkms`,
the kernel headers, `git`, `python3-pil` and `v4l-utils`, fetches the atomisp sources from
[EasyNetDev/atomisp-6.10-dkms](https://github.com/EasyNetDev/atomisp-6.10-dkms) at a fixed
commit, and builds the DKMS package `atomisp-bben` (about 4 minutes on the Atom).

The modules are blacklisted so udev never loads them early in boot. `atomisp-camera.timer`
loads them 60 s after boot. If a load ever hangs the machine, the next boot skips the
camera; re-enable it with `sudo atomisp-camera enable`. After a kernel update the service
rebuilds the modules on the next boot before loading them. Debian's own kernel hook is not
used, because a failed DKMS build there would stop the new kernel's initramfs and GRUB entry.

    sudo systemctl start atomisp-camera     # load now
    sudo systemctl stop atomisp-camera      # unload (safe to load again in the same boot)
    /usr/local/sbin/atomisp-camera status
    sudo bash uninstall.sh

## Tools

Run from the repository on the mini PC, as a user in the `video` group.

`tools/stream.py` serves a live view on `http://127.0.0.1:8081`, localhost only. Reach it from
another machine through SSH:

    python3 tools/stream.py --size 1144x758 --sensor-fps 30
    ssh -N -L 8081:localhost:8081 <mini-pc>      # on the other machine

`tools/camfeed.py` writes length-prefixed JPEG frames to stdout and status lines to stderr, for
a viewer that runs it over SSH:

    ssh <mini-pc> python3 -u <repo>/tools/camfeed.py --size 704x396 --sensor-fps 60 --fps 30

Higher sensor rates shorten the longest possible exposure, so the picture is darker in low
light. JPEG encoding on the Atom limits the sent rate (about 25 to 40 fps at 1144x758, depending
on the scene).
`tools/modes_test.py` streams every mode at 30 fps and at its maximum and prints statistics.

The camera device opens once at a time: stop one tool before starting another.

## Regenerating the driver

`driver/atomisp-ar0330.c` is generated. To rebuild it from the Windows driver yourself:

    python3 driver/gen_variant.py ar0330.sys driver/atomisp-ar0330-base.c driver/atomisp-ar0330.c modes

## Legal notes

- No Windows file is included. The register values and the colour calibration numbers were
  read from the Windows driver package for this hardware, to make the hardware work on
  Linux (interoperability). The files they come from are identified by hash in
  `docs/REVERSE-ENGINEERING.md`.
- The ISP firmware is Intel's and is not included.
- The atomisp sources are downloaded at install time from their own repository; they are not
  part of this one.

## Credits

- The AR0330 driver base: Fuzhou Rockchip Electronics (2017), via
  [kitakar5525/surface3-atomisp-cameras](https://github.com/kitakar5525/surface3-atomisp-cameras).
- atomisp: the Linux kernel staging driver and its maintainers; out-of-tree packaging by
  [EasyNetDev](https://github.com/EasyNetDev/atomisp-6.10-dkms).

## License

GPL-2.0, see `LICENSE`. The AR0330 driver keeps its original GPL-2.0 notice.
