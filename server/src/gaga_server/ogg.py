"""Ogg Opus 封装与解包。

设备上行的是裸 Opus 包（docs/data-model.md §2：一帧 Opus 一个传输帧），
而 ffmpeg 只认 Ogg 容器，所以服务端解码前先用 pack_opus_ogg() 把裸包封装成
Ogg Opus 临时文件；parse_opus_ogg() 是反向解包，给 scripts/simulate_device.py
用——从 ffmpeg 生成的 .opus 文件里拆出裸包，字节级模拟真实设备上行。
"""
from __future__ import annotations

import struct

# Ogg CRC32：poly 0x04C11DB7，MSB 优先，初值 0，无最终异或（与 zlib.crc32 不同）
_CRC_TABLE: list[int] = []
for _i in range(256):
    _r = _i << 24
    for _ in range(8):
        _r = ((_r << 1) ^ 0x04C11DB7) if (_r & 0x80000000) else (_r << 1)
        _r &= 0xFFFFFFFF
    _CRC_TABLE.append(_r)


def _ogg_crc(data: bytes) -> int:
    crc = 0
    for b in data:
        crc = ((crc << 8) & 0xFFFFFFFF) ^ _CRC_TABLE[((crc >> 24) & 0xFF) ^ b]
    return crc


_HEADER_TYPE_BOS = 0x02
_HEADER_TYPE_EOS = 0x04

# 设备录音参数（docs/data-model.md §2）：16kHz 单声道、20ms/帧；
# Opus 解码输出恒为 48kHz 基准，20ms = 960 采样（granule 按 48k 计）
_FRAME_SAMPLES_48K = 960
_PRE_SKIP = 3840  # 与 ffmpeg 生成的 Ogg Opus 保持一致


def _build_page(serial: int, seq: int, granule: int, header_type: int,
                packets: list[bytes]) -> bytes:
    seg_table = bytearray()
    body = bytearray()
    for pkt in packets:
        full, rem = divmod(len(pkt), 255)
        seg_table.extend(b"\xff" * full)
        seg_table.append(rem)  # 255 的倍数时以 0 收尾，标记包结束
        body += pkt
    # 页头 27B：capture/version/type/granule/serial/seq/CRC/segment_count
    fixed = struct.pack("<4sBBqII", b"OggS", 0, header_type, granule, serial, seq)
    tail = bytes([len(seg_table)]) + bytes(seg_table) + bytes(body)
    crc = _ogg_crc(fixed + b"\x00\x00\x00\x00" + tail)  # CRC 字段自身按 0 计算
    return fixed + struct.pack("<I", crc) + tail


class OggStreamWriter:
    """增量 Ogg Opus 封装器：页序列号/granule 跨调用连续，产出是一条完整 Ogg 流
    的连续切片（火山流式 ASR format=ogg 的流式上行用，ADR-022）。"""

    def __init__(self, input_sample_rate: int = 16000):
        self.serial = 0x47414741  # "GAGA"
        self.seq = 0
        self.granule = 0
        self._header_sent = False
        self._sample_rate = input_sample_rate

    def header(self) -> bytes:
        """OpusHead(BOS) + OpusTags 两页，一条流只发一次。"""
        if self._header_sent:
            return b""
        self._header_sent = True
        opus_head = (
            b"OpusHead"
            + struct.pack("<BBHIhB", 1, 1, _PRE_SKIP, self._sample_rate, 0, 0)
        )
        vendor = b"gaga-server"
        opus_tags = (
            b"OpusTags" + struct.pack("<I", len(vendor)) + vendor + struct.pack("<I", 0)
        )
        out = _build_page(self.serial, 0, 0, _HEADER_TYPE_BOS, [opus_head])
        out += _build_page(self.serial, 1, 0, 0, [opus_tags])
        self.seq = 2
        return out

    def write_packets(self, packets: list[bytes], eos: bool = False) -> bytes:
        """若干裸 Opus 包 → 连续的音频页字节（一包一页，granule 按 48k 递增）。"""
        if not self._header_sent:
            raise RuntimeError("先调 header()")
        out = bytearray()
        for i, pkt in enumerate(packets):
            self.granule += _FRAME_SAMPLES_48K
            htype = _HEADER_TYPE_EOS if (eos and i == len(packets) - 1) else 0
            out += _build_page(self.serial, self.seq, self.granule, htype, [pkt])
            self.seq += 1
        return bytes(out)


def pack_opus_ogg(packets: list[bytes], input_sample_rate: int = 16000) -> bytes:
    """裸 Opus 包序列 → Ogg Opus 字节流（可喂 ffmpeg 解码）。"""
    if not packets:
        raise ValueError("空包列表")
    writer = OggStreamWriter(input_sample_rate)
    return writer.header() + writer.write_packets(packets, eos=True)


def parse_opus_ogg(data: bytes) -> list[bytes]:
    """Ogg Opus 字节流 → 裸 Opus 包列表（跳过 OpusHead/OpusTags 两个头包）。

    支持 lacing 跨页拼接；不校验 CRC（信任本地 ffmpeg 产物）。
    """
    packets: list[bytes] = []
    partial = bytearray()
    pos = 0
    while pos + 27 <= len(data):
        if data[pos:pos + 4] != b"OggS":
            raise ValueError(f"Ogg 页头魔数不匹配 @offset {pos}")
        seg_count = data[pos + 26]
        seg_start = pos + 27
        seg_table = data[seg_start:seg_start + seg_count]
        body_start = seg_start + seg_count
        body = data[body_start:body_start + sum(seg_table)]
        offset = 0
        for lace in seg_table:
            partial += body[offset:offset + lace]
            offset += lace
            if lace < 255:
                packets.append(bytes(partial))
                partial.clear()
        pos = body_start + sum(seg_table)
    if partial:
        raise ValueError("Ogg 尾部有未闭合的包")
    if len(packets) >= 2 and packets[0].startswith(b"OpusHead"):
        return packets[2:]
    return packets
