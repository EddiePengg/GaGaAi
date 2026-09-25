#!/usr/bin/env python3
"""模拟设备实时对话（M5 全链路联调）：talk_request → talk_ready → Opus 帧流 →
收下行音频/文本 → 退出意图或 talk_end 收尾。

用法（先起 mosquitto + gaga-server）：
  .venv/bin/python scripts/simulate_talk.py                      # 默认两轮短对话
  .venv/bin/python scripts/simulate_talk.py --skip-exit-test     # 只跑一轮（省钱）

流程：
  轮 1：say 生成"你好，请用一句话介绍你自己" → opus → 20ms 节奏上行 → 收模型回复
  轮 2：say 生成"谢谢，再见，退出对话" → 验证退出意图（20000002 → talk_end）
落盘：test-audio/talk_reply_t1.ogg / t2.ogg → ffmpeg 解码 → 本地 whisper 回识内容。
真实调用火山 API（计费），脚本控制在一两秒一问的两轮短对话。
"""
from __future__ import annotations

import argparse
import base64
import json
import subprocess
import sys
import threading
import time
from pathlib import Path

import paho.mqtt.client as mqtt

from gaga_server.config import load_config
from gaga_server.frames import (FRAME_TYPE_JSON, FRAME_TYPE_OPUS,
                                FrameReassembler, encode_frame)
from gaga_server.ogg import parse_opus_ogg

ROOT = Path(__file__).resolve().parents[1]
AUDIO_DIR = ROOT / "test-audio"


def make_speech(text: str, name: str, tail_silence_s: float = 3.0) -> list[bytes]:
    """macOS say 生成中文语音 → ffmpeg 编码 16k opus → 尾部拼静音 → 裸包列表。"""
    m4a = AUDIO_DIR / f"{name}.m4a"
    opus = AUDIO_DIR / f"{name}.opus"
    if not opus.exists():
        subprocess.run(["say", "-v", "Tingting", text, "-o", str(m4a)], check=True)
        subprocess.run(
            ["/opt/homebrew/bin/ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
             "-i", str(m4a),
             "-f", "lavfi", "-t", str(tail_silence_s), "-i", "anullsrc=r=16000:cl=mono",
             "-filter_complex", "[0:a][1:a]concat=n=2:v=0:a=1",
             "-ar", "16000", "-ac", "1", "-c:a", "libopus", "-b:a", "16k",
             "-frame_duration", "20", str(opus)],
            check=True)
    return parse_opus_ogg(opus.read_bytes())


def decode_and_transcribe(ogg_path: Path) -> str:
    """下行 ogg_opus → 16k PCM → 本地 whisper 回识（客观验证内容是模型回复）。"""
    proc = subprocess.run(
        ["/opt/homebrew/bin/ffmpeg", "-hide_banner", "-loglevel", "error",
         "-i", str(ogg_path), "-f", "s16le", "-acodec", "pcm_s16le",
         "-ar", "16000", "-ac", "1", "pipe:1"],
        capture_output=True)
    if proc.returncode != 0:
        return f"<ffmpeg 解码失败: {proc.stderr.decode()[:200]}>"
    pcm = proc.stdout
    print(f"  [{ogg_path.name}] 解码 {len(pcm) / 2 / 16000:.1f}s PCM")
    if len(pcm) < 3200:  # <0.1s
        return "<音频过短>"
    from gaga_server.asr.local_whisper import LocalWhisperASR
    if not hasattr(decode_and_transcribe, "_asr"):
        decode_and_transcribe._asr = LocalWhisperASR("small")
    return decode_and_transcribe._asr.transcribe(pcm)  # type: ignore[attr-defined]


