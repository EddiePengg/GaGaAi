"""主动 TTS 语音通知：notify 信令 → 节流音频帧 → notify_end（设备语音播报）。

协议契约（固件侧并行实现，device 字段 = 目标设备 ID，非目标设备忽略）：
  1. {"type":"notify","device":...,"text":"已拍平文本","id":"n_<ms>"} — 通知开始
  2. 若干 type=0x01 音频帧：ogg_opus 24kHz 单声道流按 ~2KB 切段，设备按到达
     顺序拼接成完整 Ogg 流（与 M5 talk 下行音频同格式）；帧间隔 ~60ms
     节流，防 BLE 洪水
  3. {"type":"notify_end","device":...,"id":"n_<ms>"} — 音频流结束
并发保护：同一时刻只允许一路通知音频在发，后来者拒绝（HTTP 层转 429）。
"""
from __future__ import annotations

import logging
import os
import threading
import time
from collections.abc import Callable

from . import tts
from .config import Config
from .textfmt import flatten_markdown

log = logging.getLogger("gaga_server.notify")

_CHUNK_BYTES = 2048       # 单个 0x01 帧 payload 切段大小（~2KB）
# 帧间隔默认 60ms；可用 TTS_FRAME_INTERVAL 环境变量覆盖（调试/链路承压实验用，
# 如 BLE 写泵有丢包时放慢到 0.3s 可绕过突发窗口验证全链路）
_FRAME_INTERVAL = float(os.environ.get("TTS_FRAME_INTERVAL", "0.06"))


class NotifyPusher:
    def __init__(self, cfg: Config,
                 publish_json: Callable[[dict], None],
                 publish_audio: Callable[[bytes], None],
                 downlink: "threading.RLock | None" = None):
        self.cfg = cfg
        self.publish_json = publish_json
        self.publish_audio = publish_audio
        # 下行串行闸门（MqttBridge.downlink）：音频分片流推送期间全程持有，
        # 防止 30s 心跳/receipt 等单包帧插进分片帧中间——分片帧内嵌单包帧
        # 是协议级不可恢复的错位（真机实锤：心跳插进 notify 音频流，设备
        # 解码器 Hunt 误判吞流，只播出 0.55s）
        self.downlink = downlink
        self._lock = threading.Lock()   # 全局单槽：同一时刻只发一路通知音频

    def push(self, device: str, text: str, voice: str | None = None) -> dict:
        """同步入口（HTTP 层放线程池跑）：TTS → notify 信令 → 起 daemon 线程发音频。

        占线（另一路通知音频在发）→ 返回 {"ok": False, "msg": ...}（HTTP 429），
        notify 信令也不发；TTS 失败抛 tts.TTSError（HTTP 502）。
        """
        if not self._lock.acquire(blocking=False):
            return {"ok": False, "msg": "另一个通知正在下发"}
        try:
            flat = flatten_markdown(text)
            wav = tts.synthesize(self.cfg, flat, voice)
            ogg = tts.to_ogg_opus_24k(self.cfg, wav)
            duration_ms = tts.wav_duration_ms(wav)
        except Exception:
            self._lock.release()
            raise
        notify_id = f"n_{int(time.time() * 1000)}"
        self.publish_json({"type": "notify", "device": device,
                           "text": flat, "id": notify_id})
        threading.Thread(target=self._push_audio, daemon=True,
                         name="notify-audio",
                         args=(device, notify_id, ogg)).start()
        return {"ok": True, "id": notify_id, "duration_ms": duration_ms,
                "audio_bytes": len(ogg)}

    def _push_audio(self, device: str, notify_id: str, ogg: bytes) -> None:
        """daemon 线程：ogg 切段发 0x01 帧（节流）→ notify_end。异常只记日志。

        全程持有 downlink 闸门：音频分片流期间禁止心跳/信令插队。"""
        try:
            if self.downlink is not None:
                self.downlink.acquire()
            try:
                for off in range(0, len(ogg), _CHUNK_BYTES):
                    self.publish_audio(ogg[off:off + _CHUNK_BYTES])
                    time.sleep(_FRAME_INTERVAL)
                self.publish_json({"type": "notify_end", "device": device,
                                   "id": notify_id})
                log.info("notify %s 音频下发完成（%d 字节）", notify_id, len(ogg))
            finally:
                if self.downlink is not None:
                    self.downlink.release()
        except Exception as e:
            log.warning("notify %s 音频下发失败: %s", notify_id, e)
        finally:
            self._lock.release()
