"""
Minimal V4L2 mmap capture for atomisp, plus the ISP colour parameters it needs.

atomisp only accepts ISP parameter ioctls on the file descriptor that is streaming
(a second open of /dev/video0 returns EBUSY while streaming, and the ioctls return
EINVAL when nothing streams), so capture and colour control live in one object.

Colour values come from the Windows tuning file AR0330_FFMODULE_CHT1_C.cpf
(record 3 black level, record 18 colour matrices); see
docs/REVERSE-ENGINEERING.md.
"""
import fcntl
import mmap
import os
import select
import struct


def _ioc(direction, typ, nr, size):
    return (direction << 30) | (size << 16) | (ord(typ) << 8) | nr


_W, _R, _RW = 1, 2, 3
VIDIOC_G_FMT = _ioc(_RW, "V", 4, 208)
VIDIOC_S_FMT = _ioc(_RW, "V", 5, 208)
VIDIOC_REQBUFS = _ioc(_RW, "V", 8, 20)
VIDIOC_QUERYBUF = _ioc(_RW, "V", 9, 88)
VIDIOC_QBUF = _ioc(_RW, "V", 15, 88)
VIDIOC_DQBUF = _ioc(_RW, "V", 17, 88)
VIDIOC_STREAMON = _ioc(_W, "V", 18, 4)
VIDIOC_STREAMOFF = _ioc(_W, "V", 19, 4)
# Values printed by a C probe compiled against the atomisp 6.12 header on the mini PC.
ATOMISP_IOC_S_BLACK_LEVEL_COMP = 0x401876C4  # struct atomisp_ob_config, 24 bytes
ATOMISP_IOC_S_ISP_WHITE_BALANCE = 0x401476D0  # struct atomisp_wb_config, 20 bytes
ATOMISP_IOC_S_PARAMETERS = 0x41F076E0  # struct atomisp_parameters, 496 bytes
PARAMS_SIZE, PARAMS_OFF_YUV2RGB = 496, 192

BUF_TYPE_VIDEO_CAPTURE, MEMORY_MMAP = 1, 1
PIX_FMT_YUV420 = 0x32315559  # 'YU12'
# struct v4l2_format: u32 type, 4 bytes padding (the union holds pointers), then
# v4l2_pix_format: width height pixelformat field bytesperline sizeimage ...
_FMT = struct.Struct("<I4xIIIIII176x")
# struct v4l2_buffer on x86-64, 88 bytes: index type bytesused flags field | timestamp |
# timecode | sequence memory m.offset(+4 pad) length reserved2 request_fd(+4 pad)
_BUF = struct.Struct("<IIIII4x16x16xIII4xII8x")

