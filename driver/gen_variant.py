#!/usr/bin/env python3
"""Generate the AR0330 driver from atomisp-ar0330-base.c and the register tables of the
Windows driver (ar0330.sys 21.10586.6069.2007, x86, from the Microsoft Update Catalog).

    gen_variant.py <ar0330.sys> atomisp-ar0330-base.c atomisp-ar0330.c modes

Last argument: "modes" (what this repository ships: every usable mode, runtime fps),
"tunable" (1424x808 only, runtime fps), a FRAME_LENGTH_LINES number, or nothing (1424x808
at the Windows 30 fps). The Windows file is only read, never copied; see
docs/REVERSE-ENGINEERING.md for the table layout.
"""
import re
import struct
import sys

SYS, SRC, OUT = sys.argv[1:4]
# Optional FRAME_LENGTH_LINES override: fewer blanking lines -> higher fps.
# "tunable" instead lowers the VBLANK control minimum so fps is adjustable at runtime:
#   v4l2-ctl -d /dev/v4l-subdev0 -c vertical_blanking=<FLL - height>
EXTRA = sys.argv[4] if len(sys.argv) > 4 else None
FLL_OVERRIDE = int(EXTRA) if EXTRA and EXTRA.isdigit() else None
# "modes" = tunable plus every Windows resolution (see MODE_TABLES).
MODES = EXTRA == "modes"
TUNABLE = EXTRA in ("tunable", "modes")
VBLANK_MIN = 16  # AR0330 needs a few blanking rows; 16 keeps a margin over the 12 used above
TABLE_RVA = 0x10F48  # _sensor_res_video[2].regs -> 1424x808 @ 30 fps
WIDTH, HEIGHT = 1424, 808
XVCLK = 19200000  # PRE_PLL 3, MULT 103, VT_PIX 5, VT_SYS 2 -> 30.0 fps only at 19.2 MHz


def read_table(path, rva):
    d = open(path, "rb").read()
    e = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, e + 6)[0]
    o = e + 24 + struct.unpack_from("<H", d, e + 20)[0]
    off = None
    for i in range(nsec):
        _, vs, va, rs, rp = struct.unpack_from("<8sIIII", d, o + 40 * i)
        if va <= rva < va + max(vs, rs):
            off = rp + rva - va
    regs = []
    while True:
        typ, reg, val, _ = struct.unpack_from("<4I", d, off)
        off += 16
        if typ == 0xFFFF:
            return regs
        if typ != 2:
            raise SystemExit(f"unexpected entry type {typ:#x} at reg {reg:#x}")
        regs.append((reg, val))


def timing(final):
    """(vt_pix, llp, fll, width, height) of a register table, after asserting RAW10."""
    vco = XVCLK // final[0x302E] * final[0x3030]
    vt_pix = vco // (final[0x302A] * final[0x302C])
    assert final[0x31AC] & 0xFF == 10
    w = final[0x3008] - final[0x3004] + 1
    h = final[0x3006] - final[0x3002] + 1
    return vt_pix, final[0x300C], final[0x300A], w, h


# _sensor_res_preview (0x4141f0): RVA of each mode's register table, the output size, and
# register overrides applied on top of the Windows table.
# The Windows 2320x1548 mode itself is left out: atomisp only streams in its preview run
# mode, which tops out at 1920 wide (the video run mode stalls after one frame and the
# still-capture mode has no firmware binary), and leaving it in makes REQBUFS without
# S_FMT fall back to it and fail. Two modes derived from its table replace it:
FULL = 0x11BD8


def window(x, y, w, h):
    return {0x3004: x, 0x3002: y, 0x3008: x + w - 1, 0x3006: y + h - 1}


