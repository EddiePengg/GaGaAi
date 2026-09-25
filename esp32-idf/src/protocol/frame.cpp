#include "frame.h"

// 与 Arduino 线 frame.cpp 逻辑逐行一致（仅去 Arduino.h，min 换内联实现）
namespace gaga {

static inline size_t gmin(size_t a, size_t b) { return a < b ? a : b; }

// 把一帧编码成若干 ≤maxPacketSize 的 BLE 包；放不下就按 0x80 分包规则切片
std::vector<std::vector<uint8_t>> encodeFrame(uint8_t type,
                                              const uint8_t* payload,
                                              uint16_t len,
                                              size_t maxPacketSize) {
    std::vector<std::vector<uint8_t>> packets;
    if (maxPacketSize <= FRAME_HEADER_SIZE) return packets;
    if (len > 0 && payload == nullptr) return packets;

    const size_t chunkCap = maxPacketSize - FRAME_HEADER_SIZE;

    // 单包放得下的情形（含空 payload）：type 不置 0x80，len = payload 长度
    if (len <= chunkCap) {
        std::vector<uint8_t> pkt(FRAME_HEADER_SIZE + len);
        pkt[0] = type;
        pkt[1] = static_cast<uint8_t>(len >> 8);
        pkt[2] = static_cast<uint8_t>(len & 0xFF);
        if (len > 0) memcpy(pkt.data() + FRAME_HEADER_SIZE, payload, len);
        packets.push_back(std::move(pkt));
        return packets;
    }

    // 分片
    size_t offset = 0;
    bool   first  = true;
    while (offset < len) {
        const size_t n = gmin(chunkCap, static_cast<size_t>(len) - offset);
        const bool   last = (offset + n >= len);

        uint8_t  typeField;
        uint16_t lenField;
        if (first) {
            typeField = type | FRAME_FLAG_MORE;
            lenField  = len;              // 首包宣告整帧总长度
        } else if (!last) {
            typeField = FRAME_TYPE_CONTINUATION;
            lenField  = static_cast<uint16_t>(n);
        } else {
            typeField = type;             // 尾包恢复正常 type
            lenField  = static_cast<uint16_t>(n);
        }

        std::vector<uint8_t> pkt(FRAME_HEADER_SIZE + n);
        pkt[0] = typeField;
        pkt[1] = static_cast<uint8_t>(lenField >> 8);
        pkt[2] = static_cast<uint8_t>(lenField & 0xFF);
        memcpy(pkt.data() + FRAME_HEADER_SIZE, payload + offset, n);
        packets.push_back(std::move(pkt));

        offset += n;
        first   = false;
    }
    return packets;
}

// 喂一个 BLE 包，按包头 type/len 推进重组状态机
void FrameDecoder::feed(const uint8_t* packet, size_t len) {
    if (packet == nullptr || len < FRAME_HEADER_SIZE) {
        if (errorCb_) errorCb_("packet too short");
        return;
    }

    const uint8_t  typeField = packet[0];
    const uint16_t lenField  = (static_cast<uint16_t>(packet[1]) << 8) | packet[2];
    const uint8_t* body      = packet + FRAME_HEADER_SIZE;
    const size_t   bodyLen   = len - FRAME_HEADER_SIZE;

    if (typeField & FRAME_FLAG_MORE) {
        // 首包：len 字段是整帧 payload 总长度
        reassembling_ = true;
        frameType_    = typeField & 0x7F;
        expectedLen_  = lenField;
        buf_.clear();
        buf_.reserve(expectedLen_);
        appendChunk(body, bodyLen);
        return;
    }

    if (typeField == FRAME_TYPE_CONTINUATION) {
        if (!reassembling_) {  // 没有首包却来了中间包：半截残帧，丢弃
            if (errorCb_) errorCb_("orphan continuation");
            return;
        }
        appendChunk(body, gmin(bodyLen, static_cast<size_t>(lenField)));
        return;
    }

    if (reassembling_) {
        // 尾包
        appendChunk(body, gmin(bodyLen, static_cast<size_t>(lenField)));
    } else {
        // 单包完整帧
        if (lenField > bodyLen) {
            if (errorCb_) errorCb_("payload truncated");
            return;
        }
        if (frameCb_) frameCb_(typeField, body, lenField);
    }
}

// 追加一段 payload；累计到 expectedLen_ 就收帧
void FrameDecoder::appendChunk(const uint8_t* data, size_t len) {
    if (buf_.size() + len > expectedLen_) {
        // 超过首包宣告的总长度：对端不按协议来，丢弃残帧自保
        if (errorCb_) errorCb_("frame overflow");
        reset();
        return;
    }
    buf_.insert(buf_.end(), data, data + len);
    if (buf_.size() >= expectedLen_) finishFrame();
}

// 整帧凑齐：回调上层，然后复位状态机
void FrameDecoder::finishFrame() {
    if (frameCb_) frameCb_(frameType_, buf_.data(), static_cast<uint16_t>(buf_.size()));
    reset();
}

// 清空重组状态机（断连或残帧自保时调用）
void FrameDecoder::reset() {
    reassembling_ = false;
    frameType_    = 0;
    expectedLen_  = 0;
    buf_.clear();
}

}  // namespace gaga
