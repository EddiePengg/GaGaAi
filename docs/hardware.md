# 硬件规格

## 主板：微雪 ESP32-S3 1.75" AMOLED 圆形触摸开发板

| 部件 | 型号/规格 | 用途 |
|---|---|---|
| 主控 | ESP32-S3R8 双核 LX7 240MHz，叠封 8MB PSRAM（octal），外接 Flash 实测 32MB（esptool 确认；Wiki 页写 16MB 过时） | 有向量指令集（AI 加速）、BLE 5.0、原生 USB-OTG |
| 屏幕 | 1.75" AMOLED 466×466，驱动 CO5300（QSPI） | 支持 AOD 息屏显示（黑色像素零功耗） |
| 触摸 | CST9217（I2C，共享总线） | attachInterrupt 用 INT 脚 |
| IMU | QMI8658 六轴（共享 I2C 总线） | 官方示例用轮询，中断脚未引出 |
| 音频·收音 | **ES7210**：多通道麦克风 ADC（MIC1/MIC2 双麦，硬件回声消除 AEC） | "说"半边：麦克风×2 → ES7210 →(I2S)→ ESP32 |
| 音频·放音 | **ES8311**：语音 codec（内部含 AD+DA），**板上只用 DA 半边**——AD 未接线且固件主动下电让数据线（AudioPipe.h 约束⑤） | "听"半边：ESP32 →(I2S)→ ES8311 →(模拟小信号)→ 功放 |
| 音频·功放 | **NS4150B**：2.4W D 类功放，**独立第三颗芯片，不是 codec**；使能脚=GPIO46（高有效） | 喇叭推力：ES8311 模拟输出 → NS4150B → 扬声器 |
| 电源 | AXP2101 PMU + MX1.25 锂电池接口 | I2C 读电量；PWR 按键也挂在它上面 |
| RTC | PCF85063（共享 I2C 总线，经 AXP2101 电池供电） | 断电走时 |
| 电池 | 用户自选 602035 高温型（6×20×35mm，400mAh，3.7V，四重保护） | ⚠️ 比官方 702530 长 5mm，到手先比划舱位；⚠️ MX1.25 插头分正/反向，插前核对红线对丝印 `+`；⚠️ 充电电流须配 ≤200mA（0.5C），PMU 驱动接入时设置 |

状态栏数据源（fw-idf 0.3.0）：电量 = AXP2101 `BAT_PERCENT_DATA(0xA4)` 0~100、充电 =
`STATUS2(0x01)` bits7:5==0x01、在位 = `STATUS1(0x00)` bit3（寄存器对照官方 XPowersLib
AXP2101 示例）；时间 = PCF85063 `SEC_REG(0x04)` 起 7 字节 BCD（秒 bit7=OS 停摆标志），
信令 envelope 的 `ts` 自动对时。**实测注（2026-09-24）**：本机 I2C 扫描 0x51 无应答
（PCF85063 缺席/未焊），固件走**软件时钟兜底**（ts 对时 + millis 推算）；硬件 RTC 在位
时自动切回硬件优先。
| IO 扩展 | TCA9554 | 扩展 GPIO |
| 存储 | TF 卡槽（SDMMC） | 本期不用 |
| 按键 | PWR（接 AXP2101 PKEY）+ BOOT（GPIO0） | 物理位置映射实测确认 |
| 接口 | Type-C（原生 USB，macOS 识别为 /dev/cu.usbmodem*，无需驱动） | |
| 外壳 | 铝合金 + 挂绳 | 胸前佩戴 |

## 按键分工（写死，ADR-016，2026-09-22 v2 版）

| 按键 | 动作 | 功能 |
|---|---|---|
| 右上 | 单击 | 亮屏；无操作 10 秒自动息屏（无手动关屏） |
| 右上 | 长按 | 进入 realtime 对话模式（M5） |
| 右下 | 长按 | 按住说话：长按触发 → 滴滴 → 说话 → 松开 → 滴滴 → 发送 |
| 右下 | 单击/双击 | 不分配（避免与按住说话手势冲突） |

防抖：40ms 电气防抖；双击判定窗口 300ms；长按阈值 400ms（2026-09-23 从 800ms 调短，用户反馈）。录音中屏幕常亮显示红色图标+计时。
**松手宽限 500ms**（2026-09-23，用户报障"稍微松力就断"）：本板按键释放阈值离按住力太近，握力波动易误断；断开后 500ms 内重新咬合=无缝续录，窗尽才真正结束。串口 `w` 循环 0/250/500/1000/2000ms（0=断开即结束）；`l` 切锁定模式（松手完全不算数，再按一下才结束，100% 免疫握力）。

## 到手第一件事

```bash
pip3 install esptool
esptool.py --port /dev/cu.usbmodemXXXX read_flash 0x0 0x2000000 factory_backup.bin
```
备份原厂 32MB 固件（地址 0x2000000 = 32MB，别抄 8MB 的教程）。
⚠️ 2026-09-22 曾备份一份，但用户确认**并非原厂固件**（是前手刷过的未知系统），2026-09-25 已删除。如需原厂镜像请向微雪官方索取。

## 功耗预算（估算，待实测修正）

