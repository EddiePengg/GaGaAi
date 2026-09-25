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
