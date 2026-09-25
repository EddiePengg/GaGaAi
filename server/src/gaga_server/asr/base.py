"""ASR provider 接口（ADR-013：provider 可切换，配置化）。

两种形态：
- 批处理 ASRProvider：transcribe(整段 PCM) -> str（local_whisper / volcengine）。
- 流式 StreamASRFactory：帧边到边转发（qwen / volc_stream），会话接口见下。
"""
from __future__ import annotations

from typing import Protocol, runtime_checkable


class ASRError(Exception):
    """ASR 环节失败（解码后的识别错误；映射到错误码 ASR_FAIL）。"""


@runtime_checkable
class ASRProvider(Protocol):
    name: str

    def transcribe(self, pcm_s16le_16k: bytes) -> str:
        """16kHz 单声道 s16le PCM → 文本。失败抛 ASRError。返回空串表示没识别到内容。"""
        ...


@runtime_checkable
class StreamASRSession(Protocol):
    """一次流式录音会话（qwen_message / volc_stream 的共有接口，结构化鸭子类型）。
    生命周期：start() → add_packet()×N → finish() → 回调 on_final/on_error（各一次）；
    cancel() 无条件作废不回调。归属属性由 session.py 填写（多段并发收尾分锅）。"""
    rec_stop_ts: float
    fallback_packets: list[bytes]

    def start(self) -> None: ...
    def add_packet(self, packet: bytes) -> None: ...
    def finish(self, expected_duration_ms: float | None = None) -> None: ...
    def cancel(self) -> None: ...


@runtime_checkable
class StreamASRFactory(Protocol):
    name: str

    def start_session(self, on_final, on_error, on_definite=None):
        """建一个流式会话；返回 None 表示当前没有可用流式引擎（调用方走批处理）。"""
        ...
