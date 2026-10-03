# 通信协议规范

App 是哑管道：BLE 收到的字节原样转发到服务器，服务器下发的字节原样写到 BLE。帧格式定义在两端（设备固件 ↔ 服务器），App 不解析内容。实现细节与历史决策见 `.agents/decisions.md`（ADR）。

## 1. BLE 链路（ESP32 ↔ App）

ESP32 是 GATT Server（外设），App 是 Central（中心），App 主动扫描连接。

| 项 | 值 |
|---|---|
| Service UUID | `6e400001-b5a3-f393-e0a9-e50e24dcca9e`（NUS 标准串口服务） |
| TX 特征（Notify，设备 → App） | `6e400003-b5a3-f393-e0a9-e50e24dcca9e` |
| RX 特征（Write，App → 设备） | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` |
| MTU | 517 |
| 设备广播名 | `GAGA-XXXX`（XXXX = MAC 后四位） |

## 2. 帧格式

```
[1B type][2B payload_len (big-endian)][payload]
```

| type | 含义 | payload |
|---|---|---|
| 0x01 | 音频帧 | Opus 包 |
| 0x02 | JSON 信令 | UTF-8 JSON 文本 |

分片规则：payload 超 509B 时首包 `type|0x80`（len = **整帧 payload 总长度**）、中间包 `type=0x00`（len = 本包长度）、尾包原 type；接收端凑满总长即重组。分片帧内不允许插入其他帧。

## 3. JSON 信令（type=0x02）

所有信令共享 envelope：`{"type":"<信令名>","ts":1758537600,...}`。`ts`（Unix 秒）由服务端自动补齐，设备收到即对时。

### 3.1 设备/App → 服务器（gaga/up）

| 信令 | 字段 | 干什么用 |
|---|---|---|
| `hello` | `device`, `fw`, `name?` | 上线握手：上报设备 ID、固件版本、显示名 |
| `rec_start` | `device?`, `name?`, `rec_seq?` | 开始录音 |
| `rec_stop` | `duration_ms` | 结束录音；服务端以 rec_stop 后 2s 静默窗口收齐音频帧再触发 ASR |
| `rec_status_query` | `device?` | **对账查询**：消息卡"识别中"超 10s 未收到结果时每 10s 重发（退避 10/20/30s）；服务端幂等重发**最近一条**结果信令。已知缺口：不带 `rec_seq`，多张卡在等时会错配（升级方向：查询带 seq、服务端按 seq 回） |
| `talk_request` | `device?`, `provider?` | 请求实时对话；`provider` 省略 = 服务端默认引擎 |
| `talk_end` | — | 主动结束实时对话 |
| `ping` | — | 应用层心跳 |

### 3.2 服务器 → 设备（gaga/down）

| 信令 | 字段 | 干什么用 |
|---|---|---|
| `hello_ack` | `ts` | hello 应答 + **下行心跳**（服务端每 30s 周期性发一帧；App 凭"35s 无 hello_ack = 僵尸连接"判死重建，设备纯对时） |
| `receipt` | `text`, `msg_id?`, `rec_seq?` | ASR 终稿（消息卡"你："栏）+ 送达确认 |
| `reply` | `text`, `reply_to?` | Hermes 回复（已拍平为纯文本）→ 消息卡"GAGA："栏 |
| `error` | `code`, `msg`, `rec_seq?` | 失败信息 → 卡片标"发送失败"。code 见下表 |
| `notify` | `device`, `text`, `id` | 主动 TTS 通知开始（ADR-066）；随后 type=0x01 帧是通知音频 |
| `notify_end` | `device`, `id` | 通知音频流结束 |
| `talk_ready` | `session` | 实时会话已建立 |
| `talk_asr` | `text`, `final` | 实时对话用户语音 ASR（上屏；空 text = 新一句开始） |
| `talk_reply` | `text`, `final` | 模型回复文本（流式上屏） |
| `talk_end` | `reason` | 会话结束：`exit_intent` / `device_end` / `timeout` / `timeout_max` / `error` |

错误码：`ASR_FAIL`（识别失败/空内容）、`CHANNEL_FAIL`（接入端投递失败）、`NOT_IMPLEMENTED`、`BUSY`（对话中冲突请求）、`REALTIME_FAIL`、`AUTH_FAIL`、`RATE_LIMIT`。

下行音频边界：talk 回复音频在 `talk_ready`↔`talk_end` 之间、通知音频在 `notify`↔`notify_end` 之间，均为 type=0x01 的 ogg_opus 24kHz 分片（拼接即完整 Ogg 流），靠 JSON 信令界定边界。非本机 `device` 戳的信令设备忽略。

### 3.3 App 本地信令（BLE 直发，不经服务器）

| 信令 | 方向 | 字段 | 干什么用 |
|---|---|---|---|
| `link` | App → 设备 | `mqtt: bool` | 手机 MQTT 链路状态；设备按键预检拦"手机没连上服务器" |
| `set_name` | App → 设备 | `name` | 设置设备显示名（空 = 清除） |
| `wifi_cfg` | App → 设备 | `ssid`, `pass`, `host?`, `port?`, `mqtt_user?`, `mqtt_pass?` | 家模式 WiFi + broker 配置（host 缺省 = 不启 MQTT） |

### 3.4 rec_seq（段序号）

一次录音 = 一段。`rec_seq` 是设备侧分配的 uint32 单调递增计数（持久化、重启不回绕），随 `rec_start` 上报，服务端在 `receipt`/`error` 中原样回显。设备凭它把结果信令**精确配对**到对应消息卡：只配同 seq 的"识别中"卡，找不到即丢弃（重放幂等）；不带 seq 的旧信令回落 FIFO 配对。

## 4. MQTT 主题

| Topic | 方向 | QoS | retain |
|---|---|---|---|
| `gaga/up` | App → 服务器 | 1 | 否 |
| `gaga/down` | 服务器 → App | 1 | 否 |

- clientId：`app-<androidId>`（App）/ `server`（服务端）/ `watch-<deviceId>`（手表）
- broker 公网暴露必须开 username/password 鉴权
- 手表上行发单包完整帧（不分片），下行照常分片
- 多设备扩展：`gaga/{device_id}/up|down`

## 5. 音频数据

| 项 | 值 |
|---|---|
| 编码 / 采样率 / 帧长 | Opus 16kbps / 16kHz 单声道 / 20ms 帧（一帧 Opus 一个 0x01 帧） |
| talk / notify 下行音频 | ogg_opus 24kHz 分片（服务器 2KB/帧、60ms/帧 pacing） |

## 6. 服务端实体（持久化）

**device**：`id` PK、`fw_version`、`last_seen_at`

**recording**：`id` PK、`device_id` FK、`duration_ms`、`transcript`、`channel_msg_id`、`status`（received/transcribed/delivered/failed）、`created_at`

**session**（实时对话）：`id` PK、`device_id` FK、`provider`、`started_at/ended_at`、`turns`

## 7. 设备端消息卡

一次录音 = 一张卡：`Sending → Waiting → Replied / Failed`。Waiting 超 60s 显示"（还没回复）"、Sending 超 60s 显示"识别超时，仍在等…"（软超时，迟到结果照样点亮）。配对规则见 §3.4。

## 8. 链路流程

**发消息（M1）**：按键开录 → `rec_start` → 上行 Opus 帧 → `rec_stop` → 服务器 ASR → 接入端发飞书 → `receipt`（卡填 ASR 文本）→ Hermes 群里回复 → `reply`（卡填回复 + 叮咚）

**实时对话（M5）**：双击 → `talk_request` → 服务器建连火山全双工 → `talk_ready` → 设备持续上行 16k Opus → 火山下行 ASR/回复文本 + 回复音频 → 退出意图 / `talk_end` / 超时 → `talk_end`

**主动通知（ADR-066）**：调用方 `POST /notify {device, text}` → 服务器 TTS → `notify` → TTS 音频帧流 → `notify_end` → 设备播放（录音/talk 中到达只上屏不播）
