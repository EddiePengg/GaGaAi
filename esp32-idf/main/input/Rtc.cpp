#include "Rtc.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include <cstdio>

#include "boards/common/Board.h"
#include "boards/common/board_selected.h"   // GAGA_I2C_ADDR_* 地址权威
#include "compat.h"

namespace gaga {

static const char* TAG = "gaga.rtc";

// PCF85063（SensorLib REG/PCF85063Constants.h 对照；地址 = GAGA_I2C_ADDR_PCF85063）：
// 秒寄存器 0x04 起 7 字节 BCD
static constexpr uint8_t PCF85063_ADDR    = GAGA_I2C_ADDR_PCF85063;
static constexpr uint8_t PCF85063_SEC_REG = 0x04;
static constexpr uint8_t PCF85063_OS_BIT  = 0x80;  // 秒寄存器 bit7：振荡器停摆

static i2c_master_dev_handle_t s_rtc = nullptr;

// 软件时钟兜底（2026-09-24 真机实锤：本机 I2C 扫描只有 0x18/0x34/0x40/0x5A/0x6B，
// PCF85063 @0x51 无应答——时钟芯片缺席/未焊）。信令 ts 对时写进 RAM 基准，
// 读取时按 millis 递增推算；硬件 RTC 在位则硬件优先（断电走时能力保留）。
static int64_t  s_baseUnix  = 0;   // 对时时刻的 Unix 秒（0 = 从未对时）
static uint32_t s_baseMs    = 0;   // 对时时刻的 millis

// BCD ↔ 十进制（PCF85063 时间寄存器是 BCD 编码，每字节两位数字）
static inline uint8_t bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static inline uint8_t dec2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }
static void unixToCivil(int64_t s, RtcTime* t);
static int64_t civilToUnix(const RtcTime* t);

// I2C 连续读/写 RTC 寄存器（首字节是寄存器地址；50ms 超时，调用方持锁）
static bool rtcReadRegs(uint8_t reg, uint8_t* buf, size_t len) {
    return i2c_master_transmit_receive(s_rtc, &reg, 1, buf, len, 50) == ESP_OK;
}

static bool rtcWriteRegs(uint8_t reg, const uint8_t* buf, size_t len) {
    uint8_t tmp[8];
    if (len + 1 > sizeof(tmp)) return false;
    tmp[0] = reg;
    memcpy(&tmp[1], buf, len);
    return i2c_master_transmit(s_rtc, tmp, len + 1, 50) == ESP_OK;
}

// 初始化 RTC：探测/挂 PCF85063；芯片不在就静默转软件时钟（不算失败）
bool rtcInit() {
    if (s_rtc) return true;
    i2cLock();
    i2c_master_bus_handle_t bus = Board::instance().i2cBus();
    if (bus == nullptr) {
        i2cUnlock();
        ESP_LOGW(TAG, "I2C 总线不可用，RTC 禁用");
        return false;
    }
    if (i2c_master_probe(bus, PCF85063_ADDR, 50) != ESP_OK) {
        i2cUnlock();
        ESP_LOGW(TAG, "PCF85063 无应答，走软件时钟（信令 ts 对时）");
        return true;  // 软件时钟可用，不算初始化失败
    }
    i2c_device_config_t cfg = {};
    cfg.device_address = PCF85063_ADDR;
    cfg.scl_speed_hz   = 400000;
    const bool ok = i2c_master_bus_add_device(bus, &cfg, &s_rtc) == ESP_OK;
    i2cUnlock();
    if (!ok) {
        ESP_LOGE(TAG, "PCF85063 挂总线失败");
        return false;
    }
    RtcTime t{};
    if (rtcGetTime(&t)) {
        ESP_LOGI(TAG, "RTC %s %04d-%02d-%02d %02d:%02d:%02d",
                 t.valid ? "走时中" : "未校时(OS)",
                 t.year, t.month, t.day, t.hour, t.minute, t.second);
        if (t.valid && s_baseUnix == 0) {
            // 播种软件锚点（2026-09-27）：rtcHmAt（卡片时间）只认软件基准，
            // 不播种的话重启后第一张卡要等第一条心跳才有时间
            s_baseUnix = civilToUnix(&t);
            s_baseMs   = millis();
            ESP_LOGI(TAG, "软件锚点已从硬件 RTC 播种 unix=%lld",
                     static_cast<long long>(s_baseUnix));
        }
    }
    return true;
}

// 读当前时间：硬件 RTC 优先（断电走时），缺席/无效回落软件时钟
bool rtcGetTime(RtcTime* out) {
    if (!out) return false;
    // 硬件 RTC 优先（断电走时）
    if (s_rtc) {
        uint8_t b[7] = {};
        i2cLock();
        const bool ok = rtcReadRegs(PCF85063_SEC_REG, b, 7);  // 0x04 起 7 字节一次读出
        i2cUnlock();
        if (ok) {
            out->valid  = (b[0] & PCF85063_OS_BIT) == 0;  // OS=1 表示曾停振，时间不可信
            out->second = bcd2dec(b[0] & 0x7F);
            out->minute = bcd2dec(b[1] & 0x7F);
            out->hour   = bcd2dec(b[2] & 0x3F);   // 24 小时制
            out->day    = bcd2dec(b[3] & 0x3F);
            out->month  = bcd2dec(b[5] & 0x1F);
            out->year   = bcd2dec(b[6]) + 2000;
            if (out->valid) return true;
        }
    }
    // 软件时钟：对时基准 + millis 递增推算（对时前返回 false → 状态栏显示 --:--）
    if (s_baseUnix == 0) return false;
    const int64_t now = s_baseUnix + (millis() - s_baseMs) / 1000;
    unixToCivil(now, out);
    out->valid = true;
    return true;
}

