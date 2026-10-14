"""Stream every sensor mode once and report fps and frame statistics; no frame is stored.

For each requested output size: set the format, stream at the mode's default 30 fps,
then at the minimum vertical blanking, and time frames by wall clock (atomisp leaves
buffer timestamps empty).
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stream
import v4l2isp

SIZES = sys.argv[1:] or ["1408x792", "1920x1080", "1144x758", "704x396", "660x496"]


def luma_stats(frame, w, h, stride):
    y = bytes(frame[r * stride + c] for r in range(0, h, 7) for c in range(0, w, 7))
    ys = sorted(y)
    return f"Y p1={ys[len(ys) // 100]} p50={ys[len(ys) // 2]} p99={ys[len(ys) * 99 // 100]}"


def timed(cap, n):
    t0 = time.monotonic()
    for _ in range(n):
        f = cap.read(timeout=3)
        if f is None:
            return None, 0.0
    return f, n / (time.monotonic() - t0)


for size in SIZES:
    try:
        cap = v4l2isp.Capture(width=int(size.split("x")[0]), height=int(size.split("x")[1]))
    except OSError as e:
        print(f"{size}: open/stream failed: {e}")
        continue
    try:
        w, h, stride, nbytes = cap.width, cap.height, cap.stride, cap.size
        timing = stream.sensor_timing()
        top = stream.max_sensor_fps(timing)
        print(f"{size}: got {w}x{h} stride {stride} ({nbytes} B), sensor rows {timing[2]}, "
              f"line {timing[1]}, rate {timing[0]}, max {top} fps", flush=True)
        f, _ = timed(cap, 15)
        if f is None:
            print("  stalled during start-up")
            continue
        cap.set_black_level()
        cap.set_white_balance(*v4l2isp.WB_INDOOR)
        cap.set_color_matrix(v4l2isp.nearest_ccm(*v4l2isp.WB_INDOOR))
        stream.set_sensor_fps(30, timing)
        timed(cap, 10)
        f, fps30 = timed(cap, 60)
        stream.set_sensor_fps(top, timing)
        timed(cap, 10)
        g, fpsmax = timed(cap, 120)
        if f is None or g is None:
            print("  stalled while timing")
            continue
        print(f"  30 fps target -> {fps30:.2f}   {top} fps target -> {fpsmax:.2f}   "
              f"{luma_stats(g, w, h, stride)}   frames differ: {f != g}", flush=True)
    finally:
        cap.close()

err = subprocess.run(["journalctl", "-k", "-n", "300", "--no-pager"],
                     capture_output=True, text=True).stdout
if not err.strip():
    sys.exit("kernel log not readable without sudo; check: sudo dmesg | tail")
bad = [l for l in err.splitlines() if any(k in l for k in ("BUG", "Oops", "atomisp-isp2", "error"))]
print("kernel lines with errors (last 300):", len(bad))
for l in bad[-8:]:
    print("  ", l[-160:])
