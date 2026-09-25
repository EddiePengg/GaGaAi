#!/usr/bin/env python3
"""IMU 原始数据流采集（'A' 串口流 → 文件）。

用法：python3 scripts/accel_capture.py [时长秒] [输出文件]
默认 600 秒，输出 logs/accel_<时间戳>.log。
自动找 /dev/cu.usb* 端口；打开后发 'A' 开流，结束时再发 'A' 停流。
"""
import glob
import sys
import time

import serial

DUR = int(sys.argv[1]) if len(sys.argv) > 1 else 600
OUT = sys.argv[2] if len(sys.argv) > 2 else time.strftime("logs/accel_%Y%m%d_%H%M%S.log")

ports = glob.glob("/dev/cu.usb*")
if not ports:
    sys.exit("找不到串口（/dev/cu.usb*），设备插了吗？")
port = ports[0]
print(f"[capture] 端口 {port}，时长 {DUR}s，输出 {OUT}")

ser = serial.Serial(port, 115200, timeout=1)
time.sleep(2.0)          # 等 USB-CDC 就绪（开端口可能触发枚举重连）
ser.write(b"A")          # 开流
ser.flush()
print("[capture] 'A' 已发，开始采集…")

end = time.time() + DUR
n = 0
got_any = False      # 收到过数据（设备醒着且流已开）
last_try = 0.0
with open(OUT, "wb") as f:
    while time.time() < end:
        d = ser.read(4096)
        if d:
            got_any = True
            f.write(d)
            n += len(d)
        elif not got_any and time.time() - last_try > 5:
            ser.write(b"A")   # 设备未醒/流未开：每 5s 重发一次（醒来即开流）
            last_try = time.time()
            print("[capture] 静默中，重发 'A'…")

ser.write(b"A")          # 停流
ser.flush()
ser.close()
print(f"[capture] 完成，共 {n} 字节 → {OUT}")
