#!/usr/bin/env python3
"""转向定标台 v2：按用户协议自动轮询采集（30s 轻拿 60-70° ↔ 30s 竖直 90°+）。

用法：python3 scripts/orient_calib.py → 浏览器开 http://127.0.0.1:8766
点「开始测试」后页面自动倒计时切换阶段，用户只管拿着设备。
数据分两桶（70°/90°）实时显示统计，结束后给出两姿态对比 + 判向建议。
原始标注数据落盘 logs/orient_phases_<ts>.txt。
"""
import glob
import json
import math
import statistics
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import serial

ts = time.strftime("%Y%m%d_%H%M%S")
RAW_PATH = f"logs/orient_phases_{ts}.txt"

# 阶段序列：姿势 A（内容正）↔ 姿势 B（内容倒），各 15s，两轮 = 1 分钟
PHASES = []
for r in range(2):
    PHASES.append({"name": f"第{r+1}轮·姿势A（内容正）", "key": "A",
                   "how": "拿起来看，内容是【正的】，保持不动", "secs": 15})
    PHASES.append({"name": f"第{r+1}轮·姿势B（内容倒）", "key": "B",
                   "how": "整体上下翻转 180°，内容【变倒】，保持不动", "secs": 15})

state_lock = threading.Lock()
cur = {"ax": 0.0, "ay": 0.0, "az": 0.0, "tilt": 0.0, "theta": 0.0,
       "gx": 0.0, "gy": 0.0, "gz": 0.0}
running = False
done = False
phase_idx = -1
phase_start = 0.0
buckets = {"A": [], "B": []}   # (ax, ay, az)
samples_total = 0
serial_err = ""
ser = None


def find_port():
    ports = glob.glob("/dev/cu.usb*")
    return ports[0] if ports else None


def serial_thread():
    global samples_total, serial_err
    last_try = 0.0
    got_any = False
    while True:
        line = ser.readline()
        if not line:
            if not got_any and time.time() - last_try > 5:
                try:
                    ser.write(b"A")
                except Exception as e:
                    serial_err = f"串口写入失败: {e}"
                    return
                last_try = time.time()
            continue
        text = line.decode("utf-8", "replace").strip()
        if "[A]," not in text:
            continue
        try:
            f = text.split("[A],")[1].split(",")
            # f = [t, mag, dev, ax, ay, az, gx, gy, gz] —— 加速度 f[3..5]，陀螺 f[6..8]
            # （2026-09-25 错位事故：曾误读 f[4..6]=ay,az,gx，定标数据全废）
            ax, ay, az = float(f[3]), float(f[4]), float(f[5])
            gx, gy, gz = float(f[6]), float(f[7]), float(f[8])
        except (IndexError, ValueError):
            continue
        got_any = True
        with state_lock:
            samples_total += 1
            cur["ax"], cur["ay"], cur["az"] = ax, ay, az
            cur["gx"], cur["gy"], cur["gz"] = gx, gy, gz
            cur["tilt"] = math.degrees(math.atan2(math.hypot(ax, ay), abs(az)))
            cur["theta"] = math.degrees(math.atan2(ay, ax))
            if running and 0 <= phase_idx < len(PHASES):
                key = PHASES[phase_idx]["key"]
                buckets[key].append((ax, ay, az))
                with open(RAW_PATH, "a") as fh:
                    fh.write(f"{key},{ax:.4f},{ay:.4f},{az:.4f}\n")


def tick_thread():
    """阶段推进：到时自动切下一阶段。"""
    global running, done, phase_idx, phase_start
    while True:
        time.sleep(0.2)
        with state_lock:
            if not running or done or phase_idx < 0:
                continue
            if time.monotonic() - phase_start >= PHASES[phase_idx]["secs"]:
                phase_idx += 1
                phase_start = time.monotonic()
                if phase_idx >= len(PHASES):
                    done = True
                    running = False
                    phase_idx = -1


def cluster_stats(key):
    d = buckets[key]
    if len(d) < 30:
        return None
    xs = [p[0] for p in d]
    ys = [p[1] for p in d]
    zs = [p[2] for p in d]
    ax, ay, az = statistics.fmean(xs), statistics.fmean(ys), statistics.fmean(zs)
    tilt = math.degrees(math.atan2(math.hypot(ax, ay), abs(az)))
    theta = math.degrees(math.atan2(ay, ax))
    return {"n": len(d), "ax": round(ax, 3), "ay": round(ay, 3),
            "az": round(az, 3), "tilt": round(tilt, 1), "theta": round(theta, 1)}


def api_state():
    with state_lock:
        idx = phase_idx
        rem = round(max(0.0, PHASES[idx]["secs"] - (time.monotonic() - phase_start)), 1) \
            if 0 <= idx < len(PHASES) else 0
        st = {
            "running": running, "done": done, "idx": idx,
            "phase": PHASES[idx] if 0 <= idx < len(PHASES) else None,
            "remaining": rem,
            "ax": round(cur["ax"], 3), "ay": round(cur["ay"], 3),
            "az": round(cur["az"], 3),
            "gx": round(cur["gx"], 2), "gy": round(cur["gy"], 2),
            "gz": round(cur["gz"], 2),
            "tilt": round(cur["tilt"], 1), "theta": round(cur["theta"], 1),
            "count": samples_total, "error": serial_err,
            "cA": cluster_stats("A"), "cB": cluster_stats("B"),
            "fw": ("翻转(倒)" if cur["ax"] < 0 else "正位") if abs(cur["az"]) <= 0.7 else "朝向不判",
        }
    # 对比结论：两桶都有数据时给差异摘要
    if st["cA"] and st["cB"]:
        a, b = st["cA"], st["cB"]
        st["diff"] = (f"A(正): ax{a['ax']:+.2f} ay{a['ay']:+.2f} az{a['az']:+.2f} "
                      f"| B(倒): ax{b['ax']:+.2f} ay{b['ay']:+.2f} az{b['az']:+.2f}")
    return json.dumps(st)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, ctype, body):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/":
            self._send(200, "text/html; charset=utf-8", HTML.encode())
        elif self.path == "/api/state":
            self._send(200, "application/json", api_state().encode())
        else:
            self._send(404, "text/plain", b"404")

    def do_POST(self):
        global running, phase_idx, phase_start, done
        if self.path == "/api/start":
            with state_lock:
                running = True
                done = False
                phase_idx = 0
                phase_start = time.monotonic()
            self._send(200, "application/json", b'{"ok":true}')
            return
        self._send(404, "text/plain", b"404")


