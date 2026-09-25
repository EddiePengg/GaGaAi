# server — gaga ai 服务端

MQTT 桥 + 帧重组 + Whisper ASR + 接入端 channels/（飞书官方 API 收发，可扩微信/Telegram，ADR-030）+ 豆包实时对话桥（M5）。系统唯一的大脑（AGENTS.md 宪法）。
里程碑 M1 已跑通：`curl 上传录音 → ASR → 飞书群出文字 → MQTT 回执`。
M5 服务端已跑通：`talk_request → 豆包 Seeduplex 全双工 → 回复语音/文本下行`（ADR-021）。

> ⚠️ **公网警告**：本期无鉴权（MQTT 匿名 + HTTP 调试口裸奔），前提仅限本机 +
> 局域网 + 自托管内网。公网暴露**之前**必须补：MQTT username/password 或 TLS、
> HTTP 口鉴权或干脆不暴露端口。

## 本地起法（macOS，无 Docker）

```bash
brew install mosquitto ffmpeg                       # 一次性
/opt/homebrew/sbin/mosquitto -c mosquitto/mosquitto.conf &   # 起 broker（1883）

uv venv --python 3.12 && uv pip install -e .        # 建环境（不要用系统 Python 3.9）
cp .env.example .env                                 # 填 FEISHU_APP_ID/SECRET（CHANNEL=feishu）
.venv/bin/gaga-server                                # 起服务（首次会下载 whisper small 模型 ~460MB）
```

服务包含两部分（同进程）：MQTT 桥（订 `gaga/up` / 发 `gaga/down`）+ HTTP 调试口（:8000）。

## 验证（M1 三层，2026-09-22 真跑结果）

### a. HTTP 层

```bash
# 造测试音频（macOS TTS；任何 m4a/wav/mp3 都行）
say -v Tingting "测试一下 gaga 语音识别，鸭子你好" -o test.m4a

curl -F file=@test.m4a localhost:8000/debug/audio
# → {"ok":true,"text":"测试一下高高语音识别,压子你好。"}
# → 飞书群收到：🦆 测试一下高高语音识别,压子你好。
```

（small 模型对 TTS 合成音会把 "gaga" 听成 "高高"，链路正确；真人语音更准。）

### b. MQTT 层（模拟设备，App 联调预演）

```bash
# 把音频编码成 Ogg Opus（16kHz 单声道 16kbps 20ms/帧，与设备参数一致）
ffmpeg -i test.m4a -ar 16000 -ac 1 -c:a libopus -b:a 16k -frame_duration 20 test.opus

.venv/bin/python scripts/simulate_device.py --file test.opus
# 发 hello → rec_start → 166 个 Opus 帧（按 100B 分包走重组路径）→ rec_stop
# → 飞书群收到：🦆 [gaga-sim] 各式依下高高语音识别,鸭子你好。（16kbps Opus 压缩略损音质）
# → gaga/down 收到 {"type":"receipt"}，脚本 exit 0
```

旁路观察回执（脚本之外独立验证）：

```bash
mosquitto_sub -t gaga/down -v   # 会收到带 3 字节帧头的 {"type":"receipt"}
```

### c. 信令分派

`talk_request` → 已配 `VOLCENGINE_API_KEY` 时建立实时会话回 `talk_ready`（见下节）；
未配置时回 `{"type":"error","code":"NOT_IMPLEMENTED","msg":"实时对话未启用…"}`。

## 流式 ASR（ADR-022，M1 提速）

`ASR_PROVIDER=volc_stream` + `VOLCENGINE_API_KEY` 即启用：录音一开始就把 Opus 帧
流式转发火山（sauc 双向流式优化版，200ms 聚组增量 Ogg 直发），rec_stop 帧齐即发
空负包收尾拿终稿；任何环节失败自动降级本地 whisper 批处理（帧缓冲全程保留）。

```bash
# 流式链路探针（独立验证 sauc 协议/授权/延迟，不经 MQTT）
.venv/bin/python scripts/asr_stream_probe.py
```

**2026-09-23 实测**（`resource=volc.bigasr.sauc.duration`，ASR 1.0 小时版）：