// Unix 秒 → 年月日时分秒（civil_from_days 算法，1970 纪元，无闰秒）
static void unixToCivil(int64_t s, RtcTime* t) {
    // 时区（2026-09-26 修复）：服务器 envelope 的 ts 是 Unix 秒 = UTC。
    // 此前直接换算，东八区显示慢 8 小时——"每次时间都不对"的根因。
    // 写死 UTC+8：本产品用户群在中国，出海时再做成可配置。
    s += 8 * 3600;
    // 先把"天"和"当天剩下的秒"拆开
    int64_t days = s / 86400;
    int64_t rem  = s % 86400;
    if (rem < 0) { rem += 86400; days -= 1; }  // 负时间戳（1970 前）的余数修正
    t->hour   = static_cast<int>(rem / 3600);
    t->minute = static_cast<int>((rem % 3600) / 60);
    t->second = static_cast<int>(rem % 60);
    // Howard Hinnant civil_from_days：天数 → 年月日
    days += 719468;
    const int64_t era  = (days >= 0 ? days : days - 146096) / 146097;
    const uint64_t doe = static_cast<uint64_t>(days - era * 146097);
    const uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y    = static_cast<int64_t>(yoe) + era * 400;
    const uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint64_t mp  = (5 * doy + 2) / 153;
    const uint64_t d   = doy - (153 * mp + 2) / 5 + 1;
    const uint64_t m   = mp < 10 ? mp + 3 : mp - 9;
    t->year  = static_cast<int>(y + (m <= 2 ? 1 : 0));
    t->month = static_cast<int>(m);
    t->day   = static_cast<int>(d);
}

// 年月日时分秒 → Unix 秒（Howard Hinnant days_from_civil，unixToCivil 的逆）。
// 用途：开机从硬件 RTC 播种软件基准（否则重启后第一张消息卡的时间要等
// 第一条心跳才显示——状态栏读芯片所以是对的，卡片却没时间，割裂）
static int64_t civilToUnix(const RtcTime* t) {
    int64_t y = t->year;
    const int64_t m = t->month;
    const int64_t d = t->day;
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;                     // [0, 399]
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = era * 146097 + doe - 719468;
    return days * 86400 + t->hour * 3600 + t->minute * 60 + t->second;
}

// 对时：软件基准必更（millis 推算的起点），有硬件 RTC 再把芯片也写了
bool rtcSetUnix(int64_t unixSec) {
    // 软件时钟基准无论有没有硬件 RTC 都更新（对时即校准 millis 推算起点）
    s_baseUnix = unixSec;
    s_baseMs   = millis();
    if (!s_rtc) {
        ESP_LOGI(TAG, "软件时钟已对时 unix=%lld（无硬件 RTC）",
                 static_cast<long long>(unixSec));
        return true;
    }
    RtcTime t{};
    unixToCivil(unixSec, &t);
    uint8_t b[7];
    b[0] = dec2bcd(static_cast<uint8_t>(t.second));      // OS 位清零：时钟可信
    b[1] = dec2bcd(static_cast<uint8_t>(t.minute));
    b[2] = dec2bcd(static_cast<uint8_t>(t.hour));
    b[3] = dec2bcd(static_cast<uint8_t>(t.day));
    // 星期寄存器仅状态栏不用，SensorLib 以 Zeller 算——写 0 安全（BCD 0 合法）
    b[4] = 0;
    b[5] = dec2bcd(static_cast<uint8_t>(t.month));
    b[6] = dec2bcd(static_cast<uint8_t>(t.year % 100));
    i2cLock();
    const bool ok = rtcWriteRegs(PCF85063_SEC_REG, b, 7);
    i2cUnlock();
    if (ok) {
        ESP_LOGI(TAG, "RTC 已对时 %04d-%02d-%02d %02d:%02d:%02d",
                 t.year, t.month, t.day, t.hour, t.minute, t.second);
    }
    return ok;
}

// 设备开机毫秒 → 墙钟 "HH:MM"（卡片发送时刻显示，2026-09-27）。
// 用软件对时基准换算（不是读芯片——要的是"那一刻"）：该时刻 Unix =
// 基准 Unix +（该时刻 uptime - 基准 uptime）/1000。有符号差容忍对时
// 在卡片创建之后发生（时钟被校正过的历史卡也能换算），millis 回绕由
// uint32 减法自然处理。从未对时返回 false，调用端省略时间。
bool rtcHmAt(uint32_t uptimeMs, char* buf, size_t bufLen) {
    if (s_baseUnix == 0) return false;
    const int32_t dt = static_cast<int32_t>(uptimeMs - s_baseMs);
    const int64_t unixSec = s_baseUnix + dt / 1000;
    RtcTime t{};
    unixToCivil(unixSec, &t);
    snprintf(buf, bufLen, "%02d:%02d", t.hour, t.minute);
    return true;
}

}  // namespace gaga
