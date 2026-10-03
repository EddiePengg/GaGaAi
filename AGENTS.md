# AGENTS.md — Agent 必读

> 任何 Agent（或人类）在本仓库工作前，必须先读完本文件和 `docs/` 下相关文档。

## 项目一句话

gaga ai（Grasp up, Get AI，吉祥物：鸭子 🦆）：胸前佩戴的单手可操作 AI 入口设备。
ESP32 设备（当前开发板：微雪 1.75C，见 docs/hardware.md 固件支持矩阵）→ BLE → 手机 App（哑管道）→ 服务器 → 飞书群里的 Hermes agent / 火山引擎。

## 铁律：改代码必须同步改文档

1. **任何改动如果影响了 `docs/` 里描述的内容，必须在同一次提交里更新对应文档**。文档和代码不一致视为工作未完成。
2. 新增协议帧、信令字段、MQTT topic → 更新 `docs/protocol.md`（信令 schema 与数据实体的唯一权威文档）。
3. 做了架构层面的取舍（换库、换协议、换链路）→ 在 `.agents/decisions.md` 追加一条决策记录，写清楚"为什么"，不许只写"是什么"。
4. 硬件相关信息（引脚、实测功耗、固件版本）→ `docs/hardware.md`。

## 仓库地图

```
gaga-ai/
├── AGENTS.md          ← 本文件，必读
├── README.md          # 项目门面（给人类看的简介）
├── docs/              # 人类可读文档都在这里（工作过程文档一律进 .agents/）
│   ├── architecture.md   # 系统架构与三组件职责
│   ├── protocol.md       # 通信协议唯一权威：帧格式 / 信令 schema（按方向）/ 错误码 / MQTT / 服务端实体
│   ├── hardware.md       # 硬件规格与按键分工
├── app/               # Android 哑管道（Kotlin，已可构建；独立 Gradle 工程）
├── watch/             # 手表客户端（Kotlin，已可构建；独立 Gradle 工程，ADR-051）——设备侧客户端，不是哑管道
├── server/            # 服务端（MQTT + ASR + 飞书，M1 已跑通，Python 3.12 + uv）
├── esp32-idf/         # 设备固件 ESP-IDF 线（ADR-020；PlatformIO framework=espidf，组件依赖经 main/idf_component.yml 拉取）
│                        #   固件内部分 board 硬件抽象层：main/boards/common/（Board 基类，ADR-076）
│                        #   + main/boards/<board>/（板子目录，当前仅 waveshare-s3-amoled-1_75c）
├── website/           # 对外展示站：零依赖纯静态单页（ADR-040），内容与 docs/ 同步
├── reference/         # 芯片/开发板参考资料（微雪原理图、官方示例快照）
├── .agents/skills/    # 本仓库专用 agent skill（写代码辅助知识，非运行时依赖；格式：<skill>/SKILL.md；含 Jakub Krehel interfaces 界面构建 skill 合集）
├── .agents/decisions.md  # 架构决策记录（ADR，AI 维护，只追加不改史）
├── .agents/bug-analysis-0923.md / screen-blackout-case.md  # 疑难 bug 根因分析/排查 SOP（AI 工作文档）
└── .agents/power.md   # AI 工作文档：功耗优化台账（现状/方案/结果三件套，AI 维护；人看的归档在 docs/power-log.md）
```

## 各组件的"宪法"

- **app/**：永远是哑管道，只转发不解析。禁止在 App 里写业务逻辑（对话状态机、工具调用一律放 server）。
- **watch/**：手表客户端，**设备侧客户端而非哑管道**（ADR-051）——它自己组帧、走信令状态机、编码音频，与 esp32-idf/ 是同一层的东西（都是"嘎嘎设备"）。但信令 schema 与帧格式仍以 `docs/protocol.md` 为准，业务大脑仍在 server。**不许反向依赖 app/**，两边是平级独立工程。
- **server/**：系统唯一的大脑。鉴权、ASR、工具分发、日志全在这。
- **esp32-idf/**：引脚权威来源 = `main/boards/<board>/board_config.h`（GAGA_* 宏 re-export BSP + 原理图，禁止猜），板级能力经 `main/boards/common/Board.h` 抽象（ADR-076）。

## 构建环境（本机）

- JDK：`/Applications/Android Studio.app/Contents/jbr/Contents/Home`（JDK 25，需配 Gradle 9.x）
- Android SDK：`~/Library/Android/sdk`（platforms: android-35）
- App 构建：`cd app && JAVA_HOME=... ./gradlew assembleDebug`（详见 `app/README.md`）
- 手表构建：`cd watch && JAVA_HOME=... ./gradlew assembleDebug`（详见 `watch/README.md`，产物 `watch/build/outputs/apk/debug/gaga-watch-debug.apk`）
- 固件构建：`cd esp32-idf && python3 scripts/build.py list / build <板名> / --all / matrix`（多板入口，板级参数权威在 `main/boards/<board>/config.json`）；`pio run` 是本地快捷方式（env 是 config.json 的手维护镜像，详见 `esp32-idf/README.md`，ADR-077）。

## 约定

- 文档和代码注释用中文，命名用英文。
- 功耗优化工作状态记 `.agents/power.md`（给 AI 的台账：现状/方案/结果随做随更）；正式实测归档仍是 `docs/power-log.md`，取舍记 `.agents/decisions.md`。
- 不引入测试脚手架，除非用户明确要求。
- 音频帧走二进制，信令走 JSON 文本——这个二分不许破坏。
