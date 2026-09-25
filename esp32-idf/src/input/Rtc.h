#pragma once

#include <cstdint>

// PCF85063 RTC（共享 I2C 总线 0x51，经 AXP2101 电池供电，断电走时）
// 寄存器语义对照官方 SensorLib SensorPCF85063.hpp（勿猜）：
//   SEC_REG=0x04 起 7 字节 BCD：秒 分 时 日 星期 月 年(2000+)
//   秒寄存器 bit7=OS（振荡器停摆标志）：置位 = 时钟不可信
// 状态栏时间来源；服务端信令带 ts 时自动对时（protocol.md §3 envelope）。
namespace gaga {

struct RtcTime {
    bool    valid;   // OS 标志清零且读取成功
    int     year;    // 20xx
    int     month;   // 1-12
    int     day;     // 1-31
    int     hour;    // 0-23
    int     minute;  // 0-59
    int     second;
};

// 初始化：探测 PCF85063；芯片不在也返回 true（转软件时钟兜底）
bool rtcInit();
// 读当前时间：硬件 RTC 优先（断电走时），缺席/无效时回落软件时钟
bool rtcGetTime(RtcTime* out);
// Unix 秒 → 写 RTC（服务端 ts 对时）；内部自己做历法换算
bool rtcSetUnix(int64_t unixSec);

}  // namespace gaga
