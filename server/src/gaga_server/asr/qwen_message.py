"""阿里云百炼 Qwen-Audio-3.1-ASR-Flash-Message 流式 ASR（ADR-042）。

选它的原因（对比火山 sauc 单向）：定稿是"生成式后处理"过的——标点、文本归一化
（"4乘4"→"4×4"）、去语气词/润色（disfluency_removal_enabled）、错字更少；
代价是定稿要等服务端整句处理（尾包 ~0.2-0.7s），消息模式完全可接受。

链路（官方 Fun-ASR WebSocket 协议，原生实现不引 SDK）：
    run-task → task-started → 二进制 PCM 帧（100ms 组）→ finish-task
    → result-generated（sentence_end=true 的定稿逐句拼接）→ task-finished
中转事实（手册/实测）：
- 仅支持 16kHz；format=pcm 最稳（opus 需 Ogg 封装页边界，没必要）
- 定稿 = sentence_end=true 的 result-generated；中间结果不开（intermediate_result_enabled）
- task-failed 后连接关闭不可复用；心跳不需要（单次任务短连接）
- WebSocket 地址：百炼业务空间专属域名（wss://{WorkspaceId}.cn-beijing...），
  从 ~/.bailian/config.json 的 base_url 推导，DASHSCOPE_WS_URL 可覆盖
- API Key：DASHSCOPE_API_KEY 优先，回落 ~/.bailian/config.json（不明文进仓库）

接口与 volc_stream 完全同构（start_session/on_final/on_error + 归属属性），
SessionManager 零改动切换；on_definite 不触发——千问的句级定稿不代表说完，
提前收尾交给帧数覆盖检查（session.py）。
"""
from __future__ import annotations

import asyncio
import json
import logging
import threading
import uuid
from collections.abc import Callable

import websockets

from ..config import Config
from ..opus_codec import OpusCodecDecoder
from .base import ASRError

log = logging.getLogger("gaga_server.asr.qwen_message")

_PCM_RATE = 16000
_GROUP_MS = 100          # 上行聚组：100ms PCM（手册建议包大小 1~16KB）
_GROUP_BYTES = _GROUP_MS * _PCM_RATE * 2 // 1000   # 3200 字节
_START_TIMEOUT = 10.0    # 等 task-started
_TAIL_TIMEOUT = 8.0      # finish 后等定稿的上限（超时走兜底/降级）
_IDLE_TIMEOUT = 30.0     # 非收尾态的有界等待（每轮重查 finishing）


class QwenMessageASR:
    """流式 provider 工厂：需要 DASHSCOPE_API_KEY（或 ~/.bailian/config.json）。"""

    name = "qwen"

    def __init__(self, cfg: Config):
        if not cfg.dashscope_api_key:
            raise ASRError(
                "qwen 需要 DASHSCOPE_API_KEY（或 ~/.bailian/config.json 的 api_key）")
        if not cfg.dashscope_ws_url:
            raise ASRError(
                "qwen 需要 WebSocket 地址：DASHSCOPE_WS_URL 或 ~/.bailian/config.json 的 base_url")
        self.cfg = cfg

    def start_session(self,
                      on_final: Callable[..., None],
                      on_error: Callable[..., None],
                      on_definite: Callable[..., None] | None = None,
                      ) -> "QwenMessageSession":
        return QwenMessageSession(self.cfg, on_final, on_error)


