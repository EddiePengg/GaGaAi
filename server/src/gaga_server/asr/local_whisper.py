"""本地 faster-whisper provider（ADR-013：默认，零 API Key、离线可跑）。

启动时若模型已在本机缓存（~/.cache/huggingface/hub/models--Systran--*），
自动置 HF_HUB_OFFLINE=1 跳过 huggingface.co 在线检查——否则在代理/弱网环境下
snapshot_download 的 repo_info 请求会挂死启动（2026-09-22 实测复现）。
"""
from __future__ import annotations

import logging
import os
from pathlib import Path

import numpy as np

from .base import ASRError

log = logging.getLogger("gaga_server.asr")


def _model_cached(model_size: str) -> bool:
    cache = Path.home() / ".cache" / "huggingface" / "hub" / f"models--Systran--faster-whisper-{model_size}"
    return cache.is_dir() and any(cache.rglob("*.bin"))


class LocalWhisperASR:
    name = "local_whisper"

    def __init__(self, model_size: str = "small", device: str = "cpu",
                 compute_type: str = "int8", language: str = "zh"):
        self.language = language
        if _model_cached(model_size) and not os.environ.get("HF_HUB_OFFLINE"):
            os.environ["HF_HUB_OFFLINE"] = "1"
            log.info("模型已在本地缓存，HF_HUB_OFFLINE=1（跳过在线检查）")
        from faster_whisper import WhisperModel  # 延迟导入：先设好 HF_HUB_OFFLINE
        try:
            self.model = WhisperModel(model_size, device=device,
                                      compute_type=compute_type)
        except Exception as e:  # 模型下载失败等，启动即暴露
            raise ASRError(f"whisper 模型加载失败: {e}") from e

    def transcribe(self, pcm_s16le_16k: bytes) -> str:
        if not pcm_s16le_16k:
            return ""
        audio = (np.frombuffer(pcm_s16le_16k, dtype=np.int16)
                 .astype(np.float32) / 32768.0)
        try:
            segments, _info = self.model.transcribe(
                audio, language=self.language, beam_size=5, vad_filter=True)
            return "".join(seg.text for seg in segments).strip()
        except Exception as e:
            raise ASRError(f"whisper 识别失败: {e}") from e
