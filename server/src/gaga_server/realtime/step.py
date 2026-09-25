"""阶跃星辰 Step Audio 3 Realtime provider（OpenAI Realtime 风格协议）。

⚠️ 状态：协议实现完成，**未真机联调**（需 STEP_API_KEY 实测）；默认 provider
仍是 volc。设备端可在设置里选"对话引擎"（talk_request.provider → bridge）。

协议事实（官方文档 + 2026-09-24 真机联调）：
- 端点 wss://api.stepfun.com/v1/realtime?model=<模型ID>——**model 必须放 URL
  查询参数**，缺席报 400 "model is empty"（session.update 里的 model 字段无效）
- 鉴权：Authorization: Bearer <key>
- 会话：session.update（session.modalities=["text","audio"] / instructions /
  voice / input_audio_format / output_audio_format / turn_detection.server_vad）
- 上行音频：input_audio_buffer.append（audio=base64 **pcm16**，建议 20ms 块）
  —— 设备上行是 Opus 16k/20ms：OpusCodecDecoder 转 PCM（opus_codec.py）
- 下行音频：response.audio.delta（base64 PCM；会话配置 output_audio_format
  =pcm16 24k）——PCM → OpusEncoder → OggStreamWriter 帧化喂设备
- 文本：response.audio_transcript.delta/.done（回复字幕）、
  conversation.item.input_audio_transcription.completed（用户 ASR 终稿，
  **无 started/delta**——asr_started 字幕清屏语义用 input_audio_buffer.
  speech_started 替代，asr 终稿一次性给出）
- 打断：input_audio_buffer.speech_started（服务端 VAD）→ bridge 处理；
  取消：response.cancel
- 会话最长 30 分钟；直接断开即结束（无 session.close 事件）

音色（voice）：stepaudio-3 支持 linjiajiejie 等；STEP_VOICE 配置。
"""
from __future__ import annotations

import asyncio
import base64
import json
import logging
import uuid
from array import array

import websockets

from ..ogg import OggStreamWriter
from ..config import Config
from .base import OnEvent, RealtimeProvider, RealtimeProviderError
from ..opus_codec import OpusCodecDecoder, OpusCodecEncoder

log = logging.getLogger("gaga_server.realtime.step")

_PCM_RATE_IN = 16000
_PCM_RATE_OUT = 24000
_FRAME_MS = 20


