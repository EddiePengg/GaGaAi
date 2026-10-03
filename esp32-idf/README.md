# esp32-idf —— gaga ai 设备固件

gaga 设备的固件主仓：音频采集/播放、按键与 IMU 交互、圆屏 UI、BLE/WiFi 链路。
当前固件版本见 `main/version.h` 的 `FW_VERSION`（串口启动横幅
`gaga ai fw-idf <版本>` 即此宏）。

**板级硬件不是固件的本体身份**：固件按 board 抽象层组织（ADR-076/077），
支持矩阵与当前开发板见 [`../docs/hardware.md`](../docs/hardware.md)。

## 技术栈

| 层 | 选型 | 出处/权威 |
|---|---|---|
| 框架 | ESP-IDF v5.5.x（PlatformIO espressif32@6.13 自带） | ADR-020 |
| board 抽象 | `main/boards/`（xiaozhi 式；`board_config.h` = 引脚唯一权威） | ADR-076/077 |
| GUI | LVGL 9 + 自生子集中文字体（OFL 思源黑体） | `main/idf_component.yml` / `assets/fonts/` |
| BLE | NimBLE（IDF 内置 `bt` 组件 host） | 沿用 ADR-011 选型 |
| 音频 | esp_codec_dev（ES8311 放音 / ES7210 双麦），I2S 通道固件自建 | `main/audio/AudioPipe.cpp` |
| Opus | `espressif/esp_audio_codec`（直接调 esp_opus_enc/dec_*） | `main/idf_component.yml` |
| 组件依赖 | IDF Component Manager 拉取（BSP/LVGL/esp_audio_codec） | `main/idf_component.yml` + `dependencies.lock` |

## 目录树

```
esp32-idf/
├── platformio.ini      # 本地开发快捷入口（env 是板 config.json 的手维护镜像）
├── sdkconfig.base      # 跨板公共 sdkconfig（入 git，手改这里）
├── sdkconfig.defaults  # ⚠️ 生成产物（scripts/build.py 合并 base+板 append），勿手改勿提交
├── scripts/build.py    # 多板构建入口（list/build/matrix）
├── partitions.csv      # 32MB 分区表（factory/ota_0 各 4M + model 1M + littlefs 资源区）
├── assets/fonts/       # 中文字体资产（字符集 + 生成方法）
├── main/               # 主组件（PlatformIO src_dir=main，对齐 xiaozhi 布局）
│   ├── main.cpp        #   装配层：模块实例 + 事件接线 + 任务拉起（ADR-038）
│   ├── boards/         #   board 硬件抽象层：
│   │   ├── common/     #     Board.h 基类 + board_selected.h 选板开关
│   │   └── <board>/    #     板子目录：board_config.h（引脚权威）+ board_caps.h
│   │                   #     + board_*.cpp（Board 子类）+ config.json（构建变体）
│   ├── protocol/  ble/  net/   # 帧协议 / NimBLE 链路 / BLE+WiFi 链路总机（LinkManager）
│   ├── audio/     voice/       # 音频前端/编解码 / WakeNet 语音唤醒
│   ├── input/     motion/      # 按键（BOOT+AXP2101 PWR）/ IMU 驱动+唤醒检测
│   ├── state/     rec/  talk/  notify/  # 状态机/消息卡/设置 / 录音语义 / realtime 会话 / TTS 通知
│   ├── ui/        power/       # 显示链路 + LVGL 界面 / 息屏与深睡
│   └── debug/                  # 串口调试命令（'h' 内存体检等，见下表）
├── managed_components/ # 组件注册中心拉取的依赖（勿改，已 gitignore）
└── dependencies.lock   # 组件版本锁定（提交入库保证可复现）
```

## 构建与刷机

多板构建入口是 `scripts/build.py`（PIO 版 xiaozhi build.py，ADR-077）：板级参数
（选板宏 / pio board / flash 容量 / 分区表 / sdkconfig 体质参数）的权威在
`main/boards/<board>/config.json`，脚本合并 `sdkconfig.base` + 板 append 生成
工程根 `sdkconfig.defaults`，再生成临时 ini 驱动 pio。

```bash
python3 scripts/build.py list                              # 列出所有板/变体
python3 scripts/build.py build waveshare-s3-amoled-1_75c   # 构建指定板（--all 全量）
python3 scripts/build.py build waveshare-s3-amoled-1_75c --upload --port /dev/cu.usbmodem101
python3 scripts/build.py matrix                            # GitHub Actions 矩阵 JSON（将来 CI 用）
```

本地开发快捷方式：`pio run` 照常可用——`platformio.ini` 里的 env 是板
config.json 的**手维护镜像**（改板级参数两处都要改）；刷机
`pio run -t upload --upload-port /dev/cu.usbmodem101`。两个注意：

- ⚠️ kconfgen 的 defaults 只补缺：老 env 想吸收 sdkconfig.base/config.json 的
  改值，先删 `sdkconfig.<env>` 再 `pio run`（build.py 路径按内容 hash 自动处理）。
