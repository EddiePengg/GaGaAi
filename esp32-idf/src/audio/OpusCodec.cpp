#include "OpusCodec.h"

#include "esp_log.h"
#include "esp_opus_enc.h"
#include "esp_opus_dec.h"

namespace gaga {

static const char* TAG = "gaga.opus";

// ---------------------------------------------------------------- 编码器 ----
// 上行链路：MIC PCM → 本编码器 → type=0x01 帧 → BLE → App → MQTT → 服务端喂火山
bool OpusEnc::begin() {
    if (enc_ != nullptr) return true;
    esp_opus_enc_config_t cfg = {
        .sample_rate      = SAMPLE_RATE_HZ,
        .channel          = 1,
        .bits_per_sample  = 16,
        .bitrate          = BITRATE_BPS,
        .frame_duration   = ESP_OPUS_ENC_FRAME_DURATION_20_MS,
        .application_mode = ESP_OPUS_ENC_APPLICATION_VOIP,
        .complexity       = 8,      // 与 Arduino 线一致（默认 10 略降，S3 单核富余）
        .enable_fec       = false,
        .enable_dtx       = false,
        .enable_vbr       = true,
    };
    if (esp_opus_enc_open(&cfg, sizeof(cfg), &enc_) != ESP_AUDIO_ERR_OK || enc_ == nullptr) {
        ESP_LOGE(TAG, "opus enc open 失败");
        enc_ = nullptr;
        return false;
    }
    int inSize = 0, outSize = 0;
    esp_opus_enc_get_frame_size(enc_, &inSize, &outSize);
    ESP_LOGI(TAG, "编码器就绪（%dHz mono %dbps VBR，帧输入 %dB=%d 采样）",
             SAMPLE_RATE_HZ, BITRATE_BPS, inSize, inSize / 2);
    return true;
}

// 关编码器并清句柄
void OpusEnc::end() {
    if (enc_) {
        esp_opus_enc_close(enc_);
        enc_ = nullptr;
    }
}

// 编码一帧（输入必须正好 FRAME_SAMPLES 个采样）；返回输出字节数（<0 失败）
int OpusEnc::encode(const int16_t* pcm, uint8_t* out, int outCap) {
    if (enc_ == nullptr) return -1;
    esp_audio_enc_in_frame_t  inFrame  = {};
    esp_audio_enc_out_frame_t outFrame = {};
    inFrame.buffer  = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(pcm));
    inFrame.len     = FRAME_SAMPLES * sizeof(int16_t);
    outFrame.buffer = out;
    outFrame.len    = outCap;
    const esp_audio_err_t err = esp_opus_enc_process(enc_, &inFrame, &outFrame);
    if (err != ESP_AUDIO_ERR_OK) {
        ESP_LOGW(TAG, "opus enc 失败 err=%d", err);
        return -1;
    }
    return static_cast<int>(outFrame.encoded_bytes);  // 实际编码长度在 encoded_bytes（len 是容量）
}

// ---------------------------------------------------------------- 解码器 ----
// 下行链路：talk 服务端 → BLE type=0x01 ogg_opus 分片 → OggDemux 拆出裸包 → 本解码器 → 扬声器
bool OpusDec::begin() {
    if (dec_ != nullptr) return true;
    esp_opus_dec_cfg_t cfg = {
        .sample_rate    = SAMPLE_RATE_HZ,
        .channel        = 1,
        .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_INVALID,  // 由包自适应（60ms 上限缓冲）
        .self_delimited = false,  // Ogg 包是裸 Opus 包，非自分隔格式
    };
    if (esp_opus_dec_open(&cfg, sizeof(cfg), &dec_) != ESP_AUDIO_ERR_OK || dec_ == nullptr) {
        ESP_LOGE(TAG, "opus dec open 失败");
        dec_ = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "解码器就绪（%dHz mono）", SAMPLE_RATE_HZ);
    return true;
}

// 关解码器并清句柄
void OpusDec::end() {
    if (dec_) {
        esp_opus_dec_close(dec_);
        dec_ = nullptr;
    }
}

// 解一个裸 Opus 包为单声道 PCM；返回采样数（<0 失败）
int OpusDec::decode(const uint8_t* pkt, int len, int16_t* pcmOut, int maxSamples) {
    if (dec_ == nullptr) return -1;
    esp_audio_dec_in_raw_t     raw   = {};
    esp_audio_dec_out_frame_t  frame = {};
    esp_audio_dec_info_t       info  = {};
    raw.buffer   = const_cast<uint8_t*>(pkt);
    raw.len      = len;
    frame.buffer = reinterpret_cast<uint8_t*>(pcmOut);
    frame.len    = maxSamples * sizeof(int16_t);
    const esp_audio_err_t err = esp_opus_dec_decode(dec_, &raw, &frame, &info);
    if (err != ESP_AUDIO_ERR_OK) {
        ESP_LOGW(TAG, "opus dec 失败 err=%d len=%d", err, len);
        return -1;
    }
    return static_cast<int>(frame.decoded_size / sizeof(int16_t));
}

}  // namespace gaga
