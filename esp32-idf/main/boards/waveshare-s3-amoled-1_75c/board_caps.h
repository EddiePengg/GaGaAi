#pragma once

// 板级能力清单（编译期宏）：BoardCaps 的运行时来源（board_175c.cpp）。
// 新增能力 = 这里加宏 + BoardCaps 加字段；无某能力的板子把对应宏定义为 0。

#include "board_config.h"   // BSP_LCD_H_RES/V_RES

// 外设能力（1.75C 全配）
#define GAGA_HAS_TOUCH    1   // CST9217 触摸（自持 indev，坐标随旋转变换）
#define GAGA_HAS_IMU      1   // QMI8658C 六轴（摇动/抬手/自动转向）
#define GAGA_HAS_PMIC     1   // AXP2101（PWR 键 + 电池三件套）
#define GAGA_HAS_RTC      1   // PCF85063 焊盘（本机缺席，软时钟兜底仍算能力）
#define GAGA_HAS_DISPLAY  1   // CO5300 AMOLED 圆屏（QSPI）
#define GAGA_HAS_KWS      1   // 语音唤醒（esp-sr WakeNet，"你好小智"）

// 屏幕分辨率（re-export BSP 宏，UI 布局禁止再写 466 裸值）
#define GAGA_LCD_H_RES    BSP_LCD_H_RES
#define GAGA_LCD_V_RES    BSP_LCD_V_RES