class StepProvider(RealtimeProvider):
    name = "step"

    def __init__(self, cfg: Config, on_event: OnEvent):
        super().__init__(on_event)
        self.cfg = cfg
        self.ws: websockets.ClientConnection | None = None
        self.session_id = ""
        self._reply_text = ""
        self._closed = asyncio.Event()
        self._setup_done = asyncio.Event()
        self._recv_task: asyncio.Task | None = None
        # PCM 转码（ctypes 直调 libopus，零第三方依赖——pyogg 0.6.14a1 是
        # 半成品弃用，见 opus_codec.py 头注）
        self._dec = OpusCodecDecoder(fs=_PCM_RATE_IN, channels=1)
        self._enc = OpusCodecEncoder(fs=_PCM_RATE_OUT, channels=1)

    async def connect(self) -> None:
        key = self.cfg.step_api_key
        if not key:
            raise RealtimeProviderError("STEP_API_KEY 未配置")
        self.ws = await websockets.connect(
            f"{self.cfg.step_endpoint}?model={self.cfg.step_model}",
            additional_headers={"Authorization": f"Bearer {key}"},
            open_timeout=15, max_size=2**22,
            proxy=None)  # 国内服务直连；系统 SOCKS 代理缺 python-socks 会炸（同 volc）
        self._closed = asyncio.Event()
        self._setup_done = asyncio.Event()
        # session.created 握手后立刻推（URL 已带 model）——收包循环必须先行，
        # 否则 created/updated 及后续事件在 recv_loop 启动前堆积/丢失
        self._recv_task = asyncio.create_task(self.recv_loop())
        try:
            await asyncio.wait_for(self._setup_done.wait(), timeout=10)
        except asyncio.TimeoutError:
            log.warning("step 会话确认事件 10s 未到（继续，靠收包循环验证）")
        await self._send({
            "type": "session.update",
            "session": {
                "modalities": ["text", "audio"],
                "instructions": self.cfg.realtime_instructions,
                "voice": self.cfg.step_voice,
                "input_audio_format": "pcm16",
                "output_audio_format": "pcm16",
                "turn_detection": {"type": "server_vad"},
            },
        })
        self.session_id = self.session_id or str(uuid.uuid4())
        self.on_event({"kind": "session_created", "session": self.session_id})
        log.info("step realtime 已连接（model=%s voice=%s）",
                 self.cfg.step_model, self.cfg.step_voice)

    # ---- 上行：Opus → PCM16 → input_audio_buffer.append ----

    async def send_audio(self, opus_packet: bytes) -> None:
        pcm = self._dec.decode(opus_packet)
        await self._send({
            "type": "input_audio_buffer.append",
            "audio": base64.b64encode(bytes(pcm)).decode("ascii"),
        })

    async def commit_audio(self) -> None:
        await self._send({"type": "input_audio_buffer.commit"})

    async def cancel_response(self) -> None:
        await self._send({"type": "response.cancel"})

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

    # ---- 下行：Step 事件 → 归一化事件 ----

    async def recv_loop(self) -> None:
        assert self.ws is not None
        mux = OggStreamWriter(input_sample_rate=_PCM_RATE_OUT)
        header_sent = False
        step = _PCM_RATE_OUT * _FRAME_MS // 1000  # 20ms 帧步进（采样数）
        try:
            async for raw in self.ws:
                try:
                    evt = json.loads(raw)
                except json.JSONDecodeError:
                    continue
                etype = evt.get("type", "")
                if etype in ("session.created", "session.updated"):
                    if etype == "session.created":
                        sid = str(evt.get("session", {}).get("id", ""))
                        self.session_id = sid or self.session_id
                    self._setup_done.set()
                elif etype == "input_audio_buffer.speech_started":
                    # 服务端 VAD：新一句用户语音开始（清上一句字幕，防叠字）
                    self.on_event({"kind": "asr_started"})
                elif etype == ("conversation.item."
                               "input_audio_transcription.completed"):
                    text = str(evt.get("transcript")
                               or evt.get("text") or "")
                    log.info("ASR 终稿: %s", text)
                    self.on_event({"kind": "asr_final", "text": text})
                elif etype == "response.audio_transcript.delta":
                    self._reply_text += evt.get("delta", "")
                    self.on_event({"kind": "reply_delta",
                                   "text": self._reply_text})
                elif etype == "response.audio_transcript.done":
                    self._reply_text = (evt.get("text")
                                        or self._reply_text)
                    self.on_event({"kind": "reply_final",
                                   "text": self._reply_text})
                    self._reply_text = ""
                elif etype == "response.audio.delta":
                    b64 = evt.get("delta")
                    if not b64:
                        continue
                    samples = array("h", base64.b64decode(b64))
                    packets = []
                    for i in range(0, len(samples) - step + 1, step):
                        chunk = samples[i:i + step]
                        packets.append(
                            self._enc.encode(chunk.tobytes()))
                    if packets:
                        if not header_sent:
                            # 首块前必须先取 Ogg 头页（write_packets 的硬约束）
                            self.on_event({"kind": "audio_delta",
                                           "ogg": mux.header()})
                            header_sent = True
                        page = mux.write_packets(packets)
                        self.on_event({"kind": "audio_delta", "ogg": page})
                elif etype == "error":
                    # 软错误（如 server_vad 模式下手动 commit 被拒）只记日志；
                    # 会话保持——真致命错误会伴随连接关闭，走 recv 循环自然退出
                    msg = str(evt.get("error", evt))[:300]
                    log.warning("step 软错误（会话保持）: %s", msg)
                elif etype in ("response.done",):
                    self.on_event({"kind": "usage", "in": None, "out": None})
                else:
                    log.debug("step 未处理事件 %s", etype)
        finally:
            self._closed.set()

    async def _send(self, event: dict) -> None:
        if self.ws is None:
            raise ConnectionError("step websocket 未连接")
        event.setdefault("event_id", str(uuid.uuid4()))
        await self.ws.send(json.dumps(event, ensure_ascii=False))
