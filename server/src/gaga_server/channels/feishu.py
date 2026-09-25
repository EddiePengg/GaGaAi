"""飞书接入端：官方 API 收发（自建应用，ADR-029/030），webhook 时代结束。

出向：im/v1/messages（bot 身份发文本，返回 message_id——receipt 的 msg_id 从此有真值，
为将来问答复配铺路）。
入向：im/v1/messages **轮询**（FEISHU_POLL_INTERVAL，默认 2s）。不用事件长连接的原因
（ADR-030）：控制台事件订阅（im.message.receive_v1 + 长连接模式）未配置即收不到事件，
而消息列表 API 权限已实测可用，且返回的 sender 带 app_id（Hermes 是应用，精确过滤
靠它；事件 payload 里应用类发送者的 sender_id 字段不可靠）。延迟 ≤2s 对"回复点亮
消息卡"够用，将来要更低延迟再补事件订阅。

前置（飞书开放平台控制台）：
- 权限：im:message:send_as_bot（发）、im:message.group_msg:readonly（读群消息）、
  im:chat:readonly（拉群列表——FEISHU_CHAT_ID 留空时自动发现用）
- 机器人拉入目标群

chat_id 解析：FEISHU_CHAT_ID 配了用配置；没配则自动发现——只在恰好一个群时生效
（多群报错列出候选，防发错群）。

入向过滤链（防自环/串扰，按序）：
1. msg_type 非 text/post → 跳过（system/图片/卡片等）；
2. sender 是自己（app_id 相同）或文本"🦆"前缀 → 上行回声，忽略；
3. FEISHU_REPLY_SENDER 配置（ou_ 用户 / cli_ 应用均可）：非指定发送者 → 忽略；
   未配置时默认只认**应用类**发送者（sender_type=app，即群里的 bot 回复，
   如 Hermes；人的闲聊不会点亮设备消息卡）。
启动时先拉一轮只记 id 不触发回调（不把历史消息当回复重放）。
"""
from __future__ import annotations

import json
import logging
import threading
import time

import lark_oapi as lark
from lark_oapi.api.im.v1 import (CreateMessageRequest, CreateMessageRequestBody,
                                 ListChatRequest, ListMessageRequest)

from ..config import Config
from .base import Channel, ChannelError

log = logging.getLogger("gaga_server.channels.feishu")

_UPLINK_PREFIX = "🦆"  # 上行文本固定前缀（pipeline.deliver_text）——回声识别


