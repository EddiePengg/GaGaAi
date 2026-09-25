# Bug 分析报告（2026-09-23，三个真机 bug 的根因分析）

> 工作方式：全部结论都先读官方代码（本地 `reference/waveshare-1.75C-repo/` 的
> 1.75C 官方仓库 + ESP-IDF 内置 NimBLE 例程 + managed_components 里已下载的
> esp_codec_dev/esp_lvgl_adapter 源码），引用到具体文件：行号。拿不准的标"[未证实]"。
>
> 本文件只分析不改代码；修复方案在文末，审过再动手。

## Bug 1：麦克风采音全零（RMS 恒 0）

### 实测事实链（我这边已采集到的证据）

1. 采集泵节奏正确（8s 录音 321~324 帧 ≈ 20ms/帧），Opus 静音包 8B（修法正确），但 RMS 恒 0。
2. GPIO 活动探测（串口 'g'，50ms 翻转计数）在录音中测得：
   MCLK42=14079~16142、BCLK9=11601~12288、WS45=2248（≈24kHz 帧）都在跳，
   **DIN10=0——ES7210 的 SDOUT 物理线全程静止**。结论：ESP32 侧时钟全在跑，
   ES7210 芯片没往线上吐数据。
3. ES7210 寄存器回读（串口 'e'，经 esp_codec_dev_read_reg）全部符合驱动预期：
   REG02=C1（DLL）、REG07=20（OSR）、REG11=60（16bit I2S 标准格式）、
   REG41/42=70（micbias 2.87V 开）、REG4B=00（MIC12 上电）、ES8311 REG0E=00
   （ASDOUT 高阻修复已生效，见下）。
4. 官方 05_Spec_Analyzer（1.75C 仓库）与本机寄存器配置逐项一致后仍全零。

### 官方代码里的采集配置（两种官方姿势）

**姿势 A：05_Spec_Analyzer（简单立体声）**
`reference/waveshare-1.75C-repo/examples/esp-idf/05_Spec_Analyzer/`：
- main.c:16-18：`SAMPLE_RATE 16000`、`CHANNELS 2`（**立体声 16kHz，非 TDM**）
- main.c:57 经 `bsp_extra_i2s_read` → `components/bsp_extra/src/bsp_board_extra.c:66-72`
  = `esp_codec_dev_read(record_dev_handle, ...)` 一次读 1024×2ch×2B
- main.c:64-68：左+右声道（MIC1=L/MIC2=R）相加 /2 合成单声道
- `bsp_extra/src/bsp_board_extra.c:150-167`：`bsp_audio_codec_speaker_init()` +
  `bsp_audio_codec_microphone_init()` 后 `set_fs(16000, 16bit, 2ch)` 把**播放和采集
  两个 codec 一起 open 到 16kHz 立体声**（bsp_board_extra.c:82-107）
- I2S 通道来自注册中心 BSP 的 `bsp_audio_init`（STD 双工 mono 22050 默认，
  靠 esp_codec_dev open 时重配到 16k 立体声）

**姿势 B：出厂 Brookesia Recorder（TDM 4 槽）**
`firmware/brookesia/components/waveshare__esp32_s3_touch_amoled_1_75/esp32_s3_touch_amoled_1_75.c`：
- `bsp_audio_init_voice_24k()`（433-464 行）：TX=STD 立体声 24k + **RX=TDM 4 槽 24k**
  （`I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG` + SLOT0-3，`bclk_div=8`，`total_slot=4`，
  两边 `mclk_multiple=256`），TX 的 din / RX 的 dout 置 UNUSED
- 麦克风 codec 初始化（545-552 行）：TDM 模式下 `mic_selected = MIC1|MIC2|MIC3`
  （`BSP_AUDIO_ES7210_CONNECTED_MIC_MASK`；**MIC3 是回声参考回路**，见
  `bsp_extra/include/bsp_board_extra.h`:31-41——TDM 槽序 MIC1/MIC3(ref)/MIC2/MIC4）
