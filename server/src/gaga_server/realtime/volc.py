"""豆包实时语音 3.0（Seeduplex 全双工）provider —— 火山引擎 WebSocket 协议。

协议事实（2026-09-23 官方文档实测，probe 见 scripts/realtime_probe.py）：
- 端点 wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue
- 鉴权头：X-Api-Key（+ X-Api-Connect-Id UUID），纯 JSON 文本帧
- 上行：session.create / input_audio_buffer.append（base64 Opus，严格 20ms 实时节奏）
       / input_audio_buffer.commit（强制判停）/ input_audio_mute.commit（关麦保活）
       / response.cancel（打断）/ session.close（优雅关闭，否则服务端报 55000001）
- 下行：session.created / conversation.item.input_audio_transcription.*（ASR 流式）
       / response.output_text.* / response.output_audio.*（delta 是 base64 ogg_opus 分片）
       / response.done / response.canceled / error
- output_audio.done 的 status_code="20000002" = 退出意图（需 session.create 时
  extension.dialog.extra.enable_user_query_exit=true）

ASR 快照合并（实测 2026-09-23）：豆包的 transcription.delta 是**累积快照**而非
增量（同句直接替换）；output_text.delta 是增量。_merge_delta 两种约定都兼容。
"""
from __future__ import annotations

import asyncio
import base64
import json
import logging
import uuid

import websockets

from ..config import Config
from .base import OnEvent, RealtimeProvider

log = logging.getLogger("gaga_server.realtime.volc")

_EXIT_INTENT_CODE = "20000002"

# 万金油工具（ADR-036）：把用户指令转交给 Hermes（经接入端发进对话群），
# 异步拿结果——函数立即返回占位语义，真实结果由 bridge 经 conversation.item.create
# 插回会话（不等它，防止实时对话卡死）。
HERMES_TOOL = {
    "type": "function",
    "name": "send_to_hermes",
    "description": (
        "把用户的执行型指令转交给用户的智能助手 Hermes 处理（智能家居控制、"
        "记录备忘、查系统状态、操作任务等），异步执行。指令要说清做什么，"
        "完成后助手会把结果发回来（由系统自动插入本对话）。"
        "闲聊、天气、百科等你自己能答的不要调用本工具。"
    ),
    "parameters": {
        "type": "object",
        "properties": {
            "text": {
                "type": "string",
                "description": "要交给助手处理的指令原文，一句话说清楚要做什么",
            },
        },
        "required": ["text"],
    },
}


def _parse_function_call(evt: dict) -> dict:
    """从 function_call_arguments.done 事件提取 {call_id, name, args}。

    实测坑（ADR-036）：豆包把字段包在 items[0] 里（顶层同名字段缺省）；
    arguments 可能是标准 JSON / 裸文本 / 缺席，逐级容错，用户指令统一
    归一到 text 键（兼容 query/content/input 等别名）。"""
    items = evt.get("items")
    item = items[0] if isinstance(items, list) and items else evt
    raw = item.get("arguments") or evt.get("arguments")
    try:
        parsed = json.loads(raw) if raw else {}
    except json.JSONDecodeError:
        parsed = {"text": str(raw)}          # 裸文本 → 整段当指令
    if not isinstance(parsed, dict):
        parsed = {"text": str(parsed)}       # JSON 但不是对象（如纯字符串）
    if not parsed.get("text"):
        # 常见别名兜底（query/content/input），保证 text 键必有
        alias = next((parsed[k] for k in ("query", "content", "input")
                      if parsed.get(k)), "")
        parsed = {"text": str(alias)}
    return {
        "call_id": str(item.get("call_id") or evt.get("call_id", "")),
        "name": str(item.get("name") or evt.get("name", "")),
        "args": parsed,
    }


