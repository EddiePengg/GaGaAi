"""pipeline 编排：解码(ffmpeg) → ASR → 接入端投递（channel，ADR-030）。

两条入口：
- process_opus_packets：MQTT 链路（裸 Opus 包 → Ogg 封装 → ffmpeg 解 PCM）
- process_audio_bytes：HTTP 调试链路（任意音频文件 → ffmpeg 解 PCM）
两条链在 PCM 之后完全共用。回执由调用方（session/http_api）转成下行信令或 HTTP 响应。
"""
from __future__ import annotations

import logging
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

from .asr.base import ASRError, ASRProvider
from .channels.base import Channel, ChannelError
from .config import Config
from .ogg import pack_opus_ogg

log = logging.getLogger("gaga_server.pipeline")

_FFMPEG_TIMEOUT = 60.0


@dataclass
class PipelineResult:
    ok: bool
    text: str = ""
    code: str = ""   # 错误码（docs/data-model.md §1）：ASR_FAIL / CHANNEL_FAIL
    msg: str = ""
    msg_id: str = ""  # 平台消息 id（接入端返回；receipt 携带，复配预留）


class DecodeError(Exception):
    pass


class Pipeline:
    def __init__(self, cfg: Config, asr: ASRProvider, channel: Channel):
        self.cfg = cfg
        self.asr = asr
        self.channel = channel

    # ---- 解码 ----

    def _ffmpeg_to_pcm(self, src: Path) -> bytes:
        """任意音频文件 → 16kHz 单声道 s16le PCM（ASR 标准输入）。"""
        proc = subprocess.run(
            [self.cfg.ffmpeg_bin, "-hide_banner", "-loglevel", "error",
             "-i", str(src),
             "-f", "s16le", "-acodec", "pcm_s16le", "-ar", "16000", "-ac", "1",
             "pipe:1"],
            capture_output=True, timeout=_FFMPEG_TIMEOUT)
        if proc.returncode != 0:
            raise DecodeError(proc.stderr.decode("utf-8", "replace")[:300].strip())
        return proc.stdout

    def _decode_opus_packets(self, packets: list[bytes]) -> bytes:
        ogg_data = pack_opus_ogg(packets)
        with tempfile.NamedTemporaryFile(suffix=".ogg", delete=False) as f:
            f.write(ogg_data)
            tmp = Path(f.name)
        try:
            return self._ffmpeg_to_pcm(tmp)
        finally:
            tmp.unlink(missing_ok=True)

    # ---- 编排 ----

    def process_opus_packets(self, packets: list[bytes],
                             device: str = "") -> PipelineResult:
        try:
            pcm = self._decode_opus_packets(packets)
        except (DecodeError, ValueError, subprocess.TimeoutExpired) as e:
            log.warning("opus 解码失败: %s", e)
            return PipelineResult(False, code="ASR_FAIL", msg=f"音频解码失败: {e}")
        log.info("解码完成: %d 个 opus 包 → %.1fs PCM",
                 len(packets), len(pcm) / 2 / 16000)
        return self._process_pcm(pcm, device=device)

    def process_audio_bytes(self, data: bytes, suffix: str) -> PipelineResult:
        with tempfile.NamedTemporaryFile(suffix=suffix, delete=False) as f:
            f.write(data)
            tmp = Path(f.name)
        try:
            pcm = self._ffmpeg_to_pcm(tmp)
        except (DecodeError, subprocess.TimeoutExpired) as e:
            log.warning("音频解码失败: %s", e)
            return PipelineResult(False, code="ASR_FAIL", msg=f"音频解码失败: {e}")
        finally:
            tmp.unlink(missing_ok=True)
        return self._process_pcm(pcm)

    def _process_pcm(self, pcm: bytes, device: str = "") -> PipelineResult:
        try:
            text = self.asr.transcribe(pcm)
        except ASRError as e:
            log.warning("ASR 失败: %s", e)
            return PipelineResult(False, code="ASR_FAIL", msg=str(e))
        return self.deliver_text(text, device=device)

    def deliver_text(self, text: str, device: str = "") -> PipelineResult:
        """ASR 文本的公共后半段：空文本拦截 + 接入端投递（发进对话群）。两条 ASR 链路
        （本地批处理 / 火山流式 ADR-022）共用。返回平台 msg_id 供 receipt 复配。"""
        text = text.strip()
        if not text:
            return PipelineResult(False, code="ASR_FAIL", msg="未识别到语音内容")
        try:
            msg_id = self.channel.send_text(
                f"🦆 [{device}] {text}" if device else f"🦆 {text}")
        except ChannelError as e:
            log.warning("接入端投递失败（%s）: %s", self.channel.name, e)
            return PipelineResult(False, code="CHANNEL_FAIL", msg=str(e))
        log.info("已投递 %s（msg_id=%s）: %s", self.channel.name, msg_id, text)
        return PipelineResult(True, text=text, msg_id=msg_id)
