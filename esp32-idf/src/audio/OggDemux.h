#pragma once

#include <cstddef>
#include <cstdint>
#include <queue>
#include <vector>

// 最小 Ogg 解封装（M5 talk 下行）：服务端下行 type=0x01 帧是 ogg_opus 24kHz
// 分片（protocol.md §3，任意字节边界切碎；按到达顺序拼接即完整 Ogg 流）。
// 本类是**流式**解析器：feed() 喂任意字节，nextPacket()/popPacket() 逐个吐裸 Opus 包。
// 处理：Ogg 页头/段表（lacing=255 续包跨页）、BOS 新流重置、续页残骸跳过、
// 跳过 OpusHead/OpusTags 头包。CRC 不校验（BLE 链路已有完整性，算力留给编解码）。
namespace gaga {

class OggDemux {
public:
    void feed(const uint8_t* data, size_t len);  // 喂任意字节流，内部尽量拆出整包
    void reset();                                // 清空一切状态（新流/重连时调用）

    // 队头 Opus 包（未消费完前指针有效）；空队列返回 false
    bool nextPacket(const uint8_t** pkt, size_t* len) const;
    void popPacket();   // 消费队头（与 nextPacket 配对使用）
    size_t pending() const { return queue_.size(); }

    // 统计（调试用）
    uint32_t pages() const { return pages_; }
    uint32_t packets() const { return packets_; }
    uint32_t resyncs() const { return resyncs_; }

private:
    bool parseOnePage();  // 尽量拆一个 Ogg 页；字节不够返回 false 等下次 feed

    std::vector<uint8_t>              stream_;  // 未消费的流字节
    std::vector<uint8_t>              curPkt_;  // 组包中的 Opus 包（处理 255 续包）
    std::queue<std::vector<uint8_t>>  queue_;   // 组好的 Opus 包
    bool     skippingWreck_ = false;            // 续页残骸丢弃中
    uint32_t pages_    = 0;
    uint32_t packets_  = 0;
    uint32_t resyncs_  = 0;
};

}  // namespace gaga
