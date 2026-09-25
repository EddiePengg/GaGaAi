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

// CTRL9 命令协议（SensorLib writeCommand 同款）：
// 写 cmd → 轮询 STATUSINT 命令位 → 写 ACK → 等清零。
// 位掩码 0x81：bit7=CmdDone（writeCommand 实现）、bit0=CmdErr/部分固件 Done
// （SensorLib 常量表标 _BV(0) 与实现自相矛盾——两位都认，Err 不算成功）
bool Qmi8658::command(uint8_t cmd) {
    wr(REG_CTRL9, 0x00);                     // 预清命令状态（残留 done 会假成功）
    if (!wr(REG_CTRL9, cmd)) return false;
    for (int i = 0; i < 100; i++) {          // 上限 100ms
        const int st = rd(REG_STATUSINT);
        if (st < 0) return false;
        if (st & 0x81) {
            const bool err = (st & 0x01) != 0 && (st & 0x80) == 0;
            if (err) {
                ESP_LOGW(TAG, "CTRL9 cmd 0x%02X CmdErr（status=0x%02X）", cmd, st);
                wr(REG_CTRL9, CMD_ACK);
                return false;
            }
            if (!wr(REG_CTRL9, CMD_ACK)) return false;
            for (int k = 0; k < 100; k++) {   // 等 ACK 清掉命令位
                const int s2 = rd(REG_STATUSINT);
                if (s2 < 0) return false;
                if (!(s2 & 0x81)) return true;
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    ESP_LOGW(TAG, "CTRL9 cmd 0x%02X 超时无 Done", cmd);
    return false;
}

// 初始化：whoami → 加速度 4G@500Hz → 硬件 Tap 引擎（双击）
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
    // SensorLib begin 姿势：CTRL1 bit6（地址自动递增）
    wr(REG_CTRL1, 0x40);
    // accel：CTRL2 = 量程(4G=1)<<4 | ODR(500Hz=4) —— Tap 检测官方推荐 ≥500Hz
    wr(REG_CTRL2, (1 << 4) | 4);
    // gyro：CTRL3 = 量程(±1024dps=6)<<4 | ODR(448.4Hz=4)。摇动 v3 调参要角速度：
    // 跑步/颠簸没有手腕旋转特征，绳子甩是大幅旋转——纯加速度分不开，角速度分
    wr(REG_CTRL3, (6 << 4) | 4);
    // CTRL7 bit7 清 = 非同步采样（Tap 引擎前置条件，SensorLib disableSyncSampleMode）
    // ⚠️ CONFIGURE_TAP 命令前传感器必须全关（SensorLib configTap 姿势：
    // disableSyncSample → disableGyro → disableAccel → 写参数 → 命令 → 重开；
    // 真机 2026-09-25 实测：开着 accel 发命令，CmdDone 永不置位 → "配置失败"）
    wr(REG_CTRL7, 0x00);

    // ---- Tap 引擎参数（官方 TapDetectionExample 默认值，单位=样本@500Hz）----
    // 第一段：窗口与优先级
    //   peakWindow=20（40ms 峰窗）、priority=PRIORITY0（X>Y>Z）、
    //   tapWindow=50（100ms 首击后静默）、dTapWindow=250（500ms 双击窗）
    wr(REG_CAL1_L, 20);                       // peakWindow
    wr(REG_CAL1_H, 0);                        // priority
    wr(REG_CAL2_L, 50 & 0xFF);                // tapWindow L
    wr(REG_CAL2_H, (50 >> 8) & 0xFF);         // tapWindow H
    wr(REG_CAL3_L, 250 & 0xFF);               // dTapWindow L
    wr(REG_CAL3_H, (250 >> 8) & 0xFF);        // dTapWindow H
    wr(REG_CAL4_H, 0x01);
    const bool ok1 = command(CMD_CONFIGURE_TAP);
    // 第二段：滤波系数与阈值
    //   alpha=0.0625 gamma=0.25（7bit 定点 ×128）、peakMagThr=0.8g²、UDMTh=0.4g²
    //   （阈值编码：g² → 0.001g² 分辨率，参考 SensorLib configTap）
    wr(REG_CAL1_L, static_cast<uint8_t>(0.0625f * 128));   // alpha
    wr(REG_CAL1_H, static_cast<uint8_t>(0.25f * 128));     // gamma
    const uint16_t peak = static_cast<uint16_t>(0.8f / 0.001f);
    wr(REG_CAL2_L, peak & 0xFF);
    wr(REG_CAL2_H, (peak >> 8) & 0xFF);
    const uint16_t udm = static_cast<uint16_t>(0.4f / 0.001f);
    wr(REG_CAL3_L, udm & 0xFF);
    wr(REG_CAL3_H, (udm >> 8) & 0xFF);
    wr(REG_CAL4_H, 0x02);
    const bool ok2 = command(CMD_CONFIGURE_TAP);
    // 诊断：回读参数寄存器 + CTRL 现值（tap 排障的眼睛，真机定位用）
    ESP_LOGI(TAG, "[tap-diag] cmd1=%d cmd2=%d CAL1=%02X%02X CAL2=%02X%02X "
             "CAL3=%02X%02X CAL4=%02X%02X CTRL7=%02X CTRL8=%02X STATUSINT=%02X",
             ok1 ? 1 : 0, ok2 ? 1 : 0,
             rd(REG_CAL1_H), rd(REG_CAL1_L), rd(REG_CAL2_H), rd(REG_CAL2_L),
             rd(REG_CAL3_H), rd(REG_CAL3_L), rd(REG_CAL4_H), rd(REG_CAL4_L),
             rd(REG_CTRL7), rd(REG_CTRL8), rd(REG_STATUSINT));
    // 参数落地后才开传感器：accel（bit0）+ gyro（bit1）+ Tap 引擎（CTRL8 bit0；
    // 无 INT 引脚，事件轮询 STATUS1）。⚠️ gyro 常开 ≈ +1~3mA，v3 定型后评估取舍
    wr(REG_CTRL7, 0x03);
    wr(REG_CTRL8, 0x01);
    i2cUnlock();

    ready_ = ok1 && ok2;
    ESP_LOGI(TAG, "QMI8658 %s（accel 4G@500Hz + gyro ±1024dps，双击引擎 %s）",
             id == 0x05 ? "在线" : "缺席", ready_ ? "就绪" : "配置失败");
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

bool Qmi8658::readGyro(float* x, float* y, float* z) {
    if (!ready_) return false;
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

// 轮询 Tap 事件（STATUS1 bit10，读该寄存器即清除事件位）
// dump 所有敲击相关寄存器（调参眼睛）
void Qmi8658::dumpTapDebug() {
    if (!ready_) { ESP_LOGW(TAG, "IMU not ready"); return; }
    i2cLock();
    // 控制寄存器
    printf("[tap] CTRL1=0x%02X CTRL2=0x%02X CTRL7=0x%02X CTRL8=0x%02X\n",
           rd(REG_CTRL1), rd(REG_CTRL2), rd(REG_CTRL7), rd(REG_CTRL8));
    printf("[tap] STATUSINT=0x%02X STATUS1=0x%02X TAP_STATUS=0x%02X\n",
           rd(REG_STATUSINT), rd(REG_STATUS1), rd(REG_TAP_STATUS));
    // 敲击参数（CAL 寄存器）
    printf("[tap] CAL1=0x%02X%02X CAL2=0x%02X%02X CAL3=0x%02X%02X CAL4=0x%02X%02X\n",
           rd(REG_CAL1_H), rd(REG_CAL1_L), rd(REG_CAL2_H), rd(REG_CAL2_L),
           rd(REG_CAL3_H), rd(REG_CAL3_L), rd(REG_CAL4_H), rd(REG_CAL4_L));
    // 原始加速度
    int16_t ax, ay, az;
    if (readAccelRaw(&ax, &ay, &az)) {
        printf("[tap] accel raw: x=%d y=%d z=%d\n", ax, ay, az);
    }
    i2cUnlock();
}

// 原始 TAP_STATUS 寄存器值（调试用，不解析位）
int Qmi8658::tapStatusRaw() {
    if (!ready_) return -1;
    i2cLock();
    const int st = rd(REG_TAP_STATUS);
    i2cUnlock();
    return st;
}

// 轮询 TAP_STATUS(0x59)：bits 6:4 = 敲击类型（0=无 1=单击 2=双击）
// 读自动清除。返回 1 = 有敲击事件（不含方向和类型细节）
int Qmi8658::tapEvent() {
    if (!ready_) return 0;
    i2cLock();
    const int st = rd(REG_TAP_STATUS);
    i2cUnlock();
    if (st < 0) return -1;
    // bits 6:4：0=无敲击，非 0 = 有敲击事件
    return ((st >> 4) & 0x07) > 0 ? 1 : 0;
}

int Qmi8658::tapAxis() {
    i2cLock();
    const int t = rd(REG_TAP_STATUS);
    i2cUnlock();
    if (t < 0) return 0;
    return (t >> 4) & 0x03;
}

}  // namespace gaga
