#include "TalkSession.h"

#include <cstdlib>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "cJSON.h"

#include "ble/GattServer.h"

extern QueueHandle_t bleQ_;  // main 的 BLE 帧队列（下行到达速率仪表用）
#include "state/AppState.h"
#include "audio/AudioPipe.h"
#include "protocol/frame.h"
#include "compat.h"

namespace gaga {

static const char* TAG = "gaga.talk";

// 保存依赖，建下行抖动队列（装 AudioChunk* 指针）
void TalkSession::begin(Link* gatt, AppState* app, AudioPipe* audio) {
    gatt_  = gatt;
    app_   = app;
    audio_ = audio;
    if (jitterQ_ == nullptr) {
        jitterQ_ = xQueueCreate(JITTER_QUEUE_DEPTH, sizeof(AudioChunk*));
    }
}

bool TalkSession::isOff() const {
    return app_ == nullptr || app_->talkState() == TalkState::Off;
}

bool TalkSession::isActive() const {
    return app_ != nullptr && app_->talkState() == TalkState::Active;
}

// 下行正在出声（播放泵活着 且 300ms 内处理过数据 或 队列还有存货）
bool TalkSession::downlinkBusy() const {
    if (playTask_ == nullptr) return false;
    if (uxQueueMessagesWaiting(jitterQ_) > 0) return true;
    return (millis() - lastDownlinkMs_) < 300;
}

// ---------------------------------------------------------------- 生命周期 ----
// 开始会话：前置检查（互斥/联网）→ 状态进 Connecting → 发 talk_request 等 talk_ready
// provider：对话引擎选择（ADR-035，选择权在小设备；空串=服务端默认）
bool TalkSession::start(const char* provider) {
    if (!isOff()) return false;
    if (app_->recState() != RecState::Idle) {
        ESP_LOGW(TAG, "录音进行中，拒绝 talk_request");
        return false;
    }
    if (!gatt_->isConnected()) {
        ESP_LOGW(TAG, "BLE 未连接，talk_request 发不出去");
        return false;
    }
    if (!app_->setTalkState(TalkState::Connecting)) return false;
    if (provider && provider[0]) {
        char buf[64];
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"talk_request\",\"provider\":\"%s\"}", provider);
        ESP_LOGI(TAG, "talk_request →（provider=%s，等 talk_ready）", provider);
        gatt_->sendJson(buf);
    } else {
        ESP_LOGI(TAG, "talk_request →（服务端默认，等 talk_ready）");
        gatt_->sendJson("{\"type\":\"talk_request\"}");
    }
    return true;
}

// 本地结束会话；sendSignal=true 时补发 talk_end 让服务端一起收尾
void TalkSession::stop(bool sendSignal, const char* reason) {
    if (isOff()) return;
    ESP_LOGI(TAG, "本地结束 talk（%s）", reason);
    if (sendSignal && gatt_->isConnected()) {
        gatt_->sendJson("{\"type\":\"talk_end\"}");
    }
    teardown(reason);
}

