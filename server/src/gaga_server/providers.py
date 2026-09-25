"""provider 统一登记处（ADR-043）：服务端所有"用哪个"的唯一答案表。

服务端有三类可插拔能力，历史上各有各的选择机制（CHANNEL / ASR_PROVIDER /
REALTIME_PROVIDER 环境变量 + 设备信令覆盖），散在装配代码里。本模块收拢成
一张表 + 一条优先级规则：

    设备信令（talk 场景，设备拍板） > 运行时切换（HTTP /debug/providers） >
    环境变量（启动默认）

各能力的语义：
    channel   消息接入端（feishu）。常驻轮询，运行时切换不动已启动的实例
              （仅登记，标注"切换需重启"）。
    asr       录音转文字引擎（qwen / volc_stream / off）。下一段录音生效；
              off = 不走流式，直接批处理（whisper）。
    realtime  实时对话引擎（volc / step / gemini）。设备信令显式指定时以
              设备为准（选择权在小设备，用户拍板）；没指定才用这里的值。
              下一段 talk 生效，不打断进行中的会话。

HTTP 调试口（http_api.py）读 describe() / 写 set_runtime()。
"""
from __future__ import annotations

from dataclasses import dataclass, field
import threading

# 各能力候选与中文名（登记即文档；channel 只有 feishu 实现，列出备查）
_CATALOG: dict[str, dict[str, str]] = {
    "channel": {"feishu": "飞书群（官方 API 收发）"},
    "asr": {"qwen": "千问 message（定稿润色，ADR-042）",
            "volc_stream": "豆包单向流式（裸文本，ADR-033）",
            "off": "批处理 whisper（不用流式）"},
    "realtime": {"volc": "豆包实时语音 3.0",
                 "step": "阶跃星辰 Step Audio 3",
                 "gemini": "Gemini Live（待联调）"},
}

# 运行时切换需要重启/不即时生效的能力说明
_NOTES: dict[str, str] = {
    "channel": "常驻轮询，改 CHANNEL 后需重启",
    "asr": "下一段录音生效；设备上无切换入口，用 POST /debug/providers 切",
    "realtime": "设备信令显式指定时以设备为准；这里只决定'设备没指定时用哪个'",
}


@dataclass
class _Entry:
    env_default: str          # 环境变量给的启动默认
    runtime: str = ""         # HTTP 运行时覆盖（空 = 没覆盖）
    options: dict[str, str] = field(default_factory=dict)
    note: str = ""

    def current(self) -> tuple[str, str]:
        """(当前值, 来源)。"""
        if self.runtime:
            return self.runtime, "runtime"
        return self.env_default, "env"


class ProviderRegistry:
    """三类能力的当前选择。线程安全（paho/uvicorn 两个线程都会碰）。"""

    def __init__(self, env_defaults: dict[str, str]):
        self._entries = {
            cap: _Entry(env_default=name, options=dict(_CATALOG[cap]),
                        note=_NOTES.get(cap, ""))
            for cap, name in env_defaults.items()
        }
        self._lock = threading.Lock()

    def current(self, cap: str) -> str:
        """当前应使用的 provider 名（设备信令优先级更高，由调用方先判）。"""
        with self._lock:
            return self._entries[cap].current()[0]

    def set_runtime(self, cap: str, provider: str | None) -> None:
        """运行时切换；provider=None 清除覆盖回落环境变量默认。"""
        with self._lock:
            self._entries[cap].runtime = provider or ""

    def describe(self) -> dict:
        """全部能力现状（GET /debug/providers 的响应体）。"""
        with self._lock:
            return {
                cap: {
                    "current": cur,
                    "source": src,
                    "options": e.options,
                    "note": e.note,
                }
                for cap, e in self._entries.items()
                for cur, src in [e.current()]
            }
