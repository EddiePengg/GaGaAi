#!/usr/bin/env python3
"""火山大模型流式 ASR（sauc 二进制协议）连通性/格式探针。

验证：X-Api-Key 鉴权 + format=ogg/codec=opus 增量 Ogg 流直发是否被接受、
延迟数字（首响应/终稿）。用法：
  .venv/bin/python scripts/asr_stream_probe.py [test-audio/test.opus]
"""
from __future__ import annotations

import asyncio
import json
import struct
import sys
import time
import uuid
from pathlib import Path

import websockets

from gaga_server.config import load_config
from gaga_server.ogg import OggStreamWriter, parse_opus_ogg

# sauc 二进制协议（大模型流式 ASR 文档 §WebSocket 二进制协议）
MSG_FULL_CLIENT_REQUEST = 0b0001
MSG_AUDIO_ONLY_REQUEST = 0b0010
MSG_FULL_SERVER_RESPONSE = 0b1001
MSG_ERROR = 0b1111
FLAG_NONE = 0b0000
FLAG_POS_SEQ = 0b0001
FLAG_LAST_NO_SEQ = 0b0010
FLAG_NEG_SEQ = 0b0011


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
    msg_type = data[1] >> 4
    flags = data[1] & 0xF
    comp = data[2] & 0xF
    pos = 4
    seq = None
    if flags in (FLAG_POS_SEQ, FLAG_NEG_SEQ):
        seq = struct.unpack(">i", data[pos:pos + 4])[0]
        pos += 4
    if msg_type == MSG_ERROR:
        code = struct.unpack(">I", data[pos:pos + 4])[0]
        size = struct.unpack(">I", data[pos + 4:pos + 8])[0]
        return {"type": "error", "code": code,
                "msg": data[pos + 8:pos + 8 + size].decode("utf-8", "replace")}
    size = struct.unpack(">I", data[pos:pos + 4])[0]
    payload = data[pos + 4:pos + 4 + size]
    if comp == 1:
        import gzip
        payload = gzip.decompress(payload)
    return {"type": "response", "seq": seq,
            "json": json.loads(payload) if payload else {}}


async def probe(path: str) -> int:
    cfg = load_config()
    if not cfg.volcengine_api_key:
        print("FAIL: VOLCENGINE_API_KEY 未配置")
        return 1

    endpoint = ("wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async")
    headers = {
        "X-Api-Key": cfg.volcengine_api_key,
        # 实测（2026-09-23）：本账号 seedasr（ASR 2.0）未授权 403；
        # bigasr（ASR 1.0 小时版）已开通可用
        "X-Api-Resource-Id": "volc.bigasr.sauc.duration",
        "X-Api-Request-Id": str(uuid.uuid4()),
        "X-Api-Sequence": "-1",
        "X-Api-Connect-Id": str(uuid.uuid4()),
    }
    packets = parse_opus_ogg(Path(path).read_bytes())
    groups = [packets[i:i + 10] for i in range(0, len(packets), 10)]  # 200ms/组
    print(f"{path}: {len(packets)} 包 → {len(groups)} 组（200ms/组）")

    t0 = time.monotonic()
    try:
        ws_ctx = websockets.connect(endpoint, additional_headers=headers,
                                    proxy=None, open_timeout=15)
        ws = await ws_ctx
    except Exception as e:
        print(f"握手失败: {type(e).__name__}: {e}")
        resp = getattr(e, "response", None)
        if resp is not None:
            print("状态码:", resp.status_code)
            print("响应头:", list(resp.headers.raw_items()))
            body = getattr(resp, "body", None)
            if body:
                print("响应体:", body[:500])
        return 1
    async with ws:
        print(f"已连接 logid={ws.response.headers.get('X-Tt-Logid')}")
        full_req = {
            "user": {"uid": "gaga-probe"},
            "audio": {"format": "ogg", "codec": "opus", "rate": 16000,
                      "bits": 16, "channel": 1},
            "request": {"model_name": "bigmodel", "enable_itn": True,
                        "enable_punc": True, "show_utterances": True},
        }
        await ws.send(encode_frame(MSG_FULL_CLIENT_REQUEST, FLAG_NONE,
                                   1, 0, json.dumps(full_req).encode()))
        print("full client request 已发（format=ogg codec=opus）")

        results: list[dict] = []
        t_first = t_final = None

        async def receiver() -> None:
            nonlocal t_first, t_final
            async for raw in ws:
                resp = parse_frame(raw)
                dt = time.monotonic() - t0
                if resp["type"] == "error":
                    print(f"  [{dt:5.2f}s] ERROR code={resp['code']} {resp['msg']}")
                    return
                j = resp["json"]
                text = j.get("result", {}).get("text", "")
                dur = j.get("audio_info", {}).get("duration")
                utt = j.get("result", {}).get("utterances", [])
                definite = [u.get("definite") for u in utt]
                print(f"  [{dt:5.2f}s] seq={resp['seq']} dur={dur} "
                      f"definite={definite} text={text!r}")
                if text and t_first is None:
                    t_first = dt
                results.append(j)
                if resp["seq"] is not None and resp["seq"] < 0:
                    t_final = dt
                    return

        recv_task = asyncio.create_task(receiver())

        writer = OggStreamWriter()
        await ws.send(encode_frame(MSG_AUDIO_ONLY_REQUEST, FLAG_NONE, 0, 0,
                                   writer.header()))
        for i, group in enumerate(groups):
            last = i == len(groups) - 1
            await ws.send(encode_frame(
                MSG_AUDIO_ONLY_REQUEST,
                FLAG_LAST_NO_SEQ if last else FLAG_NONE, 0, 0,
                writer.write_packets(group, eos=last)))
            await asyncio.sleep(0.1)  # 建议 100~200ms 发包间隔
        print(f"音频发完（{time.monotonic() - t0:.2f}s），等终稿...")

        try:
            await asyncio.wait_for(recv_task, timeout=15)
        except asyncio.TimeoutError:
            print("等终稿超时")

    print(f"\n首文本延迟: {t_first and round(t_first, 2)}s；"
          f"终稿延迟: {t_final and round(t_final, 2)}s")
    if results:
        print(f"最终文本: {results[-1].get('result', {}).get('text', '')!r}")
    return 0 if t_final else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(probe(sys.argv[1] if len(sys.argv) > 1
                               else "test-audio/test.opus")))
