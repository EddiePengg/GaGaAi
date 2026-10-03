#include "audio/UplinkPump.h"

#include <cstring>

#include <cstdio>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"

#include "AppContext.h"
#include "state/Settings.h"
#include "audio/AudioPipe.h"
#include "audio/OpusCodec.h"
#include "ble/GattServer.h"
#include "compat.h"
#include "version.h"
#include "protocol/frame.h"
#include "state/AppState.h"
#include "state/MsgLog.h"
#include "talk/TalkSession.h"
#include "ui/Ui.h"

namespace gaga {

static const char* TAG = "gaga.pump";

// 栈水位实锤（2026-09-29 'h' 观测）：boot 自检后已用 32048/32768（剩 720B），
// 真机录音（语音激励 opus + 缓存补发链）存在打穿 32K 的现实路径——打穿即溢出
// 踩进 PSRAM 堆邻块（TCB/队列结构体全在 PSRAM），与全天"互斥锁断言/链表
// 0xffffffff/金丝雀冤案"同族。PSRAM 余量 4.6MB，直接给到 48K 买断风险。
static constexpr uint32_t UPLINK_TASK_STACK = 49152;  // opus_encode 栈需求大

void UplinkPump::begin(AppContext* ctx) {
    ctx_ = ctx;
    // 确认窗缓冲 PSRAM 惰性分配（成员原为静态数组，见头文件注释）
    bufPkts_ = static_cast<uint8_t (*)[REC_BUF_PKT_SZ]>(heap_caps_malloc(
        REC_BUF_PKTS * REC_BUF_PKT_SZ, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (bufPkts_ == nullptr) {
        ESP_LOGE(TAG, "确认窗缓冲 PSRAM 分配失败（%dB）", REC_BUF_PKTS * REC_BUF_PKT_SZ);
    }
    taskCreatePsram(taskEntry, "gaga_uplink", UPLINK_TASK_STACK, this, 6, nullptr, 1);
}

void UplinkPump::taskEntry(void* arg) {
    // 卡死自保：见 main.cpp appTask 的 TWDT 注释（音频泵 wedge → 5s 整机自复位）
    esp_task_wdt_add(nullptr);
    static_cast<UplinkPump*>(arg)->run();
}

void UplinkPump::beginSession(uint32_t seq) {
    sessionSeq_ = seq;   // 本段 rec_seq（随 rec_start 上行、占位卡配对用，ADR-070）
    framesSent_ = 0;
    framesDrop_ = 0;
    recRmsSum_  = 0;
    committed_  = false;
    silenceRunMs_   = 0;   // 静音自动收尾统计（2026-09-28）
    voicedMs_       = 0;
    recFrames_      = 0;
    autoStopReq_    = false;
    autoDiscard_    = false;
    sessionStartMs_ = millis();
    bufCount_   = 0;
    sessionLive_ = true;
    linkSawDown_ = false;      // 离线粘性标记：每段录音重新起判
    if (capLen_ > 0) capMarkSegment();  // 缓存里还有前一条：插分界，补发不粘连
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
    // ⚠️ 必须内部 RAM（2026-09-29 判别实验二：PSRAM 版录音"无语音内容"+卡死，
    // 退回定罪/脱罪。机制假说：泵任务栈在 PSRAM，数据缓冲再进 PSRAM 后撞
    // cache freeze 窗口/缓存一致性 → 空数据）。这 1920B 先认怂。
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
    if (!isTalk && ctx_->app->recState() == RecState::Recording) watchVoice(rms);
    uint8_t pkt[256];
    const int n = ctx_->enc->encode(mono320, pkt, sizeof(pkt));
    if (n <= 0) {
        ESP_LOGW(TAG, "[rec] opus 编码失败，丢帧");
        return;
    }
    // 离线缓存：录音中的每一帧同步进 PSRAM（断链时整段可补发，别白说）
    if (!isTalk && ctx_->app->recState() == RecState::Recording) {
        capAppend(pkt, static_cast<uint16_t>(n));
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
    // 计数要诚实：sendFrame 真的把包推进了 NimBLE 才算 sent（.agents/bug-analysis-0923 §3a）
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
    // ⚠️ 必须内部 RAM（2026-09-29 判别实验二，同 stereoBuf 一案）。
    static int16_t mono480[480];  // 下混后单声道（24k 一帧 480 采样为上限）
    static int16_t mono320[OpusEnc::FRAME_SAMPLES];
    for (;;) {
        esp_task_wdt_reset();   // TWDT 订阅者自喂（漏喂=每 5s 误报，见 main.cpp）
        watchRecState();
        const uint32_t now = millis();
        // 对账查询（2026-09-26；2026-09-27 扩到 Waiting + ADR-059 上限退避；
        // 2026-09-29 风暴源封顶）：
        //   Sending 卡 [10s, 120s]、Waiting 卡 [10s, 5min] 窗口内追询——服务端
        //   幂等重发，设备侧由 MsgLog 去重环对重复信令免疫（见 msglog 头注释）。
        //   间隔按"追没追回东西"自适应：账本版本没动就 +10s 退避到 30s 封顶；
        //   连续 8 次纹丝不动 = 这轮追不回来 → 休眠 10 分钟（账本一动立即唤醒）。
        //   无此封顶时，服务端有卡永远"没回复"会造成 10~30s 一轮的无限对刷
        //   （2026-09-29 实测 30 分钟几百条，还把新卡劫持去配旧回执）。
        // 账本动了 = 来了新卡/新 receipt：提前结束休眠，别让它等满 10 分钟
        if (queryCooldownUntilMs_ != 0 &&
            lastQueryVersion_ != 0 &&
            lastQueryVersion_ != ctx_->log->version()) {
            queryCooldownUntilMs_ = 0;
            queryNoChangeN_ = 0;
        }
        if (ctx_->app->recState() == RecState::Idle &&
            now >= queryCooldownUntilMs_ &&
            ctx_->app->mqttLink() && ctx_->link != nullptr &&
            ctx_->link->isConnected() &&
            (ctx_->log->hasStaleSending(10000, 120000) ||
             ctx_->log->hasStaleWaiting(10000, 300000)) &&
            now - lastStatusQueryMs_ >= queryIntervalMs_) {
            if (lastQueryVersion_ != 0 &&
                lastQueryVersion_ == ctx_->log->version()) {
                queryIntervalMs_ = queryIntervalMs_ >= 30000
                                       ? 30000 : queryIntervalMs_ + 10000;
                // 风暴源封顶（2026-09-29 卡片劫持案）：服务端有卡永远"没回复"
                // （reply 丢帧/桥断）时，版本永远不动 → 10s~30s 一轮的查询和
                // 服务端整批重发会无限对刷（实测 30 分钟刷了几百条）。账本
                // 连续 8 次纹丝不动 = 这轮追不回来了：休眠 10 分钟再试，有新
                // 卡/新 receipt 时版本变化会提前唤醒计数器（下面 else 归零）。
                if (queryIntervalMs_ >= 30000 && ++queryNoChangeN_ >= 8) {
                    queryNoChangeN_ = 0;
                    queryCooldownUntilMs_ = now + 600000;
                    ESP_LOGW(TAG, "[cache] 对账连续 8 次无进展，休眠 10 分钟（防风暴对刷）");
                }
            } else {
                queryIntervalMs_ = 10000;
                queryNoChangeN_ = 0;
            }
            lastQueryVersion_ = ctx_->log->version();
            lastStatusQueryMs_ = now;
            ctx_->link->sendJson("{\"type\":\"rec_status_query\",\"device\":\"" DEVICE_ID "\"}");
            ESP_LOGI(TAG, "[cache] 回执超时，向服务端对账查询（间隔 %lus）",
                     static_cast<unsigned long>(queryIntervalMs_ / 1000));
        }
        // 待补发 + 链路齐备 + 空闲 + 退避到期 → 整段重放（同步执行：此刻泵没别的事）
        if (capPending_ && capLen_ > 0 && millis() >= capRetryAtMs_ &&
            ctx_->app->recState() == RecState::Idle &&
            ctx_->app->talkState() == TalkState::Off &&
            ctx_->link != nullptr && ctx_->link->isConnected() &&
            ctx_->app->mqttLink()) {
            capReplay();
        }
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

// ---- 离线缓存（2026-09-25 用户需求：断链别白说）----

// 追加一帧到 PSRAM 缓存（[len16][payload]）。惰性分配、按 512KB 步进增长，
// 到 8 分钟上限就封顶（录音照常上行，只是缓存不再增长）。
void UplinkPump::capAppend(const uint8_t* pkt, uint16_t n) {
    if (capFull_) return;
    if (capLen_ + 2 + n > capCap_) {
        const uint32_t want = (capCap_ == 0) ? (256 * 1024) : (capCap_ + 512 * 1024);
        if (want > CAP_MAX_BYTES) {
            capFull_ = true;
            ESP_LOGW(TAG, "[cache] 缓存到顶（%lus 语音），后续帧只上行不缓存",
                     static_cast<unsigned long>(CAP_MAX_BYTES / 1024 / 50));
            return;
        }
        uint8_t* p = static_cast<uint8_t*>(
            heap_caps_realloc(capBuf_, want, MALLOC_CAP_SPIRAM));
        if (p == nullptr) {
            capFull_ = true;
            ESP_LOGW(TAG, "[cache] PSRAM 扩容失败，缓存封顶");
            return;
        }
        capBuf_ = p;
        capCap_ = want;
    }
    capBuf_[capLen_++] = static_cast<uint8_t>(n & 0xFF);
    capBuf_[capLen_++] = static_cast<uint8_t>((n >> 8) & 0xFF);
    memcpy(capBuf_ + capLen_, pkt, n);
    capLen_ += n;
    capFrames_++;
}

// 消息分界标记（2026-09-26 三合一 bug）：缓存流里插入 0xFFFF 两字节。
// 真帧长 ≤256B，0xFFFF 不可能撞车。beginSession 时若缓存非空就插一个
// ——补发按段各发 rec_start/rec_stop，N 条离线消息 = N 个独立会话。
// rec_seq 扩展（ADR-070，2026-09-29）：段头扩为 6 字节 = 0xFFFF + uint32
// rec_seq（小端，memcpy 直写）——每段离线消息带着自己的序号补发，服务端
// receipt/error 原样带回，设备精确配对回原卡（不再 FIFO 盲配）。缓存是易失
// PSRAM，重启即清空，格式变更无需向后兼容
void UplinkPump::capMarkSegment() {
    if (capLen_ + 6 > capCap_) {
        if (capLen_ + 6 > CAP_MAX_BYTES) {
            capFull_ = true;
            return;
        }
        const uint32_t want = (capCap_ == 0) ? (256 * 1024) : (capCap_ + 512 * 1024);
        uint8_t* p = static_cast<uint8_t*>(
            heap_caps_realloc(capBuf_, want, MALLOC_CAP_SPIRAM));
        if (p == nullptr) {
            capFull_ = true;
            return;
        }
        capBuf_ = p;
        capCap_ = want;
    }
    capBuf_[capLen_++] = 0xFF;
    capBuf_[capLen_++] = 0xFF;
    memcpy(capBuf_ + capLen_, &sessionSeq_, sizeof(sessionSeq_));  // 小端直写
    capLen_ += sizeof(sessionSeq_);
}

// rec 状态迁移的账本：Sending→Idle（8s 无回执）时判断是否真丢了数据——
// 全帧送达只是 ASR 慢（正常，receipt 迟早来，清缓存防重复补发）；
// 丢过帧或 MQTT 断了才是真失败（标记待补发）。→Sent = 送达（同样清）。
void UplinkPump::watchVoice(uint32_t rms) {
    if (autoStopReq_) return;   // 请求已置位，等 Recorder 执行收尾
    recFrames_++;
    if (rms >= SILENCE_RMS_THR) {
        voicedMs_ += 20;        // 20ms/帧
        silenceRunMs_ = 0;
    } else {
        silenceRunMs_ += 20;
    }
    const uint32_t durMs = millis() - sessionStartMs_;
    if (silenceRunMs_ >= SILENCE_STOP_MS || durMs >= MAX_REC_MS) {
        autoDiscard_ = voicedMs_ < MIN_VOICE_MS;   // 整段几乎无语音 → 静默撤销
        autoStopReq_ = true;
        ESP_LOGI(TAG, "[sil] 自动收尾：连续静音=%lums 段长=%lums 有效语音=%lums 均RMS=%lu → %s",
                 static_cast<unsigned long>(silenceRunMs_),
                 static_cast<unsigned long>(durMs),
                 static_cast<unsigned long>(voicedMs_),
                 recFrames_ ? static_cast<unsigned long>(recRmsSum_ / recFrames_) : 0,
                 autoDiscard_ ? "静默丢弃" : "正常发送");
    }
}

void UplinkPump::watchRecState() {
    const int nowRec = static_cast<int>(ctx_->app->recState());
    // 粘性标记（2026-09-26 离线录音）：本段录音期间手机侧 MQTT 断过一次即置位。
    // 帧被 App 丢弃时设备的 framesDrop_ 不会涨（BLE 层是成功的），只在超时
    // 时刻看 mqttLink 会漏掉"断过又恢复"的窗口 → 缓存被误清、音频永久丢失。
    if (!ctx_->app->mqttLink()) linkSawDown_ = true;
    if (prevRecState_ == static_cast<int>(RecState::Sending) &&
        nowRec == static_cast<int>(RecState::Idle)) {
        if (framesDrop_ > 0 || linkSawDown_) {
            capMarkPending();
        } else {
            if (capLen_ > 0) {
                ESP_LOGI(TAG, "[cache] 全帧送达（ASR 慢而已），清缓存防重复补发");
            }
            capClear();   // 服务器已收到全部帧：绝不重复补发
        }
    }
    if (nowRec == static_cast<int>(RecState::Sent) && !capPending_) {
        capClear();   // 已送达（保留 capBuf_ 分配，下段复用）
    }
    prevRecState_ = nowRec;
}

void UplinkPump::capClear() {
    capLen_ = 0;
    capFrames_ = 0;
    capFull_ = false;
    capPending_ = false;
    capDurMs_ = 0;
    capDoneOff_ = 0;   // 补发续传点一并归零
}

void UplinkPump::capMarkPending() {
    if (capLen_ == 0) return;
    capPending_ = true;
    capDurMs_ = capFrames_ * 20;   // 20ms/帧
    ESP_LOGW(TAG, "[cache] 本段录音未送达：缓存 %u 帧 / %lus（capLen=%uKB），链路恢复自动补发",
             static_cast<unsigned>(capFrames_),
             static_cast<unsigned long>(capDurMs_ / 1000),
             static_cast<unsigned>(capLen_ / 1024));
}

// 整段重放：rec_start → 缓存帧 → rec_stop（模拟一次完整录音上行）。
// 只在空闲态调用；按直播同速 20ms/帧节流（全速 blast 会打爆 NimBLE 队列，
// 真机实锤 2026-09-25：失败→从头再来死循环，1.4 万条 notify 刷屏）。
// 段落语义（2026-09-26 三合一 bug 修复）：缓存流按 0xFFFF 分界标记切成
// N 条消息，每段独立走 rec_start → 帧 → rec_stop——服务端各自出各自的
// ASR/飞书消息/receipt，不再合成一句话。段头 6 字节（ADR-070）：0xFFFF +
// uint32 rec_seq（小端），seq 属其后那段——补发 rec_start 原样携带，服务端
// receipt/error 带回后设备按号精确配对回原卡。capDoneOff_ 记录最近一个已完成
// rec_stop 的段尾：中途断链/让路后重试从这续，已完成的段不重复发
//（服务端无去重，重发=群里双消息）。段时长以 帧数×20ms 计（每帧即 20ms
// 音频，rec_stop 的 duration_ms 只是服务端日志信息，无需精确）。
void UplinkPump::capReplay() {
    capPending_ = false;
    ESP_LOGW(TAG, "[cache] 链路恢复，补发离线录音（%u 帧 / 从 %uB 续）",
             static_cast<unsigned>(capFrames_),
             static_cast<unsigned>(capDoneOff_));
    ctx_->ui->showNote("补发离线录音…");
    constexpr uint16_t kSegMark = 0xFFFF;
    bool inSession = false;
    uint32_t segSeq = 0;   // 当前段的 rec_seq（段头里读的；0=旧格式/无号）
    uint32_t segFrames = 0, totalSent = 0, segs = 0;
    uint32_t off = capDoneOff_;
    while (off + 2 <= capLen_) {
        // 喂狗（2026-09-28 误杀事故修复）：本循环 18ms/帧埋头重放，长的段
        // （>277 帧 ≈5.5s 语音）会超过 TWDT 5s 时限——任务实际没卡死，只是
        // 回不到 run() 顶部的 esp_task_wdt_reset()，被看门狗误判卡死而复位
        // （真机实锤：12s 录音补发 609 帧 ≈11s → 必死，重启循环闭环）。
        esp_task_wdt_reset();
        // 用户开始新录音：补发让路（缓存未清，录音结束退避后继续）
        if (ctx_->app->recState() == RecState::Recording) {
            capPending_ = true;
            capRetryAtMs_ = millis() + 10000;
            return;
        }
        const uint16_t n = static_cast<uint16_t>(capBuf_[off] | (capBuf_[off + 1] << 8));
        off += 2;
        if (n == kSegMark) {
            if (inSession) {
                if (!sendSegStop(segFrames)) { bailReplay(); return; }
                segs++;
            }
            inSession = false;
            // 段头续 4 字节 rec_seq（小端直读）；不够 = 缓存截断，别越界
            if (off + 4 > capLen_) break;
            memcpy(&segSeq, capBuf_ + off, sizeof(segSeq));
            off += 4;
            capDoneOff_ = off;
            continue;
        }
        if (off + n > capLen_) break;
        if (!inSession) {
            // 补发段的 rec_start 同样带 name（ADR-064）：离线补发的消息也要正确署名
            // + rec_seq（ADR-070）：段头里存的号原样上行，receipt 按它精确配对
            char rs[200];
            const char* nm = ctx_->settings->devName();
            if (nm[0] != '\0')
                snprintf(rs, sizeof(rs),
                         "{\"type\":\"rec_start\",\"device\":\"" DEVICE_ID "\","
                         "\"name\":\"%s\",\"rec_seq\":%lu}", nm,
                         static_cast<unsigned long>(segSeq));
            else
                snprintf(rs, sizeof(rs),
                         "{\"type\":\"rec_start\",\"device\":\"" DEVICE_ID "\","
                         "\"rec_seq\":%lu}", static_cast<unsigned long>(segSeq));
            if (!ctx_->link->sendJson(rs)) { bailReplay(); return; }
            inSession = true;
            segFrames = 0;
        }
        if (!ctx_->link->isConnected() ||
            !ctx_->link->sendFrame(FRAME_TYPE_OPUS, capBuf_ + off, n)) {
            bailReplay();   // 中途又断：缓存未清，恢复待补发
            return;
        }
        segFrames++;
        totalSent++;
        off += n;
        vTaskDelay(pdMS_TO_TICKS(18));   // 与直播同速，队列不爆
    }
    if (inSession) {
        if (!sendSegStop(segFrames)) { bailReplay(); return; }
        segs++;
        capDoneOff_ = off;
    }
    ESP_LOGW(TAG, "[cache] 补发完成：本轮 %u 段 / %u 帧",
             static_cast<unsigned>(segs), static_cast<unsigned>(totalSent));
    ctx_->ui->showNote("✓ 离线录音已补发");
    // 失败卡复活：红 → 黄（Sending）。补发已送达服务端正在识别，随后的
    // receipt（ASR 文本）经 fillAsk 自然填回卡片——此前卡片非 Sending 态，
    // ASR 结果被丢弃 = 用户看到的"空卡"（2026-09-26 修复）
    if (ctx_->log->failToSending() >= 0) ctx_->ui->onLogChanged();
    capClear();
}

// 段收尾信令；false = 链路断了（调用方走退避重试）
bool UplinkPump::sendSegStop(uint32_t frames) {
    char stop[64];
    snprintf(stop, sizeof(stop),
             "{\"type\":\"rec_stop\",\"duration_ms\":%lu,\"device\":\"%s\"}",
             static_cast<unsigned long>(frames * 20), DEVICE_ID);
    return ctx_->link->sendJson(stop);
}

// 补发中止的统一出口：保住缓存与续传点，10s 后再试
void UplinkPump::bailReplay() {
    capPending_ = true;
    capRetryAtMs_ = millis() + 10000;
}

}  // namespace gaga