- esp_codec_dev 驱动侧：≥3 麦才进 TDM（`es7210.c`:16 `ENABLE_TDM_MAX_NUM=3`、
  ~185 行 `mic_num>=3`），进 TDM 后写 REG12=0x02（SDOUT TDM 输出）
- 增益：`esp_codec_dev_set_in_channel_gain(mic, 0x3, 24.0)`（bsp_extra_factory.c:287-291）

**两种姿势我都逐参数复刻过，都读全零**（含姿势 A 必需的 REG0E=0x00 修复——
ES8311 ASDOUT 与 ES7210 SDOUT1 在原理图上是同一根 net（GPIO10），esp_codec_dev 的
es8311.c:360 open 时无条件 REG0E=0x02 把 ADC 上电导致 ASDOUT 驱动全零盖线，
这个修复已回读验证生效）。所以"全零"的根因**不在软件配置层**——寄存器、时钟、
槽位、读法全部对齐官方后，DIN10 物理线仍然静止。

### 剩余嫌疑（按可能性排序）

1. **[高度怀疑] MCLK 引脚归属**：1.75C 原理图（本地 Schematic/ESP32-S3-Touch-AMOLED-1.75C-schematic.pdf）
   里 I2S0MCLK 网表挂 **GPIO16**；而本机 Arduino 线的 ES8311 提示音在 **MCLK=42** 下真实出声
   （ADR-018 真机验证记录）。两者矛盾说明：要么本机是 1.75 接线（MCLK=42）而 1.75C 的
   官方资料是另一个 PCB 改版，要么 ES8311 当时走了 BCLK 衍生时钟而 ES7210 没 MCLK 可用。
   **ES7210 是纯从设备，MCLK 不到它腿上它的调制器就不工作，SDOUT 恒静止——与观测完全吻合。**
   决定性实验（修复期做）：MCLK 改 GPIO16 录一次；data 流出=本机 C 接线，归零且 ES8311 也哑=42 是对的。
2. [次要] ES7210 这路供电/偏置在本机的实测状态（micbias 2.87V 已在寄存器层确认，
   但供电轨是否经某个我们没开的电源路径——[未证实]，需要万用表/原理图对电源页）。
3. [已排除] esp_codec_dev data_if 的 in_reconfig 零填充/字节数吞掉——已改直连
   `i2s_channel_read` 自证全零依旧；不是它。

### PCM 16k 重采样官方怎么做

官方没有做重采样：姿势 A 直接 16kHz 原生采集（codec 开 16k）；姿势 B 24kHz 原生进
ESP-SR/录音（24k 是该板语音通路官方速率）。我们的上行 Opus 必须 16k（火山 speech_opus
硬约束，ADR-021），因此：M1 录音用姿势 A 的 16k 原生（零重采样最稳）；M5 talk 因为
双工同速率约束（esp_codec_dev `check_fs_compatible`，对端开着时速率不同直接拒绝）
且下行是 24k ogg_opus，talk 期 24k 采集 + 2:3 抽取到 16k（自写 3 抽头预滤波+线性插值，
`esp32-idf/src/audio/AudioPipe.cpp` resample24to16）。

## Bug 2：中文显示豆腐块 → 修后变成"每个字背后一块黑底"

### 根因链（三个叠加，均已实锤）

1. **豆腐块**：ESP 内置 `lv_font_source_han_sans_sc_16_cjk`（我们最初启用）的子集里
   **没有"录/连"等字**——LVGL 对缺字画空心矩形占位 = 用户看到的豆腐块。
   证据：用 LVGL 像素快照（串口 'p'，lv_snapshot_take 离线渲染当前屏）统计，
   ESP 内置字体的字形 dsc 能查到但渲染墨迹为零；换自生子集后同位置墨迹出现。
2. **自生子集第一版完全渲染不出来**（连 ASCII 都不出）：lv_font_conv 1.5.3 默认
   RLE 压缩（`LV_FONT_FMT_TXT_COMPRESSED`），本构建（ESP lvgl 9.4.0）下压缩位图路径
   不吐像素——对照实验：压缩版 'P' 探针零墨迹，同字符集 `--no-compress` 版立刻出墨迹
   （snap 分带墨迹 0→637）。**结论：lv_font_conv 出 LVGL 9.4 必须加 `--no-compress`。**
