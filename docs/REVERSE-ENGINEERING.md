# How the Windows driver was used

The BBEN MNC100 shipped with Windows. Its camera is an onsemi (Aptina) AR0330 sensor
(ACPI `APTA0330:00`) on two MIPI lanes behind the Intel Atom ISP2401 (PCI `8086:22b8`).
On Linux the community AR0330 driver for atomisp produced its first frame and then about
0.55 fps. This document records what was read from the Windows driver to fix that, so the
work can be checked and repeated. No Windows file is part of this repository.

## Source

The Windows camera package from the Microsoft Update Catalog (search for `ACPI\APTA0330`),
version `21.10586.6069.2007` (`DriverVer=03/02/2016`). Files read:

| File | sha256 |
|---|---|
| `ar0330.sys` (32-bit kernel driver) | `efeae93bb1987bf37ba7aea3dea69afe907a96ce4dd4d622e91c5ede44912290` |
| `AR0330_FFMODULE_CHT1_C.cpf` (ISP tuning) | `aad6d286dfdd7cab748a875eda9c67e6c0093277a11dd705f325248c6990132e` |

Function and table names come from the package's own PDB (Ghidra does not apply it to this
binary; `rz-bin -P ar0330.pdb` lists the symbols). Image base `0x400000`.

## Sensor modes

`_sensor_res_preview` (`0x4141f0`) is an array of 0x88-byte mode entries: width, height,
fps, a binning factor, the link rate, and a pointer to a register table. Register tables
are 16-byte entries `{type = 2, register, value, 0}` ending with type `0xFFFF`.

| Mode | Table RVA | Window | Readout | PLL (pre, mult) | VCO | Line length | Frame length |
|---|---|---|---|---|---|---|---|
| 2320x1548 | `0x11BD8` | full array | normal | 3, 106 | 678.4 MHz | 1256 | 1800 |
| 1424x808 | `0x10F48` | centre crop | normal | 3, 103 | 659.2 MHz | 1256 | 1748 |
| 720x412 | `0x10618` | 2160x1236 | skip 3 + bin | 2, 40 | 384 MHz | 1242 | 1030 |
| 676x512 | `0x10308` | 2028x1536 | skip 3 + bin | 2, 40 | 384 MHz | 1242 | 1030 |

Every table gives exactly 30 fps, but only with a 19.2 MHz input clock (Cherry Trail
`pmc_plt_clk_4`). The Linux driver this work started from assumed 24 MHz and RAW12:

| | Windows | Linux driver before |
|---|---|---|
| Input clock | 19.2 MHz | 24 MHz |
| Bit depth (`0x31AC`) | RAW10 (`0x0A0A`) | RAW12 (`0x0C0C`) |
| PLL pre / mult / vt_pix / op_pix | 3 / 103-106 / 5 / 10 | 2 / 49 / 6 / 12 |
| MIPI timing `0x31B0-0x31BC` | `0032 0014 3d45 418f 208a 0208 8006` | `002d 0012 3b44 314d 2089 0206 8005` |

With the Windows 1424x808 table the stream runs continuously at 30.03 fps. `_Stream@8`
only toggles bit 2 of `0x301A`, as the Linux driver does.

### Above 30 fps

The 30 fps comes from vertical blanking (frame length 1748 for 808 rows), not from a
hardware limit. The driver lowers the `vertical_blanking` control minimum to 16 lines, so
the rate is set at run time:

    fps = pixel_rate / (line_length * (rows + vertical_blanking))

### Modes that work with atomisp

atomisp only streams in its preview run mode, which is limited to 1920 pixels wide. The
video run mode stalls after one frame and the still run mode has no firmware binary for this
pipeline. So the 2320x1548 table is not offered; two modes are derived from it instead
(same PLL, MIPI timing and line length):

| Output (sensor minus ISP padding) | Sensor mode | Field of view | Max fps measured |
|---|---|---|---|
| 1920x1080 | 1936x1096 centre crop of the 2320 table | 1.36x the 1424x808 crop | 48.0 |
| 1408x792 | 1424x808 (Windows) | centre crop | 63.0 |
| 1144x758 | 2320x1548 window, skip 2 + bin | full sensor | 68.1 |
| 704x396 | 720x412 (Windows) | full sensor | 72.1 |
| 660x496 | 676x512 (Windows) | full sensor, 4:3 | 58.0 |

Frames were checked to differ and to carry real data, by statistics only.

## Colour

The raw picture had a grey-brown cast. It is not infrared: Windows does all colour work in
the ISP, from the tuning file, and touches neither the sensor data pedestal (`0x301E`) nor
its channel gains. The `.cpf` file is a `CPFF` container holding an `AIQB` block of records
`{u32 size, u16 version, u16 id}`:

- record 3, black level: `0x29DF..0x29ED` per Bayer channel at gain 1x, in 8.8 fixed point
  on a 10-bit scale (about 42/1023);
- record 5, saturation level: 1023;
- record 15, sensor chromaticity response for 7 light sources;
- record 18, five colour matrices (84-byte entries, 16.16 fixed point) with the sensor R/G
  and B/G ratio they belong to.

`tools/v4l2isp.py` applies them through atomisp ioctls on the streaming file descriptor:
fixed black level (`ATOMISP_IOC_S_BLACK_LEVEL_COMP`), white balance gains in 2.14 fixed
point (`ATOMISP_IOC_S_ISP_WHITE_BALANCE`; 1.15 caps gains at 2.0, too low for indoor
light), and the colour matrix nearest to the measured white balance, folded into the ISP's
YCgCo-to-RGB stage (`ATOMISP_IOC_S_PARAMETERS`). A grey-world loop on low-chroma pixels
sets the white balance once per second. Under ~3800 K room light it converges to red 1.23,
blue 2.46, with neutral R, G and B equal.

## atomisp details found on the way

- Capture must set the format (`VIDIOC_S_FMT`) on the same file descriptor it streams
  from. atomisp drops the pipeline when that descriptor closes, and `REQBUFS` then picks
  the largest sensor mode.
- atomisp reads `V4L2_CID_LINK_FREQ` with `v4l2_g_ctrl`, which returns the menu index, not
  the frequency. With one menu entry it reads 0 and uses its default MIPI settle timing.
- atomisp never unregisters its `v4l2_async` notifier. Loading it a second time in one boot
  corrupts the notifier list (`kernel BUG ... list_add corruption`) unless `v4l2_async` is
  unloaded too, which `system/atomisp-camera stop` does.
