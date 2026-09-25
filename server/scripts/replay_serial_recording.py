#!/usr/bin/env python3
"""串口录音回放器：把真机串口 dump 的 Opus 帧灌进服务器，验证全链路（免手机 App）。

配合固件的串口调试命令（esp32/src/main.cpp，esp32/README.md「串口调试命令」）：
  1. 脚本向设备串口发 'r' → 固件开始录音，每帧 Opus 以 "OPUS <len> <hex>" dump
  2. 脚本收帧，再发 'r' 停止
  3. 脚本扮演"设备+App 哑管道"（同 simulate_device.py）：rec_start → 帧流 → rec_stop
     发到 MQTT gaga/up，订阅 gaga/down 等 receipt/error

与 simulate_device.py 的差别：音频来自鸭子真机麦克风（ES7210→Opus 真编码），
不是 ffmpeg 预生成文件。用法：
  ../server/.venv/bin/python scripts/replay_serial_recording.py --port /dev/cu.usbmodem101 --seconds 6
（录音期间对着鸭子说话，或把设备凑在 Mac 扬声器旁放 `say` 音频）
"""
from __future__ import annotations

import argparse
import json
import sys
import threading
import time

import paho.mqtt.client as mqtt
import serial

from gaga_server.frames import (FRAME_TYPE_JSON, FRAME_TYPE_OPUS,
                                FrameReassembler, encode_frame)


def main() -> int:
    ap = argparse.ArgumentParser(description="真机串口录音 → MQTT 全链路回放")
    ap.add_argument("--port", default="/dev/cu.usbmodem101")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=6.0, help="录音时长")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port-mqtt", type=int, default=1883)
    ap.add_argument("--timeout", type=float, default=120.0)
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0.1)
    packets: list[bytes] = []
    stop_evt = threading.Event()

    def reader() -> None:
        buf = b""
        while not stop_evt.is_set():
            data = ser.read(4096)
            if data:
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    line = line.decode("utf-8", "replace").strip()
                    if line.startswith("OPUS "):
                        _, ln, hx = line.split(" ", 2)
                        pkt = bytes.fromhex(hx)
                        assert len(pkt) == int(ln), f"帧长度不符: {ln} vs {len(pkt)}"
                        packets.append(pkt)
                    elif line:
                        print(f"[dev] {line}", flush=True)

    t = threading.Thread(target=reader, daemon=True)
    t.start()

    print(f"[replay] 发 'r' 开始录音，{args.seconds}s 后停止——请说话", flush=True)
    ser.write(b"r")
    time.sleep(args.seconds)
    ser.write(b"r")
    time.sleep(0.6)  # 收尾部帧
    stop_evt.set()
    t.join(timeout=2)
    ser.close()
    print(f"[replay] 采到 {len(packets)} 个 Opus 包（≈{len(packets) * 20}ms）", flush=True)
    if not packets:
        print("[replay] 没采到帧，检查串口/录音日志", flush=True)
        return 1

    done = threading.Event()
    ok = [False]
    reasm = FrameReassembler()
    reasm.on_frame = lambda ft, pl: on_down(ft, pl)

    def on_down(ftype: int, payload: bytes) -> None:
        if ftype != FRAME_TYPE_JSON:
            return
        msg = json.loads(payload.decode("utf-8"))
        print(f"[down] {msg}", flush=True)
        if msg.get("type") == "receipt":
            ok[0] = True
            done.set()
        elif msg.get("type") == "error":
            done.set()

    cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                      client_id="serial-replay")
    cli.on_connect = lambda c, u, f, rc, p=None: c.subscribe("gaga/down", qos=1)
    cli.on_message = lambda c, u, m: [reasm.feed_packet(p) for p in [bytes(m.payload)]]
    cli.connect(args.host, args.port_mqtt, keepalive=30)
    cli.loop_start()

    def pub(ftype: int, payload: bytes) -> None:
        for pkt in encode_frame(ftype, payload, 512):
            cli.publish("gaga/up", pkt, qos=1)

    pub(FRAME_TYPE_JSON, json.dumps(
        {"type": "hello", "device": "gaga-duck-serial", "fw": "serial-dump"}).encode())
    pub(FRAME_TYPE_JSON, b'{"type":"rec_start"}')
    for i, p in enumerate(packets):
        pub(FRAME_TYPE_OPUS, p)
        if i % 25 == 0:
            print(f"[replay] 上行 {i}/{len(packets)}", flush=True)
    pub(FRAME_TYPE_JSON, json.dumps(
        {"type": "rec_stop", "duration_ms": len(packets) * 20}).encode())
    print(f"[replay] 上行完毕，等服务器回执（{args.timeout}s 超时）", flush=True)

    done.wait(args.timeout)
    cli.loop_stop()
    cli.disconnect()
    print("[replay] 结果:", "receipt ✅" if ok[0] else "error/超时 ❌", flush=True)
    return 0 if ok[0] else 1


if __name__ == "__main__":
    sys.exit(main())
