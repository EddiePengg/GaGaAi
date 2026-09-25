#include "Rtc.h"

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

#include "compat.h"

namespace gaga {

static const char* TAG = "gaga.rtc";

// PCF85063（SensorLib REG/PCF85063Constants.h 对照）：0x51，秒寄存器 0x04 起 7 字节 BCD
static constexpr uint8_t PCF85063_ADDR    = 0x51;
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
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == nullptr) {
        i2cUnlock();
        ESP_LOGW(TAG, "I2C 总线不可用，RTC 禁用");
        return false;
    }
    if (i2c_master_probe(bus, PCF85063_ADDR, 50) != ESP_OK) {
        i2cUnlock();
        ESP_LOGW(TAG, "PCF85063 @0x51 无应答，走软件时钟（信令 ts 对时）");
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

}  // namespace gaga
