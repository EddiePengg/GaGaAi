"""设备 talk 会话生命周期管理（ADR-021 / ADR-034 provider 化 / ADR-036 工具转发）。

整体职责（按数据流）：
    talk_request → 建连对话引擎（provider，见 realtime/__init__.py）→ talk_ready
    → 设备上行 Opus 帧 ── 20ms 节奏器(pacer) ──→ provider（吸收 BLE/MQTT 抖动）
    → provider 下行归一化事件(base.py) ──→ 音频帧 / ASR·回复字幕 / 工具调用 分发

工具转发（ADR-036）：模型发起 send_to_hermes 函数调用时——
    ① 立即回占位结果（对话不卡死，模型只确认"已转交"）；
    ② 指令经接入端（飞书群）转交 Hermes，msg_id 记入台账（ToolCallTracker）；
    ③ Hermes 的回复回流时按台账路由（引用精确配对 → 时间窗兜底），
       双通道送达：插入会话上下文（尽力而为）+ reply 信令点亮消息卡（可靠）。

结束条件：退出意图 / 设备 talk_end / 空闲·最长超时(watchdog) / error。

线程模型：外部线程（paho/feishu 轮询）调 start_session/send_opus/stop/
on_channel_reply（线程安全入口）；所有 websocket 操作在自带的 asyncio 循环线程里。
单会话（单设备阶段）。
"""
from __future__ import annotations

import asyncio
import logging
import threading
import time
from collections import deque
from collections.abc import Callable

from ..config import Config
from ..textfmt import flatten_markdown
from . import create_provider
from .base import RealtimeProvider

log = logging.getLogger("gaga_server.realtime.bridge")

_FRAME_INTERVAL = 0.020     # 20ms 一包（协议硬性节奏）
_MAX_BACKLOG = 25           # 上行缓冲上限 25 包 = 500ms，超出丢最旧的追平
_MUTE_AFTER = 1.0           # 上行断流 1s 自动 mute 保活（不支持 mute 的后端为空操作）
_TEXT_THROTTLE = 0.3        # talk_reply 流式下行节流（秒）
_ASR_THROTTLE = 1.0         # talk_asr 流式下行节流（秒）——快照是全量重发，太勤会
                            # 挤占同一条 BLE 通道的音频带宽（真机实测：字幕每帧
                            # 数 KB，音频欠载每秒 2 次；2026-09-25 talk 卡顿修复）
_ASR_SNAPSHOT_MAX = 1500    # ASR 快照超过此字节数跳过流式下发（终稿会补）
_TOOL_RESULT_WINDOW = 180   # 无引用 Hermes 回复的兜底配对窗口（秒，ADR-036）
_SESSION_CONNECT_TIMEOUT = 10  # 会话建立等待上限（秒）


class ToolCallTracker:
    """挂起中的工具调用台账（ADR-036）：判定 Hermes 的回复是不是工具结果。

    规则（两条，按优先级）：
      1. 引用配对：回复的 parent/root == 我们发出的指令 msg_id → 精确命中；
      2. 时间窗兜底：回复无任何引用 + 有挂起指令 + 投递未超窗 → 配最旧的
         （实测 Hermes 回复常不带引用，只靠引用链会漏掉真实结果）。

    命中即出账（一次消费）；未命中返回 None，调用方走普通 reply 路径。
    """

    WINDOW_SECONDS = _TOOL_RESULT_WINDOW

    def __init__(self) -> None:
        self._pending: dict[str, tuple[str, float]] = {}  # msg_id → (call_id, 投递时刻)

    def register(self, msg_id: str, call_id: str) -> None:
        """指令已发进群：记下它的平台 msg_id 等回流。"""
        if msg_id:
            self._pending[msg_id] = (call_id, time.time())

    def match(self, parent_id: str, root_id: str) -> str | None:
        """尝试把一条回复配到某次工具调用。命中即出账并返回 call_id。"""
        # 规则 1：引用精确配对（parent / root 任一命中）
        for key in (parent_id, root_id):
            if key and key in self._pending:
                call_id, _ = self._pending.pop(key)
                return call_id
        # 规则 2：无引用 + 有挂起 + 未超窗 → 配最旧的
        if not parent_id and not root_id and self._pending:
            oldest = min(self._pending, key=lambda k: self._pending[k][1])
            call_id, dispatched_at = self._pending.pop(oldest)
            age = time.time() - dispatched_at
            if age <= self.WINDOW_SECONDS:
                log.info("工具结果按时间窗兜底配对（无引用，窗内 %.0fs）", age)
                return call_id
            # 超窗：把台账还原（min/pop 已取走），当作未命中
            self._pending[oldest] = (call_id, dispatched_at)
        return None


