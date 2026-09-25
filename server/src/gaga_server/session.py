"""录音会话装配：信令分派 + Opus 帧累积 + ASR 触发 + talk 会话路由。

时序兼容（重要）：固件链路是 rec_start → 录音 → rec_stop → 音频帧流上行
（esp32/src/main.cpp：Opus 帧"随 rec_stop 上行"；docs/data-model.md §1），
而帧也有可能在 rec_stop 之前到达。服务端不假设顺序：rec_stop 后开一个
静默窗口（REC_FLUSH_TIMEOUT，默认 2s），窗口内收到的帧全部纳入，
窗口结束触发 ASR。单设备阶段全局一个会话（protocol.md §4 备注）。

ASR 两条链路（ADR-022）：
- 流式（volc_stream）：rec_start 即开火山流式会话，帧边到边转发；
  rec_stop 静默窗结束 → finish（预期时长判齐发负包）→ 终稿回调 → 飞书。
  流式任何环节失败 → 自动降级本地 whisper 批处理（帧缓冲全程保留）。
- 批处理（local_whisper 等）：攒齐 → ffmpeg 解码 → transcribe（老路）。

talk 会话（M5，realtime/bridge.py）：活跃期间 Opus 帧直通火山，不进录音缓冲；
talk_request 建会话回 talk_ready，talk_end/退出意图/超时/error 结束回 talk_end。
"""
from __future__ import annotations

import json
import logging
import re
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import TYPE_CHECKING

from .config import Config
from .frames import FRAME_TYPE_JSON, FRAME_TYPE_OPUS
from .ogg import pack_opus_ogg
from .pipeline import Pipeline

if TYPE_CHECKING:
    from .asr.volc_stream import VolcStreamASR, VolcStreamSession
    from .realtime.bridge import TalkBridge

log = logging.getLogger("gaga_server.session")