3. **"每个字背后一块黑底"= 不是字体/渲染问题，是刷屏失败留下的陈旧 GRAM**：
   esp_lvgl_adapter 把 LVGL 显存放 PSRAM（`display_manager.c`:1159/1169 行，
   `MALLOC_CAP_SPIRAM`），而 esp_lcd 的 SPI 面板驱动**没有** `SPI_TRANS_DMA_USE_PSRAM`
   路径（esp_lcd/spi/esp_lcd_panel_io_spi.c 全文无此标志），于是 spi_master 对 PSRAM
   显存只能反弹到内部 DMA 缓冲（`esp_driver_spi/src/gpspi/spi_master.c`:1179-1210
   `setup_dma_priv_buffer`），单块 = 466×50行×2B ≈ 45KB。内部 RAM 被 NimBLE/opus 吃到
   只剩几百字节时分配失败（`Failed to allocate priv TX buffer` →
   `Draw bitmap failed: ESP_ERR_NO_MEM` 刷屏直接丢帧），面板 GRAM 留下没刷新的黑块。
   实锤对照：buffer_height=50 时一次快照期间 36 个 NO_MEM；改 10 行后 0 个，
   且快照内容干净了。
   修复：esp_lv_adapter profile `buffer_height` 50→10（反弹块 ≤9.3KB，内部 RAM 紧张也可分配），
   `esp32-idf/src/ui/Display.cpp`。

### 当前状态

`src/ui/fonts/gaga_font_cjk_16.c`（Noto Sans SC/思源黑体，OFL 1.1，886 字子集：
UI 固定文案 + docs/ 领域词汇 + ASCII + 常用标点；生成脚本与出处见
`esp32-idf/assets/fonts/README.md`）+ no-compress + buffer_height=10，
快照特写带里"测试ABC连接录音"各字形清晰无黑底。**待用户肉眼确认物理屏。**

### 对照官方例程核对的两个细节

- **label 怎么建**：官方 02_lvgl_demo_v9（`main/main.c`）与 05_Spec_Analyzer 都是
  `lv_label_create(lv_screen_active())` + `lv_label_set_text` 直建，不设任何背景
  （label 默认透明底）；03_esp-brookesia 的 sdkconfig.defaults 字体段全是 Montserrat，
  无 freetype/blend 类特殊配置。我们的 Ui.cpp 建法与之一致——所以"黑底"不是建 label
  姿势问题，坐实到上面的刷屏失败链路。
- **官方字体渲染配置**：官方 sdkconfig 只开 `CONFIG_LV_USE_CLIB_MALLOC/STRING/SPRINTF`
  与 `LV_OS_FREERTOS`、`LV_DEF_REFR_PERIOD=15`，没有额外的字体混合/bpp 开关；
  我们没有必要也不应该加这类开关。

## Bug 3：BLE 上行沉默 + 首次录音内存崩溃

### 3a. 上行沉默（一包都没出去）——**我引入的 bug，已定位到行**

官方 NimBLE 例程（IDF 自带 `examples/bluetooth/nimble/bleprph/main/gatt_svr.c`:239-252）
的注册姿势只有两步：`ble_gatts_count_cfg` + `ble_gatts_add_svcs`。

但 NimBLE 源码（`components/bt/host/nimble/nimble/nimble/host/src/ble_gatts.c`）里
`ble_gatts_add_svcs` **只是把服务表挂进待注册链表**（`ble_gatts_svc_defs[num++]=svcs`），
真正分配 ATT 句柄、回填 `val_handle` 的是 `ble_gatts_start()`（ble_gatts.c:1811），
而它在**主机任务启动时被 `ble_hs_start()` 自动调用**（ble_hs.c:808，即
nimble_port_freertos_init 拉起的 host task 里，异步发生）。

