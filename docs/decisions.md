# 架构决策记录（ADR）

每条决策记录"选了什么 + 为什么 + 放弃了什么"。新的追加在末尾，不许改历史条目。

## ADR-001：设备不直连火山引擎，必须有服务端中转
- **选择**：设备 → App/服务器 → 火山引擎，API Key 只放服务器
- **理由**：① 固件可被 dump，Key 烧进设备必泄漏；② TLS+WebSocket 长连接是 ESP32 上最脆弱的场景；③ Realtime 协议状态机（VAD、打断、事件）在串口监视器里无法调试，放服务端有完整日志
- **放弃**：设备直连（省一跳延迟约 30~80ms，但密钥安全和可调试性代价太大）

## ADR-002：手机 App 是哑管道，不做业务逻辑
- **选择**：App 只做 BLE↔服务器字节转发
- **理由**：手机端开发调试成本远高于服务器；App 做的事越少，被 ColorOS 杀后台后的行为越可预期；加新功能不用重新发版 App
- **放弃**：App 直连火山引擎（无服务端方案）——会让 App 从 200 行管道膨胀成真应用，复杂度只是搬了家

## ADR-003：轻信令（MQTT 常连）+ 重媒体（WebSocket 按需）
- **选择**：平时只挂 MQTT；WebSocket 仅实时对话期间建立
- **理由**：WebSocket 常连耗电；MQTT 心跳几十字节、支持离线消息暂存，是物联网标准做法
- **放弃**：WebSocket 常连承载一切（耗电）；纯 MQTT 传音频（QoS 语义不适合流式媒体）

## ADR-004：户外 BLE 模式下 MQTT 由手机 App 代挂
- **选择**：BLE 模式时设备只维持 BLE 连接，互联网侧常连成本由手机承担
- **理由**：手机电量和后台能力比 ESP32 强一个数量级；设备侧功耗压到 ~1mA 级
- **放弃**：设备外挂 4G 模块（体积/功耗/资费违背"轻便常戴"初衷）；手机热点（iPhone 热点自动关闭，且耗电 15~20%）

## ADR-005：S3 无经典蓝牙，放弃"伪装蓝牙耳机"路线
- **选择**：自研 App 做 BLE GATT 桥接
- **理由**：ESP32-S3 只有 BLE 没有 BR/EDR，HFP/A2DP 物理上说不了
- **备注**：Opus 16kbps 语音流远小于 BLE 5.0 实际吞吐（100kbps+），带宽不是问题，抖动才是

## ADR-006：里程碑 1 用"录一小段再传"，不做流式
- **选择**：按住录音 → 松开 → 整段上传 → 异步 ASR
- **理由**：语音发飞书是异步场景，不需要实时性；比流式简单一个量级，BLE 慢传几秒无所谓，功耗可忽略
- **放弃**：流式 ASR（留给里程碑 4 的实时对话）

## ADR-007：固件用 Arduino 框架（PlatformIO 管理），不用 ESP-IDF
- **选择**：Arduino + LVGL 8.x，PlatformIO 组织项目
- **理由**：微雪官方示例就是 Arduino 版；LVGL 在 Arduino 生态教程多；PlatformIO 让项目成为文件系统里的真实结构，方便 Agent 管理和版本化
- **放弃**：ESP-IDF（为量产准备的复杂度，DIY 阶段纯负担）；Arduino IDE 的 .ino（配置藏在 IDE 里不利于 Agent）
- **备注**：底层都是 FreeRTOS，LVGL 界面代码两边通用，将来要迁移成本不高

## ADR-008：App 用 HiveMQ MQTT Client，不用 Paho
- **选择**：HiveMQ MQTT Client 1.4.0（纯 Java）
- **理由**：不依赖 Android Service 组件，自带指数退避自动重连，对现代 Android 省心
- **放弃**：Eclipse Paho Android（API 老旧，依赖 Service 组件）

## ADR-009：App 工具链用 Gradle 9.7.1 + AGP 9.4.1
- **选择**：Gradle 9.x
- **理由**：本机 JDK 是 Android Studio JBR 的 JDK 25，Gradle 8.x 最高只支持 Java 24
- **放弃**：降级 JDK（引入额外环境管理成本）

## ADR-010：~~~~按键分工写死~~（已被 ADR-016 取代，保留作历史记录）~~（Arduino 线专用决策，线已退役删除 ADR-041，条目移除）

## ADR-016：按键交互 v2——自动息屏 + 按住说话 + 长按进 realtime
- **选择**：
  - 右上单击 = 亮屏；无操作 10s 自动息屏（取消手动关屏）
  - 右下长按 = 按住说话：长按触发 → 滴滴 → 说话 → 松开 → 滴滴 → 发送（push-to-talk）
  - 右上长按 = 进入 realtime 对话模式（M5）
  - 右下单击/双击 = 不分配（防止与按住说话手势冲突）
- **理由**：① 用户实测习惯：屏幕只需要"看一眼"，自动息屏比手动关屏更符合挂件形态（手环同逻辑）；② 按住说话在骑行戴手套场景比"按一下开始再按一下结束"可靠——按住期间手和设备自然形成挡风罩，且起止边界绝对明确；③ realtime 从双击改为长按，与录音手势（也在右下）分开到不同按键，避免双击/长按在同一键上的判定冲突
- **放弃**：ADR-010 的"单击录音 + 双击对话"（同键多手势在手套场景误触率高）
- **2026-09-23 修正（IDF 线）**：右下录音从"长按 400ms 触发"改为**按下沿即录**（press-down 进录音态+开 mic+红点 UI 即时反馈），300ms 确认窗过后才发 recStart/滴滴，窗内松手=误触静默撤销（零信令）；右下/右上单击均唤醒亮屏；依据：用户实测 800/400ms 长按等待都嫌慢，对讲机式"按下即录"才是正确手感
- **备注（2026-09-23，用户 UX 改进）**：右下键单击也唤醒亮屏（"任意键单击先亮屏"；右下单击此前未分配，无冲突——ButtonHandler 里长按后的松开不算单击，与按住说话不打架；唤醒判定有 300ms 双击窗口延迟，亮屏场景可接受）。IDF 线已落地（esp32-idf/src/main.cpp wireButtons）。另：PWR 长按阈值 800ms 远低于 AXP2101 的 6s 硬件强制关机线，安全。
- **2026-09-23 修正（松手宽限 / 锁定模式）**：用户报障"按住时稍微松力就断（手没打算松）"——本板按键是两段式轻触开关，**释放阈值离按住力太近**，握力波动瞬间越过断开点，而固件语义是"电路一断=松手=立刻结束"。改为 PTT 松手宽限（hang time）：已 commit 的录音在电路断开后 **500ms** 内不结束，窗内重新咬合=无缝续录（mic 常开、泵不断流，音频无接缝），窗尽才 rec_stop；确认窗内（<300ms）误触语义不变（立即静默撤销）。串口 `w` 循环 0/250/500/1000/2000ms 调参；`l` 切**锁定模式**（松手完全不算数、再按一下才结束，100% 免疫握力）。代价：真松手的结束反馈晚 500ms，且尾部多录 ~500ms 环境音（ASR 无感）。不选"单击开始/单击结束"：保留 ADR-016 理由②的 PTT 语义。

## ADR-011：~~BLE 栈用 NimBLE（NimBLE-Arduino 1.4.x），不用 Bluedroid~~（Arduino 线专用决策，线已退役删除 ADR-041，条目移除）

## ADR-012：服务端用 Python 3.12 + uv + FastAPI，不用系统 Python/Node/Go
- **选择**：Python 3.12（uv 管理 venv 与锁版本），FastAPI + uvicorn 做 HTTP 调试口
- **理由**：① 系统 Python 是 3.9，太老且不可动，uv 一条命令装 3.12 并锁依赖；② ASR 生态（faster-whisper/火山 SDK）Python 最全；③ FastAPI 的 `/debug/audio` 是 curl 测试入口，且为 M5 的 WebSocket 媒体层留同进程基础，不用换框架
- **放弃**：Node/Go（ASR 库生态薄，还要自己绑定 whisper）；系统 Python 3.9（faster-whisper 要求 ≥3.10）

## ADR-013：ASR 默认本地 faster-whisper（small，CPU），火山引擎留 provider 接口本期不接
- **选择**：`ASR_PROVIDER` 配置切换，默认 `local_whisper`；`volcengine` 只有接口与配置项占位（`VOLC_APP_ID` 等），调用即报 `ASR_FAIL`
- **理由**：① 零 API Key、离线可跑、Mac CPU 识别中文够用；② 火山一句话识别没有 Key 无法联调，但接口边界（PCM 16k 单声道 → text）现在就能定死，将来接入不改 pipeline
- **放弃**：一上来接云端 ASR（阻塞在 Key）；写死单一实现（将来火山/其他云没法插）
- **备注**：实测 small 对中文 TTS 音频可用但会听错专有名词（"gaga"→"高高"）；识别质量不够时先升模型档位（medium），再考虑火山

## ADR-014：MQTT Broker 本地开发用 brew mosquitto，生产用 docker-compose 里的 eclipse-mosquitto
- **选择**：本机无 Docker，开发期 `/opt/homebrew/sbin/mosquitto -c server/mosquitto/mosquitto.conf` 直起；生产（dokploy）用 `server/docker-compose.yml` 里的 eclipse-mosquitto 服务，同一份 `mosquitto.conf`
- **理由**：一份配置两处复用，开发/生产行为一致；compose 随仓库交付，dokploy 直接起
- **放弃**：本机强制 Docker 化（环境没有，装 Docker Desktop 是额外负担）；生产用托管 MQTT（多一个外部依赖和计费）
- **备注**：本期无鉴权仅限本机+局域网；公网暴露前必须开 username/password 或 TLS，见 `server/README.md` 警告

## ADR-015：Opus 解码走 ffmpeg 子进程 + 服务端自写 Ogg 封装，不引 opuslib
- **选择**：设备上行裸 Opus 包（data-model.md §2）→ 服务端 `ogg.py` 封装成 Ogg Opus 临时文件 → ffmpeg 子进程解成 16k PCM
- **理由**：① ffmpeg 只认 Ogg 容器不认裸包，封装层（~100 行，含 Ogg CRC）是必要的适配；② 比 opuslib 省一个 native 依赖（opuslib 需要编译 libopus，ARM Mac 上多一个坑）；③ 整段录音一次解码，子进程开销（几十 ms）相对 ASR 秒级可忽略；④ ffmpeg 同时服务 HTTP 调试口（m4a/wav 任意格式进 PCM），一个工具两条链
- **放弃**：opuslib 逐包解码（native 依赖）；要求设备直接发 Ogg 容器（把封装复杂度推给 ESP32，不值）
- **备注**：`ogg.py` 的 parse 方向（Ogg→裸包）给 `scripts/simulate_device.py` 用，字节级模拟真实设备上行

## ADR-017：~~固件板级隔离——variants/<板型>/ + src/hal/ 抽象接口 + [env:null] 退路（参考 Meshtastic）~~（Arduino 线专用决策，线已退役删除 ADR-041，条目移除）

## ADR-018：提示音用 I2S 合成 1kHz 正弦 + 异步播放任务；ES8311 驱动原样搬运官方 es8311.c；I2S 引脚以原理图为准
- **选择**：
  - 提示音（"滴滴"）不放 wav 资源文件，固件内实时合成 1kHz 正弦（16kHz 采样整 16 点/周期），100ms 一声、间隔 80ms，立体声左右同相；振幅 0.5 满幅 + codec 音量 90
  - 播放入独立 FreeRTOS 任务（core 0，优先级 2）+ 队列：`AudioDriver::beep(n)` 只投递不返回等待，主循环零阻塞（两声 ~360ms 的 DMA 喂数据全在任务里）
  - ES8311 寄存器驱动直接原样搬运官方 Demo 08 的 `es8311.c/h/es8311_reg.h`（乐鑫官方驱动，I2C 走 Arduino 内部 `i2cWrite` 即 Wire 端口 0），不自写初始化序列
  - I2S 外设用 ESP-IDF legacy `driver/i2s.h`（I2S0 master TX，MCLK=256×fs=4.096MHz），不用 Demo 里的 `ESP_I2S.h`——那是 Arduino core 3.x 才有的 API，本工程锁 core 2.0.x（ADR-011 同源约束）
  - **I2S 引脚修正**：DOUT=8（ESP32→ES8311 DSDIN）、DIN=10（ES8311 ASDOUT/ES7210 SDOUT→ESP32）、MCLK=42（唯一一路，两芯片共用）、BCLK=9、WS=45、PA=46- **理由**：① 合成音省去文件系统/资源管理依赖，且时长间隔参数化（将来 receipt"叮"一声直接复用）；② 阻塞式播放会让按住说话松开后的 BLE 信令发送延迟 ~360ms，异步任务消除这个耦合；③ 官方 pin_config.h 的 `// ES8311` 块（I2S_MCK_IO=16）与板子原理图矛盾——原理图网表显示 GPIO16 无音频连接，MCLK 只有 GPIO42 一路（与官方 ESP-IDF BSP `BSP_I2S_MCLK=GPIO_NUM_42`、Demo 08 实际 `setPins(9,45,8,10,42)` 三方一致），引脚权威层级：原理图 > BSP/Demo 代码 > pin_config.h 宏注释
- **放弃**：wav 资源播放（多一个 LittleFS 依赖，提示音场景不值）；`ESP_I2S.h` I2SClass（core 3.x 才有，换轨成本见 ADR-011）；mclk_from_mclk_pin=false 的 BCLK 衍生时钟（官方明说并非所有采样率支持）
- **备注**：① `tx_desc_auto_clear=true` 保证提示音播完 DMA 欠载自动补零静默；② 录音采集（PIN_I2S_DIN=10，ES7210）留待 Opus 录音里程碑，I2S RX 通道届时挂同一 I2S0；③ 显示侧同期决策：CO5300 初始化照抄官方 Demo（Arduino_ESP32QSPI + 列偏移 6），息屏 `displayOff()`=DISPOFF+SLPIN 双保险（AMOLED 黑像素零功耗之上再睡面板），且息屏期间 flush 直接短路不再推图；QSPI 刷新窗口偶数对齐的 rounder_cb 由 display_config.h 的 `DISPLAY_NEEDS_EVEN_ROUNDER` 开关，不污染共享 LvglPort；④ setup() 末尾 `beep(1)` 开机冒烟一声——刷机/复位即验证音频链路（无按键/拍照手段时的自测信号），串口同步打 `[audio] beep x1 播放完成`；⑤ GFX 库锁 `~1.4.9`：1.5+ 的 Arduino_ESP32SPI.h 引入 core 3.x 专属 `esp32-hal-periman.h`，与本工程 core 2.0.x（ADR-011）不兼容，1.4.9 已含完整 CO5300/QSPI 支持（含 displayOn/displayOff/setBrightness）；⑥ **GFX 构造函数签名随版本漂移的大坑（2026-09-22 真机踩实）**：1.4.9 的 `Arduino_CO5300(bus,rst,rotation,ips,w,h,...)` 比 1.6.x 多一个 `ips` 参数，照抄 1.6.x Demo 实参不报错（int→bool 隐式转换），症状 = ips 被塞 466→true（反色开）、h 被挤成 6（Arduino_TFT 裁剪丢弃 y>5 的所有 LVGL 分块）、列偏移丢失——屏幕呈现"顶部窄条反色 + 其余 GRAM 垃圾色（粉）"。**"偏粉/花屏"先查构造参数与裁剪，别动字节序开关**；display.cpp begin() 已加 `gfx->width()/height()` 实测打印兜底，flush 前 14 块打坐标日志
- **备注2（2026-09-23，IDF 线决定性实验更正 MCLK 归属）**：本条"I2S 引脚修正"里的 **MCLK=42 只适用于 1.75**，本机实为 1.75C——1.75C 原理图 I2S0MCLK 挂 GPIO16。真机实验：MCLK=42 时 ES7210 采集全零（DIN10 全程无翻转）；MCLK=GPIO16 立刻出数，录音→BLE→MQTT→ASR→飞书全链路真实打通（10s/32s 录音零丢弃零崩溃，服务端完整解码出中文语音）。**结论：本机音频 MCLK=GPIO16；BCLK=9/WS=45/DOUT=8/DIN=10/PA=46 不变**（详见 docs/bug-analysis-0923.md Bug1 修复结果）。IDF 线已落地（esp32-idf AudioPipe 自建 I2S 通道）；Arduino 线若回放提示音需同步改 16 回归。另：当年"I2S0 双工 RX 全零"的悬案，真凶不是双工也不是驱动，是 MCLK 一直没到 ES7210 腿上。

## ADR-019：App 允许为日志显示读取帧头/解析 JSON（ADR-002 哑管道原则的显示层破例），但禁止影响转发行为
- **选择**：Android App 的日志显示层（`app/util/FrameLogger.kt`）可以读帧头 type、解析 JSON payload，把音频帧聚合成"🎙 音频上行中… 已发 N 包 / 完成，共 N 包"（500ms 节流 + 500ms 静默定稿），把信令显示为"开始录音 / 结束录音（时长）/ 设备上线 / ✅ 叮 / 💬 回复 / ❌ 错误"等人话；转发路径（BLE ↔ MQTT 字节原样透传）一行不动，FrameLogger 的输出只进 BridgeState 日志缓冲
- **理由**：① 实测按住说话时 11B/12B 音频帧把日志区刷爆，下行 receipt/reply 只显示字节数完全无法排查链路；② 帧格式（ADR-002/protocol.md §2）已经足够稳定，读 type 字节和 JSON `type` 字段属于低成本观察，不构成解析职责；③ 显示层破例是调试可观测性需求，不是业务逻辑——解析失败一律回退字节数显示，永不阻断/修改转发字节
- **放弃**：App 侧做音频帧重组再显示（哑管道不做协议重组，分片 JSON 标"…（分片）"即可）；把聚合逻辑塞进 BleManager/MqttManager 转发路径（污染哑管道核心，违反 ADR-002）
- **备注**：铁律——FrameLogger 只能"读"，任何字段缺失/解析失败/未知 type 都必须回退到字节数显示且不影响转发；将来帧格式变更时日志显示允许滞后失真，但转发行为永远正确

