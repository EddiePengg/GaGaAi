#include "NotifySession.h"

#include <cstdlib>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "cJSON.h"

#include "state/AppState.h"
#include "state/MsgLog.h"
#include "audio/AudioPipe.h"
#include "ui/Ui.h"
#include "talk/TalkSession.h"
#include "compat.h"

namespace gaga {

static const char* TAG = "gaga.notify";

// 保存依赖，建下行抖动队列（装 AudioChunk* 指针）
void NotifySession::begin(AppState* app, AudioPipe* audio, MsgLog* log,
                          Ui* ui, TalkSession* talk) {
    app_   = app;
    audio_ = audio;
    log_   = log;
    ui_    = ui;
    talk_  = talk;
    if (jitterQ_ == nullptr) {
        jitterQ_ = xQueueCreate(JITTER_QUEUE_DEPTH, sizeof(AudioChunk*));
    }
}

// ---------------------------------------------------------------- 下行入口 ----
// notify 开始：互斥检查（talk 活跃/录音中 → 只上屏）→ 播放中来了新通知则
// 停旧起新；notify_end：播放态置 endPending（泵排空自退，tick 里断电收尾），
// Silent 态直接清状态
void NotifySession::onSignalJson(const char* json) {
    if (json == nullptr) return;
    cJSON* root = cJSON_Parse(json);
    if (root == nullptr) return;
    const cJSON* type = cJSON_GetObjectItem(root, "type");
    const char* t = (cJSON_IsString(type)) ? type->valuestring : "";

    if (strcmp(t, "notify") == 0) {
        const cJSON* text = cJSON_GetObjectItem(root, "text");
        const cJSON* id   = cJSON_GetObjectItem(root, "id");
        const char* tx = cJSON_IsString(text) ? text->valuestring : "";
        utf8CopyTrunc(curId_, sizeof(curId_),
                      cJSON_IsString(id) ? id->valuestring : "");
        // 播放中来了新通知：停掉旧的，从头开始新的（复用收尾+激活序列）
        if (state_ == State::Playing) finish("superseded");

        showCard(tx);  // 消息卡 + 叮咚 + 亮屏直达详情（两种态都要上屏）

        // 互斥检查（TalkSession::start 同款）：talk 活跃或正在录音 →
        // 只上屏不播语音；notify_end 到时只清状态
        if (talk_->isActive() || app_->recState() != RecState::Idle) {
            state_ = State::Silent;
            silentSinceMs_ = millis();
            ESP_LOGI(TAG, "notify %s 到达（talk/录音中）：只上屏不播语音",
                     curId_[0] ? curId_ : "?");
            cJSON_Delete(root);
            return;
        }
        // 先开收帧闸门再激活：activate 里等叮咚+上电 24k 有 ~1s 窗口，
        // TTS 音频帧 60ms 内陆续到达，必须提前入队等泵来取（激活里不再清队列）
        accepting_ = true;
        activate();
    } else if (strcmp(t, "notify_end") == 0) {
        const cJSON* id = cJSON_GetObjectItem(root, "id");
        ESP_LOGI(TAG, "notify_end %s（state=%d）",
                 cJSON_IsString(id) ? id->valuestring : "?",
                 static_cast<int>(state_));
        if (state_ == State::Playing) {
            endPending_ = true;  // 泵排空队列后自退，tick 里 finish
        } else if (state_ == State::Silent) {
            state_ = State::Idle;   // 只上屏的通知：收尾即清态
        }
    }
    cJSON_Delete(root);
}

// 下行音频分片入抖动队列；accepting_（激活过程中）或播放态都收，其余丢弃
// （main 已把 talk 的帧分给 talk；Silent 态不收——设计上就不播）
void NotifySession::onAudioFrame(const uint8_t* payload, uint16_t len) {
    if ((!accepting_ && state_ != State::Playing) ||
        payload == nullptr || len == 0) return;
    app_->notifyActivity();  // 下行音频活动 = 亮屏续命
    lastFrameMs_ = millis();
    AudioChunk* c = static_cast<AudioChunk*>(malloc(sizeof(AudioChunk) + len));
    if (c == nullptr) {
        playDropped_++;
        return;
    }
    c->len = len;
    memcpy(c->data, payload, len);
    if (xQueueSend(jitterQ_, &c, 0) != pdTRUE) {
        // 队列满：丢最旧追平（与 TalkSession 同策略）
        AudioChunk* oldest = nullptr;
        if (xQueueReceive(jitterQ_, &oldest, 0) == pdTRUE && oldest) free(oldest);
        playDropped_++;
        if (xQueueSend(jitterQ_, &c, 0) != pdTRUE) free(c);
    }
}

// ---------------------------------------------------------------- UI ----
// 消息卡 + 叮咚 + 息屏亮屏直达详情（照抄 main.cpp reply 分支交互）：
// addNotify 头部插卡（"GAGA 提醒"），息屏时 openDetail 自带亮屏
void NotifySession::showCard(const char* text) {
    const int idx = log_->addNotify(text);
    app_->notifyActivity();
    audio_->event(AudioPipe::SndEv::Received);  // 三声 = 有新消息
    if (idx >= 0) {
        const uint32_t id = log_->at(idx)->id;
        if (app_->screen() == ScreenState::Off) {
            ui_->openDetail(id);
        } else {
            ui_->onLogChanged();
        }
    }
}

// ---------------------------------------------------------------- 生命周期 ----
// 播放态激活：等在飞叮咚作业落地 → 入队上电@24k → 等完成 → 检查通路 → 起泵
void NotifySession::activate() {
    ESP_LOGI(TAG, "notify %s 激活，开 24kHz 播放通路", curId_[0] ? curId_ : "?");
    demux_.reset();
    if (!dec_.begin()) {
        ESP_LOGE(TAG, "opus 解码器初始化失败");
    }
    // 不清抖动队列：accepting_ 已提前置位，早到的帧在队列里等泵（上一个通知
    // 的残留由 finish/pump 排空保证为空）
    playPackets_ = playBytesIn_ = playPcmOut_ = playUnderrun_ = playDropped_ = 0;
    lastFrameMs_ = 0;
    endPending_  = false;
    playSinceMs_ = millis();

    // 等叮咚播完再换档（TalkSession::activate 同款等待）
    for (int i = 0; i < 250 && audio_->beepBusy(); i++) vTaskDelay(pdMS_TO_TICKS(10));
    audio_->requestOpen(AudioPipe::RATE_TALK);
    for (int i = 0; i < 250 && audio_->beepBusy(); i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (!audio_->spkOpened()) {
        ESP_LOGE(TAG, "24kHz 播放通路打开失败，通知只上屏");
        finish("audio_open_fail");
        return;
    }
    audio_->setTalkActive(true);   // 播报期间提示音禁用（与 talk 同机制）
    state_ = State::Playing;

    playStop_ = false;
    // opus 解码栈需求大，播放泵给 24KB（栈放 PSRAM，见 compat.h；优先级参照 gaga_play）
    taskCreatePsram(playbackTask, "gaga_notify", 24576, this, 5, &playTask_, 0);
}

// 收尾：毒丸唤醒并等停播放泵 → 断电音频前端 → 解码器复位 → 状态回 Idle
// （泵先一步自然退出——notify_end 排空/超时自退——时这里为空转，幂等）
void NotifySession::finish(const char* reason) {
    if (playTask_ != nullptr) {
        playStop_ = true;
        AudioChunk* poison = nullptr;
        xQueueSend(jitterQ_, &poison, pdMS_TO_TICKS(100));
        // 等泵退出（最多 500ms）
        for (int i = 0; i < 50 && playTask_ != nullptr; i++) vTaskDelay(pdMS_TO_TICKS(10));
        if (playTask_ != nullptr) {
            ESP_LOGW(TAG, "播放泵未按期退出，强删");
            vTaskDelete(playTask_);
            playTask_ = nullptr;
        }
    }
    // 清空队列
    AudioChunk* c = nullptr;
    while (xQueueReceive(jitterQ_, &c, 0) == pdTRUE && c) free(c);

    audio_->requestClose();
    audio_->setTalkActive(false);
    dec_.end();
    accepting_ = false;

    ESP_LOGI(TAG, "notify %s 收尾（%s）；播放统计：包=%lu 入=%luB pcm=%lu 采样 欠载=%lu 丢帧=%lu",
             curId_[0] ? curId_ : "?", reason,
             static_cast<unsigned long>(playPackets_),
             static_cast<unsigned long>(playBytesIn_),
             static_cast<unsigned long>(playPcmOut_),
             static_cast<unsigned long>(playUnderrun_),
             static_cast<unsigned long>(playDropped_));
    state_ = State::Idle;
    endPending_ = false;
}

// ---------------------------------------------------------------- 主循环 tick ----
// 泵自然退出后的断电收尾（必须在 appTask 上下文做 requestClose）+ 超时兜底
void NotifySession::tick() {
    if (state_ == State::Playing) {
        if (playTask_ == nullptr) {
            finish(endPending_ ? "ended" : "timeout");  // 泵已自退（排空/超时）
        } else if (lastFrameMs_ == 0 &&
                   millis() - playSinceMs_ > FIRST_FRAME_TIMEOUT_MS) {
            ESP_LOGW(TAG, "notify %s 60s 无音频帧，收尾", curId_[0] ? curId_ : "?");
            finish("no_audio");   // notify 后首帧 60s 没来（长 TTS 合成期）：泵还在空等，毒丸收掉
        }
    } else if (state_ == State::Silent) {
        if (millis() - silentSinceMs_ > END_TIMEOUT_MS) state_ = State::Idle;
    }
}

// ---------------------------------------------------------------- 播放泵 ----
// 取分片 → Ogg 拆包 → Opus 解码 → 写扬声器（I2S DMA 满时阻塞 = 天然播放节奏）
// 退出两条路：①毒丸（finish 停旧/收尾）②自然——endPending 排空 或 10s 无帧超时，
// 自退后由 tick() 在 appTask 上下文断电收尾
void NotifySession::playbackTask(void* arg) {
    auto* self = static_cast<NotifySession*>(arg);
    int16_t* pcm = static_cast<int16_t*>(
        malloc(OpusDec::MAX_FRAME_SAMPLES * sizeof(int16_t)));

    // 抖动缓冲：8 块（~480ms）起步——notify 是提醒播报，延迟半秒无感，卡顿
    // 才致命（2026-09-28 实锤：2 块起步时，App MQTT→BLE 转发的首帧群被拉开
    // ~1-2s，薄垫被瞬间穿透 → 播放早期 3 次 200ms 欠载 = "卡来卡去"；endPending
    // 后跳过攒垫直接播尾）。断粮加厚沿用（封顶 10 块），talk 对话场景的 2 块
    // 起步不在此动（首字节延迟敏感）。
    bool primed = false;
    int primeNeed = 8;
    int smoothPackets = 0;
    for (;;) {
        if (self->playStop_) break;
        // notify_end 已到且队列播空：自然收尾（不等毒丸）
        if (self->endPending_ && uxQueueMessagesWaiting(self->jitterQ_) == 0) break;
        AudioChunk* c = nullptr;
        if (xQueueReceive(self->jitterQ_, &c, pdMS_TO_TICKS(200)) != pdTRUE) {
            self->playUnderrun_++;  // 200ms 无下行帧：I2S auto_clear 输出静默
            // 超时保护：最后一帧后 10s 仍无 notify_end → 自退
            if (!self->endPending_ && self->lastFrameMs_ != 0 &&
                millis() - self->lastFrameMs_ > END_TIMEOUT_MS) {
                ESP_LOGW(TAG, "最后一帧后 %lus 仍无 notify_end，泵自退",
                         END_TIMEOUT_MS / 1000);
                break;
            }
            // 断粮加厚只在"已开声"后才有意义（talk 长音频自适应，沿用）。
            // 2026-09-28 notify 短音频事故：priming 前等帧是常态（TTS 帧
            // 经 BLE 1~2s 才到齐），这里每 200ms 欠载把门槛 +2，4 帧的
            // 短通知门槛被吹到 6 → 永远 priming 不满足 → 泵空转到天荒地老
            // （实锤：入=0B 欠载=2，被下一条 superseded 才退）。开声前的
            // 等待不加厚。
            if (primed) {
                primed = false;
                primeNeed = primeNeed < 10 ? primeNeed + 2 : 10;
            }
            continue;
        }
        if (c == nullptr) break;  // 毒丸：finish 唤醒
        // priming 攒垫；notify_end 已到则跳过（剩下的播完就收尾，垫不垫无所谓——
        // 防短音频在 priming 门槛被抬高后卡死，见上）
        if (!primed && !self->endPending_ &&
            uxQueueMessagesWaiting(self->jitterQ_) + 1 < primeNeed) {
            xQueueSendToFront(self->jitterQ_, &c, 0);
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }
        primed = true;
        self->playBytesIn_ += c->len;
        self->demux_.feed(c->data, c->len);
        free(c);

        const uint8_t* pkt = nullptr;
        size_t pktLen = 0;
        while (self->demux_.nextPacket(&pkt, &pktLen)) {
            const int samples = self->dec_.decode(pkt, pktLen, pcm,
                                                  OpusDec::MAX_FRAME_SAMPLES);
            self->demux_.popPacket();
            if (samples <= 0) continue;
            self->playPackets_++;
            self->playPcmOut_ += samples;
            // 连续顺畅播放 50 包（~2s）降一档门槛（最低 2 块）
            if (++smoothPackets >= 50 && primeNeed > 2) {
                primeNeed--;
                smoothPackets = 0;
            }
            // 写扬声器（单声道 → 内部 L=R 复制；I2S DMA 满时阻塞 = 天然播放节奏）
            int left = samples;
            const int16_t* p = pcm;
            while (left > 0 && !self->playStop_) {
                const int wrote = self->audio_->spkWriteMono(p, left);
                if (wrote <= 0) break;
                p += wrote;
                left -= wrote;
            }
        }
    }

    free(pcm);
    ESP_LOGI(TAG, "播放泵退出");
    self->playTask_ = nullptr;
    vTaskDelete(NULL);
}

}  // namespace gaga
