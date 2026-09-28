"""MQTT 桥：订阅 gaga/up（重组后交给会话），发布 gaga/down（帧化 JSON 信令）。

Topic/QoS/clientId 约定见 docs/protocol.md §4：QoS 1、不 retain、clientId=server。
"""
from __future__ import annotations

import json
import logging
import threading
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
        # reply 对账（2026-09-26）：reply 信令是一次性推送，BLE 忙时设备收不到
        # = 卡片永远"正在回复"。缓存最近一条未领取回复，rec_status_query 时重发
        # reply 未领取台账（2026-09-27 起按设备）：device → 最近一条 reply。
        # 多设备同群（手表+胸前）时全局单槽会把 A 的回复补给 B（真机实锤）。
        self._last_reply_by_dev: dict = {}      # device → reply 信令
        self._reply_unclaimed: set = set()      # 有未领取 reply 的 device 集合

        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                                  client_id=cfg.mqtt_client_id)
        # 下行串行闸门（RLock）：publish_json/publish_audio 每次发布持锁；
        # NotifyPusher 音频分片流期间全程持锁——分片帧内不允许插入任何单包帧
        self.downlink = threading.RLock()
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
        payload = bytes(msg.payload)
        # 对账：rec_status_query（设备回执/回复丢失时补发最后一次 reply）
        try:
            if len(payload) > 3 and payload[0] == 0x02:
                sig = json.loads(payload[3:].decode("utf-8", "replace"))
                t = sig.get("type")
                dev = str(sig.get("device", "")) or ""
                if t == "rec_start" and dev:
                    # 新一轮消息开始：该设备上一轮 reply 的补发窗口就此关闭
                    self._reply_unclaimed.discard(dev)
                if t == "rec_status_query":
                    # 2026-09-27 按设备补发：查询带 device 只补它的；旧固件
                    # 不带 device 时维持广播行为（全部未领取的都补）。
                    # 不清 unclaimed——补发落在僵尸窗里丢了就永远丢了（真机
                    # 实锤），改为每轮查询都补直到该设备下一轮 rec_start；
                    # 重复无害（设备 fillReply 对已 Replied 卡配对落空即丢）。
                    targets = [dev] if dev else list(self._reply_unclaimed)
                    for d in targets:
                        if d in self._reply_unclaimed:
                            log.info("rec_status_query（%s）→ 补发未领取 reply", d)
                            self.publish_json(self._last_reply_by_dev[d],
                                              from_replay=True)
        except Exception as e:
            log.debug("对账解析跳过: %s", e)
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
        threading.Thread(target=self._heartbeat_loop, daemon=True,
                         name="mqtt-heartbeat").start()

    def _heartbeat_loop(self) -> None:
        """下行心跳（2026-09-26）：每 30s 发一帧 hello_ack（对时信令复用）。

        三重用途：① App 僵尸连接检测的参照流量（"已连接却 90s 无任何下行"
        = 判死重建，见 App MqttManager downlink-watchdog）；② 设备持续对时
        （状态栏时钟不再依赖业务信令的到来）；③ 无业务流量时段的链路探活。
        纯管道信令：App 端 FrameLogger 对 hello_ack 静默，设备端 envelope
        ts 静默校准，都不产生 UI 噪音。失败只记日志——下一拍再来。
        """
        while True:
            time.sleep(30)
            try:
                self.publish_json({"type": "hello_ack"})
            except Exception as e:  # broker 抖动：心跳非关键路径
                log.warning("心跳发布失败（下一拍再试）: %s", e)

    def publish_json(self, msg: dict, *, from_replay: bool = False) -> None:
        """下行 JSON 信令 → 帧化（type=0x02）→ gaga/down。App 原样转发给设备。

        envelope 自动补 ts（Unix 秒，data-model.md §1）：设备凭它对时（状态栏时钟）。
        from_replay=True = 对账重发，不重复置未领取标记（否则无限重发循环）。
        """
        if msg.get("type") == "reply" and not from_replay:
            dev = str(msg.get("device", "")) or ""
            if dev:   # 带设备戳的 reply 按设备记账；无戳（旧链路）不进台账
                self._last_reply_by_dev[dev] = msg
                self._reply_unclaimed.add(dev)
        if "ts" not in msg:
            msg = {**msg, "ts": int(time.time())}
        payload = json.dumps(msg, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        with self.downlink:
            for pkt in encode_frame(FRAME_TYPE_JSON, payload):
                self.client.publish(self.cfg.topic_down, pkt, qos=1)
        log.info("下行 %s → %s", msg, self.cfg.topic_down)

    def publish_audio(self, data: bytes) -> None:
        """下行音频（talk 会话的模型回复，ogg_opus 24kHz 分片）→ type=0x01 帧。"""
        with self.downlink:
            for pkt in encode_frame(FRAME_TYPE_OPUS, data):
                self.client.publish(self.cfg.topic_down, pkt, qos=1)
