#include "PwrKey.h"

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

#include "compat.h"

namespace gaga {

static const char* TAG = "gaga.pwrkey";

// AXP2101（共享 I2C 总线，0x34）PKEY 边沿 IRQ
static constexpr uint8_t  AXP2101_ADDR    = 0x34;
static constexpr uint8_t  AXP2101_INTEN2  = 0x41;  // IRQ 使能 2
static constexpr uint8_t  AXP2101_INTSTS2 = 0x49;  // IRQ 状态 2（写 1 清除）
static constexpr uint8_t  AXP2101_STATUS1 = 0x00;  // bit3 = 电池在位
static constexpr uint8_t  AXP2101_STATUS2 = 0x01;  // bits7:5 充放电状态
static constexpr uint8_t  AXP2101_BAT_PCT = 0xA4;  // 电量 0~100
static constexpr uint8_t  AXP2101_INTSTS2_PKEY_POS = 0x01;  // bit0 = positive edge（松开）
static constexpr uint8_t  AXP2101_INTSTS2_PKEY_NEG = 0x02;  // bit1 = negative edge（按下）

static i2c_master_dev_handle_t s_axp      = nullptr;
static bool                    s_pressed  = false;  // 边沿合成的当前电平

// 读/写 AXP2101 单字节寄存器（50ms 超时；调用方负责持 i2c 锁）
static bool axp2101Read(uint8_t reg, uint8_t* val) {
    return i2c_master_transmit_receive(s_axp, &reg, 1, val, 1, 50) == ESP_OK;
}

static bool axp2101Write(uint8_t reg, uint8_t val) {
    const uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_axp, buf, sizeof(buf), 50) == ESP_OK;
}

// 初始化 PWR 键：探测/挂载 AXP2101，开 PKEY 正负边沿 IRQ，清历史挂起
bool pwrKeyInit() {
    if (s_axp) return true;
    i2cLock();  // codec 寄存器序列与轮询互斥（compat.h）
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == nullptr) {
        i2cUnlock();
        ESP_LOGW(TAG, "I2C 总线不可用，PWR 键禁用占位");
        return false;
    }
    if (i2c_master_probe(bus, AXP2101_ADDR, 50) != ESP_OK) {
        i2cUnlock();
        ESP_LOGW(TAG, "AXP2101 @0x34 无应答，PWR 键禁用占位");
        return false;
    }
    i2c_device_config_t cfg = {};
    cfg.device_address = AXP2101_ADDR;
    cfg.scl_speed_hz   = 400000;  // 与 BSP 总线一致
    if (i2c_master_bus_add_device(bus, &cfg, &s_axp) != ESP_OK) {
        i2cUnlock();
        ESP_LOGE(TAG, "AXP2101 设备挂总线失败");
        return false;
    }
    // 使能 PKEY 正/负边沿 IRQ（RMW，不动其他位）
    uint8_t en = 0;
    bool ok = axp2101Read(AXP2101_INTEN2, &en) &&
              axp2101Write(AXP2101_INTEN2,
                           en | AXP2101_INTSTS2_PKEY_POS | AXP2101_INTSTS2_PKEY_NEG);
    // 清掉历史挂起（写 1 清除）
    ok = axp2101Write(AXP2101_INTSTS2, 0xFF) && ok;
    i2cUnlock();
    if (!ok) {
        ESP_LOGW(TAG, "AXP2101 IRQ 初始化失败，PWR 键禁用占位");
        return false;
    }
    s_pressed = false;
    ESP_LOGI(TAG, "PWR 键就绪（AXP2101 PKEY 边沿 IRQ 轮询，INTEN2 0x%02X→0x%02X）",
             en, en | 0x03);
    return true;
}

// 轮询 INTSTS2 边沿位，合成"当前是否按下"的电平（边沿事件 → 电平状态）
bool pwrKeyPressed() {
    if (s_axp == nullptr) return false;
    i2cLock();
    uint8_t sts = 0;
    if (!axp2101Read(AXP2101_INTSTS2, &sts)) {
        i2cUnlock();
        return s_pressed;  // 读失败：沿用上次电平，不误判成松开
    }
    const uint8_t pkeyBits = sts & (AXP2101_INTSTS2_PKEY_POS | AXP2101_INTSTS2_PKEY_NEG);
    if (pkeyBits != 0) {
        // 清掉已消费的位
        axp2101Write(AXP2101_INTSTS2, pkeyBits);
        // PKEY 按下 = 信号拉低 = negative edge；松开 = positive edge。
        // 同一次轮询两边沿齐现（<10ms 的极速点按）时顺序不可知，按松开优先落定
        // （漏记一次超短点按，可接受；人类按键 >> 10ms）
        const bool before = s_pressed;
        if (pkeyBits & AXP2101_INTSTS2_PKEY_NEG) s_pressed = true;
        if (pkeyBits & AXP2101_INTSTS2_PKEY_POS) s_pressed = false;
        ESP_LOGI(TAG, "PKEY 边沿 sts=0x%02X：%s → %s", pkeyBits,
                 before ? "按下" : "松开", s_pressed ? "按下" : "松开");
    }
    i2cUnlock();
    return s_pressed;
}

// 读电池三件套（在位/充电/电量）拼成快照；读失败的字段留默认值
bool pwrGetBattery(PwrBattery* out) {
    if (!out || s_axp == nullptr) return false;
    out->present  = false;
    out->charging = false;
    out->usb      = false;
    out->percent  = -1;
    uint8_t st1 = 0, st2 = 0, pct = 0;
    i2cLock();
    const bool ok1 = axp2101Read(AXP2101_STATUS1, &st1);
    const bool ok2 = axp2101Read(AXP2101_STATUS2, &st2);
    bool ok3 = false;
    out->present = ok1 && (st1 & 0x08);  // STATUS1 bit3 = 电池在位
    out->usb     = ok1 && (st1 & 0x20);  // STATUS1 bit5 = VBUS good（官方例程同位）
    if (out->present) {
        ok3 = axp2101Read(AXP2101_BAT_PCT, &pct);  // 只有在位时电量寄存器才可信
    }
    i2cUnlock();
    if (!ok1 && !ok2) return false;
    // STATUS2 bits7:5：0x01=充电 0x02=放电 0x00=待机（XPowersAXP2101.tpp isCharging）
    out->charging = ok2 && ((st2 >> 5) & 0x07) == 0x01;
    if (ok3 && out->present) out->percent = pct <= 100 ? pct : -1;
    return true;
}

}  // namespace gaga
