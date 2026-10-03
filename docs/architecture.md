# 系统架构

## 三组件职责

```
┌─────────────┐   BLE    ┌──────────────┐  MQTT   ┌─────────────────┐
│  esp32 设备  │ ←──────→ │  Android App  │ ←─────→ │     server      │
│  (感知+交互)  │          │  (纯哑管道)    │         │  (唯一的大脑)    │
└─────────────┘          └──────────────┘         └────────┬────────┘
                                                           │ channel（ADR-030）
                                                    ┌──────▼──────┐
                                                    │ 飞书群/Hermes │
                                                    └─────────────┘
```

| 组件 | 职责 | 禁止事项 |
|---|---|---|
| esp32 | 采集/播放音频、按键与 IMU 交互、屏幕 UI、BLE 通信 | 不藏任何密钥、不直接连火山引擎 |
| app | BLE↔MQTT 字节转发、前台服务保活 | 不解析帧内容、不写业务逻辑 |
| server | MQTT Broker、ASR、接入端（channels/，ADR-030：飞书官方 API 收发，可扩微信/Telegram）、Realtime 会话、工具分发、日志 | 不做设备 UI 决策 |

## 通信分层

| 层 | 协议 | 生命周期 | 用途 |
|---|---|---|---|
| 信令层 | MQTT | 常连（户外由 App 代挂） | 回执、通知、对话协商，心跳几十字节，省电 |
| 媒体层 | WebSocket | 仅实时对话期间建立 | Opus 音频流双向传输 |

设计原则：轻信令常开 + 重媒体按需。WebSocket 常连费电，所以平时只有 MQTT 活着。

## 数据流：里程碑 1（语音发飞书）

```
按下右下键立即开录（摇一摇同权）→ 再按一下收尾（<300ms 丢弃）→ 设备 Opus 编码 → BLE 上行
→ App 原样转发 MQTT gaga/up → 服务器重组帧、解 Opus
→ ASR 转文字 → channel.send_text（飞书官方 API）→ Hermes 收到
→ 服务器 MQTT gaga/down 发 {"type":"receipt"}
→ App 写 BLE → 设备"叮" + 屏幕 ✅
```

## 复杂度分布哲学

集中式复杂度远比分布式复杂度好维护。三个组件里只允许 server 复杂；
app 和 esp32 的目标是"稳定到几乎不需要改"。以后加功能（新工具、新模型、
多设备）只动 server。

## 固件分层（esp32-idf/）

```
┌──────────────────────────────────────────────┐
│ 业务层：state/ protocol/ talk/ notify/ rec/   │
│          ble/ net/ voice/ debug/ ui/ audio/   │  ← 只认 Board / Imu / Link 抽象
├──────────────────────────────────────────────┤
│ 抽象层：boards/common/Board.h（板级能力 +      │  ← xiaozhi 式 board 层
│          InputActions 输入动作集） motion/Imu.h│    （ADR-076/078）
├──────────────────────────────────────────────┤
│ 板子层：boards/<board>/（board_config.h 引脚   │  ← 当前仅 waveshare-s3-amoled-1_75c
│          唯一权威 + Board 子类 + board_input   │    激活方式/键位分工也是板级决策：
│          物理键→动作映射 + config.json 变体）   │    换板 = 换映射，业务零改动
├──────────────────────────────────────────────┤
│ BSP：微雪官方组件（I2C 总线/显示/触摸初始化）    │
└──────────────────────────────────────────────┘
```

选板 = `platformio.ini` 的 `-DGAGA_BOARD_*` 编译宏 →
`boards/common/board_selected.h` 头文件切换；换板/加板业务层零改动。IMU 已按
同一先例抽象（`motion/Imu.h`，`net/Link.h` 模式），链路抽象见 `net/Link.h`
（ADR-039）；外设装配按 `BoardCaps`（touch/imu/pmic/rtc/display/kws）门控。

## 未来扩展预留

- **在家 WiFi 模式**：设备直连服务器（MQTT 常连 + WebSocket 按需），App 不参与。
  架构上已留口子，固件加 WiFi 连接管理即可，协议不变。
- **双击实时对话**：信令层协商（`talk_request`/`talk_ready`）→ 拉起 WebSocket →
  服务器桥接火山引擎 Realtime API。
- **多设备**：MQTT topic 已按 `gaga/up`、`gaga/down` 命名，扩展为
  `gaga/{device_id}/up|down` 即可支持多个前端（手表 App、厨房屏等）。