class TalkBridge:
    def __init__(self, cfg: Config,
                 publish_json: Callable[[dict], None],
                 publish_audio: Callable[[bytes], None],
                 send_command: Callable[[str], str] | None = None,
                 registry=None):
        """publish_json/publish_audio：下行信令与音频（→ gaga/down）。
        send_command（ADR-036）：工具指令 → 接入端（飞书群）→ 返回平台 msg_id；
        None = 接入端未就绪，函数调用回"通道不可用"。
        registry（ADR-043）：provider 登记处——设备没指定引擎时查它的当前值
        （HTTP 运行时切换 > 环境变量）；None = 直接用环境变量默认。"""
        self.cfg = cfg
        self.publish_json = publish_json
        self.publish_audio = publish_audio
        self._send_command = send_command
        self._registry = registry
        self._tools = ToolCallTracker()

        # asyncio 循环线程：所有 websocket 操作都跑在这里
        self._loop = asyncio.new_event_loop()
        self._thread = threading.Thread(target=self._loop.run_forever,
                                        daemon=True, name="realtime-loop")
        self._thread.start()

        self._queue: deque[bytes] = deque()       # 上行帧队列（pacer 消费）
        self._queue_lock = threading.Lock()
        self._active = threading.Event()          # 会话占位标记（防并发 start）
        self._provider: RealtimeProvider | None = None
        self._stopping = False
        self._stop_reason = "device_end"
        self._last_activity = 0.0                 # watchdog：最近一次下行事件
        self._last_uplink_ts = 0.0                # watchdog：最近一次上行帧（0=还没开始流）
        self._started_at = 0.0                    # watchdog：会话开始时刻
        self._session_created = asyncio.Event()   # provider 报会话已建立
        self._last_text_pub = {"talk_asr": 0.0, "talk_reply": 0.0}
        # 下行音频节奏统计（延迟分锅用，ADR-034 续）：每秒打一行
        self._audio_bytes = 0
        self._audio_msgs = 0
        self._last_audio_log = 0.0
        self._reply_window = 0.0   # 回复静音窗（loop 时刻前不转发上行帧）：
                                   # 豆包生成期间环境音会触发它的 VAD 自打断，
                                   # 导致 TTS 时断时续（真机节奏日志实锤）

        # 归一化事件 → 处理方法（见 _on_provider_event）
        self._handlers = {
            "session_created": self._on_session_created,
            "asr_started": self._on_asr_started,
            "asr_delta": self._on_asr_delta,
            "asr_final": self._on_asr_final,
            "reply_delta": lambda evt: (self._extend_reply_window(),
                                        self._publish_text("talk_reply", evt.get("text", ""), False)),
            "reply_final": lambda evt: (self._extend_reply_window(),
                                        self._publish_text("talk_reply", evt.get("text", ""), True)),
            "audio_delta": self._on_audio_delta,
            "exit_intent": self._on_exit_intent,
            "function_call": self._on_function_call,
            "usage": self._on_usage,
            "error": self._on_error,
        }

    # ---- 线程安全入口（外部线程调用）----

    def is_active(self) -> bool:
        return self._active.is_set()

    def start_session(self, device: str, provider: str = "") -> None:
        """开始一次实时对话。provider = 设备指定的对话引擎（ADR-035，空 = 服务端默认）。"""
        if self._active.is_set():
            self.publish_json({"type": "error", "code": "BUSY",
                               "msg": "实时对话已在进行中"})
            return
        self._active.set()  # 立即占位，防并发 start；失败路径里清掉
        self._loop.call_soon_threadsafe(
            lambda: asyncio.create_task(self._run(device, provider)))

    def send_opus(self, packet: bytes) -> None:
        """设备上行的一帧 Opus（进队列，由 pacer 按实时节奏发给 provider）。"""
        with self._queue_lock:
            if len(self._queue) >= _MAX_BACKLOG:
                self._queue.popleft()  # 积压超 500ms：丢最旧的追平实时节奏
            self._queue.append(packet)

    def stop(self, reason: str) -> None:
        """结束当前会话（设备主动 / 超时 / 错误）。会话未活跃时无操作。"""
        if self._active.is_set():
            self._loop.call_soon_threadsafe(self._request_stop, reason)

    def on_channel_reply(self, msg_id: str, parent_id: str, root_id: str,
                         text: str) -> bool:
        """接入端回流路由（feishu 轮询线程调用，ADR-036）。

        判定这条回复是不是某次工具调用的结果（规则见 ToolCallTracker）。
        命中后双通道送达：
          ① reply 信令点亮设备消息卡（可靠——豆包全双工无法强制模型开口念
             注入内容，消息卡是用户必然能拿到的结果通道）；
          ② 插入实时会话上下文（尽力而为，模型下一轮或可引用）。
        返回 True = 已按工具结果处理；False = 普通回复（调用方走原路径）。"""
        call_id = self._tools.match(parent_id, root_id)
        if call_id is None:
            return False
        log.info("工具结果回流命中（call_id=%s）: %s", call_id, text[:80])
        self.publish_json({"type": "reply", "text": flatten_markdown(text)})
        if self._active.is_set() and self._provider is not None:
            def inject() -> None:
                asyncio.create_task(self._inject_tool_result(call_id, text))
            self._loop.call_soon_threadsafe(inject)
        return True

    # ---- asyncio 循环线程：会话生命周期 ----

    def _request_stop(self, reason: str) -> None:
        if self._stopping:
            return
        self._stopping = True
        self._stop_reason = reason
        log.info("talk 会话结束请求: %s", reason)
        if self._provider is not None:
            asyncio.create_task(self._provider.close())

    async def _run(self, device: str, provider_name: str = "") -> None:
        """会话主体：建连 → 等就绪 → 起节奏器/看门狗 → 阻塞到会话结束 → 清理。"""
        self._stopping = False
        self._stop_reason = "device_end"
        self._last_activity = self._started_at = self._loop.time()
        self._last_uplink_ts = 0.0
        self._session_created.clear()

        # provider 选择权在设备（talk_request.provider，用户拍板 ADR-035）；
        # 信令没带（旧固件）→ 查登记处当前值（HTTP 切换 > 环境变量，ADR-043）
        if not provider_name and self._registry is not None:
            provider_name = self._registry.current("realtime")
        try:
            provider = create_provider(self.cfg, on_event=self._on_provider_event,
                                       provider_override=provider_name)
        except Exception as e:
            log.warning("对话引擎不可用（%s）: %s",
                        provider_name or self.cfg.realtime_provider, e)
            self.publish_json({"type": "error", "code": "REALTIME_FAIL",
                               "msg": f"对话引擎不可用: {e}"})
            self._active.clear()
            return
        self._provider = provider

        # 建连 + 等会话就绪（provider 上报 session_created）
        recv_task: asyncio.Task | None = None
        try:
            await provider.connect()
            recv_task = asyncio.create_task(provider.recv_loop())
            await asyncio.wait_for(self._session_created.wait(),
                                   timeout=_SESSION_CONNECT_TIMEOUT)
        except Exception as e:
            log.warning("realtime 会话建立失败（provider=%s）: %s",
                        provider_name or self.cfg.realtime_provider, e)
            self.publish_json({"type": "error", "code": "REALTIME_FAIL",
                               "msg": f"实时会话建立失败: {e}"})
            if recv_task is not None:
                recv_task.cancel()
            await provider.close()
            self._provider = None
            self._active.clear()
            return

        # 会话就绪：设备收到 talk_ready 后开始全双工
        self.publish_json({"type": "talk_ready", "session": provider.session_id})
        log.info("talk_ready session=%s device=%s provider=%s",
                 provider.session_id, device, provider.name)
        pacer = asyncio.create_task(self._pacer(provider))
        watchdog = asyncio.create_task(self._watchdog())
        try:
            await recv_task  # 阻塞到会话关闭 / 断连 / 异常
        except Exception as e:
            log.warning("realtime 收包循环异常结束: %s", e)
            if not self._stopping:
                self._stop_reason = "error"
        finally:
            pacer.cancel()
            watchdog.cancel()
            await provider.close()
            self._provider = None
            self._active.clear()
            with self._queue_lock:
                self._queue.clear()
            self.publish_json({"type": "talk_end", "reason": self._stop_reason})
            log.info("talk 会话已清理（reason=%s）", self._stop_reason)

    def _extend_reply_window(self) -> None:
        self._reply_window = self._loop.time() + 5.0

    async def _pacer(self, provider: RealtimeProvider) -> None:
        """上行节奏器：严格 20ms 发一帧；断流 1s 自动 mute 保活，来帧自动 unmute。
        （豆包硬性要求按真实节奏收帧，过快/过慢都报错；关麦不发帧会被判超时。）"""
        muted = False
        last_sent = self._loop.time()
        while True:
            await asyncio.sleep(_FRAME_INTERVAL)
            with self._queue_lock:
                packet = self._queue.popleft() if self._queue else None
            try:
                if packet is not None:
                    if muted:
                        await provider.unmute()
                        muted = False
                        log.info("上行恢复，已 unmute")
                    await provider.send_audio(packet)
                    last_sent = self._loop.time()
                    self._last_uplink_ts = last_sent  # 设备存活心跳（失联判定）
                elif not muted and self._loop.time() - last_sent > _MUTE_AFTER:
                    await provider.mute()
                    muted = True
                    log.info("上行断流 >1s，已 mute 保活")
            except Exception as e:
                log.warning("上行发帧失败: %s", e)
                return

    async def _watchdog(self) -> None:
        """会话保底：空闲超时（无上行帧也无下行事件）、设备失联或到达最长时长则结束。"""
        while True:
            await asyncio.sleep(5)
            now = self._loop.time()
            # 设备失联（2026-09-25）：实时对话中设备每 20ms 就该有一帧上行
            # （静默也有数据帧），8 秒完全没有 = 设备已死（重启/断电/断连）。
            # 不立刻清场的话，孤儿会话会把设备重启后的新 talk_request 顶回 BUSY，
            # 用户被锁在门外直到 120s 空闲超时（真机实测）
            if (self._last_uplink_ts
                    and now - self._last_uplink_ts > 8.0
                    and not self._stopping):
                log.warning("上行帧断流 >8s，判定设备失联，自动结束会话")
                self._request_stop("device_lost")
                return
            if now - self._last_activity > self.cfg.realtime_idle_timeout:
                self._request_stop("timeout")
                return
            if now - self._started_at > self.cfg.realtime_max_duration:
                self._request_stop("timeout_max")
                return

    # ---- 归一化事件分发（provider 线程无关，都在 asyncio 循环线程）----

    def _on_provider_event(self, evt: dict) -> None:
        """provider 归一化事件的唯一入口：刷新活跃时刻后按 kind 分发给处理方法。"""
        self._last_activity = self._loop.time()
        kind = evt.get("kind", "")
        handler = self._handlers.get(kind)
        if handler is None:
            log.debug("realtime 未处理事件 %s", kind)
            return
        handler(evt)

    def _on_session_created(self, evt: dict) -> None:
        self._session_created.set()  # 唤醒 _run 里的就绪等待

    def _on_asr_started(self, evt: dict) -> None:
        # 新一句用户语音开始：立即清设备上一句字幕（防空文本语义，ADR-034。
        # 放开节流——reset 必须即时，否则旧句还会闪一下）
        self.publish_json({"type": "talk_asr", "text": "", "final": False})

    def _on_asr_delta(self, evt: dict) -> None:
        text = evt.get("text", "")
        if len(text) > _ASR_SNAPSHOT_MAX:
            # 病理性大快照（连续长句时豆包的全量重发涨到数 KB）：直接跳过
            # 流式下发，终稿(asr_final)会补——BLE 带宽必须让给音频
            log.info("ASR 快照过大（%d 字节），跳过流式下发", len(text))
            return
        self._publish_text("talk_asr", text, final=False)

    def _on_asr_final(self, evt: dict) -> None:
        log.info("ASR 终稿: %s", evt.get("text", ""))
        self._publish_text("talk_asr", evt.get("text", ""), final=True)

    def _on_audio_delta(self, evt: dict) -> None:
        # 下行音频节奏统计（每秒一行）：分辨"音频慢"的锅在哪——
        # 豆包侧挤（包/秒低=TTS 实时生成节奏）vs 到达突刺（回声引发 VAD 停发）
        now = self._loop.time()
        self._reply_window = now + 5.0  # 回复生成中：静音上行（环境音→VAD 自打断）
        self._audio_bytes += len(evt.get("ogg") or b"")
        self._audio_msgs += 1
        if now - self._last_audio_log >= 1.0:
            log.info("下行音频节奏: %d 包 / %dB / %.1f 秒",
                     self._audio_msgs, self._audio_bytes, now - self._last_audio_log)
            self._audio_bytes = 0
            self._audio_msgs = 0
            self._last_audio_log = now
        ogg = evt.get("ogg")
        if ogg:
            self.publish_audio(ogg)

    def _on_exit_intent(self, evt: dict) -> None:
        log.info("识别到退出意图，结束会话")
        self._request_stop("exit_intent")

    def _on_function_call(self, evt: dict) -> None:
        asyncio.create_task(self._dispatch_tool(evt))

    def _on_usage(self, evt: dict) -> None:
        self._reply_window = 0.0  # 回复结束：解除上行静音
        log.info("一轮交互结束 usage: in=%s out=%s",
                 evt.get("in"), evt.get("out"))

    def _on_error(self, evt: dict) -> None:
        log.warning("realtime error: %s", str(evt.get("msg", ""))[:400])
        self.publish_json({"type": "error", "code": "REALTIME_FAIL",
                           "msg": str(evt.get("msg", ""))[:200]})
        self._request_stop("error")

    def _publish_text(self, kind: str, text: str, final: bool) -> None:
        """字幕信令下发（带节流）。final 恒定直发；流式增量按类型限速。"""
        if not text and not final:
            return  # 空 reset 走 asr_started 分支即时发；这里只放行有内容/终稿
        now = self._loop.time()
        throttle = _ASR_THROTTLE if kind == "talk_asr" else _TEXT_THROTTLE
        if final or now - self._last_text_pub[kind] >= throttle:
            self.publish_json({"type": kind, "text": text, "final": final})
            self._last_text_pub[kind] = now

    # ---- 万金油工具（ADR-036）：指令转发 Hermes + 异步结果回流 ----

    async def _dispatch_tool(self, evt: dict) -> None:
        """函数调用分发三步：
        ① 校验指令（未知工具/空参数 → 明确告知模型"未执行"）；
        ② 经接入端把指令发进群（喂 Hermes），拿到平台 msg_id 记入台账；
        ③ 立即回"受理回执"占位结果——对话不卡死，模型只确认已转交。
        任何一步失败都回传明确的 tool 结果，绝不让模型干等。"""
        call_id = evt.get("call_id", "")
        name = evt.get("name", "")
        text = str((evt.get("args") or {}).get("text", "")).strip()

        if name != "send_to_hermes" or not text:
            await self._return_tool_result(call_id, "未知指令，未执行")
            return
        if self._send_command is None or not self._active.is_set():
            await self._return_tool_result(
                call_id, "转交通道不可用（接入端未就绪或对话已结束）")
            return

        try:
            msg_id = self._send_command(f"[嘎嘎实时指令] {text}")
        except Exception as e:
            log.warning("工具指令投递失败: %s", e)
            await self._return_tool_result(call_id, f"指令发送失败: {e}")
            return

        if not msg_id:
            await self._return_tool_result(
                call_id, "指令已发出，但通道未返回消息标识，可能无法追踪结果。")
            return

        self._tools.register(msg_id, call_id)
        log.info("工具指令已投递 %s（call_id=%s msg_id=%s），等 Hermes 回复",
                 self.cfg.channel, call_id, msg_id)
        await self._return_tool_result(
            call_id,
            "这只是受理回执：指令已转交给助手，任务还没执行完。"
            "请只向用户确认\"已转交、等结果\"，严禁声称任务已完成。")

    async def _return_tool_result(self, call_id: str, text: str) -> None:
        """tool 结果回传给对话引擎（call_id 配对）。会话已结束时静默放弃。"""
        provider = self._provider
        if provider is None or not call_id:
            return
        try:
            await provider.send_tool_result(call_id, text)
        except Exception as e:
            log.warning("tool 结果回传失败（会话可能已结束）: %s", e)

    async def _inject_tool_result(self, call_id: str, text: str) -> None:
        """Hermes 结果插入会话上下文（尽力而为：豆包实测不进上下文也不报错，
        属 provider 差异；OpenAI 风格引擎可正常生效）。"""
        provider = self._provider
        if provider is None:
            return
        try:
            await provider.inject_context(
                f"[助手任务结果回执] {text}\n"
                "（这是刚才转交指令的执行结果，请用一两句话向用户自然汇报，"
                "不要逐字复读）")
        except Exception as e:
            log.warning("工具结果插入会话失败: %s", e)
