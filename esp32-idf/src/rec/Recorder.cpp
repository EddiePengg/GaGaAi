#include "rec/Recorder.h"

#include <cstdio>

#include "esp_log.h"

#include "AppContext.h"
#include "audio/AudioPipe.h"
#include "audio/UplinkPump.h"
#include "ble/GattServer.h"
#include "compat.h"
#include "state/AppState.h"
#include "state/MsgLog.h"
#include "ui/Ui.h"

namespace gaga {

static const char* TAG = "gaga.rec";

void Recorder::begin(AppContext* ctx) {
    ctx_ = ctx;
}

// 带锁 JSON 信令出口（GattServer.sendFrame 已内置互斥，这里直接发）
static void sendJson(AppContext* ctx, const char* json) {
    ctx->link->sendJson(json);
}

// 开录（按下沿 / 串口 'r'）：空闲 / 非 talk / BLE 已连 三道闸都过才进录音态。
// 按下即录（2026-09-23 用户反馈长按等待慢）：按下沿立刻进录音态 + 红点 UI，
// "滴"在音频上电完成后播（作业 FIFO，略晚）；300ms 确认窗过后才发 recStart。
void Recorder::start() {
    // 只挡"正在录"：Sending/Sent（上一条等回执/已回执）允许立即开新录音——
    // 上一条回执异步到达、各归各卡（2026-09-23 异步语义；原 Idle 闸门让用户
    // 松手后要白等回执链走完 ~5s 才能再录，2026-09-25 反馈修复）
    if (ctx_->app->recState() == RecState::Recording) return;
    if (ctx_->app->isTalking()) {
        // 静默拒绝不行（用户反馈 2026-09-25）：不看屏幕必须给声音提示
        ESP_LOGW(TAG, "talk 进行中，按住说话不可用（音频通路被占）");
        ctx_->ui->showNote("实时对话中，无法录音");
        ctx_->audio->poot();
        return;
    }
    if (!ctx_->link->isConnected()) {
        // 断链提示（2026-09-23 用户需求）："拿起鸭子却不能说话"的时刻，屏幕告诉你为什么
        ESP_LOGW(TAG, "BLE 未连接，录音不启动（提示打开手机 App）");
        ctx_->ui->showNote("请打开手机App");
        ctx_->audio->event(AudioPipe::SndEv::Error);   // 错误音（噗噗/滴滴）
        return;
    }
    if (!ctx_->app->mqttLink()) {
        // 手机侧 MQTT 断（ADR-038）：BLE 通但服务器不通，说了也白发——立刻拦下。
        // 状态由 App 经 link 信令推送；屏幕文字区分"没连蓝牙"和"手机没网"。
        ESP_LOGW(TAG, "手机侧 MQTT 未连接，录音不启动");
        ctx_->ui->showNote("手机没连上服务器");
        ctx_->audio->event(AudioPipe::SndEv::Error);
        return;
    }
    if (!ctx_->app->startRecording()) return;
    // 上电走作业 FIFO（ADR-027）。开录提示音全固件只有唯一发声点：
    // UplinkPump 第一帧麦克风数据到手（ADR-037：嘎=一切正常）。
    // 双响事故病根（2026-09-25 用户四次报障）：这里曾又发一次 Listen，
    // 两处各响一声 = 按下必嘎两声。删。
    ctx_->audio->requestOpen(AudioPipe::RATE_REC);
    ctx_->pump->beginSession();
    commitAtMs_ = millis() + UplinkPump::REC_COMMIT_MS;
}

// 误触撤销：确认窗内松手 → 丢缓冲回 Idle，服务端全程不知情（零信令）
void Recorder::cancel() {
    ESP_LOGI(TAG, "[rec] 确认窗内松手（<%ums），按误触静默撤销",
             static_cast<unsigned>(UplinkPump::REC_COMMIT_MS));
    releasePending_ = false;
    ctx_->pump->discard();
    ctx_->app->cancelRecording();
    ctx_->audio->requestClose();  // 会话没开成也把音频前端断电回零耗
}

// 正常收尾（真松手/锁定模式再按/串口 'r'）：停泵→占位卡→rec_stop→"滴滴"→断电
void Recorder::finish() {
    if (ctx_->app->recState() != RecState::Recording) return;
    ESP_LOGI(TAG, "[rec] finish 进入（时长 %lums）",
             static_cast<unsigned long>(ctx_->app->recordingElapsedMs()));
    releasePending_ = false;
    ctx_->app->stopRecordingAndSend();  // 先落状态：泵立刻停读，"滴滴"不会被录进尾巴帧
    ctx_->log->addSending();            // 占位卡立即上屏（你：识别中…）
    ctx_->ui->onLogChanged();
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"type\":\"rec_stop\",\"duration_ms\":%lu}",
             static_cast<unsigned long>(ctx_->app->lastDurationMs()));
    sendJson(ctx_, buf);
    vTaskDelay(pdMS_TO_TICKS(30));      // 等在飞的采集周期（≤20ms）收尾再放"嘎嘎"
    ctx_->audio->event(AudioPipe::SndEv::Sent);    // 两声 = 已发出
    ctx_->audio->requestClose();        // FIFO：滴滴播完 → 断电回零耗
}

