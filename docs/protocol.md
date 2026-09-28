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
- 首包：type|0x80，len = **整帧 payload 总长度**（接收端据此预分配缓冲）；**首包自带 payload，恒为 509B（512−3 帧头；分片 ⇒ 总长 >509 ⇒ 首包必满）**——接收端收完首包 509B 再读下一个子包头（ADR-067：漏收首包 payload 曾是潜伏至今的解码器 bug）
- 中间包：type=0x00，len = 本包 payload 字节数
- 尾包：type=原始 type，len = 本包 payload 字节数
- 单包帧：type 不置 0x80，len = payload 长度
- 接收端凑满首包宣告的总长度即重组完成；超出总长度视为对端异常，丢弃残帧
- **分片帧内不允许插入任何其他帧**（单包帧插进分片流是协议级不可恢复的错位：续帧包与帧边界无法区分）。服务端下行已用 RLock 串行化：通知音频分片流推送期间，心跳/receipt/reply 一律排队（ADR-067）
- **写入边界无意义（ADR-062）**：设备端 `FrameDecoder` 是字节流状态机——一次 BLE 写入可含任意多帧的拼接，一帧也可跨多次写入。App 的合批层（12ms/50ms 窗口）与 MTU 切块都不对齐帧边界，这是协议的既定语义而非例外；按"一次写入=一个协议包"解析会静默丢弃合批里的后续帧（曾致 receipt/reply 断链一整天）。配套：残帧超 1s 无续包即重扫（续包丢失自愈）；帧头非法/超长（>8KB）即滑窗 Hunt 找下一个合法帧头（2026-09-28 新增，≤1 个物理包自愈，不再吞掉整流等超时）

## 3. JSON 信令（type=0x02）

设备上行：
- `{"type":"hello","device":"gaga-01","fw":"0.1.0"}` —— 服务器回 `hello_ack`（纯对时，envelope ts）
- **`device` 字段（ADR-057，2026-09-27）**：`rec_start` / `rec_stop` / `rec_status_query` 同样携带（如 `{"type":"rec_start","device":"gaga-01"}`）——多设备（手表 + 胸前）同群同 broker 时服务端按它记账与补发；下行 `receipt`/`reply`/`error` 由服务端盖同款戳，设备端非本机的信令直接忽略。不带 device 的旧信令维持兼容（回落 hello 登记设备）
- **`name` 字段（ADR-064，2026-09-27）**：`hello` / `rec_start` 可携带设备显示名（如 `{"type":"rec_start","device":"gaga-01","name":"胸前嘎嘎"}`）——服务端投递进群时用它署名（`🦆 [胸前嘎嘎] …`），不带则回退 device ID。名字的权威存储在设备端（ESP32 = NVS `dname` 键，手表 = 本地 prefs），随段刷新即改名即时生效
- `{"type":"set_name","name":"胸前嘎嘎"}` —— **App 本地信令（ADR-064）**：设置设备显示名。App 设置对话框经 BLE 推给设备，设备 JSON 安全化（引号/反斜杠/控制字符→空格，32B 截断）后存 NVS，屏幕便签回显"已改名"。空 name = 清除（回退设备 ID 署名）。同 wifi_cfg 模式：App 只当传令兵，不碰转发内容
- `{"type":"rec_status_query"}` —— **对账查询（2026-09-26）**：消息卡"识别中"超 10s 仍无 receipt/reply（回程可能在 BLE/MQTT 某跳丢了）时，设备每 10s 问一次；服务端幂等重发最近一条结果信令（receipt 或 reply），设备侧 fillAsk/fillReply 对重复信令安全。这是下行丢失的自愈通道

服务器下行：
- `{"type":"hello_ack","ts":1758537600}` —— hello 应答：设备凭 envelope `ts` 校准状态栏时钟
- `{"type":"rec_start"}` / `{"type":"rec_stop","duration_ms":8200}`
- `{"type":"talk_request","provider":"volc"}` —— 请求实时对话；`provider` 可选（设备设置页选对话引擎，ADR-035），省略 = 服务端默认
- `{"type":"link","mqtt":true|false}` —— **App 本地信令（不经服务器，ADR-038）**：App 把自己的 MQTT 连接状态经 BLE 推给设备；BLE 每次就绪先推一次当前状态，之后连/断即刻推送。设备据此在按键预检时拦下"手机没连上服务器"的情形
- `{"type":"wifi_cfg","ssid":"...","pass":"...","host":"...","port":1883,"mqtt_user":"...","mqtt_pass":"..."}` —— **App 本地信令（ADR-039 + 2026-09-27 家模式落地）**：家模式 WiFi 凭证 + broker 下发。`ssid`/`pass` 必填；`host`/`port`/`mqtt_user`/`mqtt_pass` 可选（缺省沿用旧值；host 缺省 = 不启 MQTT，仅连 WiFi）。App 表单经 BLE 推给设备，设备存 NVS 并持久化（2026-09-27 修复 Settings 只声明不读写的旧 bug）；密码仅存设备 NVS，服务器不经手。设备侧调试等价入口：串口 `'w'` + JSON 行
- `{"type":"talk_end"}` —— 主动结束实时对话（M5）

