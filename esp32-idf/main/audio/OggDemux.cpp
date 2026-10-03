#include "OggDemux.h"

#include <cstring>

namespace gaga {

// Ogg 页布局：'OggS'(4) ver(1) type(1) granule(8) serial(4) seq(4) crc(4) nsegs(1) segtab(n) payload
static constexpr size_t  OGGS_HDR            = 27;
static constexpr uint8_t OGGS_TYPE_CONTINUED = 0x01;
static constexpr uint8_t OGGS_TYPE_BOS       = 0x02;

// 全部状态归零：流缓冲、组包中、已组队、统计
void OggDemux::reset() {
    stream_.clear();
    curPkt_.clear();
    while (!queue_.empty()) queue_.pop();
    skippingWreck_ = false;
    pages_ = packets_ = resyncs_ = 0;
}

// 字节流进缓冲，能拆几页拆几页（页可能横跨多次 feed）
void OggDemux::feed(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) return;
    stream_.insert(stream_.end(), data, data + len);
    while (parseOnePage()) {}
}

// 窥视队头包（不消费；popPacket 才真正取走）
bool OggDemux::nextPacket(const uint8_t** pkt, size_t* len) const {
    if (queue_.empty()) return false;
    *pkt = queue_.front().data();
    *len = queue_.front().size();
    return true;
}

// 弹出队头
void OggDemux::popPacket() {
    if (!queue_.empty()) queue_.pop();
}

// 从 stream_ 消费一个完整 Ogg 页并拆出裸 Opus 包；成功返回 true（可继续拆下一页）
bool OggDemux::parseOnePage() {
    // 找 'OggS' 捕获字（容错重同步：垃圾字节丢弃）
    size_t magic = 0;
    while (magic + 4 <= stream_.size() &&
           memcmp(stream_.data() + magic, "OggS", 4) != 0) {
        magic++;
    }
    if (magic > 0) {
        stream_.erase(stream_.begin(), stream_.begin() + magic);
        resyncs_++;
    }
    if (stream_.size() < OGGS_HDR) return false;

    const uint8_t* h = stream_.data();
    if (h[4] != 0) {  // version 必须为 0，否则按失步处理
        stream_.erase(stream_.begin());
        resyncs_++;
        return true;
    }
    const uint8_t type  = h[5];
    const uint8_t nsegs = h[26];
    const size_t  hdrLen = OGGS_HDR + nsegs;
    if (stream_.size() < hdrLen) return false;  // 等更多字节

    size_t payloadLen = 0;
    for (int i = 0; i < nsegs; i++) payloadLen += h[OGGS_HDR + i];
    if (stream_.size() < hdrLen + payloadLen) return false;  // 等更多字节

    const uint8_t* segs    = h + OGGS_HDR;
    const uint8_t* payload = h + hdrLen;
    pages_++;

    // BOS：新流开始（每次回复音频一条独立 Ogg 流），清掉上个流的组包状态
    if (type & OGGS_TYPE_BOS) {
        curPkt_.clear();
        skippingWreck_ = false;
    }

    // 续页但本地没有组到一半的包：续包头部已丢失（流被截断/中途接入），
    // 本页属于残骸——丢弃段直到遇到 <255 的段（残骸结束）为止
    if ((type & OGGS_TYPE_CONTINUED) && curPkt_.empty()) {
        skippingWreck_ = true;
    }

    size_t off = 0;
    for (int i = 0; i < nsegs; i++) {
        const uint8_t segLen = segs[i];
        if (skippingWreck_) {
            off += segLen;
            if (segLen < 255) skippingWreck_ = false;  // 残骸结束
            continue;
        }
        curPkt_.insert(curPkt_.end(), payload + off, payload + off + segLen);
        off += segLen;
        if (segLen < 255) {  // 段值 <255 = 本包结束
            if (!curPkt_.empty()) {
                // 跳过 Opus 头包（OpusHead / OpusTags）
                const bool isHead = curPkt_.size() >= 8 &&
                    (memcmp(curPkt_.data(), "OpusHead", 8) == 0 ||
                     memcmp(curPkt_.data(), "OpusTags", 8) == 0);
                if (!isHead) {
                    queue_.push(std::move(curPkt_));
                    packets_++;
                }
                curPkt_.clear();
            }
        }
        // segLen == 255：包延续到下一页（curPkt_ 保留待续）
    }

    stream_.erase(stream_.begin(), stream_.begin() + hdrLen + payloadLen);
    return true;
}

}  // namespace gaga