// 右下按下沿：录音中再按有两种含义（锁定=收尾；宽限期内=续录）
void Recorder::pressDown() {
    if (ctx_->app->recState() == RecState::Recording) {
        if (latchMode_) {
            ESP_LOGI(TAG, "[key] bottom press: 锁定模式再按 → rec stop");
            finish();
        } else if (releasePending_) {
            // 松手宽限期内重新咬合：无缝续录（mic 常开帧不断流，音频无接缝）
            releasePending_ = false;
            ctx_->app->notifyActivity();
            ESP_LOGI(TAG, "[key] bottom press: 宽限期内重新咬合，续录");
        }
        return;
    }
    start();
}

// 右下松开：确认窗内 = 误触静默撤销；确认窗后进松手宽限（hang time）——
// 本板按键释放阈值近、握力波动易误断（2026-09-23 用户实测）：断开不立刻结束，
// 窗内重新咬合=无缝续录，窗尽才 rec_stop。锁定模式下松手完全忽略。
void Recorder::releaseUp() {
    if (ctx_->app->recState() != RecState::Recording) return;
    if (latchMode_) return;  // 锁定模式：结束由"再按一下"触发
    if (!ctx_->pump->committed()) {
        cancel();  // 确认窗内松手 = 误触，立即静默撤销（不等宽限）
        return;
    }
    if (graceMs_ == 0) {  // 旧行为：断开即结束
        ESP_LOGI(TAG, "[key] bottom release: rec stop（宽限关闭）, %lums",
                 static_cast<unsigned long>(ctx_->app->lastDurationMs()));
        finish();
        return;
    }
    releasePending_ = true;
    releaseAtMs_    = millis() + graceMs_;
    ESP_LOGI(TAG, "[key] bottom release: 松手宽限 %lums（重新按住=续录）",
             static_cast<unsigned long>(graceMs_));
}

// 串口 'r'：空闲=开录，录音中=收尾（不走松手宽限）
void Recorder::toggle() {
    if (ctx_->app->recState() == RecState::Idle) start(); else finish();
}

// 主循环 tick：确认窗到点 → rec_start（之后泵自动把缓冲整体冲出）；
// 宽限窗耗尽没等到重新咬合 = 真松手 → 收尾
void Recorder::tick() {
    const RecState rs = ctx_->app->recState();
    if (rs == RecState::Recording) {
        if (!ctx_->pump->committed() && millis() >= commitAtMs_) {
            ctx_->pump->commit();
            sendJson(ctx_, "{\"type\":\"rec_start\"}");  // 误触不惊动服务端
        }
        if (releasePending_ && millis() >= releaseAtMs_) {
            releasePending_ = false;
            ESP_LOGI(TAG, "[key] 松手宽限耗尽：rec stop, %lums",
                     static_cast<unsigned long>(ctx_->app->recordingElapsedMs()));
            finish();
        }
    }
}

// 串口 'w'：松手宽限调参（0=断开即结束）：0 → 250 → 500 → 1000 → 2000 → 0 …
void Recorder::cycleGrace() {
    static const uint32_t kSteps[] = {0, 250, 500, 1000, 2000};
    uint32_t next = 0;
    for (int i = 0; i < 5; i++) {
        if (kSteps[i] == graceMs_) {
            next = kSteps[(i + 1) % 5];
            break;
        }
    }
    graceMs_ = next;
    releasePending_ = false;
    ESP_LOGI(TAG, "[dbg] 松手宽限 → %lums（0=断开即结束）",
             static_cast<unsigned long>(graceMs_));
}

// 串口 'l'：锁定模式开关（松手不算数，再按一下才结束）
void Recorder::toggleLatch() {
    latchMode_ = !latchMode_;
    releasePending_ = false;
    ESP_LOGI(TAG, "[dbg] 锁定模式 → %s（松手%s）",
             latchMode_ ? "开" : "关",
             latchMode_ ? "不算数，再按一下结束" : "即结束，宽限内可续录");
}

}  // namespace gaga
