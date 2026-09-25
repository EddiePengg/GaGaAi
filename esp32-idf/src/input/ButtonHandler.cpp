#include "ButtonHandler.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include "compat.h"

namespace gaga {

static const char* TAG = "gaga.input";

// GPIO 直读构造（普通按键）
ButtonHandler::ButtonHandler(int gpioNum, bool activeLow)
    : gpioNum_(gpioNum), activeLow_(activeLow) {}

// 自定义 reader 构造（按键不在 GPIO 上：PWR 键走 AXP2101 边沿轮询）
ButtonHandler::ButtonHandler(std::function<bool()> reader)
    : gpioNum_(-1), activeLow_(true), reader_(std::move(reader)) {}

// 使能按键：有 reader 直接用；否则配置 GPIO 输入 + 上/下拉
void ButtonHandler::begin() {
    if (reader_) {
        enabled_ = true;  // 自定义读取无需 GPIO 配置
        return;
    }
    if (gpioNum_ < 0) {
        ESP_LOGW(TAG, "button gpio = -1（占位），按键未启用");
        return;
    }
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << gpioNum_;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = activeLow_ ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = activeLow_ ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "gpio%d 配置失败", gpioNum_);
        return;
    }
    enabled_ = true;
}

// 主循环轮一圈：读电平 → 40ms 防抖 → 按/松沿回调 → 长按与双击窗判定
void ButtonHandler::loop() {
    if (!enabled_) return;

    const uint32_t now = millis();
    const bool raw = reader_
        ? reader_()
        : (gpio_get_level(static_cast<gpio_num_t>(gpioNum_)) == (activeLow_ ? 0 : 1));

    // 40ms 电气防抖：原始电平变化后等待稳定
    if (raw != lastRaw_) {
        lastRaw_       = raw;
        lastRawChange_ = now;
    }
    // 连续 40ms 电平不变才算数，此时才派发按下/松开沿
    if (lastRaw_ != stablePressed_ && (now - lastRawChange_) >= DEBOUNCE_MS) {
        stablePressed_ = lastRaw_;
        if (stablePressed_) handlePress(now); else handleRelease(now);
    }

    // 长按：按下超过阈值触发一次，松开时不再算单击
    if (state_ == State::Pressed && !longFired_ && (now - pressAt_) >= LONG_PRESS_MS) {
        longFired_ = true;
        if (longCb_) longCb_();
    }

    // 双击窗口（300ms）超时 → 落定为单击
    if (state_ == State::WaitDouble && (now - releaseAt_) >= DOUBLE_CLICK_MS) {
        state_      = State::Idle;
        clickCount_ = 0;
        if (singleCb_) singleCb_();
    }
}

// 按下沿：记时刻、复位长按标志；Idle 或 WaitDouble（第二次按下）都进 Pressed
void ButtonHandler::handlePress(uint32_t now) {
    pressAt_   = now;
    longFired_ = false;
    // Idle 或 WaitDouble（第二次按下）都进入 Pressed
    state_ = State::Pressed;
    if (pressCb_) pressCb_();
}

// 松开沿：先报原始松手（按住说话靠它停录），再决定算不算 click
void ButtonHandler::handleRelease(uint32_t now) {
    if (releaseCb_) releaseCb_();

    if (longFired_) {
        // 长按后的松开不产生 click 事件
        longFired_  = false;
        clickCount_ = 0;
        state_      = State::Idle;
        return;
    }

    clickCount_++;
    if (clickCount_ >= 2) {
        clickCount_ = 0;
        state_      = State::Idle;
        if (doubleCb_) doubleCb_();
    } else {
        // 松手宽限：先进 WaitDouble 等 300ms，没有第二次按下才由 loop() 落定单击
        state_     = State::WaitDouble;
        releaseAt_ = now;
    }
}

}  // namespace gaga