## ADR-020：新开 ESP-IDF 平行固件线（esp32-idf/），Arduino 线保留保底
- **选择**：在 monorepo 新建 `esp32-idf/`（PlatformIO `framework = espidf` + 微雪官方 BSP `waveshare/esp32_s3_touch_amoled_1_75`），逐步追到与 Arduino 线功能平价；Arduino 线（`esp32/`）冻结为可用保底版，IDF 平价后再评估切换
- **理由**：① 用户决定（2026-09-22）；② ES7210 手工移植实战证明 Arduino 生态在音频采集上的贫瘠，而 IDF 有官方 BSP + esp_codec_dev（ES7210/ES8311 现成驱动）+ ESP-SR（AFE 降噪，骑行风噪刚需）；③ 小智 AI（xiaozhi-esp32）是同板型 IDF 开源固件，可作 M5 realtime 对话的灯塔；④ PlatformIO 原生支持 IDF + IDF 组件注册中心，工具链不用换
- **放弃**：在 Arduino 线上继续手工移植 ES7210（音频任务 2026-09-22 中止）；立刻推倒重来（Arduino 版当天已验证可用，炸掉它是净损失）
- **备注**：可平移的资产——帧协议（protocol.md 不变）、状态机/按键逻辑（纯 C++ 设计可抄）、LVGL 界面结构、App/服务端零改动（协议兼容）；IDF 线第一期目标：启动 + BSP 点亮屏幕 + NimBLE 广播 GAGA-XXXX
- **备注2（2026-09-23 第一期落地，BSP 接入方式的关键取舍）**：
  - BSP 锁 `waveshare/esp32_s3_touch_amoled_1_75` ^3.0.1（dependencies.lock 实锁 3.0.1）：3.x 代的显示栈是 **esp_lvgl_adapter**（不是老 BSP 的 esp_lvgl_port），`bsp_display_start()` 一站式拉起 CO5300 + CST9217 + LVGL 任务，应用侧只用 `bsp_display_lock/unlock`；音频走 `bsp_audio_init()` + esp_codec_dev 的 `bsp_audio_codec_speaker/microphone_init()`（ES8311/ES7210 现成驱动，正是 ADR-020 想要的）
  - PlatformIO 路线走通但踩了两个上游坑，对策全部收进工程内（不换工具链）：① pio 6.x 的 IDF 构建器 `get_app_flags` 对编译选项 `sorted()`，拆散 esp_lvgl_adapter PUBLIC 传播的 `-include lvgl_port_alignment.h` 二元组导致 g++ 报 multiple files——根 `CMakeLists.txt` 从 INTERFACE_COMPILE_OPTIONS 剥离该选项（语义无损，注释完整）；② BSP `CONFIG_BSP_ERROR_CHECK=n` 是编译不过的上游 bug（返回指针的函数里 return esp_err_t），保持默认 y
  - `board_build.partitions` 必须显式指向 `partitions.csv`：否则 pio 静默用默认 singleapp 表生成 partitions.bin 并按 1MB 检查体积，与 sdkconfig 的自定义分区打架（两侧现指向同一文件）
  - 控制台用 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`（原生 USB 单口刷机+日志）：主机不开串口时写入被丢弃，抓日志的标准姿势是"端口常开 + RTS 硬复位"（README 有脚本）；固件 `app_main()` 开头等 1.5s 保版本行可抓
  - 广播名尾号 0A4D→0A4E：IDF 5.x 的 ESP_MAC_BT 派生为 base+2（IDF 4.4 是 base+1），名字与控制器实际 BLE MAC 一致，App 过滤不受影响

## ADR-021：实时对话桥（realtime/）——音频不开 WebSocket 走 MQTT 帧透传 + 服务端 20ms 节奏器
- **选择**：
  - 新增 `server/src/gaga_server/realtime/`：`client.py`（豆包 Seeduplex 全双工协议封装：session.create/append/mute/cancel/close）+ `bridge.py`（TalkBridge 会话生命周期）
  - **音频媒体不开 WebSocket**：设备↔服务器复用既有 MQTT 常连通道，上行 Opus 帧（type=0x01）直通火山、下行 ogg_opus 分片帧化回 gaga/down——App 零改动，哑管道不破（ADR-002）
  - **服务端 20ms 节奏器**（bridge 的 `_pacer`）：设备帧入队、每 20ms 出一包发火山，吸收 BLE+MQTT jitter；积压超 500ms 丢最旧包追平；断流 >1s 自动 `input_audio_mute.commit` 保活、来帧自动 unmute（火山文档硬性要求：关麦不发帧会被判超时无响应）
  - 线程模型：TalkBridge 自带 asyncio 循环线程（uvicorn 占主线程、paho 占自己线程，互不阻塞）；paho 线程经 `call_soon_threadsafe` 进入
  - 结束条件四路：退出意图（`output_audio.done` status_code=20000002）、设备 `talk_end`、空闲/最长超时、error；一律先 `session.close` 优雅关闭（直接断连触发火山 55000001 ContextCanceled）
- **理由**：① BLE 上行 Opus 16kbps 远低于 MQTT 承载力，WS 只省包头却要另维护一条连接生命周期；② 节奏器是硬需求——火山对过快/过慢都报错，而 BLE+MQTT 必然有抖动，必须在服务端平滑；③ 独立 loop 线程让 whisper（CPU 密集）与 realtime 会话互不拖累
- **放弃**：WebSocket 媒体通道（roadmap 原计划项；设备侧 M5 联调后若时延不达标再评估）；Function Calling/Hermes 工具本期不接（`response.function_call_arguments.done` 仅记日志，接入点注释留在 client.py/bridge.py）
- **备注（2026-09-23 五轮真实 API 实测事实）**：
  - 鉴权头 `X-Api-Key` + `X-Api-Connect-Id`（UUID）即可，不需要老协议的 X-Api-Resource-Id
  - ASR 流式 delta 是**累积快照且有交叠**（快慢双跑次），直接拼接会重复；终稿取 `conversation.item.input_audio_transcription.completed` 的 **`text`** 字段（干净全文）；`response.output_text.delta` 是真增量
  - 下行 `response.output_audio.delta`（base64）按到达顺序拼接即为完整 Ogg 流，ffmpeg 可解码
  - **退出意图未触发**：`enable_user_query_exit=true`（extension.dialog.extra 对象形式，与老 StartSessionPayload 一致）五轮里模型都回了告别语但 `output_audio.done` 从无 status_code 字段——疑似 `say` 合成音韵律不足以触发，处理代码已就位，留真机人声验证
  - 本机 SOCKS 代理环境必须 `proxy=None` 直连（websockets 走 SOCKS 需要 python-socks，而火山是国内服务直连即可）
  - usage 统计在 `response.done` 的 response.usage（input/output audio/text tokens 分列）

## ADR-022：M1 录音链路 ASR 升级为火山流式直通（volc_stream），本地 whisper 降为兜底
- **选择**：
  - 新增 `asr/volc_stream.py`（+ `asr/sauc.py` 二进制协议编解码）：rec_start 即开 sauc 双向流式**优化版**会话（`wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async`），Opus 帧边到边转发——200ms 聚组 + `OggStreamWriter` 增量封装成连续 Ogg 流直发（`format=ogg codec=opus`，实测服务器接受）
  - rec_stop 时机：帧数覆盖 duration_ms（-100ms 容差）立即收尾，否则静默窗兜底；收尾动作 = 发**空负载负包**（FLAG_LAST_NO_SEQ + 空 payload）
  - 失败自动降级：建连 403/中途协议错误/终稿超时 → 攒下的帧走本地 whisper 批处理老路（`_buf` 全程保留），日志注明降级
  - `ASR_PROVIDER=volc_stream` 开启；HTTP 调试口永远走本地 whisper（它拿的是整段文件）
- **理由**：① 延迟：本地 whisper small 批处理 rec_stop→出字 ~3.7s（2s 静默窗 + ~1.7s CPU 推理），流式直通实测 rec_stop→终稿 **1.57s**（模拟突发）/ 真机人声 2.15s（BLE 丢包走满窗口），而火山自身"终包→definite"只需 0.08~1.4s——音频在录音期间早已传完识别完；② 准确率：真机人声 "能听到我说话吧？鸭子。" 一字不差，whisper small 对 TTS/真人都有可见错词；③ ogg 直通省掉常驻 ffmpeg 解码进程（ADR-015 的封装器增量化复用）
- **放弃**：攒段批处理做主路（慢一个数量级）；PCM 直发（要常驻 ffmpeg 管道，ogg 实测可行就不需要）；二遍识别 enable_nonstream（ASR 2.0 未开通，且1.0 准确率实测已够用）
- **备注（2026-09-23 实测事实）**：
  - 鉴权头：`X-Api-Key` + `X-Api-Resource-Id` + `X-Api-Request-Id` + `X-Api-Sequence: -1`（+ Connect-Id UUID）
  - **资源授权**：本账号 `volc.seedasr.sauc.*`（ASR 2.0）403 `requested resource not granted`（需控制台开通"豆包流式语音识别模型2.0"）；`volc.bigasr.sauc.duration`（1.0 小时版）与 `.concurrent` 已开通可用，默认用 duration 版
  - **空负包是低延迟关键**：音频组从不带 last 标志，排空后补空负载负包，服务端 ~0.1s 内吐出全部积压结果 + definite；不带负包则 VAD 判停要等 1.5s+，完全 idle ~10s 触发 45000081
  - 终稿信号：优化版终帧 seq 仍为正 → 以 `utterances[].definite=true` 为主、负 sequence 兜底
  - 发包节奏：文档建议 100~200ms/包间隔；我们 200ms 音频一组、50ms 间隔（4x 速）实测无节奏错误
  - 降级实测：resource 403 → 自动走本地 whisper，飞书/receipt 正常（日志见 server/README.md）
  - **修正（2026-09-23 用户抓虫）**：原实现在新 rec_start 时 cancel 上一段未完成的流式会话并清空缓冲——上一段录音被静默丢弃。正确语义是异步队列：每段录音是独立任务，rec_start 时旧段**立即收尾完成投递**（流式发结束负包等终稿、批处理立即触发 pipeline），不作废。配套修正：流式回调带 session 参数区分多段并发收尾的归属（终稿延迟计时、definite 短路、降级帧缓冲都挂在会话对象上，旧会话迟到的 definite/error 不会错串新录音）。验证：`scripts/simulate_two_recs.py`（段1 虚报时长停在静默窗 + 0.5s 后段2 rec_start）两段各自投递飞书 + 各收 receipt

> **ADR-019 备注（2026-09-23，M5 实时对话信令）**：FrameLogger 新增 talk_ready/talk_asr/talk_reply/talk_end 显示映射；talk_asr/talk_reply 的 interim 文本复用 BridgeState 进度行原地刷新（key=talk_asr/talk_reply），final=true 定稿；新增 ping 心跳单行原地刷新（💓，避免心跳刷屏字节数日志）。仍只读不转发。


## ADR-023：音频双工常开（spk+mic 一次 open 用到底），废除"每声提示音现场开关 codec"

- **日期**：2026-09-23
- **背景**：真机报障"长按右下角没有滴滴声；说过话之后就有了"。串口实锤根因两条：
  ①**冷路径播放不出数**：spk 单独首次 open 后播放，GPIO 探针显示 BCLK9/WS45 有翻转但
  **DOUT8 全程零翻转**（时钟在跑、数据全零）→ 物理无声；录过一次音（mic+spk 同开的
  配对重配舞蹈）后 TX 数据通路才活——与"说过话之后就有了"完全吻合。
  ②**提示音被开麦掐死**：beep 的 spkOpen 与录音确认窗的 micOpen 抢 I2S——本板 I2C 极慢
  （单次 codec open 0.5~2s，boot 日志已有 pull-up 告警），按下后"滴"还在开喇叭，300ms
  确认窗一到 micOpen 插进来做配对重配，把在播的"滴"掐成碎片；松手的"滴滴"在 mic 关闭后
  才播所以响（日志里 "beep x1 播放完成" 与 mic open 完全交叠）。
- **官方对照**（`reference/waveshare-1.75C-repo` + 微雪 BSP）：init 时 play+record
  两路一起 open 且**用完不关**，beep/TTS 只做 `esp_codec_dev_write` 纯写；换速率才
  "关两路→开两路"原子切换（`bsp_extra_codec_set_fs`）。自研旧版每声 beep
  `spkOpen→write→spkClose`，close 触发 es8311 全下电+复位舞蹈，等于每声都是冷启动。
- **附带澄清（假线索）**：排查中 `gpio_get_level(46)` 恒 0 一度被判成"功放使能没驱动"——
  实为测法错误：引脚配成 `GPIO_MODE_OUTPUT` 后输入通路切断，读回恒 0。es8311 驱动
  （`es8311_pa_power`）在 open 时一直有拉高 PA（说完话能响即证明）。改为
  `GPIO_MODE_INPUT_OUTPUT` 后读回才是真值。
- **决定**：照官方改成**双工常开**（`AudioPipe::duplexOpen`）：`begin()` 双路 open @16k
  常开；beep=纯写数据（不再现场开关）；talk activate/teardown 用 `duplexOpen` 在
  24k↔16k 整体换档（换档前等 beepBusy 落地）；PA=GPIO46 在 open 前拉高并常保。
- **代价**：ES7210+ES8311 待机常上电（估算 +几 mA，进 hardware.md 功耗预算待实测）；
  深度休眠时再统一断电。
- **不选的方案**：①只推迟 micOpen/加锁——冷路径 DOUT8=0 依然无解；②确认窗等 beep 播完
  再开 mic——I2C 慢导致时序窗口不可控（实测 500ms 等待上限被击穿）。

## ADR-024：App 保活组合拳——六道自愈通道 + 电池白名单 + 通知状态灯

- **日期**：2026-09-23
- **背景**：用户报障三类断联：①锁屏/久置后服务"已停止"；②被系统杀死；③手滑关掉。
  ColorOS 后台管控激进，单靠前台服务 + START_STICKY（原实现）不够。
  用户反馈系统里**找不到"深度睡眠应用"入口**（部分 ColorOS 版本无该项），设置层
  已到顶，剩余差距只能代码层补。
- **选择**：六道自愈通道 + 一道豁免 + 一道可视化（`app/`，v0.1.6）：
  ① `BootReceiver`：开机自启（BOOT_COMPLETED ∥ QUICKBOOT）；
  ② `KeepAlive` 看门狗闹钟：`AlarmManager.setAndAllowWhileIdle` 15 分钟自续期，
  服务没活就拉起（不走 setExact——免 SCHEDULE_EXACT_ALARM 权限，对看门狗够用）；
  ③ `onTaskRemoved` 自愈：滑卡片/一键清理后立即拉起 + 1.5s 闹钟兜底（闹钟广播带
  系统临时白名单，是少数允许后台起前台服务的窗口）；
  ④ 电池白名单一键跳转（`ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS`）；
  ⑤ 打开 App 自动拉起服务（也是"强行停止"后唯一的复活路径）；
  ⑥ 伴生设备出现自拉（ADR-025）。
  可视化：常驻通知改 IMPORTANCE_DEFAULT 静音 + 文本跟随 BLE/MQTT 状态 = **链路状态灯**
  （LOW 重要性在 ColorOS 被折叠成"静默通知"，等于不可见）。
- **纪律**：用户明确点"停止服务"（`Prefs.userStopped`）→ 所有自愈通道让位，
  直到重新打开 App。**明确意图 > 保活**，不做流氓复活。
- **不选**：双进程互保/账号同步拉活/推送唤醒——违背 ADR-002 哑管道原则，Android 10+
  基本无效且容易被商店拉黑；WorkManager 周期任务——AlarmManager 广播已是
  后台起服务的合法窗口，无需再引依赖。
- **代价**：常驻通知可见性提高（用户可手动隐藏）；闹钟每 15 分钟一次（Doze 下顺延）。

## ADR-025：CompanionDeviceManager 伴生设备——官方保活特权 + BLE MAC 直连

- **日期**：2026-09-23
- **背景**：胸挂 BLE 设备 + 手机转发 App 正是 Android"伴生设备"（手表/耳机类）的
  教科书场景。CDM（API 26+）是系统给这类 App 的官方特权通道。
- **选择**：
  - **配对**：`MainActivity`「配对嘎嘎」→ `CompanionDeviceManager.associate()` 系统
    设备选择框 → 选定即建立永久关联，MAC 存 `Prefs`；
  - **伴生服务**：`GagaCompanionService`（Manifest 声明
    `BIND_COMPANION_DEVICE_SERVICE` + action `android.companion.CompanionDeviceService`），
    系统在嘎嘎出现/离开时绑定；出现 → 自动拉起转发服务（保活第六道），离开 → 不动作
    （户外 BLE 断连属常态，交给重连循环）；
  - **特权收益**：绑定期进程保级（清后台时待遇高一档）；Android 12+ 的
    "禁止后台起前台服务"对已关联伴生 App **豁免**（看门狗/自愈拉服务不再被拒）；
  - **MAC 直连重连**：`BleManager.connectPreferDirect()` 用已存 MAC 直接
    `connectGatt`，**不扫描**——绕开 Android 后台扫描限流（ColorOS 停发结果的根源，
    BleManager 文件头注释实录），连续 3 次直连失败退回扫描发现。任意一次连接成功
    也可学习 MAC（scan 路径兜底存档）。
- **代价**：不解决"强行停止"（系统安全底线，谁都绕不过）；ColorOS 魔改下特权可能
  打折；配对框为系统 UI 不可定制（一次性动作）。
- **放弃**：setDeviceProfile(WATCH)——当前诉求是保级+后台拉服务，关联本身已覆盖；
  后续要更多手表级特权再补。

## ADR-026：确认窗音频"缓冲不丢"——开头丢字根治

- **日期**：2026-09-23
- **背景**：用户报障：按下听到"滴"后立刻说"1233211234567"，收到的从"3"开始。根因：固件把
  按下后前 ~450ms 的音频帧**主动丢弃**——300ms 防误触确认窗（快按=撤销，期间帧不要）+
  150ms"防杂音窗"（防旧模型现场开 mic 的初始化杂音）。mic 常开后这些帧其实**录上了**，
  是软件扔掉的；"12"正落在被丢区间。防杂音窗在双工模型下纯属历史遗留。
- **选择**：确认窗内帧**编码后入环形缓冲**（64 包 × 20ms = 1.28s），commit（300ms 确认）
  时把缓冲按时间序整体冲出再续流式——按下起零丢失。删掉防杂音窗。误触语义不变：
  <300ms 松手 = 丢缓冲 + 零信令。
- **实测（ASR 污染验证）**：担心"滴"声段进录音会污染识别——本地 whisper 实测
  "0.15s 1kHz 单音 + 数字串"对照纯数字串，两组都完整听出数字、单音未产生乱字。
  结论：滴声段放心进录音开头；若线上 ASR 出现污染再裁前 ~250ms。
- **代价**：多 64×64B 缓冲（4KB）；rec_stop 的 duration 覆盖完整按下区间（语义更对了）。

## ADR-027：音频前端按需上电——推翻 ADR-023 的"双工常开"（功耗不可接受）

- **日期**：2026-09-23
- **背景**：ADR-023 为根治"按下没滴声"把 ES7210+ES8311+PA **全时上电**。用户质询：
  "麦克风双工常开肯定是极度耗电的……麦克风用的时候再开吧"——成立。挂坠 400mAh，
  ES7210（双麦 ADC+前置，估 10~15mA）+ ES8311（数 mA）+ PA 静态（数 mA）+ I2S 时钟
  常转，待机代价不可接受。ADR-023 的"估算 +几 mA"低估且未充分告知，记为失误。
- **选择**：**音频作业队列**（FIFO，单任务 audioTask 串行执行上电/播放/断电）：
  - 会话开始（按下）：入队 上电@16k（spk+mic 配对舞蹈一次做完）→ 入队"滴"——
    **先开麦后放滴**（用户拍板的次序）：上电的 I2S 舞蹈不再掐碎在播提示音（旧竞态），
    配对舞蹈保证 TX 数据通路出数（冷路径根治），且开麦完成在用户开口之前=开头零丢失；
  - 会话结束（真松手/误触）：入队 滴滴 → 入队 断电（mic→spk 次序守约束④）——
    整条前端回零耗；会话外的 receipt"叮"= 临时上电、播完即断电；
  - talk（M5）：入队 上电@24k 换档，teardown 断电回零耗；
  - 开机自检：上电→冒烟滴→断电。
- **代价**：按下后"滴"晚响 ~0.2~1s（等上电；本板 I2C 慢，实测为准）——换来开头语音
  零丢失 + 待机零耗，用户明确选后者。**风险**：会话级 open/close 回到 ADR-023 之前的
  开关模式，冷路径/竞态两坑靠"作业次序（先开后播后关）+ 单任务串行"根治——需真机回归
  验证首段录音与提示音。
- **推翻**：ADR-023 的"双工常开"取舍（其根因分析仍有效：掐滴竞态、冷路径 DOUT8 零翻转
  皆实锤；只是解法从"常开"改为"次序正确的按需开关"）。
- **功耗账（估算，待实测修正）**：待机从 ~15~25mA → **~0**（前端全断，仅 BLE 空闲）；
  每次会话多 0.2~1s 上电开销（一次性）。

## ADR-028：消息卡反馈闭环 UI——圆屏消息流 + 详情页直达（fw-idf 0.3.0）

- **日期**：2026-09-24
- **背景**：M1 链路单向：说话→飞书，设备只见"已送达"，发了什么/回了什么全靠手机看群。
  用户拍板：设备屏幕要闭环——"我发了什么要求，以及 AI 的回复，在嘎嘎上都能看到"。
- **交互定案（用户逐条拍板）**：
  - **消息卡列表**为待机主页（不再是"GAGA IDF"大字）：一条 = 一问一答，保留 **20 条**
    （RAM 环形缓冲 ~13KB，重启即清——持久化等真用出需求再挂 NVS/LittleFS）；
  - **顶部状态栏**（圆屏收中央一排，不贴边——圆角安全区约束）：时间 | 电量(+充) | BLE；
    时间来自 PCF85063 RTC（断电走时）+ 信令 `ts` 自动对时；电量来自 AXP2101
    BAT_PERCENT_DATA(0xA4)/STATUS2(0x01)（寄存器语义对照官方 XPowersLib）。
  - **松手立即出占位卡**（"你：识别中…"），receipt 填 ASR 文本转"正在回复…"——即时反馈优先；
  - **reply 到达**：亮屏中就地填充 + 叮咚双音（不抢屏）；**息屏中自动亮屏直达该卡详情页**
    （= 用户手动点开，全文展示——用户点名"要完整显示，不是屏幕上的简写"）；
  - 60s 无回复软超时"（还没回复）"，迟到 reply 照样点亮；点击任意卡进详情页（全文滚动）。
- **音效分工**：松手"嗖"（暂用"叮"顶）= 发出去了；回复"叮咚"（E6→B5 双音合成）= 答案来了。
  receipt **不再响音**（与松手音连响太吵，卡片填字即反馈）。
- **圆屏布局约束**：内容宽 300px 居中（466 圆 ±160px 高度处半宽 ≥155，全程不切边）；
  状态栏"左中右"三段收中央一排——用户点名"最上面不要一整排放，左右往中间收"。
- **存储取舍**：消息卡纯 RAM——20×(192+384+24)B≈13KB 内部 RAM 放得下，写 flash 反而
  浪费寿命；字库（GB2312 全集 7448 字 ≈1MB）在 **flash**（.rodata 随固件镜像），RAM 零占用。
- **放弃**：Markdown 富文本渲染（lv_spangroup 迷你渲染器 v2 再议）——圆屏 16px 单字号
  施展不开表格/嵌套列表；服务端 `textfmt.flatten_markdown` 拍平成纯文本（复杂度压 server，
  宪法）：去粗斜体/标题/反引号标记留内容，列表转"· "，表格转"a / b"，链接留文字。
- **问答复配**：FIFO（单人佩戴够用）；`msg_id` 精确复配留给飞书官方 API 阶段（ADR-029 规划中）。
- **真机修正（2026-09-24 验收期实锤）**：
  ① `SPIRAM_MALLOC_ALWAYSINTERNAL` 16K→256：消息卡 20 卡×3 对象全是小块 malloc，
  "小块内部优先"绕过 `SPIRAM_MALLOC_RESERVE_INTERNAL` 的 DMA 保留把内部堆吃到 6.6KB，
  SPI 刷屏反弹缓冲（MALLOC_CAP_DMA）分不出来 → 满屏 `Draw bitmap failed: ESP_ERR_NO_MEM`。
  小块改落 PSRAM 后内部堆 BLE 起后仍有 ~21KB，六连注入零 NO_MEM。
  ② PCF85063 @0x51 本机无应答（I2C 扫描实锤），时钟改**软件时钟兜底**：信令 `ts` 对时
  写 RAM 基准 + millis 推算；硬件 RTC 在位时硬件优先（断电走时能力保留）。新增
  `hello_ack` 信令保证开机即对时。
  ③ serialTask 栈 4K→8K：串口 'p' 像素快照（lv_snapshot 调用链）压爆 4K 栈。

## ADR-029：飞书入向 = 官方 API 长连接（lark-oapi ws），发送侧维持自定义 webhook

- **日期**：2026-09-24
- **背景**：M1 链路单向——服务器经 webhook 把 ASR 文本发进群（feishu.py 21 行，只能发
  不能收），Hermes 在群里的回复回不到设备。设备端消息卡 UI（ADR-028）的"GAGA："栏
  只能靠 `POST /debug/reply` 手工注入联调。
- **选择**：
  - 新增 `feishu_in.py`：自建应用（app_id `cli_aa3ebd…`）以 **lark-oapi ws.Client
    长连接**订阅 `im.message.receive_v1`，SDK 自带断线重连；收到群消息 →
    过滤（目标 chat_id / 指定回复者 open_id / 🦆 前缀回声）→ `flatten_markdown` →
    `{"type":"reply","text":…}` 下行 gaga/down。不配置凭证则跳过启动（debug 注入兜底）。
  - **发送侧不动**：上行进群仍走自定义机器人 webhook。
  - 过滤初值：FEISHU_CHAT_ID / FEISHU_REPLY_SENDER 留空 = 不过滤，日志打印每条
    消息的 chat_id/open_id 供回填收紧（首条消息即可定参）。
- **理由**：① 长连接免公网 HTTPS 回调——本地 Mac 直跑与 dokploy 生产同一条链路，
  回调 URL 方案在开发机要内网穿透；② 一次一问场景问答复配设备端 FIFO 已够
  （ADR-028），发送侧换 Bot API 拿 message_id 做精确复配是"需要时再做"，第一步
  不引入；③ lark-oapi 是官方 SDK，事件分发/重连/心跳不用自维护（长连接协议
  未公开文档化，手写 websockets 客户端是逆协议，不做）。
- **放弃**：事件订阅回调 URL（公网端点 + challenge 握手 + 可选事件加密，部署门槛
  高）；出向同步换 Bot API（多一条 token 管理，暂无收益）；引入消息卡片交互
  （按钮回复等，M6 之后再说）。
- **备注**：① 应用侧前置条件（飞书开放平台控制台）：权限
  `im:message.group_msg:readonly`（收群消息事件）+ `im:chat:readonly`（拉群列表），
  事件订阅添加 `im.message.receive_v1` 且**订阅方式选"使用长连接接收事件"**，机器人
  拉入目标群；② lark-oapi 约束 `websockets<16`（uv 解析 17.1→15.0.1），现有
  `additional_headers`/`proxy=None` 用法在 15.x 兼容，realtime/volc_stream 不受影响；
  ③ Hermes 若连发多条消息，每条各点亮一张等待卡（单设备一次一问，多条属罕见）。

## ADR-030：接入端抽象 channels/（模仿 openclaw）+ 飞书全量官方 API 收发，webhook 退役

- **日期**：2026-09-24
- **背景**：项目要发布为开源仓库，飞书只是第一个接入端——别人可能用微信，海外用户
  可能用 Telegram。ADR-029 的入向长连接方案当天被用户决策推翻一半：发送侧也不再走
  自定义 webhook，统一走官方 API；且要求接入端可插拔。
- **选择**：
  - 新增 `server/src/gaga_server/channels/`：`base.Channel` 抽象（`send_text(text)→msg_id`
    出向 / `on_message(text)` 入向回调 / `start()` 拉接收端）+ `__init__.py` 注册表工厂
    （`CHANNEL` 环境变量选平台，目前 `feishu`；新平台 = 新文件 + 注册一行，server
    其余零改动——openclaw 的 channel adapter 模式）。
  - **飞书适配器全量官方 API**（自建应用 `cli_aa3ebd…`）：
    - 出向 `im/v1/messages`（bot 身份），返回 message_id → `receipt` 信令携带真
      `msg_id`（data-model.md 预留字段从此有值，为将来问答复配铺路）；
    - 入向 **`im/v1/messages` 轮询**（`FEISHU_POLL_INTERVAL`，默认 2s）：启动先拉一轮
      只记 message_id（历史不当回复重放），之后新消息过过滤链 → `on_message` →
      `flatten_markdown` → `reply` 信令下行。
  - 配置全环境变量：`CHANNEL` + `FEISHU_APP_ID/SECRET/CHAT_ID/REPLY_SENDER/
    POLL_INTERVAL`；`.env` 已实测回填（chat_id `oc_9beabcb8…` 群 GaGa；
    回复者 `cli_a9f77be0…` = Hermes，从群消息实查得知）。
  - `FEISHU_WEBHOOK` 配置项删除；错误码 `WEBHOOK_FAIL` → `CHANNEL_FAIL`（设备侧
    只显示 msg 文本，不受影响）。
- **理由（轮询 vs 事件订阅）**：① 事件长连接虽握手成功（ADR-029 验证），但控制台
  事件订阅（im.message.receive_v1 + 长连接模式）未配置就收不到任何事件，多一道
  只有人工能做的控制台工序；② 消息列表 API 权限已实测可用，且返回 sender 带
  **app_id**——Hermes 是应用（cli_a9f77be0…），事件 payload 里应用类发送者的
  sender_id（open_id 域）反而过滤不可靠；③ 轮询 2s 的延迟对"回复点亮消息卡"
  完全够（设备侧 60s 等待窗）；④ Telegram 生态本来就是 polling（getUpdates），
  轮询模式跨平台可复制。
- **放弃**：事件订阅长连接（升级路径，要更低延迟/更少 API 调用时再补，握手代码
  验证过可行）；多接入端并行路由（单设备阶段 CHANNEL 单选，多平台路由等多设备/
  多用户需求真实出现再做）；`reply` 精确复配（receipt 已带 msg_id，设备端 FIFO
  一次一问够用）。
- **备注**：① 防串扰三道闸：目标 chat_id（自动发现仅限唯一群）、指定回复者
  （`FEISHU_REPLY_SENDER`，未配置时默认只认应用类消息——人的闲聊不点亮消息卡）、
  上行回声（自家 app_id + "🦆"前缀双保险）；② 群里除 Hermes 外还有别的 bot
  （如 Kimi cli_c08abc1d…），REPLY_SENDER 已按 Hermes 收紧；③ 实测记录
  （2026-09-24 端到端）：官方 API 发"适合骑车吗"→ Hermes 18s 回 post 富文本 →
  轮询 2s 内捕获 → on_message 全文到手（探针 scripts/feishu_channel_probe.py）；
  ④ **ListMessage 大坑**：默认排序从建群最旧消息开始，不显式 sort_type=
  ByCreateTimeDesc 时轮询永远盯历史首页、新消息静默丢失（90s 实测无输出才发现），
  已在 _list_messages 修复并注释；⑤ Hermes 偶尔回 audio 类型消息，当前只认
  text/post，audio 跳过（需要再处理）。

## ADR-031：正式 UI = 深空黑主题 + 设备端设置系统（"设备即应用"路线定案）

- **日期**：2026-09-24（深夜）
- **背景**：M6 消息卡 UI 上线时用的是黄底黑字（2026-09-23 用户指定的验收期诊断色，
  表盘/AOD 里程碑再换）。2026-09-24 用户反馈两条：① 黄底+卡片白边"看起来奇怪"，
  要求整体重设计；② 状态栏离下方内容太近、离圆顶太远。同时提出基础功能需求
  （设置/音量等）与产品方向之问："拿起来就能用"的助手，需要 App 管理还是
  打开就是应用？
- **产品决策（对方向之问的回答）**：**设备即应用**。圆屏+双键+触摸的设备本体
  就是应用的全部界面，开机即对话；手机 App 永远是隐形网线（哑管道定位不变，
  CDM 配对+自动重连让它"存在感为零"），不是管理界面；**设置做在设备触摸屏上，
  不做进 App**——这是本路线的第一步落地；终局是 M6 的"在家 WiFi 直连模式"
  （设备直连 MQTT，摆脱手机，外出 BLE 兜底），到那时才是完整"拿起来就能用"。
- **选择**：
  - **深空黑主题**：纯黑底（AMOLED 黑像素零功耗，AOD 顺路）+ 白主文字 +
    鸭黄 `FFD60A` accent（吉祥物品牌色延续）+ 次要灰 `8E959E`。两级半透明
    层级：卡面白 10%（≈#1A1A1A）、控件底白 20%。卡片 `lv_obj_remove_style_all`
    后自绘样式——LVGL 默认主题的 1px 白边框（用户观感"奇怪"的来源）根除。
  - **状态栏重排**：`电量·BLE圆点 | 时间(montserrat_24) | 设置入口`，整体
    上移至 y=22 贴圆顶（时间做大字号视觉锚点，圆表盘经典排布），与主体
    （CENTER+24，高 290）拉开 62px 呼吸区——一举解决"太近"与"太靠下"。
  - **设置系统**：`state/Settings.cpp`（NVS namespace "gaga"，全量写回）：
    音量 0~100（ES8311 out vol，spkOpen 后必设）、亮度 10~100（CO5300 0x51
    寄存器，走 BSP 函数——自组显示链路同样可用，bsp_display_new 会登记静态
    io_handle）、息屏时长 5/10/30/60s（AppState 常量改实例成员）、提示音开关
    （beep/chime 空转，talk 下行不受影响）。设置页全触摸：slider 拖动实时生效、
    松手才落盘（NVS 写频率友好）；关于页（版本/设备/电量/运行时长）。
    分层：Ui 经 `onApplySetting` 回调送 audio/display/appState——UI 层不碰硬件，
    宪法（app 哑管道/设备无业务逻辑）不破。
  - 串口验证入口：'S' 设置页开关、'V' 音量循环、'L' 亮度循环（无触摸调试路径）。
- **理由**：① 黄底诊断色使命完成（M6 验收过），正式视觉选黑底是功耗（AMOLED
  特性）、AOD 铺路、审美三者同向的选择；② 设置进设备不进 App：设备有触摸屏，
  就地调整所见即所得，且不依赖手机在线——与"设备即应用"自洽；③ 音量落在
  ES8311 硬件 DAC 音量（esp_codec_dev_set_out_vol），不打 software volume
  补丁——codec 本来就有寄存器，白送的精度；④ 亮度走 BSP 0x51 命令，自组链路
  复用 BSP 静态 io_handle，零新代码。
- **放弃**：App 内设置页（依赖手机在线，方向相反）；LVGL 内置主题再配色
  （默认主题的白边与控件默认样式是观感问题的根源，remove_style_all 自绘更可控）；
  设置项继续硬编码（用户已开口要设置功能，硬编码 85% 音量的时代结束）。
- **备注**：① 真机验证方式：串口 'p' 像素快照——非黑像素从黄底时代 95%+ 降到
  31%（只剩卡面与文字），设置页五行分带墨迹均匀分布（0 1000 7790 14532 17302
  14258 12695 0）证实五行渲染；② 验证期间抓到用户真实对话全链路（录音→ASR→
  飞书→Hermes reply 下行）在 0.4.0 上正常工作，无崩溃——最有说服力的回归；
  ③ montserrat_24 已在 sdkconfig.defaults（此前 LVGL 字体探针期开的），无新增
  构建配置；④ AMOLED 烧屏预防：深色主题下静态元素亮度已大幅下降，AOD 里程碑
  再做像素位移。

## ADR-032：文本可显示性过滤放设备端（lv_font_get_glyph_dsc 原地剔除），不做服务端共享字符集 JSON

- **日期**：2026-09-24
- **背景**：真机报障 reply 里的"——"（U+2014）显示成方块字——字库按 GB2312 收全（横线
  映射是 U+2015，形同码异），AI 输出的 U+2014 落在集合外。用户要求过滤不可显示字符，
  给了两方案：服务端共享字符集 JSON 联动过滤 / 设备端自知自滤。
- **选择**：设备端（fw-idf 0.4.1，`ui/FontFilter.cpp`）：动态文本（receipt/reply/error/
  talk 字幕）入库上屏前逐字符查**本机实际烧录的** `gaga_font_cjk_16` 字形表，
  查不到的连同 UTF-8 字节整体删除；\n 白名单（布局语义）；非法 UTF-8 序列丢弃。
  main.cpp 四个文本入口统一走 filterToBuf。
- **理由**：① 字符查表微秒级，两方案续航无差（不构成决策因素）；② 设备端运行时自证，
  字库升级/回滚与服务端永不漂移——共享 JSON 一旦和已刷固件不同步，方块字照旧
  （本次报障本质即"字库重生成与固件版本不同步"的漂移形态）；③ 显示能力是设备本地
  知识，非业务逻辑，不违反"复杂度集中服务端"宪法；④ 未来新字符源（WiFi 模式、
  其他 channel）自动被覆盖，零协调成本。
- **放弃**：服务端共享字符集 JSON（漂移风险 + 无续航收益 + 三方同步维护成本）；
  字体 fallback 链（只解决 ASCII 补位，不解决 CJK 外符号/emoji 的方块）。
- **备注**：emoji 依旧被本过滤静默删除（消失比方块体面）；要真显示 emoji 需嵌入彩色
  位图字体，flash 代价大，未做。同期字库已补 U+2014/U+2013（assets/fonts/README
  有坑记录），本过滤是它之上的兜底网。

## ADR-033：录音链路 ASR 端点改单向流式（bigmodel_nostream）

- **日期**：2026-09-24
- **选择**：`VOLC_ASR_ENDPOINT` 默认改为 `sauc/bigmodel_nostream`（单向流式，整句返回），
  接收器补认 `is_last_package=true` 收尾信号；资源 ID 不变（volc.seedasr.sauc.duration）。
- **理由**：用户拍板——按住说话不需要中间结果（那是会议字幕场景），官方文档明言
  单向"准确率优于双向流式接口"，协议实测兼容（sauc 帧负包收尾 3/3 通过）。
- **代价**：终稿延迟实测 0.39s vs 双向 0.18s（多 ~0.2s，换文档声称的更高准确率）。
  A/B 探针留档 `scripts/asr_ab_test.py`，识别质量争议可随时复测。

## ADR-037：三种唤醒 + 低功耗一天路线（IMU 硬件双击 / 抬手姿态 / WakeNet 关键词）

- **日期**：2026-09-25（凌晨）
- **背景**：用户提出三项：挂脖拿起亮屏（抬手）、指敲两下唤醒（双击）、
  关键词唤醒（"官方支持哪些词就用哪些词"，点名 你好乐行/你好ESP/你好小智），
  且"不想要麦克风一直开着"、目标续航一天。
- **硬件事实（决定方案形态）**：QMI8658 中断脚未引出（docs/hardware.md）→
  深睡时 IMU 无法唤醒主控，抬手/双击只能在 CPU 运行态轮询；BSP 无 IMU 驱动
  （BSP_CAPS_IMU=0）→ 自写寄存器级驱动（寄存器序列抄 SensorLib 同款）。
- **选择**：
  - **双击 = QMI8658 硬件 Tap 引擎**（CTRL9 命令通道 CONFIGURE_TAP，双击窗
    500ms；检测在传感器内部跑，主控 50ms 轮询 STATUS1 bit10，读清除）——
    比软件尖峰检测可靠（ODR 500Hz 官方推荐）且 µA 级开销。accel 4G@500Hz 常开。
  - **抬手 = 软件姿态状态机**（motion/Wake.cpp）：Baseline（学静息方向 2s）
    → Motion（|a| 偏离 1g>0.3 或方向变 >45°）→ 1.5s 窗内"屏幕朝上稳定"
    （Z>0.72g、模长≈1g、与基线不同向、持稳 250ms）→ 亮屏。走路晃动不亮
    （姿态仍朝前）；焊装 Z 极性反了串口 'K' 一键翻转。
  - **关键词 = esp-sr WakeNet9**（espressif/esp-sr ^2.0，组件经 idf_component.yml
    拉取）：唤醒词选 **wn9_hilexin"Hi,乐鑫"**（你好乐行，乐鑫主推最成熟；
    Hi,ESP/你好小智模型同库可换，sdkconfig 一行）。**做成设置开关默认关**：
    用户明言不想要麦克风常开——开着才 mic 常开本地推理（AFE 只跑 WebRTC NS +
    WakeNet，关 AEC/SE/VAD/AGC；录音/talk 时 KWS 让路挂起）。AFE 内存走
    MORE_PSRAM。
  - **低功耗三层**：① 息屏降频 240→80MHz（`rtc_clk_cpu_freq_set_config`）；
    ② 挂机深睡：息屏 + BLE 未连 + 未充电 10min → `esp_deep_sleep`（ext0 =
    BOOT 键 GPIO0 低电平唤醒，冷启动后 App 自动重连）；③ IMU µA 级常开。
    功耗账：亮屏 ~100mA / 息屏浅待机 ~25-40mA / 深睡 <1mA，"一天"的达成路径
    = 间歇使用 + 挂机自动深睡兜底。
- **踩坑实录**：① `rtc_clk_cpu_freq_set_xtal()` 会**关 BBPLL**——USB-Serial-JTAG
  和 BLE modem 的 48MHz 时钟同源，息屏降频瞬间串口当场掉线（真机复现，
  设备靠"双击唤醒→亮屏恢复 240MHz"自救回来）。必须用 `set_config(80MHz)`
  （PLL480/6 分频，PLL 保持）。② esp-sr 的 Kconfig 关不掉 nsnet3：CMake 把
  全部预编译库塞进 --start-group，libnsnet 的 NS 模型工厂无条件引用 nsnet3/
  gtcrn 的 RAM 权重表（101KB DRAM），链接即段溢出——解法 `voice/NsnetStub.c`
  同名强定义顶替工厂（我们只用 WebRTC NS 不调它），链接器不再拉入 nsnet3。
  ③ ext0 GPIO 唤醒 API：`esp_sleep_enable_ext0_wakeup`（GPIO0 是 RTC IO）。
- **放弃**：深睡时 IMU 唤醒（中断脚未引出，硬件死结，等下一版硬件）；
  esp_pm DFS/tickless（全局行为变化大，留功耗实测后评估）；WN9S 三词同载
  （内存换唤醒词数量，真机稳定后再开）。
- **备注**：全部待真机回归（用户约定次日统一烧录）：双击灵敏度/抬手阈值
  参数在 motion/Wake.h 顶部常量，串口 'M' 看姿态现场、'Z' 验深睡唤醒路径。

## ADR-038：固件模块化重构（main.cpp 1050 行 → 330 行装配层）

- **日期**：2026-09-25（凌晨）
- **背景**：用户要求"代码重构一下，更清晰更简洁更直观，让我能够读懂"。
  main.cpp 已长到 1092 行（录音状态机、上行泵、串口调试命令、快照工具全塞
  一起），唤醒/低功耗再塞进去就不可读了。
- **选择**：按领域拆模块 + 装配上下文：
  - `AppContext.h`：全模块指针集合，main 装配时填一份，模块经它取兄弟——
    "谁用谁"一眼可读，免层层 setter。
  - `rec/Recorder`：按住说话语义（确认窗/松手宽限/锁定/rec_start-stop/建卡）。
  - `audio/UplinkPump`：上行泵任务（mic→Opus→BLE，含确认窗帧缓冲）。
  - `motion/`（Qmi8658 驱动 + Wake 检测）、`power/Power`（降频+深睡）、
    `voice/KeywordWake`（WakeNet）、`debug/SerialCmd`（串口命令+快照工具）。
  - `GattServer` 发送接口**内置互斥**（原 main.cpp 的裸 mutex 下沉），多任务
    并发直调。
  - main.cpp 只剩：装配、按键/信令事件接线、任务拉起。
- **理由**：纯机械搬迁（行为不变）+ 一处真正的内聚修复（BLE 发送锁归位）；
  每个模块一个头文件讲清"它管什么、不管什么"，可读性即文档。
- **备注**：重构后构建通过（RAM 20.7%/Flash 40.2%）；真机回归随 0.5.0 一起。

## ADR-034：实时对话三优化（provider 可插拔 / 字幕开关 / ASR 分层清屏）

- **日期**：2026-09-25（凌晨）
- **背景**：用户三点反馈：① 不应只接豆包，要预留 Gemini/"星辰"等可配置；
  ② talk 长按期间"一卡一卡"，疑似字幕渲染拖累，要给显示文字加开关；
  ③ ASR 文本是分层发的，上一句会残留叠加（"我的我的什么我的什么是什么"），
  要求"这句出来的时候，上一句就可以清掉"。
- **选择**：
  - **① provider 抽象**：`realtime/base.py` 定义 RealtimeProvider + **归一化
    事件契约**（session_created/asr_started/asr_delta|final/reply_delta|final/
    audio_delta{ogg}/exit_intent/error/usage）——厂商协议差异（事件名、音频
    格式、快照/增量语义）全部消化在 provider 内部，bridge 只吃归一化事件。
    `volc.py`（豆包，原 client.py 迁移+归一化，已联调语义）；`gemini.py`
    （Gemini Live BidiGenerateContent，上行 Opus→PCM/下行 PCM→Opus+Ogg 双向
    转码，pyogg 懒依赖，**实现完整待 API key 联调**）；`REALTIME_PROVIDER`
    环境变量选择；"星辰"= base.py 三步接入法（新文件+注册一行）。
  - **② 对话字幕开关**：Settings.talkSubtitles（NVS 持久化，默认开）+ 设置页
    "对话字幕"行 + 固件 `Ui::setTalkText` 关=纯语音短路——字幕渲染的 LVGL
    长文本重排（CJK 逐字测宽+wrap）是 talk 卡顿的最大嫌疑，开关即根治；
    串口 'X' 同步切换。
  - **③ ASR 分层清屏**：豆包 `input_audio_transcription.started`（新一句开始）
    → provider 报 asr_started → bridge **立即**（不节流）下发
    `{"type":"talk_asr","text":"","final":false}` → 固件清掉上一句再显示新句。
    协议语义补进 protocol.md §3（空 text = reset）。
- **理由**：①事件归一化让新后端不碰 bridge（改一处 bug 全后端受益）；
  ②卡顿优先按用户假设给出开关（可立即止血），渲染优化另行实测；③清屏
  动作放服务端下发（设备固件 0.3.0/0.4.0 不升级也能吃到一半收益——旧固件
  空 text 只是显示空，新固件才真正清屏）。
- **放弃**：把 ASR 快照合并挪到设备端（固件已溢出风险，且 provider 归一化
  后设备只收"本句累积文本"，职责更清晰）；Gemini 服务端 ASR 字幕（Gemini
  的 input transcription 区域可用性受限，音频通路先行，字幕升级路径留注释）。

## ADR-035：顶部下拉快捷面板（控制中心手势）

- **日期**：2026-09-25（凌晨）
- **背景**：用户反馈"屏幕有时候太亮"，并给出交互要求：**从上面往下滑是状态栏，
  可以调整亮度之类的**——手机下拉控制中心的心智模型，调亮度不该绕进设置页。
- **选择**：
  - **手势区**：顶部 280×76 透明区（盖住状态栏弧区，pullZone）。
    ① 按住**下滑 >45px** → 呼出快捷面板（跟手）；② 轻点（位移 <15px）
    **右角（x>330，"设置"文字区）** → 进设置页（原入口语义保留）；
    ③ 轻点**其余区域** → 直接开面板——拖动和点击双通道，新手可发现性。
  - **面板内容（高频四件）**：亮度大滑条（第一行——太亮痛点）、音量大滑条、
    语音唤醒/对话字幕宽胶囊开关、收起按钮。slider 拖动实时生效、松手落盘，
    与设置页共用 applySetting 落地链路（两处 UI 永远同步）。
  - **交互闭环**：面板 220ms ease 滑入屏顶 y=8；收起三通道——点"收起"、
    点面板外全屏透明罩、15s 无操作自动收（防忘记关白亮屏）。录音/talk
    开始时强制收+手势区隐藏让路（覆盖层优先）。
- **理由**：①亮度/音量是佩戴设备最高频的两个调节，一步直达；②点/拖双
  通道降低学习成本；③罩+自动收兜底所有忘记关的路径。
- **放弃**：面板里塞息屏时长/关于（低频，留在设置页，面板保持四件套极简）；
  上滑收起手势（有罩+按钮+超时三通道，再加手势边际收益低）。
- **备注**：串口 'Q' 无触摸验证路径；真机回归项：手势阈值（45px）在圆屏
  弧区的手感、右角点击分界（x>330）是否顺手。

## ADR-036：导航模型定稿——右滑返回 + 鸭子页（陪伴层），砍掉任务管理器

- **日期**：2026-09-25（凌晨）
- **背景**：用户提出对照手机交互：返回按钮保留 + "从左往右滑是返回"必须
  有；顶部下拉=状态栏（已做 ADR-035）；底部上滑=任务管理器——用户倾向
  不要（征求意见）；"打开应用后从右往左滑"放什么没想好，倾向通知+小鸭子
  （"UI 更美观，不那么鸡肋，给女生用也愿意用"），但顾虑"看消息要滑一下
  没那么方便"。
- **决策一：底部上滑任务管理器 = 不做**。理由：①"设备即应用"（ADR-031）
  没有多任务，任务管理器无对象；②上下滑已被列表滚动占用——顶部下拉能
  成立恰因状态栏区不在列表内，底部上滑必与滚消息卡冲突。
- **决策二：右滑（从右往左滑）= 鸭子页，定位陪伴层不是通知**。用户顾虑
  正中要害：本设备通知源本质=回复，已在消息卡+叮咚+亮屏直达里，单独通知
  页与首页重复才是真鸡肋。鸭子页的价值在温度：
  - 自绘扁平小鸭子（7 个纯色 obj 拼合：黄头/黑眼白高光/橙嘴/粉腮红/
    黄胶囊身/深黄翅膀/青涟漪——深空黑主题上的品牌温度）
  - 按时段问候（夜深了/早上好/…）
  - 今日概览（聊了 N 句/电量）
  - 鸭子气泡 = 最近一条回复摘要（通知语义收编为气泡，不另起页面）
  看消息照旧首页一步到位；滑一下是"看看鸭子看看今天"。
- **决策三：手指右滑（从左往右滑）= 返回**，详情/设置/鸭子页通用，
  返回胶囊按钮保留（用户拍板"按钮+手势都要"）。实现走 LVGL indev 级
  `LV_EVENT_GESTURE`（快滑触发、与列表滚动共存，慢速拖动不误触）。
- **导航模型定稿**：中央上下滑=消息列表；顶部下拉=控制中心（ADR-035）；
  首页左滑=鸭子页；子页面右滑=返回；物理键=录音/对话/亮屏（ADR-016）。
- **放弃**：鸭子页内嵌完整天气/日程（等 WiFi 直连有数据源再说）；AOD 表盘
  （roadmap 保留，未来=鸭子页的降亮度特例——鸭子页就是表盘的骨架）。
- **备注**：串口 'C' 无触摸验证；真机回归：gesture 手感阈值、鸭子摆放
  视觉（buildDuck 坐标集中可调）、气泡与鸭头的贴合度。


## ADR-035：实时对话接入阶跃星辰 Step Audio 3 Realtime；对话引擎选择权在小设备

- **日期**：2026-09-24
- **选择**：
  - 新增 `realtime/step.py`（provider 第三员）：端点 `wss://api.stepfun.com/v1/realtime`，
    Bearer 鉴权，`session.update` 建 会话（model=`stepaudio-3-realtime-preview`，
    server_vad），事件 = OpenAI Realtime 风格（input_audio_buffer.append /
    response.audio.delta / response.audio_transcript.delta 等）。
  - 音频转码：Step 只收发 **pcm16**（上行 16k / 下行 24k）——新增
    `realtime/opus_codec.py`，ctypes 直调 brew libopus（~90 行，零第三方依赖）。
  - **选择权在设备（用户拍板）**：`talk_request` 信令新增 `provider` 字段（设备
    设置页"引擎：< 豆包/星辰 >"循环，NVS 持久化）；服务端 bridge 按它建连，
    信令未带（旧固件）回落 `REALTIME_PROVIDER` 环境变量。
