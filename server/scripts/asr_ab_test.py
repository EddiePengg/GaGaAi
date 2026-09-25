#!/usr/bin/env python3
"""ASR 端点 A/B：双向 bigmodel_async vs 单向 bigmodel_nostream。

争议背景：单向（nostream）文档声称"准确率优于双向流式接口"；用户不需要
实时中间结果，只要"说完出字快 + 准"。本脚本同一音频、同资源 ID、同 4x 发速，
量"最后一个音频组发出 → 终稿"延迟（对齐真实链路 rec_stop→出字），比对文本。

用法：.venv/bin/python scripts/asr_ab_test.py [test-audio/real_test.opus] [每端轮数]
"""
from __future__ import annotations

import asyncio
import json
import statistics
import sys
import time
import uuid
from pathlib import Path

import websockets

sys.path.insert(0, str(Path(__file__).parent))
from asr_stream_probe import (FLAG_LAST_NO_SEQ, FLAG_NONE, MSG_AUDIO_ONLY_REQUEST,  # noqa: E402
                              MSG_FULL_CLIENT_REQUEST, encode_frame, parse_frame)
from gaga_server.config import load_config  # noqa: E402
from gaga_server.ogg import OggStreamWriter, parse_opus_ogg  # noqa: E402

ENDPOINTS = {
    "双向 bigmodel_async": "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async",
    "单向 bigmodel_nostream": "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_nostream",
}


async def run_once(endpoint: str, resource: str, api_key: str,
                   groups: list[list[bytes]], last_on_group: bool) -> dict:
    """跑一轮：连接 → 4x 速发音频 → 终稿。返回延迟与文本；异常抛出。"""
    headers = {
        "X-Api-Key": api_key,
        "X-Api-Resource-Id": resource,
        "X-Api-Request-Id": str(uuid.uuid4()),
        "X-Api-Sequence": "-1",
        "X-Api-Connect-Id": str(uuid.uuid4()),
    }
    ws = await websockets.connect(endpoint, additional_headers=headers,
                                  proxy=None, open_timeout=15)
    result: dict = {"final": None, "text": "", "err": None}
    try:
        async with ws:
            await ws.send(encode_frame(
                MSG_FULL_CLIENT_REQUEST, FLAG_NONE, 1, 0,
                json.dumps({
                    "user": {"uid": "gaga-ab"},
                    "audio": {"format": "ogg", "codec": "opus", "rate": 16000,
                              "bits": 16, "channel": 1},
                    "request": {"model_name": "bigmodel", "enable_itn": True,
                                "enable_punc": True, "show_utterances": True},
                }).encode()))
            state = {"done": asyncio.Event(), "text": "", "sent_all": False}

            async def receiver() -> None:
                try:
                    async for raw in ws:
                        resp = parse_frame(raw)
                        if resp["type"] == "error":
                            result["err"] = f"code={resp['code']} {resp['msg']}"
                            state["done"].set()
                            return
                        j = resp["json"]
                        r = j.get("result") or {}
                        text = r.get("text", "")
                        if text:
                            state["text"] = text
                        seq = resp.get("seq")
                        definite = any(u.get("definite")
                                       for u in r.get("utterances", []))
                        is_last_pkg = j.get("is_last_package") is True
                        if ((seq is not None and seq < 0) or is_last_pkg
                                or (definite and state["sent_all"])):
                            state["done"].set()
                            return
                except Exception as e:  # 接收协程崩了要暴露，别静默超时
                    result["err"] = f"receiver: {type(e).__name__}: {e}"
                    state["done"].set()

            recv_task = asyncio.create_task(receiver())
            writer = OggStreamWriter()
            await ws.send(encode_frame(MSG_AUDIO_ONLY_REQUEST, FLAG_NONE, 0, 0,
                                       writer.header()))
            for i, group in enumerate(groups):
                last = i == len(groups) - 1
                await ws.send(encode_frame(
                    MSG_AUDIO_ONLY_REQUEST,
                    FLAG_LAST_NO_SEQ if (last and last_on_group) else FLAG_NONE,
                    0, 0, writer.write_packets(group, eos=last)))
                await asyncio.sleep(0.05)  # 200ms 音频/50ms = 4x，对齐生产节奏
            t_last = time.monotonic()
            state["sent_all"] = True
            if not last_on_group:  # 生产语义：排空后补空负载负包
                await ws.send(encode_frame(MSG_AUDIO_ONLY_REQUEST,
                                           FLAG_LAST_NO_SEQ, 0, 0, b""))
            try:
                await asyncio.wait_for(state["done"].wait(), timeout=8)
            except asyncio.TimeoutError:
                result["err"] = "终稿超时(8s)"
            result["final"] = time.monotonic() - t_last
            result["text"] = state["text"]
            result["err"] = result["err"]
            recv_task.cancel()
    finally:
        await ws.close()
    return result


async def main() -> None:
    cfg = load_config()
    audio = sys.argv[1] if len(sys.argv) > 1 else "test-audio/real_test.opus"
    rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    packets = parse_opus_ogg(Path(audio).read_bytes())
    groups = [packets[i:i + 10] for i in range(0, len(packets), 10)]
    print(f"{audio}: {len(packets)} 包 = {len(packets)*20}ms 音频，{len(groups)} 组；"
          f"每端 {rounds} 轮，资源 {cfg.volc_asr_resource_id}")

    for name, endpoint in ENDPOINTS.items():
        # 单向若负包收尾不灵，退回"最后一组带 last 标志"再试一轮
        for last_on_group in (False, True):
            stats = []
            for r in range(rounds):
                try:
                    res = await run_once(endpoint, cfg.volc_asr_resource_id,
                                         cfg.volcengine_api_key, groups, last_on_group)
                except Exception as e:
                    res = {"final": None, "text": "", "err": f"{type(e).__name__}: {e}"}
                tag = "组尾last" if last_on_group else "负包收尾"
                if res["final"] is not None and not res["err"]:
                    stats.append(res["final"])
                    print(f"  [{name}/{tag}] 第{r+1}轮 终稿 {res['final']:.2f}s "
                          f"文本={res['text'][:40]!r}")
                else:
                    print(f"  [{name}/{tag}] 第{r+1}轮 失败: {res['err']}")
            if stats:
                print(f"== {name}（{'组尾last' if last_on_group else '负包收尾'}）："
                      f"均值 {statistics.mean(stats):.2f}s / "
                      f"最小 {min(stats):.2f}s / 最大 {max(stats):.2f}s\n")
                break
            if last_on_group:
                print(f"== {name}：两种收尾均失败\n")


if __name__ == "__main__":
    asyncio.run(main())
