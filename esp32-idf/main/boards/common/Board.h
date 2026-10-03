#pragma once

#include <cstdint>

#include "driver/i2c_master.h"

#include "InputActions.h"   // wireInput() 的契约：应用注入的抽象输入动作集

// ============================================================================
// Board 硬件抽象基类（参考 xiaozhi-esp32 的 main/boards/common/board.h）：
// 板子是"多块硬件里的第一块"，业务代码只经 Board::instance() 拿板级能力，
// 新增板子 = 新目录 + 新子类，共享代码不动。
//
// 工厂模式照抄 xiaozhi：板子实现文件里写一次 DECLARE_BOARD(<子类>) 登记
// 静态实例，Board::instance() 全程唯一入口（app_main 装配期首调即就绪）。
// ============================================================================
namespace gaga {

// 板级能力清单（运行时镜像 board_caps.h 的编译期宏）
struct BoardCaps {
    bool     touch;     // 触摸
    bool     imu;       // IMU（摇动/抬手/自动转向）
    bool     pmic;      // 电源管理（PWR 键 + 电池）
    bool     rtc;       // RTC（缺席的板子靠软时钟兜底）
    bool     display;   // 显示
    bool     kws;       // 语音唤醒（WakeNet/esp-sr）
    uint16_t lcdHRes;   // 屏横向分辨率
    uint16_t lcdVRes;   // 屏纵向分辨率
};

class Board {
public:
    virtual ~Board() = default;

    // 板名（日志/诊断显示）
    virtual const char* name() const = 0;

    // 共享 I2C 总线句柄（codec / PMIC / IMU / RTC 全挂这条）。
    // 约定：所有总线访问必须持 i2cMutex() 递归锁（compat.h，真机踩实的
    // 互斥纪律），句柄本身创建后永不变，不必持锁取。
    virtual i2c_master_bus_handle_t i2cBus() = 0;

    // 板级能力清单（不可变，返回静态常量）
    virtual const BoardCaps& caps() const = 0;

    // 深睡唤醒源配置（esp_sleep_enable_*）。app_main 装配期调用一次，
    // 配置落在 RTC 域一直有效到真正进深睡；板子按自己的按键/中断脚使能。
    virtual void setupWakeupSources() = 0;

    // 物理输入接线：应用注入抽象动作集，板子把物理键/手势映射上去（ADR-078）。
    // 板子自行决定键位分工、是否使能某颗键（如 PMIC 键初始化失败则禁用占位）。
    virtual void wireInput(const InputActions& actions) = 0;
    // 主循环驱动按键扫描（原 appTask 里的 btn.loop()，收进板层）
    virtual void inputTick() = 0;

    // 全局唯一板实例（DECLARE_BOARD 登记，静态初始化期即就绪）
    static Board& instance() { return *instance_; }

protected:
    static Board* instance_;
};

// DECLARE_BOARD：在唯一板子实现文件（boards/<board>/board_*.cpp）里写一次，
// 把静态实例登记进 Board::instance_（xiaozhi 工厂模式同款）
#define DECLARE_BOARD(BOARD_CLASS_NAME)       \
    static BOARD_CLASS_NAME s_boardInstance;  \
    Board* Board::instance_ = &s_boardInstance;

}  // namespace gaga