- **理由**：设备是用户直接交互的入口，切换对话引擎属于使用偏好；服务端/App 全量
  接入但不代选（符合"复杂度集中 server，决策权在端"的产品定位）。opus_codec
  弃 pyogg：0.6.14a1 三处硬伤（__init__ 不导出编解码类、opus.py 引用未定义的
  c_int_p、类壳无 encode/decode），且依赖 libogg/libopusfile 全家桶；ctypes 直调
  只需 brew libopus 一个库，Linux 部署（dokploy）同函数签名直过。
- **放弃**：pyogg（alpha 半成品）；服务端配置选引擎（违背用户拍板的设备主权）；
  App 侧选择 UI（哑管道不碰业务）。
- **备注（2026-09-24 真实 API 联调通过）**：① Step 无 ASR started/delta 事件——
  asr_started 清字幕语义用 `input_audio_buffer.speech_started`（服务端 VAD）替代，
  ASR 终稿一次性 `completed`；② 会话最长 30 分钟，无 session.close 事件（直接断开）；
  ③ **协议三坑（联调实测）**：model 必须放 URL 查询参数（缺席 400 "model is empty"，
  session 字段无效）；server_vad 模式下手动 commit 被拒（error "commit when server
  vad"，属软错误只记日志不断会话）；收包循环必须在 connect 内先行启动（session.created
  握手即推，晚了丢事件）；④ 端到端实测：真实语音上行 → ASR 终稿/回复终稿/下行 Ogg
  分片全通，回复内容角色一致性正常（"我是gaga，一只挂你胸前的鸭子"）。