| 状态 | 估算电流 | 500mAh 电池 |
|---|---|---|
| 深度休眠（IMU 待命） | ~0.1mA | 按周计 |
| AOD 息屏时钟 | ~10~25mA | 1~2 天 |
| 亮屏使用 | ~60~150mA | 半天 |
| 语音通话（息屏） | ~100~250mA | 3~5 小时 |

> ✅ 2026-09-23 修订（ADR-027）：音频前端**按需上电**——按下说话才给 ES7210+ES8311+PA
> 上电（先开麦后放"滴"），会话结束整条前端断电（临时提示音播完即断）。曾短暂采用
> "双工常开"（ADR-023，估算 +几 mA）被用户否决：400mAh 挂坠付不起麦克风常开，且该
> 估算偏乐观。代价：按下后"滴"晚响 0.2~1s（等上电）。待机功耗回到上表水平，实测后再填。

电老虎是音频+屏幕，不是通信（BLE 转发仅 10~30mA）。实测方法：充满电挂 AOD，
每小时经 AXP2101 读电量寄存器记录。

## 骑行风噪四层防线

1. 凑近嘴边说话（距离平方律，最强手段）
2. 麦克风孔贴防风棉（砍 10~20dB 低频风噪）
3. ESP-SR AFE 噪声抑制（NS 模块；AEC 在录音场景闲置，实时对话才上岗）
4. 服务端 ASR 鲁棒性兜底

## 引脚定义

✅ 2026-09-22 已从官方示例包 `Mylibrary/pin_config.h` 抄录（GitHub 镜像：
doublemarkpro/ESP32-S3-Touch-AMOLED-CodexPedometer；原样快照存
`reference/waveshare-s3-amoled-1_75/pin_config.h`）。代码侧权威来源是
（Arduino 线已退役删除，ADR-041 修订①）；IDF 线的引脚权威 = 微雪官方 BSP。

| 功能 | 引脚 |
|---|---|
| 屏幕 CO5300（QSPI） | CS=12, SCLK=38, SDIO0~3=4/5/6/7, RST=39；TE 未引出 |
| 共享 I2C 总线 | SDA=15, SCL=14（触摸/IMU/PMU/RTC/IO 扩展全部共线） |
| 触摸 CST9217 | INT=11, RST=40 |
| I2S 音频 | BCLK=9, WS=45, DOUT=8(ESP32→ES8311 DSDIN→功放→扬声器), DIN=10(ES8311 ASDOUT/ES7210 SDOUT→ESP32), **MCLK=16**（2026-09-23 IDF 线决定性实验定案，见下）, PA 使能=46（NS4150B CTRL，高有效） |

> ⚠️ I2S 引脚 2026-09-22 经原理图（官方示例包 `Schematic/ESP32-S3-Touch-AMOLED-1.75-schematic.pdf` 网表）+ 官方 ESP-IDF BSP（`BSP_I2S_*`）+ Demo 08 实际 `setPins(9,45,8,10,42)` 三方交叉确认。示例包 `pin_config.h` 的 `// ES8311` 块有误导：`I2S_MCK_IO=16` 是错的（GPIO16 在原理图上无音频连接），其 `I2S_DI_IO=10 / I2S_DO_IO=8` 是 ESP32 视角的正确值；第二组宏 `DOPIN=10 / DIPIN=8` 是 codec 视角，别混。详见 decisions.md ADR-018。
>
> ⚠️⚠️ **2026-09-23 更正 MCLK 归属**：上面那条是 **1.75** 原理图的分析；本机实为 **1.75C**——
> 1.75C 原理图（`reference/waveshare-1.75C-repo/Schematic/`）里 I2S0MCLK 网表挂在
> **GPIO16**。IDF 线决定性实验（docs/bug-analysis-0923.md Bug1）：MCLK=42 时 ES7210
> 采集全零、DIN10 全程无翻转；切 GPIO16 立刻出数，且录音→BLE→MQTT→ASR→飞书全链路
> 真实打通。结论：**本机音频 MCLK=GPIO16**（当年 Arduino 线 pin_config.h 的 `I2S_MCK_IO=16`，线已退役 ADR-041）
> 对 1.75C 反而是对的；ADR-018 的"GPIO16 无音频连接"结论只适用于 1.75，不适用于本机）。
> （Arduino 线已退役删除，ADR-041；此结论对 IDF 线与本板永久有效。）
| IMU QMI8658 | I2C 共线（0x6B），中断脚未引出（轮询）；**Tap 双击引擎在传感器内部跑**（CTRL9 CONFIGURE_TAP，双击窗 500ms@500Hz ODR，官方推荐参数），主控轮询 STATUS1 bit10（读清除）——ADR-032 双击唤醒。深睡时 IMU 唤不醒主控（无 INT 脚，硬件死结）。**tap/抬手实测待真机回归** |
| BOOT 键 | GPIO0（digitalRead 直读）✅ 实测 = 物理**右下**键（2026-09-22） |
| PWR 键 | ⚠️ 非 GPIO，接 AXP2101 PKEY，`getIrqStatus()` 轮询读取 = 物理**右上**键 |
| TF 卡（备用） | CLK=2, CMD=1, DATA=3, CS=41 |

官方库对照：GFX_Library_for_Arduino（CO5300）、SensorLib（QMI8658/CST9217/PCF85063）、
XPowersLib（AXP2101）、ESP32_IO_Expander（TCA9554）、LVGL 8.4.0。