// talk_ready 到达后激活：复位下行链路 → 走作业队列开 24k 双工 → 起播放泵
void TalkSession::activate() {
    ESP_LOGI(TAG, "talk_ready 到达，开 24kHz 双工音频通路");
    // 下行链路状态复位
    demux_.reset();
    if (!dec_.begin()) {
        ESP_LOGE(TAG, "opus 解码器初始化失败");
    }
    // 清抖动队列
    AudioChunk* junk = nullptr;
    while (xQueueReceive(jitterQ_, &junk, 0) == pdTRUE && junk) free(junk);
    playPackets_ = playBytesIn_ = playPcmOut_ = playUnderrun_ = playDropped_ = 0;

    // 换档走音频作业队列（ADR-027 全串行化）：等在飞作业落地 → 入队上电@24k
    //（对已开不同档=换档）→ 等完成 → 检查通路。
    for (int i = 0; i < 250 && audio_->beepBusy(); i++) vTaskDelay(pdMS_TO_TICKS(10));
    audio_->requestOpen(AudioPipe::RATE_TALK);
    for (int i = 0; i < 250 && audio_->beepBusy(); i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (!audio_->spkOpened() || !audio_->micOpened()) {
        ESP_LOGE(TAG, "24kHz 双工打开失败，会话终止");
        teardown("audio_open_fail");
        if (errCb_) errCb_("AUDIO_FAIL", "双工通路打开失败");
        return;
    }
    audio_->setTalkActive(true);   // talk 期间提示音禁用
    app_->setTalkState(TalkState::Active);

    playStop_ = false;
    // opus 解码栈需求大，播放泵给 24KB（栈放 PSRAM，见 compat.h）
    taskCreatePsram(playbackTask, "gaga_play", 24576, this, 5, &playTask_, 0);
    ESP_LOGI(TAG, "[talk] active（上行泵由主循环驱动）");
}

// 会话收尾：毒丸唤醒并等停播放泵 → 清队列 → 断电音频前端 → 状态回 Off → 通知 UI
void TalkSession::teardown(const char* reason) {
    if (playTask_ != nullptr) {
        playStop_ = true;
        // 唤醒可能阻塞在队列上的播放泵
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

    // 音频前端断电回零耗（ADR-027：闲置=全关，下次会话再上电）
    audio_->requestClose();
    audio_->setTalkActive(false);
    dec_.end();

    app_->setTalkState(TalkState::Off);
    ESP_LOGI(TAG, "[talk] ended（%s）；播放统计：包=%lu 入=%luB pcm=%lu 采样 欠载=%lu 丢帧=%lu",
             reason,
             static_cast<unsigned long>(playPackets_),
             static_cast<unsigned long>(playBytesIn_),
             static_cast<unsigned long>(playPcmOut_),
             static_cast<unsigned long>(playUnderrun_),
             static_cast<unsigned long>(playDropped_));
    if (endCb_) endCb_(reason);
}

// ---------------------------------------------------------------- 下行入口 ----
// 处理 talk JSON 信令：talk_ready 激活 / talk_end 与 error 收尾 / asr、reply 文本上屏
void TalkSession::onSignalJson(const char* json) {
    if (json == nullptr) return;
    cJSON* root = cJSON_Parse(json);
    if (root == nullptr) return;
    const cJSON* type = cJSON_GetObjectItem(root, "type");
    const char* t = (cJSON_IsString(type)) ? type->valuestring : "";

    if (strcmp(t, "talk_ready") == 0) {
        if (app_->talkState() == TalkState::Connecting) {
            const cJSON* sess = cJSON_GetObjectItem(root, "session");
            ESP_LOGI(TAG, "talk_ready session=%s",
                     cJSON_IsString(sess) ? sess->valuestring : "?");
            activate();
        }
    } else if (strcmp(t, "talk_end") == 0) {
        const cJSON* reason = cJSON_GetObjectItem(root, "reason");
        const char* r = cJSON_IsString(reason) ? reason->valuestring : "server_end";
        ESP_LOGI(TAG, "服务端 talk_end reason=%s", r);
        if (!isOff()) teardown(r);
    } else if (strcmp(t, "talk_asr") == 0 || strcmp(t, "talk_reply") == 0) {
        const cJSON* text  = cJSON_GetObjectItem(root, "text");
        const cJSON* final = cJSON_GetObjectItem(root, "final");
        if (cJSON_IsString(text)) {
            app_->notifyActivity();  // 下行文本活动=亮屏续命（ADR-016 息屏逻辑）
            if (textCb_) textCb_(strcmp(t, "talk_reply") == 0,
                                 text->valuestring, cJSON_IsTrue(final));
        }
    } else if (strcmp(t, "error") == 0) {
        const cJSON* code = cJSON_GetObjectItem(root, "code");
        const cJSON* msg  = cJSON_GetObjectItem(root, "msg");
        const char* c = cJSON_IsString(code) ? code->valuestring : "ERROR";
        const char* m = cJSON_IsString(msg) ? msg->valuestring : "";
        ESP_LOGW(TAG, "服务端 error: %s %s", c, m);
        if (!isOff()) teardown(c);
        if (errCb_) errCb_(c, m);
    }
    cJSON_Delete(root);
}

// 下行音频分片入抖动队列；队列满丢最旧（追平直播节奏，不堆积延迟）
void TalkSession::onAudioFrame(const uint8_t* payload, uint16_t len) {
    if (!isActive() || payload == nullptr || len == 0) return;
    app_->notifyActivity();  // 下行音频活动=亮屏续命
    // 下行到达速率仪表（每秒一行）：与服务器发布速率对比，定位积压在哪一跳
    downBytes_ += len;
    downChunks_++;
    const uint32_t now = millis();
    if (now - downLastLog_ >= 1000) {
        ESP_LOGI(TAG, "[down] 音频到达 %luB/s（%lu 块/s，BLE队列=%u）",
                 static_cast<unsigned long>(downBytes_),
                 static_cast<unsigned long>(downChunks_),
                 static_cast<unsigned>(uxQueueMessagesWaiting(bleQ_)));
        downBytes_ = 0;
        downChunks_ = 0;
        downLastLog_ = now;
    }
    AudioChunk* c = static_cast<AudioChunk*>(malloc(sizeof(AudioChunk) + len));
    if (c == nullptr) {
        playDropped_++;
        return;
    }
    c->len = len;
    memcpy(c->data, payload, len);
    if (xQueueSend(jitterQ_, &c, 0) != pdTRUE) {
        // 队列满：丢最旧追平（ADR-021 服务端节奏器同策略思想）
        AudioChunk* oldest = nullptr;
        if (xQueueReceive(jitterQ_, &oldest, 0) == pdTRUE && oldest) free(oldest);
        playDropped_++;
        if (xQueueSend(jitterQ_, &c, 0) != pdTRUE) free(c);
    }
}

// ---------------------------------------------------------------- 播放泵 ----
// 取分片 → Ogg 拆包 → Opus 解码 → 写扬声器（I2S DMA 满时阻塞 = 天然播放节奏）
void TalkSession::playbackTask(void* arg) {
    auto* self = static_cast<TalkSession*>(arg);
    int16_t* pcm = static_cast<int16_t*>(
        malloc(OpusDec::MAX_FRAME_SAMPLES * sizeof(int16_t)));
    uint32_t lastLog = 0;

    // 自适应抖动缓冲（2026-09-25 v3）：固定厚垫虽治卡顿但句间停顿也要重攒
    // （用户体感"说完一段等很久"）。改为：垫子从 2 块（~80ms）起步，
    // 真断粮才加厚（+2/次，封顶 10 块=400ms），播放顺畅时缓慢变薄——
    // 网络好时延迟最低，网络抖时自动变稳
    bool primed = false;
    int primeNeed = 2;
    int smoothPackets = 0;
    for (;;) {
        if (self->playStop_) break;
        AudioChunk* c = nullptr;
        if (xQueueReceive(self->jitterQ_, &c, pdMS_TO_TICKS(200)) != pdTRUE) {
            self->playUnderrun_++;  // 200ms 无下行帧：I2S auto_clear 输出静默
            if (primed || primeNeed < 10) {
                primed = false;
                primeNeed = primeNeed < 10 ? primeNeed + 2 : 10;  // 断粮→垫子加厚
            }
            continue;
        }
        if (c == nullptr) break;  // 毒丸：teardown 唤醒
        // 未达门槛：先还回队列攒垫（差几块等几块，每块 ~40ms）
        if (!primed && uxQueueMessagesWaiting(self->jitterQ_) + 1 < primeNeed) {
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
            // 连续顺畅播放 50 包（~2s）降一档门槛（最低 2 块）：网络好时延迟回归最低
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

        const uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - lastLog >= 1000) {
            lastLog = now;
            ESP_LOGI(TAG, "[play] 包=%lu pcm=%lu 采样 队列=%u 欠载=%lu 丢=%lu 页=%lu 重同步=%lu",
                     static_cast<unsigned long>(self->playPackets_),
                     static_cast<unsigned long>(self->playPcmOut_),
                     uxQueueMessagesWaiting(self->jitterQ_),
                     static_cast<unsigned long>(self->playUnderrun_),
                     static_cast<unsigned long>(self->playDropped_),
                     static_cast<unsigned long>(self->demux_.pages()),
                     static_cast<unsigned long>(self->demux_.resyncs()));
        }
    }

    free(pcm);
    ESP_LOGI(TAG, "播放泵退出");
    self->playTask_ = nullptr;
    vTaskDelete(NULL);
}

}  // namespace gaga