## ADR-036：实时对话万金油工具 send_to_hermes——占位立即回 + 飞书通道转 Hermes + 回流双通道送达

- **日期**：2026-09-24（当日真机联调 + 次日回流修复）
- **背景**：用户在实时对话里说"智能家居/帮我记录"类执行型指令，模型只会聊天。
  要求：函数调用转交 Hermes 执行；**函数立即返回占位结果**（对话不能卡死）；
  真实结果完成后"插入"实时会话告知用户。通道即飞书 provider（无 Hermes 直连）。
- **选择**：
  - `session.create.tools` 注册 `send_to_hermes(text)`（JSON Schema，火山文档
    标准格式）；FC 事件归一化 `{kind:"function_call", call_id, name, args}`。
  - 分发：立即 `conversation.item.create(role=tool, call_id)` 回占位回执 +
    `channel.send_text("[嘎嘎实时指令] …")` 进群喂 Hermes，平台 msg_id 记入
    pending 表（call_id, 时刻）。
  - 回流路由（TalkBridge.on_channel_reply）：① parent/root 精确配对；② 无引用
    回复 + 投递 180s 窗内 → 配最旧（兜底）；③ 都不中 → 普通 reply 信令。
    命中后**双通道送达**：inject_context 插回会话（尽力而为）+ reply 信令
    点亮消息卡+叮咚（100% 可靠）。
- **真机联调实测（2026-09-24/25 日志为准）**：
  - 语音"帮我在群里问一下成功日记"→ FC 触发 → 1s 内占位口头确认 → 指令进群 ✓
  - **坑1**：FC 事件字段包在 `items[0]`，顶层读全空（初版 args 全丢）；
  - **坑2**：ASR completed 依赖 mute 信号——pacer 断流 1s 自动 mute 恰好满足；
  - **坑3**：Hermes 回复常**不带引用**（首条结果被"引用链校验"丢弃 = 用户报的
    "realtime 收不到"）；且会**回错线程**（第二条回复 parent 指向第一条）→
    引用链只做精确配对，另加时间窗兜底；引用链过滤从 channel 层移除；
  - **坑4**：注入不进上下文——user role item.create 无 ack 且模型无感知；
    `response.create` 不存在（45000000）；同 call_id 二次回传静默忽略——
    豆包全双工**无法强制模型开口念注入内容**，故结果送达以消息卡为主通道，
    注入仅尽力而为。
- **代价**：Hermes 处理耗时（实测 18~31s）期间对话只能口头说"已转交"；窗内
  兜底在多挂起并发时可能配错（单设备一次一问，实际风险低）。
- **放弃**：等结果再回 tool 结果（卡死对话，用户明确否决）；Hermes 直连 API
  （没有，飞书是唯一通道）；时间窗替代引用配对（两者互补，不替代）。

## ADR-037：ColorOS"完全允许后台行为"无法检测也无法直达——进 App 强制引导 + 被杀记录间接判断

- **日期**：2026-09-24（PKH110，ColorOS 16.1.0 / Android 16 真机实测）
- **背景**：ADR-024 ④的电池白名单是 AOSP 层豁免，而 ColorOS 的"设置→电池→
  应用耗电管理→完全允许后台行为"是 powerkeeper 私有管控，两套系统互不等价。
  用户实测：只开原生白名单照样被杀，手动开 ColorOS 开关后才活。用户要求
  App 进门自动检测、缺了就强制引导，不该让用户自己想起来去翻设置。
- **真机实测的三堵墙**：① 没有公开 API 能读"完全允许后台行为"开关状态；
  ② `com.oplus.battery` 所有耗电页 intent 均需签名级权限
  （`com.oplus.permission.safe.SETTINGS` / `oplus.permission.OPLUS_COMPONENT_SAFE`），
  `am start` 全部 SecurityException（ColorOS 16 收紧，老版本可直达的 action 全灭）；
  ③ 目标开关页 `PowerControlActivity` 未导出（uid 1000 专属），连应用详情页的
  "耗电管理"入口都是设置 App 代跳的。
- **选择**（`app/` v0.1.7，`KeepAlivePermission`）：
  - 跳转走公开 API：`ACTION_APPLICATION_DETAILS_SETTINGS` 应用详情页
    （必达）→ 文字指引用户点「耗电管理」→「完全允许后台行为」；
  - "检测"间接化：`ApplicationExitInfo`（API 30+）查最近退出是否系统杀
    （SIGNALED / LOW_MEMORY / EXCESSIVE_RESOURCE_USAGE / OTHER；用户主动杀、
    CRASH、ANR 排除），叠加用户点「已完成设置」的时间戳（`Prefs.batteryGuideDoneAt`）
    ——确认过就不再弹，除非确认之后又被杀（= 设置没生效/被改回去）；
  - 弹窗 `cancelable(false)`，只有「去设置」「已完成设置」两条出路（用户点名要强制）；
  - 老版 ColorOS 直达组件（com.coloros.safecenter 自启动管理等）保留为尽力尝试，
    resolve 成功就走，失败自动落回详情页；
  - 非 OPPO 系 ROM（OPPO/OnePlus/realme 之外）不弹此框，只走原生白名单按钮。
- **代价**：读不到真实开关状态，「已完成设置」只能信用户；新装首次进门必弹一次
  确认（哪怕已手动设置过）；引导比老版直达多一步（详情页→耗电管理）。
- **放弃**：直达 ColorOS 耗电页（签名权限封死，写了也是 SecurityException）；
  读 powerkeeper ContentProvider / 私有 settings（无公开 schema，版本私有随时碎）；
  在 onResume 触发弹窗（跳设置回来会无限重弹，只认冷启动 onCreate）。
- **修订（v0.1.8，同日）**：① 触发条件去掉"原生白名单未开"——ColorOS 开关
  （「完全允许后台行为」）开启时系统会联动加 AOSP 白名单（真机
  `deviceidle whitelist` 可见 `user,com.gagaai.app`），反之该条件会覆盖用户
  「已完成设置」的记忆，导致每次进门都弹，违背"只在 onboarding 弹一次"的初衷；
  现在只看「从未确认」和「确认后又被系统杀」。② 另发现应用数据被清除
  （CDM 记录 `pkg-data-cleared`）会把确认标记和配对关联一起抹掉，属数据层
  固有行为，弹一次 onboarding 重新确认是可接受的兜底。