服务器下行：
- `{"type":"receipt","text":"今天天气怎么样","msg_id":"om_xxx","ts":1758537600}` —— 消息已送达飞书；`text` = ASR 终稿（设备消息卡"你："一栏凭它显示），`msg_id` = 平台消息 id（设备存卡上，reply 的 reply_to 凭它精确配对），`ts` = Unix 秒（设备对时，envelope 自动补）
- `{"type":"reply","text":"晴，25℃","reply_to":"om_xxx"}` —— Hermes 回复（**已拍平为纯文本**，服务端 textfmt.flatten_markdown 处理掉 Markdown 标记），设备消息卡"GAGA："一栏就地填充；息屏时设备自动亮屏直达该卡详情页。`reply_to` = 它引用的那条鸭子消息 id（2026-09-26 会话圈过滤后只转发回复嘎嘎的回流；设备优先按 reply_to 精确配对，miss 才 FIFO 兜底）
- `{"type":"talk_ready","session":"..."}` —— 实时会话已建立（M5：音频仍走 MQTT 帧透传，无 ws_url，ADR-021）
- `{"type":"talk_asr","text":"...","final":false}` —— 实时对话中用户语音的 ASR 文本（上屏；final=true 为终稿）。**text 为空串 = 新一句开始**（ADR-034：实时 ASR 快照分层含上一句残留，服务端在新一句开始时先发空文本，设备清掉上一句字幕再显示新句）
- `{"type":"talk_reply","text":"...","final":false}` —— 模型回复文本（上屏；流式节流 ~300ms，final=true 为整句）
- `{"type":"talk_end","reason":"exit_intent|device_end|timeout|timeout_max|error"}` —— 实时会话已结束
- `{"type":"notify","device":"gaga-01","text":"...","id":"n_..."}` —— **服务器主动通知（ADR-066）**：TTS 语音提醒开始。`text` = 已拍平的通知文本（设备消息卡"GAGA 提醒"一栏展示），`id` = 通知唯一号（`n_<毫秒时间戳>`）；随后到达的 type=0x01 帧是本次通知的 TTS 音频（ogg_opus 24kHz，见 §7）。非本机 `device` 的信令设备端忽略；talk 会话中或录音中到达时只上屏不播语音
- `{"type":"notify_end","device":"gaga-01","id":"n_..."}` —— 通知 TTS 音频流结束，设备排空播放后收尾关音频
- `{"type":"error","code":"...","msg":"..."}` —— 消息卡标"发送失败：msg"

所有下行信令 envelope 自动带 `ts`（Unix 秒）：设备状态栏时钟凭它对时（PCF85063 断电走时，开机/每次信令自校准）。

**talk 会话期间的下行音频**：type=0x01 帧承载的是模型回复音频（ogg_opus 24kHz 分片，
按到达顺序拼接即完整 Ogg 流），与上行 16kHz/20ms 裸 Opus 包不是同一种内容，设备播放侧注意区分。

**notify 通知期间的下行音频**：notify 与 notify_end 之间的 type=0x01 帧承载的是 TTS
语音（同样 ogg_opus 24kHz 分片、拼接即完整 Ogg 流，与 talk 下行同格式）。区分靠信令
边界而非帧内容：收到 notify 进通知播放态，收到 notify_end（或 10s 超时兜底）收尾。

## 4. MQTT 主题（App ↔ 服务器，户外模式）

Broker 由服务器提供。

| Topic | 方向 | 内容 |
|---|---|---|
| `gaga/up` | App → Broker | 设备上行帧（原样字节） |
| `gaga/down` | Broker → App | 服务器下行帧（原样字节） |

- QoS 1，不 retain
- App clientId: `app-<androidId>`，keepalive 60s
- 服务器 clientId: `server`
- **鉴权（ADR-061，2026-09-27）**：broker 公网暴露必须开 username/password（mosquitto `allow_anonymous false`，操作手册见 `server/README.md` 公网部署章节）。客户端凭据留空 = 匿名，仅限局域网/内网 broker。
- **手表客户端（ADR-051）**：clientId `watch-<deviceId>`，同样发 `gaga/up` / 订 `gaga/down`。手表没有 BLE，MQTT 也无 MTU 限制，所以**上行直接发单包完整帧**（不走 §2 的 0x80 分片）；服务端 `frames.py` 的 FrameAssembler 同时吃单包帧与分片帧，无需任何改动。**下行照常分片**：服务端按 512B/包分片下发（§2 语义），手表端用 `Frame.Reassembler` 重组（v0.2.6 起补齐；此前只剥帧头不重组，>509B 的 reply 静默丢弃）。

## 5. 里程碑 1 链路（语音发飞书）

```
按下右下键/摇动开录（按下沿即录零丢失）→ 再按/再摇收尾（<300ms 录制时长静默丢弃零信令）→ 消息卡占位上屏（你：识别中…）→ 设备 Opus 编码 → BLE 分帧上行
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

## 7. 服务器主动 TTS 通知链路（ADR-066）

不经用户按键触发：任意调用方（如 Hermes 到点提醒）HTTP 调服务器，服务器合成语音主动下发。

```
POST /notify {device, text[, voice]}
→ 服务器 DashScope TTS（默认 qwen3-tts-flash，TTS_MODEL/TTS_VOICE 可配；文本 flatten_markdown 拍平）
→ wav → ffmpeg 转 ogg_opus 24kHz 单声道
→ 下行 notify{text,id} 信令（带 device 戳；同 broker 上的非目标设备忽略）
→ type=0x01 音频帧流（ogg 流按 2KB/帧切，60ms/帧 pacing，防 BLE 洪水；
  单通知音频由后台线程发送，HTTP 请求不等播放完成即返回）
→ 下行 notify_end{id} → 设备排空播完收尾（requestClose 断电）
```

设备侧行为：notify 到达即亮屏 + 消息卡"GAGA 提醒"展示文本 + 叮咚，随后播放 TTS 语音
（复用 talk 下行播放链路：OggDemux + OpusDec + 抖动缓冲）。talk 会话中或录音中到达的
通知只上屏不播语音；深睡设备收不到（预期行为）。下行音频同一时刻只允许一个通知在发
（服务器 429 拒绝并发第二个）。设备离线感知暂无（无 LWT，见 decisions.md ADR-066）。
