#pragma once

#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "audio/OggDemux.h"
#include "audio/OpusCodec.h"

// 服务器主动 TTS 语音通知（notify）：非 talk 会话期间服务器推送的语音播报。
// 下行序列（protocol.md，与 talk 下行音频完全同格式）：
//   {"type":"notify","text":"...","id":"n_..."} —— 亮屏 + 消息卡 + 叮咚 + 准备播放
//   若干 type=0x01 帧（ogg_opus 24k 单声道分片，按到达顺序拼接 = 完整 Ogg 流）
//   {"type":"notify_end","id":"n_..."}          —— 排空播完收尾
// 与 talk/录音互斥（TalkSession::start 同款检查）：到达时 talk 活跃或正在录音
//   → 只上屏不播语音（Silent 态），notify_end 到时只清状态。
// 超时保护：最后一帧后 10s 仍无 notify_end → 自动收尾；notify 后 10s 一帧
//   没来同样收尾（防服务器半道失联把播放泵挂死）。
// 播放链路与 TalkSession 同范式：抖动队列 → OggDemux 拆包 → OpusDec 解码 →
//   spkWriteMono（I2S 满时阻塞 = 天然节奏）；上电/断电走 AudioPipe 作业队列。
namespace gaga {

class AppState;
class AudioPipe;
class MsgLog;
class Ui;
class TalkSession;

class NotifySession {
public:
    void begin(AppState* app, AudioPipe* audio, MsgLog* log, Ui* ui, TalkSession* talk);

    // BLE 下行入口（app 事件泵调用）：notify 开始 / notify_end 收尾
    void onSignalJson(const char* json);
    // 下行 ogg_opus 分片：accepting_（notify 到达即置位，不等播放通路就绪）或
    // 播放态入抖动队列；其余丢弃
    void onAudioFrame(const uint8_t* payload, uint16_t len);
    // 主循环 tick（appTask 10ms 节拍）：泵自然退出后的断电收尾 + 超时兜底
    void tick();
    bool isPlaying() const { return state_ == State::Playing; }

private:
    // 状态机：Idle（无通知）→ Playing（上屏 + 播放中）/ Silent（只上屏）
    enum class State : uint8_t { Idle, Playing, Silent };

    void showCard(const char* text);   // 消息卡 + 叮咚 + 息屏亮屏直达详情（照抄 reply 分支）
    void activate();                   // 播放态激活：等叮咚落地 → 上电 24k → 起播放泵
    void finish(const char* reason);   // 收尾：毒丸停泵 → 断电 → 解码器复位 → 回 Idle
    static void playbackTask(void* arg);

    // 下行帧队列（jitter buffer）：元素 = malloc 的 {len, bytes}（与 TalkSession 同款）
    struct AudioChunk {
        uint16_t len;
        uint8_t  data[];  // 柔性数组
    };
    static constexpr UBaseType_t JITTER_QUEUE_DEPTH = 128;  // 帧粒度（2026-09-28 晚
    // 64→128：马赛克音实锤——WiFi 打盹 stall→TCP 爆发送达把 64 深打满丢 18 帧，
    // Ogg 流失同步糊到下一页。128 ≈ 2.6s 缓冲吸收打盹突发；payload 走 PSRAM 不贵
    // notify_end 丢失兜底：最后一帧后 10s 仍没收尾 → 超时自退
    static constexpr uint32_t END_TIMEOUT_MS = 10000;
    // 首帧等待窗口（2026-09-28 晚独立出来）：长文本 TTS 合成要 20-30s，原复用
    // END_TIMEOUT(10s) 会在合成完成前误杀会话（音频赶到无人收，长通知报废）。
    // 60s 覆盖千字级合成；真·无音频的垃圾通知最坏多等一分钟，可接受。
    static constexpr uint32_t FIRST_FRAME_TIMEOUT_MS = 60000;

    AppState*    app_   = nullptr;
    AudioPipe*   audio_ = nullptr;
    MsgLog*      log_   = nullptr;
    Ui*          ui_    = nullptr;
    TalkSession* talk_  = nullptr;

    State state_ = State::Idle;
    char  curId_[32] = "";       // 当前通知 id（日志用；notify_end 不严格配对）

    QueueHandle_t jitterQ_  = nullptr;
    TaskHandle_t  playTask_ = nullptr;
    volatile bool playStop_   = false;   // 毒丸外的人工停泵标志（finish 用）
    volatile bool endPending_ = false;   // notify_end 已到：泵排空队列后自退
    volatile bool accepting_  = false;   // notify 到达即置位：激活过程中先收帧入队
                                         // （TTS 帧 60ms 内就到，等叮咚+上电的
                                         // ~1s 窗口不能丢帧），finish 时清零
    volatile uint32_t lastFrameMs_ = 0;  // 最后一帧到达时刻（泵内超时判定）
    uint32_t playSinceMs_   = 0;         // 进入播放态时刻（一帧没来兜底判定）
    uint32_t silentSinceMs_ = 0;         // 进入 Silent 态时刻（兜底清态）

    OggDemux demux_;
    OpusDec  dec_;

    // 播放统计（日志用）
    uint32_t playPackets_  = 0;
    uint32_t playBytesIn_  = 0;
    uint32_t playPcmOut_   = 0;
    uint32_t playUnderrun_ = 0;
    uint32_t playDropped_  = 0;
};

}  // namespace gaga
