#!/usr/bin/env python3
"""一次性验证脚本：等设备启动稳定 → 注入带 emoji 的卡 → 截屏存 PNG。
全程不重开串口（重开 = DTR 脉冲 = 设备复位 = 注入丢失，教训见 dev-log）。"""
import sys
import time

import serial

PORT = "/dev/cu.usbmodem101"
W, H = 466, 466


def main() -> None:
    out = sys.argv[1] if len(sys.argv) > 1 else "verify.png"
    ser = serial.Serial(PORT, 921600, timeout=0.2)
    ser.setDTR(False)
    ser.setRTS(False)
    time.sleep(4)            # 等启动稳定（开串口可能触发复位）
    ser.reset_input_buffer()

    ser.write(b"t")          # 确保 talk 关闭（幂等：关→开→再关也收敛）
    time.sleep(1)
    ser.write(b"t")
    time.sleep(1)
    ser.write(b"y")          # 注入带 emoji 的测试卡
    time.sleep(1)
    ser.reset_input_buffer()

    ser.write(b"F")          # 整帧转储
    hdr = b""
    while True:
        line = ser.readline()
        if line.startswith(b"FRAME "):
            n = int(line.split()[1])
            break
    data = bytearray()
    deadline = time.time() + 30
    while len(data) < n and time.time() < deadline:
        chunk = ser.read(min(8192, n - len(data)))
        if chunk:
            data.extend(chunk)
    ser.close()
    if len(data) != n:
        raise SystemExit(f"数据不足 {len(data)}/{n}")

    import struct
    import zlib
    px = bytearray()
    for i in range(0, len(data), 2):
        v = struct.unpack_from("<H", data, i)[0]
        px += bytes((((v >> 11) & 31) * 255 // 31,
                     ((v >> 5) & 63) * 255 // 63,
                     (v & 31) * 255 // 31))
    raw = b"".join(b"\x00" + px[y * W * 3:(y + 1) * W * 3] for y in range(H))

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw))
    png += chunk(b"IEND", b"")
    open(out, "wb").write(png)
    print(f"已写 {out}")


if __name__ == "__main__":
    main()
