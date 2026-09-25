"""帧编解码与重组（docs/protocol.md §2）。

语义与 esp32/src/protocol/frame.cpp 严格对齐：
  [1B type][2B payload_len (big-endian)][payload]
  首包   type | 0x80，len 字段 = 整帧 payload 总长度（供接收端预分配）
  中间包 type = 0x00，len 字段 = 本包 payload 字节数
  尾包   type = 原始 type，len 字段 = 本包 payload 字节数
  单包帧 type 不置 0x80，len = payload 长度
接收端凑满首包宣告的总长度即重组完成；超出总长度视为对端异常，丢弃残帧。

与 esp32 端的一个差异：MQTT 一条消息 = 一个物理包（App 逐包原样转发），
所以这里按"一次喂一包"处理；另外加了超时——首包后 5s 未凑齐丢弃残帧。
"""
from __future__ import annotations

import time
from collections.abc import Callable

FRAME_TYPE_CONTINUATION = 0x00  # 续帧（中间包专用）
FRAME_TYPE_OPUS = 0x01          # Opus 音频帧
FRAME_TYPE_JSON = 0x02          # JSON 信令

FRAME_FLAG_MORE = 0x80
FRAME_HEADER_SIZE = 3
FRAME_MAX_PAYLOAD = 0xFFFF

DEFAULT_MAX_PACKET_SIZE = 512  # 服务端下行用；BLE 侧由 App 按 MTU 再切


def encode_frame(frame_type: int, payload: bytes,
                 max_packet_size: int = DEFAULT_MAX_PACKET_SIZE) -> list[bytes]:
    """把一帧编码为若干 ≤ max_packet_size 字节的包（语义同 esp32 encodeFrame）。"""
    if max_packet_size <= FRAME_HEADER_SIZE:
        raise ValueError("max_packet_size 必须大于帧头 3 字节")
    if len(payload) > FRAME_MAX_PAYLOAD:
        raise ValueError("payload 超过 65535 字节")
    cap = max_packet_size - FRAME_HEADER_SIZE
    total = len(payload)
    if total <= cap:
        return [bytes([frame_type, total >> 8, total & 0xFF]) + payload]

    packets: list[bytes] = []
    offset = 0
    first = True
    while offset < total:
        size = min(cap, total - offset)
        last = offset + size >= total
        if first:
            type_field, len_field = frame_type | FRAME_FLAG_MORE, total
        elif not last:
            type_field, len_field = FRAME_TYPE_CONTINUATION, size
        else:
            type_field, len_field = frame_type, size
        packets.append(
            bytes([type_field, len_field >> 8, len_field & 0xFF])
            + payload[offset:offset + size]
        )
        offset += size
        first = False
    return packets


class FrameReassembler:
    """逐包喂入，凑齐一帧触发 on_frame(frame_type, payload)。

    异常（孤儿续帧/超长/截断）走 on_error(reason) 并自保性丢弃残帧。
    """

    def __init__(self, timeout: float = 5.0):
        self.timeout = timeout
        self.on_frame: Callable[[int, bytes], None] | None = None
        self.on_error: Callable[[str], None] | None = None
        self._reset()

    def _reset(self) -> None:
        self._reassembling = False
        self._frame_type = 0
        self._expected = 0
        self._buf = bytearray()
        self._started_at = 0.0

    def _error(self, reason: str) -> None:
        if self.on_error:
            self.on_error(reason)

    def _emit(self, frame_type: int, payload: bytes) -> None:
        if self.on_frame:
            self.on_frame(frame_type, payload)

    def feed_packet(self, packet: bytes) -> None:
        if len(packet) < FRAME_HEADER_SIZE:
            self._error("packet too short")
            return
        if (self._reassembling
                and time.monotonic() - self._started_at > self.timeout):
            self._error("reassemble timeout")
            self._reset()

        type_field = packet[0]
        len_field = (packet[1] << 8) | packet[2]
        body = packet[FRAME_HEADER_SIZE:]

        if type_field & FRAME_FLAG_MORE:
            # 首包：len 字段是整帧 payload 总长度
            self._reassembling = True
            self._frame_type = type_field & 0x7F
            self._expected = len_field
            self._buf = bytearray()
            self._started_at = time.monotonic()
            self._append(body)
            return

        if type_field == FRAME_TYPE_CONTINUATION:
            if not self._reassembling:
                self._error("orphan continuation")
                return
            self._append(body[:len_field])
            return

        if self._reassembling:
            # 尾包（长度没凑满就继续等，与 frame.cpp 行为一致）
            self._append(body[:len_field])
        else:
            # 单包完整帧
            if len_field > len(body):
                self._error("payload truncated")
                return
            self._emit(type_field, bytes(body[:len_field]))

    def _append(self, data: bytes) -> None:
        if len(self._buf) + len(data) > self._expected:
            # 超过首包宣告的总长度：对端不按协议来，丢弃残帧自保
            self._error("frame overflow")
            self._reset()
            return
        self._buf += data
        if len(self._buf) >= self._expected:
            self._emit(self._frame_type, bytes(self._buf))
            self._reset()
