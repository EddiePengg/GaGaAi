# gaga ai

**Grasp up, Get AI.** 胸前佩戴的单手可操作 AI 入口设备，吉祥物是一只鸭子 🦆

核心价值：**单手一按就能输入，另一只手不用离开正在做的事**（骑行/做饭场景是立身之本）。

硬件：微雪 ESP32-S3 + 1.75" AMOLED 圆屏（466×466，32MB Flash，双麦+AEC，QMI8658 六轴，XP2101 PMU，双按键）。

## 仓库结构

```
gaga-ai/
├── AGENTS.md   # ⚠️ Agent 必读：工作规范与文档同步铁律
├── docs/       # 全部文档：架构/协议/数据建模/决策记录/硬件/路线图
├── app/        # Android 手机端：纯哑管道（前台服务保活 + BLE↔服务器转发）
├── server/     # 服务端：MQTT Broker + 信令 + ASR(Whisper) + 接入端 channels/（飞书官方 API，可扩微信/Telegram）+ Realtime 会话
├── esp32-idf/  # 设备固件：ESP-IDF 线（微雪官方 BSP，ADR-020）
├── website/    # 对外展示站：零依赖纯静态单页（预览：cd website && python3 -m http.server 8787）
├── scripts/    # 本机辅助脚本（IMU 采集、堆监控、素材转换）
└── reference/  # 芯片参考资料（微雪原理图、官方示例；大体积快照不入库）
```

## 配置与密钥

服务端密钥（飞书 / 火山引擎 / Step 等）全部走环境变量：`cp server/.env.example server/.env` 后填入自己的值。`.env` 已被 `.gitignore` 排除，任何密钥不得提交进仓库。依赖与构建产物（`.venv`、`.pio`、`managed_components`、Gradle build 等）同样不入库，重建方式见各子目录 README。

## 架构总览

```
【户外 BLE 模式】
ESP32 ←— BLE —→ App(哑管道) ←— MQTT/WebSocket —→ 服务器 ←——→ 火山引擎 / 飞书

【在家 WiFi 模式】（第二阶段）
ESP32 ←———— WiFi (MQTT常连, WebSocket仅对话期) ————→ 服务器
```

原则：
- WebSocket 只在实时对话期间存在，平时断开（省电）
- MQTT 是平时的心跳信令线（回执"叮"走这里下行），户外由 App 代挂
- 音频帧 = 二进制，信令 = JSON 文本帧
- App 只做转发，不理解任何协议；复杂度集中在服务器

## 按键分工（设备端写死，ADR-016 v2 版）

| 按键 | 动作 | 功能 |
|---|---|---|
| 右上 | 单击 | 亮屏（无操作 10 秒自动息屏） |
| 右上 | 长按 | 进入 realtime 对话模式（M5） |
| 右下 | 长按 | 按住说话：滴滴 → 说话 → 松开 → 滴滴 → 发送 |

录音中屏幕常亮显示红色图标+计时，发送后显示"发送中→✅"。

## 里程碑顺序

1. **服务端先行**：Whisper + 飞书 webhook，电脑上直接测，不等硬件
2. 设备到手：`esptool read_flash 0x0 0x2000000 factory_backup.bin` 备份原厂 32MB 固件 → 点亮屏幕 + 按键/IMU 中断 → 录音 UI 状态机
3. **App 管道打通** → "按一下说话 → 飞书群收到文字 → Hermes 响应 → 设备叮一声" 全链路闭环（项目核心价值）
4. 双击实时对话（火山 Realtime API）、AOD 表盘、装饰粒子效果

## 骑行风噪四层防线

凑近嘴边（距离平方律）> 防风棉 > ESP-SR AFE 的 NS 降噪 > 服务端 ASR 鲁棒性兜底。
