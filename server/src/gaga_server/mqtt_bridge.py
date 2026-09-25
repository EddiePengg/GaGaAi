"""MQTT 桥：订阅 gaga/up（重组后交给会话），发布 gaga/down（帧化 JSON 信令）。

Topic/QoS/clientId 约定见 docs/protocol.md §4：QoS 1、不 retain、clientId=server。
"""
from __future__ import annotations

import json
import logging
import time
from collections.abc import Callable

import paho.mqtt.client as mqtt

from .config import Config
from .frames import FRAME_TYPE_JSON, FRAME_TYPE_OPUS, FrameReassembler, encode_frame

log = logging.getLogger("gaga_server.mqtt")


class MqttBridge:
    def __init__(self, cfg: Config, on_frame: Callable[[int, bytes], None]):
        self.cfg = cfg
        self.reassembler = FrameReassembler(timeout=cfg.frame_timeout)
        self.reassembler.on_frame = on_frame
        self.reassembler.on_error = lambda reason: log.warning("帧重组丢弃: %s", reason)

        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                                  client_id=cfg.mqtt_client_id)
        if cfg.mqtt_username:
            self.client.username_pw_set(cfg.mqtt_username, cfg.mqtt_password)
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message
        self.client.on_disconnect = self._on_disconnect

    def _on_connect(self, client, _userdata, _flags, reason_code, _properties):
        if reason_code.is_failure:
            log.error("MQTT 连接被拒: %s", reason_code)
            return
        client.subscribe(self.cfg.topic_up, qos=1)
        log.info("MQTT 已连接 %s:%d，订阅 %s",
                 self.cfg.mqtt_host, self.cfg.mqtt_port, self.cfg.topic_up)

    def _on_disconnect(self, _client, _userdata, _flags, reason_code, _properties):
        log.warning("MQTT 断开: %s（paho 自动重连）", reason_code)

    def _on_message(self, _client, _userdata, msg):
        # 一条 MQTT 消息 = 一个物理包（App 逐包原样转发，protocol.md §4）
        self.reassembler.feed_packet(bytes(msg.payload))

    def start(self) -> None:
        deadline = time.monotonic() + 30
        while True:
            try:
                self.client.connect(self.cfg.mqtt_host, self.cfg.mqtt_port,
                                    keepalive=60)
                break
            except OSError as e:
                if time.monotonic() > deadline:
                    raise RuntimeError(
                        f"MQTT broker {self.cfg.mqtt_host}:{self.cfg.mqtt_port} "
                        f"连不上（30s 重试耗尽）: {e}") from e
                log.warning("MQTT 连接失败（%s），2s 后重试...", e)
                time.sleep(2)
        self.client.loop_start()

    def publish_json(self, msg: dict) -> None:
        """下行 JSON 信令 → 帧化（type=0x02）→ gaga/down。App 原样转发给设备。

        envelope 自动补 ts（Unix 秒，data-model.md §1）：设备凭它对时（状态栏时钟）。
        """
        if "ts" not in msg:
            msg = {**msg, "ts": int(time.time())}
        payload = json.dumps(msg, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        for pkt in encode_frame(FRAME_TYPE_JSON, payload):
            self.client.publish(self.cfg.topic_down, pkt, qos=1)
        log.info("下行 %s → %s", msg, self.cfg.topic_down)

    def publish_audio(self, data: bytes) -> None:
        """下行音频（talk 会话的模型回复，ogg_opus 24kHz 分片）→ type=0x01 帧。"""
        for pkt in encode_frame(FRAME_TYPE_OPUS, data):
            self.client.publish(self.cfg.topic_down, pkt, qos=1)
