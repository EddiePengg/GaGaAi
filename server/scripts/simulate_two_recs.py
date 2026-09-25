#!/usr/bin/env python3
"""两段连发测试：验证 rec_start 到来时上一段未完成的录音被立即收尾投递
（异步队列语义，ADR-022 修正），而不是被作废。

流程：hello → rec_start → 段1 帧流 → rec_stop → 0.5s 后 rec_start（此时段1
流式会话仍在等终稿）→ 段2 帧流 → rec_stop → 等两个 receipt。
判定：两段各自投递飞书（服务端日志两段 "已投递飞书"）+ 收到两个 receipt。

用法：.venv/bin/python scripts/simulate_two_recs.py
素材：test-audio/talk_q1.opus + test.opus（两句不同文本，飞书里可区分）
"""
from __future__ import annotations

import json
import sys
import threading
import time
from pathlib import Path

import paho.mqtt.client as mqtt

from gaga_server.frames import (FRAME_TYPE_JSON, FRAME_TYPE_OPUS,
                                FrameReassembler, encode_frame)
from gaga_server.ogg import parse_opus_ogg

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    seg_files = [ROOT / "test-audio/talk_q1.opus", ROOT / "test-audio/test.opus"]
    segments = [parse_opus_ogg(p.read_bytes()) for p in seg_files]
    for p, seg in zip(seg_files, segments):
        if not seg:
            print(f"FAIL: {p} 无 opus 包")
            return 1

    receipts: list[float] = []
    done = threading.Event()
    reasm = FrameReassembler()

    def on_down(ftype: int, payload: bytes) -> None:
        if ftype != FRAME_TYPE_JSON:
            return
        msg = json.loads(payload.decode("utf-8"))
        print(f"[down] {msg}", flush=True)
        if msg.get("type") == "receipt":
            receipts.append(time.monotonic())
            if len(receipts) >= 2:
                done.set()
        elif msg.get("type") == "error":
            print("[FAIL] 收到 error")
            done.set()

    reasm.on_frame = on_down
    reasm.on_error = lambda r: print(f"[down] 重组异常: {r}")

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                         client_id="app-two-recs-sim")
    client.on_message = lambda _c, _u, m: reasm.feed_packet(bytes(m.payload))
    client.connect("127.0.0.1", 1883, keepalive=60)
    client.subscribe("gaga/down", qos=1)
    client.loop_start()
    time.sleep(0.5)

    def pub(ftype: int, payload: bytes) -> None:
        for pkt in encode_frame(ftype, payload):
            client.publish("gaga/up", pkt, qos=1)

    pub(FRAME_TYPE_JSON, b'{"type":"hello","device":"gaga-sim-2rec","fw":"sim-0.1.0"}')
    time.sleep(0.2)

    for idx, packets in enumerate(segments, 1):
        duration_ms = len(packets) * 20
        # 段1 虚报 +2s 时长：让帧数判不齐、停在静默窗里（真实 BLE 慢传场景），
        # 使段2 的 rec_start 真正命中"旧段还在收尾"的强制 finalize 路径
        declared_ms = duration_ms + 2000 if idx == 1 else duration_ms
        pub(FRAME_TYPE_JSON, b'{"type":"rec_start"}')
        print(f"[up] 段{idx} rec_start（{len(packets)} 包 ≈{duration_ms}ms）", flush=True)
        for p in packets:
            pub(FRAME_TYPE_OPUS, p)
        pub(FRAME_TYPE_JSON,
            json.dumps({"type": "rec_stop", "duration_ms": declared_ms}).encode())
        print(f"[up] 段{idx} rec_stop duration_ms={declared_ms}", flush=True)
        if idx == 1:
            time.sleep(0.5)  # 关键：段1 还在收尾（等终稿）时立即开段2
            print("[up] 0.5s 后立刻开段2（段1 应仍在收尾中）", flush=True)

    print("[up] 两段发完，等两个 receipt（60s 超时）...", flush=True)
    if not done.wait(timeout=60):
        print(f"FAIL: 只收到 {len(receipts)} 个 receipt")
        return 1
    ok = len(receipts) >= 2
    print("PASS: 两段都收到 receipt" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