- **修订（v0.1.8 终版，同日）**：① 弹窗触发收敛为唯一条件「从未点过已完成设置」
  （用户要求只 onboarding 一次）；确认后被系统杀（实测 ColorOS o-kill，
  「完全允许后台行为」也不是绝对豁免）只写日志栏提示不弹窗。
  ② 连带修掉两个 crash：CDM 配对回调 `ClassCastException`——Android 13+ 把
  `EXTRA_DEVICE` 装成 `ScanResult`，按 `BluetoothDevice` 强转在"选完设备"瞬间
  闪退（用户实测命中）；权限重置（清数据/重装）后看门狗后台拉 `connectedDevice`
  前台服务抛 `SecurityException` 循环崩溃——`startForeground` 捕获后静默退出。
  ③ 数据被清（CDM 记录 `pkg-data-cleared`）会同时抹掉确认标记与配对关联，
  重新 onboarding 一次是可接受兜底。

## ADR-025 修订：配对流程自动化 + CDM 过滤器（2026-09-24，v0.1.9）

- **空 filter 是"配对框搜不到嘎嘎"的第二根源**：`AssociationRequest.Builder().build()`
  不带过滤器时，系统选择框只列系统已配对的经典蓝牙设备、**不做 BLE 实时扫描**，
  未配对的嘎嘎永远不会出现。改为 `addDeviceFilter(BluetoothDeviceFilter.setNamePattern
  ("GAGA-.*"))`（注意：Builder 只有 `setNamePattern`，正则用 `"GAGA-.*"` 兼容
  matches/find 两种语义；API 31+，低版本维持空 filter）。
- **配对自动化**：嘎嘎连着 BLE 时不广播，选择框扫不到——现在点「配对嘎嘎」
  自动完成整套动作：撤看门狗（15 分钟闹钟若在配对窗口到点会把服务拉回来、
  BLE 又连上、嘎嘎又不广播）→ stopService 断开 BLE → 1.5s 后弹选择框 →
  配对结束（成功/取消/失败）自动恢复服务并重连。不再需要用户手动
  "先停止服务再配对"。
- **连接成功自动记 MAC**（补齐 ADR-024 直连通道宣称但缺失的一环）：链路就绪
  （CCCD 写入成功）时写入 `Prefs.pairedMac`，MAC 直连不再依赖 CDM 配对；
  CDM 配对的独立价值收窄为系统级保活（设备出现时系统拉活服务 + 进程保级）。

## ADR-025 修订②：显式开启"设备出现"观察（2026-09-24，v0.2.0）

- **坑**：API 33+ 起系统不再"配对即观察"——必须 App 显式调
  `CompanionDeviceManager.startObservingDevicePresence(associationId)`，
  否则即使 association 已注册，系统也不会在设备出现时绑定
  CompanionDeviceService（真机 dumpsys 验证：Bound 列表里只有手表没有嘎嘎）。
- **修复**：MainActivity.onCreate 里对全部 `myAssociations` 逐一开启观察
  （幂等，跨重启持久）。注意 API 重载陷阱：`getAssociations()`（List<String>，
  旧）与 `getMyAssociations()`（List<AssociationInfo>，新）同名，Kotlin 的
  `associations` 属性会解析到旧版，必须显式调 `myAssociations`。
- **触发语义**：观察靠 BLE 广播发现设备——嘎嘎"连着不广播"时不会触发，
  断开/重启恢复广播的瞬间触发 `onDeviceAppeared` → 拉起转发服务 + 绑定期
  进程保级。`userStopped=true`（用户明确停止）时伴生通道同样让位。
- 另：同一设备重复 associate 会产生多条关联（mId=4/5 并存，无去重），
  拉活动作幂等故无实害。

## ADR-037：录音原始 Opus 存档——识别定责的"黑匣子"

- **日期**：2026-09-24
- **背景**：实时识别偶发出错（如"P9 博物馆"→"PIO 博物馆"），无法区分是设备
  麦克风问题还是流式识别问题——录音在链路里"用完即焚"（流式边转边丢、批处理
  临时文件解完即删），回听定责无从谈起。
- **选择**：`_flush` 时把整段原始 Opus 包（无重编码）经 pack_opus_ogg 打包成
  `.ogg` 落盘到 `RECORDING_ARCHIVE_DIR`（默认 `data/recordings`，空 = 关闭）；
  识别成功后文件名自动追加转写摘要（前 24 字符，非法字符清理）。存档路径随
  会话对象走（挂 stream / 随批处理闭包），两段近距录音不串档；识别失败保留
  原名（无文本可加）。
- **理由**：存的是设备发来的原始码流，播放即"用户实际说了什么"的客观证据；
  打包复用现有 pack_opus_ogg（零新依赖、几行代码）；Ogg Opus 任何播放器可放。
- **放弃**：存 PCM/WAV（体积 ×32，无信息增益）；自动清理策略（磁盘满了再手动
  清，不加隐式删除逻辑）；talk 会话音频存档（实时对话音频是模型回复，不是
  用户的语音输入，定责场景用不到）。
- **备注**：定责方法——回听 .ogg：声音清晰 → 锅在识别模型（可试 2.0 资源档位
  或后续接热词）；声音发糊 → 锅在设备端（麦克风/风噪/增益）。

## ADR-024/025 修订③：手机重启实测——两条自动通道全哑 + 观察重注册三时机（2026-09-24，v0.2.1）

- **实测**：用户重启手机后（未打开任何 App），转发服务**没有被拉起**——进程死、
  服务无、通知无。①开机自启：BootReceiver 未生效，判定为 ColorOS 自启动管控
  默认禁止（独立于耗电管理的开关，应用详情页无此入口，只能引导用户去
  设置→应用→应用启动管理 手动允许，已加进保活引导弹窗文案）；
  ②伴生观察：`startObservingDevicePresence` 只在 MainActivity.onCreate 注册，
  重启后 App 从未运行 → 观察无人注册 → 系统不在嘎嘎广播时绑定拉活。
- **修复**：观察重注册提取为 `KeepAlive.observeCompanionPresence()`，在进程
  三个短暂复活时机各打一遍：BOOT_COMPLETED（开机广播）、看门狗闹钟（15 分钟
  一跳，广播 receiver 复活）、打开 App。幂等。这样即使 FGS 启动被管控拦下，
  观察也已注册——嘎嘎广播时系统有资格主动拉活（系统绑定不受后台启动限制）。
- **待验证**：用户再次重启手机实测。若仍不拉活 → ColorOS 连 BOOT_COMPLETED
  都未放行，唯一出路是用户手动允许自启动（伴生观察在开机广播复活时同样能
  注册上，两道修复互相独立）。


## ADR-038：手机 MQTT 链路状态推送（App→设备本地信令 link）——按键预检拦下"对着空气说话"

- **日期**：2026-09-25
- **背景**：用户实测（9/24"40 秒录音消失"）暴露盲区：BLE 未连有"噗噗"提示，但
  **BLE 通、手机 MQTT 断**（ColorOS 冻结后自愈中/VPN 掉/服务器不可达）时设备一无所知，
  照常"滴"后录音，说完全部石沉大海。ESP32 无法自行感知 MQTT 状态——App 是唯一
  同时看得见 BLE 和 MQTT 两端的角色，必须由 App 推送。
- **选择**：
  - **信令**：`{"type":"link","mqtt":true|false}`，App 本地信令经 BLE 直发设备
    （不经服务器）。App 侧唯一自组帧的点（[0x02][len][payload]，格式与服务器
    encode_frame 严格一致），哑管道纪律不破——只组这一种本地帧，仍不解析转发内容。
  - **推送时机**：①BLE 每次就绪（服务发现+订阅完成）先推一次当前状态；
    ②MQTT 连/断状态变化即刻推。BLE 未就绪时静默跳过（就绪后会补推）。
  - **设备侧**：AppState.mqttLink（默认 true 乐观——App 冻结时 BLE 也断，走原有
    蓝牙预检；只有"App 活着但网络不通"才显式推 false）；状态变化时屏显
    "手机没连上服务器"；录音（Recorder::start）与 talk（右上长按）在蓝牙预检后
    增加第二道预检：false → 提示 + 错误音 + 拒绝启动。
- **理由**：用户要求"有问题马上听到"，而不是说完 40 秒才发现石沉大海。默认乐观
  避免误报（BLE 刚连、link 信令还没到的窗口期不拦人）；两道预检 + 变化即推 =
  任何问题在按键瞬间就有声音和文字反馈。
- **代价**：App 需升级（组帧 + 推送逻辑，v0.1.7）；MQTT 抖动期反复推送在设备端
  只在状态变化时提示（去重），无刷屏。
- **备注**：录音进行中 MQTT 断开属残留盲区（说到一半断，帧被 App 丢弃）——
  App 可在录音期推 link=false 让设备提前收尾，本期不做，先观察实际发生频率。

## ADR-039：MQTT 同 ID 乒乓互踢——MqttManager 单例化 + start 幂等（2026-09-24，v0.2.2）

- **症状**：broker 端同一 Client ID（app-<ANDROID_ID>）每秒 1~3 次
  `session taken over` 互踢，App"一直开着"期间出现；清理后台重开即恢复。
- **诊断**：单安装（pm list packages 排除双 App）、单创建点（MqttClient.builder
  全仓唯一）、互踢时段进程活着（17:36:41 才被 o-kill）。真机 ss 显示健康态仅
  1 条连接。结论：乒乓期间服务被 ColorOS 快速反复重建（停服务 + START_STICKY
  拉扯），**每个新 Service 实例都 new 一个 MqttManager → build 一个新 HiveMQ
  客户端**连向同一 ID；旧实例的 DISCONNECT 包在 VPN 隧道抖动下可能送不干净
  （retire 最多等 3s 放弃），多实例并发连接即互相 taken over；autoReconnect
  默认 1s 起步放大烈度。清理后台 = 进程重建清零实例，故"重开就好"。
- **修复**（结构级，非打补丁）：`MqttManager` 改进程级 object 单例——服务重建
  不再新建客户端；`start()` 幂等（地址未变且客户端存活 → 直接复用现有连接）；
  自动重连退避 1s→3s 起步。ADR-038 的 onStateChanged 钩子保留为可重设字段。
- **代价**：单例状态（clientId/onDownlink/onStateChanged）由服务每次 onCreate
  重设，依赖"服务 onCreate 先于使用"的生命周期纪律；跨进程（未来多进程化）
  需重新设计。

## ADR-040：官网以零依赖纯静态单页起步（website/，2026-09-25）

- **背景**：需要一个类似 openclaw.ai 的对外展示站。本期只做纯展示内容，
  文档/教程/社区为占位空态（`#soon`）。站点按 `.agents/skills/` 新收入的
  Jakub Krehel interfaces skill 合集（better-ui/typography/colors/layout/
  writing/accessibility）的标准构建。
- **决策**：`website/` 单页——手写 `index.html` + `style.css` + 约 40 行原生
  JS + SVG favicon；零框架、零构建、零 CDN 外链。视觉语言与设备固件
  深空黑 + 鸭黄主题（ADR-031）同源；全部事实性内容（规格/进度/按键分工）
  取自 `docs/`，核对锚点写进 `website/README.md`。
- **理由**：① 展示站要的是"随时能改、永不腐坏"——无构建链意味着任何
  agent 或人都能 30 秒内改完并预览（`python3 -m http.server`），不会像
  npm 工程那样一年后装不动依赖；② 网站直接穿设备的主题，是产品 UI 的
  延伸，不另起第二套设计系统；③ 色板 oklch 语义 token 化且经 WCAG
  对比度实测（正文 16.2:1 / 次文字 9.2:1 / 鸭黄按钮 12.3:1），将来换
  框架或接文档生成器，token 可整体平移。
- **代价**：无组件化/模板，页面变多后 header/footer 会重复——届时引入
  静态生成器（MkDocs/VitePress），以本页 token 为设计基线迁移；`#soon`
  占位内容靠人工与 `docs/` 同步，铁律同前（改 docs 被引用内容必须同
  提交改网站）。
- **备注**：动效为 fail-open 结构——JS 只给"首屏之下"的元素挂
  `reveal-pending`，滚动扫描显形；无 JS、减少动效偏好或任何脚本异常时
  内容完整可读。已验证 1440px 与 320px 无横向溢出、锚点瞬跳不丢内容。

## ADR-040 修订①：i18n 用单文件字典切换，站点默认英语（2026-09-25）

- **背景**：网站要默认英语、可切中文（后续可扩更多语言），仍须守住零依赖零构建。
- **决策**：`index.html` 静态内容即英文；`assets/i18n.js` 内置中英字典
  （`data-i18n` / `data-i18n-aria` 标注），EN / 中文 分段切换钮放导航右侧；
  优先级 `?lang=` > `localStorage` > 英语；切换同步 `<html lang>` 与 `title`，
  并把语言写进 URL 便于分享。
- **理由**：① 相比 en/zh 两份 HTML：单文件保证结构永远一致，不会改版只改一份；
  ② 相比多语言静态站生成器：不引构建链（ADR-040 主决策不破）；③ 默认英语
  静态写死，无 JS / 爬虫 / 首屏都不依赖 JS，字典只在切换时生效——flash 仅
  存在于"已存中文偏好"的二次访问首帧，可接受。
- **代价**：中英文案双处维护（静态 HTML + 字典），`website/README.md` 已写明
  双写规则；文案含标签的元素必须走 innerHTML，字典属第一方可信内容。
- **备注**：加新语言三步：DICT 加对象、LANGS 登记、加切换钮。已验证：默认英、
  切中、刷新持久、`?lang=en` 覆盖、`<html lang>`/title 同步、320px 移动端布局。


## ADR-041：Arduino 线（esp32/）退役删除——IDF 线独挑大梁

- **日期**：2026-09-25
- **背景**：ADR-020 设立双线并行（Arduino 保底 + IDF 主力）。IDF 线已全面接管
  （M1~M4 真机回归、M5/设置系统/消息卡全在 IDF 线），Arduino 线自 09-23 起冻结
  无改动；其 385MB 目录里 353MB 是 .pio 构建缓存。用户拍板删除。
- **选择**：
  - 删除 `esp32/` 整目录（含 .pio 缓存）；
  - **先保两样再删**：① `esp32/backup/factory_backup.bin`（32MB 原厂固件，
    不可再生）→ 移至根 `backup/esp32-factory_backup.bin`；② Arduino 线全部
    源码（src/variants/platformio.ini 等）→ 打包 `reference/esp32-arduino-line.tar.gz`
    （49K，仓库非 git，这是唯一保险；确认不要时可删）；
  - `esp32/reference/`（微雪原理图/官方示例快照）→ 用户已移至根 `reference/`。
- **理由**：双线维护成本（文档同步/心智负担）大于保底价值——真要回退，tarball
  里有完整可构建源码；芯片参考资料独立成根 `reference/` 是常见仓库实践
  （与 docs/ 分离：docs 是"我们写的"，reference 是"别人写的"）。
- **放弃**：保留双线（维护成本无回报）；把原厂固件备份塞进 reference/（混淆
  "参考资料"与"设备镜像"两类资产）；不打包直接删源码（非 git 仓库 = 永久丢失）。
- **备注**：文档同步完成（AGENTS.md 仓库地图与宪法、README、hardware.md 路径
  三处）；同日按用户指示，roadmap 的 Arduino M3 段与 decisions 内纯 Arduino 条目（010/011/017）一并移除。

## ADR-041 修订①：两个保险箱一并删除（2026-09-25）

- `factory_backup.bin`：用户确认**不是原厂固件**（是前手刷过的未知系统，"原厂备份"
  系当初误判），无保留价值，删除。如日后需要原厂镜像，向微雪官方索取。
- `esp32-arduino-line.tar.gz`：用户确认 Arduino 线完全不需要，删除。
- `backup/` 目录随之移除；`reference/`（芯片参考资料）保留。

## ADR-042：消息模式 ASR 豆包单向 → 千问 message（定稿润色）

- **日期**：2026-09-25
- **背景**：火山单向流式（ADR-033）出的是裸转写，无标点润色无纠错——用户实测
  错字频出（"Today 日记"识别成"特带日记"、人名常错）。录音存档（ADR-037）回听
  证实麦克风拾音清楚，锅在识别模型。用户选定阿里云百炼
  `qwen-audio-3.1-asr-flash-message`：最终结果（sentence_end=true）经生成式
  后处理——标点、文本归一化（"4乘4"→"4×4"）、去语气词/润色
  （disfluency_removal_enabled）、结合上下文纠错。
- **实测**（scripts/asr_qwen_probe.py，存档录音回放真实链路）：
  松手→定稿 0.21~0.56s，与火山单向（0.39s）相当或更快——finish-task 强制收尾，
  不用等 VAD 静音窗（手册宣称的"最后等待"实测不成立）；"特带日记"→"Today 日记"
  （上下文纠错）、《影视飓风》自动书名号、空录音干净返回空串。
- **选择**：
  - 原生 WebSocket 实现（6 个事件：run-task/task-started/二进制音频/
    result-generated/finish-task/task-finished），**不引 dashscope SDK**
    （与 volc_stream 同构、人类易读，同接口可互换）；
  - 上行 PCM 16k（千问仅支持 16k；opus 要 Ogg 页边界没必要），Opus 解码用
    提升为公共模块的 `opus_codec.py`（原在 realtime/ 下，step/gemini/qwen 三家共用）；
  - 失败自动降级本地 whisper 批处理（原有机制不变）；
  - API Key：`DASHSCOPE_API_KEY` env → 回落 `~/.bailian/config.json`（bl CLI
    登录产物，key 不进仓库）；WS 地址从其 base_url 推导（业务空间专属域名）。
- **放弃**：dashscope SDK（黑盒线程/超时行为 + 新依赖）；火山 opus 流式格式
  （PCM 最稳）；热词/context（手册支持，等真实需求再开——人名错字可望靠它根治）。
- **备注**：豆包单向保留为可切选项（POST /debug/providers 或改 ASR_PROVIDER），
  未见得删；实时对话（talk）链路不经过此 provider，豆包 realtime 自带 ASR 不变。

## ADR-043：provider 统一登记处（providers.py）+ /debug/providers

- **日期**：2026-09-25
- **背景**：服务端可插拔能力已有三类，选择机制各搞各的：`CHANNEL` 选接入端、
  `ASR_PROVIDER` 选 ASR、`REALTIME_PROVIDER` 选实时对话（另有设备信令覆盖）。
  散在装配代码里，"我什么时候用什么"没有单一答案处，运行时也没法切。
- **选择**：`providers.py` 一张登记表 + 一条优先级规则：
  **设备信令（talk 场景） > HTTP 运行时切换 > 环境变量**。
  - `GET /debug/providers`：三类能力当前用哪个、来源、候选清单（带中文说明）；
  - `POST /debug/providers {"capability":"asr","provider":"qwen"}`：运行时切换，
    即时生效于下一段录音/talk（不打断进行中的会话）；provider 传 null = 清除
    覆盖回落环境变量；
  - 语义差异明示在返回体 note 里：asr 下一段录音生效（设备无切换入口）；
    realtime 设备显式指定时以设备为准（选择权在小设备，ADR-035 不动摇）；
    channel 常驻轮询，改后需重启。
- **理由**：一个模块一个规则，新增 provider = 实现类 + 登记表加一行；调试期
  免重启换引擎对比效果（本次 ADR-042 的 A/B 就是这么测的）。
- **放弃**：把 asr 切换也做进设备信令（设备上没有消息模式的设置入口，先观察
  需求）；配置文件方案（环境变量已覆盖，不引新机制）。

## ADR-044：MQTT 重连收回自管——拿掉 automaticReconnect（v0.4.2）

