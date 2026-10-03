#pragma once

#include <cstdint>

#include "driver/i2c_master.h"

#include "boards/common/board_selected.h"   // GAGA_I2C_ADDR_QMI8658 地址权威
#include "motion/Imu.h"

// QMI8658C 六轴 IMU 寄存器级驱动（共享 I2C 总线，持 i2cMutex，实现 motion/Imu.h）
// 只开本项目用到的能力：加速度计（4G @500Hz）+ 陀螺仪（±1024dps @448Hz）。
// 陀螺/磁力/FIFO 一概不碰——唤醒场景用不上，少配少错。
//
// ⚠️ 型号能力边界（QST 官方手册，.agents/decisions.md ADR-072）：**C 无硬件 Tap
// 敲击引擎**（Tap/Pedometer/Any/No/Sig-Motion 检测均为 QMI8658A 专有，C 的
// 手册里运动相关只有 Wake on Motion §9）。原照抄 SensorLib 的 Tap 引擎代码
// 已按死代码删除。双击唤醒若要做 = 软件尖峰判据（'A' 流 + 09-25 数据集④敲壳
// 已验证叩屏尖峰可见）。
// 本机注意（docs/hardware.md）：INT1 引到 GAGA_PIN_IMU_INT1（board_config.h，
// RTC IO，可深睡 ext1 唤醒）。当前固件未接中断，事件靠轮询；中断驱动
// （走 C 的 Wake on Motion）是待立项的功耗优化项。
namespace gaga {

class Qmi8658 : public Imu {
public:
    bool begin(i2c_master_bus_handle_t bus) override;  // whoami 校验 + accel/gyro 配置

    // 功耗档位（2026-09-26 省电）：Active = accel 500Hz + gyro 448Hz（调参/亮屏）；
    // Idle = accel 94Hz + gyro 关（息屏挂脖——摇动判据只需加速度；94Hz 对 4 样本
    // ×11ms 摆检测绰绰有余，传感器电流约减半。⚠️ 曾出"摇不动"回归：500Hz 标定
    // 的阈值在 94Hz 下削峰凑不齐，降档须连阈值一起重标定，见 dev-log 09-25）
    void setPowerProfile(bool active) override;

    // 读当前加速度（单位 g，屏幕法线 = Z）。false = I2C 失败
    bool readAccel(float* x, float* y, float* z) override;

    // 读角速度（单位 dps）。false = I2C 失败。摇动 v3 调参用
    bool readGyro(float* x, float* y, float* z) override;

    bool ready() const override { return ready_; }

private:
    bool wr(uint8_t reg, uint8_t val);
    int  rd(uint8_t reg);
    bool rdN(uint8_t reg, uint8_t* buf, int n);  // 连续读（burst）

    bool readAccelRaw(int16_t* x, int16_t* y, int16_t* z);

    i2c_master_dev_handle_t dev_ = nullptr;
    bool ready_ = false;
    bool profileActive_ = true;   // 当前功耗档（true=Active）

    static constexpr uint8_t ADDR = GAGA_I2C_ADDR_QMI8658;  // QMI8658_L_SLAVE_ADDRESS
    // 寄存器表（QMI8658Constants.h）
    static constexpr uint8_t REG_WHOAMI   = 0x00;  // =0x05
    static constexpr uint8_t REG_CTRL1    = 0x02;
    static constexpr uint8_t REG_CTRL2    = 0x03;  // accel 量程<<4 | ODR
    static constexpr uint8_t REG_CTRL3    = 0x04;  // gyro 量程<<4 | ODR（同 CTRL2 布局）
    static constexpr uint8_t REG_CTRL7    = 0x08;  // bit0=aEN bit1=gEN bit7=SyncSample
    static constexpr uint8_t REG_AX_L     = 0x35;  // 加速度 6 字节起始
    static constexpr uint8_t REG_GX_L     = 0x3B;  // 陀螺仪 6 字节起始（SensorLib 常量表）
    // 4G 量程 → 32768 LSB 对应 4g
    static constexpr float SCALE = 4.0f / 32768.0f;
    // 陀螺 ±1024dps（CTRL3 量程码 6）→ 手腕摇/绳子甩都不会饱和
    static constexpr float GSCALE = 1024.0f / 32768.0f;
};

}  // namespace gaga
