#!/usr/bin/env python3
"""
Camera feed for a remote viewer: JPEG frames on stdout, status on stderr.

Meant to run as `ssh <mini-pc> python3 -u <repo>/tools/camfeed.py ...`, so the video only ever
travels inside the SSH channel. Each frame is a 4-byte big-endian length followed by the
JPEG. Every 2 s one JSON line goes to stderr: {"cap_fps", "pub_fps", "wb", "w", "h"}.
When the reader goes away the next write fails and the process exits, releasing the camera.
"""
import argparse
import json
import os
import signal
import struct
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stream
import v4l2isp

AWB_PERIOD = 1.0


def status(**kw):
    print(json.dumps(kw), file=sys.stderr, flush=True)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--size", default="1144x758")
    p.add_argument("--sensor-fps", type=int, default=30)
    p.add_argument("--fps", type=float, default=25, help="frames sent per second")
    p.add_argument("--scale", type=float, default=1.0)
    p.add_argument("--quality", type=int, default=80)
    p.add_argument("--saturation", type=float, default=1.0)
    a = p.parse_args()
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)  # a closed SSH channel ends us quietly

    w, h = (int(x) for x in a.size.lower().split("x"))
    # A previous feed whose SSH channel just closed can take a few seconds to exit and
    # release the camera (it shows up as EBUSY or EINVAL), so retry for a while.
    for attempt in range(16):
        try:
            cap = v4l2isp.Capture(stream.DEV, w, h)
            break
        except OSError as e:
            err = e
            time.sleep(0.5)
    else:
        status(error=f"cannot open the camera: {err.strerror} "
                     "(is camera-live or another viewer running?)")
        return 1
    timing = stream.sensor_timing()
    top = stream.max_sensor_fps(timing)
    sensor_fps = max(stream.SENSOR_FPS_MIN, min(a.sensor_fps, top))
    period = 1.0 / max(0.5, min(a.fps, sensor_fps))

    newest = {"frame": None, "seq": 0}
    cond = threading.Condition()
    stop = threading.Event()
    counts = {"cap": 0}
    awb = v4l2isp.AutoWhiteBalance()

    def reader():
        """Drain the camera at the sensor rate; all ISP ioctls stay on this thread."""
        tuned, next_awb = False, 0.0
        try:
            while not stop.is_set():
                f = cap.read(timeout=3)
                if f is None:
                    status(error="camera stalled")
                    break
                with cond:
                    newest["frame"] = f
                    newest["seq"] += 1
                    cond.notify()
                counts["cap"] += 1
                if not tuned:
                    tuned = True
                    stream.set_sensor_fps(sensor_fps, timing)
                    cap.set_black_level()
                    cap.set_white_balance(awb.red, awb.blue)
                    cap.set_color_matrix(v4l2isp.nearest_ccm(awb.red, awb.blue), a.saturation)
                now = time.monotonic()
                if now >= next_awb:
                    next_awb = now + AWB_PERIOD
                    if awb.update(f, cap.width, cap.height, cap.stride):
                        cap.set_white_balance(awb.red, awb.blue)
                        cap.set_color_matrix(v4l2isp.nearest_ccm(awb.red, awb.blue), a.saturation)
        except OSError as e:
            status(error=f"capture error: {e}")
        finally:
            stop.set()
            with cond:
                cond.notify()

    rt = threading.Thread(target=reader, daemon=True)
    rt.start()
    out = sys.stdout.buffer
    seen, sent, t0 = 0, 0, time.monotonic()
    status(w=cap.width, h=cap.height, sensor_fps=sensor_fps, max_fps=top)
    try:
        while not stop.is_set():
            start = time.monotonic()
            with cond:
                cond.wait_for(lambda: newest["seq"] != seen or stop.is_set(), timeout=3)
                if stop.is_set():
                    break
                frame, seen = newest["frame"], newest["seq"]
            jpeg = stream.to_jpeg(frame, cap.width, cap.height, cap.stride, a.scale, a.quality)
            out.write(struct.pack(">I", len(jpeg)) + jpeg)
            out.flush()
            sent += 1
            dt = time.monotonic() - t0
            if dt >= 2.0:
                status(cap_fps=round(counts["cap"] / dt, 2), pub_fps=round(sent / dt, 2),
                       wb=[round(awb.red, 3), round(awb.blue, 3)], w=cap.width, h=cap.height,
                       sensor_fps=sensor_fps, max_fps=top)
                counts["cap"], sent, t0 = 0, 0, time.monotonic()
            spare = period - (time.monotonic() - start)
            if spare > 0:
                time.sleep(spare)
    except BrokenPipeError:
        pass
    finally:
        stop.set()
        rt.join(timeout=4)  # never unmap buffers under a running read
        cap.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
