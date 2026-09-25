#pragma once

#include <cstddef>
#include <cstdint>

// 消息卡数据层（M6 反馈闭环 UI）：一条消息 = 一问一答
//   松手发送 → 占位卡（识别中）→ receipt 填 ASR 文本（等待回复）
//   → reply 填回复（已回复）/ 60s 无回复软超时（迟到 reply 仍可点亮）
// 存储：PSRAM 数组 20 条（~47KB，20×(256+2048+24)——reply 全文级容量，
// 内部 RAM 付不起只能落 PSRAM；懒分配：首条消息进来才 calloc），重启即清——
// 用户拍板"不超过 20 条"；持久化（NVS/LittleFS）等真用出需求再挂。
// 容量依据（2026-09-25 用户报障"回复显示不全"）：Hermes 日报类回复数百字，
// 旧 reply[384]（~128 汉字）在数据层就砍掉了，详情页滚动的意义都没了。
// FIFO 匹配语义：receipt 取最早"发送中"，reply 取最早"等待回复"（单人佩戴场景
// 先进先出够用；多设备/并发留 msg_id 关联到飞书官方 API 阶段）。
// id 稳定递增：UI 详情页靠 id 认卡，头部插卡/顶卡时下标漂移不串卡。
namespace gaga {

enum class MsgState : uint8_t {
    Sending,   // 已松手，ASR 未回（ask 占位"识别中"）
    Waiting,   // ask 已填，等 Hermes 回复
    Replied,   // 问答齐活
    Failed,    // 发送/识别失败（error 信令）
};

struct Msg {
    uint32_t id;            // 稳定标识（单调递增，0=无效）
    MsgState state;
    uint32_t createdAtMs;   // 松手时刻（软超时基准）
    uint32_t repliedAtMs;   // 回复到达时刻
    char     ask[1024];     // ASR 文本 UTF-8 全文（~340 汉字，长口述不截断）
    char     reply[2048];   // GAGA 回复 UTF-8 全文（~680 汉字，日报级回复够用）
    char     msgId[40];     // 平台消息 id（receipt 携带）——回复按 reply_to 精确配对
};

class MsgLog {
public:
    static constexpr int MAX_MSGS = 20;
    // 等待回复软超时：到点显示"（还没回复）"，state 不变（迟到 reply 照样点亮）
    static constexpr uint32_t REPLY_SOFT_TIMEOUT_MS = 60000;

    void clear();  // 清空全部卡

    // 松手发送：头部插入占位卡（Sending），最旧一条被顶掉。返回新卡下标（0）
    int addSending();
    // receipt：填 ASR 文本 + 平台 msg_id，Sending → Waiting。返回卡下标（-1 = 无匹配）
    int fillAsk(const char* text, const char* msgId = "");
    // reply：优先按 reply_to（= receipt 存过的 msg_id）精确配对，无匹配退回
    // FIFO 最早 Waiting 卡。Waiting → Replied。返回卡下标（-1 = 无匹配）
    int fillReply(const char* text, const char* replyTo = "");
    // error：Sending → Failed，记原因（截断进 reply 槽显示）
    int failSending(const char* reason);

    int count() const { return count_; }
    // 下标 0 = 最新（分配失败/越界返回 nullptr；实现在 .cpp）
    const Msg* at(int i) const;
    const Msg* findById(uint32_t id) const;  // 按稳定 id 认卡（0=无效）
    // 等待中且已超软超时（UI 显示"（还没回复）"）
    static bool softTimedOut(const Msg& m, uint32_t nowMs);

    // 数据版本号：每次变更 +1，UI 对比后决定重刷（避免无谓重绘）
    uint32_t version() const { return version_; }

private:
    // PSRAM 懒分配的卡数组（ensureStorage 在各写入口调用；读路径判 null 安全）
    void ensureStorage();
    Msg*     msgs_ = nullptr;
    int      count_ = 0;
    uint32_t nextId_ = 1;
    uint32_t version_ = 0;
};

// UTF-8 安全截断：写入 dst（含 \0，容量 cap），在字符边界收尾
void utf8CopyTrunc(char* dst, size_t cap, const char* src);

}  // namespace gaga
