#include "frame.h"

#include "esp_log.h"

// 从固件首版平移：去框架头依赖，min 换内联实现（逻辑逐行一致）。
namespace gaga {

static const char* kTag = "gaga.frame";

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

// 喂一段字节流：写入边界对协议无意义（App 合批/MTU 切块不对齐帧边界）。
// 状态机逐字节推进：Scan 凑包头 → 单包帧直接收 payload；分片帧先收 0x80
// 首包，再按子包头（0x00 中间/原 type 尾包）逐段收，凑满 expectedLen 收帧。
// 一次 feed 里解出任意多帧；一帧也可以横跨任意多次 feed。
void FrameDecoder::feed(const uint8_t* data, size_t len) {
    if (data == nullptr) return;
    size_t i = 0;
    while (i < len) {
        switch (mode_) {
        case Mode::Scan: {  // 凑 3 字节包头
            hdr_[hdrGot_++] = data[i++];
            if (hdrGot_ < FRAME_HEADER_SIZE) break;
            hdrGot_ = 0;
            acceptHeader();
            break;
        }
        case Mode::Hunt: {  // 流错位自救：3 字节滑窗，找到合法帧头才回 Scan
            hdr_[0] = hdr_[1];
            hdr_[1] = hdr_[2];
            hdr_[2] = data[i++];
            const uint8_t  t = hdr_[0];
            const uint16_t n = (static_cast<uint16_t>(hdr_[1]) << 8) | hdr_[2];
            const uint8_t  base = t & 0x7F;
            if ((base == FRAME_TYPE_OPUS || base == FRAME_TYPE_JSON) &&
                n <= kMaxPayload) {
                acceptHeader();  // 找到一个像真帧头的东西，赌它是（赌错会再 Hunt）
            }
            // 否则滑窗继续：0x00 续帧/未知 type/超长 len 一律跳过
            break;
        }
        case Mode::SinglePayload: {  // 收单包帧 payload（可跨 feed 边界）
            const size_t n = gmin(len - i, remain_);
            buf_.insert(buf_.end(), data + i, data + i + n);
            i += n;
            remain_ -= n;
            if (remain_ == 0) {
                finishFrame();
                mode_ = Mode::Scan;
            }
            break;
        }
        case Mode::SubHeader: {  // 分片帧：凑 3 字节子包头
            hdr_[hdrGot_++] = data[i++];
            if (hdrGot_ < FRAME_HEADER_SIZE) break;
            hdrGot_ = 0;
            const uint8_t  t = hdr_[0];
            const uint16_t n = (static_cast<uint16_t>(hdr_[1]) << 8) | hdr_[2];
            if (t == FRAME_TYPE_CONTINUATION || t == frameType_) {
                if (n > remain_) {  // 子包宣告超首包总长：流已脏，自保重扫
                    enterHunt("frame overflow");
                    break;
                }
                subRemain_ = n;
                mode_ = Mode::SubPayload;
            } else {
                enterHunt("bad sub header");
                break;
            }
            break;
        }
        case Mode::SubPayload: {  // 收本子包 payload
            const size_t n = gmin(len - i, subRemain_);
            buf_.insert(buf_.end(), data + i, data + i + n);
            i += n;
            subRemain_ -= n;
            remain_    -= n;
            if (subRemain_ == 0) mode_ = Mode::SubHeader;
            if (remain_ == 0) {
                finishFrame();
                mode_ = Mode::Scan;
            }
            break;
        }
        }
    }
}

// hdr_ 凑满 3 字节后的统一校验与分发（Scan 与 Hunt 共用）。
// 不合法/超长一律不进 payload 态，直接 Hunt 滑窗自救——旧行为（未知 type
// 当单包帧收、超长假帧吞流等残帧超时）在链路丢包时会把后续所有真帧吃光
void FrameDecoder::acceptHeader() {
    const uint8_t  t = hdr_[0];
    const uint16_t n = (static_cast<uint16_t>(hdr_[1]) << 8) | hdr_[2];
    ESP_LOGD(kTag, "hdr t=%02x n=%u", t, static_cast<unsigned>(n));  // 下行对账（排障开 'G'）
    const uint8_t  base = t & 0x7F;
    if (base != FRAME_TYPE_OPUS && base != FRAME_TYPE_JSON) {
        enterHunt("bad type");   // 含孤儿续帧（0x00）与流内脏字节
        return;
    }
    if (n > kMaxPayload) {
        enterHunt("oversize");
        return;
    }
    if (t & FRAME_FLAG_MORE) {  // 分片首包：n = 整帧 payload 总长
        frameType_ = base;
        remain_    = n;
        buf_.clear();
        buf_.reserve(n);
        // 首包自带 payload（发送端切块 512B/包 → 首包 payload 恒 509B；
        // 分片 ⇒ 总长 >509 ⇒ 首包必满）。必须先收首包 payload 再读子包头——
        // 旧代码直接进 SubHeader，把首包 509B 数据当子包头解析，ogg 的
        // "OggS" 被误读成 02 00 00 假帧，随后整流误判吞掉（2026-09-28 真机
        // 实锤；分片下行此前从未被真实业务使用过——reply 多为 <509B 单包帧）
        subRemain_ = (n < kFirstPayload) ? n : kFirstPayload;
        mode_ = Mode::SubPayload;
    } else {  // 单包完整帧：n = payload 长度
        frameType_ = t;
        remain_    = n;
        buf_.clear();
        buf_.reserve(n);
        mode_ = (n == 0) ? Mode::Scan : Mode::SinglePayload;
        if (n == 0) finishFrame();
    }
}

// 进入滑窗自救态：清空半帧状态，从当前 hdr_ 位置继续滑（Hunt 每字节滑窗）
void FrameDecoder::enterHunt(const char* reason) {
    if (errorCb_) errorCb_(reason);
    frameType_ = 0;
    remain_    = 0;
    subRemain_ = 0;
    buf_.clear();
    buf_.shrink_to_fit();
    hdrGot_ = 0;
    mode_   = Mode::Hunt;
}

// 整帧凑齐：回调上层（状态回 Scan 由调用点负责/这里不动 mode_）
void FrameDecoder::finishFrame() {
    ESP_LOGI(kTag, "整帧就绪 type=%02x len=%u", frameType_,
             static_cast<unsigned>(buf_.size()));  // 下行链路对账锚点（2026-09-28）
    if (frameCb_) frameCb_(frameType_, buf_.data(),
                           static_cast<uint16_t>(buf_.size()));
    buf_.clear();
    buf_.shrink_to_fit();
    frameType_ = 0;
    remain_    = 0;
    subRemain_ = 0;
}

// 清空重组状态机（断连或残帧自保时调用）
void FrameDecoder::reset() {
    mode_      = Mode::Scan;
    hdrGot_    = 0;
    frameType_ = 0;
    remain_    = 0;
    subRemain_ = 0;
    buf_.clear();
    buf_.shrink_to_fit();
}

}  // namespace gaga