| 场景 | rec_stop→终稿 | 备注 |
|---|---|---|
| 本地 whisper small（M1 批处理，对照） | ~3.7s | 2s 静默窗 + ~1.7s CPU 推理 |
| volc_stream，模拟突发上行 | 1.57s | 帧已齐立即收尾 + 火山 ~1.4s |
| volc_stream，真实节奏 paced | 2.17s | 走满 2s 静默窗 + 火山 0.17s |
| volc_stream，真机人声（鸭子麦克风） | 2.15s | BLE 丢包致帧数<声明时长 → 走满窗口；"能听到我说话吧？鸭子。" 识别一字不差 |

降级路径实测：把 `VOLC_ASR_RESOURCE_ID` 指向未授权资源（403 `requested resource
not granted`）→ 日志 "流式 ASR 失败…降级本地 whisper" → 本地 whisper 出字、
飞书/receipt 正常。录音中途失败则攒帧到收尾再降级。

⚠️ **ASR 2.0 未开通**：`volc.seedasr.sauc.*` 当前 403。要用 Seed-ASR 2.0（准确率更高、
二遍识别）需在控制台开通"豆包流式语音识别模型2.0"后改 `VOLC_ASR_RESOURCE_ID`。

**多段连按语义（2026-09-23 修正）**：每段录音是独立任务。新 `rec_start` 到达时上一段
若还在收尾（静默窗内/流式等终稿），会**立即收尾并照常投递**（日志 "新 rec_start 到达，
上一段立即收尾"），不作废。帧归属按时间序天然可分（paho 单线程顺序回调）：rec_stop
后静默窗内的帧属旧段，rec_start 后的帧属新段。回归测试：
`.venv/bin/python scripts/simulate_two_recs.py`（两段连发，各自出字 + 各收 receipt）。

## 实时对话（M5，豆包 Seeduplex 全双工，ADR-021）

`.env` 配 `VOLCENGINE_API_KEY`（控制台 > API Key 管理）即启用；默认音色 Vivi 2.0。

```bash
# 连通性探针（只建会话不发音频，不耗对话时长）
.venv/bin/python scripts/realtime_probe.py

# 全链路联调（真实调用火山 API，计费！默认两轮短对话；--skip-exit-test 只跑一轮）
.venv/bin/python scripts/simulate_talk.py
```

simulate_talk.py 模拟设备：talk_request → talk_ready → say 生成的问题语音按 20ms
节奏上行 Opus 帧 → 收下行 talk_asr / talk_reply 文本 + 音频帧落盘
（`test-audio/talk_reply_tN.ogg`）→ ffmpeg 解码 + 本地 whisper 回识验证内容 →
第二轮告别语测退出意图 → talk_end 收尾。

**2026-09-23 实测结果**（五轮真实 API）：
- 连接/session.create/优雅关闭全部正常；模型遵循 instructions（自我介绍说"我是挂在你胸前的语音助手 gaga"）
- 下行音频拼接后 ffmpeg 解码正常，whisper 回识与 talk_reply 文本一致
- 上行 Opus 原包直通可行（say 合成的 16k opus 帧，服务端节奏器转发，无节奏错误）
- 上行断流自动 mute 保活、来帧自动 unmute 已验证
- 已知项：① ASR 流式 interim 有交叠（双跑次特性，终稿干净）；② 退出意图
  status_code=20000002 五轮未触发（say 合成音韵律疑似不足，处理代码已就位，真机人声再验）

talk 会话期间下行 type=0x01 帧是 **ogg_opus 24kHz 分片**（模型回复音频），
与上行的 16kHz 裸 Opus 包不同——设备播放侧注意区分（protocol.md §3）。

## 生产部署（dokploy）

```bash
cp .env.example .env   # 填 FEISHU_APP_ID/SECRET
docker compose up -d   # mosquitto + server 两个服务，whisper 模型挂 volume 只下载一次
```

## 行为要点（实现与协议的对应关系）

- **帧重组**（`frames.py`）：严格按 `docs/protocol.md` §2 分包细则，语义与
  `esp32/src/protocol/frame.cpp` 一致；首包后 5s（`FRAME_TIMEOUT`）未凑齐丢弃残帧。
