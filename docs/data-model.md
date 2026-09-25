# 数据建模

帧的二进制格式见 `protocol.md`，本文档建模**信令 JSON 的 schema** 和**服务端数据实体**。

## 1. 信令 Schema（type=0x02 帧的 payload）

所有信令共享一个 envelope：

```json
{ "type": "<信令名>", "ts": 1758537600, ... }
```

`ts`（Unix 秒）由服务端 `publish_json` 自动补齐；设备收到即对时（状态栏时钟，PCF85063 RTC 断电走时 + 信令自校准）。

### 设备上行

| 信令 | 字段 | 说明 |
|---|---|---|
| `hello` | `device: string`, `fw: string` | 连接建立后第一帧，上报设备 ID 与固件版本 |
| `rec_start` | — | 开始录音（UI 已进录音态） |
| `rec_stop` | `duration_ms: int` | 结束录音。音频帧流可在 rec_stop 前**或**后到达（固件当前是录完随 rec_stop 上行）；服务端以 rec_stop 后的静默窗口（默认 2s）收齐帧再触发 ASR，两种顺序都兼容 |
| `link` | `mqtt: bool` | **App→设备本地信令（BLE 直发，不经服务器；ADR-038）**：手机侧 MQTT 链路状态。BLE 每次就绪先推一次，连/断变化即推；设备按键预检用（false 时提示"手机没连上服务器"并拦下录音/talk） |
| `talk_request` | `provider?: string` | 请求实时对话；`provider` = 对话引擎（`volc`/`step`…，ADR-035）——**选择权在小设备**（设置页），省略时服务端用默认后端 |
| `talk_end` | — | 主动结束实时对话（M5） |
| `ping` | — | 应用层心跳（MQTT keepalive 之外的活性证明） |

### 服务器下行

| 信令 | 字段 | 说明 |
|---|---|---|
| `hello_ack` | —（envelope `ts`） | hello 应答：设备凭 `ts` 校准状态栏时钟（软件时钟兜底时的唯一对时源） |
| `receipt` | `text: string`, `msg_id?: string` | 消息已送达飞书。`text` = ASR 终稿 → 消息卡"你："一栏；设备显示"✓ 已送达"便签 |
| `reply` | `text: string`, `msg_id?: string` | Hermes 回复（服务端已 `flatten_markdown` 拍平为纯文本，见 textfmt.py）→ 消息卡"GAGA："一栏就地填充 + 叮咚；息屏时自动亮屏直达该卡详情页。回流已落地（官方 API 长连接，ADR-029）；`msg_id` 预留给发送侧换 Bot API 后的问答复配（当前 FIFO） |
| `talk_ready` | `session: string` | 实时会话已建立，携带火山 session.id。**ws_url 字段废弃**：M5 实际音频走 MQTT 帧透传（ADR-021），不开 WebSocket |
| `talk_asr` | `text: string`, `final: bool` | 实时对话中用户语音的 ASR 文本（上屏）。流式 interim 可能有交叠（双跑次特性），`final=true` 为干净终稿 |
| `talk_reply` | `text: string`, `final: bool` | 模型回复文本（上屏）。流式节流 ~300ms，`final=true` 为整句 |
| `talk_end` | `reason: string` | 实时会话已结束。reason：`exit_intent`（退出意图）/ `device_end`（设备主动）/ `timeout` / `timeout_max` / `error` |
| `error` | `code: string`, `msg: string` | 错误码 + 人类可读信息 |

错误码约定：`ASR_FAIL`（识别失败/空文本/音频解码失败）、`CHANNEL_FAIL`（接入端投递失败，ADR-030；旧名 `WEBHOOK_FAIL` 随 webhook 一起退役）、`NOT_IMPLEMENTED`（功能未启用，如未配置 VOLCENGINE_API_KEY 时的 talk_request）、`BUSY`（实时对话进行中又收到 talk_request/rec_start）、`REALTIME_FAIL`（火山实时会话建立失败或中途出错）、`AUTH_FAIL`、`RATE_LIMIT`。

### 设备端消息卡（M6 反馈闭环 UI，fw-idf 0.3.0）

一条消息 = 一次"按住说话"问答（发送占位 → receipt 填 ASR → reply 填回复）。固件 RAM 环形缓冲 20 条（约 13KB），重启即清；问答复配按 FIFO（receipt 认最早"发送中"，reply 认最早"等待回复"），`msg_id` 精确复配留给飞书官方 API 阶段。状态机：`Sending → Waiting → Replied / Failed`，Waiting 超 60s 显示"（还没回复）"（软超时，迟到 reply 照样点亮）。

> 备注（2026-09-23，ADR-022）：M1 录音链路升级为火山流式 ASR 直通后**信令零变化**
> （rec_start/rec_stop/receipt/error 语义不变），只是服务端内部从"攒段批处理"改为
> "帧边到边转发 + rec_stop 帧齐即收尾"。设备/App 无感知。

## 2. 音频数据

| 项 | 值 | 备注 |
|---|---|---|
| 编码 | Opus | 16kbps 语音质量足够，BLE 带宽友好 |
| 采样率 | 16kHz 单声道 | ASR 场景标准；音乐场景再议 |
| 帧长 | 20ms/帧 | 320 采样/帧 |
| 传输 | type=0x01 二进制帧 | 一帧 Opus 一个传输帧，不打包 |
| talk 下行音频 | ogg_opus 24kHz 分片 | M5 实时对话的模型回复：同为 type=0x01 帧，但内容是火山下行的 Ogg 分片（拼接即完整 Ogg 流），与上行裸包不同（ADR-021） |

## 3. 服务端实体（持久化）

服务器用 SQLite/PostgreSQL 均可，实体如下：

### device（设备）
| 字段 | 类型 | 说明 |
|---|---|---|
| id | string PK | 如 `gaga-01` |
| fw_version | string | 最近 hello 上报的固件版本 |
| last_seen_at | datetime | 最近一次在线时间 |

### recording（录音记录）
| 字段 | 类型 | 说明 |
|---|---|---|
| id | uuid PK | |
| device_id | string FK | |
| audio_path | string | 原始音频存档路径（可选保留） |
| duration_ms | int | |
| transcript | text | ASR 结果 |
| channel_msg_id | string | 接入端消息 id（channel.send_text 返回；ADR-030） |
| status | enum | `received / transcribed / delivered / failed` |
| created_at | datetime | |

### session（实时对话会话，里程碑 4 才用）
| 字段 | 类型 | 说明 |
|---|---|---|
| id | uuid PK | |
| device_id | string FK | |
| provider | string | 如 `volcengine-realtime` |
| started_at / ended_at | datetime | |
| turns | int | 对话轮数 |

## 4. MQTT Topic 规划

| Topic | 方向 | QoS | retain |
|---|---|---|---|
| `gaga/up` | App/设备 → server | 1 | 否 |
| `gaga/down` | server → App/设备 | 1 | 否 |

多设备扩展：`gaga/{device_id}/up|down`。当前单设备阶段用无前缀版本。