- **日期**：2026-09-25
- **背景**：ADR-039（MqttManager 单例化）之后乒乓仍复发。服务端日志给出铁证：
  `session taken over` 互踢间隔精确为 3 秒 = automaticReconnect 的 initialDelay，
  即手机进程内同时存在两个同 Client ID 的活性连接——一个是 retire() 断开失败
  后"复活"的旧客户端。链条：锁屏时段 ColorOS 周期性掐网 → 服务重建触发
  stop()/start() → retire 的 disconnect 最多等 3s 就放弃 → 旧 client 身上的
  automaticReconnect 没有被摘除（HiveMQ 无"事后拆除"API）→ 网络恢复后旧
  client 自己重连，与新 client 同 ID 互踢，双方 connect 都"成功"导致退避永远
  重置回 3s，永不收敛。开得越久、锁屏次数越多，僵尸诞生概率越接近必然。
- **选择**：**不用 HiveMQ automaticReconnect**，重连事务收回 MqttManager 自管：
  - 每个 client 配一条 `mqtt-supervisor` 守护线程：connect（30s 封顶）→ 订阅
    gaga/down → 轮询等断线 → 指数退避（3s 起、2min 封顶）后对**同一个 client
    对象**重新 connect；
  - 被 generation 作废的 client 没有任何自动重连路径：retire 断开失败也只是
    占着一条旧连接，同 ID 下次 connect 时被 broker 以 session taken over 踢掉
    （踢的只能是僵尸，方向永远是"活者踢僵尸"）；
  - 下行监听从"每次 subscribe 带 callback"改为 `publishes(ALL)` 全局回调、
    每 client 只注册一次——supervisor 每轮重连重新 subscribe 时不再叠加回调
    （v0.3 踩过的重复下行坑从结构上封死，此前只靠"只在首次连接后订阅"规避）；
  - retire 失败写一行 terminal 日志（cleanup failed (harmless)），便于现场识别。
- **放弃**：
  - 每实例唯一 Client ID（`app-xxx-<gen>`）：同样能让僵尸踢不到活者，但僵尸
    会在 broker 上累积连接/订阅，且改协议文档里的 ID 约定——治标；
  - 保留 automaticReconnect + retire 时反复重试 disconnect 直到成功：依赖
    "disconnect 成功才拆除重连"的时序，掐网窗口内保证不了——治标；
  - unique ID + 自动重连兜底双保险：两条重连机制并存反而更难推理。
- **备注**：与 ADR-039 的关系——039 解决"多实例并发"，044 解决"僵尸复活"，
  两条合起来才是完整的乒乓根治。ColorOS 后台周期性断网本身是 ROM 行为不受
  本 ADR 管（断网造成的断连由 supervisor 正常退避重连，属预期自愈）。

## ADR-045：录音交互点对点化——按住说话退役，右下单击/摇动同权开合

- **日期**：2026-09-26
- **背景**：按住说话（ADR-016 v2：长按/按住右下 → 说话 → 松开发送）在真实
  挂脖场景暴露结构性问题——用户松开手后要去骑车/吃东西/干活，"必须一直按住"
  意味着录音期间双手被绑死；而收尾手势（摇动）又是另一条独立入口。三条线
  （按钮按住、摇动、轻叩）各自只能"开"或只能"关"，用户要求点对点：任何一种
  方式都能开始，任何一种方式都能结束。同日真机实锤自动转向卡死：录音中横持
  设备（az≈0）微微摆动，面内重力分量在 0 附近反复过零，静态兜底判向
  `ax<0→180` 跟着反复翻转 → 全屏重绘 + 外沿光效连环触发，与录音 UI 抢渲染
  直到卡死。
- **选择**：
  - **交互**：右下单击 = 唯一按钮入口，toggle 开合（空闲开录 / 录音中收尾 /
    300ms 确认窗内再点=静默撤销，rec_start 未发零信令）；摇动保持 toggle
    （ADR-040）不变——按钮与摇动同权，任一可开任一可关。轻叩（双击）引擎
    未就绪，留待接入时自然获得同权（同为 toggle 入口）。单击落定要等 300ms
    双击窗，录音启动相应延迟，用户拍板接受；急促双拍被双击窗整体吞掉
    （未注册双击回调），天然防误触。
  - **转向防抖三件套**：①录音中不判向不翻转（录音界面自有动画）；②静止
    判向一轮只评一次（原过 400ms 后每 10ms 连评）；③兜底判向加死区
    （面内主分量 |s|<0.35g 保持现状）+ 3s 翻转冷却。
- **理由**：①按住说话的当初优势（起止边界绝对明确、挡风罩）在挂脖形态下
  不敌"双手解放"；确认窗撤销 + 双击吞拍已覆盖误触面。②点对点让手势与状态
  解耦：用户不需要记"哪种方式开的要用哪种方式关"。③转向卡死的根因不是
  判向算法本身而是"判向结果无阻尼地直达重渲染"——在入口处限流（录音禁翻 +
  死区 + 冷却）比在渲染端做队列更便宜且不引新机制。
- **放弃**：
  - 右下双击 = 开合（与单击 toggle 语义冲突：双击=两次 toggle=没变，用户
    演示时"双击一下没反应"会被当成故障；保持双击无动作更诚实）；
  - 转向卡死在 Ui::applyRotation 里加动画排队/合并：治标，判向源头仍每
    10ms 连评刷日志；
  - 录音中也允许转向（仅靠死区挡振荡）：录音界面翻转没有用户价值，纯风险。
- **备注**：按住说话的松手宽限/锁定模式代码保留（串口 `w`/`l` 可实验），
  主路径不再触达；Recorder::toggle 吸收了原 releaseUp 的"确认窗内撤销"
  语义。陀螺仪积分判向的符号→方向映射仍未真机标定（拿一次正的看 [ROT]
  日志即可定死），本次防抖不改变该待办。

### ADR-045 v1 废弃、v2 定稿（2026-09-26 当天迭代）

- **背景**：v1（单击开合+双击窗防误触）真机一用即废——单击落定要等 300ms
  双击窗，mic 从"落定"才开始采，用户按下到开录 ~0.45s，**边点边说的开头
  1~2 个字被吞**。用户明确否决："不允许丢任何内容"。
- **v2 定稿**：**按下沿立即开录**（40ms 电气防抖是唯一延迟，开头零丢失）；
  松手不结束（点一下开始、说完再点一下结束，录音期间手可以离开）；再按
  一下时收尾判定——**录制时长 <300ms（rec_start 未发，泵只缓冲没上行）→
  静默丢弃零信令**。急促双击 = 开了马上关 = 净效果什么都没发生，用录制
  时长判误触，不需要任何前置等待窗。
- **理由**：①"双击窗防误触"和"开录零延迟"本来可以用同一个 300ms 表达，
  v1 把它花在了错误的位置（录音开始前），v2 把它移到收尾侧（录制时长），
  两全；②按下即录沿用按住说话时代验证过的"按下沿开录"路径（ADR-026 按
  下起零丢失），风险最小；③确认窗撤销语义原样保留，只是触发从"松手"变
  成"第二下按下"。
- **代价**：单次误触（点一下就走、没有第二下）会让录音持续到用户注意到
  再点结束——红点+计时可见，用户拍板接受；按钮不再注册单击/双击回调，
  ButtonHandler 的双击窗对右下键空转（无害）。
- **清理**：松手宽限（'w'）、锁定模式（'l'）连同 releaseUp/pressDown 旧
  路径整体删除（v1 曾留作串口调试，v2 一并退役——半退役代码就是 confusion
  源）；Recorder 对外只剩 toggle()/tick()。

## ADR-046：BLE 广播看门狗——广播静默即强制重启

- **日期**：2026-09-26 晚
- **背景**：真机串口全程日志实锤一类"设备 BLE 隐身"故障：18:25 一次正常断连后
  广播恢复；18:36 一次链路握手失败（reason 0x3e，连接建立失败）触发 NimBLE
  内部 "Reattempt advertising"，重试失败 rc=3——**此后广播永久静默**。设备
  应用层一切正常（屏幕/按键/IMU 活着），但对所有手机隐身：App 直连反复
  失败（status=147）、系统配对扫描搜不到，只能重启设备救回。既有自愈
  （断连回调里 startAdvertising）救不了这条路：没有断连事件会来。这解释了
  长期以来"嘎嘎重启后 APK 必须折腾才连得上"的一族症状。
- **选择**：appTask 周期自查（10s 节流）：`未连接 && !ble_gap_adv_active()`
  → `ble_gap_adv_stop()` 强停清状态 + 重新 `startAdvertising()`，并打
  `[advwdt]` 警告日志。`ble_gap_adv_*` 走 host 内部锁，app 任务调用安全；
  startAdvertising 入口加 `connected_` 守卫防看门狗与连接建立的竞态。
- **理由**：①故障态可被唯一且无歧义地检测（不在广播=不健康，正常态要么
  连着要么广播着）；②在设备侧自愈是唯一能覆盖所有触发路径（NimBLE 内部
  重试失败/host 异常/未来未知路径）的位置；③成本一行自查。
- **放弃**：只修 rc=3 的重试逻辑（得改 NimBLE 组件源码，升级即丢）；
  在 App 侧检测"设备长时间扫不到"提示用户重启（治标，还把锅甩给用户）。
- **备注**：同晚 APK v0.4.4：终端日志区长按复制整段（排障搬运通道）；
  之前 v0.4.3 已有 BLE 写队列看门狗（App 侧）——两端各自守门，故障面收窄
  到"任一端 10s 内自愈"。

## ADR-047：飞书会话圈——只有"回复嘎嘎"的回流才进设备

- **日期**：2026-09-26 晚
- **背景**：用户报"群里不属于嘎嘎的消息也被推给嘎嘎并响一声"。根因：入向
  路由只过滤发送者（FEISHU_REPLY_SENDER=Hermes），不校验"回复的是谁"——
  Hermes 在群里回别人的话、主动发言一律转发，设备端 FIFO 兜底还把它错贴
  到最旧的等待卡上。这正是用户上一轮 reply 路由重构规划的第一步。
- **选择**：FeishuChannel 给 send_text 发出的每条消息 id 记账（2h/200 条
  滚动裁剪；语音上行和 talk 工具指令都算——都是嘎嘎发起的）；路由层
  （main._on_channel_message）在工具回流判定**之后**加闸门：parent/root
  都不命中账本 → 丢弃（打日志）。reply_to 取"确属嘎嘎"的那个 id（深层
  回复链 parent 是 Hermes 中间消息时用 root，设备才认得）。
- **理由**：①会话圈判据唯一且可靠（自己发过什么自己最清楚，不依赖对方
  行为）；②闸门放路由层而非 feishu.py：talk 工具回流的"无引用时间窗
  兜底"（ADR-036）必须豁免，放接入端会把工具结果饿死；③丢弃带日志，
  "Hermes 忘带回复标签导致回复被丢"可现场观察，为后续"强制 AI 末尾带
  ID"方案铺路（服务器届时从文本尾部解析 id 等同 parent）。
- **代价（明示）**：Hermes 不带引用的直接回复从此到不了设备（此前靠
  FIFO 兜底能点亮）。这是用户拍板的行为——补偿路径是把 ID 喂给 Hermes
  让它带上，规划中。

## ADR-048：MQTT 下行僵尸自愈——服务端心跳 + App 端判死重建

- **日期**：2026-09-26 深夜
- **背景**：真机复现并活体实验实锤的新故障形态：App 的 MQTT 连接"半死"——
  上行发布正常（设备对账查询每 10s 到达服务端）、订阅 SUBACK 过、broker
  侧向新订阅者正常投递，但 App 的 publishes 回调长期不被调用 → 设备收不到
  任何下行，App 状态全绿、零错误日志。出现在 keepalive 超时（App 后台被
  冻结发不出 PINGREQ → broker 90s 掐线）引发的数次重连之后；确切根因悬置
  在 HiveMQ 客户端内部，无法从外部进一步定位。
- **选择**：自愈不依赖根因——
  - 服务端：每 30s 发布一帧 hello_ack 心跳（复用对时信令；App 端
    FrameLogger 静默、设备端 envelope ts 静默校准，零 UI 噪音）；
  - App：downlink-watchdog 线程监视"已连接但 90s 无任何下行回调"，
    判死即整体重建 MQTT 客户端（stop+start 同地址）；订阅落定即为计时
    起点，重建后从零起判防连环误杀；
  - 可观测：下行到达但代际过期被丢弃时打日志（根因候选的头号路径，
    下次复现即可定罪）。
- **理由**：①参照流量是检测"静默半死"的唯一手段，心跳同时白赚设备持续
  对时与链路探活；②90s = 3 个心跳周期，健康连接不可能误判；③重建是
  幂等操作，误杀代价是一次重连。
- **代价（明示）**：服务端重启会丢对账内存（最近 receipt/reply），期间
  设备的 rec_status_query 得不到应答——已知边界，设备侧 60s 软超时文案
  兜底；用户重发新消息即恢复。

## ADR-049：离线录音——拆掉"手机没连上服务器"的录音硬门

- **日期**：2026-09-26 深夜
- **背景**：ColorOS 熄屏对后台 App 的连接扑杀无解（用户已开全部电池设置；
  实测连接存活 3~12 秒即被杀，重建连接也被拦数十秒），录音入口被
  mqttLink 硬门拦成"想说话说不出"——可穿戴设备的立身场景（掏出来就说）
  被手机系统的后台政策绑架。
- **选择**：
  - Recorder::start 不再看 mqttLink：BLE 连着就能录。每帧本来就在同步进
    PSRAM 离线缓存（8 分钟容量），链路恢复自动补发（rec_start + 帧序 +
    对账兜底），对着断链说完的话照样进群；
  - UplinkPump 加"本段见过断链"粘性标记（linkSawDown_）：帧被 App 丢弃
    时设备侧 framesDrop_ 不涨（BLE 层成功），只在 8s 超时时刻看 mqttLink
    会漏掉"断过又恢复"的窗口导致缓存被误清、音频永久丢失——录音期间
    采样断链一次即置位，超时判定改用它；
  - "手机没连上服务器"提示退役；BLE 断连门保留（连缓存都送不出去的
    场景才有拦的意义）。
- **理由**：缓存+补发链路早就在（ADR-025/对账），这道门是它在世时留下
  的过度防御；拆门让"录音"与"联网"解耦，ColorOS 的连接扑杀从"不能说话"
  降级为"晚几秒进群"。
- **代价**：离线说的话在链路恢复前卡片一直"识别中"（60s 后显示"识别超时，
  仍在等…"软文案）；补发与新录音互相让路（既有逻辑）。

### ADR-049 补遗：离线缓存分段补发（三合一 bug 修复，2026-09-26 深夜）

- **现象**：离线连说 3 条，恢复后群里只出现 1 条（三段音频被拼成一个
  会话、一次 ASR、一张卡），另外两张设备卡永远"识别超时"。
- **根因**：缓存是单一流（capAppend 只追加无边界），capReplay 只在开头
  发一次 rec_start、结尾一次 rec_stop——服务端视角=一个连续会话。
- **修复**：段间插 0xFFFF 分界标记（真帧长 ≤256B，不可能撞车）；补发按段
  各走 rec_start→帧→rec_stop；capDoneOff_ 记录已完成段尾作断点续传点，
  中途断链重试从这续（服务端无去重，重发整段=群里双消息）。
- **验证口径**：离线 N 条 → 恢复后群里 N 条独立消息 + N 张卡各亮各的。

## ADR-050：WiFi 高性能锁——熄屏 WiFi 休眠挂起是"今天熄屏就断"的根因

- **日期**：2026-09-26 深夜
- **背景**：用户对比实锤——昨天骑摩托车锁屏揣兜发一整天（移动数据 + 远程
  broker）稳如泰山；今天在家（WiFi + 调试 broker）熄屏数秒 MQTT 即死。
  adb dumpsys wifi 抓到直接证据：熄屏瞬间 `CMD_SET_SUSPEND_OPT_ENABLED
  screen=off`——ColorOS 把 WiFi 射频打入休眠挂起，后台长连接全灭
  （"Software caused connection abort"）；系统级 wifi_sleep_policy=NEVER
  是摆设，OEM 不认。移动数据路径无此机制，故昨天正常。
- **选择**：前台服务持有 `WifiLock(WIFI_MODE_FULL_HIGH_PERF)`——Android
  为"熄屏 + WiFi + 后台长连接"场景留的标准武器，阻止射频进休眠挂起。
  onDestroy 释放；WAKE_LOCK 权限清单已有。
- **理由**：设备侧离线录音（ADR-049）解决"熄屏说不出话"，WiFi 锁解决
  "熄屏发不出去/收不到"——两层防御把 OEM 网络政策的伤害面清到最小。
- **代价**：持锁期间 WiFi 射频不休眠，手机耗电略增（前台服务的正当开销，
  与导航/音乐类 App 同级）。

## ADR-051：手表形态立项——`watch/` 作为嘎嘎的"第二副身躯"（2026-09-26）

- **日期**：2026-09-26
- **背景**：产品判断三条——
  1. **手机不是嘎嘎的形态**：手机本身已是最高配 AI 入口（飞书 + 输入法 +
     屏幕键盘全都在）。任何"手机上的嘎嘎 App"都是在已有最短路径旁边修一条
     更长的路。手机唯一正当角色 = 哑管道（给穿戴设备中转）。
  2. **穿戴设备是真正的空地**：厂商手表的 AI（OPPO 小布、Apple Siri 之类）
     锁死在厂商生态里——进不了你的飞书群、走不了你的 Hermes、用不了你调的
     提示词。嘎嘎的本质 = 把"你自己的 AI 栈"接到任何穿戴形态上。这个楔子
     厂商助手永远给不了。
  3. **一个大脑，多副身躯**：手表（日常/走路，APK 天级迭代）+ 胸前硬件
     （骑车/干活时手不空，固件周级迭代）+ 手机（只做哑管道）共享同一颗
     服务端大脑、同一套帧协议、同一个飞书群——在哪副身躯上说话，对面都是
     同一个嘎嘎。
- **选择**：根目录新建 `watch/` **独立 Gradle 工程**（独立 applicationId
  `com.gagaai.watch`，与 `app/`、`server/`、`esp32-idf/` 平级），复用品牌
  资源、HiveMQ 依赖、ADR-044 的 MQTT 硬化模式，协议直连 `gaga/up` /
  `gaga/down`，服务端零改动。
  - 手表没有 BLE，本身就是一台"嘎嘎设备"（不是哑管道）：组帧、上行、
    信令状态机都在手表内；帧格式仍走 `docs/protocol.md` §2，发**单包完整帧**
    （MQTT 无 MTU 限制，不用 BLE 的 0x80 分片），服务端 FrameAssembler
    同时吃两种形态。
  - 交互**刻意不用按住说话**：点一下开始、再点一下结束发送。原因——胸前
    两段式按钮有"稍微松力就断"的物理手感问题（2026-09-25 真机反馈），手表
    屏小手滑，切换式最稳。硬件键（表冠/侧键/音量/相机键）与点按钮同权。
  - 音频走 Android `MediaCodec` Opus 编码（API 29+ 运行时探测）+ `AudioRecord`
    VOICE_RECOGNITION 音源 16kHz mono PCM16 / 20ms 一包，与 ESP32 端
    es7320+es8311 链路采样率一致；系统编码器帧粒度若真机不稳，再换 libopus
    JNI。
  - 交互形态对齐 ADR-045（点对点开关）而非按住说话，直接规避了 ADR-045 v1
    废弃时踩过的同款"松手误断"坑。
- **理由**：手表 APK 是**验证车 + 第二形态**——迭代天级、分发门槛低（用户
  已确认表冠/侧键可自定义、可换 APK），让"第二个人一周内用上嘎嘎"成为可能；
  同时它用的还是同一套协议，服务端和胸前硬件线一行都不用改。