class FeishuChannel(Channel):
    name = "feishu"

    def __init__(self, cfg: Config, on_message):
        super().__init__(cfg, on_message)
        if not (cfg.feishu_app_id and cfg.feishu_app_secret):
            raise ChannelError(
                "feishu 接入端需要 FEISHU_APP_ID / FEISHU_APP_SECRET（自建应用凭证，.env）")
        self._chat_id = cfg.feishu_chat_id
        self._api = (lark.Client.builder()
                     .app_id(cfg.feishu_app_id)
                     .app_secret(cfg.feishu_app_secret).build())
        self._seen: set[str] = set()
        self._poll_thread: threading.Thread | None = None

    # ---- 生命周期 ----

    def start(self) -> None:
        if not self._chat_id:
            self._chat_id = self._discover_chat_id()
        self._sync_seen()  # 首轮只记 id：历史消息不当回复重放
        self._poll_thread = threading.Thread(target=self._poll_loop,
                                             daemon=True, name="feishu-poll")
        self._poll_thread.start()
        log.info("feishu 接入端就绪（chat_id=%s，轮询 %.1fs；回复者=%s）",
                 self._chat_id, self.cfg.feishu_poll_interval,
                 self.cfg.feishu_reply_sender or "默认=任意应用(bot)消息")

    def _discover_chat_id(self) -> str:
        """FEISHU_CHAT_ID 未配置：拉机器人所在群列表自动发现（恰好一个群才放行）。"""
        resp = self._api.im.v1.chat.list(
            ListChatRequest.builder().page_size(50).build())
        if not resp.success():
            raise ChannelError(
                f"拉群列表失败 code={resp.code} msg={resp.msg}"
                "（缺 im:chat:readonly 权限？或直接在 .env 配 FEISHU_CHAT_ID 绕过）")
        names = {i.chat_id: i.name for i in (resp.data.items or [])}
        if len(names) == 1:
            chat_id, chat_name = next(iter(names.items()))
            log.info("FEISHU_CHAT_ID 未配置，自动发现唯一群 %s（%s）", chat_id, chat_name)
            return chat_id
        raise ChannelError(
            f"机器人在 {len(names)} 个群里，不能猜（候选：{names}）——"
            "在 .env 里配 FEISHU_CHAT_ID 指定目标群")

    # ---- 出向 ----

    def send_text(self, text: str) -> str:
        if not self._chat_id:
            self._chat_id = self._discover_chat_id()  # 惰性解析：不依赖 start() 先跑
        req = (CreateMessageRequest.builder()
               .receive_id_type("chat_id")
               .request_body(CreateMessageRequestBody.builder()
                             .receive_id(self._chat_id)
                             .msg_type("text")
                             .content(json.dumps({"text": text}, ensure_ascii=False))
                             .build())
               .build())
        resp = self._api.im.v1.message.create(req)
        if not resp.success():
            raise ChannelError(f"飞书发送失败 code={resp.code} msg={resp.msg}"
                               "（缺 im:message:send_as_bot 权限？机器人不在群里？）")
        return resp.data.message_id or ""

    # ---- 入向：轮询 ----

    def _poll_loop(self) -> None:
        interval = self.cfg.feishu_poll_interval
        while True:
            time.sleep(interval)
            try:
                self._poll_once()
            except Exception as e:  # 网络/接口抖动：记日志下一轮再试，别死循环
                log.warning("feishu 轮询异常（下轮重试）: %s", e)

    def _poll_once(self) -> None:
        for m in self._list_messages():
            if m.message_id in self._seen:
                continue
            self._seen.add(m.message_id)
            self._handle(m)

    def _list_messages(self):
        """拉最新一页（新→旧）。sort_type 必须 Desc：默认排序从建群最早开始，
        拿到的是历史首页而非最新消息（2026-09-24 实测踩坑——90s 轮询静默无效）。
        首轮 _sync_seen 已挡历史，_seen 幂等挡重复。"""
        resp = self._api.im.v1.message.list(
            ListMessageRequest.builder()
            .container_id_type("chat")
            .container_id(self._chat_id)
            .sort_type("ByCreateTimeDesc")
            .page_size(20)
            .build())
        if not resp.success():
            raise ChannelError(f"拉消息失败 code={resp.code} msg={resp.msg}")
        return list(reversed(resp.data.items or []))  # 时间正序处理

    def _sync_seen(self) -> None:
        for m in self._list_messages():
            self._seen.add(m.message_id)

    def _handle(self, m) -> None:
        if m.msg_type not in ("text", "post"):
            return
        sender = m.sender
        sender_id = sender.id or "?"
        is_self_app = (sender.sender_type == "app"
                       and sender_id == self.cfg.feishu_app_id)
        text = self._extract_text(m)
        if is_self_app or text.startswith(_UPLINK_PREFIX):
            return  # 自己发出的上行回声
        if self.cfg.feishu_reply_sender:
            if sender_id != self.cfg.feishu_reply_sender:
                return  # 非指定回复者（日志都不打：轮询下每条消息都会过这，降噪）
        elif sender.sender_type != "app":
            return  # 未配置过滤时的默认：只认应用(bot)消息，人的闲聊不点亮消息卡
        if not text:
            return
        # 引用链不再在本层过滤（ADR-036 联调修正）：实测 Hermes 回复常不带
        # parent/root 引用，这里丢弃会让工具结果永远到不了设备。带引用/不带引用
        # 全部上抛，由 TalkBridge.on_channel_reply 决定路由（精确配对 → 时间窗
        # 兜底 → 普通回复信令）。
        log.info("feishu 回复（sender=%s type=%s msg_id=%s parent=%s root=%s）: %s",
                 sender_id, sender.sender_type, m.message_id,
                 m.parent_id or "-", m.root_id or "-", text)
        self.on_message(text, msg_id=m.message_id or "",
                        parent_id=m.parent_id or "", root_id=m.root_id or "")

    @staticmethod
    def _extract_text(m) -> str:
        """text 直接取；post 富文本抽文字段（title + text/a 元素）；其余返回空。"""
        try:
            content = json.loads(m.body.content or "{}")
        except json.JSONDecodeError:
            return ""
        if m.msg_type == "text":
            return str(content.get("text", "")).strip()
        if m.msg_type == "post":
            lines = []
            for para in content.get("content", []):
                parts = []
                for el in para:
                    if isinstance(el, dict) and el.get("tag") in ("text", "a"):
                        parts.append(str(el.get("text", "")))
                lines.append("".join(parts))
            title = str(content.get("title") or "")
            body = "\n".join(lines).strip()
            return f"{title}\n{body}".strip() if title else body
        return ""
