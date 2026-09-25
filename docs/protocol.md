# 通信协议规范

App 是哑管道：BLE 收到的字节原样转发到服务器，服务器下发的字节原样写到 BLE。帧格式定义在两端（设备固件 ↔ 服务器），App 不解析内容。

## 1. BLE 链路（ESP32 ↔ App）

ESP32 是 GATT Server（外设），App 是 Central（中心），App 主动扫描连接。

Service UUID: `6e400001-b5a3-f393-e0a9-e50e24dcca9e`（NUS 风格自定义服务）

| Characteristic | UUID | 属性 | 方向 | 内容 |
|---|---|---|---|---|
| TX | `6e400003-b5a3-f393-e0a9-e50e24dcca9e` | Notify | 设备 → App | 帧数据 |
| RX | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` | Write | App → 设备 | 帧数据 |

MTU 协商到 517；单帧超过 MTU-3 时按 `docs` 下述分帧规则切分。

设备广播名：`GAGA-XXXX`（XXXX = MAC 后四位），App 按名称前缀 + Service UUID 过滤。

## 2. 帧格式（设备 ↔ 服务器，App 不解析只转发）

```
[1B type][2B payload_len (big-endian)][payload]
```

| type | 含义 | payload |
|---|---|---|
| 0x01 | Opus 音频帧 | 原始 Opus 包 |
| 0x02 | JSON 信令 | UTF-8 JSON 文本 |

BLE 单包放不下时，分帧规则：第一包 bit7 置 0 的 type 加 0x80 标记"还有更多"，最后一包正常 type；中间包 type=0x00。App 转发前按 MTU 切分物理包，重组在接收端。

分包实现细则（esp32-idf/src/protocol/frame.cpp 为准）：
- 每个 BLE 包都带 3 字节帧头 `[1B type 字段][2B len BE]`
- 首包：type|0x80，len = **整帧 payload 总长度**（接收端据此预分配缓冲）
- 中间包：type=0x00，len = 本包 payload 字节数
- 尾包：type=原始 type，len = 本包 payload 字节数
- 单包帧：type 不置 0x80，len = payload 长度
- 接收端凑满首包宣告的总长度即重组完成；超出总长度视为对端异常，丢弃残帧

## 3. JSON 信令（type=0x02）

设备上行：
- `{"type":"hello","device":"gaga-01","fw":"0.1.0"}` —— 服务器回 `hello_ack`（纯对时，envelope ts）

服务器下行：
- `{"type":"hello_ack","ts":1758537600}` —— hello 应答：设备凭 envelope `ts` 校准状态栏时钟
- `{"type":"rec_start"}` / `{"type":"rec_stop","duration_ms":8200}`
- `{"type":"talk_request","provider":"volc"}` —— 请求实时对话；`provider` 可选（设备设置页选对话引擎，ADR-035），省略 = 服务端默认
- `{"type":"link","mqtt":true|false}` —— **App 本地信令（不经服务器，ADR-038）**：App 把自己的 MQTT 连接状态经 BLE 推给设备；BLE 每次就绪先推一次当前状态，之后连/断即刻推送。设备据此在按键预检时拦下"手机没连上服务器"的情形
- `{"type":"wifi_cfg","ssid":"...","pass":"..."}` —— **App 本地信令（ADR-039）**：家模式 WiFi 凭证下发。App 表单经 BLE 推给设备，设备存 NVS（B 期 WiFi 直连 MQTT 时读取）；密码仅存设备 NVS，服务器不经手
- `{"type":"talk_end"}` —— 主动结束实时对话（M5）

服务器下行：
- `{"type":"receipt","text":"今天天气怎么样","ts":1758537600}` —— 消息已送达飞书；`text` = ASR 终稿（设备消息卡"你："一栏凭它显示），`ts` = Unix 秒（设备对时，envelope 自动补）
- `{"type":"reply","text":"晴，25℃","msg_id":"..."}` —— Hermes 回复（**已拍平为纯文本**，服务端 textfmt.flatten_markdown 处理掉 Markdown 标记），设备消息卡"GAGA："一栏就地填充；息屏时设备自动亮屏直达该卡详情页
- `{"type":"talk_ready","session":"..."}` —— 实时会话已建立（M5：音频仍走 MQTT 帧透传，无 ws_url，ADR-021）
- `{"type":"talk_asr","text":"...","final":false}` —— 实时对话中用户语音的 ASR 文本（上屏；final=true 为终稿）。**text 为空串 = 新一句开始**（ADR-034：实时 ASR 快照分层含上一句残留，服务端在新一句开始时先发空文本，设备清掉上一句字幕再显示新句）
- `{"type":"talk_reply","text":"...","final":false}` —— 模型回复文本（上屏；流式节流 ~300ms，final=true 为整句）
- `{"type":"talk_end","reason":"exit_intent|device_end|timeout|timeout_max|error"}` —— 实时会话已结束
- `{"type":"error","code":"...","msg":"..."}` —— 消息卡标"发送失败：msg"

所有下行信令 envelope 自动带 `ts`（Unix 秒）：设备状态栏时钟凭它对时（PCF85063 断电走时，开机/每次信令自校准）。

**talk 会话期间的下行音频**：type=0x01 帧承载的是模型回复音频（ogg_opus 24kHz 分片，
按到达顺序拼接即完整 Ogg 流），与上行 16kHz/20ms 裸 Opus 包不是同一种内容，设备播放侧注意区分。

## 4. MQTT 主题（App ↔ 服务器，户外模式）

Broker 由服务器提供。

| Topic | 方向 | 内容 |
|---|---|---|
| `gaga/up` | App → Broker | 设备上行帧（原样字节） |
| `gaga/down` | Broker → App | 服务器下行帧（原样字节） |

- QoS 1，不 retain
- App clientId: `app-<androidId>`，keepalive 60s
- 服务器 clientId: `server`

## 5. 里程碑 1 链路（语音发飞书）

```
按住右下键说话 → 松开 → 消息卡占位上屏（你：识别中…）→ 设备 Opus 编码 → BLE 分帧上行
→ App 转发 MQTT gaga/up → 服务器重组、解 Opus
→ Whisper ASR → 接入端投递（channel.send_text，ADR-030；飞书=官方 API）→ receipt{text,ts,msg_id} 回设备
→ 消息卡填"你：<ASR 文本>"，转"正在回复…"（60s 无回复转"（还没回复）"）
→ Hermes 在群里回复 → 接入端回流（feishu 轮询 im/v1/messages，ADR-029/030）→ reply{text} 回设备
→ 消息卡就地填"GAGA：<回复>" + 叮咚；息屏时自动亮屏直达该卡详情页
```

## 6. 里程碑 5 链路（实时对话，ADR-021）

```
双击（或长按右上键）→ talk_request → 服务器建连豆包 Seeduplex 全双工
→ session.create（上行 speech_opus 16k，下行 ogg_opus 24k）→ talk_ready
→ 设备持续上行 Opus 帧（20ms 实时节奏；服务端节奏器平滑后直通火山）
→ 火山下行：ASR 文本（talk_asr）/ 回复文本（talk_reply）上屏，
  回复音频（ogg_opus 分片，type=0x01 下行帧）设备播放
→ 结束：退出意图（20000002）/ 设备 talk_end / 超时 / error → talk_end 下行
```

打断：设备上行帧照常发（全双工模型自己判打断）；显式打断由服务器发 `response.cancel`。
