"""sauc 二进制协议编解码（火山大模型流式 ASR 文档 §WebSocket 二进制协议）。

帧格式：4B 位域头 + [可选 4B sequence] + 4B payload size（大端）+ payload。
byte0: version(4b)=1 | header_size(4b)=1（×4B）
byte1: message_type(4b) | flags(4b)
byte2: serialization(4b)（0=无 1=JSON） | compression(4b)（0=无 1=gzip）
byte3: 保留 0x00
flags：0b0000 无 sequence；0b0001 正 sequence；0b0010 最后一包（无 sequence）；
       0b0011 负 sequence（最后一包）。服务端负 sequence 帧 = 最终响应。
"""
from __future__ import annotations

import gzip
import json
import struct

MSG_FULL_CLIENT_REQUEST = 0b0001
MSG_AUDIO_ONLY_REQUEST = 0b0010
MSG_FULL_SERVER_RESPONSE = 0b1001
MSG_ERROR = 0b1111

FLAG_NONE = 0b0000
FLAG_POS_SEQ = 0b0001
FLAG_LAST_NO_SEQ = 0b0010
FLAG_NEG_SEQ = 0b0011

SER_NONE = 0b0000
SER_JSON = 0b0001
COMP_NONE = 0b0000
COMP_GZIP = 0b0001


class SaucError(Exception):
    def __init__(self, code: int, msg: str):
        super().__init__(f"sauc 错误 {code}: {msg}")
        self.code = code
        self.msg = msg


def encode_frame(msg_type: int, flags: int, serialization: int,
                 compression: int, payload: bytes,
                 sequence: int | None = None) -> bytes:
    header = bytes([0x11, (msg_type << 4) | flags,
                    (serialization << 4) | compression, 0x00])
    body = b""
    if flags in (FLAG_POS_SEQ, FLAG_NEG_SEQ) and sequence is not None:
        body += struct.pack(">i", sequence)
    return header + body + struct.pack(">I", len(payload)) + payload


def parse_frame(data: bytes) -> dict:
    """解析下行帧。response: {type:'response', seq, json}；error: {type:'error', code, msg}。"""
    if len(data) < 4:
        raise SaucError(0, f"帧过短（{len(data)}B）")
    msg_type = data[1] >> 4
    flags = data[1] & 0xF
    comp = data[2] & 0xF
    pos = 4
    seq = None
    if flags in (FLAG_POS_SEQ, FLAG_NEG_SEQ):
        seq = struct.unpack(">i", data[pos:pos + 4])[0]  # 有符号：负包=最终响应
        pos += 4
    if msg_type == MSG_ERROR:
        code = struct.unpack(">I", data[pos:pos + 4])[0]
        size = struct.unpack(">I", data[pos + 4:pos + 8])[0]
        return {"type": "error", "code": code,
                "msg": data[pos + 8:pos + 8 + size].decode("utf-8", "replace")}
    size = struct.unpack(">I", data[pos:pos + 4])[0]
    payload = data[pos + 4:pos + 4 + size]
    if comp == COMP_GZIP:
        payload = gzip.decompress(payload)
    return {"type": "response", "seq": seq,
            "json": json.loads(payload) if payload else {}}