- 首次构建会下载 ESP-IDF 工具链与 managed components，较慢；网络慢可喂代理：
  `HTTPS_PROXY=http://127.0.0.1:7890 pio run`。

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
稍慢也能抓到版本行。

## 触摸交互（导航模型定稿，ADR-035/036）

- 中央**上下滑**：消息卡列表（点卡进详情）
- **顶部下拉 / 轻点顶部**：直接打开完整设置页（音量/亮度/息屏时长/提示音/音效/语音唤醒/对话字幕/关于）
- 首页**左滑**：鸭子页（陪伴层：小鸭子 + 时段问候 + 今日概览 + 最近回复气泡）
- 子页面**右滑**：返回（详情/设置/鸭子页；返回胶囊按钮并存）

## 串口调试命令（无实体按键手段时的自测入口）

| 键 | 作用 |
|---|---|
| `r` | 按住说话 开始/停止切换 |
| `t` | talk 开始/结束切换 |
| `y` | 注入一对完整问答卡（UI 验收，不需要服务端） |
| `u` | 注入一张"正在回复…"等待卡（UI 验收） |
| `m` | 采集声道循环（0=MIC1 / 1=MIC2 / 2=平均） |
| `b` | 提示音一声（音频输出自测） |
| `s` | 亮屏/息屏切换 |
| `p` / `P` | LVGL 像素快照（当前屏统计 + ASCII 画；`P` 加中文探针行） |
| `d` | 采集 L/R 声道 RMS + 采样 hex 打印开关 |
| `e` | ES7210/ES8311 关键寄存器回读自检 |
| `g` | I2S 时钟/数据线 GPIO 翻转计数（链路活动性） |
| `i` | I2C 总线扫描 |
| `h` | 内存体检：内部/最大连续块/PSRAM/各任务栈水位（⚠️ 标记告警） |
| `n` | 发一帧 ping JSON（notify 通路自测） |
| `w` | 松手宽限循环 0/250/500/1000/2000ms |
| `l` | 锁定模式开关 |
| `S` | 设置页开/关（无触摸验证路径，ADR-031） |
| `V` / `L` | 音量循环 / 亮度循环（NVS 持久化） |
| `M` | IMU 姿态现场 dump（抬手调参的眼睛） |
| `K` | 抬手 Z 轴极性翻转（设备焊装方向兜底） |
| `W` | 语音唤醒开关 |
| `X` | 对话字幕开关 |
| `Q` / `C` | 顶部下拉快捷面板 / 鸭子页开关（无触摸验证路径） |
| `Z` | 立即深睡（验证 BOOT 键 ext0 唤醒路径） |

## 构建期已踩过的坑（仍活的知识）

1. **PlatformIO 6.x 的 IDF 构建器对 app 组件编译选项做 `sorted()`**
   （`builder/frameworks/espidf.py` 的 `get_app_flags`）：`esp_lvgl_adapter` 经 PUBLIC
   传播的 `-include <abs-path>/lvgl_port_alignment.h` 二元组被拆散 → g++ 报
   `cannot specify '-o' with '-c' ... with multiple files`。对策在根
   `CMakeLists.txt`（从两个组件的 INTERFACE_COMPILE_OPTIONS 剥离该选项，语义无损）。
2. **BSP 的 `CONFIG_BSP_ERROR_CHECK=n` 编译不过**（上游 bug）：保持默认 y，
   sdkconfig.base 里有备注。
3. **`board_build.partitions` 必须显式给 pio**：否则 pio 静默用默认
   `partitions_singleapp.csv` 并按 1MB app 做体积检查，与 sdkconfig 的自定义分区
   不一致。两侧都指向同一 `partitions.csv`（build.py 生成的 env 已代管）。
4. **BLE 广播名尾号**：ESP-IDF 5.x 的 `ESP_MAC_BT` 派生规则（base+2）与早期固件
   不同，广播名 GAGA-XXXX 的 XXXX = 实际 BLE MAC 后四位，App 按 `GAGA-` 前缀 +
   Service UUID 过滤不受影响。
5. **kconfgen 语法陷阱**：sdkconfig.defaults 里 CONFIG 赋值行不许带行尾注释
   （INT 会被静默丢弃）、"is not set" 行必须带行首 `# `——build.py 合并时会警告，
   详见 ADR-077。

真机音频/显示/采集三大坑（2026-09-23）的根因分析与修复全记录：
[`../.agents/bug-analysis-0923.md`](../.agents/bug-analysis-0923.md)（MCLK 引脚、
中文字体、NimBLE val_handle）。

## 相关文档

| 文档 | 内容 |
|---|---|
| [`../docs/hardware.md`](../docs/hardware.md) | 板子支持矩阵、引脚权威、按键分工 |
| [`../docs/protocol.md`](../docs/protocol.md) | BLE 帧格式 / 信令 / MQTT 主题 |
| [`../.agents/decisions.md`](../.agents/decisions.md) | 架构决策记录（ADR-020 起；board 层 = ADR-076/077） |
| [`../.agents/power.md`](../.agents/power.md) | 功耗优化台账（AI 工作文档） |
