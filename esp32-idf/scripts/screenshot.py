#!/usr/bin/env python3
"""gaga ai 真机截屏工具（像 ADB 截屏一样）：
通过 USB 串口发 'F' 命令，固件 dump 一帧 466×466 RGB565，
本脚本按 header 协议收全字节后转 PNG。
用法：python3 screenshot.py [输出.png]
"""
import struct
import sys
import time

import serial

PORT = "/dev/cu.usbmodem101"
W, H = 466, 466
EXPECTED = W * H * 2  # RGB565 = 2 字节/像素


def read_frame(ser) -> bytes:
    # 固件 'W'：先一行 ASCII 头 "FRAME <len>\n"，再裸数据
    # 跳过混在流里的日志行，直到读到 "FRAME <n>\n"
    while True:
        line = ser.readline()
        if line.startswith(b"FRAME "):
            n = int(line.split()[1])
            break
        if not line:
            continue
    data = bytearray()
    deadline = time.time() + 30
    while len(data) < n and time.time() < deadline:
        chunk = ser.read(min(4096, n - len(data)))
        if chunk:
            data.extend(chunk)
    if len(data) != n:
        raise RuntimeError(f"数据不足 {len(data)}/{n}")
    return bytes(data)


def rgb565_to_png(data: bytes, out: str) -> None:
    try:
        from PIL import Image
    except ImportError:
        # 无 PIL：写 PPM（macOS 预览直接能开）
        with open(out.replace(".png", ".ppm"), "wb") as f:
            f.write(f"P6\n{W} {H}\n255\n".encode())
            px = bytearray()
            for i in range(0, len(data), 2):
                v = struct.unpack_from("<H", data, i)[0]
                r = (v >> 11) & 0x1F
                g = (v >> 5) & 0x3F
                b = v & 0x1F
                px += bytes((r * 255 // 31, g * 255 // 63, b * 255 // 31))
            f.write(bytes(px))
        print(f"已写 {out.replace('.png', '.ppm')}（无 PIL，PPM 格式）")
        return
    img = Image.new("RGB", (W, H))
    px = img.load()
    for y in range(H):
        for x in range(W):
            v = struct.unpack_from("<H", data, (y * W + x) * 2)[0]
            r = (v >> 11) & 0x1F
            g = (v >> 5) & 0x3F
            b = v & 0x1F
            px[x, y] = (r * 255 // 31, g * 255 // 63, b * 255 // 31)
    img.save(out)
    print(f"已写 {out}")


def main() -> None:
    out = sys.argv[1] if len(sys.argv) > 1 else "screenshot.png"
    for attempt in range(3):  # USB 偶发丢字节：短读即整帧重试
        ser = serial.Serial(PORT, 921600, timeout=0.2)
        time.sleep(0.3)
        ser.reset_input_buffer()
        ser.write(b"F")
        try:
            data = read_frame(ser)
            ser.close()
            rgb565_to_png(data, out)
            return
        except RuntimeError as e:
            print(f"第 {attempt + 1} 次失败（{e}），重试")
            ser.close()
            time.sleep(0.5)
    raise SystemExit("连续 3 次失败")


if __name__ == "__main__":
    main()
