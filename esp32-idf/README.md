# esp32-idf —— gaga ai 设备固件 ESP-IDF 线（ADR-020）

ESP-IDF 平行固件线，与 Arduino 线（`../esp32/`）**协议兼容**（BLE UUID/帧格式不变，
`../docs/protocol.md`），目标逐步追到功能平价后切换主线。Arduino 线冻结为可用保底版。

## 技术栈

| 层 | 选型 | 出处 |
|---|---|---|
| 框架 | ESP-IDF v5.5.x（PlatformIO espressif32@6.13 自带） | ADR-020 |
| 板级支持包 | 微雪官方 BSP `waveshare/esp32_s3_touch_amoled_1_75c` ^3.0.0（1.75C 归属） | ESP Component Registry，经 `src/idf_component.yml` 拉取 |
| 显示 | BSP `bsp_display_new()` + 自组 esp_lvgl_adapter 链路（保留 panel 句柄供息屏） | `src/ui/Display.cpp` |
| GUI | LVGL 9.4.x + 自生子集中文字体（OFL 思源黑体 886 字） | `src/idf_component.yml` / `assets/fonts/` |
| BLE | NimBLE（IDF 内置 `bt` 组件 host） | 对应 Arduino 线 ADR-011 选型 |
| 音频 | esp_codec_dev（ES8311 扬声器 / ES7210 双麦），I2S 通道固件自建 | `src/audio/AudioPipe.cpp`（MCLK=GPIO16 实测定案） |
| Opus | `espressif/esp_audio_codec`（直接调 esp_opus_enc/dec_*） | `src/idf_component.yml` |

## 当前进度（fw-idf 0.5.0，2026-09-25 落码待真机回归）

- [x] 工程骨架：PlatformIO + ESP-IDF 5.5.3 + 32MB 分区表（与 Arduino 线同布局）
- [x] 串口版本行 `gaga ai fw-idf 0.4.0`（115200，原生 USB）
- [x] 屏幕点亮 + LVGL 9.4 黑底 UI（录音/发送/已送达/对话各态）+ 中文完整显示
- [x] NimBLE 广播 `GAGA-0A4E` + NUS GATT（TX notify/RX write，MTU 517）+ 帧协议分包重组（与 Arduino 线字节级一致）
- [x] 按键（ADR-016）：BOOT=GPIO0 右下（长按按住说话、单击唤醒）、PWR=AXP2101 PKEY 边沿轮询右上（单击亮屏、长按进/出 realtime 占位）
- [x] **录音链路真通**：ES7210 采集 → Opus 16k/20ms → BLE 上行；真机 10s/32s 录音零丢弃零崩溃，经 Mac 桥到服务端 ASR 出字上飞书（M4 闭环实测）
- [x] 提示音 + 无操作 10s 自动息屏（DISPOFF+SLPIN）+ 任意键单击唤醒
- [x] M5 talk 骨架：talk_request→talk_ready→24k 双工→上行泵→下行 Ogg/Opus 播放泵→talk_end 全流程跑通（真人全双工对话待联调）
- [ ] 真人语音全双工对话联调（下行放音效果/打断/退出意图）
- [ ] ESP-SR AFE（风噪/回声消除，骑行刚需）

三个真机 bug 的根因分析+修复全记录：`../docs/bug-analysis-0923.md`

## 构建与刷机

```bash
# 构建（首次会下载 ESP-IDF 工具链与 BSP 等 managed components，较慢；
# 网络慢可先给 pio 喂代理：HTTPS_PROXY=http://127.0.0.1:7890 pio run）
pio run

# 刷机（板子是原生 USB，macOS 识别为 /dev/cu.usbmodem*）
pio run -t upload --upload-port /dev/cu.usbmodem101
```

### 抓启动日志（原生 USB 的关键姿势）

USB-Serial-JTAG 在**主机打开串口之前**的写入会被丢弃，所以"刷完机再开串口"什么
也看不到。正确姿势是端口常开 + 经 RTS 硬复位芯片（esptool `HardReset(uses_usb=True)`
序列），复位后从头抓：

```bash
python3 - <<'EOF'
import serial, sys, time
s = serial.Serial("/dev/cu.usbmodem101", 115200, timeout=0.2)
s.setRTS(True); time.sleep(0.2); s.setRTS(False); time.sleep(0.2)  # EN 拉低再放开 = 硬复位
s.reset_input_buffer()
end = time.time() + 12
while time.time() < end:
    d = s.read(4096)
    if d: sys.stdout.write(d.decode(errors="replace")); sys.stdout.flush()
EOF
```

固件在 `app_main()` 开头有 1.5s 控制台等待（见 main.cpp 注释），即使串口挂得
稍慢也能抓到版本行 `gaga ai fw-idf 0.5.0`。

## 触摸交互（导航模型定稿，ADR-035/036）

