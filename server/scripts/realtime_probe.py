#!/usr/bin/env python3
"""火山 duplex realtime 连通性探针：session.create → session.created → session.close。

只验证鉴权 + 会话建立，不发音频（不消耗对话时长）。用法：
  .venv/bin/python scripts/realtime_probe.py
"""
from __future__ import annotations

import asyncio
import json
import sys
import uuid

import websockets

from gaga_server.config import load_config

SESSION_CREATE = {
    "type": "session.create",
    "session": {
        "model": "1.2.6.1",
        "instructions": "你是语音助手，用中文简短回答。",
        "audio": {
            "input": {"format": {"type": "speech_opus", "rate": 16000}},
            "output": {
                "format": {"type": "ogg_opus", "rate": 24000},
                "voice": "zh_female_vv_uranus_bigtts",
            },
        },
        "extension": {"dialog": {"extra": {"enable_user_query_exit": True}}},
    },
}


async def probe() -> int:
    cfg = load_config()
    if not cfg.volcengine_api_key:
        print("FAIL: VOLCENGINE_API_KEY 未配置（server/.env）")
        return 1

    headers = {
        "X-Api-Key": cfg.volcengine_api_key,
        "X-Api-Connect-Id": str(uuid.uuid4()),
    }
    print(f"连接 {cfg.realtime_endpoint} ...")
    try:
        async with websockets.connect(
                cfg.realtime_endpoint,
                additional_headers=headers,
                proxy=None,  # 绕过系统 SOCKS 代理：火山是国内服务，直连
                open_timeout=15) as ws:
            print(f"已连接（HTTP {ws.response.status_code}），"
                  f"logid={ws.response.headers.get('X-Tt-Logid')}")
            await ws.send(json.dumps(SESSION_CREATE))
            print("已发 session.create，等下行事件...")

            created = False
            deadline = asyncio.get_event_loop().time() + 10
            while asyncio.get_event_loop().time() < deadline:
                try:
                    raw = await asyncio.wait_for(ws.recv(), timeout=3)
                except asyncio.TimeoutError:
                    continue
                except websockets.ConnectionClosed as e:
                    print(f"连接被关闭: code={e.code} reason={e.reason}")
                    break
                evt = json.loads(raw)
                etype = evt.get("type", "?")
                print(f"  <- {etype}: {json.dumps(evt, ensure_ascii=False)[:300]}")
                if etype == "session.created":
                    created = True
                    break
                if etype == "error":
                    break

            if not created:
                print("FAIL: 未拿到 session.created")
                return 1

            # 优雅关闭（不发 close 直接断会触发服务端 55000001 ContextCanceled）
            await ws.send(json.dumps(
                {"type": "session.close", "event_id": str(uuid.uuid4())}))
            while True:
                try:
                    raw = await asyncio.wait_for(ws.recv(), timeout=5)
                except (asyncio.TimeoutError, websockets.ConnectionClosed):
                    break
                evt = json.loads(raw)
                print(f"  <- {evt.get('type')}: "
                      f"{json.dumps(evt, ensure_ascii=False)[:200]}")
                if evt.get("type") == "session.closed":
                    break
            print("OK: session.created 拿到，session.close 优雅关闭")
            return 0
    except websockets.InvalidStatusCode as e:
        print(f"FAIL: 握手被拒 HTTP {e.status_code}, headers={dict(e.headers)}")
        return 1
    except Exception as e:
        print(f"FAIL: {type(e).__name__}: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(asyncio.run(probe()))
