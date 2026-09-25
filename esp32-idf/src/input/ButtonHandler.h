#pragma once

#include <cstdint>
#include <functional>

// 按键状态机：单击 / 双击 / 长按（ADR-016 手势分工）
// 逻辑与具体引脚无关：GPIO 由构造参数传入；按键不在 GPIO 上时（PWR 键接
// TCA9554 EXIO4）用自定义 reader 构造。
// 与 Arduino 线 esp32/src/input/ButtonHandler.* 逻辑一致（millis 走 compat.h）。
// 非阻塞，需在主循环里持续调 loop()。
namespace gaga {

class ButtonHandler {
public:
    static constexpr uint32_t DEBOUNCE_MS     = 40;   // 电气防抖
    static constexpr uint32_t DOUBLE_CLICK_MS = 300;  // 双击判定窗口
    static constexpr uint32_t LONG_PRESS_MS   = 400;  // 长按阈值（2026-09-23 用户反馈 800ms 太长，降为 400ms）

    using Callback = std::function<void()>;

    // activeLow=true：按下为 LOW（内部上拉）；gpioNum < 0 时 begin() 为空操作（占位）
    explicit ButtonHandler(int gpioNum, bool activeLow = true);
    // 自定义读取：按键不在 GPIO 上时使用（PWR 键经 TCA9554 EXIO4 电平轮询）
    // reader 返回"当前是否按下"。此模式下 gpioNum 参数忽略，不做 gpio 配置。
    explicit ButtonHandler(std::function<bool()> reader);

    // 配置 GPIO（有自定义 reader 时跳过）；gpioNum < 0 且无 reader 时不做任何事
    void begin();
    // 每圈轮询：防抖 → 按/松沿回调 → 长按与双击窗判定（非阻塞，主循环持续调）
    void loop();

    bool isPressed() const { return stablePressed_; }
    bool enabled() const { return enabled_; }
    // 在 onReleaseUp 回调里查询：本次松开是否发生在长按之后（ADR-016 按住说话）
    bool longFired() const { return longFired_; }

    // 单击：松手后 300ms 双击窗内没有第二次按下才触发
    void onSingleClick(Callback cb) { singleCb_ = std::move(cb); }
    // 双击：300ms 内连按两下（触发后不再补单击）
    void onDoubleClick(Callback cb) { doubleCb_ = std::move(cb); }
    // 长按：按住超过 400ms 触发一次，这次松手不再算 click
    void onLongPress(Callback cb)   { longCb_   = std::move(cb); }
    // 原始按下/松开（"按住说话"场景用，ADR-016）
    void onPressDown(Callback cb)   { pressCb_  = std::move(cb); }
    void onReleaseUp(Callback cb)   { releaseCb_ = std::move(cb); }  // 配 longFired() 区分"长按说话结束"和普通松手

private:
    enum class State : uint8_t { Idle, Pressed, WaitDouble };

    // 按下沿（防抖确认后）：记时刻、复位长按标志、报 onPressDown
    void handlePress(uint32_t now);
    // 松开沿：先报 onReleaseUp，再决定这次松手算不算 click
    void handleRelease(uint32_t now);

    int      gpioNum_;
    bool     activeLow_;
    bool     enabled_       = false;
    std::function<bool()> reader_;  // 非空则用自定义读取（PWR 键走 TCA9554 的场景）

    // 电气防抖
    bool     lastRaw_       = false;
    bool     stablePressed_ = false;
    uint32_t lastRawChange_ = 0;

    // 手势识别
    State    state_      = State::Idle;
    uint32_t pressAt_    = 0;
    uint32_t releaseAt_  = 0;
    uint8_t  clickCount_ = 0;
    bool     longFired_  = false;

    Callback singleCb_, doubleCb_, longCb_, pressCb_, releaseCb_;
};

}  // namespace gaga