- 中央**上下滑**：消息卡列表（点卡进详情）
- **顶部下拉 / 轻点顶部**：直接打开完整设置页（音量/亮度/息屏时长/提示音/音效/语音唤醒/对话字幕/关于——面板与设置已合并，ADR-038）
- 首页**左滑**：鸭子页（陪伴层：小鸭子 + 时段问候 + 今日概览 + 最近回复气泡）
- 子页面**右滑**：返回（详情/设置/鸭子页；返回胶囊按钮并存）
- 任务管理器：不做（设备即应用无多任务；上下滑已归列表）

## 串口调试命令（无实体按键手段时的自测入口）

| 键 | 作用 |
|---|---|
| `r` | 按住说话 开始/停止切换（M1 链路） |
| `t` | talk 开始/结束切换（M5 realtime 占位） |
| `y` | 注入一对完整问答卡（UI 验收：你+GAGA 回复，不需要服务端） |
| `u` | 注入一张"正在回复…"等待卡（UI 验收） |
| `m` | 采集声道循环（0=MIC1 / 1=MIC2 / 2=平均） |
| `b` | 提示音一声（音频输出自测） |
| `s` | 亮屏/息屏切换 |
| `p` | LVGL 像素快照（当前屏统计 + ASCII 画） |
| `P` | 同上 + 探针行"测试ABC连接录音"（中文字体自证） |
| `d` | 采集 L/R 声道 RMS + 采样 hex 每 0.5s 打印开关 |
| `e` | ES7210/ES8311 关键寄存器回读自检 |
| `g` | I2S 时钟/数据线 GPIO 翻转计数（链路活动性） |
| `i` | I2C 总线扫描 |
| `h` | 内存体检：内部/最大连续块/PSRAM/各任务栈水位（⚠️ 标记告警） |
| `n` | 发一帧 ping JSON（notify 通路自测） |
| `w` | 松手宽限循环 0/250/500/1000/2000ms（默认 500，0=断开即结束） |
| `l` | 锁定模式开关（松手不算数，再按一下才结束） |
| `S` | 设置页开/关（触摸设置的无键盘验证路径，ADR-031） |
| `V` | 音量循环 0/30/60/85/100%（NVS 持久化，同设置页 slider） |
| `L` | 亮度循环 10/30/60/100%（CO5300 0x51 寄存器） |
| `M` | IMU 姿态现场 dump（抬手调参的眼睛：xyz/模长/屏态/降频态） |
| `K` | 抬手 Z 轴极性翻转（设备焊装方向兜底） |
| `W` | 语音唤醒开关（"Hi,乐鑫"，开 = mic 常开本地推理） |
| `X` | 对话字幕开关（关 = talk 纯语音模式，防长文本卡顿） |
| `Q` | 顶部下拉快捷面板开/关（ADR-035 无触摸验证路径） |
| `C` | 鸭子页开/关（ADR-036 左滑手势的无触摸验证路径） |
| `Z` | 立即深睡（验证 BOOT 键 ext0 唤醒路径） |

## 已踩过的坑（构建期）

1. **PlatformIO 6.x 的 IDF 构建器对 app 组件编译选项做 `sorted()`**
   （`builder/frameworks/espidf.py` 的 `get_app_flags`）：`esp_lvgl_adapter` 经 PUBLIC
   传播的 `-include <abs-path>/lvgl_port_alignment.h` 二元组被拆散，头文件路径被 g++
   当成第二个输入文件 → `cannot specify '-o' with '-c' ... with multiple files`。
   对策在根 `CMakeLists.txt`（从两个组件的 INTERFACE_COMPILE_OPTIONS 剥离该选项，
   语义无损，注释里有完整解释）。上游修复后那段可删。
2. **BSP 的 `CONFIG_BSP_ERROR_CHECK=n` 编译不过**（上游 bug）：
   `BSP_ERROR_CHECK_RETURN_ERR` 在返回指针的 `bsp_io_expander_init()` 里 `return
   esp_err_t`。保持默认 y 即可，sdkconfig.defaults 里有备注。
3. **`board_build.partitions` 必须显式给 pio**（`platformio.ini`）：否则 pio 静默用
   默认 `partitions_singleapp.csv` 生成 partitions.bin 并按 1MB app 做体积检查，
   与 sdkconfig 的自定义分区不一致。sdkconfig 与 pio 两侧都指向同一 `partitions.csv`。
4. **广播名尾号从 GAGA-0A4D 变为 GAGA-0A4E**：ESP-IDF 5.x 的 `ESP_MAC_BT` 派生规则
   与 Arduino 线所用 IDF 4.4 不同（base+2 vs base+1）。IDF 线固件的名字与实际
   BLE MAC（`80:45:6B:35:0A:4E`）一致，App 按 `GAGA-` 前缀 + Service UUID 过滤不受影响。
5. **真机音频/显示/采集三大坑（2026-09-23）已根因修复**：采音全零=MCLK 引脚归属
   （本机 1.75C 接线，MCLK=GPIO16）；中文豆腐块=ESP 内置字体子集缺字+
   lv_font_conv RLE 压缩不渲染（--no-compress 解决）+刷屏反弹块内存不足；
   BLE 上行沉默=NimBLE val_handle 在 host 启动才回填（不能 begin 时闩）。
   全部分析与修复记录在 `../docs/bug-analysis-0923.md`。