**我的错误**：`GattServer::begin()` 在 `add_svcs` 之后立刻读
`txValHandle_ = s_txValHandle`——那一刻 host task 还没跑 start，句柄恒 0，
`sendFrame` 前置检查 `txValHandle_ == 0` 永远拒绝，于是录音循环里每帧都静默丢弃。
（"上行 119 帧"的计数是在 isConnected() 分支里无条件 ++ 的，没反映发送成败——
计数器骗人，日志已修。）
我还一度错加了一次显式 `ble_gatts_start()`——它属重复调用（host 已调），
ble_gatts.c:3556 注释明说"gatts memory gets freed on next call to ble_gatts_start()"，
多余调用有风险，修复期移除。

**正确修法**：删掉显式 `ble_gatts_start()`；`txValHandle_` 改为用的时候现读
（host start 后 `s_txValHandle` 已回填稳定；最稳是在 handleConnect 里赋值）。
另外真机联调注意一个坑：macOS CoreBluetooth 会缓存设备 GATT 表，固件改过服务表后
Mac 端 bleak 会用陈旧缓存（报"Characteristic not found"）——换客户端会话/重启蓝牙可解，
不是固件 bug。

### 3b. 首次录音崩溃（LoadProhibited 重启）

根因链：esp_codec_dev 在 mic open 时做 I2S slot 重配（mono→stereo 或 TDM 切换），
重配要重分配 DMA 描述符；内部 RAM 当时只剩 ~300B（实测日志 `内部堆剩余 315B`），
分配失败 → esp_codec_dev 的善后路径在已坏的 TX 通道上 NULL 解引用
（`i2s_tx_channel_start`，i2s_common.c:130）→ 崩。

