#include "rec/Recorder.h"

#include <cstdio>

#include "esp_log.h"

#include "AppContext.h"
#include "state/Settings.h"
#include "audio/AudioPipe.h"
#include "audio/UplinkPump.h"
#include "ble/GattServer.h"
#include "compat.h"
#include "version.h"
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

// 开录（右下按下沿 / 摇动 / 串口 'r'）：空闲 / 非 talk / BLE 已连 三道闸都过
// 才进录音态。按下即录（2026-09-26 用户拍板 ADR-045 v2：马上就录，开头一个
// 字都不丢）：立即进录音态 + 红点 UI，"嘎"在音频上电完成后播（作业 FIFO，
// 略晚）；300ms 确认窗过后才发 recStart。
void Recorder::start() {
    // 只挡"正在录"：Sending/Sent（上一条等回执/已回执）允许立即开新录音——
    // 上一条回执异步到达、各归各卡（2026-09-23 异步语义；原 Idle 闸门让用户
    // 松手后要白等回执链走完 ~5s 才能再录，2026-09-25 反馈修复）
    if (ctx_->app->recState() == RecState::Recording) return;
    if (ctx_->app->isTalking()) {
        // 静默拒绝不行（用户反馈 2026-09-25）：不看屏幕必须给声音提示
        ESP_LOGW(TAG, "talk 进行中，录音不可用（音频通路被占）");
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
    // 手机侧 MQTT 断不再拦截（2026-09-26 ADR-049，离线录音）：ColorOS 熄屏
    // 会秒杀 App 的每一条后台连接（今晚实测：连接存活 3~12s 一杀），录音入口
    // 被这道门拦成"想说话说不出"。而泵本来就把每帧同步进 PSRAM 缓存、链路
    // 恢复自动补发（rec_status_query 对账兜底）——对着断链说完话，恢复后
    // 照样进群。"手机没连上服务器"提示随之退役；真无路可走只剩 BLE 断连
    // 那道门（连缓存都送不出去时才有拦的意义）。
    if (!ctx_->app->startRecording()) return;
    // 上电走作业 FIFO（ADR-027）。开录提示音全固件只有唯一发声点：
    // UplinkPump 第一帧麦克风数据到手（ADR-037：嘎=一切正常）。
    // 双响事故病根（2026-09-25 用户四次报障）：这里曾又发一次 Listen，
    // 两处各响一声 = 按下必嘎两声。删。
    ctx_->audio->requestOpen(AudioPipe::RATE_REC);
    ctx_->pump->beginSession();
    commitAtMs_ = millis() + UplinkPump::REC_COMMIT_MS;
}

// 误触撤销：确认窗内再触发（rec_start 未发，泵只缓冲没上行）→ 丢缓冲回
// Idle，服务端全程不知情（零信令）。急促双击 = 开了马上关 = 在这里丢弃。
void Recorder::cancel() {
    ESP_LOGI(TAG, "[rec] 确认窗内撤销（<%ums），静默丢弃",
             static_cast<unsigned>(UplinkPump::REC_COMMIT_MS));
    ctx_->pump->discard();
    ctx_->app->cancelRecording();
    ctx_->audio->requestClose();  // 会话没开成也把音频前端断电回零耗
    // 录音中上一条的 receipt/reply 可能已入卡（数据层照记），撤销后列表
    // 得补一刷，否则屏幕停留在旧状态直到下一条消息（2026-09-26 排查补）
    ctx_->ui->onLogChanged();
}

// 正常收尾：停泵→占位卡→rec_stop→"嘎嘎"→断电
void Recorder::finish() {
    if (ctx_->app->recState() != RecState::Recording) return;
    ESP_LOGI(TAG, "[rec] finish 进入（时长 %lums）",
             static_cast<unsigned long>(ctx_->app->recordingElapsedMs()));
    ctx_->app->stopRecordingAndSend();  // 先落状态：泵立刻停读，"嘎嘎"不会被录进尾巴帧
    ctx_->log->addSending();            // 占位卡立即上屏（你：识别中…）
    ctx_->ui->onLogChanged();
    char buf[80];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"rec_stop\",\"duration_ms\":%lu,\"device\":\"%s\"}",
             static_cast<unsigned long>(ctx_->app->lastDurationMs()), DEVICE_ID);
    sendJson(ctx_, buf);
    vTaskDelay(pdMS_TO_TICKS(30));      // 等在飞的采集周期（≤20ms）收尾再放"嘎嘎"
    ctx_->audio->event(AudioPipe::SndEv::Sent);    // 两声 = 已发出
    ctx_->audio->requestClose();        // FIFO：嘎嘎播完 → 断电回零耗
}

// 开/关唯一入口（右下按下沿、摇动、串口 'r' 同权，ADR-040/045 点对点）：
// 空闲 = 开录；录音中 = 收尾判定——rec_start 未发（<300ms 确认窗）→ 静默
// 撤销（双击丢弃，2026-09-26 用户拍板："再按一下，录制小于 0.3 秒就丢弃"，
// 用录制时长判误触，不需要任何前置等待窗）；已过窗 → 正常收尾。
void Recorder::toggle() {
    if (ctx_->app->recState() != RecState::Recording) {
        start();
        return;
    }
    if (!ctx_->pump->committed()) {
        cancel();
        return;
    }
    finish();
}

void Recorder::autoFinish(bool discard) {
    if (ctx_->app->recState() != RecState::Recording) return;
    if (!ctx_->pump->committed() || discard) {
        cancel();   // 确认窗内/整段无语音：静默撤销，零信令零上行
        // 反馈不能静默（用户实锤：按了键却什么都没发生=困惑）：噗噗+便签说明
        ctx_->audio->event(AudioPipe::SndEv::Error);   // "噗噗"=没录到声音
        ctx_->ui->showNote("没听到声音");
        return;
    }
    finish();
}

// 主循环 tick：确认窗到点 → rec_start（之后泵自动把缓冲整体冲出）。
// 按下即录 + 松手不停（ADR-045 v2）：没有宽限/锁定逻辑了，这里只剩 commit。
void Recorder::tick() {
    // 静音自动收尾（上行泵请求）：连续静音 8s / 超时 3min → 收尾或静默撤销
    if (ctx_->pump->autoStopReq()) {
        const bool discard = ctx_->pump->autoDiscard();
        ctx_->pump->autoStopClear();
        autoFinish(discard);
        return;
    }
    if (ctx_->app->recState() == RecState::Recording &&
        !ctx_->pump->committed() && millis() >= commitAtMs_) {
        ctx_->pump->commit();
        // name = 设备显示名（ADR-064）：服务端拿它做群里署名；空则不带字段
        const char* nm = ctx_->settings->devName();
        char rs[160];
        if (nm[0] != '\0')
            snprintf(rs, sizeof(rs),
                     "{\"type\":\"rec_start\",\"device\":\"" DEVICE_ID "\",\"name\":\"%s\"}", nm);
        else
            snprintf(rs, sizeof(rs),
                     "{\"type\":\"rec_start\",\"device\":\"" DEVICE_ID "\"}");
        sendJson(ctx_, rs);  // 误触已在确认窗内撤销，不惊动服务端
    }
}

}  // namespace gaga
