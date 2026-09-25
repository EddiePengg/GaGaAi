"""火山引擎一句话识别 provider —— 占位（ADR-013）。

本期（M1）不接真实实现：没有 API Key，见计划"不做的事"。
配置项已预留：VOLC_APP_ID / VOLC_ACCESS_TOKEN / VOLC_CLUSTER。
接入时实现 transcribe()：PCM 16k 单声道 → 火山一句话识别 HTTP API，
文档：https://www.volcengine.com/docs/6561/80816
"""
from __future__ import annotations

from .base import ASRError


class VolcengineASR:
    name = "volcengine"

    def __init__(self, app_id: str = "", access_token: str = "",
                 cluster: str = "volcengine_input_cn"):
        self.app_id = app_id
        self.access_token = access_token
        self.cluster = cluster

    def transcribe(self, pcm_s16le_16k: bytes) -> str:
        raise ASRError("volcengine provider 尚未接入（M1 边界，见 ADR-013）")