class SessionManager:
    def __init__(self, cfg: Config, pipeline: Pipeline, publish_json,
                 talk: "TalkBridge | None" = None,
                 stream_asr: "VolcStreamASR | None" = None):
        self.cfg = cfg
        self.pipeline = pipeline
        self.publish_json = publish_json  # callable(dict)：下行 JSON 信令
        self.talk = talk                  # None = realtime 未启用（未配 key 等）
        self.stream_asr = stream_asr      # None = 批处理模式（默认 local_whisper）
        self.device = ""
        self.fw = ""
        self._buf: list[bytes] = []
        self._recording = False
        self._flush_timer: threading.Timer | None = None
        self._lock = threading.Lock()
        # 流式会话状态（全部只在 _lock 下读写）
        self._stream: "VolcStreamSession | None" = None
        self._stream_failed = False
        self._stream_packets: list[bytes] = []   # 降级本地兜底用
        self._rec_stop_ts = 0.0
        self._rec_duration_ms: int | None = None
        self._archive_path: Path | None = None  # 本段录音的存档文件（识别后重命名）
        # pipeline 串行执行：whisper 是 CPU 密集，并发只会互相拖慢
        self._executor = ThreadPoolExecutor(max_workers=1,
                                            thread_name_prefix="pipeline")

    # ---- 帧入口（MQTT 线程调用）----

    def handle_frame(self, frame_type: int, payload: bytes) -> None:
        if frame_type == FRAME_TYPE_OPUS:
            self._on_opus(payload)
        elif frame_type == FRAME_TYPE_JSON:
            try:
                msg = json.loads(payload.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError) as e:
                log.warning("信令 JSON 解析失败: %s", e)
                return
            self._on_signal(msg)
        else:
            log.warning("未知帧类型 0x%02X（%d 字节），丢弃", frame_type, len(payload))

    # ---- 内部 ----

    def _on_opus(self, packet: bytes) -> None:
        # talk 会话活跃期间：Opus 帧直通火山（实时对话），不进录音缓冲
        if self.talk is not None and self.talk.is_active():
            self.talk.send_opus(packet)
            return
        flush_now = False
        with self._lock:
            if self._recording or self._flush_timer is not None:
                self._buf.append(packet)
                stream = self._stream
                # 流式提前收尾：rec_stop 后帧数已覆盖声明时长 → 不等静默窗
                if (stream is not None and self._flush_timer is not None
                        and self._frames_cover_duration()):
                    self._flush_timer.cancel()
                    self._flush_timer = None
                    flush_now = True
            else:
                stream = None
                log.warning("收到 stray Opus 帧（无录音会话），丢弃")
        if stream is not None:
            stream.add_packet(packet)  # 流式：帧边到边转发（ADR-022）
        if flush_now:
            log.info("帧数已覆盖声明时长，跳过静默窗提前收尾")
            self._flush()

    def _on_signal(self, msg: dict) -> None:
        sig = msg.get("type")
        if sig == "hello":
            self.device = str(msg.get("device", ""))
            self.fw = str(msg.get("fw", ""))
            log.info("设备登记: %s fw=%s", self.device, self.fw)
            # hello_ack：纯对时（envelope ts 自动补），设备状态栏时钟开机即校准
            self.publish_json({"type": "hello_ack"})
        elif sig == "rec_start":
            if self.talk is not None and self.talk.is_active():
                self.publish_json({"type": "error", "code": "BUSY",
                                   "msg": "实时对话进行中，不能录音"})
                return
            # 每段录音是独立任务（异步队列语义）：上一段还在收尾就立刻
            # 收尾完成识别投递（流式发结束帧等终稿/批处理立即触发），不作废。
            self._finalize_pending("新 rec_start 到达，上一段立即收尾")
            with self._lock:
                self._buf.clear()
                self._recording = True
                self._stream_failed = False
                self._stream_packets = []
                self._rec_duration_ms = None
            if self.stream_asr is not None:
                stream = self.stream_asr.start_session(
                    on_final=self._on_stream_final,
                    on_error=self._on_stream_error,
                    on_definite=self._on_stream_definite)
                with self._lock:
                    self._stream = stream
                    # rec_start 前已到的帧补进流式会话
                    for pkt in self._buf:
                        stream.add_packet(pkt)
                stream.start()
                log.info("录音开始（device=%s，流式 ASR 会话建立中）",
                         self.device or "未登记")
            else:
                log.info("录音开始（device=%s）", self.device or "未登记")
        elif sig == "rec_stop":
            flush_now = False
            with self._lock:
                self._recording = False
                self._rec_stop_ts = time.monotonic()
                self._rec_duration_ms = msg.get("duration_ms")
                if self._flush_timer is not None:
                    self._flush_timer.cancel()
                # 流式且帧已齐（含 +500ms 在途余量）：跳过静默窗
                if self._stream is not None and self._frames_cover_duration():
                    flush_now = True
                else:
                    # 自适应静默窗（2026-09-25 长录音丢尾修复）：录音越久，
                    # 端到端积压（BLE→手机→MQTT→火山消费）越大，固定 2s 窗
                    # 会把积压的尾巴剪掉（用户实测 30-60s 录音丢最后 10-30s）。
                    # 窗 = 基础 2s + 录音时长每 4 秒加 1 秒，封顶 +20s
                    extra_s = min(int((self._rec_duration_ms or 0) / 4000), 20)
                    self._flush_timer = threading.Timer(
                        self.cfg.rec_flush_timeout + extra_s, self._flush)
                    self._flush_timer.daemon = True
                    self._flush_timer.start()
            if flush_now:
                log.info("录音结束 duration_ms=%s，帧已齐，立即收尾",
                         msg.get("duration_ms"))
                self._flush()
            else:
                log.info("录音结束 duration_ms=%s，静默窗后收尾（帧数 %d ≈ %dms）",
                         msg.get("duration_ms"), len(self._buf),
                         len(self._buf) * 20)
        elif sig == "talk_request":
            if self.talk is None:
                log.info("talk_request 收到（realtime 未启用，回 error）")
                self.publish_json({"type": "error", "code": "NOT_IMPLEMENTED",
                                   "msg": "实时对话未启用（REALTIME_ENABLED/VOLCENGINE_API_KEY）"})
            else:
                # provider 选择权在设备：talk_request.provider（ADR-035），
                # 旧固件不带此字段 → 空串 → 服务端默认
                req_provider = str(msg.get("provider", "") or "")
                log.info("talk_request 收到，建立 realtime 会话（provider=%s）",
                         req_provider or "服务端默认")
                self.talk.start_session(self.device, provider=req_provider)
        elif sig == "talk_end":
            # 设备主动结束实时对话（新增信令，data-model.md §1）
            if self.talk is not None and self.talk.is_active():
                log.info("talk_end 收到（设备主动结束）")
                self.talk.stop("device_end")
        elif sig == "ping":
            log.debug("ping（device=%s）", self.device or "未登记")
        else:
            log.warning("未知信令: %s", msg)

    def _frames_cover_duration(self) -> bool:
        """帧数覆盖 rec_stop 声明时长（+500ms 余量）则视为已齐。锁内调用。

        余量为什么是正的（2026-09-25 用户报"结尾 2 秒不识别"）：rec_stop 是小
        信令，会跑在还在 BLE/手机/MQTT 管道里飞的最后几帧前面；原 -100ms 容差
        会让提前收尾跑赢在途帧 → 尾巴被剪。+500ms = 强制等在途帧落地后再收尾。
        """
        if not self._rec_duration_ms:
            return False
        return len(self._buf) * 20 >= self._rec_duration_ms + 500

    def _finalize_pending(self, reason: str) -> None:
        """立刻收尾上一段录音（若有），不作废：流式发结束帧等终稿，
        批处理立即触发 pipeline。帧归属按时间序天然可分——paho 单线程
        顺序回调，rec_start 之前到达的帧必然已在旧 _buf 里。"""
        with self._lock:
            pending = (self._recording or self._flush_timer is not None
                       or self._stream is not None)
            if self._flush_timer is not None:
                self._flush_timer.cancel()
                self._flush_timer = None
            self._recording = False
        if pending:
            log.info("%s：上一段录音强制收尾", reason)
            self._flush()

    def _flush(self) -> None:
        with self._lock:
            frames = len(self._buf)
            declared = self._rec_duration_ms
        if declared:
            covered = frames * 20
            gap = declared - covered
            level = log.warning if gap > 2000 else log.info
            level("收尾诊断：声明 %dms，实收 %d 帧 ≈ %dms（差 %dms）——"
                  "差值大=设备/BLE 丢帧，差值小=火山消费积压",
                  declared, frames, covered, gap)
        with self._lock:
            self._flush_timer = None
            if not self._buf:
                log.warning("录音会话结束但没有收到音频帧")
                if self._stream is not None:
                    self._stream.cancel()  # 无帧可识别，作废流式会话防泄漏
                    self._stream = None
                return
            packets, self._buf = self._buf, []
            stream, self._stream = self._stream, None
            self._stream_packets = packets
            failed = self._stream_failed
            duration_ms = self._rec_duration_ms
        # 原始录音存档（可回听定责：识别错误时区分"麦糊"还是"模型错"）。
        # 存档路径随会话走（挂 stream 对象 / 随批处理闭包），两段近距录音不串档。
        archive_path = self._archive_recording(packets)
        if stream is not None:
            stream.archive_path = archive_path
        if stream is not None and not failed:
            # 流式：收尾 → finish，终稿走 _on_stream_final 回调。
            # 归属状态挂在会话对象上（多段并发收尾时不错串）
            stream.rec_stop_ts = self._rec_stop_ts
            stream.fallback_packets = packets
            # 旧段没来得及 rec_stop 就被新段顶掉时 duration_ms 是 None，
            # 用实收帧数当预期时长（finish 只需要非 None 触发收尾语义）
            log.info("流式 ASR finish（%d 包，预期 %sms）",
                     len(packets), duration_ms or len(packets) * 20)
            stream.finish(expected_duration_ms=duration_ms or len(packets) * 20)
        else:
            log.info("触发批处理 pipeline: %d 个 Opus 包", len(packets))
            self._executor.submit(self._run, packets, archive_path)

    # ---- 录音存档（ADR-037）：原始 Opus 打包成 .ogg 落盘，识别后带文本重命名 ----

    def _archive_recording(self, packets: list[bytes]) -> Path | None:
        """本段录音 → {存档目录}/20260924-091505-gaga-01.ogg。
        存的就是设备发来的原始 Opus（无重编码），任何播放器可放。
        存档目录配 RECORDING_ARCHIVE_DIR（空 = 关闭存档）。"""
        archive_dir = self.cfg.recording_archive_dir
        if not archive_dir or not packets:
            return None
        try:
            dir_path = Path(archive_dir)
            dir_path.mkdir(parents=True, exist_ok=True)
            ts = time.strftime("%Y%m%d-%H%M%S")
            device = re.sub(r"[^\w-]", "", self.device) or "unknown"
            path = dir_path / f"{ts}-{device}.ogg"
            path.write_bytes(pack_opus_ogg(packets))
            log.info("录音已存档: %s（%d 包）", path, len(packets))
            return path
        except OSError as e:
            log.warning("录音存档失败（不影响识别链路）: %s", e)
            return None

    @staticmethod
    def _rename_archive(path: Path | None, text: str) -> None:
        """识别完成后把转写摘要并进文件名：回听时不用点开就知道哪段是哪句。"""
        if path is None or not text:
            return
        snippet = re.sub(r"[\\/:*?\"<>|\s]+", "", text)[:24]  # 文件名非法字符清理
        new_path = path.with_name(f"{path.stem}-{snippet}{path.suffix}")
        try:
            path.rename(new_path)
            log.info("存档已重命名: %s", new_path.name)
        except OSError as e:
            log.warning("存档重命名失败: %s", e)

    def _publish_result(self, result, archive_path: Path | None = None) -> None:
        """录音结果统一下行：成功 → receipt（带接入端 msg_id），失败 → error。
        批处理与流式两条链路的收尾共用，保证信令形态一致。
        成功时顺带把存档文件重命名成"带识别文本"的，方便回听定位。"""
        if result.ok:
            self._rename_archive(archive_path, result.text)
            extra = {"msg_id": result.msg_id} if result.msg_id else {}
            self.publish_json({"type": "receipt", "text": result.text, **extra})
        else:
            self.publish_json({"type": "error", "code": result.code,
                               "msg": result.msg})

    def _run(self, packets: list[bytes], archive_path: Path | None = None) -> None:
        """本地批处理路径（默认链路 + 流式失败的降级兜底）。"""
        result = self.pipeline.process_opus_packets(packets, device=self.device)
        self._publish_result(result, archive_path)

    # ---- 流式回调（volc-asr 线程触发；带 session 参数区分多段并发收尾）----

    def _on_stream_final(self, stream: "VolcStreamSession", text: str) -> None:
        latency = (time.monotonic() - stream.rec_stop_ts
                   if stream.rec_stop_ts else -1)
        log.info("流式 ASR 终稿: %r（rec_stop→终稿 %.2fs）", text, latency)
        result = self.pipeline.deliver_text(text, device=self.device)
        self._publish_result(result, getattr(stream, "archive_path", None))

    def _on_stream_definite(self, stream: "VolcStreamSession") -> None:
        """火山已判停（definite）且本地已 rec_stop：跳过静默窗立即收尾。

        definite 意味着火山听到了完整句子结尾—— rec_stop 之后到达的尾巴帧
        只是残响静音，等满静默窗只是白等（ADR-022 延迟优化）。
        只响应当前会话：旧会话迟到的 definite 不得 shortcut 新录音的窗口。
        """
        with self._lock:
            if (stream is not self._stream or self._flush_timer is None):
                return  # 旧会话迟到 / 录音中（rec_stop 未发）：不动
            self._flush_timer.cancel()
            self._flush_timer = None
        log.info("火山 definite 判停，跳过静默窗提前收尾")
        self._flush()

    def _on_stream_error(self, stream: "VolcStreamSession", msg: str) -> None:
        with self._lock:
            is_current = stream is self._stream
            if is_current:
                # 当前会话录音中途失败：帧还在 _buf 里累积，收尾时走批处理
                self._stream = None
                self._stream_failed = True
                mid_recording = self._recording or self._flush_timer is not None
            else:
                mid_recording = False
        if is_current and mid_recording:
            log.warning("流式 ASR 失败（%s），录音仍在进行，攒帧待降级", msg)
            return
        # 收尾后失败（或旧会话迟到失败）：用该会话自己的帧降级本地 whisper
        packets = stream.fallback_packets
        archive_path = getattr(stream, "archive_path", None)
        log.warning("流式 ASR 失败（%s），降级本地 whisper（%d 包）",
                    msg, len(packets))
        if packets:
            self._executor.submit(self._run, packets, archive_path)
        elif is_current:
            # 当前会话收尾前失败且无帧可降级（flush 已把帧交给 stream）
            with self._lock:
                packets = self._stream_packets
            if packets:
                self._executor.submit(self._run, packets, archive_path)
            else:
                self.publish_json({"type": "error", "code": "ASR_FAIL",
                                   "msg": f"流式 ASR 失败且无本地音频可降级: {msg}"})
        else:
            log.warning("旧会话迟到失败且无帧可降级（可能已终稿），忽略: %s", msg)
