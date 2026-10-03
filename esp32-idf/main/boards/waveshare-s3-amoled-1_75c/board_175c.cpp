#include "board_175c.h"

#include "bsp/esp-bsp.h"    // bsp_i2c_get_handle（BSP 初始化好的共享总线）
#include "esp_sleep.h"

#include "boards/common/board_selected.h"   // board_config.h + board_caps.h（GAGA_* 权威）

namespace gaga {

DECLARE_BOARD(Board175c)

// 共享 I2C 总线（调用方持 i2cMutex，见 Board.h 约定）
i2c_master_bus_handle_t Board175c::i2cBus() {
    return bsp_i2c_get_handle();
}

// 能力清单：编译期宏（board_caps.h）→ 运行时结构（BoardCaps）的镜像
static const BoardCaps kCaps = {
    .touch    = GAGA_HAS_TOUCH != 0,
    .imu      = GAGA_HAS_IMU != 0,
    .pmic     = GAGA_HAS_PMIC != 0,
    .rtc      = GAGA_HAS_RTC != 0,
    .display  = GAGA_HAS_DISPLAY != 0,
    .kws      = GAGA_HAS_KWS != 0,
    .lcdHRes  = GAGA_LCD_H_RES,
    .lcdVRes  = GAGA_LCD_V_RES,
};

const BoardCaps& Board175c::caps() const {
    return kCaps;
}

// 深睡唤醒源：BOOT 键 GPIO0（RTC 域 IO，ext0 低电平唤醒；PWR 键非 GPIO 唤不醒）。
// 配置落在 RTC 域，装配期调一次一直有效到真正进深睡。
void Board175c::setupWakeupSources() {
    esp_sleep_enable_ext0_wakeup(GAGA_PIN_BTN_BOOT, 0);  // BOOT 键低电平唤醒
}

}  // namespace gaga
