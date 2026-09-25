#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

// 帧协议实现，格式见 docs/protocol.md §2：
//   [1B type][2B payload_len (big-endian)][payload]
// BLE 单包放不下时的分包规则（protocol.md §2 末段）：
//   首包   type | 0x80，len 字段 = 整帧 payload 总长度（供接收端预分配）
//   中间包 type = 0x00，len 字段 = 本包 payload 字节数
//   尾包   type = 原始 type，len 字段 = 本包 payload 字节数
//   单包帧 type 不置 0x80，len = payload 长度
// 与 Arduino 线 esp32/src/protocol/frame.cpp 字节级一致（同一份协议的两端实现）。
namespace gaga {

// 帧类型（docs/protocol.md）
constexpr uint8_t FRAME_TYPE_CONTINUATION = 0x00;  // 续帧（中间包专用）
constexpr uint8_t FRAME_TYPE_OPUS         = 0x01;  // Opus 音频帧（上行裸包；下行 ogg_opus 分片）
constexpr uint8_t FRAME_TYPE_JSON         = 0x02;  // JSON 信令

constexpr uint8_t FRAME_FLAG_MORE    = 0x80;  // 首包标记：后面还有分片
constexpr size_t  FRAME_HEADER_SIZE  = 3;
constexpr size_t  FRAME_MAX_PAYLOAD  = 0xFFFF;

// 把一帧编码为若干 ≤ maxPacketSize 字节的 BLE 包。
// maxPacketSize 必须 > FRAME_HEADER_SIZE，否则返回空 vector。
std::vector<std::vector<uint8_t>> encodeFrame(uint8_t type,
                                              const uint8_t* payload,
                                              uint16_t len,
                                              size_t maxPacketSize);

// 流式重组器：逐个喂入 BLE 包，凑齐一帧后触发 onFrame 回调。
// 不持有连接状态，断连后请调用 reset() 丢弃半个残帧。
class FrameDecoder {
public:
    using FrameCallback = std::function<void(uint8_t type, const uint8_t* payload, uint16_t len)>;
    using ErrorCallback = std::function<void(const char* reason)>;

    void onFrame(FrameCallback cb)  { frameCb_ = std::move(cb); }
    void onError(ErrorCallback cb)  { errorCb_ = std::move(cb); }

    void feed(const uint8_t* packet, size_t len);  // 喂一个 BLE 包，推进重组状态机
    void reset();                                  // 清空重组状态（断连/残帧自保时调用）

private:
    void appendChunk(const uint8_t* data, size_t len);  // 追加一段 payload，凑满则收帧
    void finishFrame();                                 // 整帧齐了：回调上层并复位

    FrameCallback frameCb_;
    ErrorCallback errorCb_;

    // 重组状态机三元组 + 累积缓冲
    bool     reassembling_ = false;   // 正在收分片（见了 0x80 首包后为 true）
    uint8_t  frameType_    = 0;       // 首包记下的真实 type（去掉 0x80 位）
    uint16_t expectedLen_  = 0;       // 首包宣告的整帧 payload 总长
    std::vector<uint8_t> buf_;        // 已收的 payload 片段
};

}  // namespace gaga