# Order matters: supported_modes[0] is the power-on default (the mode used so far).
MODE_TABLES = [
    (0x10F48, 1424, 808, {}),   # Windows: centre crop, no binning
    # derived: 1920x1080 output, centred crop of the full array, 1.36x the FOV of 1424x808
    (FULL, 1936, 1096, window(6 + (2320 - 1936) // 2, 6 + (1548 - 1096) // 2, 1936, 1096)),
    # derived: full array, skip 2 + bin (as the Windows small modes do with skip 3)
    (FULL, 1160, 774, {0x30A2: 3, 0x30A6: 3, 0x3040: 0x3000}),
    (0x10618, 720, 412, {}),    # Windows: 2160x1236 window, skip 3 + bin, full FOV
    (0x10308, 676, 512, {}),    # Windows: 2028x1536 window, skip 3 + bin, full FOV (4:3)
]


def add_modes(s):
    arrays, entries, rates = [], [], []
    for rva, w, h, overrides in MODE_TABLES:
        table = [(r, overrides.get(r, v)) for r, v in read_table(SYS, rva)]
        assert set(overrides) <= {r for r, _ in table}, overrides
        final = dict(table)
        vt_pix, llp, fll, win_w, win_h = timing(final)
        skip = (final[0x30A2] + 1) // 2
        assert (win_w // skip, win_h // skip) == (w, h), (w, h, win_w, win_h, skip)
        name = f"ar0330_{w}x{h}_regs"
        body = [f"\t{{0x{r:04x}, 0x{v:04x}}}," for r, v in table]
        body.append("\t{REG_DELAY, 0x000a}, /* PLL lock after the table rewrote it */")
        arrays.append(f"static const struct regval {name}[] = {{\n" + "\n".join(body)
                      + "\n\t{REG_NULL, 0x00},\n};\n")
        rates.append(vt_pix * 2)
        entries.append(
            "\t{\n"
            f"\t\t.width = {w},\n\t\t.height = {h},\n"
            "\t\t.max_fps = {\n\t\t\t.numerator = 10000,\n\t\t\t.denominator = 300000,\n\t\t},\n"
            "\t\t.exp_def = 0x018c,\n"
            f"\t\t.hts_def = {llp * 2},\n\t\t.vts_def = {fll},\n"
            f"\t\t.reg_list = {name},\n\t\t.pixel_rate = {vt_pix * 2},\n"
            "\t},")
        print(f"mode {w}x{h}: vt_pix={vt_pix} llp={llp} fll={fll} fps={vt_pix / (llp * fll):.2f} "
              f"max={vt_pix / (llp * (h + VBLANK_MIN)):.1f}")

    def sub(pat, rep, count=1):
        nonlocal s
        s, n = re.subn(pat, rep, s, count=count, flags=re.S)
        assert n == count, pat

    sub(r"(\tconst struct regval \*reg_list;\n)", r"\1\tu32 pixel_rate;\n")
    sub(r"(\tstruct v4l2_ctrl\s+\*vblank;\n)", r"\1\tstruct v4l2_ctrl\t*pixel_rate;\n")
    sub(r"static const struct regval ar0330_2304x1296_regs\[\] = \{\n\t\{REG_NULL, 0x00\}\n\};\n",
        lambda m: "\n".join(arrays))
    sub(r"(static const struct ar0330_mode supported_modes\[\] = \{\n).*?\n\};",
        lambda m: m.group(1) + "\n".join(entries) + "\n};")
    sub(r"#define AR0330_PIXEL_RATE\s+\(\d+U\)", f"#define AR0330_PIXEL_RATE\t\t({max(rates)}U)")
    # set_fmt: a new mode brings its own pixel rate and default frame length; without the
    # explicit set, modify_range keeps the old VBLANK value whenever it is still in range.
    sub(r"(__v4l2_ctrl_modify_range\(ar0330->vblank, .*?vblank_def\);\n)",
        r"\1\t\t__v4l2_ctrl_s_ctrl(ar0330->vblank, vblank_def);\n"
        r"\t\t__v4l2_ctrl_s_ctrl_int64(ar0330->pixel_rate, mode->pixel_rate);\n")
    sub(r"\tv4l2_ctrl_new_std\(handler, NULL, V4L2_CID_PIXEL_RATE,\n\t\t\t  0, AR0330_PIXEL_RATE, 1, AR0330_PIXEL_RATE\);",
        "\tar0330->pixel_rate = v4l2_ctrl_new_std(handler, NULL, V4L2_CID_PIXEL_RATE,\n"
        "\t\t\t  0, AR0330_PIXEL_RATE, 1, mode->pixel_rate);")
    # Controls written while the sensor is powered off fail on I2C and are dropped; keep
    # them in the handler instead, __ar0330_start_stream applies them all.
    sub(r"(\tint ret = 0;\n\n\tswitch \(ctrl->id\) \{\n\tcase V4L2_CID_EXPOSURE:)",
        "\tint ret = 0;\n\n\tif (!ar0330->power_on)\n\t\treturn 0;\n\n"
        "\tswitch (ctrl->id) {\n\tcase V4L2_CID_EXPOSURE:")
    sub(r"(\tif \(on == 0\) \{\n)(\t\tret = power_down\(ar0330\);\n)",
        r"\1\t\tar0330->power_on = false;\n\2")
    sub(r"(\t\t\tv4l2_err\(sd, \"could not set init registers\\n\"\);\n\t\t\tgoto unlock_and_return;\n\t\t\}\n)",
        r"\1\t\tar0330->power_on = true;\n")
    return s


regs = read_table(SYS, TABLE_RVA)
if FLL_OVERRIDE:
    if FLL_OVERRIDE < HEIGHT + 12:
        raise SystemExit("FLL below active height plus minimum blanking")
    regs.append((0x300A, FLL_OVERRIDE))
final = dict(regs)  # later writes override earlier ones, as on the wire
vco = XVCLK // final[0x302E] * final[0x3030]
vt_pix = vco // (final[0x302A] * final[0x302C])
llp, fll = final[0x300C], final[0x300A]
bits = final[0x31AC] & 0xFF
assert final[0x3008] - final[0x3004] + 1 == WIDTH
assert final[0x3006] - final[0x3002] + 1 == HEIGHT
assert bits == 10
print(f"vco={vco} vt_pix={vt_pix} llp={llp} fll={fll} fps={vt_pix / (llp * fll):.3f}")

body = ["\t{0x301a, 0x0059}, /* reset */", "\t{REG_DELAY, 0x0064},"]
for reg, val in regs:
    body.append(f"\t{{0x{reg:04x}, 0x{val:04x}}},")
    if reg == 0x301A:
        body.append("\t{REG_DELAY, 0x0032},")
body.append("\t{REG_NULL, 0x00},")

s = open(SRC).read()
s, n = re.subn(
    r"(static const struct regval ar0330_global_regs\[\] = \{\n).*?\n\};",
    lambda m: m.group(1) + "\n".join(body) + "\n};",
    s, count=1, flags=re.S)
assert n == 1
subs = [
    (r"#define AR0330_PIXEL_RATE\s+\(\d+U\)", f"#define AR0330_PIXEL_RATE\t\t({vt_pix * 2}U)"),
    (r"#define AR0330_XVCLK_FREQ\s+\d+", f"#define AR0330_XVCLK_FREQ\t\t{XVCLK}"),
    (r"#define AR0330_LINK_FREQ\s+\d+U", f"#define AR0330_LINK_FREQ\t\t{vco // 2}U"),
    (r"MEDIA_BUS_FMT_SGRBG12_1X12", "MEDIA_BUS_FMT_SGRBG10_1X10"),
    (r"ATOMISP_INPUT_FORMAT_RAW_12", "ATOMISP_INPUT_FORMAT_RAW_10"),
    (r"#define HTS_DEF \d+", f"#define HTS_DEF {llp * 2}"),
    (r"#define VTS_DEF \d+", f"#define VTS_DEF {fll}"),
    (r"\.width = 1920,", f".width = {WIDTH},"),
    (r"\.height = 1080,", f".height = {HEIGHT},"),
]
for pat, rep in subs:
    s, n = re.subn(pat, rep, s)
    assert n >= 1, pat
if MODES:
    s = add_modes(s)
if TUNABLE:
    s, n = re.subn(r"(V4L2_CID_VBLANK, )vblank_def,", rf"\g<1>{VBLANK_MIN},", s)
    assert n == 1
    s, n = re.subn(r"(__v4l2_ctrl_modify_range\(ar0330->vblank, )vblank_def,", rf"\g<1>{VBLANK_MIN},", s)
    assert n == 1
    print(f"tunable: max fps = {vt_pix / (llp * (HEIGHT + VBLANK_MIN)):.1f}")
open(OUT, "w").write(s)
