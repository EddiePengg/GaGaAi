"""realtime provider 抽象（ADR-034）：可插拔的实时语音对话后端。

桥接层（bridge.py）只消费本文件定义的**归一化事件**，不感知厂商协议；
新增后端 = 新文件实现 RealtimeProvider + __init__.py 注册一行。

## 归一化事件契约（on_event 回调的 dict）

    {"kind": "session_created", "session": str}   # 会话建立（bridge 发 talk_ready）
    {"kind": "asr_started"}                       # 新一句用户语音开始（bridge → 设备清上一句字幕）
    {"kind": "asr_delta",  "text": str}           # 本句 ASR 累积文本（快照语义，provider 已合并）
    {"kind": "asr_final",  "text": str}           # 本句 ASR 终稿
    {"kind": "reply_delta", "text": str}          # 模型回复累积文本
    {"kind": "reply_final", "text": str}          # 模型回复终稿
    {"kind": "audio_delta", "ogg": bytes}         # 下行音频：Ogg Opus 字节流分片（设备端 demux）
    {"kind": "exit_intent"}                       # 用户表达退出意图
    {"kind": "error", "msg": str}
    {"kind": "usage", "in": int, "out": int}      # 可选：一轮 token 用量

asr_started 的意义：实时 ASR 的快照常含上一句残留（用户反馈的"叠字"根因），
provider 在新一句开始时先报 asr_started，bridge 据此下发空 talk_asr 清屏——
"这句出来的时候，上一句就可以清掉"。

## 上行接口（bridge 的 pacer 以 20ms 节奏调用）

send_audio(opus_packet)  # 设备上行 Opus 16k/20ms 裸包；provider 自行转厂商格式
commit_audio()           # 强制判停（无此能力的后端空实现）
mute() / unmute()        # 断流保活（无此能力的后端空实现）
cancel_response()        # 打断
close()                  # 优雅关闭
"""
from __future__ import annotations

import abc
from collections.abc import Awaitable, Callable

OnEvent = Callable[[dict], None]


class RealtimeProviderError(Exception):
    pass


class RealtimeProvider(abc.ABC):
    """单个实时会话的协议封装。所有方法必须在 bridge 的 asyncio 循环里调用。"""

    name = "base"  # 引擎标识（volc/gemini/step），子类覆盖；日志与信令用

    def __init__(self, on_event: OnEvent):
        self.on_event = on_event

    @abc.abstractmethod
    async def connect(self) -> None:
        """建连 + 会话创建。session_created 事件经 on_event 上报。"""

    @abc.abstractmethod
    async def send_audio(self, opus_packet: bytes) -> None:
        """一帧 20ms Opus 裸包上行（provider 负责转厂商音频格式）。"""

    async def commit_audio(self) -> None:
        """强制判停。无此概念的后端空实现。"""

    async def send_tool_result(self, call_id: str, text: str) -> None:
        """函数结果回传（ADR-036）：call_id 配对 + role=tool。未实现 FC 的后端空操作。"""

    async def inject_context(self, text: str) -> None:
        """插入一条用户侧上下文消息（ADR-036：Hermes 结果回流）。未实现的后端空操作。"""

    async def mute(self) -> None:
        """上行断流保活。无此概念的后端空实现。"""

    async def unmute(self) -> None:
        pass

    async def cancel_response(self) -> None:
        pass

    @abc.abstractmethod
    async def close(self) -> None:
        """优雅关闭（超时兜底直接断）。"""

    @abc.abstractmethod
    async def recv_loop(self) -> Awaitable[None]:
        """收事件直到连接关闭；厂商事件翻译成归一化事件后经 on_event 分发。"""
