"""火山大模型流式 ASR provider（ADR-022）：帧边到达边转发，rec_stop 后快速出终稿。

链路：rec_start 开 ws 会话 → 裸 Opus 包按 200ms 聚组 → OggStreamWriter 增量封装
→ sauc 二进制帧上行（format=ogg codec=opus，实测可行）→ rec_stop/尾帧判齐后
发最后一包（负包）→ 收终稿（负 sequence 响应或 utterances definite=true）。

实测事实（2026-09-23，scripts/asr_stream_probe.py）：
- 端点 wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async（优化版，有变化才回包）
- 鉴权头：X-Api-Key + X-Api-Resource-Id + X-Api-Request-Id + X-Api-Sequence=-1
- 本账号 ASR 2.0（volc.seedasr.*）未授权 403；ASR 1.0（volc.bigasr.sauc.*）已开通
- 终稿信号：优化版终帧 seq 仍为正 → 以 definite=true 为主、负 seq 兜底
- 3.3s 音频（2x 速发）：首文本 ~1.3s，终稿 ~3.4s；判定从最后有效识别包起算
- 上行流 idle ~10s 触发 45000081（等包超时）——录音中途断流超此阈值会话即作废
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
from ..ogg import OggStreamWriter
from . import sauc
from .base import ASRError

log = logging.getLogger("gaga_server.asr.volc_stream")

_DEFAULT_ENDPOINT = ("wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async")
_DEFAULT_RESOURCE_ID = "volc.bigasr.sauc.duration"  # ASR 1.0 小时版（2.0 见 README）


class VolcStreamASR:
    """流式 provider 工厂。transcribe() 不存在——流式会话由 Session 承载。"""

    name = "volc_stream"

    def __init__(self, cfg: Config):
        if not cfg.volcengine_api_key:
            raise ASRError("volc_stream 需要 VOLCENGINE_API_KEY")
        self.cfg = cfg

    def start_session(self,
                      on_final: Callable[..., None],
                      on_error: Callable[..., None],
                      on_definite: Callable[..., None] | None = None,
                      ) -> "VolcStreamSession":
        return VolcStreamSession(self.cfg, on_final, on_error, on_definite)


class VolcStreamSession:
    """一次录音会话 = 一条 sauc ws。线程安全入口：add_packet/finish/cancel；
    所有 ws 操作在自带 asyncio 线程里。幂等结束：回调只触发一次。"""

    def __init__(self, cfg: Config,
                 on_final: Callable[..., None],
                 on_error: Callable[..., None],
                 on_definite: Callable[..., None] | None):
        self.cfg = cfg
        # 回调都带 session 作为第一参数（多段录音并发收尾时区分归属，ADR-022 修正）
        self._on_final = on_final        # (session, text)
        self._on_error = on_error        # (session, msg)
        self._on_definite = on_definite  # (session)
        # 归属状态（session.py 在 finish 时填写）
        self.rec_stop_ts = 0.0
        self.fallback_packets: list[bytes] = []
        self._packets: list[bytes] = []          # 待上行裸 Opus 包（锁保护）
        self._lock = threading.Lock()
        self._finish_expected_ms: float | None = None
        self._opened = threading.Event()         # ws 就绪（会话请求已发）
        self._done = threading.Event()           # 已结束（final/error/cancel）
        self._open_failed: str | None = None
        self._loop: asyncio.AbstractEventLoop | None = None
        self._thread: threading.Thread | None = None
        self.acked_duration_ms = 0               # 服务端确认的音频时长
        self.interim_text = ""

    # ---- 线程安全入口（paho/executor 线程调用）----

    def start(self) -> None:
        self._thread = threading.Thread(target=self._thread_main,
                                        daemon=True, name="volc-asr")
        self._thread.start()

    def add_packet(self, packet: bytes) -> None:
        if self._done.is_set():
            return
        with self._lock:
            self._packets.append(packet)

    def finish(self, expected_duration_ms: float | None = None) -> None:
        """录音结束：发完剩余帧 + 负包，等终稿。"""
        if self._done.is_set():
            return
        self._finish_expected_ms = expected_duration_ms
        loop = self._loop
        if loop is not None:
            loop.call_soon_threadsafe(lambda: None)  # 唤醒 sender 立刻检查
        # sender 循环本来就每 50ms 检查一次 finish 条件，这里只是加速

    def cancel(self) -> None:
        """无条件中止（会话作废，不触发任何回调）。"""
        if self._done.is_set():
            return
        self._on_final = self._on_error = lambda *_: None
        self._on_definite = None
        loop = self._loop
        if loop is not None:
            loop.call_soon_threadsafe(lambda: asyncio.create_task(self._close()))

    # ---- 线程/loop 内部 ----

    def _thread_main(self) -> None:
        self._loop = asyncio.new_event_loop()
        try:
            self._loop.run_until_complete(self._run())
        except Exception as e:
            log.exception("volc_stream 会话异常")
            self._fire_error(f"会话异常: {e}")
        finally:
            self._loop.close()

    async def _run(self) -> None:
        cfg = self.cfg
        headers = {
            "X-Api-Key": cfg.volcengine_api_key,
            "X-Api-Resource-Id": cfg.volc_asr_resource_id,
            "X-Api-Request-Id": str(uuid.uuid4()),
            "X-Api-Sequence": "-1",
            "X-Api-Connect-Id": str(uuid.uuid4()),
        }
        try:
            ws = await websockets.connect(cfg.volc_asr_endpoint,
                                          additional_headers=headers,
                                          proxy=None, open_timeout=10)
        except Exception as e:
            body = getattr(getattr(e, "response", None), "body", None)
            detail = bytes(body).decode("utf-8", "replace") if body else str(e)
            log.warning("volc_stream 建连失败: %s", detail)
            self._fire_error(f"建连失败: {detail}")
            return
        log.info("volc_stream 已连接 logid=%s",
                 ws.response.headers.get("X-Tt-Logid"))
        try:
            await ws.send(sauc.encode_frame(
                sauc.MSG_FULL_CLIENT_REQUEST, sauc.FLAG_NONE,
                sauc.SER_JSON, sauc.COMP_NONE,
                json.dumps({
                    "user": {"uid": "gaga-server"},
                    "audio": {"format": "ogg", "codec": "opus", "rate": 16000,
                              "bits": 16, "channel": 1},
                    "request": {"model_name": "bigmodel", "enable_itn": True,
                                "enable_punc": True, "show_utterances": True},
                }, ensure_ascii=False).encode("utf-8")))
            self._opened.set()
            await asyncio.gather(self._sender(ws), self._receiver(ws))
        except websockets.ConnectionClosed as e:
            log.warning("volc_stream 连接中断: %s", e)
            if not self._done.is_set():
                self._fire_error(f"连接中断: {e}")
        except sauc.SaucError as e:
            log.warning("volc_stream 协议错误: %s", e)
            self._fire_error(str(e))
        finally:
            self._done.set()
            await ws.close()

    async def _sender(self, ws) -> None:
        """200ms 聚组（10 包）增量 Ogg 封装上行；finish 且队列排空后发空负包。

        实测（2026-09-23）：音频组从不带 last 标志，排空后补一个空负载负包
        （FLAG_LAST_NO_SEQ + 空 payload），服务端立即吐出全部终稿（~0.1s）；
        该终包才是低延迟关键，VAD 判停只是兜底。"""
        writer = OggStreamWriter()
        await ws.send(sauc.encode_frame(sauc.MSG_AUDIO_ONLY_REQUEST,
                                        sauc.FLAG_NONE, sauc.SER_NONE,
                                        sauc.COMP_NONE, writer.header()))
        sent_packets = 0
        group: list[bytes] = []
        while True:
            if self._done.is_set():
                return
            with self._lock:
                while self._packets:
                    group.append(self._packets.pop(0))
            finishing = self._finish_expected_ms is not None
            if group and (len(group) >= 10 or finishing):
                await ws.send(sauc.encode_frame(
                    sauc.MSG_AUDIO_ONLY_REQUEST, sauc.FLAG_NONE,
                    sauc.SER_NONE, sauc.COMP_NONE,
                    writer.write_packets(group)))
                sent_packets += len(group)
                group = []
                await asyncio.sleep(0.05)  # 200ms 音频 → 4x 速，远离服务端节奏敏感区
            elif finishing:
                await ws.send(sauc.encode_frame(
                    sauc.MSG_AUDIO_ONLY_REQUEST, sauc.FLAG_LAST_NO_SEQ,
                    sauc.SER_NONE, sauc.COMP_NONE, b""))
                log.info("volc_stream 音频发完（%d 包 = %dms），结束负包已发，等终稿",
                         sent_packets, sent_packets * 20)
                return
            else:
                await asyncio.sleep(0.05)

    async def _receiver(self, ws) -> None:
        while True:
            if self._done.is_set():
                return
            # finish 后 6s 无终稿：有 interim 用 interim 兜底，否则报错走本地降级；
            # 避免静音录音空等火山 45000081（~10s）。
            # 注意 wait_for 超时只在进入本轮时生效，故非 finish 态也给 30s 有界等待，
            # 每轮重查 finishing 标志。
            finishing = self._finish_expected_ms is not None
            try:
                raw = await asyncio.wait_for(ws.recv(),
                                             timeout=6 if finishing else 30)
            except asyncio.TimeoutError:
                if self._done.is_set():
                    return
                if finishing:
                    if self.interim_text:
                        log.warning("volc_stream 终稿超时，用 interim 兜底: %r",
                                    self.interim_text)
                        self._fire_final(self.interim_text)
                    else:
                        self._fire_error("终稿超时（可能全程静音）")
                    return
                continue
            resp = sauc.parse_frame(raw)
            if resp["type"] == "error":
                raise sauc.SaucError(resp["code"], resp["msg"])
            j = resp["json"]
            result = j.get("result") or {}
            text = result.get("text", "")
            if text:
                self.interim_text = text
            dur = j.get("audio_info", {}).get("duration")
            if dur:
                self.acked_duration_ms = dur
            definite = any(u.get("definite") for u in result.get("utterances", []))
            is_final_seq = resp["seq"] is not None and resp["seq"] < 0
            # 单向端点（bigmodel_nostream，ADR-033）以 is_last_package=true 收尾
            is_last_pkg = j.get("is_last_package") is True
            if (definite or is_final_seq or is_last_pkg) \
                    and self._finish_expected_ms is not None:
                log.info("volc_stream 终稿（%s）: %r",
                         "definite" if definite else
                         ("负包" if is_final_seq else "is_last_package"), text)
                self._fire_final(text)
                return
            if definite and self._on_definite is not None:
                self._on_definite(self)

    async def _close(self) -> None:
        self._done.set()

    # ---- 回调（幂等）----

    def _fire_final(self, text: str) -> None:
        if self._done.is_set():
            return
        self._done.set()
        try:
            self._on_final(self, text)
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