- **代价**：ColorOS Watch / Wear OS 的后台政策不可控（息屏收割长连接，
  ADR-044 的 keepalive 15s 教训同样适用，MqttManager 直接沿用）；Opus 编码
  依赖系统 codec（API 29+），低版本只能提示不支持；手表端离线缓存/补发
  （ADR-049）尚未实现，断网说的话会丢（框架阶段先接受，后续再补）。

### ADR-051 补遗：v0.1.0 框架边界（2026-09-26）

**已就位**：MQTT 硬化客户端（ADR-044 三铁律：无 automaticReconnect / 代际
生命周期 / 下行监听单次注册 + 90s 僵尸自愈）、录音→Opus→组帧→上行闭环、
**rec_start / rec_stop{duration_ms} 信令时序**（服务端 `session.py` 靠它开/收
会话，缺任一服务端不知道何时收尾，见 docs/data-model.md §1）、
receipt/reply 下行显示、对账轮询（`rec_status_query` 10s 一次）、硬件按键
监听（`onKeyDown` + 后台 `MediaSession` 兜底）、圆屏 UI（点一下开录/再点
发送/长按配 broker）。

**本期不做**（下期顺位）：talk 实时对话（M5 协议已就绪）、下行 TTS 播放、
离线录音缓存（ADR-049 手表版）、消息卡历史、后台长连接保活加固、
真机联调（本版以 `assembleDebug` 编译通过 + 静态审阅交付）。

### ADR-051 修订①：从 `app/` 子模块迁出为独立工程 `watch/`（2026-09-26）

- **问题**：初版把 `:watch` 挂在 `app/` Gradle 工程下。`app/` 的宪法是
  "永远是哑管道，只转发不解析"（AGENTS.md 各组件的宪法），而手表是设备侧
  客户端——自己组帧、走信令状态机、编码音频。塞进 `app/` 会让那条铁律
  自相矛盾，也让"app/ = 手机哑管道"的心智模型失效。
- **选择**：`watch/` 迁到仓库根，成为**独立 Gradle 工程**（自带
  `settings.gradle.kts` / `gradlew` / `local.properties`），与 `app/`、
  `server/`、`esp32-idf/` 平级。`app/settings.gradle.kts` 移除 `:watch`。
  - **为什么不做成 `app/` 的兄弟模块共用一个 Gradle 构建**：仓库惯例是
    各组件独立构建（`server/` 走 uv、`esp32-idf/` 走 PlatformIO），手表
    跟随惯例；共用构建省下的只有 HiveMQ 版本号和 compileSdk 两个常量，
    代价是"谁拥有构建"的心智混乱——不划算。
  - **为什么不搬 Gradle 根到 `gaga-ai/`**：那是教科书式的多模块布局，但
    会打断 `cd app && ./gradlew` 这条所有文档/肌肉记忆里的路径，重构面
    大于收益。
- **验证**：`watch/ ./gradlew assembleDebug` ✓（产物
  `watch/build/outputs/apk/debug/gaga-watch-debug.apk`）；
  `app/ ./gradlew assembleDebug` ✓（不受影响）。
- **代价**：Gradle wrapper 与 `gradle.properties` 各复制一份（约 50KB）。
  两个工程 AGP 版本需人工保持一致（当前都是 9.3.0），`watch/build.gradle.kts`
  与 `app/build.gradle.kts` 的版本注释里都标了这一点。

## ADR-052：手表 v0.2.0——视图体系、鸭子声音语言移植、调试自证（2026-09-26）

- **日期**：2026-09-26
- **背景**：v0.1.0 框架跑通后用户报三个问题——① 打开软件不会自动录音；
  �� MQTT 配置只在首次弹窗，后续没有入口再改；③ 界面像"桌面图标"，
  鸭子没占据屏幕、没有录音中/发送中/接收中的状态提示。并给出 v0.2.0
  需求清单（消息卡首页 / 左滑嘎嘎页 / 顶部快捷面板 / 设置页 / 调试页 /
  三种提示音 / 晃动手腕录音 / 按键测试）。
- **选择**：
  - **5 视图体系**（圆屏 466×466，FrameLayout 叠放 + visibility 切换，无转场
    更省电）：① 消息卡首页（默认，状态条+卡片列表+底部状态胶囊）② 嘎嘎页
    （左滑，满屏自绘鸭子 + 大字状态）③ 快捷面板（顶部下滑，提示音开关 +
    设置/调试两入口）④ 设置页 ⑤ 调试页。
  - **"顶部下滑"的冲突裁决**：用户对"设置"和"调试"都提到顶部下滑。收成
    **一个快捷面板**（对齐 ESP32 ADR-035 控制中心）——一次手势两个门都到，
    也不会让一个手势同时去两个地方。
  - **broker 配置从启动弹窗改为设置页常规项**：弹窗只弹一次 = 用户报的
    "后续没地方改"的根源。未配置时首页顶部一行"未配置服务器 → 点此设置"。
  - **打开即录音**（可关）：App 起来就收声，500ms 后自动 toggle。
  - **按键 250ms 去抖**：用户要"双击右下键开录音"，但双击不该被算成两次
    toggle 把录音又关掉——单击双击都只算一次。
  - **晃动手腕录音**（可关，默认关）：加速度合矢量差分超阈值触发，800ms
    冷却防一甩两判。阈值调试页滑条可调（0.5~10.5，默认 3.0）。判定用
    加速度就够；**陀螺仪只给调试页看原始数据**——用户明确要确认表里到底
    读不读得到这两个传感器，调试页显示 `ACC✓/ACC✗` `GYR✓/GYR✗`。
- **鸭子声音语言原样移植**（`esp32-idf/src/audio/` → `watch/src/main/java/
  audio/Sounds.kt`，两端听感一致）：
  | 事件 | 声音 | 实现 |
  |---|---|---|
  | 打开设备 / 开始录音 / 发送完 | **嘎** | 真鸭叫采样 `snd_quack.c`（16kHz/4880 采样/305ms）→ 转 WAV `res/raw/snd_quack.wav`，SoundPool 播放 |
  | 收到回复 | **叮咚**（= 用户说的"钉钉音"） | 1318Hz 120ms → 988Hz 180ms，线性衰减包络（ESP32 AudioPipe.cpp case 3 同参数） |
  | 出错 | **噗噗** | 70~130Hz 锯齿随机游走 + 噗呲断续包络（ESP32 case 4 同参数） |
  合成音（叮咚/噗噗）没有素材，按公式现算——跟 ESP32 一样，所以两端同源。
- **状态四态**：IDLE 待机 → RECORDING 录音中（红）→ SENDING 发送中（橙）→
  WAITING 接收中（绿）→ IDLE。鸭子跟着变色（`DuckView.mood`），不看文字
  也能一眼看出它在干嘛。
- **理由**：设置/调试是"开发期工具"，用户要它们随时可及但不能挡正事——
  所以收进面板而不是占满一个手势；鸭子声音语言是 2026-09-25 用户定稿的
  （"声数=事件类别，闭眼数数"），手表照搬就不用重新建立肌肉记忆。
- **代价**：5 视图 + 手势让 MainActivity 涨到 ~600 行（v0.1.0 是 ~350）；
  调试页传感器 100ms 轮询**只在调试页可见时跑**，否则会让手表 CPU 一直被
  sensor 唤醒（用户对功耗极敏感，`ShakeDetector.stop()` 在 onPause 必调）。

### ADR-052 补遗：v0.2.0 边界

**已就位**：5 视图（卡片/嘎嘎/面板/设置/调试）+ 左滑/顶部下滑手势、打开即
录音、按键 250ms 去抖切换、晃动手腕录音（可关）、设置页（broker/设备 ID/
提示音/自动录音/晃动开关/清除配置）、调试页（按键实测显示 keyCode+映射归属、
加速度/陀螺仪实时值、晃动阈值可视化与滑条调节、日志、三种音效试听）、
鸭子声音语言（嘎/叮咚/噗噗）、消息卡列表（20 条环形，占位→receipt→reply
就地填充）。

**本期不做**：talk 实时对话、下行 TTS 播放、离线录音缓存（ADR-049 手表版）、
卡片详情页（点击进全文）、息屏时长控制、真机续航实测。

### ADR-052 修订①：v0.2.1 三个真机 bug（2026-09-26，用户实测报障）

用户装机实测立刻暴露三处，全是 v0.2.0 设计期没验到的：

**① 端口"消失"（设置保存后只剩 IP）**
- 现象：设置里填 `192.168.88.153:1883`，点保存后输入框只剩 `192.168.88.153`。
- 根因：`fillSettingsForm()` 里 `if (brokerPort == 1883) brokerHost else "$host:$port"`
  ——端口等于默认值 1883 时只回显 host。端口其实存着，但看起来像丢了，
  用户据此以为"没法用"。
- 修复：**永远显式回显 `host:port`**，不再有"端口等于默认就省略"的分支。
  另外保存后 toast 显式打出 `已保存 host:port`，用户能当场核对。

**② 控制中心被裁、按钮点不动**
- 现象：顶部下滑进面板后标题只显示一半、底部按钮被切、按键无反应。
- 根因：**屏幕实际是 466px @320dpi = 233×233dp**（设计时按像素估，差了一倍）。
  面板内容算下来 318dp，`gravity=center` 让内容上下各溢出 42dp——标题只剩
  半行、底部按钮被挤出可视区、可点区域落在圆屏外。v0.1.0 布局 211dp 所以
  侥幸没事。
- 修复：**全视图按 233dp 重排**——面板/卡片/设置/调试一律 ScrollView 兜底 +
  36/34/32dp 横向内边距（圆屏左右不裁字）+ 字号整体下调（16→13sp 按钮、
  13→11sp 正文）+ 行距收紧。`view_panel` 从 LinearLayout 改 ScrollView。

**③ "MQTT 未连接"刷屏**
- 现象：界面一直显示 MQTT 未连接。
- 根因两层：
  1. **日志刷屏**：打开即录音后每 20ms 发一帧，`publishUplink` 里那句
     `MQTT 未连接，丢 NB 上行` 每秒打 50 条，把用户淹死——看起来像"一直显示"。
     修复：降频到 3s 一条。
  2. **状态不可读**：状态条只有一个红/绿圆点，用户看不出是配置错还是网络不通。
     修复：改成文字 `MQTT ✓` / `MQTT ✗ host:port` / `MQTT ·`——连不上时把
     配置的地址打出来，一眼分清"写错了"还是"连不上"。
  3. **解析脆弱**：中文输入法的全角冒号"："会让 `split(":")` 整串当成 host。
     修复：`parseBroker()` 先把"："归一成":"，再按最后一个冒号切分，端口
     非法回落 1883。

**教训**：圆屏手表的 dp 尺寸必须先 `adb shell wm size/density` 量一遍再画布局，
不能按像素拍脑袋。已写进 `watch/README.md` 的"目标硬件"节。

### ADR-052 修订②：OPPO Watch X3 两颗物理键 = F1/F2，可以拦截（2026-09-26，v0.2.2）

- **实测结论**（调试页实录，用户按出来的）：导航键 = `KEYCODE_F1`，
  功能键 = `KEYCODE_F2`。**它们不是系统保留键**——事件确实送进了 App
  （调试页显示了 keyCode 就是证据），v0.2.1 只是白名单漏了它们。
- **v0.2.1 的行为**：白名单只有 `STEM_*`/`CAMERA`/`FOCUS`/`VOLUME_*`/`ENTER`
  那一套，F1/F2 落到 `return super.onKeyDown(...)`，事件交回系统 → 触发
  ColorOS 默认映射（F1=退出主页、F2=任务管理器）。用户看着就是"按了没用"。
- **关键结论：`onKeyDown` 返回 `true` 就能吃掉默认行为。** 对非保留键，
  Activity 拿到事件先于系统默认动作；返回 true = 拦截，返回 false = 交回。
  只有 HOME/POWER/BACK 这类 `PhoneWindowManager.interceptKeyBeforeDispatching`
  明确消费的键才拦不到。
- **v0.2.2 映射**：
  | 键 | 动作 | 默认行为 |
  |---|---|---|
  | `KEYCODE_F1`（导航键） | **返回**：非首页→回首页；首页不做事 | ~~退出主页~~ 已吃掉 |
  | `KEYCODE_F2`（功能键） | **切换录音**（250ms 去抖，单击双击只算一次） | ~~任务管理器~~ 已吃掉 |
  两颗键都 `return true` 拦截。顺带把 F3~F12 / `BUTTON_A/B` 也纳入录音键，
  防止下一颗键又漏。
- **兜底**：`dispatchKeyEvent` 也挂了探针（先于 `onKeyDown` 看见所有按键），
  调试页现在直接显示「→ 录音键 / 返回键 / 未映射」+「**已拦截** / 交回系统」——
  用户按一下就知道这颗键有没有被吃掉，不用猜。
- **代价**：F1 的默认"退出主页"没了（想退到表盘走表冠/系统 Home）。
  如果用户实际手感里两颗键的物理位置对调（F1 是右下），改一行即可换绑。

### ADR-052 修订③：v0.2.3 视觉返工——真鸭子图、卡片归位、按钮正圆（2026-09-26）

用户装机实测第二轮报障，三条都是视觉层的自作主张：

**① 鸭子图标："谁让你直接画了的？"**
- 现象：嘎嘎页是我程序化自绘的 `DuckView`（圆身+圆头+喙的几何拼贴），
  与品牌鸭子完全不像。
- 根因：我没去 `docs/assets/` 找现成资产，自己画了一个。
  **正确资产就在 `docs/assets/icons/gaga-icon.png`**（1254×1254 照片级鸭子，
  胸前戴 gaga 设备的吉祥物本尊），另有 `gaga-logo.png`（240×240 同一只）。
- 修复：`DuckView.kt` **整类删除**。`gaga-icon.png` 下采样到 560px 落
  `res/drawable-nodpi/img_duck.png`（343KB），启动图标按 mipmap 五档
  48/72/96/144/192px 重新生成（替换原鸭黄占位）。嘎嘎页底色取原图四角均值
  `#E6E3DE`，鸭子图无缝融进圆屏，整只可见不裁。
- **教训**：视觉资产优先用仓库现成的，`docs/assets/` 是权威来源；不许自绘
  吉祥物。

**② 主页卡片消失 + "圆形不对称还这么大"**
- 现象：首页看不到卡片，底部一个巨大椭圆。
- 根因：状态胶囊 `tvStatePill` 的背景是 `bg_duck_button.xml`——`<shape oval>`
  带 `<size width=120dp height=120dp>`。套在 `wrap_content` 的 TextView 上，
  oval 被拉成 **120dp 高的巨椭圆**（宽度又 match 横向，所以"不对称还大"），
  把 `ScrollView` 的卡片区挤得只剩一条缝。
- 修复：录音按钮改 **固定 44×44dp 的 `ImageView` + `bg_rec_button`**
  （oval **不带 `<size>`**，严格贴合 View 边界 = 正圆）+ 独立状态文字行。
  卡片 `ScrollView` 权重=1 占主体空间。
- **教训**：`<shape oval>` 的 `<size>` 会强制首选尺寸，套 wrap_content 会撑爆；
  正圆靠"正方形 View + 无 size oval"，不靠 size 硬指定。

**③ 卡片像乱码（"冒号识别中，嘎嘎冒号正在回复"）**
- 现象：卡片内容是一整串 `"你：xxx\n嘎嘎：yyy"` 的纯文本，视觉上就是
  一行带冒号的字，没有卡片感。
- 修复：`renderCards()` 重写成**真卡片块**——圆角深底容器，内部分两栏
  （标签行「你」/「嘎嘎」+ 状态徽标 + 正文行，中缝分隔线）。标签用
  鸭黄/绿色区分双方，状态用右侧小字（"进行中"/"等待中"/"超时"）。
- **教训**："拼字符串塞进一个 TextView" 不等于 UI。结构化内容就该有
  结构化的视图层级。

**附带**：状态染色仍保留——真鸭子图用 `PorterDuffColorFilter` 染
  （待机原色/录音红/发送橙/接收绿），录音按钮同步换色，两处状态一眼对齐。

### ADR-052 修订④：网络路径不限于 WiFi + 内置网络诊断（2026-09-26，v0.2.4）

- **更正**：v0.2.3 回复里说"手表必须和 broker 在同一个 WiFi 网段"——**说绝对了**。
  真正的要求是 **IP 可达**，链路可以是任意一种：
  | 链路 | 手表侧地址 | 到 broker 的路径 | 可行性 |
  |---|---|---|---|
  | WiFi 直连 | 与 broker 同网段 | 直连 | ✅ 最稳 |
  | **蓝牙 PAN（手机"蓝牙网络共享"）** | `192.168.44.x` 之类独立网段 | 手机路由转发 | ✅ 可行，带宽 1~3Mbps 足够 Opus 24kbps |
  | 手机热点 | 手机分配的网段 | 手机路由转发 | ✅ 可行 |
  蓝牙 PAN 需要的三件事：① 手机端"蓝牙网络共享"开关打开（**与蓝牙配对是两件事**）；
  ② 手表真连上 PAN 并拿到 IP；③ 手机 ROM 的蓝牙 NAT 不拦内网访问（部分厂商
  只放行外网，只能实测）。
- **新增网络诊断**（设置页「网络诊断」区）：
  - **本机 IP 列表**：枚举所有非回环网卡的 IPv4。蓝牙 PAN 会以 `bnep`/`bt-pan`
    之类的接口出现——**看得到地址就说明手机把网分过来了**，看不到就是共享没开。
  - **测试连接 broker**：不走 MQTT，直接发起 TCP 连接打 `host:port`（4s 超时），
    把结果写在设置页。**把"网到不了"和"MQTT 不对"两层分开**——TCP 通了但
    MQTT 还连不上，就该查 broker/端口/防火墙；TCP 就超时，就是链路问题。
- **理由**：用户连不上时"一直是 MQTT 未连接"，既不知道是配置错还是网不通，
  也不知道蓝牙共享有没有生效。诊断信息就地展示，不用插 adb 也能自查。

## ADR-053：夜间深睡——蓝牙连着也睡（凌晨电老鼠修复）

- **日期**：2026-09-27
- **背景**：用户实测"睡前满电、凌晨 4 点剩 3%"（600mAh ÷ ~5h ≈ 100mA+ 均流，
  与亮屏基线 98mA 几乎一样）。根因不在息屏（它只关背光），而在深睡门禁：
  原条件"未连手机"被手机 App 的保活彻底堵死——BLE 一夜不断，设备整夜跑着
  CPU 主循环 + IMU 94Hz 采样 + 射频 33 次/s 连接事件唤醒，深睡从未发生。
- **选择**：夜间窗口（01:00–07:00，RTC 本地时间）内，息屏 + 空闲 + 未充电
  持续 30 分钟 → 即使蓝牙连着也深睡；白天维持原门禁（连着不睡，保住
  "消息息屏直达"）。RTC 未对时则不启用（拿不准几点就保守）。唤醒 =
  BOOT/PWR 键（ext0 + AXP 硬件级），醒来冷启动自动重连 BLE。
- **理由**：凌晨没人对嘎嘎说话，通宵 ~100mA → <1mA 是数量级改善；白天
  行为零变化，核心承诺（息屏收消息亮屏直达）不受损。
- **代价（明示）**：睡着的时段消息到不了设备（手机侧照常收，MQTT 缓存里
  的 receipt/reply 设备醒来后靠对账补——msg_id 台账 5 条内可全补）。
  早高峰前如需消息，按一下任意键即醒。

## ADR-054：熄屏渲染闸——黑屏不重绘（白天续航第一刀）

- **日期**：2026-09-27
- **背景**：用户报白天熄屏续航撑死 7 小时（~100mA 均流）。排查发现 Ui::tick
  无熄屏闸：只要有一张卡在"等待回复"，"正在回复…"点数动效每 500ms 触发
  20 卡全量重建 + QSPI 刷屏——**对着 DISPOFF 的黑屏刷一整天**。DISPOFF 只
  断显示驱动（面板零功耗），但 LVGL 渲染管线照跑，240MHz CPU 全速出力。