内存瘦身的官方依据：官方 05 示例 sdkconfig.defaults 用
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` + `RESERVE_INTERNAL=4096`——
即"大对象落 PSRAM、内部只留小对象"就是官方姿势。我已落地：
>16KB 的 malloc 自动落 PSRAM（opus 编码器态 ~50KB 因此移走，实测 opus 后内部堆
75867→67679B 只降了 188B 证明已走 PSRAM），全任务栈走 PSRAM
（`compat.h` taskCreatePsram），显示反弹块收敛到 9.3KB（Bug 2③）。
残留风险：BLE 初始化后内部堆自由量只剩 ~300B~8KB（NimBLE host+controller 是大头，
opus/BLE 之后 67679→323B），属于"薄但能跑"；若再遇 NO_MEM 类日志，
下一步选项是 esp_lv_adapter 的 `stack_in_psram=true`（再省 8KB）与
NimBLE buffer 裁剪。**I2S 槽位/模式已在初始化一次定型（TX STD 立体声 + RX 按当前形态），
open 只做时钟重配不再碰 DMA——崩溃路径从根上消失。**

## 修复方案（审过后照此动手）

1. **Bug 3a（最先修，一行级）**：删显式 `ble_gatts_start()`；`txValHandle_` 移到
   `handleConnect` 里赋值（此时 host 早已 start，句柄已回填）。
2. **Bug 1**：先按姿势 A 精确复刻跑一次（STD 立体声 16k、MIC1|MIC2、
   esp_codec_dev_read 或直连 i2s_channel_read 皆可——直连更可见）；
   仍全零则做决定性实验 MCLK=GPIO16（一次构建定乾坤：出数=本机 1.75C 接线，
   音频引脚整体切 16 并回归 beep；仍零=硬件/供电方向，把证据写进 hardware.md）。
   注意姿势 A 要求 mic 开 2 槽立体声 + 播放在 16k 同开（ duplex 同速率约束），
   M1 的 beep 已经在同速率，不冲突。
3. **Bug 2**：已修完（自生子集字体 + no-compress + buffer_height=10），
   只剩用户肉眼确认。
4. 文档：本文件 + roadmap/decisions 同步在修复完成后统一更新。

---

## 修复结果（2026-09-23 当日完成，全部真机验证）

### Bug 1（采音全零）→ **已修复，根因是 MCLK 引脚归属**

- 姿势 A（官方 05 同款：STD 立体声 16k、MIC1|MIC2、直连 i2s_channel_read）精确复刻后**仍全零**——软件配置层彻底排除完毕。
- 决定性实验：MCLK 从 GPIO42 改 **GPIO16**（1.75C 原理图接线）→ **立刻出数**
  （mic RMS 数百且随声音起伏，Opus 包从 8B 静音包变为 28~43B 语音包）。
  结论：**本机音频按 1.75C 接线，MCLK=GPIO16**。1.75 资料"GPIO16 无音频连接、
  MCLK=42 唯一一路"的说法对本机不适用——本机就是 1.75C。
- 全链路真实打通证据（Mac 扬声器 `say` 中文 TTS 喂麦克风）：
  录 10.1s / 420 帧零丢弃 → 桥转发 MQTT → 服务端解码 8.3s PCM →
  faster-whisper ASR → **飞书群收到"🦆 今天天气怎么样?适合骑车吗?"** →
  receipt 下行 → 设备收到回执。30s+ 连续录音 1513 帧零崩溃零 NO_MEM。
- 修复期发现并已修的伴随 bug：esp_codec_dev 的 ES7210 驱动在 mic_selected=0 时
  默认 MIC1|MIC2（非 TDM），不必强行开 MIC3；REG0E 修复/延期关喇叭/I2C 总线互斥锁
  全部保留（都是真问题）。

### Bug 2（中文显示）→ **已修复**（自生子集字体 + no-compress + buffer_height=10）

修复项见上文根因链；真机快照验证：状态行/标题/talk 文本各行墨迹分布正确，
探针"测试ABC连接录音"特写清晰无黑底，刷屏 NO_MEM 错误归零。**待用户肉眼确认物理屏。**

### Bug 3a（BLE 上行沉默）→ **已修复**

删显式 `ble_gatts_start()`（host 启动时 `ble_hs_start` 自动调，重复调用有害）；
`txValHandle_` 挪到 `handleConnect` 赋值。真机验证：桥 422 包到 MQTT、
服务端完整收到录音帧流（上条），notify 通路恢复。
附记：macOS CoreBluetooth 对改过形状的服务表会用陈旧缓存（bleak 报
Characteristic not found）——换客户端会话即恢复，不是固件问题。

### Bug 3b（首次录音崩溃）→ **已修复**

I2S 槽位一次定型（恒 STD 立体声）+ open 只做时钟重配 + 全任务栈 PSRAM +
>16KB malloc 落 PSRAM + 显示反弹块 9.3KB 封顶。30s+ 连续录音零崩溃；
启动期内部堆低点 6671B（薄但稳定——再紧就裁 NimBLE 缓冲/adapter 栈入 PSRAM）。

---

## 追加：用户真机验收第二轮的两个问题（2026-09-23 同日修完）

### 问题 1：PWR 键按下数秒才亮屏 → **已修复（DISPON 唤醒 0ms）**

根因链（逐段定位）：
- 轮询不慢：PWR 键读取是 AXP2101 PKEY 边沿 IRQ 轮询（10ms app tick 节奏），
  边沿→按键沿是毫秒级；
- **真正的延迟大头在面板唤醒**：息屏时我们发了 DISPOFF+SLPIN 双保险，
  而 esp_lcd_co5300 驱动在有 RST 脚的情况下，SLPIN 会进一步进 **DSTBON 深睡**
  （esp_lcd_co5300 的 `panel_co5300_disp_sleep`：`flags.sleep_mode` + 有 RST 脚就发
  `CO5300_CMD_DSTBON`），唤醒时要 reset + 重跑完整初始化序列（初始化命令表里有
  600ms 级延时）→ 用户体感"好几秒才亮"。
- 顺带核实：1.75C 原理图里 AXP2101 的 IRQ 网（AXP0IRQ）没有路由到 ESP32 的 GPIO
  （只在 BLDO1 附近有上拉），**中断唤醒在硬件上不可行**，轮询就是正确姿势。

修复：
- 息屏只发 DISPOFF（AMOLED 显示驱动断电、黑屏零功耗），不发 SLPIN——唤醒是
  亚秒内纯指令（真机实测 DISPON 耗时 0ms 日志），代价是息屏期面板静态功耗略高，
  换来"即按即亮"，对手环形态是对的取舍。
- 唤醒动作从"单击落定"挪到"按下沿"（`btnTop/btnBottom.onPressDown`）——单击判定
  要等 300ms 双击窗口，按下沿是即时的（手环同逻辑；幂等，重复触发无副作用）。
  连带把右下键（BOOT）单击也加了唤醒（用户 UX 改进，与按住说话不冲突）。
- 修复后链路预算：按下→轮询 ≤10ms + 防抖 40ms + DISPON 0ms + LVGL 重绘 ≈ **<100ms**，
  达到 <200ms 目标。真机手感待用户确认。

### 问题 2：彩色背景下每个字拖一块黑色矩形 → **内容层无此问题，已加自证**

排查过程（不猜，逐个验证）：
- 先排除字形本身：用 LVGL 快照逐像素统计新建"绿底矩形 + 白字 + 红字"测试画面
  （串口 'c' 命令）——53,200 像素里近黑像素仅 32 个（0.06%，抗锯齿边缘的正常产物），
  绿底完整、字形清晰、**无黑块**。快照路径与上屏路径内容一致（同一份 LVGL 渲染），
  证明内容层干净。
- 真凶是**刷屏陈旧 GRAM 残留**：之前 NO_MEM 丢帧时代（buffer_height=50，45KB 反弹块
  分配失败）面板 GRAM 里留了坏块；LVGL 只刷脏区，老垃圾会一直"透过"新 UI 显示，
  在彩色/文字区域看起来就是每处文字后拖黑块。
- 修复：起屏时**全屏 invalidate + refr_now 强制刷黑一次**（Display.cpp，持 LVGL 锁——
  不持锁会撞 lv_refr.c:280 "Invalidate area is not allowed during rendering" 断言死锁，
  已踩实），把 GRAM 一次性清成当前内容。加上此前 buffer_height=10 修复（不再丢帧），
  两条都补上后陈旧块不会再产生也不会再残留。
- 附教训：lv_refr_now/lv_obj_invalidate 这类调用必须持 esp_lvgl_adapter 的锁，
  且不要在有 LVGL 任务并发渲染时不持锁调（watchdog 死锁实证）。

### 问题 3：按下右下键"滴"没声，说完话松手的"滴滴"才有声 → **已修复（ADR-023 双工常开）**

实测事实链（串口日志 + GPIO 探针）：
- 按下后 beep 的 `spkOpen` 还在开喇叭（本板 I2C 极慢，boot 已有 pull-up 告警，
  单次 codec open 0.5~2s），300ms 确认窗的 `micOpen` 半路插进来做 I2S 配对重配，
  日志里 "beep x1 播放完成" 与 mic open **完全交叠**——"滴"被掐成听不见的碎片；
  松手的"滴滴"在 mic 关闭后才播所以响。
- 冷路径单独播放时 GPIO 探针：BCLK9/WS45 有翻转但 **DOUT8 全程零翻转**（时钟在跑、
  数据全零）→ 物理无声；录过一次音（mic+spk 同开舞蹈）后 TX 数据通路才活——
  与用户"说过话之后就有了"完全吻合。
- 假线索一并记录：`gpio_get_level(46)` 恒 0 曾被判成"功放使能没驱动"——实为测法错误
  （纯输出脚输入通路切断恒读 0）；es8311 驱动 `es8311_pa_power` 在 open 时一直有拉高 PA，
  说完话能响即证明。改 `GPIO_MODE_INPUT_OUTPUT` 后读回才是真值。

修复（对照官方 05_Spec_Analyzer `bsp_extra_codec_init` 的"两路 open 用完不关"姿势）：
- 双工常开（`AudioPipe::duplexOpen`）：`begin()` 双路 open @16k 后不再关；
  beep=纯写数据，彻底消灭"beep 与 micOpen 抢 I2S"竞态；talk 换 24k/回 16k 走
  整体换档（换档前等 beepBusy 落地）。
- PA=GPIO46 在 open 前拉高并常保（INPUT_OUTPUT 方向）。
- 代价：ES7210+ES8311 待机常上电（hardware.md 功耗预算已注明，深度休眠才断电）。