class TalkSim:
    def __init__(self, args):
        self.args = args
        self.cfg = load_config()
        self.done = threading.Event()       # talk_end 收到
        self.ready = threading.Event()      # talk_ready 收到
        self.failed: list[str] = []
        self.turn = 0
        self.audio_chunks: list[bytes] = []
        self.response_done = threading.Event()
        self.exit_reason = ""

        self.reasm = FrameReassembler()
        self.reasm.on_frame = self._on_down_frame
        self.reasm.on_error = lambda r: print(f"[down] 重组异常: {r}")

        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                                  client_id="app-gaga-sim-talk")
        self.client.on_message = lambda _c, _u, m: self.reasm.feed_packet(bytes(m.payload))

    # ---- 下行 ----

    def _on_down_frame(self, ftype: int, payload: bytes) -> None:
        if ftype == FRAME_TYPE_OPUS:
            self.audio_chunks.append(payload)
            return
        msg = json.loads(payload.decode("utf-8"))
        mtype = msg.get("type")
        if mtype == "talk_ready":
            print(f"[down] talk_ready session={msg.get('session')}")
            self.ready.set()
        elif mtype == "talk_asr":
            print(f"[down] ASR{'(终)' if msg.get('final') else ''}: {msg.get('text')}")
        elif mtype == "talk_reply":
            print(f"[down] 回复{'(终)' if msg.get('final') else ''}: {msg.get('text')}")
        elif mtype == "talk_end":
            print(f"[down] talk_end reason={msg.get('reason')}")
            self.exit_reason = msg.get("reason", "")
            self.done.set()
        elif mtype == "error":
            print(f"[down] error {msg.get('code')}: {msg.get('msg')}")
            self.failed.append(f"{msg.get('code')}: {msg.get('msg')}")
            self.done.set()
        else:
            print(f"[down] {msg}")

    # ---- 上行 ----

    def publish_signal(self, signal: dict) -> None:
        payload = json.dumps(signal, ensure_ascii=False).encode("utf-8")
        for pkt in encode_frame(FRAME_TYPE_JSON, payload):
            self.client.publish(self.args.topic_up, pkt, qos=1)

    def stream_packets(self, packets: list[bytes]) -> None:
        """严格 20ms 节奏上行（模拟设备实时采集）。"""
        start = time.monotonic()
        for i, packet in enumerate(packets):
            for pkt in encode_frame(FRAME_TYPE_OPUS, packet):
                self.client.publish(self.args.topic_up, pkt, qos=1)
            target = start + (i + 1) * 0.020
            delay = target - time.monotonic()
            if delay > 0:
                time.sleep(delay)
        print(f"[up] {len(packets)} 包发完（{len(packets) * 20}ms 音频）")

    # ---- 主流程 ----

    def run(self) -> int:
        self.client.connect(self.args.host, self.args.port, keepalive=60)
        self.client.subscribe(self.args.topic_down, qos=1)
        self.client.loop_start()
        time.sleep(0.5)

        self.publish_signal({"type": "hello", "device": "gaga-sim-talk",
                             "fw": "sim-0.1.0"})
        time.sleep(0.2)
        self.publish_signal({"type": "talk_request"})
        print("[up] talk_request 已发，等 talk_ready...")
        if not self.ready.wait(timeout=15):
            print("FAIL: 15s 未拿到 talk_ready")
            return 1

        turns = [("talk_q1", "你好，请用一句话介绍你自己。")]
        if not self.args.skip_exit_test:
            turns.append(("talk_q2", "拜拜，挂了啊，不用再回复我了。"))

        for idx, (name, text) in enumerate(turns, 1):
            self.turn = idx
            self.audio_chunks = []
            self.response_done.clear()
            print(f"\n===== 轮 {idx}：{text}")
            packets = make_speech(text, name)
            self.stream_packets(packets)
            # 等模型回复：response.done 后下行静默 3s 视为本轮结束
            self._wait_turn_end(timeout=30)
            self._save_turn_audio(name)
            if self.done.is_set():  # 退出意图/error 提前终场
                break

        if not self.done.is_set():
            print("\n[up] 未收到退出意图，主动发 talk_end")
            self.publish_signal({"type": "talk_end"})
            if not self.done.wait(timeout=10):
                print("FAIL: talk_end 后 10s 会话未结束")
                return 1

        self.client.loop_stop()
        return self._summary()

    def _wait_turn_end(self, timeout: float) -> None:
        """下行音频/文本静默 3s 或 talk_end/error 即本轮结束。"""
        deadline = time.monotonic() + timeout
        last_activity = time.monotonic()
        seen_audio = len(self.audio_chunks)
        while time.monotonic() < deadline and not self.done.is_set():
            time.sleep(0.2)
            if len(self.audio_chunks) != seen_audio:
                seen_audio = len(self.audio_chunks)
                last_activity = time.monotonic()
            elif time.monotonic() - last_activity > 3:
                return
        if self.done.is_set():
            return

    def _save_turn_audio(self, name: str) -> None:
        if not self.audio_chunks:
            print(f"  [轮 {self.turn}] 未收到下行音频")
            self.failed.append(f"turn{self.turn}: 无下行音频")
            return
        out = AUDIO_DIR / f"talk_reply_t{self.turn}.ogg"
        out.write_bytes(b"".join(self.audio_chunks))
        print(f"  [轮 {self.turn}] 下行音频 {len(self.audio_chunks)} 块 "
              f"{out.stat().st_size} 字节 → {out.name}")
        text = decode_and_transcribe(out)
        print(f"  [轮 {self.turn}] 下行音频回识: {text}")

    def _summary(self) -> int:
        print("\n===== 汇总")
        print(f"退出方式: {self.exit_reason or '无'}")
        if self.failed:
            print("失败项:", self.failed)
            return 1
        print("PASS")
        return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="模拟设备实时对话（M5 联调）")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--topic-up", default="gaga/up")
    ap.add_argument("--topic-down", default="gaga/down")
    ap.add_argument("--skip-exit-test", action="store_true", help="只跑一轮（省 API 费用）")
    args = ap.parse_args()
    sim = TalkSim(args)
    try:
        return sim.run()
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