- **选择**：① Ui::tick 熄屏即返回（黑屏不渲染，动效/状态栏/便签全停）；
  ② 新增 Ui::fullRefresh()：亮屏瞬间全树 invalidate + 立即重绘，一次性
  对齐熄屏期间的迟到状态（顺带兜住 GRAM 滞后不确定性）；③ main 里挂到
  setScreen(On) 回调。
- **理由**：渲染是熄屏态最大的确定性开销，闸掉是纯赚；事件驱动的直接渲染
  （息屏收 reply 亮屏直达）不受影响——那条路径本来就强制亮屏。
- **代价（明示）**：熄屏期间点数动画冻结、状态栏时钟停走（亮屏即对齐，
  无感）。余下的熄屏开销（CPU WFI ~25mA + IMU + BLE 射频）要靠 tickless
  /auto light sleep（esp_pm）再砍——那步动全局行为（USB 控制台/BLE 节奏），
  留待实测验证后再上，本刀不动它。

## ADR-055：下行写通道双修复——OEM 写上限封顶 + 合批常驻守卫（App v0.4.10/0.4.11）

- **日期**：2026-09-27
- **背景**：用户报"App 几秒必闪退 + 卡片永远黄色"。崩溃缓冲实锤第一层：
  Android 13+ 的 writeCharacteristic 对"值超 MTU-3"**抛异常而非返回错误**，
  514B 合批大包从 HiveMQ 回调线程炸出 = 进程死。修复 catch 后暴露第二层：
  设备侧协商 MTU=517，但 ColorOS 栈**确定性拒绝 514B 写**（OEM 内部上限与
  协商值不一致）；且 514B 异常后出现"小包直写通、合批帧零到达"的静默滞留
  （一次性 12ms 冲刷定时器协程失活的嫌疑最大，注入实验证实管道本身通）。
- **选择**：① 写入块硬性封顶 240B（所有 OEM 安全的经典值；音频下行
  240B/写@20ms 节奏仍远超实时）；② 合批加 50ms 常驻冲刷守卫——正确性
  不再押在任何一次性协程的存活上，缓冲最多滞留 50ms；③ 保留 v0.4.10 的
  异常 catch + MTU 缓存自愈降档作为最后防线。
- **验证**：装机后 error/link/hello_ack/receipt 四类下行全部实弹到达设备，
  对账补发恢复。
- **代价**：大帧下行写入次数变多（每 240B 一次 GATT 写）——对消息模式无感，
  talk 音频吞吐仍富余。

### ADR-056：回声判据只认 sender——🦆 前缀误杀全部 AI 回复（2026-09-27）

- **现象**：receipt 修复后用户仍报"卡片永远正在回复"（一天多）。飞书 API
  直查铁证：Hermes（cli_a9f77be018b9dcc0，与配置匹配）的回复规规矩矩带
  parent/root 引用，但**文本以"🦆"开头**——与上行消息的前缀撞车。
- **根因**：入向过滤链第 2 条"文本以 🦆 开头 = 自己的上行回声 → 忽略"。
  该判据本想双保险，实际把每一条 AI 回复都当回声静默吃掉（不打日志）。
- **修复**：回声判定只认 sender（上行/工具指令都走本 app 凭证 send_text，
  sender 判据精确无歧义），前缀判据删除。
- **教训**：内容特征（前缀）不能当身份判据用——别人也可能用同样的前缀；
  凡"静默丢弃"的过滤器都必须有日志或有精确判据，二者缺一就是埋雷。

### ADR-057：手表交互闭环——双击唤起、开录免触、双晃发送（watch v0.2.6）

- **日期**：2026-09-27
- **背景**：用户定稿主流程："双击右下键（系统级快捷方式，实测 F2）启动
  → 启动即录音 → 说完**晃两下手腕**自动停止并发送"。两个前置障碍实锤：
  ① 旧交互"晃一下=切换"从未真正可用——调试页传感器恒 0 的 bug（传感器
  只有"晃动录音"开关开着才 start，默认关）掩盖了一切，用户一度以为手表
  没有加速度计/陀螺仪；② 右上键（实测 F1）在系统里绑定"双击右下键打开
  嘎嘎"后，单击 F1 的"回主页"在 ColorOS 策略层执行，App `onKeyDown`
  返回 true 拦不住——回主页 → app 被冻结/回收 → 再双击右下键冷启动 →
  自动开录 → 循环，表现为"明明回到主页又开始录音"。
- **选择**：① 录音启停彻底去按键化：启动即录（已有）+ **双晃 = 切换录音**
  （加速度合矢量差分超阈值算一晃，两晃间隔 350ms~1400ms 才算一次指令，
  触发后 1500ms 冷却防三连晃），默认开；② 放弃 F1：不再指望拦截系统
  保留行为，右上角留系统语义（ROM 可改"无操作"但没必要）；③ 修调试页
  传感器 bug：进调试页无条件 start，离开时按开关决定停——ACC/GYR 读不
  读得到，进调试页一眼可见。
- **理由**：一下冲击的误触源太多（走路摆臂、抬手看表、拧门把手），连晃
  两下是刻意动作；免按键免触屏是穿戴设备的正解（屏小手滑，ADR-045/052
  已两次踩坑）。F1 拦截属于和 ROM 策略层掰手腕，无障碍服务方案代价
  （授权 + 省电策略查杀）远超收益。
- **代价（明示）**：传感器亮屏期间常开（onPause 必停保续航，息屏时不能
  晃停——但息屏时本就没在录）；"晃动录音"开关语义从单晃变双晃，老用户
  显式关过的保持关闭（默认值只对没动过开关的生效）。

## ADR-057：信令按设备隔离——device 字段贯穿收发与对账

- **日期**：2026-09-27
- **背景**：手表客户端上线后与胸前嘎嘎同群同 broker 并行测试，实锤多设备
  串扰：手表的 receipt 广播下来被胸前设备的 FIFO 卡"认领"（卡片显示手表
  的 ASR 文本"能听我唱吗？"），手表的 reply 也灌进胸前设备的等待卡；对账
  台账同理——A 设备的查询补到 B 设备的回执。单设备时代的一切隐式假设
  （"群里只有一台嘎嘎"）全部失效。
- **选择**：device 字段贯穿全链路——
  ① 上行：rec_start/rec_stop/rec_status_query 携带 `device`（hello 本就有）；
  ② 服务端：receipt/error 盖 device 戳下发；对账台账按 device 记账，查询
    按设备过滤重发；reply 经 msg_id→device 映射找回归属、按设备的未领取
    台账补发；不带 device 的旧信令回落 hello 登记设备（单设备零影响）；
  ③ 设备端：receipt/reply/error 带 device 戳且非本机 → 忽略（打日志）；
    无戳（旧服务端）维持兼容。
- **理由**：广播主题（gaga/down）+ 多设备 = 必须显式归属；字段可选、
  双向兼容，旧固件/旧服务端混布不会更坏（最多退化回串扰前行为）。
- **备注**：手表 App 侧的字段同步在代码里（Signaling 加 device + 过滤），
  待装机版本一并生效。

### ADR-058：紧握手势 = 切换录音——借系统识别，不自研六轴（watch v0.2.8）

- **日期**：2026-09-27
- **背景**：用户发现 OPPO Watch X3 系统支持"紧握松开"手势（可绑定打开
  App），且比"晃两下"更合适：晃动手腕会撞上"转腕自动灭屏"——第二次
  还没晃，屏幕先灭了。紧握手前臂不动，不触发灭屏。用户问：六轴能不能
  自己识别紧握？
- **选择**：**不自研识别，借系统的**。系统手势绑定"打开嘎嘎"后：紧握 #1
  冷启动 → onCreate 自动开录；紧握 #2 时 App 已在前台 → singleTask 的
  `onNewIntent` → 在这里 `toggleRecord()` = 停止并发送；紧握 #3 → 再开录。
  一处十几行的改动，零按键零触屏零传感器常开。
- **理由**：① 系统识别免费且可靠（这正是它"快速唤起"的原因），省电
  （IMU 不用常开）、免调阈值、不怕走路误触；② 自研 IMU 握拳识别信噪比
  低（握拳信号比甩腕小一个量级）、个体差异大（握力/佩戴松紧/表位）、
  要传感器常开耗电——能用系统的就别自己造。
- **代价与验证项（明示）**：**赌 ColorOS 手势在 App 前台时也会触发启动
  意图**——部分 ROM 手势只在息屏/表盘态生效。真机验证清单：录音中紧握
  能否停录。若不触发，退回备选路线：调试页先采集"紧握松开"的 ACC/GYR
  波形样本再定特征（不拍脑袋写检测器）。

### ADR-059：物理键零接触——App 不消费不拦截不映射任何按键（watch v0.2.9）

- **日期**：2026-09-27
- **背景**：v0.2.2 起的白名单把 F2/音量/相机/F3~F12/BUTTON_* 全算录音键，
  用户报"在软件里随便按上下两个键都会触发/结束录音，看着很奇怪"。历史
  负担：F1 的"回主页"在 ColorOS 策略层执行拦不住（ADR-057 已述），拦截
  与系统打架的问题消不掉。
- **选择**：录音交互已由晃两下（ADR-057/060）、系统手势唤起（ADR-058）、
  触屏覆盖——物理键**全部交回系统**：删除 KEY_RECORD_KEYS/KEY_BACK_KEYS
  白名单与 onKeyDown/onKeyUp 拦截，连 MediaSession 媒体键兜底也一并删
  （它同样是"打按钮主意"的路径，且手表无耳机场景）。调试页按键记录
  保留（纯观察，一个都不消费）。
- **理由**：拦截系统键是一场赢不了的拉锯（ROM 策略层永远优先），且用户
  已有更好的免按键交互；系统键回归系统语义（F1 回主页、F2 原默认），
  行为可预期。
- **代价**：App 内按键导航（F1 返回上一视图）没了——视图切换只剩手势
  和触屏，可接受。

### ADR-060：手势判据 v2 单轴反向交替 + 常亮 + 灭屏自动收尾（watch v0.3.0）

- **日期**：2026-09-27
- **背景**：用户实测推翻两个假设：① 紧握手势**只在表盘态有效**，App
  在前台时系统不触发——ADR-058 的 onNewIntent 路径对"软件内结束录音"
  无效（对"系统唤起已在前台的 App"仍有效，如双击右下键快捷方式，保留）；
  ② 晃动会撞自动灭屏——晃到第二下屏幕先黑了。另：v1 合矢量差分判据
  会被走路/骑车颠簸误触（竖直方向重复冲击，步频两步间隔 600ms 恰在
  350~1400ms 配对窗内）。
- **选择**：① **判据 v2 = 单轴反向交替双脉冲**：任一轴相邻帧差分超阈值
  算一脉冲，两次脉冲同轴、方向相反、窗内配对才算指令——"上下甩两下"
  （屈伸）与"左右摆两下"（偏摆）都命中，而颠簸/走路是同向冲击天然凑
  不出反向交替；手腕平摆不翻掌，也不触发 ColorOS 转腕灭屏。② **常亮
  加固**：布局 keepScreenOn 之上再挂 Window 级 FLAG_KEEP_SCREEN_ON——
  灭屏会掐手势传感器和 ColorOS 后台网，录音交互整个失效。③ **灭屏自动
  收尾**：onPause 时若还在录音就 stopAndSend——话已说到一半，与其烂在
  离线缓冲不如直接发出去。
- **理由**：判据从"幅度"升级到"幅度+方向+交替模式"，是用物理特征换
  误触免疫；常亮是穿戴录音设备的默认预期（用户明确要求）；灭屏收尾把
  最坏情况的损失从"全丢"降到"照发"。
- **代价（明示）**：常亮烧屏耗电（AMOLED 黑底 + 录音场景短时使用，可
  接受；息屏续航靠用户手动息屏/切走触发收尾）；反向交替判据要求用户
  晃得"有来有回"，纯同向连拍两下不认——调试页有实时指引（待第1晃/
  等X±反向）。

## ADR-061：RTC 硬件播种 + 卡片时间右上角 + 鸭图标答行（UI 细节定稿）

- **日期**：2026-09-27
- **背景**：① 用户报"每次时间都不对"——软件对时锚点断电即失，重启后
  时间归零，卡片时间显示时有时无；② 用户否决行内时间方案："什么软件
  会把时间放在消息条里边？应该放卡片右上角刚好空着"；③ 用户要求 GAGA
  答行用小鸭子图标顶替"GAGA"字样（"冒号可以保留，就表示你说了什么
  Gaga 说了什么"）。
- **选择**：① `rtcInit` 从硬件 RTC（内部低速时钟，断电有纽扣电池/RTC
  寄存器保活）读出墙钟，`civilToUnix`（Hinnant days_from_civil 逆变换）
  播种软件锚点 `s_baseUnix/s_baseMs`——开机即有可用时间，联网对时只做
  精修；② 卡片首行改横排容器 `askRow`：你问（flex 伸展，2 行封顶）+
  右上角 `FONT_NUM` 灰色 HH:MM（`rtcHmAt` 按卡创建时刻换算，未对时则
  隐藏）；③ 答行改 `replyRow` 横排：16px ARGB8888 鸭图标
  （`img_duck16`，与 FONT_CJK 16px 字号齐平，1KB flash）+ "：内容"
  （冒号保留在文本里；Failed 态文案无冒号前缀，图标隐藏）；详情页同步。
- **理由**：RTC 播种把"时间显示"从"需要网络"降级为"开机即得"；
  右上角时间是用户拍板的信息架构；图标是品牌资产复用（gaga-icon.png
  LANCZOS 降采样 + 圆形遮罩），16px 软件渲染零感知。
- **代价**：卡片控件树多两行容器（20 卡 × 2 个 lv_obj，LVGL 对象内存
  +1.6KB，可接受）；talk 覆盖层仍是文字前缀"GAGA："（语音优先场景，
  保持不动）。

## ADR-062：帧解码器流式重写——合批多帧丢失是下行断链的根因（ADR 序列续）

- **日期**：2026-09-27
- **背景**：用户复报"熄屏发消息 receipt 和 reply 都收不到"。四路日志
  定位：设备 10s 对账查询持续到达（上行活）→ 服务端每查询补发
  [reply+receipt×2]（服务端活）→ App 全部收到并写 BLE，写入零失败
  （App 活）→ 设备照常轮询 = 卡片永不点亮。注入实验（mosquitto_pub
  无戳信令）也石沉大海。**设备对下行字节"栈级 ACK、应用层零反应"。**
- **根因（两层叠加）**：设备 `FrameDecoder::feed` 是**按包解析**（一次
  BLE 写入 = 一个协议包，只消费 `lenField` 字节、丢弃同写入内首帧之后
  的所有字节）；而 App 为治音频延迟加了**合批层**（12ms/50ms 窗口内
  多条 MQTT 消息拼接成一次 BLE 写）。服务端的补发恰是 3 条突发 → 合成
  一写 → 只有第一条幸存。更糟：写入切在帧中间时，残字节被当包头，
  撞出随机 `0x80` 首包即进入"等幽灵帧"状态，后续几十条信令被吞进
  随机长度的鬼帧里；`decoder_.reset()` 只在 BLE 重连时调用——连接
  不断，永不复苏。
- **选择**：① `FrameDecoder` 重写为**字节流状态机**（Scan →
  SinglePayload / SubHeader → SubPayload），一次 feed 解任意多帧、一帧
  可跨任意多次 feed，写入边界对协议无意义；分片帧（0x80/0x00/尾包）
  的包结构在流内依然成立，子包超宣告总长即报错重扫。② GattServer 加
  **残帧看门狗**：`decoder_.partial()` 且 >1s 无续包（`lastRxMs_` 基准）
  → reset 重扫（帧内续包是 20-50ms 级，1s 足够宽容）。
- **理由**：按包解析对"App 是哑管道 + OEM 写上限 + 合批"的真实世界
  过于理想化——修复选择让接收端适应任意切分（防御纵深），而不是禁止
  发送端合批（合批是音频延迟的正确解，ADR-055）。
- **代价**：解码器状态机复杂度上升（四态）；孤立垃圾字节流的重扫是
  best-effort（与旧版一致）。**未烧录前用户侧缓解：重启设备**
  （reset 随连接初始化执行，可临时恢复到下一次幽灵帧）。

## ADR-063：对账轮询追赔上限 + 退避——无限轮询是灭屏掉电元凶

- **日期**：2026-09-27
- **背景**：用户手机进另一系统 1 小时，手机发热、电量 70%→20%。日志
  实锤：卡片永滞 Sending（receipt 被 ADR-062 的解码器 bug 吃掉）→
  设备 10s×∞ 轮询 → 每次查询服务端回 4 条下行 → App 全程陪跑 → 手机
  无法深度休眠。用户自己猜中了方向："一直在不停地轮询，每 10 秒一次"。
- **选择**：① `MsgLog::hasStaleSending` 加 maxAge 重载，Sending 卡只在
  [10s, 120s] 窗口内追询（UI 60s 已切"识别超时，仍在等…"软文案；
  迟到 receipt 不依赖轮询，`fillAsk` 按卡片状态自然收）；② Waiting 卡
  维持 [10s, 5min] 不变；③ 间隔自适应退避：上一轮查询后账本版本没动
  （没追回任何东西）→ 间隔 +10s 递增到 30s 封顶，追回了（或来了新卡）
  立即回满速 10s。
- **理由**：对账是**补偿性**机制，不该比主链路更执着——僵尸链路上
  无限重试既追不回丢失（服务端幂等重发也到不了），又把三端全部拖进
  忙轮询。上限 + 退避把最坏情况从"永久 10s 轮询"压到"120s 内最多
  12 次询问，之后静默等自然恢复"。
- **代价**：Sending 超 120s 后不再主动对账（若链路恢复且服务端台账
  仍在，靠下一次自然交互的 rec_start 触发；卡片文案已如实显示超时）。

### ADR-061：MQTT 公网暴露必须开鉴权——三端补用户名密码（2026-09-27）

- **背景**：手表要在出门（4G/外网）场景可用，用户计划把家里的 mosquitto
  经路由器映射到公网 1883，问"无鉴权是不是也做不了什么"。评估结论：
  做得了很多——匿名公网 MQTT 会被扫描器分钟级发现（Shodan 常年扫
  1883），且 topic 全公开在 protocol.md：① 订 `gaga/down` 实时偷听全部
  对话转写（receipt/reply，还有 rec_status_query 对账重发兜底）；② 伪造
  设备发 `gaga/up` 往飞书群注入内容 + 烧 ASR/LLM 按量账单；③ 用固定
  clientId `server` 连接互踢，直接瘫痪服务端且难排查。
- **选择**：最低防线 = MQTT username/password（三端）：mosquitto
  `allow_anonymous false` + passwd 文件（操作手册在 server/README「公网
  部署」）；服务端零代码（`MQTT_USERNAME/PASSWORD` 环境变量早就支持）；
  手表（v0.3.3）与 App（v0.4.14）各加设置项与 HiveMQ simpleAuth，凭据
  留空 = 匿名（向后兼容局域网无鉴权 broker）。幂等重连判断含凭据
  （密码变了必须重建连接），僵尸自愈重建透传凭据。
- **明确不做**：MQTT over TLS（8883）暂缓——密码已挡掉绝大多数现实风险
  （扫描器不撞库、不 sniff 专线），自签 CA 分发到三端的成本对个人项目
  不划算；若威胁模型升级（密码疑似被 sniff），再上 TLS 或直接迁云 broker
  （EMQX Cloud 免费档自带 TLS+鉴权，还是免家宽公网 IP 的替代路径）。
- **代价（明示）**：明文 TCP 上密码可被链路嗅探（接受，见上）；多设备
  共用一套账号（够用，不做 per-device 凭据与 ACL）。
