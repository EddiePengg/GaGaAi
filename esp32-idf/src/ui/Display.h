#pragma once

#include <cstdint>

#include "lvgl.h"
#include "esp_lcd_types.h"

// 显示拉起（为什么不直接用 bsp_display_start()：它不吐 panel 句柄，而
// 息屏必须拿到 panel 去发 DISPOFF——AMOLED 省电的硬需求，ADR-016）。
// 本模块用 BSP 的公开件（bsp_display_new / bsp_touch_new）+ esp_lv_adapter
// 亲手组装等价链路，逻辑逐项对照 BSP 的 bsp_display_start_with_config()。
namespace gaga {

// 初始化显示 + LVGL 任务 + 触摸；返回 lv_display_t*（失败返回 nullptr）
lv_display_t* displayStart();

// 息屏/亮屏：DISPOFF（AMOLED 显示驱动断电，黑像素零功耗；唤醒快）。
// 不发 SLPIN——co5300 的 SLPIN 在有 RST 脚时进 DSTBON 深睡，唤醒要
// reset+完整初始化（600ms 级延时，真机"按下好几秒才亮"的根因）。
void displaySleep(bool sleep);
// 查询是否息屏（只回内存标志，不碰硬件）
bool displayIsSleeping();

// 亮度 10~100%（ADR-031 用户设置）：CO5300 的 0x51 寄存器（BSP 函数，
// 自组链路同样可用——bsp_display_new 会登记静态 io_handle）。
// 纯 QSPI 参数命令，内部 SPI 事务自锁，无 LVGL 锁要求。
void displaySetBrightness(int percent);

// 屏幕内容旋转（自动转向 2026-09-25）：deg ∈ {0,90,180,270}。
// LVGL 原生软件旋转（屏是正方形 466×466，无尺寸问题）；
// 触摸坐标在本模块内按同一角度变换（自持 indev，见 touchReadCb）。
void displaySetRotation(int deg);
int  displayGetRotation();

// LVGL 互斥锁（任何 lv_* 调用必须包裹）
// 抢锁带超时；false = 没抢到，本次刷新应放弃
bool displayLock(uint32_t timeoutMs);
// 还锁，与 displayLock 成对
void displayUnlock();

}  // namespace gaga