# --- Colour data recovered from the Windows driver --------------------------------------
# Record 3, gain 1x entry, order Gr R B Gb, 8.8 fixed point on a 10-bit scale.
# u0.16 (what atomisp wants) = value / 256 / 1024 * 65536 = value / 4.
BLACK_LEVEL_U016 = tuple(v // 4 for v in (0x29DF, 0x29E4, 0x29F3, 0x29ED))
# Record 18, first matrix (CIE x 0.333, y 0.347, about 5500 K), 16.16 fixed point.
CCM_5500K = (
    (1.777, -0.596, -0.181),
    (-0.346, 1.496, -0.150),
    (-0.057, -0.591, 1.648),
)
# Same record: sensor R/G = 187/256 and B/G = 179/256, so the gains are their inverses.
WB_5500K = (256 / 187, 256 / 179)  # (red, blue), green stays 1.0
# Grey-world result under the room's ceiling light (2026-10-14): matches CCMS[1], ~3800 K.
# AWB starts here so the live view is right from the first second indoors.
WB_INDOOR = (1.23, 2.46)
# All five record-18 matrices (84-byte entries): (sensor R/G, sensor B/G, CCM).
# Pick the one whose R/G, B/G is closest to 1/red_gain, 1/blue_gain after AWB.
CCMS = (
    (0.730, 0.699, CCM_5500K),  # CIE 0.333,0.347  ~5500 K daylight
    (0.816, 0.391, ((1.678, -0.540, -0.138), (-0.419, 1.488, -0.068), (-0.132, -0.736, 1.868))),  # ~3800 K
    (0.676, 0.379, ((2.217, -1.049, -0.168), (-0.458, 1.541, -0.082), (-0.114, -0.592, 1.705))),  # ~3900 K
    (0.551, 0.824, ((1.778, -0.632, -0.146), (-0.278, 1.487, -0.209), (-0.018, -0.464, 1.482))),  # ~8000 K shade
    (1.031, 0.340, ((1.851, -0.618, -0.233), (-0.480, 1.625, -0.144), (-0.132, -1.177, 2.309))),  # ~2850 K tungsten
)


def nearest_ccm(red_gain, blue_gain):
    rg, bg = 1 / red_gain, 1 / blue_gain
    return min(CCMS, key=lambda c: (c[0] - rg) ** 2 + (c[1] - bg) ** 2)[2]


def grey_world(frame, w, h, stride, step=6):
    """Mean RGB of near-neutral pixels of a YU12 frame, and how many were sampled.

    Restricting grey-world to low-chroma, mid-tone pixels keeps saturated objects
    and clipped highlights from dragging the white balance.
    """
    cw, cs = w // 2, stride // 2
    u0 = stride * h
    v0 = u0 + cs * (h // 2)
    acc = [0.0, 0.0, 0.0]
    n = 0
    for row in range(0, h // 2, step):
        for col in range(0, cw, step):
            u = frame[u0 + row * cs + col] - 128
            v = frame[v0 + row * cs + col] - 128
            y = frame[2 * row * stride + 2 * col]
            if 30 < y < 235 and abs(u) < 40 and abs(v) < 40:
                acc[0] += y + 1.402 * v
                acc[1] += y - 0.344 * u - 0.714 * v
                acc[2] += y + 1.772 * u
                n += 1
    return [a / max(n, 1) for a in acc], n


class AutoWhiteBalance:
    """Closed-loop grey-world gains; holds its last gains when the scene is too dark."""

    def __init__(self, red=WB_INDOOR[0], blue=WB_INDOOR[1], damping=0.5, min_samples=150):
        self.red, self.blue = red, blue
        self.damping, self.min_samples = damping, min_samples

    def update(self, frame, w, h, stride):
        """Returns True when the gains changed and must be pushed to the ISP."""
        (r, g, b), n = grey_world(frame, w, h, stride)
        if n < self.min_samples or min(r, g, b) <= 1:
            return False
        clamp = lambda x: min(4.0, max(0.5, x))
        red = clamp(self.red * (g / r) ** self.damping)
        blue = clamp(self.blue * (g / b) ** self.damping)
        if abs(red - self.red) < 0.01 and abs(blue - self.blue) < 0.01:
            return False
        self.red, self.blue = red, blue
        return True


# atomisp default YCgCo -> RGB matrix (gc_2 default_yuv2rgb_cc_config, 12 fraction bits).
YCGCO_TO_RGB = ((1, -1, 1), (1, 1, 0), (1, -1, -1))


class Capture:
    """YU12 capture. The format must be set on this same file descriptor: atomisp drops
    the pipeline configuration when the descriptor that set it is closed, and REQBUFS
    then falls back to the largest sensor mode."""

    def __init__(self, dev="/dev/video0", width=None, height=None, nbuf=4):
        self.fd = os.open(dev, os.O_RDWR | os.O_NONBLOCK)
        self.bufs = []
        try:
            fmt = bytearray(_FMT.pack(BUF_TYPE_VIDEO_CAPTURE, 0, 0, 0, 0, 0, 0))
            fcntl.ioctl(self.fd, VIDIOC_G_FMT, fmt)
            _, w, h, *_ = _FMT.unpack(fmt)
            fmt = bytearray(_FMT.pack(BUF_TYPE_VIDEO_CAPTURE, width or w, height or h,
                                      PIX_FMT_YUV420, 1, 0, 0))  # field 1 = none
            fcntl.ioctl(self.fd, VIDIOC_S_FMT, fmt)
            _, self.width, self.height, _, _, self.stride, self.size = _FMT.unpack(fmt)
            req = bytearray(struct.pack("<IIII4x", nbuf, BUF_TYPE_VIDEO_CAPTURE, MEMORY_MMAP, 0))
            fcntl.ioctl(self.fd, VIDIOC_REQBUFS, req)
            count = struct.unpack_from("<I", req)[0]
            for i in range(count):
                b = bytearray(_BUF.pack(i, BUF_TYPE_VIDEO_CAPTURE, 0, 0, 0, 0, MEMORY_MMAP, 0, 0, 0))
                fcntl.ioctl(self.fd, VIDIOC_QUERYBUF, b)
                f = _BUF.unpack(b)
                offset, length = f[7], f[8]
                self.bufs.append(mmap.mmap(self.fd, length, mmap.MAP_SHARED,
                                           mmap.PROT_READ | mmap.PROT_WRITE, offset=offset))
                fcntl.ioctl(self.fd, VIDIOC_QBUF, b)
            fcntl.ioctl(self.fd, VIDIOC_STREAMON, struct.pack("<I", BUF_TYPE_VIDEO_CAPTURE))
        except Exception:
            self.close()
            raise

    def read(self, timeout=5.0):
        """Next frame as bytes (bytesused long), or None on timeout."""
        if not select.select([self.fd], [], [], timeout)[0]:
            return None
        b = bytearray(_BUF.pack(0, BUF_TYPE_VIDEO_CAPTURE, 0, 0, 0, 0, MEMORY_MMAP, 0, 0, 0))
        fcntl.ioctl(self.fd, VIDIOC_DQBUF, b)
        f = _BUF.unpack(b)
        index, used = f[0], f[2]
        data = self.bufs[index][:used]
        fcntl.ioctl(self.fd, VIDIOC_QBUF, b)
        return data

    def close(self):
        try:
            fcntl.ioctl(self.fd, VIDIOC_STREAMOFF, struct.pack("<I", BUF_TYPE_VIDEO_CAPTURE))
        except OSError:
            pass
        for m in self.bufs:
            m.close()
        self.bufs = []
        os.close(self.fd)

    # --- ISP colour pipeline ---------------------------------------------------------------
    def set_black_level(self, levels=BLACK_LEVEL_U016):
        """Fixed optical-black subtraction, levels (Gr, R, B, Gb) in u0.16."""
        gr, r, b, gb = levels
        fcntl.ioctl(self.fd, ATOMISP_IOC_S_BLACK_LEVEL_COMP,
                    struct.pack("<5IHH", 1, gr, r, b, gb, 0, 0))  # mode 1 = fixed

    def set_white_balance(self, red, blue, green=1.0):
        """Bayer-domain gains, encoded as 2.14 fixed point (integer_bits = 2, max ~4.0).

        1.15 (integer_bits = 1) silently caps gains at 2.0, which is not enough for
        warm indoor light: measured, blue stopped responding above 2.0.
        """
        enc = lambda g: max(0, min(65535, round(g * 16384)))
        fcntl.ioctl(self.fd, ATOMISP_IOC_S_ISP_WHITE_BALANCE,
                    struct.pack("<5I", 2, enc(green), enc(red), enc(blue), enc(green)))

    def set_color_matrix(self, ccm=CCM_5500K, saturation=1.0):
        """Fold an RGB colour matrix into atomisp's YCgCo->RGB stage (CCM2).

        saturation < 1 blends the matrix toward identity to tame noise.
        """
        ident = ((1, 0, 0), (0, 1, 0), (0, 0, 1))
        m = [[ident[i][j] + saturation * (ccm[i][j] - ident[i][j]) for j in range(3)]
             for i in range(3)]
        full = [[sum(m[i][k] * YCGCO_TO_RGB[k][j] for k in range(3)) for j in range(3)]
                for i in range(3)]
        frac = 11  # s2.11 covers +-4.0; the folded matrix reaches about -2.2
        flat = [max(-8192, min(8191, round(v * (1 << frac)))) for row in full for v in row]
        cc = struct.pack("<I9i", frac, *flat)
        cc_buf = bytearray(cc)
        params = bytearray(PARAMS_SIZE)
        # Only yuv2rgb_cc_config is set; every other pointer stays NULL and is ignored.
        addr = _buffer_address(cc_buf)
        struct.pack_into("<Q", params, PARAMS_OFF_YUV2RGB, addr)
        fcntl.ioctl(self.fd, ATOMISP_IOC_S_PARAMETERS, params)
        return flat


def _buffer_address(buf):
    import ctypes
    return ctypes.addressof((ctypes.c_char * len(buf)).from_buffer(buf))
