"""HTTP 调试口：POST /debug/audio 上传音频文件，走与 MQTT 链路相同的 pipeline；
POST /debug/reply 注入一条 Hermes 回复（Markdown 自动拍平），联调设备 UI 用；
GET/POST /debug/providers 查看/切换各类能力的 provider（ADR-043）。

这是 M1 的 curl 测试入口，也为以后 WebSocket 媒体层留同进程基础（ADR-012）。
"""
from __future__ import annotations

import asyncio
import logging
from pathlib import Path
from typing import TYPE_CHECKING

from fastapi import FastAPI, File, HTTPException, UploadFile
from pydantic import BaseModel

from .config import Config
from .pipeline import Pipeline
from .textfmt import flatten_markdown

if TYPE_CHECKING:
    from .providers import ProviderRegistry

log = logging.getLogger("gaga_server.http")


class ReplyBody(BaseModel):
    text: str


class ProviderBody(BaseModel):
    capability: str  # asr / realtime / channel
    provider: str | None = None  # null/空 = 清除运行时覆盖，回落环境变量


def create_app(cfg: Config, pipeline: Pipeline, publish_json=None,
               registry: "ProviderRegistry | None" = None) -> FastAPI:
    app = FastAPI(title="gaga-server", version="0.1.0")

    @app.get("/healthz")
    def healthz():
        return {"ok": True, "asr": cfg.asr_provider}

    @app.get("/debug/providers")
    def providers():
        """三类能力当前用哪个、候选有哪些、谁说了算（ADR-043）。"""
        if registry is None:
            raise HTTPException(status_code=503, detail="registry 未就绪")
        return registry.describe()

    @app.post("/debug/providers")
    def select_provider(body: ProviderBody):
        """运行时切换（即时生效于下一段录音/talk；channel 改了需重启）。"""
        if registry is None:
            raise HTTPException(status_code=503, detail="registry 未就绪")
        table = registry.describe()
        if body.capability not in table:
            raise HTTPException(status_code=404, detail={
                "msg": f"未知能力 {body.capability!r}",
                "可选": list(table)})
        chosen = body.provider or ""
        options = table[body.capability]["options"]
        if chosen and chosen not in options:
            raise HTTPException(status_code=400, detail={
                "msg": f"{body.capability} 没有 provider {chosen!r}",
                "可选": options})
        registry.set_runtime(body.capability, chosen or None)
        log.info("运行时切换 %s → %s", body.capability,
                 chosen or "（清除覆盖，回落环境变量默认）")
        return registry.describe()

    @app.post("/debug/audio")
    async def debug_audio(file: UploadFile = File(...)):
        data = await file.read()
        if not data:
            raise HTTPException(status_code=400,
                                detail={"code": "ASR_FAIL", "msg": "空文件"})
        suffix = Path(file.filename or "audio.bin").suffix or ".bin"
        log.info("HTTP 上传 %s（%d 字节）", file.filename, len(data))
        # ASR 是 CPU 密集，丢到线程池避免阻塞事件循环
        result = await asyncio.to_thread(pipeline.process_audio_bytes, data, suffix)
        if not result.ok:
            raise HTTPException(status_code=502,
                                detail={"code": result.code, "msg": result.msg})
        return {"ok": True, "text": result.text}

    @app.post("/debug/reply")
    def debug_reply(body: ReplyBody):
        """注入一条回复信令（联调设备消息卡 UI；Markdown 拍平在此生效）。"""
        if publish_json is None:
            raise HTTPException(status_code=503, detail="MQTT bridge 未就绪")
        flat = flatten_markdown(body.text)
        publish_json({"type": "reply", "text": flat})
        return {"ok": True, "text": flat}

    return app
