#pragma once

#include <cstdint>

#include "driver/i2c_master.h"

// QMI8658 六轴 IMU 寄存器级驱动（I2C 0x6B，共享总线，持 i2cMutex）
// 只开本项目用到的能力：加速度计（4G @500Hz）+ 硬件 Tap 敲击引擎（双击）。
// 陀螺/磁力/计步/FIFO 一概不碰——唤醒场景用不上，少配少错。
//
// 寄存器序列全部抄自官方 SensorLib（reference/waveshare-1.75C-repo）：
//   SensorQMI8658.hpp configTap()/enableTap()/writeCommand() + QMI8658Constants.h
// 本机注意（docs/hardware.md）：IMU 中断脚未引出 → 不用 INT，事件靠轮询 STATUS1。
namespace gaga {

class Qmi8658 {
public:
    bool begin(i2c_master_bus_handle_t bus);  // whoami 校验 + accel + tap 引擎配置

    // 读当前加速度（单位 g，屏幕法线 = Z）。false = I2C 失败
    bool readAccel(float* x, float* y, float* z);

    // 读角速度（单位 dps）。false = I2C 失败。摇动 v3 调参用
    bool readGyro(float* x, float* y, float* z);

    // 轮询敲击事件：STATUS1 bit10（读清除）。返回 1=有事件（再查 tapAxis）
    // 0=无  -1=I2C 失败
    int  tapEvent();
    int  tapStatusRaw();  // 原始 TAP_STATUS 寄存器值（调试用）
    void dumpTapDebug();  // dump 所有敲击相关寄存器（调参眼睛）
    // TAP_STATUS 的敲击轴（1=X 2=Y 3=Z，0=无；诊断用）
    int  tapAxis();

    bool ready() const { return ready_; }

private:
    // CTRL9 命令通道：写 cmd → 等 STATUSINT bit7 CmdDone → 写 ACK 清标志
    bool command(uint8_t cmd);

    bool wr(uint8_t reg, uint8_t val);
    int  rd(uint8_t reg);
    bool rdN(uint8_t reg, uint8_t* buf, int n);  // 连续读（burst）

    bool readAccelRaw(int16_t* x, int16_t* y, int16_t* z);

    i2c_master_dev_handle_t dev_ = nullptr;
    bool ready_ = false;

    static constexpr uint8_t ADDR = 0x6B;   // QMI8658_L_SLAVE_ADDRESS
    // 寄存器表（QMI8658Constants.h）
    static constexpr uint8_t REG_WHOAMI   = 0x00;  // =0x05
    static constexpr uint8_t REG_CTRL1    = 0x02;
    static constexpr uint8_t REG_CTRL2    = 0x03;  // accel 量程<<4 | ODR
    static constexpr uint8_t REG_CTRL3    = 0x04;  // gyro 量程<<4 | ODR（同 CTRL2 布局）
    static constexpr uint8_t REG_CTRL7    = 0x08;  // bit0=aEN bit1=gEN bit7=SyncSample
    static constexpr uint8_t REG_CTRL8    = 0x09;  // bit0=Tap 引擎使能
    static constexpr uint8_t REG_CTRL9    = 0x0A;  // 命令通道
    static constexpr uint8_t REG_CAL1_L   = 0x0B;  // Tap 参数窗口（4 寄存器 8 字节）
    static constexpr uint8_t REG_CAL1_H   = 0x0C;
    static constexpr uint8_t REG_CAL2_L   = 0x0D;
    static constexpr uint8_t REG_CAL2_H   = 0x0E;
    static constexpr uint8_t REG_CAL3_L   = 0x0F;
    static constexpr uint8_t REG_CAL3_H   = 0x10;
    static constexpr uint8_t REG_CAL4_L   = 0x11;
    static constexpr uint8_t REG_CAL4_H   = 0x12;
    static constexpr uint8_t REG_STATUSINT = 0x2D; // bit7=CmdDone bit0=CmdErr
    static constexpr uint8_t REG_STATUS1  = 0x2F;  // bit10=Tap 事件（读清除）
    static constexpr uint8_t REG_AX_L     = 0x35;  // 加速度 6 字节起始
    static constexpr uint8_t REG_GX_L     = 0x3B;  // 陀螺仪 6 字节起始（SensorLib 常量表）
    static constexpr uint8_t REG_TAP_STATUS = 0x59;
    static constexpr uint8_t CMD_CONFIGURE_TAP = 0x0C;
    static constexpr uint8_t CMD_ACK      = 0x00;  // SensorLib CTRL_CMD_ACK：写 0 清 CmdDone（真机实锤：0x04 清不掉，命令假报失败）
    // 4G 量程 → 32768 LSB 对应 4g
    static constexpr float SCALE = 4.0f / 32768.0f;
    // 陀螺 ±1024dps（CTRL3 量程码 6）→ 手腕摇/绳子甩都不会饱和
    static constexpr float GSCALE = 1024.0f / 32768.0f;
};

}  // namespace gaga
