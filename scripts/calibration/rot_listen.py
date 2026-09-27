#!/usr/bin/env python3
"""转向事件被动监听：只读串口，抓 [ROT]/[LIFT] 行落盘（不发 'A'，不抑制摇动）。

用法：python3 scripts/calibration/rot_listen.py
输出 logs/rot_events_<ts>.log。Ctrl+C 结束。
"""
import glob
import sys
import time

import serial

ports = glob.glob("/dev/cu.usb*")
if not ports:
    sys.exit("找不到串口，设备插了吗？")
ser = serial.Serial(ports[0], 115200, timeout=1)
time.sleep(2)
out = open(time.strftime("logs/rot_events_%Y%m%d_%H%M%S.log"), "ab")
print(f"[rot-listen] {ports[0]}，被动监听 [ROT]/[LIFT]（Ctrl+C 结束）", flush=True)
while True:
    d = ser.read(2048)
    if not d:
        continue
    text = d.decode("utf-8", "replace")
    if "[ROT]" in text or "[LIFT]" in text:
        out.write(text.encode("utf-8", "replace"))
        out.flush()
        for line in text.splitlines():
            if "[ROT]" in line or "[LIFT]" in line:
                print(line.strip(), flush=True)
