#include "audio/UplinkPump.h"

#include <cstring>

#include "esp_log.h"

#include "AppContext.h"
#include "audio/AudioPipe.h"
#include "audio/OpusCodec.h"
#include "ble/GattServer.h"
#include "compat.h"
#include "protocol/frame.h"
#include "state/AppState.h"
#include "talk/TalkSession.h"
#include "ui/Ui.h"

namespace gaga {

static const char* TAG = "gaga.pump";

static constexpr uint32_t UPLINK_TASK_STACK = 32768;  // opus_encode 栈需求大

void UplinkPump::begin(AppContext* ctx) {
    ctx_ = ctx;
    taskCreatePsram(taskEntry, "gaga_uplink", UPLINK_TASK_STACK, this, 6, nullptr, 1);
}

void UplinkPump::taskEntry(void* arg) {
    static_cast<UplinkPump*>(arg)->run();
}

void UplinkPump::beginSession() {
    framesSent_ = 0;
    framesDrop_ = 0;
    recRmsSum_  = 0;
    committed_  = false;
    bufCount_   = 0;
    sessionLive_ = true;
    cuePending_ = true;        // "嘎"挂起：等第一帧麦克风数据真正到手才响
    micFailCycles_ = 0;
    micFailReported_ = false;
}

void UplinkPump::commit()        { committed_ = true; }
void UplinkPump::discard()       { bufCount_ = 0; sessionLive_ = false; }
void UplinkPump::cycleMicChannel() {
    micChanSel_ = (micChanSel_ + 1) % 3;
    ESP_LOGI(TAG, "[dbg] mic 声道选择 → %d（0=左 1=右 2=平均）", micChanSel_);
}

// 一次 20ms 采集周期：把 mic 的 PCM 攒满一帧。返回 false=通路未开/读失败
// ES7210 立体声交织（L=MIC1 / R=MIC2，姿势 A——官方 05_Spec_Analyzer 同款）
bool UplinkPump::micReadCycle(int16_t* monoOut, int framesPerCh) {
    AudioPipe* audio = ctx_->audio;
    const int need = framesPerCh * AudioPipe::MIC_CHANNELS * sizeof(int16_t);  // 交织
    static int16_t stereoBuf[480 * 2];                    // 24k/20ms 双声道上限
    size_t got = 0;
    uint8_t* p = reinterpret_cast<uint8_t*>(stereoBuf);
    const uint32_t deadline = millis() + 200;             // 单周期兜底超时
    while (got < static_cast<size_t>(need)) {
        const int n = audio->micRead(p + got, need - got);
        if (n <= 0) {
            if (millis() > deadline) return false;  // 200ms 还读不齐 = 通路没跑起来
            continue;                               // 短暂读空只等下一轮，不判失败
        }
        got += static_cast<size_t>(n);
        if (millis() > deadline && got < static_cast<size_t>(need)) return false;
    }
    if (micDebug_) {
        // 声道分列 RMS（L=MIC1 / R=MIC2）+ 头部采样——采集全零排查的眼睛
        static int16_t leftBuf[480], rightBuf[480];
        AudioPipe::downmix(stereoBuf, leftBuf, framesPerCh, 0);
        AudioPipe::downmix(stereoBuf, rightBuf, framesPerCh, 1);
        static uint32_t dbgAt = 0;
        if (millis() - dbgAt >= 500) {
            dbgAt = millis();
            ESP_LOGI(TAG, "[dbg] mic L rms=%lu R rms=%lu | L0..3=%d %d %d %d R0..3=%d %d %d %d",
                     static_cast<unsigned long>(AudioPipe::rms(leftBuf, framesPerCh)),
                     static_cast<unsigned long>(AudioPipe::rms(rightBuf, framesPerCh)),
                     leftBuf[0], leftBuf[1], leftBuf[2], leftBuf[3],
                     rightBuf[0], rightBuf[1], rightBuf[2], rightBuf[3]);
        }
    }
    AudioPipe::downmix(stereoBuf, monoOut, framesPerCh, micChanSel_);
    return true;
}

// 编码并上行一帧（mono320 必须已凑满 320 采样）。
// isTalk=true 直发；录音帧看确认窗：commit 前入缓冲，commit 后连缓冲一起冲出
void UplinkPump::flushOneFrame(const int16_t* mono320, bool isTalk) {
    const uint32_t rms = AudioPipe::rms(mono320, OpusEnc::FRAME_SAMPLES);
    uint8_t pkt[256];
    const int n = ctx_->enc->encode(mono320, pkt, sizeof(pkt));
    if (n <= 0) {
        ESP_LOGW(TAG, "[rec] opus 编码失败，丢帧");
        return;
    }
    uint32_t* rmsSum = isTalk ? &talkRmsSum_ : &recRmsSum_;  // 能量记到对应桶
    *rmsSum += rms;
    // 确认窗内（commit 前）：入缓冲不上行——commit 时整体冲出，按下起零丢失
    //（含"滴"声段；ASR 实测 1kHz 单音不污染识别，ADR-026）
    if (!isTalk && !committed_) {
        if (n <= REC_BUF_PKT_SZ && bufCount_ < REC_BUF_PKTS) {
            memcpy(bufPkts_[bufCount_], pkt, static_cast<size_t>(n));
            bufLen_[bufCount_] = static_cast<uint16_t>(n);
            bufCount_++;
        } else {
            framesDrop_++;  // 缓冲满/包超长才丢（1.28s 容量对 300ms 窗绰绰有余）
        }
        return;
    }
    // commit 后首帧：先把缓冲按时间序整体冲出，再上行当前帧
    if (bufCount_ > 0) {
        ESP_LOGI(TAG, "[rec] 确认窗缓冲 %d 帧整体上行（按下起零丢失）", bufCount_);
        for (int i = 0; i < bufCount_; i++) {
            if (ctx_->link->isConnected() &&
                ctx_->link->sendFrame(FRAME_TYPE_OPUS, bufPkts_[i], bufLen_[i])) {
                framesSent_++;
            } else {
                framesDrop_++;
            }
        }
        bufCount_ = 0;
    }
    // 计数要诚实：sendFrame 真的把包推进了 NimBLE 才算 sent（bug-analysis-0923 §3a）
    if (ctx_->link->isConnected()) {
        const bool sent = ctx_->link->sendFrame(FRAME_TYPE_OPUS, pkt, static_cast<uint16_t>(n));
        if (sent) {
            if (isTalk) talkFrames_++; else framesSent_++;
        } else {
            framesDrop_++;  // 发送失败（未订阅/队列满）：记丢弃
        }
    } else {
        framesDrop_++;  // BLE 未连接：丢弃（协议要求记日志）
    }
    const uint32_t total = isTalk ? talkFrames_ : (framesSent_ + framesDrop_);
    if (total % 25 == 0) {  // ~0.5s 一行：能量随声音变化 = 采集存活证据
        ESP_LOGI(TAG, "[%s] frames=%lu rms(avg)=%lu pkt=%dB",
                 isTalk ? "talk" : "rec",
                 static_cast<unsigned long>(total),
                 static_cast<unsigned long>(*rmsSum / 25), n);
        *rmsSum = 0;
    }
}

// 泵主循环：它只管"采样→编码→发帧"，按键/状态机/UI 都归 app 任务。
// 录音和 talk 由 AppState 保证互斥，顺序判谁在跑就伺候谁；都闲就睡觉省 CPU
void UplinkPump::run() {
    static int16_t mono480[480];  // 下混后单声道（24k 一帧 480 采样为上限）
    static int16_t mono320[OpusEnc::FRAME_SAMPLES];
    for (;;) {
        const bool isTalk = (ctx_->app->talkState() == TalkState::Active);
        if (ctx_->app->recState() == RecState::Recording) {
            // ---- M1：16k 原生（姿势 A，零重采样）----
            if (!micReadCycle(mono480, OpusEnc::FRAME_SAMPLES)) {
                // "嘎"必须等麦克风真出数据才响（ADR-037 语义：嘎=一切正常）。
                // ~150 个失败周期 ≈ 1.5s 还没数据 = 麦克风故障：放弃本条+错误提示
                micFailCycles_++;
                if (micFailCycles_ > 150 && !micFailReported_) {
                    micFailReported_ = true;
                    ESP_LOGW(TAG, "麦克风 1.5s 无数据，放弃本条录音");
                    ctx_->app->cancelRecording();
                    ctx_->audio->poot();
                    ctx_->ui->showNote("麦克风未就绪");
                }
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            micFailCycles_ = 0;
            if (cuePending_) {
                cuePending_ = false;
                if (ctx_->link->isConnected()) {
                    // 开录提示唯一发声点（ADR-037：第一帧真到手=一切正常）。
                    // event() 按声效方案分发：鸭子=嘎×1 / 叮咚=滴×1
                    ctx_->audio->event(AudioPipe::SndEv::Listen);
                }
            }
            memcpy(mono320, mono480, sizeof(mono320));
            flushOneFrame(mono320, false);  // commit 前入缓冲，commit 后直上
        } else if (sessionLive_) {
            // 停录收尾：帧数是本泵的计数器，统计日志借这里打（app 任务不碰计数）
            sessionLive_ = false;
            ESP_LOGI(TAG, "[rec] stop: %lums, 上行 %lu 帧，丢弃 %lu 帧",
                     static_cast<unsigned long>(ctx_->app->lastDurationMs()),
                     static_cast<unsigned long>(framesSent_),
                     static_cast<unsigned long>(framesDrop_));
        } else if (isTalk) {
            // ---- M5 talk ----
            // 半双工降噪（回声根治前的替代，ADR-037）：豆包说话时不上行——
            // 扬声器回声进麦会被豆包当"有人打断"，压得语音时断时续+自说自话。
            // 服务端 pacer 会自动 mute/unmute 配合
            if (ctx_->talk->downlinkBusy()) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            if (!micReadCycle(mono480, 480)) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            AudioPipe::resample24to16(mono480, mono320);
            flushOneFrame(mono320, true);  // talk 采集即发送，没有确认窗
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));  // 录音/talk 都没跑：闲着，别空转烧 CPU
        }
    }
}

}  // namespace gaga