HTML = """<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>转向定标 v2</title><style>
body{background:#0a0a0f;color:#eee;font-family:-apple-system,sans-serif;
margin:0;padding:14px;text-align:center}
h2{color:#FFD60A;margin:4px 0}
#phase{font-size:36px;font-weight:700;margin:8px 0;color:#FFD60A;min-height:46px}
#how{font-size:20px;margin:4px 0;min-height:26px}
#count{font-size:84px;font-weight:800;line-height:1;margin:6px 0}
#bar{height:10px;background:#222;border-radius:5px;margin:10px auto;max-width:560px}
#fill{height:100%;background:#FFD60A;border-radius:5px;width:0%}
button{font-size:20px;padding:12px 32px;border-radius:12px;border:0;
background:#FFD60A;font-weight:700;margin:8px}
.grid{display:flex;gap:10px;justify-content:center;flex-wrap:wrap;margin:10px 0}
.cell{background:#15151f;border-radius:10px;padding:8px 14px;min-width:100px}
.cell .v{font-size:24px;font-weight:700}
.cell .k{color:#888;font-size:12px}
#diff{color:#9cf;font-size:15px;margin:10px auto;max-width:680px;
min-height:22px;white-space:pre-wrap}
#stat{color:#666;font-size:13px}
</style></head><body>
<h2>🧭 转向定标（30s 轮询采集）</h2>
<div id=phase>就绪</div><div id=how>点「开始测试」，跟着提示拿设备</div>
<div id=count>--</div>
<div id=bar><div id=fill></div></div>
<button id=go>开始测试</button>
<div class=grid>
<div class=cell><div class=v id=vax>--</div><div class=k>ax (g)</div></div>
<div class=cell><div class=v id=vay>--</div><div class=k>ay (g)</div></div>
<div class=cell><div class=v id=vaz>--</div><div class=k>az (g)</div></div>
<div class=cell><div class=v id=vtilt>--</div><div class=k>倾角(°)</div></div>
<div class=cell><div class=v id=vtheta>--</div><div class=k>面内角(°)</div></div>
</div>
<div class=grid>
<div class=cell><div class=v id=vgx>--</div><div class=k>gx (°/s)</div></div>
<div class=cell><div class=v id=vgy>--</div><div class=k>gy (°/s)</div></div>
<div class=cell><div class=v id=vgz>--</div><div class=k>gz (°/s)</div></div>
<div class=cell><div class=v id=vfw>--</div><div class=k>固件判定</div></div>
</div>
<div id=diff></div><div id=stat></div>
<script>
async function tick(){const r=await fetch('/api/state'),s=await r.json();
vax.textContent=s.ax;vay.textContent=s.ay;vaz.textContent=s.az;
vgx.textContent=s.gx;vgy.textContent=s.gy;vgz.textContent=s.gz;
vtilt.textContent=s.tilt;vtheta.textContent=s.theta;
fw.textContent='固件判定：'+s.fw;
stat.textContent=`样本 ${s.count}｜${s.error||'串口正常'}`;
diff.textContent=s.diff||'';
if(s.done){phase.textContent='✅ 完成';how.textContent='数据齐了，告诉那边一声';
count.textContent='';bar.firstElementChild.style.width='0%';go.style.display='none';return;}
if(!s.running){phase.textContent='就绪';count.textContent='--';
how.textContent='点「开始测试」，跟着提示拿设备';return;}
phase.textContent=s.phase.name;how.textContent=s.phase.how;
count.textContent=Math.ceil(s.remaining);
fill.style.width=(100*(s.phase.secs-s.remaining)/s.phase.secs)+'%';}
go.onclick=()=>{fetch('/api/start',{method:'POST'});go.style.display='none';};
setInterval(tick,250);tick();
</script></body></html>"""


def main():
    global ser
    port = find_port()
    if not port:
        sys.exit("找不到串口（/dev/cu.usb*）——设备插了吗？")
    try:
        ser = serial.Serial(port, 115200, timeout=1)
    except Exception as e:
        sys.exit(f"串口打开失败（被占用？）：{e}")
    time.sleep(2.0)
    open(RAW_PATH, "w").close()
    threading.Thread(target=serial_thread, daemon=True).start()
    threading.Thread(target=tick_thread, daemon=True).start()
    print(f"[calib] 串口 {port}，原始数据 {RAW_PATH}")
    print("[calib] 打开 http://127.0.0.1:8766 → 点「开始测试」")
    ThreadingHTTPServer(("127.0.0.1", 8766), Handler).serve_forever()


if __name__ == "__main__":
    main()
