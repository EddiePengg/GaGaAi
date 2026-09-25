#pragma once

#include <cstdint>
#include <functional>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "audio/OggDemux.h"
#include "audio/OpusCodec.h"
#include "net/Link.h"

// M5 实时对话会话（protocol.md §6 / data-model.md §1 / ADR-021）：
//   右上长按 → start() 发 talk_request → 收 talk_ready → Active（全双工）
//     上行：采集泵在 main.cpp（24k 采集 → 2:3 抽取 → Opus 16k/20ms → type=0x01 帧）
//     下行：type=0x01 帧（ogg_opus 24k 分片）→ feedAudio 入队 → 播放泵解 Ogg/Opus
//           → ES8311 播放（jitter buffer = 帧队列，溢出丢最旧）
//   结束：本地 stop()（发 talk_end）或服务端 talk_end/error 下行
// 打断（barge-in）：协议层面由服务端 response.cancel 主导（ADR-021/protocol.md §6），
//   本期固件不做主动打断——上行帧照常发，全双工模型自己判打断。
namespace gaga {

class GattServer;
class AppState;
class AudioPipe;

class TalkSession {
public:
    // 文本事件（talk_asr/talk_reply 上屏）：isReply=false 用户语音 ASR，true 模型回复
    using TextCallback  = std::function<void(bool isReply, const char* text, bool final)>;
    // 会话结束通知（UI 收尾）：reason 原样透传（exit_intent/device_end/timeout/...）
    using EndCallback   = std::function<void(const char* reason)>;
    // 错误通知（talk 建立失败/中途出错）：code 如 REALTIME_FAIL/BUSY
    using ErrorCallback = std::function<void(const char* code, const char* msg)>;

    void begin(Link* link, AppState* app, AudioPipe* audio);  // 注入依赖、建下行帧队列

    // 用户触发开始；已在录音/talk 中则拒绝并返回 false
    // provider：对话引擎选择（ADR-035，选择权在小设备；空串=服务端默认）
    // 状态机：Off → Connecting（发 talk_request）→ 收 talk_ready 后 Active → 结束回 Off
    bool start(const char* provider = "");
    // 本地结束；sendSignal=true 时上行 talk_end（本地发起）
    void stop(bool sendSignal, const char* reason = "device_end");

    // BLE 下行入口（app 事件泵调用，仅在 talk 相关信令/帧时）
    // 上行不走这里：采集泵（main.cpp）把 Opus 帧直接 sendFrame，
    // 经 App 哑管道 → MQTT → 服务端喂火山实时语音
    void onSignalJson(const char* json);                    // talk_ready/end、asr/reply、error
    void onAudioFrame(const uint8_t* payload, uint16_t len); // 下行 ogg_opus 分片入抖动队列

    bool isOff() const;        // TalkState::Off
    bool isActive() const;     // TalkState::Active（音频泵在跑）
    bool downlinkBusy() const;  // 下行正在出声（半双工降噪判定，ADR-037）

    void onText(TextCallback cb)   { textCb_ = std::move(cb); }
    void onEnd(EndCallback cb)     { endCb_ = std::move(cb); }
    void onError(ErrorCallback cb) { errCb_ = std::move(cb); }

private:
    // 会话生命周期
    void activate();                       // talk_ready 到达：开 24k 双工通路 + 播放泵
    void teardown(const char* reason);     // 停泵、关通路、状态回 Off

    static void playbackTask(void* arg);   // 下行播放泵

    // 下行帧队列（jitter buffer）：元素 = malloc 的 {len, bytes}
    struct AudioChunk {
        uint16_t len;
        uint8_t  data[];  // 柔性数组
    };
    static constexpr UBaseType_t JITTER_QUEUE_DEPTH = 64;   // 帧粒度，远大于抖动窗口

    Link*       gatt_  = nullptr;  // 仍叫 gatt_：历史命名，实为链路指针
    AppState*   app_   = nullptr;
    AudioPipe*  audio_ = nullptr;

    QueueHandle_t jitterQ_  = nullptr;   // AudioChunk* 指针队列
    volatile uint32_t lastDownlinkMs_ = 0;  // 最近一次下行块处理时刻
    uint32_t downBytes_ = 0;                // 下行到达速率仪表（每秒归零）
    uint32_t downChunks_ = 0;
    uint32_t downLastLog_ = 0;
    TaskHandle_t  playTask_ = nullptr;
    volatile bool playStop_ = false;

    OggDemux demux_;
    OpusDec  dec_;

    // 播放统计（串口验证用）
    uint32_t playPackets_   = 0;
    uint32_t playBytesIn_   = 0;
    uint32_t playPcmOut_    = 0;
    uint32_t playUnderrun_  = 0;
    uint32_t playDropped_   = 0;

    TextCallback  textCb_;
    EndCallback   endCb_;
    ErrorCallback errCb_;
};

}  // namespace gaga