- **录音时序**（`session.py`）：音频帧流可能在 `rec_stop` 前**或**后到达
  （固件是录完随 rec_stop 上行）。服务端不假设顺序：rec_stop 后开静默窗口
  （`REC_FLUSH_TIMEOUT` 默认 2s），窗口内帧全部纳入，窗口结束触发 pipeline。
- **Opus 解码**（ADR-015）：设备上行裸 Opus 包 → `ogg.py` 封装 Ogg → ffmpeg 子进程
  解成 16k PCM。HTTP 口上传的任意格式也走 ffmpeg，两链在 PCM 后完全共用。
- **回执**：全部经 MQTT `gaga/down` 下行，帧化（type=0x02）后 App 原样转发给设备。
  成功 → `receipt`（带接入端 msg_id）；空文本/识别失败 → `error(ASR_FAIL)` 不投递；接入端失败 → `error(CHANNEL_FAIL)`。
- **ASR 可切换**（ADR-013 / ADR-022）：`ASR_PROVIDER=local_whisper`（本地批处理兜底）/
  `volcengine`（占位未实现）/ `volc_stream`（**火山流式直通，推荐**——rec_start 即开
  sauc 会话，帧边到边转发，rec_stop 帧齐即发空负包收尾；失败自动降级 local_whisper）。
  HTTP 调试口永远走本地 whisper（输入是整段文件）。

## 配置（.env，模板见 .env.example）

| 变量 | 默认 | 说明 |
|---|---|---|
| `CHANNEL` | `feishu` | 接入端（ADR-030，openclaw channels 模式）；新平台见 channels/__init__.py |
| `FEISHU_APP_ID` / `FEISHU_APP_SECRET` | （必填） | 飞书自建应用凭证（ADR-029/030）：官方 API 收发 |
| `FEISHU_CHAT_ID` | 空=自动发现 | 目标群 chat_id（oc_ 开头）；仅当机器人在唯一群里可自动发现 |
| `FEISHU_REPLY_SENDER` | 空=任意应用消息 | 指定回复者（ou_ 用户 / cli_ 应用；本仓 Hermes=cli_a9f77be0…）；空时只认应用(bot)消息 |
| `FEISHU_POLL_INTERVAL` | `2.0` | 消息轮询间隔（秒） |
| `ASR_PROVIDER` | `local_whisper` | ASR 提供方（ADR-013/022）：local_whisper / volcengine / volc_stream |
| `WHISPER_MODEL` | `small` | 模型档位（tiny/base/small/medium），volc_stream 时作兜底 |
| `WHISPER_LANGUAGE` | `zh` | 识别语言 |
| `VOLC_ASR_ENDPOINT` | sauc bigmodel_nostream | 流式 ASR 端点（单向，ADR-033） |
| `VOLC_ASR_RESOURCE_ID` | `volc.bigasr.sauc.duration` | ASR 1.0 小时版；2.0 需控制台开通后切换 |
| `MQTT_HOST` / `MQTT_PORT` | `127.0.0.1:1883` | broker 地址 |
| `HTTP_HOST` / `HTTP_PORT` | `127.0.0.1:8000` | 调试口监听 |
| `REC_FLUSH_TIMEOUT` | `2.0` | rec_stop 后收帧静默窗口（秒） |
| `FRAME_TIMEOUT` | `5.0` | 帧重组超时（秒） |
| `RECORDING_ARCHIVE_DIR` | `data/recordings` | 录音存档目录（原始 Opus .ogg；识别成功后文件名带转写摘要；空 = 关闭，ADR-037） |
| `FFMPEG_BIN` | 自动探测 | ffmpeg 路径 |
| `VOLCENGINE_API_KEY` | （M5 必填） | 火山 API Key，不配则 talk_request 回 NOT_IMPLEMENTED |
| `REALTIME_ENABLED` | `true` | 实时对话总开关 |
| `REALTIME_PROVIDER` | `volc` | 默认对话后端（volc/gemini/step）；设备 talk_request.provider 优先（ADR-035） |
| `STEP_API_KEY` / `STEP_MODEL` / `STEP_VOICE` | — | 阶跃星辰 Realtime 凭证与参数（ADR-035） |
| `REALTIME_PROVIDER` | `volc` | 实时后端：`volc` 豆包（已联调）/ `gemini` Gemini Live（实现完整，需 `GEMINI_API_KEY` + `uv pip install pyogg`，未联调）。新后端（如"星辰"）三步接入：实现 `realtime/base.py` 契约 → `realtime/__init__.py` 注册一行 → decisions.md 记协议事实（ADR-034） |
| `GEMINI_API_KEY` / `GEMINI_MODEL` | 空 / `models/gemini-2.0-flash-live-001` | Gemini provider 凭证与模型 |
| `REALTIME_ENDPOINT` | 见 .env.example | 豆包全双工 wss 端点 |
| `REALTIME_VOICE` | `zh_female_vv_uranus_bigtts` | Vivi 2.0；音色列表见火山文档 |
| `REALTIME_INSTRUCTIONS` | 内置胸前助手人格 | 系统提示词 |
| `REALTIME_IDLE_TIMEOUT` | `120` | talk 空闲超时（秒） |
| `REALTIME_MAX_DURATION` | `300` | talk 单会话最长时长（秒） |

