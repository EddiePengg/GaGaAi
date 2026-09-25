"""飞书接入端探针：只跑 FeishuChannel，验证官方 API 收发（不碰 MQTT/ASR）。

做什么：
1. send_text 实发一条（验证发送权限与 message_id 返回）；
2. start() 起轮询——在群里发条消息（或让 Hermes 回一条），应打印 sender / 文本。

跑法：cd server && .venv/bin/python scripts/feishu_channel_probe.py [可选发送文本]
"""
from __future__ import annotations

import logging
import sys
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s %(levelname)s %(name)s: %(message)s")

from gaga_server.channels.base import ChannelError  # noqa: E402
from gaga_server.channels.feishu import FeishuChannel  # noqa: E402
from gaga_server.config import load_config  # noqa: E402

cfg = load_config()
text = sys.argv[1] if len(sys.argv) > 1 else "🦆 probe"

print(f"CHANNEL={cfg.channel}  chat_id: {cfg.feishu_chat_id or '（自动发现）'}"
      f"  回复者: {cfg.feishu_reply_sender or '（默认=任意应用消息）'}"
      f"  轮询 {cfg.feishu_poll_interval}s")

ch = FeishuChannel(cfg, on_message=lambda t: print(f"\n>>> 收到回复: {t!r}\n"))
try:
    print("send_text ok →", ch.send_text(text))
except ChannelError as e:
    print("send_text 失败:", e)
try:
    ch.start()
except ChannelError as e:
    raise SystemExit(f"start 失败: {e}")
print("轮询监听中（在群里发条消息 / 让 Hermes 回一句；Ctrl+C 退出）")
threading.Event().wait()
