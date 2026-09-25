#include "AppState.h"

#include "esp_log.h"

#include "compat.h"

namespace gaga {

static const char* TAG = "gaga.state";

// 上电复位：亮屏、空闲、talk 关，自动息屏计时从现在起算
void AppState::begin() {
    screen_ = ScreenState::On;
    rec_    = RecState::Idle;
    talk_   = TalkState::Off;
    lastActivityMs_ = millis();
}

// 主循环节拍：各状态的超时回落 + 无操作自动息屏
void AppState::tick() {
    if (rec_ == RecState::Sent && (millis() - sentAtMs_) >= SENT_HOLD_MS) {
        setRec(RecState::Idle);
    }
    // 发送超时兜底：服务器无回执（网络断/音频未接入等）不永远卡住
    if (rec_ == RecState::Sending && (millis() - sendingAtMs_) >= SENDING_TIMEOUT_MS) {
        ESP_LOGW(TAG, "sending 超时（8s 无回执），回 idle");
        setRec(RecState::Idle);
    }
    // talk_ready 等待超时：服务端未建起会话
    if (talk_ == TalkState::Connecting && (millis() - talkConnAtMs_) >= TALK_READY_TIMEOUT_MS) {
        ESP_LOGW(TAG, "talk_ready 超时（10s），回 idle");
        setTalkState(TalkState::Off);
    }
    // 无操作自动息屏（ADR-016）；录音/发送过程不息屏，保证状态可见。
    // talk 期间靠下行活动（notifyActivity）续命——对话安静 10s 屏幕照样睡，会话不断。
    // 时长可由用户设置（ADR-031），默认 10s。
    if (screen_ == ScreenState::On && rec_ == RecState::Idle &&
        (millis() - lastActivityMs_) >= screenTimeoutMs_) {
        setScreen(ScreenState::Off);
    }
}

// 切屏（同态直接返回）；回调让 UI 真正开关显示面板
void AppState::setScreen(ScreenState s) {
    if (screen_ == s) return;
    screen_ = s;
    if (screenCb_) screenCb_(s);
}

// 戳活动时间戳：按键/触摸/talk 下行都算活动，用来续自动息屏的命
void AppState::notifyActivity() {
    lastActivityMs_ = millis();
}

// 起录音：除 Recording 外都可起（Sending/Sent 不挡下一段，异步队列）
bool AppState::startRecording() {
    // 异步队列（2026-09-23 用户要求）：Sending/Sent 也允许起新录音——
    // 上一段的回执异步到达，只负责叮一声/更新 UI，不阻塞下一段
    if (rec_ == RecState::Recording) return false;
    if (talk_ != TalkState::Off) return false;  // talk 期间不录（音频通路被占）
    recStartMs_ = millis();
    durationMs_ = 0;
    notifyActivity();
    setScreen(ScreenState::On);  // 录音 UI 必须可见，息屏状态下也直接唤醒
    setRec(RecState::Recording);
    return true;
}

// 松手停录并上送：冻结本段时长、记发送时刻，进入 Sending 等服务器回执
bool AppState::stopRecordingAndSend() {
    if (rec_ != RecState::Recording) return false;
    durationMs_ = millis() - recStartMs_;
    sendingAtMs_ = millis();
    setRec(RecState::Sending);
    return true;
}

// 误触撤销（按下即录、300ms 内松手）：当没发生过，什么信令都不发
void AppState::cancelRecording() {
    if (rec_ != RecState::Recording) return;
    durationMs_ = 0;
    setRec(RecState::Idle);
}

// 服务器 receipt：Sending → Sent；记时刻给 ✅ 停留 1.5s 用
void AppState::notifyReceipt() {
    if (rec_ != RecState::Sending) return;
    sentAtMs_ = millis();
    setRec(RecState::Sent);
}

// 服务器 error / 发送失败：Sending 直接回 idle（不进 Sent）
void AppState::notifyError() {
    if (rec_ == RecState::Sending) setRec(RecState::Idle);
}

// talk 状态迁移（TalkSession 调用）：只认合法边，进非 Off 时强制亮屏
bool AppState::setTalkState(TalkState s) {
    if (talk_ == s) return false;
    // 合法迁移：Off→Connecting→Active→Off；允许 Connecting/Active 直接回 Off
    const bool ok =
        (talk_ == TalkState::Off && s == TalkState::Connecting) ||
        (talk_ == TalkState::Connecting && (s == TalkState::Active || s == TalkState::Off)) ||
        (talk_ == TalkState::Active && s == TalkState::Off);
    if (!ok) return false;
    if (s == TalkState::Connecting) talkConnAtMs_ = millis();
    notifyActivity();
    if (s != TalkState::Off) setScreen(ScreenState::On);
    talk_ = s;
    if (talkCb_) talkCb_(s);
    return true;
}

// 录音中返回实时计时；结束后返回 stopRecordingAndSend 冻结的时长
uint32_t AppState::recordingElapsedMs() const {
    if (rec_ == RecState::Recording) return millis() - recStartMs_;
    return durationMs_;
}

// 换录音状态并回调 UI（同态忽略）
void AppState::setRec(RecState s) {
    if (rec_ == s) return;
    rec_ = s;
    if (recCb_) recCb_(s);
}

}  // namespace gaga
