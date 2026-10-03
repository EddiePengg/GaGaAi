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
int MsgLog::addSending(uint32_t seq) {
    ensureStorage();
    if (msgs_ == nullptr) return -1;
    if (count_ < MAX_MSGS) count_++;
    memmove(&msgs_[1], &msgs_[0], sizeof(Msg) * (count_ - 1));  // 最旧被顶掉
    Msg& m = msgs_[0];
    memset(&m, 0, sizeof(m));
    m.id          = nextId_++;
    m.state       = MsgState::Sending;
    m.createdAtMs = millis();
    m.recSeq      = seq;   // 本段 rec_seq（ADR-070；0=无段，notify/测试卡）
    version_++;
    return 0;
}

// 服务器主动通知建卡（notify 信令）：复用 addSending 的头部插卡 + id/时间戳，
// 但无问只有答——ask 槽借来放"GAGA 提醒"来源标签，reply 槽放通知全文，
// 直接 Replied（NotifySession 已自行亮屏/叮咚，卡只负责呈现）
int MsgLog::addNotify(const char* text) {
    const int idx = addSending();   // 占位卡：头部插入 + id + createdAtMs
    if (idx < 0) return -1;
    Msg& m = msgs_[0];
    utf8CopyTrunc(m.ask, sizeof(m.ask), "GAGA 提醒");
    fontFilterDisplayable(m.ask);
    utf8CopyTrunc(m.reply, sizeof(m.reply), text ? text : "");
    fontFilterDisplayable(m.reply);
    m.state = MsgState::Replied;
    m.repliedAtMs = millis();
    version_++;
    return 0;
}

// ── 服务端重发去重环（2026-09-29 卡片劫持案）────────────────────────────
bool MsgLog::ringHas_(const char (*ring)[40], int n, const char* key) {
    for (int i = 0; i < n; i++) {
        if (ring[i][0] != '\0' && strncmp(ring[i], key, 40) == 0) return true;
    }
    return false;
}

// receipt msg_id 去重：空 id（旧服务端）不判重；命中=true 且不入环
bool MsgLog::seenReceiptId_(const char* msgId) {
    if (msgId == nullptr || msgId[0] == '\0') return false;
    if (ringHas_(seenIds_, kSeenIds, msgId)) return true;
    utf8CopyTrunc(seenIds_[seenIdsHead_], sizeof(seenIds_[seenIdsHead_]), msgId);
    seenIdsHead_ = (seenIdsHead_ + 1) % kSeenIds;
    return false;
}

// reply_to 去重（reply_to 是 receipt msg_id，receipt 到达时已登记过 id 环——
// 所以这里必须独立环，否则会把自己人全误判成重复）
bool MsgLog::seenReplyTo_(const char* replyTo) {
    if (replyTo == nullptr || replyTo[0] == '\0') return false;
    if (ringHas_(seenReply_, kSeenReply, replyTo)) return true;
    utf8CopyTrunc(seenReply_[seenReplyHead_], sizeof(seenReply_[seenReplyHead_]), replyTo);
    seenReplyHead_ = (seenReplyHead_ + 1) % kSeenReply;
    return false;
}

// error 去重：error 帧无 msg_id，reason（code+msg）逐字相同即判重
bool MsgLog::seenError_(const char* reason) {
    if (reason == nullptr || reason[0] == '\0') return false;
    for (int i = 0; i < kSeenErrs; i++) {
        if (seenErrs_[i][0] != '\0' && strncmp(seenErrs_[i], reason, 96) == 0) return true;
    }
    utf8CopyTrunc(seenErrs_[seenErrsHead_], sizeof(seenErrs_[seenErrsHead_]), reason);
    seenErrsHead_ = (seenErrsHead_ + 1) % kSeenErrs;
    return false;
}