6. **BSP 归属 _c 变体**：`waveshare/esp32_s3_touch_amoled_1_75c` ^3.0.0 与 1_75 的
   差异仅在 MCLK/LCD_RST/TP_RST 三个引脚宏（_c: 16/1/2；1_75: 42/39/40）。本机
   音频 I2S 引脚由固件自建（AudioPipe 内 GAGA_I2S_GPIO_CFG，MCLK=16 实测定案），
   不吃 BSP 宏；显示/触摸对 RST 差异不敏感（真机回归通过）。**复用 BSP 引脚宏前
   必须核对本机实测**。

## 工程结构（PlatformIO 的 IDF 项目约定）

```
esp32-idf/
├── platformio.ini      # espressif32@^6.13.0 + framework=espidf + 32MB flash + 自定义分区表
├── CMakeLists.txt      # IDF 工程根（含 pio 构建器 sorted() 拆对 bug 的对策，注释完整）
├── partitions.csv      # 32MB 分区表，与 Arduino 线一致（双 8MB app 槽 + ~15.9MB 资源区）
├── sdkconfig.defaults  # flash 32MB/QIO、octal PSRAM+XIP、>16KB malloc 落 PSRAM、
│                       # USB-Serial-JTAG 控制台、NimBLE、LVGL 9（含 SNAPSHOT）
├── assets/fonts/       # 中文字体资产：字符集 + 生成方法（OFL 思源黑体子集）
├── src/
│   ├── CMakeLists.txt      # main 组件（REQUIRES 里 managed component 名是双下划线）
│   ├── idf_component.yml   # BSP(_c) + LVGL + esp_audio_codec 依赖
│   ├── main.cpp            # 装配层（ADR-038）：模块实例 + 事件接线 + 任务拉起
│   ├── AppContext.h        # 装配上下文：全模块指针集合（谁用谁一眼可读）
│   ├── version.h           # FW_VERSION / DEVICE_ID
│   ├── compat.h            # millis() / PSRAM 任务栈 / I2C 总线互斥锁
│   ├── protocol/frame.*    # 帧协议（与 Arduino 线字节级一致）
│   ├── ble/GattServer.*    # NimBLE 封装（发送接口自带互斥）
│   ├── input/ButtonHandler.*  # 单击/双击/长按状态机（平移）
│   ├── input/PwrKey.*      # PWR 键：AXP2101 PKEY 边沿 IRQ 轮询
│   ├── state/AppState.*    # 录音/屏幕/talk 状态机
│   ├── state/MsgLog.*      # 消息卡数据（20 条环形）
│   ├── state/Settings.*    # 用户设置（NVS：音量/亮度/息屏/提示音/唤醒/字幕）
│   ├── ui/Display.*        # 自组 BSP 显示链路（panel 句柄供息屏 + 亮度）
│   ├── ui/Ui.*             # LVGL 9 深空黑界面（含设置页）
│   ├── ui/fonts/gaga_font_cjk_16.c  # 生成的子集字体（提交入库）
│   ├── audio/AudioPipe.*   # ES7210/ES8311 通路（自建 I2S，含全部真机约束注释）
│   ├── audio/OpusCodec.*   # esp_audio_codec 的 Opus 编/解码封装
│   ├── audio/OggDemux.*    # M5 下行 ogg_opus 流式解封装
│   ├── audio/UplinkPump.*  # 上行泵（mic→Opus→BLE；确认窗帧缓冲）
│   ├── rec/Recorder.*      # 按住说话语义（确认窗/松手宽限/锁定/建卡）
│   ├── talk/TalkSession.*  # M5 会话状态机 + 下行播放泵（jitter 队列）
│   ├── motion/Qmi8658.*    # IMU 寄存器驱动（硬件 Tap 双击引擎）
│   ├── motion/Wake.*       # 唤醒检测（双击轮询 + 抬手姿态状态机）
│   ├── voice/KeywordWake.* # WakeNet 关键词唤醒（"Hi,乐鑫"，esp-sr）
│   ├── voice/NsnetStub.c   # NS 模型工厂 stub（断链 nsnet3 101KB DRAM，ADR-037）
│   ├── power/Power.*       # 息屏降频 + 挂机深睡
│   └── debug/SerialCmd.*   # 串口调试命令 + 快照/扫描工具
├── managed_components/ # 组件注册中心拉取的依赖（勿改，已 gitignore）
└── dependencies.lock   # 组件版本锁定（提交入库保证可复现）
```

## 与 Arduino 线的差异备忘

- 控制台走原生 USB（`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`），不再是 Arduino 的
  TinyUSB CDC；刷机/日志同一个 `/dev/cu.usbmodem*` 口。
- LVGL 由 BSP 经 esp_lvgl_adapter 管理（自建任务），应用侧只需
  `bsp_display_lock()`/`unlock()` 包裹 LVGL 调用。
- BLE 主机栈直接用 IDF 内置 NimBLE（不是 NimBLE-Arduino），API 不同但广播名/UUID 一致。
- 板级知识全部收敛到微雪 BSP（引脚/初始化序列不再自己维护，对照 ADR-017 的 Arduino 侧做法）。
