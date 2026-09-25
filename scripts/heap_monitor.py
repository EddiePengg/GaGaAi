#!/usr/bin/env python3
"""堆水位监视：每 60s 向设备发一次 'h'（heapReport），原始应答落盘。

用途：追"长时间开机卡死"。若内存泄露/碎片化，内部最大连续块会持续下滑；
若真崩溃，日志在某一刻静默（最后应答时间 = 死亡时间）。
只读+发 'h'，不碰数据流，不干扰任何功能。Ctrl+C 结束。
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
out = open(time.strftime("logs/heap_%Y%m%d_%H%M%S.log"), "ab")
print(f"[heap-mon] {ports[0]} → {out.name}（每 60s 一次 'h'，Ctrl+C 结束）")
n = 0
while True:
    try:
        ser.write(b"h")
    except Exception as e:
        print(f"[heap-mon] 串口写入失败：{e}")
        break
    t0 = time.time()
    got = 0
    while time.time() - t0 < 3:
        d = ser.read(512)
        if d:
            out.write(d)
            out.flush()
            got += len(d)
    n += 1
    print(f"[heap-mon] #{n} 应答 {got}B" + ("  ⚠️ 无应答" if got == 0 else ""))
    time.sleep(57)
