"""主动 TTS：百炼多模态生成 API 合成语音（wav）→ 转 ogg_opus 24kHz 下行音频。

链路：POST /api/v1/services/aigc/multimodal-generation/generation
→ output.audio.url（OSS wav 地址，24h 有效）→ httpx 下载。
注意：qwen-audio-3.1 / 3.0-tts-flash 在本账号报 "url error"（未开通），
默认模型用已实测可用的 qwen3-tts-flash（2026-09-27），做成配置项 TTS_MODEL。
"""
from __future__ import annotations

import logging
import struct
import subprocess

import httpx

from .config import Config

log = logging.getLogger("gaga_server.tts")

_TTS_URL = ("https://dashscope.aliyuncs.com/api/v1/services/"
            "aigc/multimodal-generation/generation")
_MAX_TEXT = 500        # API 限 512 token，中文一字≈一 token，留余量
_HTTP_TIMEOUT = 60.0
_FFMPEG_TIMEOUT = 60.0


class TTSError(Exception):
    """TTS 失败（HTTP 层转 502 TTS_FAIL）。"""


def synthesize(cfg: Config, text: str, voice: str | None = None) -> bytes:
    """文本 → wav bytes。失败抛 TTSError（带服务端 message）。"""
    text = (text or "").strip()[:_MAX_TEXT]
    if not text:
        raise TTSError("空文本")
    body = {
        "model": cfg.tts_model,
        "input": {
            "text": text,
            "voice": voice or cfg.tts_voice,
            "language_type": "Chinese",
        },
    }
    try:
        with httpx.Client(timeout=_HTTP_TIMEOUT) as client:
            resp = client.post(
                _TTS_URL,
                headers={"Authorization": f"Bearer {cfg.dashscope_api_key}"},
                json=body)
            data = resp.json()
            if resp.status_code != 200:
                raise TTSError(str(data.get("message")
                                   or f"HTTP {resp.status_code}")[:300])
            url = data.get("output", {}).get("audio", {}).get("url")
            if not url:
                raise TTSError(str(data.get("message")
                                   or "响应缺 output.audio.url")[:300])
            wav = client.get(url).content
    except httpx.HTTPError as e:
        raise TTSError(f"请求失败: {e}") from e
    if not wav:
        raise TTSError("音频下载为空")
    return wav


def wav_duration_ms(wav: bytes) -> int:
    """RIFF/WAVE 时长（ms）：正经解析 chunk——fmt 拿 byte_rate，data 拿数据量。"""
    if len(wav) < 12 or wav[:4] != b"RIFF" or wav[8:12] != b"WAVE":
        raise TTSError("非 RIFF/WAVE 音频")
    byte_rate = 0
    data_size = 0
    offset = 12
    while offset + 8 <= len(wav):
        chunk_id = wav[offset:offset + 4]
        chunk_size = struct.unpack_from("<I", wav, offset + 4)[0]
        body = offset + 8
        if chunk_id == b"fmt " and chunk_size >= 16:
            byte_rate = struct.unpack_from("<I", wav, body + 8)[0]
        elif chunk_id == b"data":
            # 部分合成器（qwen3-tts-flash 实测）data chunk 长度写成 0x7FFFFFxx
            # 哨兵值，按实际可读字节截断
            data_size = min(chunk_size, len(wav) - body)
        offset = body + chunk_size + (chunk_size & 1)  # chunk 按 2 字节对齐
    if not byte_rate or not data_size:
        raise TTSError("WAV 缺 fmt/data chunk")
    return int(data_size * 1000 / byte_rate)


def to_ogg_opus_24k(cfg: Config, wav: bytes) -> bytes:
    """wav → ogg_opus 24kHz 单声道（与 M5 talk 下行音频同格式，protocol.md §3）。"""
    proc = subprocess.run(
        [cfg.ffmpeg_bin, "-hide_banner", "-loglevel", "error",
         "-f", "wav", "-i", "pipe:0",
         "-ac", "1", "-ar", "24000", "-c:a", "libopus",
         "-b:a", "32k", "-application", "audio",
         "-f", "ogg", "pipe:1"],
        input=wav, capture_output=True, timeout=_FFMPEG_TIMEOUT)
    if proc.returncode != 0 or not proc.stdout:
        raise TTSError(proc.stderr.decode("utf-8", "replace")[:300].strip()
                       or "ffmpeg 转码失败")
    return proc.stdout
