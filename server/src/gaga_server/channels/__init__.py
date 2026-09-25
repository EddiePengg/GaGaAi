"""接入端工厂：CHANNEL 环境变量选平台，新增接入端在此注册。

新增接入端的做法（模仿 openclaw channels 模式）：
1. 新建 channels/<平台>.py，实现 base.Channel（start 收 / send_text 发）；
2. 在下方 _REGISTRY 加一行；
3. 平台凭证环境变量（<平台大写>_*）加进 config.py / .env.example。
server 其余部分零改动。
"""
from __future__ import annotations

from collections.abc import Callable

from ..config import Config
from .base import Channel, ChannelError
from .feishu import FeishuChannel

# 接入端注册表：CHANNEL=<键> → 适配器类
# 计划中：wechat（企业微信/个人微信回调）、telegram（Bot long polling）
_REGISTRY: dict[str, type[Channel]] = {
    "feishu": FeishuChannel,
}


def create_channel(cfg: Config, on_message: Callable[[str], None]) -> Channel:
    cls = _REGISTRY.get(cfg.channel)
    if cls is None:
        raise ChannelError(
            f"未知接入端 CHANNEL={cfg.channel!r}（可用：{sorted(_REGISTRY)}）；"
            "新平台见本文件 docstring 的三步接入法")
    return cls(cfg, on_message)
