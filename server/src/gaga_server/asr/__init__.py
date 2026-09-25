"""ASR provider 工厂 + 流式路由（ADR-013 / ADR-022 / ADR-042 / ADR-043）。

两条 ASR 链路：
- 流式（消息模式主路）：StreamASRRouter 按 registry 当前选择（qwen /
  volc_stream）建会话，帧边到边转发；选 off 或工厂不可用（缺 key 等）
  时 start_session 返回 None = 走批处理。
- 批处理（create_asr / create_local_fallback）：整段 PCM → transcribe，
  兼作流式失败的降级兜底 + HTTP 调试口。
"""
from __future__ import annotations

import logging
import os
import threading
from typing import TYPE_CHECKING

from .base import ASRProvider, ASRError
from .local_whisper import LocalWhisperASR
from .volcengine import VolcengineASR

if TYPE_CHECKING:
    from ..config import Config
    from ..providers import ProviderRegistry

log = logging.getLogger("gaga_server.asr")


def create_asr(cfg: "Config") -> ASRProvider:
    if cfg.asr_provider == "local_whisper":
        return LocalWhisperASR(
            model_size=cfg.whisper_model,
            device=cfg.whisper_device,
            compute_type=cfg.whisper_compute_type,
            language=cfg.whisper_language,
        )
    if cfg.asr_provider == "volcengine":
        return VolcengineASR(
            app_id=os.environ.get("VOLC_APP_ID", ""),
            access_token=os.environ.get("VOLC_ACCESS_TOKEN", ""),
            cluster=os.environ.get("VOLC_CLUSTER", "volcengine_input_cn"),
        )
    raise ValueError(f"未知 ASR_PROVIDER: {cfg.asr_provider!r}"
                     "（批处理可选 local_whisper / volcengine；"
                     "流式 qwen / volc_stream 走 StreamASRRouter）")


def create_local_fallback(cfg: "Config") -> ASRProvider:
    """流式模式的本地兜底 + HTTP 调试口用（流式时 pipeline 里挂它）。"""
    return LocalWhisperASR(
        model_size=cfg.whisper_model,
        device=cfg.whisper_device,
        compute_type=cfg.whisper_compute_type,
        language=cfg.whisper_language,
    )


def create_stream_asr(cfg: "Config", registry: "ProviderRegistry") -> "StreamASRRouter | None":
    """流式 ASR 主路（ASR_PROVIDER 是流式名时启用）。"""
    if cfg.asr_provider not in ("qwen", "volc_stream"):
        return None
    return StreamASRRouter(cfg, registry)


class StreamASRRouter:
    """按 registry 当前选择建流式会话；工厂惰性构造并缓存（False=不可用）。"""

    def __init__(self, cfg: "Config", registry: "ProviderRegistry"):
        self._cfg = cfg
        self._registry = registry
        self._factories: dict[str, object] = {}
        self._lock = threading.Lock()

    def peek(self) -> tuple[str, bool]:
        """(当前选择, 是否可用)。启动日志用，顺带预热工厂。"""
        name = self._registry.current("asr")
        return name, self._factory(name) is not None

    def start_session(self,
                      on_final, on_error,
                      on_definite=None):
        """返回流式会话；None = 没有可用流式引擎（session.py 走批处理）。"""
        if self._registry.current("asr") == "off":
            return None
        factory = self._factory(self._registry.current("asr"))
        if factory is None:
            return None
        return factory.start_session(on_final, on_error, on_definite)

    def _factory(self, name: str):
        with self._lock:
            if name in self._factories:
                return self._factories[name]
            try:
                if name == "qwen":
                    from .qwen_message import QwenMessageASR  # noqa: PLC0415
                    factory = QwenMessageASR(self._cfg)
                elif name == "volc_stream":
                    from .volc_stream import VolcStreamASR  # noqa: PLC0415
                    factory = VolcStreamASR(self._cfg)
                else:
                    factory = None
                    log.warning("未知流式 ASR 选择 %r，按批处理走", name)
            except ASRError as e:
                log.warning("流式 ASR %s 不可用（%s），将走批处理兜底", name, e)
                factory = False
            self._factories[name] = factory
            return factory


__all__ = ["ASRProvider", "create_asr", "create_local_fallback",
           "create_stream_asr"]
