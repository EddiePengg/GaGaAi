"""入口：起 MQTT 桥 + HTTP 调试口（同进程，uvicorn 阻塞主线程，paho 后台线程）。"""
from __future__ import annotations

import logging

import uvicorn

from .asr import create_asr, create_local_fallback, create_stream_asr
from .channels import create_channel
from .channels.base import ChannelError
from .config import load_config
from .http_api import create_app
from .mqtt_bridge import MqttBridge
from .pipeline import Pipeline
from .providers import ProviderRegistry
from .session import SessionManager
from .textfmt import flatten_markdown

log = logging.getLogger("gaga_server")


def main() -> None:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s")

    cfg = load_config()

    # provider 登记处（ADR-043）：三类能力的"用哪个"都从这里查——
    # 设备信令 > HTTP 运行时切换 > 环境变量，GET/POST /debug/providers 可看可切
    registry = ProviderRegistry({
        "channel": cfg.channel,
        "asr": cfg.asr_provider,
        "realtime": cfg.realtime_provider,
    })

    # ASR：流式（消息模式主路）+ 批处理（whisper，兜底 + HTTP 调试口）
    stream_asr = None
    if cfg.asr_provider in ("qwen", "volc_stream"):
        asr = create_local_fallback(cfg)
        stream_asr = create_stream_asr(cfg, registry)
        name, ok = stream_asr.peek()
        log.info("流式 ASR 路由就绪（当前 %s，%s；whisper 批处理兜底）",
                 name, "可用" if ok else "不可用→批处理")
    else:
        asr = create_asr(cfg)

    # 接入端（ADR-030，openclaw channels 模式）：CHANNEL 选平台。
    # 出向 send_text 由 pipeline 投递 ASR 文本；入向 on_message = 回复文本
    # → talk 工具回流路由（ADR-036）或 reply 信令（点亮消息卡）。
    bridge: MqttBridge  # 前向声明：channel 回调经闭包引用 bridge
    talk_bridge = None  # 前向声明：channel 回调要查工具回流表

    def _on_channel_message(text: str, msg_id: str = "",
                            parent_id: str = "", root_id: str = "") -> None:
        if talk_bridge is not None and talk_bridge.on_channel_reply(
                msg_id, parent_id, root_id, text):
            return  # 命中工具回流：已插入实时会话（其台账在先，不受下述过滤影响）
        # 会话圈过滤（2026-09-26 用户拍板）：只转发"回复嘎嘎消息"的回流。
        # Hermes 在群里回别人的话 / 主动发言不再打扰设备（此前照发不误，
        # 设备端 FIFO 兜底还会把它错贴到等待卡上响一声）。
        # 无引用（parent/root 皆空）的 Hermes 消息同样丢弃——"忘记打回复
        # 标签"的补偿走用户规划的"强制 AI 末尾带 ID"方案；丢弃有日志可观察。
        if not (channel.isMine(parent_id) or channel.isMine(root_id)):
            log.info("非嘎嘎会话消息（parent=%s root=%s），忽略: %s",
                     parent_id or "-", root_id or "-", text[:40])
            return
        # reply_to 优先取"确属嘎嘎"的那个：深层回复链 parent 指向 Hermes
        # 中间消息时，root 才是设备认识的（receipt 存过 msgId）
        reply_to = parent_id if channel.isMine(parent_id) else root_id
        # 设备盖戳（2026-09-27 补完 ADR-057 的收尾半步）：reply 经 msg_id→device
        # 映射找回归属。无戳 reply 广播能到、丢了没人补（熄屏僵尸窗实测：
        # receipt 全到 reply 全灭的根源）；带戳后进按设备台账，设备端也只认
        # 自己的（多设备隔离）。
        rdev = session.device_for_msg(reply_to)
        reply_msg = {"type": "reply", "text": flatten_markdown(text),
                     "reply_to": reply_to}
        if rdev:
            reply_msg["device"] = rdev
        bridge.publish_json(reply_msg)

    try:
        channel = create_channel(cfg, _on_channel_message)
    except ChannelError as e:
        raise SystemExit(f"接入端初始化失败（CHANNEL={cfg.channel}）: {e}") from e
    pipeline = Pipeline(cfg, asr, channel)

    # session 需要下行发布，bridge 需要上行回调——互相依赖，用闭包延迟求值
    # realtime（M5）：talk 会话桥，启用条件 = 开关开 + 有 API Key。
    # send_command = 万金油工具通道（ADR-036）：指令经接入端进群喂 Hermes，
    # 返回平台 msg_id 供回流配对。
    if cfg.realtime_enabled and cfg.volcengine_api_key:
        from .realtime.bridge import TalkBridge
        talk_bridge = TalkBridge(
            cfg,
            publish_json=lambda m: bridge.publish_json(m),
            publish_audio=lambda d: bridge.publish_audio(d),
            send_command=channel.send_text,
            registry=registry)
        log.info("realtime 已启用（%s，音色 %s）",
                 cfg.realtime_endpoint, cfg.realtime_voice)
    elif cfg.realtime_enabled:
        log.warning("REALTIME_ENABLED=true 但缺 VOLCENGINE_API_KEY，talk 将回 NOT_IMPLEMENTED")
    session = SessionManager(cfg, pipeline,
                             publish_json=lambda m: bridge.publish_json(m),
                             talk=talk_bridge, stream_asr=stream_asr)
    bridge = MqttBridge(cfg, on_frame=session.handle_frame)
    bridge.start()

    # 接入端接收端最后拉起（入向 on_message → reply 信令，见上方 _on_channel_message）
    try:
        channel.start()
    except ChannelError as e:
        log.warning("接入端接收端启动失败（%s）: %s——发送仍可用，"
                    "reply 仅可经 /debug/reply 注入", channel.name, e)

    app = create_app(cfg, pipeline, publish_json=bridge.publish_json,
                     registry=registry)
    log.info("HTTP 调试口就绪: POST http://%s:%d/debug/audio | /debug/reply | "
             "GET/POST /debug/providers", cfg.http_host, cfg.http_port)
    uvicorn.run(app, host=cfg.http_host, port=cfg.http_port, log_level="warning")


if __name__ == "__main__":
    main()