class VolcProvider(RealtimeProvider):
    name = "volc"

    def __init__(self, cfg: Config, on_event: OnEvent):
        super().__init__(on_event)
        self.cfg = cfg
        self.ws: websockets.ClientConnection | None = None
        self.session_id = ""
        self._asr_text = ""
        self._reply_text = ""
        self._closed = asyncio.Event()

    async def connect(self) -> None:
        headers = {
            "X-Api-Key": self.cfg.volcengine_api_key,
            "X-Api-Connect-Id": str(uuid.uuid4()),
        }
        # 绕过系统代理：火山是国内服务，直连；本机 SOCKS 代理缺 python-socks 会炸
        self.ws = await websockets.connect(
            self.cfg.realtime_endpoint, additional_headers=headers,
            proxy=None, open_timeout=15)
        self._closed = asyncio.Event()
        log.info("volc realtime 已连接，logid=%s",
                 self.ws.response.headers.get("X-Tt-Logid"))
        await self._send({
            "type": "session.create",
            "session": {
                "model": self.cfg.realtime_model,
                "instructions": self.cfg.realtime_instructions,
                "audio": {
                    "input": {"format": {"type": "speech_opus", "rate": 16000}},
                    "output": {
                        "format": {"type": "ogg_opus", "rate": 24000},
                        "voice": self.cfg.realtime_voice,
                    },
                },
                "tools": [HERMES_TOOL],
                # extension 内部结构与原 StartSessionPayload 一致（接入必读）：
                # dialog.extra 是对象（老协议 input_mod 等同款写法）
                "extension": {"dialog": {"extra": {"enable_user_query_exit": True}}},
            },
        })

    # ---- 上行 ----

    async def send_audio(self, opus_packet: bytes) -> None:
        await self._send({"type": "input_audio_buffer.append",
                          "audio": base64.b64encode(opus_packet).decode("ascii")})

    async def commit_audio(self) -> None:
        await self._send({"type": "input_audio_buffer.commit"})

    async def send_tool_result(self, call_id: str, text: str) -> None:
        """函数结果回传（ADR-036）：role=tool + 原 call_id，模型随即继续生成。"""
        await self._send_items([{
            "call_id": call_id,
            "role": "tool",
            "content": [{"type": "input_text", "text": text}],
        }])

    async def inject_context(self, text: str) -> None:
        """插入一条用户侧上下文（ADR-036）：Hermes 异步结果回流时用——
        模型把它当刚收到的消息消化（简短口头汇报），不重复原文。"""
        await self._send_items([{
            "type": "message",
            "role": "user",
            "content": [{"type": "input_text", "text": text}],
        }])

    async def _send_items(self, items: list[dict]) -> None:
        """conversation.item.create 统一出口（工具结果回传 / 上下文注入共用）。"""
        await self._send({"type": "conversation.item.create", "items": items})

    async def mute(self) -> None:
        await self._send({"type": "input_audio_mute.commit"})

    async def unmute(self) -> None:
        await self._send({"type": "input_audio_unmute.commit"})

    async def cancel_response(self) -> None:
        await self._send({"type": "response.cancel"})

    async def close(self) -> None:
        if self.ws is None:
            return
        try:
            await self._send({"type": "session.close"})
            await asyncio.wait_for(self._closed.wait(), timeout=5)
        except (asyncio.TimeoutError, websockets.ConnectionClosed):
            pass
        finally:
            await self.ws.close()
            self.ws = None

    # ---- 下行：豆包事件 → 归一化事件 ----

    async def recv_loop(self) -> None:
        assert self.ws is not None
        try:
            async for raw in self.ws:
                try:
                    evt = json.loads(raw)
                except json.JSONDecodeError:
                    log.warning("volc 下行非 JSON 帧（%d 字节），忽略", len(raw))
                    continue
                self._translate(evt)
        finally:
            self._closed.set()

    def _translate(self, evt: dict) -> None:
        etype = evt.get("type", "")
        if etype == "session.created":
            self.session_id = str(evt.get("session", {}).get("id", ""))
            self.on_event({"kind": "session_created", "session": self.session_id})
        elif etype == "session.closed":
            self._closed.set()
        elif etype == "conversation.item.input_audio_transcription.started":
            # 新一句开始：先清 ASR 累积，让 bridge 通知设备清上一句字幕（防叠字）
            self._asr_text = ""
            self.on_event({"kind": "asr_started"})
        elif etype == "conversation.item.input_audio_transcription.delta":
            self._asr_text = self._merge_delta(self._asr_text, evt.get("delta", ""))
            self.on_event({"kind": "asr_delta", "text": self._asr_text})
        elif etype == "conversation.item.input_audio_transcription.completed":
            # 终稿在 text 字段（实测）；流式 delta 有交叠属双跑次正常现象
            self._asr_text = (evt.get("text") or evt.get("transcript")
                              or self._asr_text)
            log.info("ASR 终稿: %s", self._asr_text)
            self.on_event({"kind": "asr_final", "text": self._asr_text})
            self._asr_text = ""
        elif etype == "response.output_text.delta":
            self._reply_text = self._merge_delta(self._reply_text,
                                                 evt.get("delta", ""))
            self.on_event({"kind": "reply_delta", "text": self._reply_text})
        elif etype == "response.output_text.done":
            self._reply_text = evt.get("text") or self._reply_text
            self.on_event({"kind": "reply_final", "text": self._reply_text})
            self._reply_text = ""
        elif etype == "response.output_audio.delta":
            delta = evt.get("delta")
            if delta:
                self.on_event({"kind": "audio_delta",
                               "ogg": base64.b64decode(delta)})
        elif etype == "response.output_audio.done":
            # 退出意图：status_code=20000002（字段位置顶层/嵌套都查）
            status = str(evt.get("status_code")
                         or evt.get("response", {}).get("status_code", ""))
            log.info("output_audio.done status=%s", status or "-")
            if status == _EXIT_INTENT_CODE:
                self.on_event({"kind": "exit_intent"})
        elif etype == "response.function_call_arguments.done":
            # 万金油工具触发（ADR-036）：字段解析与容错见 _parse_function_call
            info = _parse_function_call(evt)
            log.info("function_call name=%s call_id=%s args=%s",
                     info["name"], info["call_id"], str(info["args"])[:200])
            self.on_event({"kind": "function_call", **info})
        elif etype == "response.done":
            usage = evt.get("response", {}).get("usage", {})
            self.on_event({"kind": "usage",
                           "in": usage.get("input_tokens"),
                           "out": usage.get("output_tokens")})
        elif etype == "error":
            self.on_event({"kind": "error",
                           "msg": str(evt.get("message", evt))[:300]})
        else:
            log.debug("volc 未处理事件 %s: %s", etype, str(evt)[:200])

    @staticmethod
    def _merge_delta(current: str, delta: str) -> str:
        """实测（2026-09-23）：ASR delta 是累积快照而非增量（直接拼接会重复），
        output_text.delta 是增量。两种约定都兼容：快照则替换，增量则追加。"""
        if not delta:
            return current
        if current and delta.startswith(current):
            return delta          # 累积快照
        if current and current.startswith(delta):
            return current        # 快照回退（乱序到达），保持现有
        return current + delta    # 真增量

    async def _send(self, event: dict) -> None:
        if self.ws is None:
            raise ConnectionError("volc websocket 未连接")
        event.setdefault("event_id", str(uuid.uuid4()))
        await self.ws.send(json.dumps(event, ensure_ascii=False))
