"""环境配置加载：环境变量优先，缺省回落到 server/.env（极简解析，不引依赖）。"""
from __future__ import annotations

import json
import os
import shutil
from dataclasses import dataclass
from pathlib import Path

_ENV_PATH = Path(__file__).resolve().parents[2] / ".env"
_BAILIAN_CONFIG = Path.home() / ".bailian" / "config.json"


def _load_dotenv(path: Path) -> None:
    if not path.is_file():
        return
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        key, value = key.strip(), value.strip().strip('"').strip("'")
        if key and key not in os.environ:
            os.environ[key] = value


def _resolve_ffmpeg() -> str:
    if env := os.environ.get("FFMPEG_BIN"):
        return env
    if found := shutil.which("ffmpeg"):
        return found
    for candidate in ("/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg"):
        if os.path.exists(candidate):
            return candidate
    return "ffmpeg"


def _bailian() -> dict:
    """百炼 CLI 配置（~/.bailian/config.json）：DASHSCOPE_* 环境变量的回落源，
    避免把 API Key 明文写进仓库（bl CLI 登录后自动有）。"""
    try:
        return json.loads(_BAILIAN_CONFIG.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def _resolve_dashscope_ws(bailian: dict) -> str:
    """百炼推理 WebSocket 地址：env 覆盖 → CLI base_url 推导（https→wss）。
    业务空间专属域名形如 wss://{WorkspaceId}.cn-beijing.maas.aliyuncs.com。"""
    if url := os.environ.get("DASHSCOPE_WS_URL", ""):
        return url
    base = str(bailian.get("base_url", "")).strip().rstrip("/")
    if base.startswith("https://"):
        return f"wss://{base[len('https://'):]}{'/api-ws/v1/inference'}"
    return ""


@dataclass(frozen=True)
class Config:
    # ---- 接入端（ADR-030，openclaw channels 模式；CHANNEL 选平台）----
    channel: str
    # ---- 飞书接入端（CHANNEL=feishu；官方 API 收发，ADR-029/030）----
    feishu_app_id: str
    feishu_app_secret: str
    feishu_chat_id: str       # 目标群 chat_id（oc_ 开头；空 = 启动时自动发现唯一群）
    feishu_reply_sender: str  # 指定回复者（ou_ 用户 / cli_ 应用；空 = 任意应用(bot)消息）
    feishu_poll_interval: float  # 消息轮询间隔（秒）
    asr_provider: str
    whisper_model: str
    whisper_language: str
    whisper_device: str
    whisper_compute_type: str
    ffmpeg_bin: str
    mqtt_host: str
    mqtt_port: int
    mqtt_username: str
    mqtt_password: str
    mqtt_client_id: str
    topic_up: str
    topic_down: str
    http_host: str
    http_port: int
    # rec_stop 后等音频帧到齐的静默窗口（秒）：帧流可能在 rec_stop 前后到达，
    # 窗口内无新帧即触发 pipeline（见 docs/decisions.md ADR-014 上方备注）
    rec_flush_timeout: float
    frame_timeout: float  # 帧重组超时（protocol.md §2 细则）
    recording_archive_dir: str  # 录音存档目录（空 = 不存档）；回听定责用（ADR-037）
    # ---- 实时对话（M5，豆包 Seeduplex 全双工，ADR-021）----
    volcengine_api_key: str
    realtime_provider: str          # 默认后端；设备 talk_request.provider 可覆盖（用户拍板：选择权在小设备）
    gemini_api_key: str
    gemini_model: str
    # 阶跃星辰 Step Audio 3 Realtime（ADR-035）
    step_api_key: str
    step_endpoint: str
    step_model: str
    step_voice: str
    realtime_enabled: bool
    realtime_endpoint: str
    realtime_model: str
    realtime_voice: str
    realtime_instructions: str
    realtime_idle_timeout: float   # 空闲超时（秒）：无上行帧也无下行事件则结束会话
    realtime_max_duration: float   # 单会话最长时长（秒），兜底防忘关
    # ---- 流式 ASR（消息模式主路，见 providers.py 登记处）----
    #   qwen        千问 Qwen-Audio-3.1-ASR-Flash-Message（ADR-042，定稿润色）
    #   volc_stream 豆包单向流式 sauc（ADR-022/033，裸文本）
    #   off         不走流式，直接批处理 whisper
    qwen_asr_model: str
    dashscope_api_key: str
    dashscope_ws_url: str
    volc_asr_endpoint: str
    volc_asr_resource_id: str


def load_config() -> Config:
    _load_dotenv(_ENV_PATH)
    bailian = _bailian()
    return Config(
        channel=os.environ.get("CHANNEL", "feishu"),
        feishu_app_id=os.environ.get("FEISHU_APP_ID", ""),
        feishu_app_secret=os.environ.get("FEISHU_APP_SECRET", ""),
        feishu_chat_id=os.environ.get("FEISHU_CHAT_ID", ""),
        feishu_reply_sender=os.environ.get("FEISHU_REPLY_SENDER", ""),
        feishu_poll_interval=float(os.environ.get("FEISHU_POLL_INTERVAL", "2.0")),
        asr_provider=os.environ.get("ASR_PROVIDER", "local_whisper"),
        whisper_model=os.environ.get("WHISPER_MODEL", "small"),
        whisper_language=os.environ.get("WHISPER_LANGUAGE", "zh"),
        whisper_device=os.environ.get("WHISPER_DEVICE", "cpu"),
        whisper_compute_type=os.environ.get("WHISPER_COMPUTE_TYPE", "int8"),
        ffmpeg_bin=_resolve_ffmpeg(),
        mqtt_host=os.environ.get("MQTT_HOST", "127.0.0.1"),
        mqtt_port=int(os.environ.get("MQTT_PORT", "1883")),
        mqtt_username=os.environ.get("MQTT_USERNAME", ""),
        mqtt_password=os.environ.get("MQTT_PASSWORD", ""),
        mqtt_client_id=os.environ.get("MQTT_CLIENT_ID", "server"),
        topic_up=os.environ.get("MQTT_TOPIC_UP", "gaga/up"),
        topic_down=os.environ.get("MQTT_TOPIC_DOWN", "gaga/down"),
        http_host=os.environ.get("HTTP_HOST", "127.0.0.1"),
        http_port=int(os.environ.get("HTTP_PORT", "8000")),
        rec_flush_timeout=float(os.environ.get("REC_FLUSH_TIMEOUT", "2.0")),
        frame_timeout=float(os.environ.get("FRAME_TIMEOUT", "5.0")),
        recording_archive_dir=os.environ.get("RECORDING_ARCHIVE_DIR",
                                             "data/recordings"),
        volcengine_api_key=os.environ.get("VOLCENGINE_API_KEY", ""),
        realtime_enabled=os.environ.get("REALTIME_ENABLED", "true").lower()
                         not in ("0", "false", "no"),
        realtime_provider=os.environ.get("REALTIME_PROVIDER", "volc"),
        gemini_api_key=os.environ.get("GEMINI_API_KEY", ""),
        gemini_model=os.environ.get("GEMINI_MODEL",
                                    "models/gemini-2.0-flash-live-001"),
        step_api_key=os.environ.get("STEP_API_KEY", ""),
        step_endpoint=os.environ.get("STEP_ENDPOINT",
                                     "wss://api.stepfun.com/v1/realtime"),
        step_model=os.environ.get("STEP_MODEL",
                                  "stepaudio-3-realtime-preview"),
        step_voice=os.environ.get("STEP_VOICE", "linjiajiejie"),
        realtime_endpoint=os.environ.get(
            "REALTIME_ENDPOINT",
            "wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue"),
        realtime_model=os.environ.get("REALTIME_MODEL", "1.2.6.1"),
        realtime_voice=os.environ.get("REALTIME_VOICE",
                                      "zh_female_vv_uranus_bigtts"),
        realtime_instructions=os.environ.get(
            "REALTIME_INSTRUCTIONS",
            "你是挂在用户胸前的语音助手 gaga（一只鸭子）。用户通过按键发起对话，"
            "环境可能是户外骑行等嘈杂场景。请用中文、口语化、简短回答（一两句），"
            "不要说客套话。"),
        realtime_idle_timeout=float(os.environ.get("REALTIME_IDLE_TIMEOUT", "120")),
        realtime_max_duration=float(os.environ.get("REALTIME_MAX_DURATION", "300")),
        volc_asr_endpoint=os.environ.get(
            "VOLC_ASR_ENDPOINT",
            "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_nostream"),
        # 本账号 ASR 2.0（volc.seedasr.*）未授权；1.0 小时版已开通（2026-09-23 实测）
        volc_asr_resource_id=os.environ.get("VOLC_ASR_RESOURCE_ID",
                                            "volc.bigasr.sauc.duration"),
        qwen_asr_model=os.environ.get("QWEN_ASR_MODEL",
                                      "qwen-audio-3.1-asr-flash-message"),
        dashscope_api_key=os.environ.get("DASHSCOPE_API_KEY",
                                         str(bailian.get("api_key", ""))),
        dashscope_ws_url=_resolve_dashscope_ws(bailian),
    )
