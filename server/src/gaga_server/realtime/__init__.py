"""实时对话 provider 注册表（ADR-034）：设备 ↔ 可插拔 realtime 后端。

选择权在设备（用户拍板）：talk_request 信令带 provider 字段（设备设置里选
"对话引擎"）；信令未带时回落 REALTIME_PROVIDER 环境变量。
    volc    豆包实时语音 3.0（Seeduplex 全双工；ADR-021，已真机联调）
    gemini  Google Gemini Live（BidiGenerateContent；实现完整，待 API key 联调）
    step    阶跃星辰 Step Audio 3 Realtime（stepaudio-3-realtime-preview；
            协议实现完成，未真机联调——ADR-035）
新后端三步接入：
    1. 新文件实现 RealtimeProvider（base.py 的归一化事件契约）
    2. 在本文件注册一行（或直接在 create_provider 加分支）
    3. README/decisions.md 记一笔协议事实
"""
from __future__ import annotations

from collections.abc import Callable

from ..config import Config
from .base import OnEvent, RealtimeProvider, RealtimeProviderError


def create_provider(cfg: Config, on_event: OnEvent,
                    provider_override: str = "") -> RealtimeProvider:
    name = provider_override or cfg.realtime_provider
    if name == "volc":
        from .volc import VolcProvider  # noqa: PLC0415
        return VolcProvider(cfg, on_event)
    if name == "gemini":
        from .gemini import GeminiProvider  # noqa: PLC0415
        return GeminiProvider(cfg, on_event)
    if name == "step":
        from .step import StepProvider  # noqa: PLC0415
        return StepProvider(cfg, on_event)
    raise RealtimeProviderError(
        f"未知 realtime provider={name}（可选：volc / gemini / step）")


__all__ = ["RealtimeProvider", "RealtimeProviderError", "create_provider"]