## 目录

```
server/
├── pyproject.toml          # uv 项目（paho-mqtt / fastapi / uvicorn / faster-whisper / httpx）
├── Dockerfile              # 生产镜像（python:3.12-slim + ffmpeg）
├── docker-compose.yml      # mosquitto + server（生产/联调）
├── mosquitto/mosquitto.conf
├── .env.example            # 配置模板
├── scripts/simulate_device.py   # 模拟设备录音链路（M1 联调）
├── scripts/simulate_two_recs.py # 两段连发回归（多段收尾语义，ADR-022 修正）
├── scripts/simulate_talk.py     # 模拟设备实时对话（M5 联调，真实调用火山 API）
├── scripts/realtime_probe.py    # realtime 连通性探针（不计对话时长）
├── scripts/asr_stream_probe.py  # 流式 ASR 探针（sauc 协议/授权/延迟）
├── scripts/replay_serial_recording.py  # 真机串口录音回放（旧固件 dump 模式用）
└── src/gaga_server/
    ├── main.py             # 入口：MQTT 桥 + HTTP 调试口
    ├── config.py           # env / .env 配置加载
    ├── frames.py           # 帧编解码与重组（protocol.md §2）
    ├── ogg.py              # 裸 Opus 包 ↔ Ogg 封装/解包（ADR-015）+ OggStreamWriter 增量封装（ADR-022）
    ├── session.py          # 录音会话装配 + 信令分派 + talk 路由 + 流式/批处理双链路
    ├── pipeline.py         # 解码(ffmpeg) → ASR → 接入端投递 编排
    ├── mqtt_bridge.py      # 订阅 gaga/up、发布 gaga/down（JSON 信令 + 音频帧）
    ├── channels/           # 接入端抽象（ADR-030，openclaw 模式）：CHANNEL 选平台
    │   ├── base.py         #   Channel 抽象（send_text 出 / on_message 入）
    │   └── feishu.py       #   飞书：官方 API 发 + 消息轮询收（ADR-029/030）
    ├── realtime/           # M5：豆包全双工桥（ADR-021）
    │   ├── client.py       #   协议封装（session.create/append/mute/cancel/close）
    │   └── bridge.py       #   TalkBridge：生命周期 + 20ms 节奏器 + 下行分发
    ├── asr/                # provider 工厂 + local_whisper + volcengine 占位
    │   ├── sauc.py         #   火山流式 ASR 二进制协议编解码（ADR-022）
    │   └── volc_stream.py  #   流式 provider：帧边到边转发 + 空负包收尾 + 降级
    └── http_api.py         # POST /debug/audio
```

详见 ../docs/protocol.md（帧/信令/MQTT）、../docs/data-model.md（schema/错误码）、
../docs/decisions.md（ADR-012~015、021、022）。
