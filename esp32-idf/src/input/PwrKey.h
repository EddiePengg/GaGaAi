#pragma once

// PWR 键（物理右上键）读取：AXP2101（0x34）PKEY 正负边沿 IRQ 轮询。
// INTSTS2(0x49) bit1（negative edge）= 按下，bit0（positive edge）= 松开
// （位号对照 XPowersLib XPowersParams.hpp：PKEY_POSITIVE=_BV(8)/PKEY_NEGATIVE=_BV(9)，
//  经 getIrqStatus 的 (INTSTS1<<16)|(INTSTS2<<8)|INTSTS3 映射落到 INTSTS2 的 bit0/bit1；
//  与官方 1.75C 仓库 01_AXP2101 示例的 INTSTS/IRQ 位定义一致）。
// 边沿事件合成"当前是否按下"的电平状态喂给 ButtonHandler（长按时长由 ButtonHandler
// 的 400ms 阈值统一裁决——远低于 AXP2101 的 6s 硬件强制关机线，安全）。
//
// 历史备注：曾试过 TCA9554/EXIO4 电平直读（HARDWARE_REFERENCE 的 SYS_OUT 路径），
// 本机 I2C 扫描 0x20 无应答（0x18/0x34/0x40/0x5A/0x6B），IO 扩展器不在本机总线上，
// 该路径 2026-09-23 移除。
namespace gaga {

// 初始化：探测 AXP2101(0x34)。返回 false = 不可用（按键保持禁用占位，不致命）。
bool pwrKeyInit();

// 当前是否按下（供 ButtonHandler 自定义 reader 轮询，~10ms 节奏）
bool pwrKeyPressed();

// 电池状态（状态栏用；寄存器语义对照官方 XPowersLib XPowersAXP2101.tpp）：
//   电量 = BAT_PERCENT_DATA(0xA4) 0~100；在位 = STATUS1(0x00) bit3
//   充电 = STATUS2(0x01) bits7:5 == 0x01（0x02=放电 0x00=待机）
//   USB  = STATUS1(0x00) bit5 VBUS good（插着电即使充满也不置 charging 位
//          ——深睡判据必须用这个，不能用 charging：满电插着 USB 也会被误睡）
struct PwrBattery {
    bool present;    // 电池在位
    bool charging;   // 充电中
    bool usb;        // USB/VBUS 在位（外接供电）
    int  percent;    // 0~100；读不到 = -1
};
bool pwrGetBattery(PwrBattery* out);

}  // namespace gaga
