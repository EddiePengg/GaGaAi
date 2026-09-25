#!/usr/bin/env python3
"""fw 0.4.0 UI 深色主题 + 设置页串口自验（设备 /dev/cu.usbmodem101）。

流程：复位读启动横幅 → 'y' 注入问答卡 → 'p' 快照（列表页）→ 'S' 开设置页
→ 'p' 快照（设置页）→ 'S' 关 → 'p' 快照（回列表）。
验证点：版本行 0.4.0、快照非黑像素统计（深色主题应远小于黄底时代）、无崩溃。
"""
import sys
import time

import serial

PORT = "/dev/cu.usbmodem101"
BAUD = 115200


def drain(ser, seconds, echo=True):
    out = b""
    end = time.time() + seconds
    while time.time() < end:
        n = ser.in_waiting
        if n:
            chunk = ser.read(n)
            out += chunk
            if echo:
                sys.stdout.write(chunk.decode("utf-8", "replace"))
                sys.stdout.flush()
        else:
            time.sleep(0.05)
    return out


def main():
    ser = serial.Serial(PORT, BAUD, timeout=0.1)
    # 打开串口触发 DTR 复位，等启动横幅
    drain(ser, 4)

    print("\n===== [1] 注入问答卡 =====")
    ser.write(b"y")
    drain(ser, 2)

    print("\n===== [2] 列表页快照 =====")
    ser.write(b"p")
    drain(ser, 5)

    print("\n===== [3] 打开设置页 =====")
    ser.write(b"S")
    drain(ser, 2)

    print("\n===== [4] 设置页快照 =====")
    ser.write(b"p")
    drain(ser, 5)

    print("\n===== [5] 音量循环两档 =====")
    ser.write(b"V")
    drain(ser, 1)
    ser.write(b"V")
    drain(ser, 1)

    print("\n===== [6] 关设置页 + 列表快照 =====")
    ser.write(b"S")
    drain(ser, 1)
    ser.write(b"p")
    drain(ser, 5)

    ser.close()
    print("\n[done]")


if __name__ == "__main__":
    main()
