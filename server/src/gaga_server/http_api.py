"""HTTP 调试口：POST /debug/audio 上传音频文件，走与 MQTT 链路相同的 pipeline；
POST /debug/reply 注入一条 Hermes 回复（Markdown 自动拍平），联调设备 UI 用。

这是 M1 的 curl 测试入口，也为以后 WebSocket 媒体层留同进程基础（ADR-012）。
"""
from __future__ import annotations

import asyncio
import logging
from pathlib import Path

from fastapi import FastAPI, File, HTTPException, UploadFile
from pydantic import BaseModel

from .config import Config
from .pipeline import Pipeline
from .textfmt import flatten_markdown

log = logging.getLogger("gaga_server.http")


class ReplyBody(BaseModel):
    text: str


def create_app(cfg: Config, pipeline: Pipeline, publish_json=None) -> FastAPI:
    app = FastAPI(title="gaga-server", version="0.1.0")

    @app.get("/healthz")
    def healthz():
        return {"ok": True, "asr": cfg.asr_provider}

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
