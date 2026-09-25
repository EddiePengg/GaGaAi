"""ASR provider 接口（ADR-013：provider 可切换，配置化）。"""
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
