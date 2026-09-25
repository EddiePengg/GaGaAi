"""Google Gemini Live provider（BidiGenerateContent）。

⚠️ 状态：协议实现完整但**未联调**（需 GEMINI_API_KEY 实测）；
默认 provider 仍是 volc（REALTIME_PROVIDER=volc）。

协议要点（官方文档，2026-09-24）：
- 端点 wss://generativelanguage.googleapis.com/ws/
        google.ai.generativelanguage.v1beta.GenerativeService.bidiGenerateContent?key=KEY
- 握手：首帧 {"setup": {"model": "models/gemini-2.0-flash-live-001",
        "generationConfig": {"responseModalities": ["AUDIO"]}, "systemInstruction": …}}
- 上行音频：{"realtimeInput": {"audioChunks": [{"data": b64, "mimeType": "audio/pcm;rate=16000"}]}}
  —— **要 PCM**：设备上行是 Opus 16k/20ms，本模块用 opus_codec 转 PCM。
- 下行：serverContent.modelTurn.parts[].inline.data = b64 PCM 24k
  —— 设备要 ogg_opus：PCM → OpusEncoder → ogg.OggStreamWriter 帧化。
- 归一化事件：Gemini 不带服务端 ASR 文本（input transcription 需开
  "audioTranscriptionInputOptions"——区域可用性受限）与回复文本（带 output
  transcription 需 realTimeInputFeedback? 阶段先不给字幕：asr/reply 事件缺省，
  bridge 自动不 tick 字幕；音频通路是完整的。
  开字幕的升级路径：setup 里开 transcription，在 _translate 补 asr_delta/final）。

依赖：libopus（opus_codec.py ctypes 直调，brew install opus / apt install libopus0）。
"""
from __future__ import annotations

import asyncio
import base64
import json
import logging
import uuid

import websockets

from ..config import Config
from .base import OnEvent, RealtimeProvider, RealtimeProviderError
from ..opus_codec import OpusCodecDecoder, OpusCodecEncoder

log = logging.getLogger("gaga_server.realtime.gemini")

_PCM_RATE_IN = 16000
_PCM_RATE_OUT = 24000
_FRAME_MS = 20


class GeminiProvider(RealtimeProvider):
    name = "gemini"

    def __init__(self, cfg: Config, on_event: OnEvent):
        super().__init__(on_event)
        self.cfg = cfg
        self.ws: websockets.ClientConnection | None = None
        self.session_id = ""
        self._closed = asyncio.Event()
        self._setup_done = asyncio.Event()
        # PCM 转码（ctypes 直调 libopus，与 step.py 共用 opus_codec）
        self._dec = OpusCodecDecoder(fs=_PCM_RATE_IN, channels=1)
        self._enc = OpusCodecEncoder(fs=_PCM_RATE_OUT, channels=1)

    async def connect(self) -> None:
        key = self.cfg.gemini_api_key
        if not key:
            raise RealtimeProviderError("GEMINI_API_KEY 未配置")
        url = (f"wss://generativelanguage.googleapis.com/ws/"
               f"google.ai.generativelanguage.v1beta.GenerativeService"
               f".bidiGenerateContent?key={key}")
        self.ws = await websockets.connect(url, open_timeout=15,
                                            max_size=2**22)
        self._closed = asyncio.Event()
        self._setup_done = asyncio.Event()
        await self._send({
            "setup": {
                "model": self.cfg.gemini_model,
                "generationConfig": {"responseModalities": ["AUDIO"]},
                "systemInstruction": {
                    "parts": [{"text": self.cfg.realtime_instructions}],
                },
            },
        })
        await asyncio.wait_for(self._setup_done.wait(), timeout=10)
        self.session_id = str(uuid.uuid4())  # Gemini 无会话 id，用本地标识
        self.on_event({"kind": "session_created", "session": self.session_id})
        log.info("gemini live 已连接（model=%s）", self.cfg.gemini_model)

    # ---- 上行：Opus → PCM → realtimeInput.audioChunks ----

    async def send_audio(self, opus_packet: bytes) -> None:
        pcm = self._dec.decode(opus_packet)
        await self._send({
            "realtimeInput": {"audioChunks": [{
                "data": base64.b64encode(bytes(pcm)).decode("ascii"),
                "mimeType": f"audio/pcm;rate={_PCM_RATE_IN}",
            }]},
        })

    async def cancel_response(self) -> None:
        await self._send({"clientContent": {"cancel": True}})

    async def close(self) -> None:
        if self.ws is None:
            return
        try:
            await asyncio.wait_for(self._closed.wait(), timeout=3)
        except asyncio.TimeoutError:
            pass
        finally:
            await self.ws.close()
            self.ws = None

    # ---- 下行：serverContent → 归一化事件 ----

    async def recv_loop(self) -> None:
        assert self.ws is not None
        from array import array
        from ..ogg import OggStreamWriter
        mux = OggStreamWriter(input_sample_rate=_PCM_RATE_OUT)
        header_sent = False
        step = _PCM_RATE_OUT * _FRAME_MS // 1000  # 20ms 帧步进（采样数）
        try:
            async for raw in self.ws:
                try:
                    evt = json.loads(raw)
                except json.JSONDecodeError:
                    continue
                if "setupComplete" in evt:
                    self._setup_done.set()
                    continue
                content = evt.get("serverContent") or {}
                audio_b64 = None
                for p in (content.get("modelTurn") or {}).get("parts") or []:
                    inline = p.get("inlineData") or {}
                    if inline.get("mimeType", "").startswith("audio/pcm"):
                        audio_b64 = inline.get("data")
                if audio_b64:
                    # PCM → Opus 帧（20ms）→ Ogg 页流（设备 demux 吃这个）
                    samples = array("h", base64.b64decode(audio_b64))
                    packets = []
                    for i in range(0, len(samples) - step + 1, step):
                        chunk = samples[i:i + step]
                        packets.append(self._enc.encode(chunk.tobytes()))
                    if packets:
                        page = mux.write_packets(packets)
                        data = page if header_sent else mux.header() + page
                        header_sent = True
                        self.on_event({"kind": "audio_delta", "ogg": data})
                if content.get("turnComplete"):
                    self.on_event({"kind": "usage", "in": None, "out": None})
        finally:
            self._closed.set()

    async def _send(self, event: dict) -> None:
        if self.ws is None:
            raise ConnectionError("gemini websocket 未连接")
        await self.ws.send(json.dumps(event))
