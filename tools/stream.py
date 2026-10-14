#!/usr/bin/env python3
"""
Live view of the integrated camera (AR0330 behind atomisp).

A reader thread captures YU12 frames over native V4L2 at the sensor rate, keeps
only the newest one, and drives the ISP colour pipeline recovered from the Windows
driver (black level, white balance, colour matrix; see v4l2isp.py); an encoder
thread turns that frame into JPEG at the publishing rate and serves it as multipart
MJPEG, which browsers render in place.

The sensor rate is set through the `vertical_blanking` control, which the sensor only
accepts while it is streaming, so it is applied after the first frame arrives.

Binds to localhost only by default. Reach it from another machine over SSH:

    ssh -N -L 8081:localhost:8081 <mini-pc>   # on the other machine
    then open http://localhost:8081

That keeps the camera feed inside the SSH tunnel instead of unauthenticated on the LAN.
Pass --bind 0.0.0.0 to expose it on the network anyway (it has no authentication).
"""
import argparse, io, os, subprocess, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import v4l2isp

DEV = "/dev/video0"
SENSOR = "/dev/v4l-subdev0"
SENSOR_FPS_MIN = 16
VBLANK_MIN = 16  # the driver's lower bound for the vertical_blanking control
AWB_PERIOD = 1.0  # seconds between white-balance updates

latest = {"jpeg": None, "at": 0.0, "n": 0, "fails": 0, "cap_fps": 0.0, "pub_fps": 0.0,
          "wb": (0.0, 0.0)}
raw = {"frame": None, "seq": 0}
lock = threading.Lock()
fresh = threading.Condition(lock)
stop = threading.Event()


def probe_format():
    out = subprocess.run(["v4l2-ctl", "-d", DEV, "-V"],
                         capture_output=True, text=True, timeout=15).stdout
    w = h = stride = size = 0
    for line in out.splitlines():
        if "Width/Height" in line:
            a = line.split(":", 1)[1].split("/")
            w, h = int(a[0]), int(a[1])
        elif "Bytes per Line" in line:
            stride = int(line.split(":", 1)[1])
        elif "Size Image" in line:
            size = int(line.split(":", 1)[1])
    if not (w and h and stride and size):
        raise RuntimeError("could not read the capture format from the driver")
    return w, h, stride, size


def set_format(width, height):
    """Ask atomisp for an output size; it picks the sensor mode that fits it."""
    r = subprocess.run(["v4l2-ctl", "-d", DEV, "-v",
                        f"width={width},height={height},pixelformat=YU12"],
                       capture_output=True, text=True, timeout=15)
    if r.returncode:
        raise RuntimeError(f"could not set {width}x{height}: {r.stderr.strip()}")


def sensor_timing():
    """(pixel_rate, line_length, rows) of the active sensor mode, from the subdev.

    fps = pixel_rate / (line_length * (rows + vertical_blanking))
    """
    run = lambda *a: subprocess.run(["v4l2-ctl", "-d", SENSOR, *a],
                                    capture_output=True, text=True, timeout=10).stdout
    fmt = run("--get-subdev-fmt", "0")
    ctrls = dict(line.split(":", 1) for line in
                 run("--get-ctrl", "pixel_rate,horizontal_blanking").splitlines() if ":" in line)
    width = rows = 0
    for line in fmt.splitlines():
        if "Width/Height" in line:
            width, rows = (int(x) for x in line.split(":", 1)[1].split("/"))
    rate = int(ctrls.get("pixel_rate", 0))
    hblank = int(ctrls.get("horizontal_blanking", 0))
    if not (width and rows and rate):
        raise RuntimeError("could not read the sensor timing from " + SENSOR)
    return rate, width + hblank, rows


def max_sensor_fps(timing):
    rate, line, rows = timing
    return int(rate / (line * (rows + VBLANK_MIN)))


