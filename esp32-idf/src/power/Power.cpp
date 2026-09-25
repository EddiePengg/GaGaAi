#include "power/Power.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "soc/rtc.h"  // rtc_clk_cpu_freq_*（S3 port 私有时钟 API）

#include "compat.h"
#include "input/PwrKey.h"
#include "state/AppState.h"

namespace gaga {

static const char* TAG = "gaga.power";

void Power::begin(AppState* app) {
    app_ = app;
    // 屏幕亮灭回调直接挂降频（亮屏即事件驱动恢复，不靠 tick 轮询）。
    // 用 rtc_clk 私有 API 定点降频（Arduino 核 setCpuFrequencyMhz 同款路径，
    // 行为可控可逆）；官方 esp_pm DFS 涉及 tickless/light-sleep 全局行为，
    // 留作功耗实测后的升级项。
    app_->onScreenChange([this](ScreenState s) {
        // ⚠️ 运行时降频撤回（真机实锤 2026-09-25 清晨：80MHz 下 QSPI 刷屏
        // 静默失败——LVGL 离线渲染正常、物理屏 flush 乱写，旧 GRAM 擦不掉，
        // 用户观感"所有界面糊成一团/残影叠加"）。QSPI 时钟从 APB 分频，
        // 切频后时序全错。低功耗路线保留挂机深睡（<1mA，第二层）；
        // 降频需先解决 esp_lcd 的时钟源再评估。
        (void)s;
    });
    ESP_LOGI(TAG, "power 就绪（挂机 %lumin 深睡；运行时降频已撤——毁 QSPI 刷屏）",
             static_cast<unsigned long>(IDLE_TIMEOUT_MS / 60000));
}

void Power::tick(bool bleConnected) {
    if (app_ == nullptr) return;
    // 深睡条件：息屏 + 没连手机 + 没在充电 + 空闲（不录音/不发送）
    const bool canSleep =
        app_->screen() == ScreenState::Off && !bleConnected &&
        app_->recState() != RecState::Recording && app_->recState() != RecState::Sending;
    if (!canSleep) {
        idleSinceMs_ = 0;
        return;
    }
    PwrBattery b{};
    if (pwrGetBattery(&b) && (b.charging || b.usb)) {
        // 插着 USB 不睡（2026-09-25 实锤：满电后 AXP2101 转"待机"，
        // charging 位变 false，插着线也深睡 → 调试/充电时反复"设备失踪"）。
        // 判据用 VBUS 在位（b.usb），charging 只是它的子集。
        idleSinceMs_ = 0;
        return;
    }
    const uint32_t now = millis();
    if (idleSinceMs_ == 0) {
        idleSinceMs_ = now;
        return;
    }
    if (now - idleSinceMs_ >= IDLE_TIMEOUT_MS) {
        ESP_LOGI(TAG, "挂机 %lumin（息屏+未连+未充），进深睡",
                 static_cast<unsigned long>(IDLE_TIMEOUT_MS / 60000));
        enterDeepSleep();
    }
}

void Power::sleepNow() {
    ESP_LOGI(TAG, "串口触发深睡");
    enterDeepSleep();
}

// 深睡前不逐个关外设：deep sleep 全域断电（保留域不用）。
// 唤醒源 = BOOT 键 GPIO0（RTC 域 IO，ext0 低电平唤醒；PWR 键非 GPIO 唤不醒）。
void Power::enterDeepSleep() {
    // 显式拉一次活动时间戳语义（醒来是冷启动，无状态可保）
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);  // BOOT 键低电平唤醒
    // 给日志一点时间冲出 USB 控制台再睡
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();
}

}  // namespace gaga
