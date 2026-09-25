# AGENTS.md — Agent 必读

> 任何 Agent（或人类）在本仓库工作前，必须先读完本文件和 `docs/` 下相关文档。

## 项目一句话

gaga ai（Grasp up, Get AI，吉祥物：鸭子 🦆）：胸前佩戴的单手可操作 AI 入口设备。
ESP32-S3 圆屏设备 → BLE → 手机 App（哑管道）→ 服务器 → 飞书群里的 Hermes agent / 火山引擎。

## 铁律：改代码必须同步改文档

1. **任何改动如果影响了 `docs/` 里描述的内容，必须在同一次提交里更新对应文档**。文档和代码不一致视为工作未完成。
2. 新增协议帧、信令字段、MQTT topic → 更新 `docs/protocol.md` 和 `docs/data-model.md`。
3. 做了架构层面的取舍（换库、换协议、换链路）→ 在 `docs/decisions.md` 追加一条决策记录，写清楚"为什么"，不许只写"是什么"。
4. 里程碑进度变化 → 更新 `docs/roadmap.md` 的勾选状态。
5. 硬件相关信息（引脚、实测功耗、固件版本）→ `docs/hardware.md`。

## 仓库地图

```
gaga-ai/
├── AGENTS.md          ← 本文件，必读
├── README.md          # 项目门面（给人类看的简介）
├── docs/              # 所有文档都在这里
│   ├── architecture.md   # 系统架构与三组件职责
│   ├── protocol.md       # BLE / 帧格式 / MQTT 通信协议
│   ├── data-model.md     # 信令 schema、数据实体建模
│   ├── decisions.md      # 架构决策记录（ADR）
│   ├── hardware.md       # 硬件规格与按键分工
│   └── roadmap.md        # 里程碑与进度
├── app/               # Android 哑管道（Kotlin，已可构建）
├── server/            # 服务端（MQTT + ASR + 飞书，M1 已跑通，Python 3.12 + uv）
├── esp32-idf/         # 设备固件 ESP-IDF 线（ADR-020；PlatformIO framework=espidf + 微雪官方 BSP，经 src/idf_component.yml 拉取）
├── website/           # 对外展示站：零依赖纯静态单页（ADR-040），内容与 docs/ 同步
├── reference/         # 芯片/开发板参考资料（微雪原理图、官方示例快照）
└── .agents/skills/    # 本仓库专用 agent skill（写代码辅助知识，非运行时依赖；格式：<skill>/SKILL.md；含 Jakub Krehel interfaces 界面构建 skill 合集）
```

## 各组件的"宪法"

- **app/**：永远是哑管道，只转发不解析。禁止在 App 里写业务逻辑（对话状态机、工具调用一律放 server）。
- **server/**：系统唯一的大脑。鉴权、ASR、工具分发、日志全在这。
- **esp32-idf/**：引脚权威来源 = 微雪官方 BSP（`waveshare/esp32_s3_touch_amoled_1_75c`）+ `reference/` 里的原理图，禁止猜。

## 构建环境（本机）

- JDK：`/Applications/Android Studio.app/Contents/jbr/Contents/Home`（JDK 25，需配 Gradle 9.x）
- Android SDK：`~/Library/Android/sdk`（platforms: android-35）
- App 构建：`cd app && JAVA_HOME=... ./gradlew assembleDebug`（详见 `app/README.md`）

## 约定

- 文档和代码注释用中文，命名用英文。
- 不引入测试脚手架，除非用户明确要求。
- 音频帧走二进制，信令走 JSON 文本——这个二分不许破坏。
