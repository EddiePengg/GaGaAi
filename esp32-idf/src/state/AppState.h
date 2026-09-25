#pragma once

#include <cstdint>
#include <functional>

// 应用状态机（docs/hardware.md 按键分工 + ADR-016 交互 v2）
//   屏幕：右上键单击亮屏；SCREEN_TIMEOUT_MS 无操作自动息屏（录音/发送中不息屏）
//   录音（M1）：idle → recording → sending → sent(✅) → idle
//   对话（M5）：off → connecting → active → off（由 TalkSession 驱动流转）
// 状态变更通过回调通知 UI / BLE 层，本类不直接依赖任何外设。
namespace gaga {

enum class ScreenState : uint8_t { On, Off };
enum class RecState : uint8_t { Idle, Recording, Sending, Sent };
enum class TalkState : uint8_t { Off, Connecting, Active };

class AppState {
public:
    // ✅ 停留时长，到点自动回 idle
    static constexpr uint32_t SENT_HOLD_MS = 1500;
    // 发送超时：等不到服务器 receipt/error 就回 idle
    static constexpr uint32_t SENDING_TIMEOUT_MS = 8000;
    // 无操作自动息屏（ADR-016）默认值；录音/发送中不息屏。
    // 运行时可改（用户设置 5/10/30/60s，ADR-031）：setScreenTimeoutMs
    static constexpr uint32_t SCREEN_TIMEOUT_MS = 10000;
    // talk_request 等待 talk_ready 的超时
    static constexpr uint32_t TALK_READY_TIMEOUT_MS = 10000;

    using RecCallback    = std::function<void(RecState)>;
    using ScreenCallback = std::function<void(ScreenState)>;
    using TalkCallback   = std::function<void(TalkState)>;

    void begin();  // 上电复位：亮屏 / 空闲 / talk 关
    void tick();   // 主循环调用：超时回落 + 屏幕无操作自动息屏

    ScreenState screen() const { return screen_; }
    RecState    recState() const { return rec_; }
    TalkState   talkState() const { return talk_; }

    void setScreen(ScreenState s);  // 强制切屏（回调 UI 去开关显示面板）
    // 息屏时长（用户设置 ADR-031；Settings 档位 5/10/30/60s，默认 10s）
    void setScreenTimeoutMs(uint32_t ms) { screenTimeoutMs_ = ms; }
    uint32_t screenTimeoutMs() const { return screenTimeoutMs_; }
    // 任何用户操作（按键/触摸）与 talk 下行活动都调它：重置自动息屏计时
    void notifyActivity();

    // 录音流转；非法迁移直接忽略并返回 false
    bool startRecording();        // idle → recording
    bool stopRecordingAndSend();  // recording → sending
    void cancelRecording();       // recording → idle（按下即录的误触撤销：300ms 内松手，什么信令都不发）
    void notifyReceipt();         // sending → sent（服务器 receipt 信令）
    void notifyError();           // sending → idle（服务器 error 信令 / 发送失败）

    // talk 流转（TalkSession 调用）：Off → Connecting → Active → Off
    bool setTalkState(TalkState s);
    // talk_request 发出时刻（tick 用做 ready 超时判定）
    uint32_t talkConnectingSinceMs() const { return talkConnAtMs_; }

    bool     isRecording() const { return rec_ == RecState::Recording; }
    bool     isTalking() const { return talk_ != TalkState::Off; }
    // 手机侧 MQTT 链路（ADR-038）：App 经 BLE 信令 {"type":"link","mqtt":…} 推送。
    // 默认 true（乐观）：App 冻结时 BLE 也断，走蓝牙预检；只有 App 活着但
    // 网络不通的情形才会显式推 false——那是本标志要拦的场景。
    void     setMqttLink(bool on) { mqttLink_ = on; }
    bool     mqttLink() const { return mqttLink_; }
    // 录音进行中的实时计时；非录音态返回最近一段的时长
    uint32_t recordingElapsedMs() const;
    // stopRecordingAndSend() 时冻结的本段时长
    uint32_t lastDurationMs() const { return durationMs_; }

    void onRecStateChange(RecCallback cb)    { recCb_ = std::move(cb); }
    void onScreenChange(ScreenCallback cb)   { screenCb_ = std::move(cb); }
    void onTalkStateChange(TalkCallback cb)  { talkCb_ = std::move(cb); }

private:
    void setRec(RecState s);

    ScreenState screen_    = ScreenState::On;
    RecState    rec_       = RecState::Idle;
    TalkState   talk_      = TalkState::Off;
    uint32_t    recStartMs_ = 0;      // 本段录音的按下时刻
    uint32_t    durationMs_ = 0;      // 冻结的本段录音时长（stop 时写入）
    uint32_t    sentAtMs_   = 0;      // 进 Sent 的时刻（✅ 停留 1.5s 计时）
    uint32_t    sendingAtMs_ = 0;     // 进 Sending 的时刻（8s 无回执超时判定）
    uint32_t    talkConnAtMs_ = 0;    // talk_request 发出时刻（ready 超时判定）
    uint32_t    lastActivityMs_ = 0;  // 最近一次用户/下行活动（自动息屏计时）
    uint32_t    screenTimeoutMs_ = SCREEN_TIMEOUT_MS;  // 无操作息屏时长（可设置）
    bool        mqttLink_   = true;   // 手机侧 MQTT 链路（App 推送，ADR-038）

    RecCallback    recCb_;
    ScreenCallback screenCb_;
    TalkCallback   talkCb_;
};

}  // namespace gaga
