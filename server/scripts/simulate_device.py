#!/usr/bin/env python3
"""模拟设备（设备+App 哑管道合体）：MQTT 全链路联调脚本（计划步骤 4b）。

发 hello → rec_start → Opus 帧流（按协议分包）→ rec_stop，
同时订阅 gaga/down，等 receipt / error 回执后退出。

用法：
  ffmpeg -i test.m4a -ar 16000 -ac 1 -c:a libopus -b:a 16k test.opus
  .venv/bin/python scripts/simulate_device.py --file test.opus

--chunk 模拟 BLE 单包上限（默认 100B，故意小于常见 Opus 包，走分包重组路径）。
"""
from __future__ import annotations

import argparse
import json
import sys
import threading
import time
from pathlib import Path

import paho.mqtt.client as mqtt

from gaga_server.frames import (FRAME_TYPE_JSON, FRAME_TYPE_OPUS,
                                FrameReassembler, encode_frame)
from gaga_server.ogg import parse_opus_ogg


def main() -> int:
    ap = argparse.ArgumentParser(description="模拟 gaga 设备上行（MQTT 层联调）")
    ap.add_argument("--file", required=True, help="Ogg Opus 文件（ffmpeg 生成）")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--device", default="gaga-sim")
    ap.add_argument("--chunk", type=int, default=100, help="分包上限（字节）")
    ap.add_argument("--timeout", type=float, default=120.0, help="等回执超时（秒）")
    ap.add_argument("--topic-up", default="gaga/up")
    ap.add_argument("--topic-down", default="gaga/down")
    args = ap.parse_args()

    packets = parse_opus_ogg(Path(args.file).read_bytes())
    duration_ms = len(packets) * 20  # 20ms/帧（data-model.md §2）
    print(f"[sim] {args.file}: {len(packets)} 个 Opus 包（≈{duration_ms}ms）")

    done = threading.Event()
    ok = [False]
    reasm = FrameReassembler()

    def on_down_frame(ftype: int, payload: bytes) -> None:
        if ftype != FRAME_TYPE_JSON:
            print(f"[down] 非 JSON 帧 type=0x{ftype:02X} len={len(payload)}")
            return
        msg = json.loads(payload.decode("utf-8"))
        print(f"[down] {msg}")
        if msg.get("type") == "receipt":
            ok[0] = True
            done.set()
        elif msg.get("type") == "error":
            done.set()

    reasm.on_frame = on_down_frame
    reasm.on_error = lambda reason: print(f"[down] 重组异常: {reason}")

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                         client_id=f"app-{args.device}")
    client.on_message = lambda _c, _u, m: reasm.feed_packet(bytes(m.payload))
    client.connect(args.host, args.port, keepalive=60)
    client.subscribe(args.topic_down, qos=1)
    client.loop_start()
    time.sleep(0.5)  # 等订阅生效

    def publish_frame(frame_type: int, payload: bytes) -> None:
        for pkt in encode_frame(frame_type, payload, max_packet_size=args.chunk):
            client.publish(args.topic_up, pkt, qos=1)

    def publish_signal(signal: dict) -> None:
        publish_frame(FRAME_TYPE_JSON,
                      json.dumps(signal, ensure_ascii=False).encode("utf-8"))

    publish_signal({"type": "hello", "device": args.device, "fw": "sim-0.1.0"})
    time.sleep(0.2)
    publish_signal({"type": "rec_start"})
    time.sleep(0.1)
    for i, packet in enumerate(packets):
        publish_frame(FRAME_TYPE_OPUS, packet)
        if i % 50 == 0:
            print(f"[up] opus {i}/{len(packets)}")
    publish_signal({"type": "rec_stop", "duration_ms": duration_ms})
    print(f"[up] rec_stop duration_ms={duration_ms}，等待回执...")

    if not done.wait(timeout=args.timeout):
        print("[sim] 超时未收到回执")
        return 2
    if ok[0]:
        print("[sim] 收到 receipt ✅")
        return 0
    print("[sim] 收到 error ❌")
    return 1


if __name__ == "__main__":
    sys.exit(main())