class QwenMessageSession:
    """一次录音会话 = 一条百炼 ws。线程安全入口：add_packet/finish/cancel；
    ws 操作在自带 asyncio 线程里。回调只触发一次（final 或 error）。"""

    def __init__(self, cfg: Config,
                 on_final: Callable[..., None],
                 on_error: Callable[..., None]):
        self.cfg = cfg
        self._on_final = on_final  # (session, text)：task-finished 后的整段定稿
        self._on_error = on_error  # (session, msg)
        # 归属状态（session.py 在 finish 时填写，与 volc_stream 同名同义）
        self.rec_stop_ts = 0.0
        self.fallback_packets: list[bytes] = []
        self._dec = OpusCodecDecoder()   # 裸 Opus 包 → PCM 16k（qwen 仅收 16k）
        self._packets: list[bytes] = []  # 待上行裸 Opus 包（锁保护）
        self._pcm: bytes = b""           # 已解码未凑满一组的 PCM 尾巴
        self._lock = threading.Lock()
        self._finish_requested = False
        self._task_id = ""
        self._texts: list[str] = []      # 逐句定稿拼接
        self._started = asyncio.Event()  # task-started 已收
        self._done = threading.Event()
        self._loop: asyncio.AbstractEventLoop | None = None

    # ---- 线程安全入口（paho/executor 线程调用）----

    def start(self) -> None:
        self._thread = threading.Thread(target=self._thread_main,
                                        daemon=True, name="qwen-asr")
        self._thread.start()

    def add_packet(self, packet: bytes) -> None:
        if self._done.is_set():
            return
        with self._lock:
            self._packets.append(packet)

    def finish(self, expected_duration_ms: float | None = None) -> None:
        """录音结束：排空队列 → 发 finish-task → 等 task-finished 的定稿。"""
        if self._done.is_set():
            return
        with self._lock:
            self._finish_requested = True
        loop = self._loop
        if loop is not None:
            loop.call_soon_threadsafe(lambda: None)

    def cancel(self) -> None:
        """无条件中止（会话作废，不触发任何回调）。"""
        if self._done.is_set():
            return
        self._on_final = self._on_error = lambda *_: None
        loop = self._loop
        if loop is not None:
            loop.call_soon_threadsafe(lambda: asyncio.create_task(self._shutdown()))

    # ---- 线程/loop 内部 ----

    def _thread_main(self) -> None:
        self._loop = asyncio.new_event_loop()
        try:
            self._loop.run_until_complete(self._run())
        except Exception as e:
            log.exception("qwen 会话异常")
            self._fire_error(f"会话异常: {e}")
        finally:
            self._loop.close()

    async def _run(self) -> None:
        headers = {"Authorization": f"Bearer {self.cfg.dashscope_api_key}"}
        try:
            ws = await websockets.connect(self.cfg.dashscope_ws_url,
                                          additional_headers=headers,
                                          proxy=None, open_timeout=10,
                                          max_size=2**22)
        except Exception as e:
            log.warning("qwen 建连失败: %s", e)
            self._fire_error(f"建连失败: {e}")
            return
        try:
            self._task_id = str(uuid.uuid4())
            await self._send(ws, "run-task", {
                "task_group": "audio", "task": "asr", "function": "recognition",
                "model": self.cfg.qwen_asr_model,
                "parameters": {
                    "format": "pcm", "sample_rate": _PCM_RATE,
                    # 润色/去语气词/文本归一化——换千问的核心诉求（ADR-042）
                    "disfluency_removal_enabled": True,
                },
                "input": {},
            })
            await asyncio.gather(self._sender(ws), self._receiver(ws))
        except websockets.ConnectionClosed as e:
            log.warning("qwen 连接中断: %s", e)
            if not self._done.is_set():
                self._fire_error(f"连接中断: {e}")
        finally:
            self._done.set()
            await ws.close()

    async def _send(self, ws, action: str, payload: dict) -> None:
        await ws.send(json.dumps({
            "header": {"action": action, "task_id": self._task_id,
                       "streaming": "duplex"},
            "payload": payload,
        }, ensure_ascii=False))

    async def _sender(self, ws) -> None:
        """Opus 解 PCM → 100ms 聚组上行；finish 且队列排空后发 finish-task。"""
        await asyncio.wait_for(self._started.wait(), timeout=_START_TIMEOUT)
        sent_ms = 0
        while True:
            group: list[bytes] = []
            with self._lock:
                while self._packets:
                    group.append(self._packets.pop(0))
                finishing = self._finish_requested
            for pkt in group:
                self._pcm += self._dec.decode(pkt)
            while len(self._pcm) >= _GROUP_BYTES:
                await ws.send(self._pcm[:_GROUP_BYTES])
                self._pcm = self._pcm[_GROUP_BYTES:]
                sent_ms += _GROUP_MS
            if finishing:
                if self._pcm:  # 不足一组的尾巴也发掉
                    await ws.send(self._pcm)
                    sent_ms += len(self._pcm) // 2 // _PCM_RATE * 1000
                    self._pcm = b""
                await self._send(ws, "finish-task", {"input": {}})
                log.info("qwen 音频发完（%dms），finish-task 已发，等定稿", sent_ms)
                return
            await asyncio.sleep(0.05)  # ~4x 速，与 volc_stream 同节奏

    async def _receiver(self, ws) -> None:
        while True:
            finishing = self._finish_requested
            try:
                raw = await asyncio.wait_for(
                    ws.recv(), timeout=_TAIL_TIMEOUT if finishing else _IDLE_TIMEOUT)
            except asyncio.TimeoutError:
                if self._done.is_set():
                    return
                if finishing:
                    if self._texts:
                        log.warning("qwen task-finished 超时，用已收定稿兜底")
                        self._fire_final()
                    else:
                        self._fire_error("定稿超时（可能全程静音）")
                    return
                continue
            try:
                evt = json.loads(raw)
            except json.JSONDecodeError:
                continue
            header, payload = evt.get("header") or {}, evt.get("payload") or {}
            action = header.get("action") or header.get("event")
            if action == "task-started":
                self._started.set()
            elif action == "task-failed":
                self._fire_error(f"{header.get('error_code')}: "
                                 f"{header.get('error_message')}")
                return
            elif action == "result-generated":
                sentence = (payload.get("output") or {}).get("sentence") or {}
                if sentence.get("heartbeat"):
                    continue
                if sentence.get("sentence_end") and sentence.get("text"):
                    self._texts.append(sentence["text"])
                    usage = payload.get("usage") or {}
                    log.info("qwen 句定稿 #%d: %r（计费 %ss）",
                             len(self._texts), sentence["text"],
                             usage.get("duration", "?"))
            elif action == "task-finished":
                self._fire_final()
                return

    async def _shutdown(self) -> None:
        self._done.set()

    # ---- 回调（幂等）----

    def _fire_final(self, text: str | None = None) -> None:
        if self._done.is_set():
            return
        self._done.set()
        try:
            self._on_final(self, text if text is not None else "".join(self._texts))
        except Exception:
            log.exception("on_final 回调异常")

    def _fire_error(self, msg: str) -> None:
        if self._done.is_set():
            return
        self._done.set()
        try:
            self._on_error(self, msg)
        except Exception:
            log.exception("on_error 回调异常")
