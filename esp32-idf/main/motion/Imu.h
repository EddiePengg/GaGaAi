#pragma once

#include "driver/i2c_master.h"

// IMU 抽象（board 层重构引入，先例照 net/Link.h）：唤醒检测/自动转向等
// 业务只认这个接口，不关心底下是 QMI8658 还是别的六轴——换 IMU 只换实现类。
namespace gaga {

class Imu {
public:
    virtual ~Imu() = default;

    // 挂共享 I2C 总线初始化（whoami 校验 + 传感器配置）；false = 缺席
    virtual bool begin(i2c_master_bus_handle_t bus) = 0;

    // 功耗档位：true = Active（调参/亮屏），false = Idle（息屏省电档）
    virtual void setPowerProfile(bool active) = 0;

    // 读加速度（单位 g，屏幕法线 = Z）。false = I2C 失败
    virtual bool readAccel(float* x, float* y, float* z) = 0;

    // 读角速度（单位 dps）。false = I2C 失败
    virtual bool readGyro(float* x, float* y, float* z) = 0;

    virtual bool ready() const = 0;
};

}  // namespace gaga
