"""接入端抽象：一种 IM 平台 = 一个 channel adapter（模仿 openclaw 的 channels 模式，ADR-030）。

gaga 的三段语义映射到任意接入端：
- send_text(text)：设备语音的 ASR 文本发进对话（原来叫"发飞书群"，抽象后不关心去哪）
- on_message(text, msg_id, parent_id, root_id)：接入端里"回复者"的文本回流
  （飞书=Hermes 群回复）→ reply 信令下行设备，或实时对话工具回流（ADR-036：
  parent/root 命中挂起 msg_id = 工具结果，插回 talk 会话）
- start()：拉起接收端（飞书=消息轮询；Telegram=Bot polling；微信=回调/轮询）

新增一个接入端 = channels/ 下新建一个文件实现 Channel + 在本包 create_channel
注册表加一行；server 其余部分（session/pipeline/信令）零改动。

配置约定：CHANNEL 选接入端（默认 feishu）；平台凭证走环境变量
（FEISHU_* / WECHAT_* / TELEGRAM_*，见 .env.example 与各适配器 docstring）。
"""
from __future__ import annotations

import logging
from abc import ABC, abstractmethod
from collections.abc import Callable

from ..config import Config

log = logging.getLogger("gaga_server.channels")


class ChannelError(Exception):
    """接入端投递失败（映射到下行错误码 CHANNEL_FAIL）。"""


OnMessage = Callable[..., None]
# on_message(text, msg_id="", parent_id="", root_id="")
# msg_id = 这条消息的平台 id；parent_id/root_id = 它回复的原消息 id（无则空）。
# 后两个参数是实时对话工具回流的配对键（ADR-036），拿不到就传空串。


class Channel(ABC):
    """接入端适配器基类。构造即做纯内存初始化；网络动作在 start()/send_text()。"""

    name = "base"

    def __init__(self, cfg: Config, on_message: OnMessage):
        self.cfg = cfg
        self.on_message = on_message  # 收到回复文本 → 服务端核心（reply 信令下行）

    @abstractmethod
    def start(self) -> None:
        """拉起接收端（后台常驻，断线自愈由适配器负责）。发送不依赖 start()。"""

    @abstractmethod
    def send_text(self, text: str) -> str:
        """上行文本 → 返回平台消息 id（拿不到返回空串）；失败抛 ChannelError。"""
