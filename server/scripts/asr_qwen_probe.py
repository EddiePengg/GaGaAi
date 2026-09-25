"""千问 message 流式 ASR 探针：拿录音存档 .ogg 走真实链路（裸 Opus 包 →
QwenMessageSession → 定稿），验证协议实现 + 量延迟。

用法：
    .venv/bin/python scripts/asr_qwen_probe.py data/recordings/<某条>.ogg [更多.ogg ...]
"""
from __future__ import annotations

import pathlib
import sys
import threading
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "src"))

from gaga_server.asr.qwen_message import QwenMessageASR  # noqa: E402
from gaga_server.config import load_config  # noqa: E402
from gaga_server.ogg import parse_opus_ogg  # noqa: E402

# 上行节奏：每帧间隔（真实设备 ≈ 20ms；4x 速消费用 5ms，与 volc_stream 同档）
PACKET_INTERVAL_S = 0.005


def run_one(cfg, path: pathlib.Path) -> None:
    packets = parse_opus_ogg(path.read_bytes())
    duration_ms = len(packets) * 20
    print(f"\n=== {path.name}（{len(packets)} 包 ≈ {duration_ms / 1000:.1f}s）")

    done = threading.Event()
    outcome: dict = {}

    def on_final(session, text: str) -> None:
        outcome["text"] = text
        outcome["latency"] = time.monotonic() - session.rec_stop_ts \
            if session.rec_stop_ts else -1
        done.set()

    def on_error(session, msg: str) -> None:
        outcome["error"] = msg
        done.set()

    stream = QwenMessageASR(cfg).start_session(on_final, on_error)
    stream.start()
    for pkt in packets:
        stream.add_packet(pkt)
        time.sleep(PACKET_INTERVAL_S)
    stream.rec_stop_ts = time.monotonic()  # 模拟 session.py：松手时刻
    stream.finish(expected_duration_ms=duration_ms)
    if not done.wait(timeout=30):
        print("!! 30s 无结果")
        return
    if "error" in outcome:
        print(f"!! 失败: {outcome['error']}")
        return
    print(f"[定稿] {outcome['text']}"
          f"（松手→定稿 {outcome['latency']:.2f}s，含 5ms/帧上行回放）")


def main() -> None:
    cfg = load_config()
    paths = [pathlib.Path(a) for a in sys.argv[1:]]
    if not paths:
        sys.exit("用法: asr_qwen_probe.py <录音.ogg> [更多.ogg ...]")
    for p in paths:
        run_one(cfg, p)


if __name__ == "__main__":
    main()
