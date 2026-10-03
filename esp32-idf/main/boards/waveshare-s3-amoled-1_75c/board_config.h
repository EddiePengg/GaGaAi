#pragma once

// ============================================================================
// 板级引脚与参数的唯一权威（微雪 ESP32-S3-Touch-AMOLED-1.75C）
//
// 本文档是固件引脚的"宪法"：其他模块一律经 src/board/board_selected.h
// 拿到这里的 GAGA_* 定义，禁止散落裸 GPIO 号 / I2C 地址（AGENTS.md 铁律）。
// 引脚数值的原始权威 = 微雪官方 BSP（waveshare__esp32_s3_touch_amoled_1_75c）
// + reference/ 里的原理图，禁止猜。
//
// 板级电气约束（实测记录，改动前必读）：
//   ① ES8311 REG0E 下电序列（约束⑤）：es8311_open 会把 ADC 重新上电，
//      与 ES7210 共用 DIN 线（GPIO10）会互相顶——每次 spkOpen/micOpen 后必须
//      补写 REG0E=0x00 让 ASDOUT 高阻让线（见 audio/AudioPipe.cpp 约束⑤，
//      .agents/bug-analysis-0923.md）。背景：1.75C 原理图 ES8311 ASDOUT 与
//      ES7210 DOUT 并到同一根 DIN，不同时只能有一边驱动。
//   ② 功放 PA（NS4150B CTRL）高有效：PA=1 使能、PA=0 断电省电。
//      上电要用 INPUT_OUTPUT 模式（纯输出时 gpio_get_level 输入通路断开
//      恒 0，2026-09-23 曾误判"PA 没驱动"，见 ADR-023）。
//   ③ MCLK=GPIO16（1.75C 原理图接线）：决定性实验——MCLK=42 时 ES7210
//      采集全零（DIN10 全程无翻转），切 16 立刻出数（2026-09-23 实测，
//      docs/hardware.md §引脚定义）。1.75 旧板资料里"MCLK=42 唯一一路"的
//      说法对本机不适用；若未来支持 1.75 旧板，此脚是差异点。
//   ④ BOOT 键（GPIO0）低有效，同时是 RTC IO：既是右下物理键，也是
//      深睡 ext0 唤醒源（低电平唤醒）。
// ============================================================================

#include "driver/gpio.h"

// BSP 头（managed_components/waveshare__esp32_s3_touch_amoled_1_75c）：
// BSP_I2S_* / BSP_POWER_AMP_IO 与 BSP_LCD_H_RES/V_RES 的数值权威
#include "bsp/esp32_s3_touch_amoled_1_75c.h"
#include "bsp/display.h"

// ---------------------------------------------------------------- 音频 I2S ----
// re-export BSP 宏（不重复写数字，见文件头③的 MCLK 实验记录）
#define GAGA_PIN_I2S_MCLK  BSP_I2S_MCLK
#define GAGA_PIN_I2S_BCLK  BSP_I2S_SCLK
#define GAGA_PIN_I2S_WS    BSP_I2S_LCLK
#define GAGA_PIN_I2S_DOUT  BSP_I2S_DOUT
#define GAGA_PIN_I2S_DIN   BSP_I2S_DSIN
// 功放使能（NS4150B CTRL，高有效，见文件头②）
#define GAGA_PIN_PA        BSP_POWER_AMP_IO

// ---------------------------------------------------------------- 按键/中断 ----
// BOOT 键 = 物理右下键（低有效，实测见 docs/hardware.md）
#define GAGA_PIN_BTN_BOOT  GPIO_NUM_0
// QMI8658 INT1（QMI_INT1，RTC IO，可深睡 ext1 唤醒）——当前固件未接中断，
// 事件靠轮询；中断驱动是待立项的功耗优化项（Qmi8658.h 头注）
#define GAGA_PIN_IMU_INT1  GPIO_NUM_21

// ------------------------------------------------------------ 共享 I2C 器件 ----
// 三条地址都在 BSP 初始化好的共享总线上，访问约定：持 i2cMutex() 递归锁
#define GAGA_I2C_ADDR_AXP2101   0x34   // 电源管理：PWR 键 + 电量 + 充电
#define GAGA_I2C_ADDR_QMI8658   0x6B   // 六轴 IMU（QMI8658_L_SLAVE_ADDRESS）
#define GAGA_I2C_ADDR_PCF85063  0x51   // RTC（本机实机缺席，软时钟兜底）
