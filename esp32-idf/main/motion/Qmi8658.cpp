#include "motion/Qmi8658.h"

#include <cmath>

#include "esp_log.h"

#include "compat.h"

namespace gaga {

static const char* TAG = "gaga.imu";

// I2C 单寄存器写
bool Qmi8658::wr(uint8_t reg, uint8_t val) {
    const uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev_, buf, 2, 20) == ESP_OK;
}

// I2C 单寄存器读（-1 = 失败）
int Qmi8658::rd(uint8_t reg) {
    uint8_t v = 0;
    if (i2c_master_transmit_receive(dev_, &reg, 1, &v, 1, 20) != ESP_OK) return -1;
    return v;
}

// 连续读 n 字节（地址自动递增，SensorLib begin 里 CTRL1 bit6 同款保证）
bool Qmi8658::rdN(uint8_t reg, uint8_t* buf, int n) {
    return i2c_master_transmit_receive(dev_, &reg, 1, buf, static_cast<size_t>(n), 30) == ESP_OK;
}

// 初始化：whoami → accel 4G@500Hz + gyro ±1024dps@448Hz
bool Qmi8658::begin(i2c_master_bus_handle_t bus) {
    i2cLock();
    i2c_device_config_t cfg = {};
    cfg.device_address = ADDR;
    cfg.scl_speed_hz   = 400000;
    if (i2c_master_bus_add_device(bus, &cfg, &dev_) != ESP_OK || dev_ == nullptr) {
        i2cUnlock();
        ESP_LOGE(TAG, "I2C 设备创建失败 @0x%02X", ADDR);
        return false;
    }
    const int id = rd(REG_WHOAMI);
    if (id != 0x05) {
        ESP_LOGE(TAG, "WHOAMI=0x%02X（期望 0x05），IMU 缺席", id);
        i2cUnlock();
        return false;
    }
    // CTRL1 bit6 = 地址自动递增（burst 读前置）
    wr(REG_CTRL1, 0x40);
    // accel：CTRL2 = 量程(4G=1)<<4 | ODR(500Hz=4) —— 摇动判据 v5 与 'A' 流调参
    // 的标定基准（500Hz 时 4 样本≈8ms 摆检测粒度；勿轻降，94Hz 削峰出过回归）
    wr(REG_CTRL2, (1 << 4) | 4);
    // gyro：CTRL3 = 量程(±1024dps=6)<<4 | ODR(448.4Hz=4)。摇动 v3 调参要角速度：
    // 跑步/颠簸没有手腕旋转特征，绳子甩是大幅旋转——纯加速度分不开，角速度分
    wr(REG_CTRL3, (6 << 4) | 4);
    wr(REG_CTRL7, 0x03);  // aEN|gEN 常开
    i2cUnlock();

    ready_ = true;
    ESP_LOGI(TAG, "QMI8658C 在线（accel 4G@500Hz + gyro ±1024dps@448Hz）");
    return ready_;
}

bool Qmi8658::readAccelRaw(int16_t* x, int16_t* y, int16_t* z) {
    uint8_t b[6];
    if (!rdN(REG_AX_L, b, 6)) return false;
    *x = static_cast<int16_t>(b[0] | (b[1] << 8));
    *y = static_cast<int16_t>(b[2] | (b[3] << 8));
    *z = static_cast<int16_t>(b[4] | (b[5] << 8));
    return true;
}

bool Qmi8658::readAccel(float* x, float* y, float* z) {
    if (!ready_) return false;
    int16_t rx, ry, rz;
    i2cLock();
    const bool ok = readAccelRaw(&rx, &ry, &rz);
    i2cUnlock();
    if (!ok) return false;
    *x = rx * SCALE;
    *y = ry * SCALE;
    *z = rz * SCALE;
    return true;
}

// 功耗档位切换：亮屏 Active（500Hz+gyro）/ 息屏 Idle（94Hz 无 gyro）。
// 序列照 SensorLib 姿势：改 ODR 前先关传感器（CTRL7），写完再开——
// 运行中直接写 CTRL2/3 会被芯片拒绝。
void Qmi8658::setPowerProfile(bool active) {
    if (!ready_) return;
    i2cLock();
    wr(REG_CTRL7, 0x00);                         // 全关（改参数前置条件）
    if (active) {
        wr(REG_CTRL2, (1 << 4) | 4);             // accel 4G @500Hz
        wr(REG_CTRL3, (6 << 4) | 4);             // gyro ±1024dps @448Hz
        wr(REG_CTRL7, 0x03);                     // aEN|gEN
    } else {
        wr(REG_CTRL2, (1 << 4) | 2);             // accel 4G @93.9Hz（ODR 码 2）
        wr(REG_CTRL7, 0x01);                     // 只开 accel
    }
    i2cUnlock();
    profileActive_ = active;
    ESP_LOGI(TAG, "IMU 功耗档 → %s", active ? "Active(500Hz+gyro)" : "Idle(94Hz)");
}

bool Qmi8658::readGyro(float* x, float* y, float* z) {    if (!ready_) return false;
    uint8_t b[6];
    i2cLock();
    const bool ok = rdN(REG_GX_L, b, 6);
    i2cUnlock();
    if (!ok) return false;
    *x = static_cast<int16_t>(b[0] | (b[1] << 8)) * GSCALE;
    *y = static_cast<int16_t>(b[2] | (b[3] << 8)) * GSCALE;
    *z = static_cast<int16_t>(b[4] | (b[5] << 8)) * GSCALE;
    return true;
}

}  // namespace gaga
