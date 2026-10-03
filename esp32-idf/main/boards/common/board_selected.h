#pragma once

// ============================================================================
// 选板入口：按 -DGAGA_BOARD_* 编译宏包含对应板目录的头文件。
//
// 选板机制刻意用编译宏 + 头文件切换，不用 xiaozhi 的 Kconfig
// CONFIG_BOARD_TYPE_* + CMake if-elseif 链（理由见 .agents/decisions.md 对应
// ADR）：PIO 下 sdkconfig 按 env 管理、Kconfig 交互弱；当前单 env，
// 宏切换零构建成本。发布期需要 CI 矩阵时再评估迁回 Kconfig，届时只动本文件。
//
// 新板 = 新增 src/boards/<board>/ 目录（board_config.h + board_caps.h +
// board_<name>.{h,cpp}）+ 这里加一个 #elif 分支 + platformio.ini 复制 env
// 并换 -D 宏（模板见 platformio.ini 注释）。
// ============================================================================

#if defined(GAGA_BOARD_WAVESHARE_1_75C)
// 微雪 ESP32-S3-Touch-AMOLED-1.75C（当前唯一在册板子）
#include "board_config.h"   // boards/waveshare-s3-amoled-1_75c（已入 INCLUDE_DIRS）
#include "board_caps.h"
#else
#error "未选板：请在 build_flags 里指定 -DGAGA_BOARD_<NAME>（见 platformio.ini）"
#endif