// receipt（FIFO）：找第一张 Sending 卡填 ASR 文本，Sending → Waiting。
// 返回 -2 = 服务端重发的重复 receipt——旧实现会把它 FIFO 盲配进新占位卡，
// 真回执反而丢弃（2026-09-29 实锤"录的话识别成功但屏上无卡"）；去重环命中
// 直接丢弃，一张卡也不碰。
// 配对窗口（同日补丁）：只配 3 分钟内的年轻卡——重启/重连后服务端会把
// 上一会话的积压回执整批推来（msg_id 全是新的，去重环拦不住首轮），盲配
// 照样劫持新卡。年轻卡窗口把首轮积压的劫持面压到"开机 1 分钟内录音"的窄缝。
// rec_seq 精确配对（ADR-070，2026-09-29）：下行 receipt 带 rec_seq 时只配
// 同号的 Sending 卡——旧服务端回落/无号时 seq==0 才走上面的去重环 + FIFO。
// 精确配对找不到 = 丢弃 + 日志，绝不碰其他卡（积压回执劫持新卡的根治）
int MsgLog::fillAsk(const char* text, const char* msgId, uint32_t seq) {
    if (msgs_ == nullptr) return -1;
    if (seq != 0) {
        for (int i = 0; i < count_; i++) {
            if (msgs_[i].state == MsgState::Sending && msgs_[i].recSeq == seq) {
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
        ESP_LOGW(TAG, "[msglog] receipt 无匹配 rec_seq=%lu 的 Sending 卡，丢弃",
                 static_cast<unsigned long>(seq));
        return -1;
    }
    if (seenReceiptId_(msgId)) {
        ESP_LOGW(TAG, "[msglog] 重复 receipt（服务端重发），丢弃");
        return -2;
    }
    const uint32_t nowMs = millis();
    for (int i = 0; i < count_; i++) {
        if (msgs_[i].state == MsgState::Sending &&
            nowMs - msgs_[i].createdAtMs < 180000) {   // 3 分钟配对窗口
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
    // 无占位卡可配对（乱序/重复/重启后的迟到 receipt）→ 丢弃。
    // 2026-09-26 排查"永远识别中"加日志：原先纯静默，丢没丢无从查起
    ESP_LOGW(TAG, "[msglog] receipt 无 Sending 卡可配对，丢弃");
    return -1;
}

// reply（FIFO）：找第一张 Waiting 卡填回复，Waiting → Replied
int MsgLog::fillReply(const char* text, const char* replyTo) {
    if (msgs_ == nullptr) return -1;
    if (seenReplyTo_(replyTo)) {
        ESP_LOGW(TAG, "[msglog] 重复 reply（服务端重发），丢弃");
        return -2;
    }
    // 精确配对优先：reply_to 命中某张 Waiting 卡的 msgId → 填那张
    //（2026-09-25 用户报"回复加错卡片"：FIFO 盲配对在回复乱序时错位）。
    // 配对窗口同上：积压 reply 首轮盲配劫持新卡，10 分钟窗（AI 回复慢是常态）
    const uint32_t nowMs = millis();
    if (replyTo && replyTo[0]) {
        for (int i = 0; i < count_; i++) {
            if (msgs_[i].state == MsgState::Waiting &&
                nowMs - msgs_[i].createdAtMs < 600000 &&
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
    // 退回 FIFO：填最早 Waiting 卡（Hermes 未带引用的合并回复场景），同窗口
    for (int i = 0; i < count_; i++) {
        if (msgs_[i].state == MsgState::Waiting &&
            nowMs - msgs_[i].createdAtMs < 600000) {
            utf8CopyTrunc(msgs_[i].reply, sizeof(msgs_[i].reply), text);
            fontFilterDisplayable(msgs_[i].reply);
            msgs_[i].state = MsgState::Replied;
            msgs_[i].repliedAtMs = millis();
            version_++;
            return i;
        }
    }
    // 无等待中的卡：孤立回复丢弃（不做无上下文卡）。补日志——reply 丢失与
    // receipt 丢失是"永远没回复"的同一族断点，静默丢弃在排查时无从下手
    ESP_LOGW(TAG, "[msglog] reply 无 Waiting 卡可配对（孤立/迟到），丢弃");
    return -1;
}

// error（FIFO）：找第一张 Sending 卡置 Failed，原因塞进 reply 槽给 UI 显示。
// 返回 -2 = 重复 error——服务端重发的同名错误，不得再失败一张新卡。
// rec_seq 精确配对（ADR-070，2026-09-29）：带 seq 的 error 跳过 seenError_
// 去重环（seq 本身就是幂等键，重发帧同号 → 同一张卡已 Failed，第二次精确
// 找不到自然丢弃），只配 recSeq==seq 的 Sending 卡；找不到 = 丢弃 + 日志，
// 不碰其他卡——"重复 error 不得再失败一张新卡"的语义由"精确找不到就丢弃"
// 天然保证。seq==0（旧服务端回落）走原逻辑（含去重环）
int MsgLog::failSending(const char* reason, const char* dedupKey, uint32_t seq) {
    if (msgs_ == nullptr) return -1;
    if (seq != 0) {
        for (int i = 0; i < count_; i++) {
            if (msgs_[i].state == MsgState::Sending && msgs_[i].recSeq == seq) {
                utf8CopyTrunc(msgs_[i].reply, sizeof(msgs_[i].reply), reason ? reason : "");
                fontFilterDisplayable(msgs_[i].reply);
                msgs_[i].state = MsgState::Failed;
                version_++;
                return i;
            }
        }
        ESP_LOGW(TAG, "[msglog] error 无匹配 rec_seq=%lu 的 Sending 卡，丢弃",
                 static_cast<unsigned long>(seq));
        return -1;
    }
    if (seenError_(dedupKey && dedupKey[0] ? dedupKey : reason)) {
        ESP_LOGW(TAG, "[msglog] 重复 error（服务端重发），丢弃");
        return -2;
    }
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

// 离线补发完成：最近一条 Failed 卡转回 Sending（黄条"识别中…"）。
// 补发已送达服务端，ASR 正在跑——随后的 receipt（ASR 文本+msg_id）经
// fillAsk 自然填回 ask（此前 ASR 结果因卡片非 Sending 态被丢弃 = 用户看到
// 的"空卡"）。之后 Hermes 回复经 fillReply 正常点亮。
int MsgLog::failToSending() {
    if (msgs_ == nullptr) return -1;
    for (int i = 0; i < count_; i++) {
        if (msgs_[i].state == MsgState::Failed) {
            msgs_[i].state = MsgState::Sending;
            version_++;
            return i;
        }
    }
    return -1;
}

bool MsgLog::hasStaleSending(uint32_t minAgeMs) const {
    return hasStaleSending(minAgeMs, 0xFFFFFFFFu);
}

bool MsgLog::hasStaleSending(uint32_t minAgeMs, uint32_t maxAgeMs) const {
    if (msgs_ == nullptr) return false;
    const uint32_t now = millis();
    for (int i = 0; i < count_; i++) {
        if (msgs_[i].state == MsgState::Sending) {
            const uint32_t age = now - msgs_[i].createdAtMs;
            if (age >= minAgeMs && age <= maxAgeMs) return true;
        }
    }
    return false;
}

bool MsgLog::hasStaleWaiting(uint32_t minAgeMs, uint32_t maxAgeMs) const {
    if (msgs_ == nullptr) return false;
    const uint32_t now = millis();
    for (int i = 0; i < count_; i++) {
        if (msgs_[i].state == MsgState::Waiting) {
            const uint32_t age = now - msgs_[i].createdAtMs;
            if (age >= minAgeMs && age <= maxAgeMs) return true;
        }
    }
    return false;
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

// 识别软超时：Sending 且超 60s——同上只改占位文案（"永远识别中"的诚实化）
bool MsgLog::sendingTimedOut(const Msg& m, uint32_t nowMs) {
    return m.state == MsgState::Sending &&
           (nowMs - m.createdAtMs) >= SENDING_SOFT_TIMEOUT_MS;
}

}  // namespace gaga
