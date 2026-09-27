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

// 流式重组器：逐字节推进的状态机，凑齐一帧后触发 onFrame 回调。
// 一次 feed() 可含任意多帧的拼接、也允许一帧横跨多次 feed（App 合批与
// MTU 切块都不对齐帧边界——2026-09-27 实锤：按包解析时合批写入里首帧后
// 的字节全部静默丢失，receipt/reply "永远收不到"的根因，ADR-060）。
// 分片帧（0x80 首包 + 0x00 中间包 + 尾包）的包结构在流内依然成立。
class FrameDecoder {
public:
    using FrameCallback = std::function<void(uint8_t type, const uint8_t* payload, uint16_t len)>;
    using ErrorCallback = std::function<void(const char* reason)>;

    void onFrame(FrameCallback cb)  { frameCb_ = std::move(cb); }
    void onError(ErrorCallback cb)  { errorCb_ = std::move(cb); }

    void feed(const uint8_t* data, size_t len);  // 喂任意一段字节流，推进状态机
    void reset();                                // 清空重组状态（断连/残帧自保时调用）
    bool partial() const { return mode_ != Mode::Scan; }  // 正在半个帧里（卡帧看门狗用）

private:
    void finishFrame();                          // 整帧齐了：回调上层并回扫描态

    enum class Mode : uint8_t {
        Scan,           // 凑 3 字节包头
        SinglePayload,  // 单包帧 payload（可跨 feed 边界）
        SubHeader,      // 分片帧：凑 3 字节子包头
        SubPayload,     // 分片帧：收本子包 payload
    };

    FrameCallback frameCb_;
    ErrorCallback errorCb_;

    Mode     mode_      = Mode::Scan;
    uint8_t  hdr_[3]    = {0, 0, 0};  // 逐字节凑包头/子包头
    uint8_t  hdrGot_    = 0;
    uint8_t  frameType_ = 0;          // 首包记下的真实 type（去掉 0x80 位）
    uint32_t remain_    = 0;          // 当前帧还差的 payload 字节数
    uint32_t subRemain_ = 0;          // 当前子包还差的 payload 字节数
    std::vector<uint8_t> buf_;        // 已收的 payload
};

}  // namespace gaga
