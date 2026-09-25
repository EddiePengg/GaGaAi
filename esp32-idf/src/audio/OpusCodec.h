#pragma once

#include <cstddef>
#include <cstdint>

// Opus 编解码封装（esp_audio_codec 组件的直接 API：esp_opus_enc_* / esp_opus_dec_*，
// 底层是乐鑫优化版 libopus）。
//   编码：上行链路（M1 录音 + M5 talk 上行）——16kHz 单声道 20ms/帧 16kbps VOIP，
//         参数与 Arduino 线 OpusEncoder 对齐（docs/data-model.md §2）
//   解码：M5 talk 下行——ogg_opus 24kHz 分片解 Ogg 后的裸 Opus 包
namespace gaga {

class OpusEnc {
public:
    static constexpr int SAMPLE_RATE_HZ = 16000;
    static constexpr int FRAME_SAMPLES  = 320;   // 20ms @ 16kHz
    static constexpr int BITRATE_BPS    = 16000;

    bool begin();   // 打开编码器（16k 单声道 / 20ms / 16kbps VOIP，幂等）
    void end();     // 关编码器
    // 输入必须正好 FRAME_SAMPLES 个采样；返回编码后字节数（<0 = 失败）
    int encode(const int16_t* pcm, uint8_t* out, int outCap);
    bool ready() const { return enc_ != nullptr; }

private:
    void* enc_ = nullptr;
};

class OpusDec {
public:
    static constexpr int SAMPLE_RATE_HZ = 24000;      // 下行 ogg_opus 24kHz（protocol.md §3）
    static constexpr int MAX_FRAME_SAMPLES = 5760;    // 120ms 上限足够覆盖任何 Volcengine 帧

    bool begin();   // 打开解码器（24k 单声道，帧长由包自适应，幂等）
    void end();     // 关解码器
    // 解一个裸 Opus 包；返回 PCM 采样数（单声道 16bit），<0 = 失败
    int decode(const uint8_t* pkt, int len, int16_t* pcmOut, int maxSamples);
    bool ready() const { return dec_ != nullptr; }

private:
    void* dec_ = nullptr;
};

}  // namespace gaga
