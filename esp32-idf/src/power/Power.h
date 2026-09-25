#pragma once

#include <cstdint>

// 低功耗策略（ADR-037，目标：400mAh 用一天）：
//   ① 息屏降频：CPU 240→80MHz（PLL 分频档；不可用 set_xtal——它关 BBPLL 会连带杀掉 USB 控制台和 BLE，真机踩实——Arduino 核
//      setCpuFrequencyMhz 同款路径；亮屏/录音/talk 必亮屏，天然只降"息屏"态）。
//      I2S/SPI/BLE modem 时钟独立于 CPU 频率，息屏时本就无流量。
//   ② 挂机深睡：息屏 + BLE 未连接 + 未充电 + 持续 IDLE_TIMEOUT 无操作
//      → esp_deep_sleep。深睡电流 <1mA（IMU 微安级在岗但 INT 脚未引出，
//      深睡时无法被 IMU 唤醒——没连手机的挂机态本来就用不了语音，零损失）。
//      唤醒源 = BOOT 键（GPIO0 ext0 低电平），唤醒即冷启动：BLE 重新广播，
//      App 自动重连（M2 保活组合拳）。充电时不睡（挂着也无所谓功耗）。
// 功耗账：亮屏 ~100mA+ / 息屏浅待机 ~25-40mA / 深睡 <1mA。
//   一天达成路径 = 正常使用间歇息屏 + 不用时自动深睡兜底。
namespace gaga {

class AppState;

class Power {
public:
    static constexpr uint32_t IDLE_TIMEOUT_MS = 10 * 60 * 1000;  // 挂机 10 分钟深睡

    void begin(AppState* app);

    // appTask 每 10ms 调用：管息屏降频 + 深睡判定。
    // bleConnected：当前 BLE 是否连着手机（连着=可能随时说话，不深睡）
    void tick(bool bleConnected);

    // 串口 'Z'：立即深睡（验证唤醒路径）
    void sleepNow();

    // CPU 降频状态（诊断）
    bool lowPowerClock() const { return lowClock_; }

private:
    void enterDeepSleep();

    AppState* app_ = nullptr;
    bool      lowClock_ = false;         // 当前是否 80MHz
    uint32_t  idleSinceMs_ = 0;          // 进入"可深睡条件"的时刻（0=不满足）
};

}  // namespace gaga
