#include "MsgLog.h"

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "compat.h"
#include "ui/FontFilter.h"

#include <cstring>

namespace gaga {

static const char* TAG = "gaga.msglog";

// PSRAM 懒分配：20 条全文卡 ~47KB（内部 RAM 付不起）。失败=无卡模式（极端
// 内存不足，语音链路不受影响，只是不上屏）。
void MsgLog::ensureStorage() {
    if (msgs_ != nullptr) return;
    msgs_ = static_cast<Msg*>(heap_caps_calloc(MAX_MSGS, sizeof(Msg),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (msgs_ == nullptr) {
        ESP_LOGE(TAG, "消息卡 PSRAM 分配失败（%dB×%d）",
                 static_cast<int>(sizeof(Msg)), MAX_MSGS);
    }
}

// UTF-8 安全拷贝：超长就在字符边界收尾（宁少一个字，不切出半个汉字）
void utf8CopyTrunc(char* dst, size_t cap, const char* src) {
    if (cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n < cap) {
        memcpy(dst, src, n + 1);
        return;
    }
    // 超长：退到 cap-1 内最后一个 UTF-8 字符边界（首字节 10xxxxxx 是续字节）
    size_t end = cap - 1;
    while (end > 0 && (static_cast<unsigned char>(src[end]) & 0xC0) == 0x80) end--;
    memcpy(dst, src, end);
    dst[end] = '\0';
}

// 清空全部卡并 bump 版本号，UI 据此重刷
void MsgLog::clear() {
    count_ = 0;
    version_++;
}

// at()/findById() 的 null 防护（分配失败时不给野指针）
const Msg* MsgLog::at(int i) const {
    if (msgs_ == nullptr || i < 0 || i >= count_) return nullptr;
    return &msgs_[i];
}

// 松手建占位卡：数组整体后移（最旧一条被 20 条上限挤掉），空出 [0] 放新卡
int MsgLog::addSending() {
    ensureStorage();
    if (msgs_ == nullptr) return -1;
    if (count_ < MAX_MSGS) count_++;
    memmove(&msgs_[1], &msgs_[0], sizeof(Msg) * (count_ - 1));  // 最旧被顶掉
    Msg& m = msgs_[0];
    memset(&m, 0, sizeof(m));
    m.id          = nextId_++;
    m.state       = MsgState::Sending;
    m.createdAtMs = millis();
    version_++;
    return 0;
}

// receipt（FIFO）：找第一张 Sending 卡填 ASR 文本，Sending → Waiting
int MsgLog::fillAsk(const char* text, const char* msgId) {
    if (msgs_ == nullptr) return -1;
    for (int i = 0; i < count_; i++) {
        if (msgs_[i].state == MsgState::Sending) {
            utf8CopyTrunc(msgs_[i].ask, sizeof(msgs_[i].ask), text);
            fontFilterDisplayable(msgs_[i].ask);
            if (msgId && msgId[0]) {
                utf8CopyTrunc(msgs_[i].msgId, sizeof(msgs_[i].msgId), msgId);
            }
            msgs_[i].state = MsgState::Waiting;
            version_++;
            return i;
        }
    }
    return -1;  // 无占位卡（乱序/重启后的迟到 receipt）：丢弃
}

// reply（FIFO）：找第一张 Waiting 卡填回复，Waiting → Replied
int MsgLog::fillReply(const char* text, const char* replyTo) {
    if (msgs_ == nullptr) return -1;
    // 精确配对优先：reply_to 命中某张 Waiting 卡的 msgId → 填那张
    //（2026-09-25 用户报"回复加错卡片"：FIFO 盲配对在回复乱序时错位）
    if (replyTo && replyTo[0]) {
        for (int i = 0; i < count_; i++) {
            if (msgs_[i].state == MsgState::Waiting &&
                strncmp(msgs_[i].msgId, replyTo, sizeof(msgs_[i].msgId)) == 0) {
                utf8CopyTrunc(msgs_[i].reply, sizeof(msgs_[i].reply), text);
                fontFilterDisplayable(msgs_[i].reply);
                msgs_[i].state = MsgState::Replied;
                msgs_[i].repliedAtMs = millis();
                version_++;
                return i;
            }
        }
    }
    // 退回 FIFO：填最早 Waiting 卡（Hermes 未带引用的合并回复场景）
    for (int i = 0; i < count_; i++) {
        if (msgs_[i].state == MsgState::Waiting) {
            utf8CopyTrunc(msgs_[i].reply, sizeof(msgs_[i].reply), text);
            fontFilterDisplayable(msgs_[i].reply);
            msgs_[i].state = MsgState::Replied;
            msgs_[i].repliedAtMs = millis();
            version_++;
            return i;
        }
    }
    return -1;  // 无等待中的卡：孤立回复丢弃（不做无上下文卡）
}

// error（FIFO）：找第一张 Sending 卡置 Failed，原因塞进 reply 槽给 UI 显示
int MsgLog::failSending(const char* reason) {
    if (msgs_ == nullptr) return -1;
    for (int i = 0; i < count_; i++) {
        if (msgs_[i].state == MsgState::Sending) {
            utf8CopyTrunc(msgs_[i].reply, sizeof(msgs_[i].reply), reason ? reason : "");
            fontFilterDisplayable(msgs_[i].reply);
            msgs_[i].state = MsgState::Failed;
            version_++;
            return i;
        }
    }
    return -1;
}

// 按稳定 id 找卡（详情页认卡用；头部插卡/顶卡下标漂移也不串卡）
const Msg* MsgLog::findById(uint32_t id) const {
    if (id == 0 || msgs_ == nullptr) return nullptr;
    for (int i = 0; i < count_; i++) {
        if (msgs_[i].id == id) return &msgs_[i];
    }
    return nullptr;
}

// 软超时：Waiting 且从松手起过了 60s——只改 UI 文案，state 不变
bool MsgLog::softTimedOut(const Msg& m, uint32_t nowMs) {
    return m.state == MsgState::Waiting &&
           (nowMs - m.createdAtMs) >= REPLY_SOFT_TIMEOUT_MS;
}

}  // namespace gaga