def set_sensor_fps(fps, timing):
    rate, line, rows = timing
    vblank = max(VBLANK_MIN, rate // (line * fps) - rows)
    r = subprocess.run(["v4l2-ctl", "-d", SENSOR, "-c", f"vertical_blanking={vblank}"],
                       capture_output=True, text=True, timeout=10)
    if r.returncode:
        print(f"could not set sensor fps {fps}: {r.stderr.strip()}", file=sys.stderr, flush=True)
    return r.returncode == 0


def reader_loop(w, h, stride, sensor_fps, timing, isp, saturation):
    """Drain the stream at the sensor rate, keeping only the newest frame.

    Capture is native V4L2 because atomisp accepts ISP colour ioctls only on the
    streaming file descriptor; every ISP call therefore happens in this thread.
    """
    while not stop.is_set():
        try:
            cap = v4l2isp.Capture(DEV, w, h)
        except OSError as e:
            print(f"cannot open {DEV}: {e}", file=sys.stderr, flush=True)
            time.sleep(2)
            continue
        awb = v4l2isp.AutoWhiteBalance()
        count, t0, tuned, next_awb = 0, time.monotonic(), False, 0.0
        try:
            while not stop.is_set():
                frame = cap.read()
                if frame is None:
                    break
                with fresh:
                    raw["frame"] = frame
                    raw["seq"] += 1
                    fresh.notify_all()
                if not tuned:
                    tuned = True
                    set_sensor_fps(sensor_fps, timing)
                    if isp:
                        cap.set_black_level()
                        cap.set_white_balance(awb.red, awb.blue)
                        cap.set_color_matrix(v4l2isp.nearest_ccm(awb.red, awb.blue), saturation)
                now = time.monotonic()
                if isp and now >= next_awb:
                    next_awb = now + AWB_PERIOD
                    if awb.update(frame, w, h, stride):
                        cap.set_white_balance(awb.red, awb.blue)
                        cap.set_color_matrix(v4l2isp.nearest_ccm(awb.red, awb.blue), saturation)
                        with lock:
                            latest["wb"] = (awb.red, awb.blue)
                count += 1
                dt = now - t0
                if dt >= 2.0:
                    with lock:
                        latest["cap_fps"] = count / dt
                    count, t0 = 0, now
        except OSError as e:
            print(f"capture error: {e}", file=sys.stderr, flush=True)
        finally:
            cap.close()
        if not stop.is_set():
            with lock:
                latest["fails"] += 1
                latest["cap_fps"] = 0.0
            print("capture stream ended; restarting", file=sys.stderr, flush=True)
            time.sleep(2)


def to_jpeg(frame, w, h, stride, scale, quality):
    """YU12 frame -> JPEG. frombuffer reads the strided planes without copying them."""
    cw, ch, cs = w // 2, h // 2, stride // 2
    uoff = stride * h
    voff = uoff + cs * ch
    mv = memoryview(frame)
    Y = Image.frombuffer("L", (w, h), mv, "raw", "L", stride, 1)
    Cb = Image.frombuffer("L", (cw, ch), mv[uoff:], "raw", "L", cs, 1)
    Cr = Image.frombuffer("L", (cw, ch), mv[voff:], "raw", "L", cs, 1)
    out = (max(2, int(w * scale)), max(2, int(h * scale)))
    # Every plane goes straight to the output size. Chroma that is only being doubled uses
    # NEAREST: the JPEG encoder subsamples it back to 4:2:0, so filtering would be wasted.
    chroma = Image.NEAREST if out == (w, h) else Image.BILINEAR
    planes = [Y if Y.size == out else Y.resize(out, Image.BILINEAR)]
    planes += [p.resize(out, chroma) for p in (Cb, Cr)]
    buf = io.BytesIO()
    Image.merge("YCbCr", planes).save(buf, "JPEG", quality=quality)
    return buf.getvalue()


def encoder_loop(w, h, stride, scale, quality, publish_fps):
    period = 1.0 / publish_fps
    seen, count, t0 = 0, 0, time.monotonic()
    while not stop.is_set():
        start = time.monotonic()
        with fresh:
            if not fresh.wait_for(lambda: raw["seq"] != seen, timeout=1.0):
                continue
            frame, seen = raw["frame"], raw["seq"]
        try:
            jpeg = to_jpeg(frame, w, h, stride, scale, quality)
        except Exception as e:
            with lock:
                latest["fails"] += 1
            print(f"encode failed: {e}", file=sys.stderr, flush=True)
            continue
        with fresh:
            latest["jpeg"] = jpeg
            latest["at"] = time.time()
            latest["n"] += 1
            fresh.notify_all()
        count += 1
        dt = time.monotonic() - t0
        if dt >= 2.0:
            with lock:
                latest["pub_fps"] = count / dt
            count, t0 = 0, time.monotonic()
        spare = period - (time.monotonic() - start)
        if spare > 0:
            time.sleep(spare)


PAGE = """<!doctype html><meta charset=utf-8><title>MNC100 camera</title>
<style>
 body{margin:0;background:#111;color:#bbb;font:14px system-ui,sans-serif;
      display:flex;flex-direction:column;align-items:center;gap:.6rem;padding:1rem}
 img{max-width:100%;height:auto;border-radius:6px;background:#000}
 .bar{display:flex;gap:1.2rem;font-variant-numeric:tabular-nums}
 a{color:#7ab}
</style>
<h3 style="margin:.2rem">BBEN MNC100 &mdash; integrated camera</h3>
<img src="/stream.mjpg" alt="camera">
<div class=bar>
  <span id=st>connecting...</span>
  <a href="/snapshot.jpg" download>save frame</a>
</div>
<script>
setInterval(async()=>{try{
  const r=await (await fetch('/status')).json();
  document.getElementById('st').textContent =
    `sensor ${r.cap_fps.toFixed(1)} fps · video ${r.pub_fps.toFixed(1)} fps · ${r.fails} failures`;
}catch(e){}},1000);
</script>
""".encode("utf-8")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _next_frame(self, after=0.0, timeout=60):
        """Newest JPEG published after `after`, or (None, 0) on timeout."""
        with fresh:
            ok = fresh.wait_for(
                lambda: stop.is_set() or (latest["jpeg"] and latest["at"] != after),
                timeout=timeout)
            if not ok or stop.is_set():
                return None, 0
            return latest["jpeg"], latest["at"]

    def do_GET(self):
        if self.path in ("/", "/index.html"):
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(PAGE)))
            self.end_headers()
            self.wfile.write(PAGE)

        elif self.path == "/status":
            with lock:
                body = ('{"n":%d,"age":%.2f,"fails":%d,"cap_fps":%.2f,"pub_fps":%.2f,'
                        '"wb_red":%.3f,"wb_blue":%.3f}'
                        % (latest["n"],
                           time.time() - latest["at"] if latest["at"] else -1,
                           latest["fails"], latest["cap_fps"], latest["pub_fps"],
                           *latest["wb"])).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        elif self.path == "/snapshot.jpg":
            jpeg, _ = self._next_frame()
            if not jpeg:
                self.send_error(503, "no frame yet")
                return
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(jpeg)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(jpeg)

        elif self.path == "/stream.mjpg":
            self.send_response(200)
            self.send_header("Content-Type",
                             "multipart/x-mixed-replace; boundary=frame")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            sent_at = 0.0
            try:
                while not stop.is_set():
                    jpeg, at = self._next_frame(after=sent_at)
                    if not jpeg:
                        break
                    sent_at = at
                    self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\n"
                                     b"Content-Length: " + str(len(jpeg)).encode()
                                     + b"\r\n\r\n" + jpeg + b"\r\n")
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass
        else:
            self.send_error(404)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--bind", default="127.0.0.1",
                   help="address to listen on (default: localhost only)")
    p.add_argument("--port", type=int, default=8081)
    p.add_argument("--scale", type=float, default=0.5,
                   help="output scale, 1.0 = full 1408x792 (default: 0.5)")
    p.add_argument("--quality", type=int, default=75)
    p.add_argument("--size", metavar="WxH", default="1408x792",
                   help="output size; atomisp picks the sensor mode that fits. Sizes: "
                        "1920x1080 (crop, up to 48 fps), 1408x792 (smaller crop, 63), "
                        "1144x758 (full view, 68), 704x396 (full view, 72), "
                        "660x496 (full view 4:3, 58). Default: 1408x792")
    p.add_argument("--sensor-fps", type=int, default=30,
                   help=f"sensor frame rate, from {SENSOR_FPS_MIN} up to the mode's maximum "
                        "(default: 30); higher rates cap the exposure time")
    p.add_argument("--fps", type=float, default=15,
                   help="published video frame rate; JPEG encoding is the CPU cost (default: 15)")
    p.add_argument("--no-isp", action="store_true",
                   help="skip the ISP colour pipeline (raw sensor colours)")
    p.add_argument("--saturation", type=float, default=1.0,
                   help="colour matrix strength, 0 = none, 1 = Windows tuning (default: 1.0)")
    a = p.parse_args()
    if a.size:
        try:
            set_format(*(int(x) for x in a.size.lower().split("x")))
        except ValueError:
            p.error("--size must look like 1408x792")
    w, h, stride, size = probe_format()
    timing = sensor_timing()
    top = max_sensor_fps(timing)
    if not SENSOR_FPS_MIN <= a.sensor_fps <= top:
        p.error(f"--sensor-fps must be between {SENSOR_FPS_MIN} and {top} for {w}x{h}")
    if not 0 < a.fps <= a.sensor_fps:
        p.error("--fps must be positive and no higher than --sensor-fps")

    print(f"camera: {w}x{h} stride {stride}, sensor {a.sensor_fps} fps -> "
          f"publishing at scale {a.scale}, up to {a.fps:g} fps", flush=True)
    threading.Thread(target=reader_loop,
                     args=(w, h, stride, a.sensor_fps, timing, not a.no_isp, a.saturation),
                     daemon=True).start()
    threading.Thread(target=encoder_loop,
                     args=(w, h, stride, a.scale, a.quality, a.fps), daemon=True).start()

    srv = ThreadingHTTPServer((a.bind, a.port), Handler)
    where = "localhost only" if a.bind == "127.0.0.1" else a.bind
    print(f"serving on http://{a.bind}:{a.port}  ({where})", flush=True)
    print("ctrl-c to stop", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        with fresh:
            fresh.notify_all()


if __name__ == "__main__":
    main()
